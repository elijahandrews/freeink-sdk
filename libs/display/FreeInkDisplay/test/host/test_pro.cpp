#include <cassert>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>
#include <type_traits>
// Inspect private state without adding test-only methods to the SDK API.
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/lut/Ssd1677Luts.h"
#include "src/driver/Uc8179Driver.h"
#include "src/driver/Uc8279X4Driver.h"
#include "src/lut/X4ProTextAaLuts.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static const Bytes& lastPlane(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command) return i->bytes;
  assert(false);
  static const Bytes empty;
  return empty;
}

static Bytes frame(unsigned seed) {
  Bytes b(48000);
  for (size_t i=0; i<b.size(); ++i) b[i]=uint8_t((i*37 + i/100*11 + seed) ^ (i>>8));
  return b;
}

template<class Driver>
static void testStream(bool reverse, unsigned gateOffset) {
  Driver d;
  EpdBus bus;
  const auto a=frame(3), b=frame(89);
  for (bool invert : {false,true}) {
    bus.clear();
    d.streamPlane(bus,0x13,a.data(),invert);
    assert(bus.writes.size()==1);
    const auto& w=bus.writes.front();
    assert(w.transactions==1 && w.bytes.size()==60000);
    for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
      uint8_t expected=0xff;
      if (y>=gateOffset && y<gateOffset+480) {
        unsigned row=y-gateOffset;
        if (reverse) row=479-row;
        expected=a[row*100+x];
        if (invert) expected=uint8_t(~expected);
      }
      assert(w.bytes[y*100+x]==expected);
    }
  }
  bus.clear();
  d.streamPlaneXor(bus,0x13,a.data(),b.data());
  assert(bus.writes.size()==1 && bus.writes[0].transactions==1);
  assert(bus.writes[0].bytes.size()==60000);
  for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
    uint8_t expected=0xff;
    if (y>=gateOffset && y<gateOffset+480) {
      unsigned row=y-gateOffset;
      if (reverse) row=479-row;
      expected=a[row*100+x]^b[row*100+x];
    }
    assert(bus.writes[0].bytes[y*100+x]==expected);
  }
  d._grayRefreshedOnce=true;
  d._oldPlaneValid=true;
  d._needFullClear=false;
  bus.clear();
  d.displayGrayscaleBase(bus,a.data(),RefreshMode::Full,false);
  assert(lastRegister(bus,0xe5)==0x1e); // honor Full even after an AA page
  for (const auto& w : bus.writes)
    if (w.command==0x10 || w.command==0x13) assert(w.transactions==1);
}

static void testSsd() {
  const auto b=frame(33);
  for (auto mode : {RefreshMode::Full,RefreshMode::Half,RefreshMode::Fast}) {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    bus.clear();
    d.display(bus,b.data(),nullptr,mode,false);
    assert(lastRegister(bus,0x22)==(mode==RefreshMode::Full ? 0xf7 : 0xd7));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,false);
    assert(lastRegister(bus,0x22)==0xfc);
  }
  // The first-paint-after-boot/wake promotion must survive turnOff. A caller that powers
  // the panel down on every refresh (CrossPoint's sunlight fading fix) otherwise never
  // consumes the one-shot, and its differential FAST cannot clear the sleep screen.
  {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    // The activation value is not the LAST 0x22 write here: 0xfc does not self-power-off,
    // so the driver follows it with the separate 0x22=0x03 power-down sequence.
    const auto activated = [&bus](uint8_t seq) {
      for (const auto& w : bus.writes)
        if (w.command == 0x22 && !w.bytes.empty() && w.bytes[0] == seq) return true;
      return false;
    };
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xd7) && !activated(0xfc));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xfc) && !activated(0xd7));
  }
  EpdBus bus;
  Ssd1677Driver d;
  d.begin(bus);
  bus.clear();
  d.displayGray(bus,b.data(),false,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcc && d._isScreenOn);
  d.deepSleep(bus);
  assert(lastRegister(bus,0x22)==3 && !d._isScreenOn);
  bus.clear();
  d.displayGray(bus,b.data(),true,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcf && !d._isScreenOn);
}

template<class Driver>
static void testAsyncFrame() {
  Driver driver;
  FreeInkDisplay display(12,11,13,18,14,6);
  display._driver=&driver;
  display.begin();
  const auto submitted=frame(27), redrawn=frame(94);
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.completeDisplay();
  // Finish must sync the frame actually submitted, even after the caller draws.
  const auto& old=display._bus.writes.back();
  assert(old.command==0x10 && old.bytes.size()==60000);
  for (unsigned y=0; y<480; ++y) {
    unsigned dst;
    if constexpr (std::is_same<Driver,Uc8179Driver>::value) dst=479-y;
    else dst=120+y;
    assert(std::equal(submitted.begin()+y*100,submitted.begin()+(y+1)*100,old.bytes.begin()+dst*100));
  }
  assert(!display.isRefreshPending());
  // Shadow-free entry points require the live frame to survive until finish.
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH,false);
  display.completeDisplay();
  const auto& next=display._bus.writes.back();
  unsigned firstRow=std::is_same<Driver,Uc8179Driver>::value ? 479 : 0;
  unsigned offset=std::is_same<Driver,Uc8179Driver>::value ? 0 : 12000;
  assert(std::equal(redrawn.begin()+firstRow*100,redrawn.begin()+(firstRow+1)*100,next.bytes.begin()+offset));
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayAsyncImpl(FreeInkDisplay::FAST_REFRESH,false,true);
  display.completeDisplay();
  const auto& noShadow=display._bus.writes.back();
  assert(std::equal(submitted.begin()+firstRow*100,submitted.begin()+(firstRow+1)*100,
                    noShadow.bytes.begin()+offset));
