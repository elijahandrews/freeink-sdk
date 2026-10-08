// Included by test_pro.cpp: actual facade + driver against the recording bus.
// Expected masks and LUT bytes below are literal independent four-color oracles.
static size_t darkActivations(const EpdBus& bus) {
  return std::count_if(bus.writes.begin(), bus.writes.end(), [](const auto& w) { return w.command == 0x12; });
}

static const Bytes& darkLastPlane(const EpdBus& bus, uint8_t command) {
  for (auto it = bus.writes.rbegin(); it != bus.writes.rend(); ++it)
    if (it->command == command && it->bytes.size() == 60000) return it->bytes;
  assert(false);
  static Bytes empty;
  return empty;
}

static void darkExpectPlane(const Bytes& wire, const Bytes& visible, bool complement = false) {
  assert(wire.size() == 60000 && visible.size() == 48000);
  assert(std::all_of(wire.begin(), wire.begin() + 12000, [](uint8_t b) { return b == 0xff; }));
  for (size_t i = 0; i < visible.size(); ++i)
    assert(wire[i + 12000] == (complement ? uint8_t(~visible[i]) : visible[i]));
}

static void darkExpectNativeBank(const EpdBus& bus) {
  constexpr uint8_t expected[5][14] = {
      {1, 2, 3, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1},    {1, 2, 3, 0x41, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1},
      {1, 2, 0x43, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1}, {1, 2, 0x43, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1},
      {1, 2, 3, 0x81, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1},
  };
  unsigned seen = 0;
  for (const auto& w : bus.writes) {
    if (w.command < 0x20 || w.command > 0x24) continue;
    assert(w.bytes.size() == 49);
    const unsigned row = w.command - 0x20;
    assert(std::equal(expected[row], expected[row] + 14, w.bytes.begin()));
    assert(std::all_of(w.bytes.begin() + 14, w.bytes.end(), [](uint8_t b) { return b == 0; }));
    seen |= 1u << row;
  }
  assert(seen == 31);
  assert(lastRegister(bus, 0x50) == 0x97);
  // No new rail voltages, temperature, PLL, prebw bank, or Direct-quality setup.
  for (const auto& w : bus.writes)
    assert(w.command != 0x01 && w.command != 0x06 && w.command != 0x30 && w.command != 0xe5 && w.command != 0x82);
}

