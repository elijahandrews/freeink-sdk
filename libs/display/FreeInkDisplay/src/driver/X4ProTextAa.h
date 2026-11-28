#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif

namespace freeink {
// Text selector and charge model adapted from Free-Ink/freeink-sdk
// e6745c137f1dbe7aa0995184ce346030c900cea2 (fix-display-updates).
// Synchronous composition borrows its immutable inputs rather than retaining a
// fourth 48 KB plane. The 1 KB histogram also lives in PSRAM, never task stack.
class X4ProTextAa {
 public:
  static constexpr uint32_t SIZE = 48000;
  static constexpr size_t STORAGE_BYTES = SIZE * 11 + 256 * sizeof(uint32_t);
  X4ProTextAa() = default;
  ~X4ProTextAa() { release(); }
  X4ProTextAa(const X4ProTextAa&) = delete;
  X4ProTextAa& operator=(const X4ProTextAa&) = delete;
  bool ensure() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    if (!_storage && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= STORAGE_BYTES + 128 * 1024 &&
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= STORAGE_BYTES)
      _storage = static_cast<uint8_t*>(heap_caps_malloc(STORAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
    return _storage != nullptr;
  }
  void invalidate() {
    _history = _composed = false;
    _median = 0;
  }
  void release() {
    free(_storage);
    _storage = nullptr;
    invalidate();
  }
  const uint8_t* low() const { return _storage; }
  const uint8_t* high() const { return _storage + SIZE; }
  bool compose(const uint8_t* bw, const uint8_t* lsb, const uint8_t* msb, size_t bytes) {
    _composed = false;
    if (!bw || !lsb || !msb || bytes != SIZE || !ensure()) return false;
    auto* lo = _storage;
    auto* hi = _storage + SIZE;
    const auto* ink = _storage + SIZE * 2;
    const auto* charge = reinterpret_cast<const int8_t*>(_storage + SIZE * 3);
    for (uint32_t i = 0; i < SIZE; ++i) {
      const uint8_t gray = lsb[i] | msb[i];
      const uint8_t white = bw[i] & ~gray;
      uint8_t hold = _history ? static_cast<uint8_t>(white & ~ink[i]) : 0;
      if (_history) {
        for (unsigned bit = 0; bit < 8; ++bit)
          if (charge[i * 8 + bit] > _median + 11) hold &= static_cast<uint8_t>(~(0x80 >> bit));
      }
      lo[i] = gray | (white & ~hold);
      hi[i] = white;
    }
    return _composed = true;
  }
  // A one-gray waveform maps both coverage masks to the same shade. For a
  // darker text setting, represent the remapped dark gray (both masks set)
  // with alternating gray and solid ink pixels. The caller already promoted
  // fully covered strokes to black when it built the masks. Only 01 (gray)
  // becomes 00 (black); white clear/hold and unmasked strokes stay intact.
  void darkenEdges(const uint8_t* lsb, const uint8_t* msb, uint16_t widthBytes) {
    uint8_t pattern = 0xaa;
    for (uint32_t row = 0; row < SIZE; row += widthBytes) {
      for (uint32_t x = 0; x < widthBytes; ++x) {
        const uint32_t i = row + x;
        _storage[i] &= ~(lsb[i] & msb[i] & pattern);
      }
      pattern ^= 0xff;
    }
  }
  // Commit only after the display's BUSY completion. Selector codes (hi,lo):
  // 00 black, 01 gray, 10 white hold, 11 white clear. Negative decay rounds DOWN.
  void finish() {
    static constexpr int x4Doses[4] = {15, -2, -2, -20};
    finish(x4Doses, 0);
  }
  // The Picco firmware rounds charge decay to nearest (+512 before shifting).
  void finish(const int (&doses)[4], int roundingBias) {
    if (!_composed) return;
    auto* ink = _storage + SIZE * 2;
    auto* charge = reinterpret_cast<int8_t*>(_storage + SIZE * 3);
    auto* histogram = reinterpret_cast<uint32_t*>(_storage + SIZE * 11);
    memset(histogram, 0, 256 * sizeof(uint32_t));
    const auto nextCharge = [&](int previous, unsigned code) __attribute__((always_inline)) {
      int v = _history ? ((previous - _median) * 998 + roundingBias) >> 10 : 0;
      v += doses[code];
      return v < -127 ? -127 : v > 127 ? 127 : v;
    };
    for (uint32_t i = 0; i < SIZE; ++i) {
      const auto lo = low()[i], hi = high()[i];
      ink[i] = ~hi;
      // Blank paper and solid strokes commonly have eight equal transitions
      // and charges. Update those pixels together, including their histogram
      // weight. memcpy keeps this safe on unaligned PSRAM addresses.
      if ((lo == 0 || lo == 0xff) && (hi == 0 || hi == 0xff)) {
        constexpr uint64_t repeatByte = UINT64_C(0x0101010101010101);
        uint64_t previous = 0;
        if (_history) memcpy(&previous, charge + i * 8, sizeof(previous));
        const uint8_t first = static_cast<uint8_t>(previous);
        if (!_history || previous == repeatByte * first) {
          const unsigned code = (hi & 2) | (lo & 1);
          const int v = nextCharge(static_cast<int8_t>(first), code);
          const uint64_t repeated = repeatByte * static_cast<uint8_t>(v);
          memcpy(charge + i * 8, &repeated, sizeof(repeated));
          histogram[v + 128] += 8;
          continue;
        }
      }
      for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned shift = 7 - bit;
        const unsigned code = ((hi >> shift) & 1) * 2 + ((lo >> shift) & 1);
        const int v = nextCharge(_history ? charge[i * 8 + bit] : 0, code);
        charge[i * 8 + bit] = static_cast<int8_t>(v);
        ++histogram[v + 128];
      }
    }
    uint32_t count = 0;
    for (int i = 0; i < 256; ++i) {
      count += histogram[i];
      if (count >= SIZE * 4) {
        _median = i - 128;
        break;
      }
    }
    _composed = false;
    _history = true;
  }

 private:
  uint8_t* _storage = nullptr;
  int _median = 0;
  bool _history = false, _composed = false;
};
}  // namespace freeink