#endif
  display.releaseBuffers();
  free(driver._grayBase);
}

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
// cleanupGrayscaleWithPreviousBuffer() only ever runs at the tail (or abort point) of a
// grayscale plane pass, and those passes use the write framebuffer as plane scratch. So
// frameBufferActive is the only thing that can be the displayed B/W frame: with the secondary
// away the driver must be told there is no baseline, never handed the plane. Handing it over
// rebases the controller's previous-frame RAM from the page's own glyph plane.
static void testGrayCleanupBaseline() {
  const auto page = frame(51), planeScratch = frame(77);

  // Secondary resident: the displayed frame is the baseline, and the write buffer is restored
  // from it so the next B/W draw starts from a real frame rather than plane bytes.
  {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    std::memcpy(display.getFrameBuffer(), page.data(), page.size());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);  // swaps: frameBufferActive == page
    std::memcpy(display.getFrameBuffer(), planeScratch.data(), planeScratch.size());
    display._bus.clear();
    display.cleanupGrayscaleWithPreviousBuffer();
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x10; }) == 1);
    assert(driver._oldPlaneValid);
    assert(std::equal(page.begin(), page.end(), display.getFrameBuffer()));
    display.releaseBuffers();
    free(driver._grayBase);
  }

  // Secondary released, then lent: in both cases no copy of the displayed frame exists, so
  // nothing may be streamed into controller RAM and the driver must fall back to a clean sync.
  for (bool lend : {false, true}) {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    std::memcpy(display.getFrameBuffer(), page.data(), page.size());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(driver._oldPlaneValid);
    if (lend) assert(display.borrowSecondaryBuffer(nullptr) != nullptr);
    else assert(display.releaseSecondaryBuffer());
    std::memcpy(display.getFrameBuffer(), planeScratch.data(), planeScratch.size());
    display._bus.clear();
    display.cleanupGrayscaleWithPreviousBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x10 && w.command != 0x13);
    assert(driver._needFullClear && !driver._oldPlaneValid);
    if (lend) assert(display.returnSecondaryBuffer());
    display.releaseBuffers();
    free(driver._grayBase);
  }
}
#endif

// A driver with no grayscale implementation must never advertise support.
class BwOnlyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 10000000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override { return {800, 480, 100, 48000}; }
  void begin(EpdBus&) override {}
  void deepSleep(EpdBus&) override {}
  void display(EpdBus&, const uint8_t*, const uint8_t*, RefreshMode, bool) override {}
};

class CombinedGrayDriver : public BwOnlyDriver {
 public:
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, true};
  }
};