static void testUc8279DarkOverlay() {
  const auto savedProfile = BoardConfig::ACTIVE;
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4Pro;
  BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  Uc8279X4Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  assert(!driver.supportsInvertedGrayscale());  // unavailable without baseline allocation
  display.begin();
  display.setInverted(true);
  const auto caps = display.grayscaleCapabilities();
  assert(caps.supported() && caps.encoding == GrayscaleEncoding::OverlayMasks);
  assert(caps.base == GrayscaleBase::Separate && !caps.asyncBase && !caps.stripUploads && !caps.stagingWhileBusy);
  assert(!display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!display.grayscaleCapabilities(GrayscaleMode::Direct).supported());
  assert(!display.supportsAsyncRefresh());

  // Repeating logical white/black/dark/light samples. Native inverted AA stays
  // selected even at 50% gray coverage; image heuristics must not pick quality.
  const Bytes bw(48000, 0x88), lsb(48000, 0x22), msb(48000, 0x33);
  const Bytes wireLsb(48000, 0xaa), wireMsb(48000, 0x99);
  const auto base = [&](unsigned entry = 0) {
    std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
    display._bus.clear();
    if (entry == 1)
      assert(display.displayGrayscaleBase(GrayscaleMode::Overlay, FreeInkDisplay::FAST_REFRESH));
    else if (entry == 2)
      display.displayGrayscaleBase(FreeInkDisplay::FAST_REFRESH);
    else
      display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(darkActivations(display._bus) == 1);
    darkExpectPlane(darkLastPlane(display._bus, 0x10), bw, true);
    darkExpectPlane(darkLastPlane(display._bus, 0x13), bw, true);
    assert(!display.isRefreshPending());
    display._bus.clear();
  };
  const auto restored = [&]() {
    darkExpectPlane(darkLastPlane(display._bus, 0x10), bw, true);
    darkExpectPlane(darkLastPlane(display._bus, 0x13), bw, true);
    assert(std::equal(bw.begin(), bw.end(), display.getFrameBuffer()));
  };
  // No stale light base may be used immediately after changing polarity.
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display._bus.clear();
  display.displayGrayBuffer();
  assert(darkActivations(display._bus) == 0);
  for (unsigned entry = 0; entry < 3; ++entry) {
    base(entry);
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    darkExpectPlane(darkLastPlane(display._bus, 0x10), wireLsb);
    darkExpectPlane(darkLastPlane(display._bus, 0x13), wireMsb);
    assert(darkActivations(display._bus) == 0);
    display._bus.clear();
    display.displayGrayBuffer(entry == 2);
    assert(darkActivations(display._bus) == 1);
    darkExpectNativeBank(display._bus);
    darkExpectPlane(darkLastPlane(display._bus, 0x10), bw, true);
    darkExpectPlane(darkLastPlane(display._bus, 0x13), bw, true);
    assert(driver._isScreenOn == (entry != 2));
    assert(driver._redriveAfterGray);
    display.cleanupGrayscaleBuffers(bw.data());
    restored();
    // Subsequent dark B/W turns use ordinary OTP re-drive, not the stock
    // light-mode prebw bank. OLD is opposite physical target at activation.
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(darkActivations(display._bus) == 1);
    assert(lastRegister(display._bus, 0xe5) == 0x5a);
    for (const auto& w : display._bus.writes) assert(w.command < 0x20 || w.command > 0x24);
    const auto old = std::find_if(display._bus.writes.begin(), display._bus.writes.end(),
                                  [](const auto& w) { return w.command == 0x10; });
    assert(old != display._bus.writes.end());
    darkExpectPlane(old->bytes, bw);  // complement of physical ~bw
  }

  // Wrong order, duplicate planes, null input, incomplete pass, custom LUT and
  // factory/quality requests never activate. Recovery retains a logical host
  // baseline, restores BOTH physical planes, then forces a clean next paint.
  for (unsigned bad = 0; bad < 9; ++bad) {
    base();
    if (bad != 0) display.copyGrayscaleLsbBuffers(lsb.data());
    if (bad == 0) display.copyGrayscaleMsbBuffers(msb.data());
    if (bad == 2) display.copyGrayscaleLsbBuffers(lsb.data());
    if (bad == 3) display.copyGrayscaleMsbBuffers(nullptr);
    if (bad >= 4) display.copyGrayscaleMsbBuffers(msb.data());
    if (bad == 4) display.copyGrayscaleMsbBuffers(msb.data());
    if (bad == 5) display.copyGrayscaleLsbBuffers(nullptr);
    if (bad == 8) display.cancelGrayscalePass();
    display._bus.clear();
    const unsigned char custom = 0;
    display.displayGrayBuffer(false, bad == 6 ? &custom : nullptr, bad == 7);
    assert(darkActivations(display._bus) == 0);
    assert(driver._needFullClear);
    std::memset(display.getFrameBuffer(), 0x33, bw.size());
    display.cleanupGrayscaleBuffers(bw.data());
    restored();
    assert(driver._needFullClear);
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(lastRegister(display._bus, 0xe5) == 0x1e);
  }

  // Missing cleanup baseline must not seed a plane from grayscale scratch.
  base();
  display.copyGrayscaleLsbBuffers(lsb.data());
  display._bus.clear();
  display.cleanupGrayscaleBuffers(nullptr);
  assert(display._bus.writes.empty() && driver._needFullClear && !driver._oldPlaneValid);

  // Polarity switch after either partial upload invalidates that transaction.
  // No canceled dark overlay may reach a later light page; light AA stays exact.
  for (unsigned uploads = 1; uploads <= 2; ++uploads) {
    base();
    display.copyGrayscaleLsbBuffers(lsb.data());
    if (uploads == 2) display.copyGrayscaleMsbBuffers(msb.data());
    display.setInverted(false);
    assert(driver._needFullClear && !driver._grayBaseValid && !driver._absoluteGrayPlanes);
    std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
    display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
    // Sparse masks avoid the existing light-mode image quality heuristic.
    Bytes sparseLsb(48000, 0), sparseMsb(48000, 0);
    sparseLsb[0] = 0x22;
    sparseMsb[0] = 0x33;
    display.copyGrayscaleBuffers(sparseLsb.data(), sparseMsb.data());
    assert(darkLastPlane(display._bus, 0x10)[12000] == 0x55);
    assert(darkLastPlane(display._bus, 0x13)[12000] == 0x66);
    display._bus.clear();
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes)
      if (w.command == 0x22 || w.command == 0x23) assert(w.bytes[2] == 0x83);
    display.cleanupGrayscaleBuffers(bw.data());
    display.setInverted(true);
  }

  // Direct driver callers have the same staging guard independently of the
  // facade's row counters. In particular an ordinary B/W snapshot is not a
  // completed gray pass, and a second MSB may not fold an already-restored base.
  for (unsigned bad = 0; bad < 5; ++bad) {
    base();
    if (bad >= 1) driver.copyGrayscaleLsb(display._bus, lsb.data());
    if (bad >= 2) driver.copyGrayscaleMsb(display._bus, msb.data());
    if (bad == 2) driver.copyGrayscaleMsb(display._bus, msb.data());
    if (bad == 3) driver.copyGrayscaleLsb(display._bus, nullptr);
    display._bus.clear();
    driver.displayGray(display._bus, bw.data(), false, nullptr, bad == 4);
    assert(darkActivations(display._bus) == 0 && driver._needFullClear);
    display.cleanupGrayscaleBuffers(bw.data());
  }

  // Capability negative controls: no other controller/panel/version can opt in.
  for (uint8_t variant : {0x02, 0x03, 0x67, 0x69, 0xff}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    assert(!display.grayscaleCapabilities().supported());
    display._bus.clear();
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    display.displayGrayBuffer();
    assert(display._bus.writes.empty());
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  for (auto board : {BoardConfig::Board::XteinkX4, BoardConfig::Board::XteinkX3Uc8279}) {
    BoardConfig::ACTIVE.board = board;
    assert(!display.grayscaleCapabilities().supported());
  }
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4Pro;
  BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8179;
  assert(!display.grayscaleCapabilities().supported());
  BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
  auto* allocation = driver._grayBase;
  driver._grayBase = nullptr;
  assert(!display.grayscaleCapabilities().supported());
  driver._grayBase = allocation;
  assert(display.grayscaleCapabilities().supported());
  display.deepSleep();
  display.releaseBuffers();
  free(driver._grayBase);
  BoardConfig::ACTIVE = savedProfile;
  std::puts("UC8279 LUT68 inverted native AA: selectors, waveform, cancel/recovery and capability gates passed");
}