static void testCapabilities() {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  assert(!display.grayscaleCapabilities().supported());
  BwOnlyDriver bw;
  display._driver = &bw;
  assert(!display.grayscaleCapabilities().supported());
  assert(!display.supportsStripGrayscale());

  Ssd1677Driver ssd;
  display._driver = &ssd;
  auto caps = display.grayscaleCapabilities();
  assert(caps.supported() && caps.encoding == GrayscaleEncoding::OverlayMasks);
  assert(caps.stripUploads && caps.asyncBase && !caps.stagingWhileBusy);
  assert(caps.base == GrayscaleBase::Separate);
  assert(display.supportsStripGrayscale() == caps.stripUploads);
  assert(display.supportsAsyncGrayscaleBase() == caps.asyncBase);
  assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!display.grayscaleCapabilities(static_cast<GrayscaleMode>(255)).supported());
  display._inversionDirty = true;
  assert(!display.grayscaleCapabilities().asyncBase);
  assert(display.grayscaleCapabilities().stripUploads);
  assert(!display.supportsAsyncGrayscaleBase());
  display._inversionDirty = false;

  CombinedGrayDriver combined;
  display._driver = &combined;
  assert(display.combinesGrayscaleBase());
  assert(display.supportsBusyGrayscaleStaging());
  assert(!display.supportsAsyncGrayscaleBase());
  display._inverted = true;
  caps = display.grayscaleCapabilities();
  assert(!caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
  assert(!display.combinesGrayscaleBase() && !display.supportsBusyGrayscaleStaging());
  assert(!display.supportsStripGrayscale());
  display._inverted = false;

  Uc8179Driver uc8179;
  Uc8279X4Driver uc8279;
  for (PanelDriver* driver : {static_cast<PanelDriver*>(&uc8179), static_cast<PanelDriver*>(&uc8279)}) {
    display._driver = driver;
    caps = display.grayscaleCapabilities();
    assert(caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
    assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    assert(!display.grayscaleCapabilities(GrayscaleMode::Absolute).stripUploads);
  }
  // Queries must neither start a refresh nor write the panel bus.
  assert(display._bus.writes.empty() && !display.isRefreshPending());
}

static void testAbsolutePipeline() {
  Ssd1677Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto absolute = GrayscaleMode::Absolute;
  assert(display.grayscaleCapabilities(absolute).base == GrayscaleBase::Combined);
  for (bool strips : {false, true}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(absolute));
    assert(display._bus.writes.empty());
    if (strips) {
      for (unsigned y = 0; y < 480; y += 80) {
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data() + y * 100, y, 80);
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data() + y * 100, y, 80);
      }
    } else display.copyGrayscaleBuffers(lsb.data(), msb.data());
    Bytes plane0, plane1;
    for (const auto& w : display._bus.writes) {
      if (w.command == 0x24) plane0.insert(plane0.end(), w.bytes.begin(), w.bytes.end());
      if (w.command == 0x26) plane1.insert(plane1.end(), w.bytes.begin(), w.bytes.end());
    }
    assert(plane0.size() == lsb.size() && plane1.size() == msb.size());
    for (size_t i = 0; i < lsb.size(); ++i) {
      assert(plane0[i] == uint8_t(~lsb[i]) && plane1[i] == uint8_t(~msb[i]));
    }
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.displayGrayBuffer(false);
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x20; }) == 1);
    assert(lastRegister(display._bus, 0x22) == 0xCC);
    assert(driver._isScreenOn && driver._needsGrayClear);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needsGrayClear);
    display._bus.clear();
    display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
    display.waitRefreshComplete();
    assert(!driver._needsGrayClear);
  }
  // Explicit shutdown must use its own activation after the gray refresh wait.
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display._bus.clear();
  display.displayGrayBuffer(true);
  std::vector<uint8_t> activations;
  for (const auto& w : display._bus.writes) {
    if (w.command == 0x22) activations.push_back(w.bytes.at(0));
  }
  assert((activations == std::vector<uint8_t>{0xCC, 0x03}));
  assert(!driver._isScreenOn && driver._needsGrayClear);
  for (unsigned failure = 0; failure < 6; ++failure) {
    assert(display.displayGrayscaleBase(absolute));
    display._bus.clear();
    if (failure == 0) display.copyGrayscaleLsbBuffers(lsb.data()); // missing MSB
    if (failure == 1) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 80, 80);
    if (failure == 2) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 481);
    if (failure == 3) display.copyGrayscaleLsbBuffers(nullptr);
    if (failure == 4) {
      display.copyGrayscaleBuffers(lsb.data(), msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data()); // duplicate plane
    }
    if (failure == 5) {
      display.copyGrayscaleMsbBuffers(msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data());
    }
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.cleanupGrayscaleBuffers(nullptr);
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, true);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
  }
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.setInverted(true);
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  assert(!display.displayGrayscaleBase(absolute));
  display.setInverted(false);
  display._bus.clear();
  assert(display.displayGrayscaleBase(absolute));
  assert(display._bus.writes.empty() && display._inversionDirty);
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer();
  assert(!display._inversionDirty);
  assert(display.displayGrayscaleBase(absolute));
  display.deepSleep();
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  display.releaseBuffers();

  Ssd1677Config unsupported = ssd1677DefaultConfig();
  unsupported.absoluteGrayscale = false;
  Ssd1677Driver other(unsupported);
  FreeInkDisplay noAbsolute(1, 2, 3, 4, 5, 6);
  noAbsolute._driver = &other;
  assert(!noAbsolute.displayGrayscaleBase(absolute));
  assert(noAbsolute._bus.writes.empty());
}

template<class Driver>
static void testUltraChipAbsolute(bool reverse, unsigned gateOffset, bool inverted) {
  for (bool snapshot : {false, true}) for (bool turnOff : {false, true}) {
    Driver driver;
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    display._driver = &driver;
    display.begin();
    if (!snapshot) { free(driver._grayBase); driver._grayBase = nullptr; }
    const auto bw = frame(91), lsb = frame(13), msb = frame(29);
    memcpy(display.getFrameBuffer(), bw.data(), bw.size());
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display._bus.clear();
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    for (unsigned p = 0; p < 2; ++p) {
      const auto& input = p ? msb : lsb;
      const auto command = p ? 0x13 : 0x10;
      const auto it = std::find_if(display._bus.writes.begin(), display._bus.writes.end(),
                                  [command](const auto& w) { return w.command == command; });
      assert(it != display._bus.writes.end() && it->bytes.size() == 60000);
      for (unsigned y = 0; y < 600; ++y) for (unsigned x = 0; x < 100; ++x) {
        uint8_t expected = 0xff;
        if (y >= gateOffset && y < gateOffset + 480) {
          unsigned row = y - gateOffset;
          if (reverse) row = 479 - row;
          expected = input[row * 100 + x];
          if (inverted) expected = uint8_t(~expected);
        }
        assert(it->bytes[y * 100 + x] == expected);
      }
    }
    if (snapshot) assert(memcmp(driver._grayBase, bw.data(), bw.size()) == 0);
    display.displayGrayBuffer(turnOff);
    assert(driver._isScreenOn == !turnOff);
    assert(driver._needFullClear && !driver._absoluteInput);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear);
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(!driver._needFullClear);
    // Incomplete uploads are canceled without a gray activation and still force a clean.
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display.copyGrayscaleLsbBuffers(lsb.data());
    display._bus.clear();
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x12);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear && !driver._absoluteInput);
    display.displayGrayscaleBase(FreeInkDisplay::HALF_REFRESH);
    assert(!driver._absoluteInput);
    display.releaseBuffers();
    free(driver._grayBase);
    driver._grayBase = nullptr;
  }
}

static void testUc8179GrayShadeSplit() {
  Uc8179Driver driver;
  freeink::EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(91), lsb = frame(13), msb = frame(29);
  for (auto mode : {GrayscaleMode::Absolute, GrayscaleMode::Overlay}) {
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, mode == GrayscaleMode::Absolute);
    const auto light = std::find_if(bus.writes.begin(), bus.writes.end(),
                                    [](const auto& w) { return w.command == 0x22; });
    const auto dark = std::find_if(bus.writes.begin(), bus.writes.end(),
                                   [](const auto& w) { return w.command == 0x23; });
    assert(light != bus.writes.end() && dark != bus.writes.end());
    assert(light->bytes.size() == 42 && dark->bytes.size() == 42);
    assert(light->bytes[0] == 0x20 && light->bytes[1] == 2 && light->bytes[2] == 2);
    auto expected = light->bytes;
    expected[2] = 1;
    expected[3] = 2;
    assert(dark->bytes == expected);
    std::vector<size_t> pllWrites;
    size_t refresh = bus.writes.size();
    for (size_t i = 0; i < bus.writes.size(); ++i) {
      if (bus.writes[i].command == 0x30 && bus.writes[i].bytes.size() == 1) pllWrites.push_back(i);
      if (bus.writes[i].command == 0x12) refresh = i;
    }
    if (mode == GrayscaleMode::Absolute) {
      assert(pllWrites.size() == 2);
      assert(bus.writes[pllWrites[0]].bytes[0] == 0x05 && bus.writes[pllWrites[1]].bytes[0] == 0x06);
      assert(pllWrites[0] < refresh && refresh < pllWrites[1]);
    } else {
      assert(pllWrites.empty());
    }
  }
  free(driver._grayBase);
}

template<class Driver>
static void testDirectSleep() {
  Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(11), lsb = frame(17), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto activations = [&]() {
    return std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x12; });
  };
  assert(display.grayscaleCapabilities(GrayscaleMode::Direct).base == GrayscaleBase::Combined);
  for (auto fallback : {FreeInkDisplay::HALF_REFRESH, FreeInkDisplay::FAST_REFRESH}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(GrayscaleMode::Direct, fallback));
    assert(activations() == 0);
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    assert(activations() == 0);
    display.displayGrayBuffer(false);
    assert(activations() == 1);
    display.cleanupGrayscaleBuffers(bw.data());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
    assert(activations() > 1);
  }
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Direct));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.displayGrayBuffer(false);
  assert(activations() == 0);
  display.deepSleep();
  free(driver._grayBase);
}

static void testUc8279X4WaveformSelection() {
  Uc8279X4Driver driver;
  EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(11);
  Bytes lsb(48000, 0), msb(48000, 0);
  const auto bank = [&]() {
    std::vector<Bytes> rows(5);
    for (const auto& w : bus.writes)
      if (w.command >= 0x20 && w.command <= 0x24 && w.bytes.size() == 49) rows[w.command - 0x20] = w.bytes;
    for (const auto& row : rows) assert(row.size() == 49);
    return rows;
  };
  const auto render = [&](GrayscaleMode mode, uint8_t mask, bool factory) {
    std::fill(msb.begin(), msb.end(), mask);
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, factory);
    return bank();
  };
  const auto quality = render(GrayscaleMode::Direct, 0, false);
  assert(quality[2] != quality[3]);
  assert(render(GrayscaleMode::Absolute, 0, false) == quality);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    for (uint8_t mask : {0x00, 0x01, 0x03}) {
      const auto text = render(GrayscaleMode::Overlay, mask, false);
      assert(text != quality);
      assert(text[2] == text[3]);
      const uint8_t timing = variant == 0x02 ? 2 : 3;
      for (unsigned t = 0; t < 5; ++t) {
        Bytes expected = {1, 2, timing, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1};
        expected.resize(49, 0);
        if (t == 1) expected[3] |= 0x40;
        if (t == 2 || t == 3) expected[2] |= 0x80;
        if (t == 4) expected[3] |= 0x80;
        assert(text[t] == expected);
      }
    }
    assert(render(GrayscaleMode::Overlay, 0x07, false) == quality);
    assert(render(GrayscaleMode::Overlay, 0, true) == quality);
    // An image pass must not leave the next sparse page on the quality bank.
    assert(render(GrayscaleMode::Overlay, 0x01, false) != quality);
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  free(driver._grayBase);
}

static void testStickyAbsolute() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  assert(driver._cfg.grayPowerUpFirst);
  assert(driver._cfg.fullSeqOverride == 0xF7);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
  assert(display._bus.writes.empty());
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer(false);
  assert(lastRegister(display._bus, 0x22) == 0xCC);
  assert(driver._isScreenOn);
  display.cleanupGrayscaleBuffers(bw.data());
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(lastRegister(display._bus, 0x22) == 0xF7);
  display.releaseBuffers();
}

static void testMetalio() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::MetalioEInk4;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  EpdBus bus;
  driver.begin(bus);
  assert(lastRegister(bus, 0x3C) == 0x01);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(driver.grayscaleCapabilities().supported());
  const auto bw = frame(42);
  bus.clear();
  // Cold FAST uses the demo's two-partial black pulse, never OTP D7. Both
  // phases run the vendor B/W fast LUT (0xCC activation, no OTP 0xFC).
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xCC);
  unsigned activations = 0;
  std::vector<Bytes> oldPlanes;
  for (const auto& w : bus.writes) {
    if (w.command == 0x20) ++activations;
    if (w.command == 0x26) oldPlanes.push_back(w.bytes);
    if (w.command == 0x22) assert(w.bytes == Bytes({0xCC}));
    if (w.command == 0x32) assert(w.bytes == Bytes(lut_fast_metalio, lut_fast_metalio + 105));
    assert(w.command != 0x1A);
  }
  assert(activations == 2 && bus.waits == 2);
  assert(oldPlanes.size() == 3);
  assert(oldPlanes[0] == Bytes(bw.size(), 0xFF));
  assert(oldPlanes[1] == Bytes(bw.size(), 0x00));
  assert(oldPlanes[2] == bw);
  assert(!driver._customLutActive);
  bus.clear();
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(lastRegister(bus, 0x18) == 0x80);
  assert(lastRegister(bus, 0x3C) == 0x80);
  assert(lastRegister(bus, 0x2C) == 0x10);
  for (const auto& w : bus.writes) if (w.command == 0x21) assert(w.bytes == Bytes({0, 0}));
  assert(driver._isScreenOn);
  bus.clear();
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  // X4 external-LUT AA path: LUT + voltage tail written, border parked at 0xC0,
  // absolute 0xCC activation.
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(lastRegister(bus, 0x3C) == 0xC0);
  bool lutWritten = false;
  for (const auto& w : bus.writes) if (w.command == 0x32) lutWritten = true;
  assert(lutWritten);
  // Exit grayscale the way the facade does so the next FAST isn't promoted.
  driver.cleanupGrayscaleBuffers(bus, bw.data());
  bus.clear();
  driver.displayStart(bus, bw.data(), bw.data(), RefreshMode::Fast, true);
  assert(lastRegister(bus, 0x22) == 0xCF);  // LUT paint, then analog/clock off
  driver.displayFinish(bus, bw.data());
  assert(lastRegister(bus, 0x22) == 0xCF);
  assert(!driver._isScreenOn);
  // Deferred FAST must resync both planes from the frame retained by the facade.
  assert(lastPlane(bus, 0x24) == bw && lastPlane(bus, 0x26) == bw);
  bus.clear();
  driver.displayStart(bus, bw.data(), nullptr, RefreshMode::Half, true);
  assert(bus.waits == 1);  // black pulse done; target waveform still pending
  unsigned pulses = 0;
  for (const auto& w : bus.writes) if (w.command == 0x20) ++pulses;
  assert(pulses == 2);
  assert(lastPlane(bus, 0x26) == Bytes(bw.size(), 0));
  driver.displayFinish(bus, bw.data());
  assert(lastPlane(bus, 0x26) == bw && lastPlane(bus, 0x24) == bw);
  assert(lastRegister(bus, 0x22) == 0xCF);
  driver.deepSleep(bus);
  assert(lastRegister(bus, 0x10) == 0x03);
}

// The split must emit the established blocking sequence byte-for-byte, with
// identical BUSY wait boundaries. The reference build also runs this fixture
// against the pinned pre-change SDK, so two refactored paths cannot agree on
// a shared regression unnoticed.
static void testDeferredGraySequence() {
  uint64_t fingerprint = 1469598103934665603ULL;
  const auto hash = [&](uint8_t b) { fingerprint = (fingerprint ^ b) * 1099511628211ULL; };
  const auto page = frame(23), next = frame(71);
  Bytes lsb(48000, 0), msb(48000, 0);
  for (size_t i = 0; i < lsb.size(); i += 13) lsb[i] = msb[i] = 1;
  unsigned cases = 0;
  for (uint8_t variant : {0x02, 0x68, 0x69})
    for (int state = 0; state < 5; ++state)
      for (auto mode : {RefreshMode::Fast, RefreshMode::Half, RefreshMode::Full})
        for (bool off : {false, true})
          for (bool cancel : {false, true}) {
            BoardConfig::ACTIVE.displayControllerVariant = variant;
            Uc8279X4Driver blocking, split;
            EpdBus a, b;
            const auto setup = [&](auto& driver, auto& bus) {
              driver.begin(bus);
              if (state) {
                driver._grayRefreshedOnce = true;
                driver._oldPlaneValid = true;
                driver._needFullClear = state >= 3;
                driver._redriveAfterGray = state == 1;
                driver._directGrayOnPanel = state == 4;
              }
              bus.clear();
            };
            setup(blocking, a);
            setup(split, b);
            blocking.beginGrayscale(a, page.data(), GrayscaleMode::Overlay, mode, off);
#ifndef FREEINK_TEST_REFERENCE
            assert(split.displayGrayscaleBaseStart(b, page.data(), mode, off));
            assert(split._pendingRefresh);
            const bool prebw = state == 1 || state == 2;
            if (mode == RefreshMode::Fast && prebw) {
              assert(b.writes.back().command == 0x12);
              // Target DTM1 is deliberately deferred until BUSY has completed.
              for (const auto& w : b.writes) assert(w.command != 0x10);
              assert(split._pendingGrayBase);
            }
            split.displayFinish(b, page.data());
            assert(!split._pendingRefresh && !split._pendingGrayBase);
#endif
            const auto finish = [&](auto& driver, auto& bus) {
              if (!cancel) {
                driver.copyGrayscaleLsb(bus, lsb.data());
                driver.copyGrayscaleMsb(bus, msb.data());
                driver.displayGray(bus, page.data(), false, nullptr, false);
              }
              driver.cleanupGrayscaleBuffers(bus, page.data());
              driver.display(bus, next.data(), nullptr, RefreshMode::Fast, true);
            };
            finish(blocking, a);
#ifndef FREEINK_TEST_REFERENCE
            finish(split, b);
            assert(a.waitPositions == b.waitPositions && a.writes.size() == b.writes.size());
            for (size_t j = 0; j < a.writes.size(); ++j) {
              assert(a.writes[j].command == b.writes[j].command);
              assert(a.writes[j].bytes == b.writes[j].bytes);
              assert(a.writes[j].transactions == b.writes[j].transactions);
            }
            assert(blocking._oldPlaneValid == split._oldPlaneValid);
            assert(blocking._needFullClear == split._needFullClear);
            assert(blocking._grayBaseValid == split._grayBaseValid);
            assert(blocking._redriveAfterGray == split._redriveAfterGray);
            assert(blocking._isScreenOn == split._isScreenOn);
#endif
            for (const auto& w : a.writes) {
              hash(w.command);
              for (unsigned n = 0; n < 4; ++n) hash(uint8_t(w.bytes.size() >> (8 * n)));
              for (uint8_t v : w.bytes) hash(v);
            }
            for (size_t pos : a.waitPositions)
              for (unsigned n = 0; n < 4; ++n) hash(uint8_t(pos >> (8 * n)));
            free(blocking._grayBase);
            free(split._grayBase);
            ++cases;
          }
  std::printf("AA base command fingerprint: %016llx cases=%u\n", (unsigned long long)fingerprint, cases);
}

static void testDeferredGrayFacade() {
#ifndef FREEINK_TEST_REFERENCE
  const auto page = frame(19), poison = frame(81);
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  for (int finishBy = 0; finishBy < 6; ++finishBy) {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    driver._grayRefreshedOnce = driver._oldPlaneValid = driver._redriveAfterGray = true;
    driver._needFullClear = false;
    memcpy(display.getFrameBuffer(), page.data(), page.size());
    uint8_t* const submitted = display.getFrameBuffer();
    display._bus.clear();
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
    assert(!display.supportsDeferredGrayscaleBase());
    assert(!display.displayGrayscaleBaseAsync());
    assert(display.getFrameBuffer() == submitted && !display.isRefreshPending());
#else
    assert(display.supportsDeferredGrayscaleBase());
    assert(display.displayGrayscaleBaseAsync());
    assert(display.frameBufferActive == submitted && display.getFrameBuffer() != submitted);
    memcpy(display.getFrameBuffer(), poison.data(), poison.size());
    const size_t startWrites = display._bus.writes.size();
    const size_t startWaits = display._bus.waitPositions.size();
    if (finishBy == 0) display.finishDisplayAsync();
    if (finishBy == 1) {
      assert(display.borrowSecondaryBuffer(nullptr));
      assert(display.returnSecondaryBuffer());
    }
    if (finishBy == 2) {
      assert(display.releaseSecondaryBuffer());
      assert(display.reallocSecondaryBuffer());
    }
    if (finishBy == 3) display.setInverted(true);
    if (finishBy == 4) display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    if (finishBy == 5) {
      display.cleanupGrayscaleWithPreviousBuffer();
      // Cleanup is a RAM writer too: finish the submitted base before either
      // its deferred reseed or the cleanup reseed, preserving the single swap.
      assert(display._bus.waitPositions.size() > startWaits && display._bus.waitPositions[startWaits] == startWrites);
      assert(display.frameBufferActive == submitted && display.getFrameBuffer() != submitted);
      assert(std::equal(page.begin(), page.end(), display.getFrameBuffer()));
    }
    assert(!display.isRefreshPending());
    // The first DTM1 reseed after the split is the displayed page, never poison.
    const EpdBus::Write* reseed = nullptr;
    for (size_t i = startWrites; i < display._bus.writes.size(); ++i)
      if (display._bus.writes[i].command == 0x10) {
        reseed = &display._bus.writes[i];
        break;
      }
    assert(reseed && reseed->bytes.size() == 60000);
    assert(std::equal(page.begin(), page.end(), reseed->bytes.begin() + 12000));
    if (finishBy == 5) {
      const auto* const writable = display.getFrameBuffer();
      display._bus.clear();
      display.cleanupGrayscaleWithPreviousBuffer();
      // Already-completed cleanup adds no wait or swap, just its normal reseed.
      assert(display._bus.waits == 0 && display._bus.writes.size() == 1);
      assert(display._bus.writes.front().command == 0x10);
      assert(display.frameBufferActive == submitted && display.getFrameBuffer() == writable);
    }
#endif
    display.releaseBuffers();
    free(driver._grayBase);
  }
  // Capability and blocking fallbacks must not start an unsafe deferred pass.
  for (int reason = 0; reason < 4; ++reason) {
    Uc8279X4Driver driver;
    FreeInkDisplay display(12, 11, 13, 18, 14, 6);
    display._driver = &driver;
    display.begin();
    memcpy(display.getFrameBuffer(), page.data(), page.size());
    BoardConfig::ACTIVE.displayControllerVariant = reason == 0 ? 0x67 : 0x68;
    if (reason == 1) display._inverted = true;
    if (reason == 2) display._inversionDirty = true;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
    if (reason == 3) assert(display.releaseSecondaryBuffer());
#endif
    assert(!display.supportsDeferredGrayscaleBase());
    assert(!display.displayGrayscaleBaseAsync());
    assert(!display.isRefreshPending());
    display.releaseBuffers();
    free(driver._grayBase);
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  std::puts("AA base facade: framebuffer lifetime, drain boundaries and blocking fallbacks passed");
#endif
}

// Per-pixel oracle intentionally uses sorting and independent input decoding.
// Covers selector history over repeated mixed pages, resets, signed rounding,
// median recentering, saturation and no-commit failure. Never drives a panel.
static void testTextAaMath() {
  X4ProTextAa aa;
  Bytes bw(48000), lsb(48000), msb(48000), prevInk(384000), expectedLo(48000), expectedHi(48000);
  std::vector<int> charge(384000), sorted(384000);
  int median=0;
  bool history=false;
  unsigned rng=0x18aa5517;
  const auto random = [&]() { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; };
  for (unsigned turn=0; turn<80; ++turn) {
    if (turn==31) { aa.invalidate(); history=false; median=0; }
    for (unsigned i=0; i<48000; ++i) {
      // Large stable white areas plus dense moving glyph-like regions exercise
      // white hold, charge correction and the lower median in skewed pages.
      bw[i]=(i%13<9) ? 0xff : uint8_t(random());
      lsb[i]=(i%13<9) ? 0 : uint8_t(random());
      msb[i]=(i%13<9) ? 0 : uint8_t(random());
    }
    if (turn%7==0) { std::fill(bw.begin(),bw.end(),255); std::fill(lsb.begin(),lsb.end(),0); std::fill(msb.begin(),msb.end(),0); }
    std::fill(expectedLo.begin(),expectedLo.end(),0);
    std::fill(expectedHi.begin(),expectedHi.end(),0);
    for (unsigned pixel=0; pixel<384000; ++pixel) {
      unsigned b=pixel/8, mask=128u>>(pixel%8);
      bool gray=(lsb[b]&mask)||(msb[b]&mask);
      bool white=(bw[b]&mask)&&!gray;
      bool hold=history && white && !prevInk[pixel] && charge[pixel]<=median+11;
      unsigned code=!white ? (gray?1:0) : (hold?2:3);
      if (code&1) expectedLo[b]|=mask;
      if (code&2) expectedHi[b]|=mask;
      int decayed=history ? int(std::floor(double(charge[pixel]-median)*998.0/1024.0)) : 0;
      int dose=code==0?15:code==3?-20:-2;
      charge[pixel]=std::max(-127,std::min(127,decayed+dose));
      prevInk[pixel]=!white;
    }
    assert(aa.compose(bw.data(),lsb.data(),msb.data(),48000));
    assert(std::equal(expectedLo.begin(),expectedLo.end(),aa.low()));
    assert(std::equal(expectedHi.begin(),expectedHi.end(),aa.high()));
    aa.finish();
    sorted=charge;
    std::nth_element(sorted.begin(),sorted.begin()+191999,sorted.end());
    median=sorted[191999]; history=true;
    assert(aa._median==median);
    const auto* actual=reinterpret_cast<const int8_t*>(aa._storage+48000*3);
    for (unsigned i=0; i<384000; ++i) assert(actual[i]==charge[i]);
    // A malformed target must neither commit selectors nor destroy history.
    assert(!aa.compose(bw.data(),lsb.data(),msb.data(),47999)); aa.finish();
    assert(aa._median==median && aa._history);
  }
  aa.release();
  fakePsramFree=0;
  assert(!aa.compose(bw.data(),lsb.data(),msb.data(),48000));
  fakePsramFree=8*1024*1024; fakePsramLargest=1;
  assert(!aa.compose(bw.data(),lsb.data(),msb.data(),48000));
  fakePsramLargest=8*1024*1024; fakePsramOom=true;
  assert(!aa.compose(bw.data(),lsb.data(),msb.data(),48000));
  fakePsramOom=false;
  assert(aa.compose(bw.data(),lsb.data(),msb.data(),48000));
  assert(!aa.compose(nullptr,lsb.data(),msb.data(),48000)); aa.finish();
  assert(!aa._history);
  std::puts("Text AA: 80-frame per-pixel oracle, charge history and OOM checks passed");
}

static unsigned commandCount(const EpdBus& bus, uint8_t cmd) {
  return std::count_if(bus.writes.begin(),bus.writes.end(),[&](const auto& w){return w.command==cmd;});
}

static void testTextAaDriver() {
  BoardConfig::ACTIVE.board=BoardConfig::Board::XteinkX4Pro;
  BoardConfig::ACTIVE.displayController=BoardConfig::DisplayController::UC8279;
  BoardConfig::ACTIVE.displayControllerVariant=0x68;
  Bytes bw(48000,0x8f), lsb(48000,0x40), msb(48000,0x20);
  for (int exit=0; exit<10; ++exit) {
    Uc8279X4Driver d; EpdBus bus; d.begin(bus);
    // Cold/uncertain glass requires an ordinary clean target before text mode.
    bus.clear(); assert(!d.displayTextAa(bus,bw.data(),lsb.data(),msb.data(),48000,false));
    assert(bus.writes.empty());
    d.display(bus,bw.data(),nullptr,RefreshMode::Full,false); bus.clear();
    const unsigned resets=bus.resets;
    assert(d.displayTextAa(bus,bw.data(),lsb.data(),msb.data(),48000,false));
    assert(commandCount(bus,0x12)==1 && commandCount(bus,0x10)==1 && commandCount(bus,0x13)==1);
    assert(commandCount(bus,0x01)==0 && commandCount(bus,0x82)==0); // Never change voltage registers.
    assert(bus.resets==resets);
    assert(lastPlane(bus,0x10).size()==60000 && lastPlane(bus,0x13).size()==60000);
    for (unsigned i=0; i<60000; ++i) {
      assert(lastPlane(bus,0x10)[i]==(i<12000?0xff:0xef));
      assert(lastPlane(bus,0x13)[i]==(i<12000?0xff:0x8f));
    }
    for (unsigned i=0; i<5; ++i) {
      const auto& reg=lastPlane(bus,0x20+i);
      assert(reg.size()==49 && std::equal(reg.begin(),reg.end(),kX4ProTextUc8279Zhx+i*49));
    }
    assert(d._textAa._history && d._textAaRegistersDirty);
    bus.clear();
    assert(d.displayTextAa(bus,bw.data(),lsb.data(),msb.data(),48000,true));
    assert(commandCount(bus,0x12)==1 && commandCount(bus,0x02)==1 && !d._isScreenOn);
    assert(bus.resets==resets); // Consecutive text must stay one-pass.
    if (exit==0) d.display(bus,bw.data(),nullptr,RefreshMode::Fast,false);
    if (exit==1) { d.displayStart(bus,bw.data(),nullptr,RefreshMode::Fast,false); d.displayFinish(bus,bw.data()); }
    if (exit==2) d.displayGrayscaleBase(bus,bw.data(),RefreshMode::Fast,false);
    if (exit==3) { d.displayGrayscaleBaseStart(bus,bw.data(),RefreshMode::Fast,false); d.displayFinish(bus,bw.data()); }
    if (exit==4) d.beginGrayscale(bus,bw.data(),GrayscaleMode::Direct,RefreshMode::Fast,false);
    if (exit==5) d.beginGrayscale(bus,bw.data(),GrayscaleMode::Absolute,RefreshMode::Full,false);
    if (exit==6) d.cleanupGrayscaleBuffers(bus,bw.data());
    if (exit==7) d.deepSleep(bus);
    if (exit==8) { d.requestResync(1); assert(d._textAaRegistersDirty); d.display(bus,bw.data(),nullptr,RefreshMode::Fast,false); }
    if (exit==9) { d.releaseTextAa(); assert(d._textAaRegistersDirty); d.display(bus,bw.data(),nullptr,RefreshMode::Fast,false); }
    assert(bus.resets==resets+1 && !d._textAaRegistersDirty && !d._textAa._history);
    free(d._grayBase);
  }
  Uc8279X4Driver d; EpdBus bus; d.begin(bus); d._needFullClear=false;
  for (unsigned variant : {0x02,0x03,0x67,0x69,0xff}) {
    BoardConfig::ACTIVE.displayControllerVariant=variant;
    bus.clear(); assert(!d.supportsTextAa());
    assert(!d.displayTextAa(bus,bw.data(),lsb.data(),msb.data(),48000,false)); assert(bus.writes.empty());
  }
  BoardConfig::ACTIVE.displayControllerVariant=0x68;
  bus.failNextRefresh=true;
  assert(!d.displayTextAa(bus,bw.data(),lsb.data(),msb.data(),48000,false));
  assert(!d._textAa._history && d._needFullClear && !d._textAaRegistersDirty);
  free(d._grayBase);
  std::puts("Text AA driver: single activation, selectors/padding, exact LUT gate, transitions and simulated failed-completion recovery passed");
}

static void testTextAaFacade() {
  BoardConfig::ACTIVE.displayController=BoardConfig::DisplayController::UC8279;
  BoardConfig::ACTIVE.displayControllerVariant=0x68;
  const auto target=frame(43);
  Bytes lsb(48000,0x44), msb(48000,0x22);
  for (int reason=0; reason<7; ++reason) {
    Uc8279X4Driver driver;
    FreeInkDisplay d(12,11,13,18,14,6); d._driver=&driver; d.begin();
    driver._needFullClear=false;
    memcpy(d.getFrameBuffer(),target.data(),48000);
    auto* submitted=d.getFrameBuffer();
    if (reason==1) d._inverted=true;
    if (reason==2) d._inversionDirty=true;
    if (reason==3) driver._needFullClear=true;
    if (reason==4) fakePsramOom=true;
    if (reason==5) driver._w=792;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
    if (reason==6) assert(d.releaseSecondaryBuffer());
#endif
    d._bus.clear();
    bool ok=d.tryDisplayTextAa(lsb.data(),msb.data(),48000,false);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
    assert(!ok);
#else
    assert(ok==(reason==0));
#endif
    if (ok) {
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
      assert(d.frameBufferActive==submitted && d.getFrameBuffer()!=submitted);
      assert(std::equal(target.begin(),target.end(),d.frameBufferActive));
#endif
    } else {
      assert(d.getFrameBuffer()==submitted && std::equal(target.begin(),target.end(),submitted));
      assert(d._bus.writes.empty());
    }
    fakePsramOom=false; d.releaseBuffers(); free(driver._grayBase);
  }
  std::puts("Text AA facade: success swaps once; rejected attempts retain the new target; unsupported/single-buffer paths unchanged");
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "metalio") == 0) {
    testMetalio();
    std::puts("Metalio refresh, temperature, grayscale fallback and async power-down passed");
    return 0;
  }
  if (argc > 1) {
    testStickyAbsolute();
    std::puts("Sticky absolute capability, activation, power-down and B/W recovery passed");
    return 0;
  }
  testTextAaMath();
  testTextAaDriver();
  testTextAaFacade();
  BoardConfig::ACTIVE.displayController=BoardConfig::DisplayController::SSD1677;
  testDeferredGraySequence();
  testDeferredGrayFacade();
  testUc8179GrayShadeSplit();
  testUc8279X4WaveformSelection();
  testDirectSleep<Uc8179Driver>();
  testDirectSleep<Uc8279X4Driver>();
  testUltraChipAbsolute<Uc8179Driver>(true, 0, false);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    testUltraChipAbsolute<Uc8279X4Driver>(false, 120, true);
  }
  testAbsolutePipeline();
  testCapabilities();
  testStream<Uc8179Driver>(true,0);
  testStream<Uc8279X4Driver>(false,120);
  testSsd();
  testAsyncFrame<Uc8179Driver>();
  testAsyncFrame<Uc8279X4Driver>();
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  testGrayCleanupBaseline();
#endif
  std::puts("Pro plane bytes, transaction counts, clean refreshes, power state and async frame ownership passed");
}
