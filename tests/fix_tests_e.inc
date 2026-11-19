// Regression tests for full-review fix worker E. Only worker E edits this file.

static std::vector<uint8_t> makeCraftedHostileAvi(uint32_t idx1Size, int32_t biHeight) {
    std::vector<uint8_t> buf;
    auto appendRaw = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    };
    auto appendU32 = [&](uint32_t v) { appendRaw(&v, 4); };

    MainAviHeader avih{};
    avih.dwMicroSecPerFrame = 33333;
    avih.dwTotalFrames = 1;
    avih.dwStreams = 1;
    avih.dwWidth = 16;
    avih.dwHeight = 16;

    AviStreamHeader strh{};
    strh.fccType = FOURCC_vids;
    strh.fccHandler = FOURCC_DIB;
    strh.dwScale = 1;
    strh.dwRate = 30;
    strh.dwLength = 1;

    AviBmpInfoHeader bih{};
    bih.biSize = sizeof(AviBmpInfoHeader);
    bih.biWidth = 16;
    bih.biHeight = biHeight;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = 0;

    uint32_t strlSize = 4 + (8 + sizeof(AviStreamHeader)) + (8 + sizeof(AviBmpInfoHeader));
    uint32_t hdrlSize = 4 + (8 + sizeof(MainAviHeader)) + (8 + strlSize);

    appendRaw("RIFF", 4);
    appendU32(0); // patched below
    appendRaw("AVI ", 4);

    appendRaw("LIST", 4);
    appendU32(hdrlSize);
    appendRaw("hdrl", 4);

    appendRaw("avih", 4);
    appendU32(sizeof(MainAviHeader));
    appendRaw(&avih, sizeof(avih));

    appendRaw("LIST", 4);
    appendU32(strlSize);
    appendRaw("strl", 4);

    appendRaw("strh", 4);
    appendU32(sizeof(AviStreamHeader));
    appendRaw(&strh, sizeof(strh));

    appendRaw("strf", 4);
    appendU32(sizeof(AviBmpInfoHeader));
    appendRaw(&bih, sizeof(bih));

    appendRaw("LIST", 4);
    appendU32(4);
    appendRaw("movi", 4);

    appendRaw("idx1", 4);
    appendU32(idx1Size);

    uint32_t riffSize = static_cast<uint32_t>(buf.size() - 8);
    std::memcpy(buf.data() + 4, &riffSize, 4);
    return buf;
}

void testFixesE() {
    std::cout << "[TEST FIXES E] Capture, Media, SIMD regression suite...\n";

    // #2 & #250: VirtualDisplayManager setVirtualDisplayMode preserves s.info on rejected resize & 128 MB cap
    {
        auto& vdm = VirtualDisplayManager::instance();
        vdm.destroyAllSessionDisplays();
        TEST_ASSERT(!VirtualDisplayManager::hasIddCxDriver());

        uint32_t id = 0;
        TEST_ASSERT(vdm.createVirtualDisplay(320, 240, 60, true, &id));
        VirtualDisplayInfo info{};
        TEST_ASSERT(vdm.getDisplayInfo(id, info));
        TEST_ASSERT(info.backend == VirtualBackend::Software);
        TEST_ASSERT(info.width == 320 && info.height == 240);

        // Reject >8192 width without mutating s.info
        TEST_ASSERT(!vdm.setVirtualDisplayMode(id, 9000, 1080, 60));
        TEST_ASSERT(vdm.getDisplayInfo(id, info));
        TEST_ASSERT(info.width == 320 && info.height == 240);

        // Reject 8192x8192 (> 128 MB cap) without mutating s.info
        TEST_ASSERT(!vdm.setVirtualDisplayMode(id, 8192, 8192, 60));
        TEST_ASSERT(vdm.getDisplayInfo(id, info));
        TEST_ASSERT(info.width == 320 && info.height == 240);

        // Capture still succeeds safely at original 320x240
        std::vector<uint8_t> bgra;
        int cw = 0, ch = 0;
        TEST_ASSERT(vdm.captureSoftwareSurface(id, bgra, cw, ch));
        TEST_ASSERT(cw == 320 && ch == 240 && bgra.size() == 320u * 240u * 4u);

        // Creating >128 MB display is rejected directly
        uint32_t badId = 0;
        TEST_ASSERT(!vdm.createVirtualDisplay(8192, 8192, 60, false, &badId));
        vdm.destroyAllSessionDisplays();
    }

#if defined(_WIN32)
    // #11, #56, #160: ScreenBlankManager DpmsOnly creates non-maximized curtain + async emergency wake
    {
        auto& sbm = ScreenBlankManager::instance();
        sbm.disengageBlanking();

        std::atomic<bool> wakeCalled{false};
        std::thread::id callerTid = std::this_thread::get_id();
        std::atomic<bool> calledOffThread{false};
        sbm.setEmergencyWakeCallback([&]() {
            calledOffThread.store(std::this_thread::get_id() != callerTid);
            wakeCalled.store(true);
        });

        TEST_ASSERT(sbm.engageBlanking(ScreenBlankMode::DpmsOnly, "Test", "Brand", false, 0));
        TEST_ASSERT(sbm.isBlankActive());

        // #11 & #160: Curtain window must exist even for DpmsOnly and must NOT be WS_MAXIMIZE
        HWND hwnd = sbm.curtainHwnd();
        TEST_ASSERT(hwnd != nullptr);
        if (hwnd) {
            TEST_ASSERT(IsWindow(hwnd) != 0);
            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            TEST_ASSERT((style & WS_MAXIMIZE) == 0);
        }

        // #56: Async emergency wake fires off the hook/caller thread
        sbm.requestAsyncEmergencyWake();
        TEST_ASSERT(!sbm.isBlankActive());
        for (int i = 0; i < 100 && !wakeCalled.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        TEST_ASSERT(wakeCalled.load());
        TEST_ASSERT(calledOffThread.load());

        sbm.disengageBlanking();
        sbm.setEmergencyWakeCallback(nullptr);
    }
#endif

    // #50: TileCodec::decodeJpeg* rejects non-JPEG payloads and oversized SOF headers before allocation
    {
        int beforeOversized = g_oversizedAllocs.load();
        std::vector<uint8_t> out;
        int w = 0, h = 0;

        // Fake PNG header
        const uint8_t fakePng[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0 };
        TEST_ASSERT(!TileCodec::decodeJpeg(fakePng, sizeof(fakePng), out, w, h));

        // Crafted JPEG SOI + SOF0 claiming 65000x65000 pixels (~16.9 GB BGRA)
        const uint8_t hugeSofJpeg[] = {
            0xFF, 0xD8,                   // SOI
            0xFF, 0xC0, 0x00, 0x0B,       // SOF0, length 11
            0x08,                         // precision
            0xFD, 0xE8,                   // height = 65000
            0xFD, 0xE8,                   // width = 65000
            0x01, 0x01, 0x11, 0x00,       // 1 component
            0xFF, 0xD9                    // EOI
        };
        TEST_ASSERT(!TileCodec::decodeJpeg(hugeSofJpeg, sizeof(hugeSofJpeg), out, w, h));
        TEST_ASSERT(g_oversizedAllocs.load() == beforeOversized);

        // Valid JPEG round-trip and dimension mismatch rejection in decodeJpegIntoCanvas
        std::vector<uint8_t> src(16 * 16 * 4, 0x80);
        auto jpg = TileCodec::encodeJpeg(src.data(), 16, 16, 80);
        if (!jpg.empty()) {
            TEST_ASSERT(TileCodec::decodeJpeg(jpg.data(), jpg.size(), out, w, h));
            TEST_ASSERT(w == 16 && h == 16 && out.size() == 16u * 16u * 4u);

            std::vector<uint8_t> canvas(32 * 32 * 4, 0);
            // Expects 32x32 while JPEG is 16x16 -> must reject
            TEST_ASSERT(!TileCodec::decodeJpegIntoCanvas(jpg.data(), jpg.size(), canvas.data(), 32, 32, 0, 0, 32, 32));
            // Exact match succeeds
            TEST_ASSERT(TileCodec::decodeJpegIntoCanvas(jpg.data(), jpg.size(), canvas.data(), 32, 32, 0, 0, 16, 16));
        }
    }

    // #52: TileThreadPool::parallelDecode falls back to sequential for overlapping tiles
    {
        std::vector<uint8_t> canvas(32 * 32 * 4, 0);
        std::vector<uint8_t> tileA(16 * 16 * 4);
        std::vector<uint8_t> tileB(16 * 16 * 4);
        for (size_t i = 0; i < 16 * 16; ++i) {
            tileA[i * 4 + 0] = 0x11; tileA[i * 4 + 1] = 0x22; tileA[i * 4 + 2] = 0x33; tileA[i * 4 + 3] = 0xFF;
            tileB[i * 4 + 0] = 0xAA; tileB[i * 4 + 1] = 0xBB; tileB[i * 4 + 2] = 0xCC; tileB[i * 4 + 3] = 0xFF;
        }
        std::vector<TileThreadPool::DecodeTask> tasks(2);
        tasks[0] = { 0, 0, 16, 16, TileEncoding::RawBGRA, tileA.data(), tileA.size(), canvas.data(), 32, 32 };
        tasks[1] = { 8, 8, 16, 16, TileEncoding::RawBGRA, tileB.data(), tileB.size(), canvas.data(), 32, 32 };
        TileThreadPool::instance().parallelDecode(tasks);

        // Pixel (10, 10) is in overlap -> second tile (tileB) must deterministically win
        const uint8_t* pxOverlap = canvas.data() + (10 * 32 + 10) * 4;
        TEST_ASSERT(pxOverlap[0] == 0xAA && pxOverlap[1] == 0xBB && pxOverlap[2] == 0xCC && pxOverlap[3] == 0xFF);
        // Pixel (2, 2) is only in tileA
        const uint8_t* pxOnlyA = canvas.data() + (2 * 32 + 2) * 4;
        TEST_ASSERT(pxOnlyA[0] == 0x11 && pxOnlyA[1] == 0x22 && pxOnlyA[2] == 0x33 && pxOnlyA[3] == 0xFF);
    }

    // #150: AVX2 tile-hash tail normalizes alpha and distinguishes reordered tail pixels
    if (SimdKernels::hasAvx2()) {
        const int w = 10; // 40 bytes = 32 vec + 8 tail bytes (2 tail pixels)
        const int h = 4;
        const int stride = w * 4;
        std::vector<uint8_t> bufA(static_cast<size_t>(stride) * h, 0x40);
        // Tail pixels at x=8 and x=9 on row 0
        bufA[8 * 4 + 0] = 10; bufA[8 * 4 + 1] = 20; bufA[8 * 4 + 2] = 30; bufA[8 * 4 + 3] = 0x00;
        bufA[9 * 4 + 0] = 40; bufA[9 * 4 + 1] = 50; bufA[9 * 4 + 2] = 60; bufA[9 * 4 + 3] = 0x11;

        std::vector<uint8_t> bufAlphaDiff = bufA;
        bufAlphaDiff[8 * 4 + 3] = 0xFF;
        bufAlphaDiff[9 * 4 + 3] = 0x77;

        std::vector<uint8_t> bufSwapped = bufA;
        std::memcpy(bufSwapped.data() + 8 * 4, bufA.data() + 9 * 4, 4);
        std::memcpy(bufSwapped.data() + 9 * 4, bufA.data() + 8 * 4, 4);

        uint64_t hA = cppdesk_avx2_hash_tile(bufA.data(), stride, stride, h);
        uint64_t hAlpha = cppdesk_avx2_hash_tile(bufAlphaDiff.data(), stride, stride, h);
        uint64_t hSwap = cppdesk_avx2_hash_tile(bufSwapped.data(), stride, stride, h);
        TEST_ASSERT(hA != 0);
        TEST_ASSERT(hA == hAlpha);
        TEST_ASSERT(hA != hSwap);
    }

    // #230: Null-pointer guards in TileCodec::encodeRect and LatestFrame::push
    {
        EncodedTile t = TileCodec::encodeRect(0, 0, 16, 16, nullptr, QualityPreset::Balanced);
        TEST_ASSERT(t.data.empty());

        LatestFrame lf;
        lf.accept(true);
        lf.push(nullptr, 16, 16);
        std::vector<uint8_t> out;
        int w = 0, h = 0;
        bool isNew = false;
        TEST_ASSERT(!lf.copy(out, w, h, isNew));
    }

    // #161: PrivacyCloakManager UTF-8 custom keywords round-trip & mosaic redaction
    {
        PrivacyCloakManager pcm;
        pcm.setCustomKeywords("Caf\xC3\xA9, \xE5\xAF\x86\xE7\xA0\x81");
        std::string kw = pcm.customKeywords();
        TEST_ASSERT(kw == "Caf\xC3\xA9, \xE5\xAF\x86\xE7\xA0\x81");

        pcm.setEnabled(true);
        pcm.setRedactionStyle(1);
        std::vector<uint8_t> frame(32 * 32 * 4, 0x40);
        RECT rc{ 0, 0, 16, 16 };
        pcm.redactCaptureBuffer(frame.data(), 32, 32, 0, 0, { rc });
        TEST_ASSERT(frame[3] == 0xFF);
    }

    // #162: SessionRewindBuffer rejects equal/backward timestamps and stays sorted
    {
        SessionRewindBuffer rb;
        std::vector<uint8_t> px(32 * 32 * 4, 0x55);
        rb.pushFrame(1000, px.data(), 32, 32);
        rb.pushFrame(1500, px.data(), 32, 32);
        rb.pushFrame(1500, px.data(), 32, 32); // duplicate -> rejected
        rb.pushFrame(1200, px.data(), 32, 32); // backward -> rejected
        rb.pushFrame(2000, px.data(), 32, 32);
        if (!rb.empty()) {
            TEST_ASSERT(rb.size() == 3);
            TEST_ASSERT(rb.oldestTimestampMs() == 1000);
            TEST_ASSERT(rb.newestTimestampMs() == 2000);
        }
        rb.clear();
        TEST_ASSERT(rb.empty());
    }

    // #163: DisplayResolutionManager does not corrupt state on failed changeResolution
    {
        DisplayResolutionManager drm;
        TEST_ASSERT(!drm.changeResolution(99999, 99999, L"\\\\.\\CPPDESK_NONEXISTENT_DISPLAY"));
        TEST_ASSERT(!drm.isResolutionChanged());
    }

    // #228: wayland::pointForNormalized guards NaN and Inf
    {
        wayland::StreamInfo s{};
        s.nodeId = 7;
        s.logicalW = 1920;
        s.logicalH = 1080;
        auto pNan = wayland::pointForNormalized(std::numeric_limits<float>::quiet_NaN(),
                                                std::numeric_limits<float>::infinity(), s, 1920, 1080);
        TEST_ASSERT(std::isfinite(pNan.x) && pNan.x == 0.0);
        TEST_ASSERT(std::isfinite(pNan.y) && pNan.y == 0.0);
    }

    // #55: SessionRecordingPlayer rejects hostile idx1 ckSize=0xFFFFFFFF and biHeight=INT32_MIN without huge alloc
    {
        int beforeOversized = g_oversizedAllocs.load();
        const std::string hostileIdxPath = "cppdesk_test_hostile_idx1.avi";
        const std::string hostileHeightPath = "cppdesk_test_hostile_height.avi";
        std::filesystem::remove(hostileIdxPath);
        std::filesystem::remove(hostileHeightPath);

        {
            auto bytes = makeCraftedHostileAvi(0xFFFFFFFFu, 16);
            std::ofstream f(hostileIdxPath, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        {
            auto bytes = makeCraftedHostileAvi(0, (std::numeric_limits<int32_t>::min)());
            std::ofstream f(hostileHeightPath, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        SessionRecordingPlayer player;
        TEST_ASSERT(!player.open(hostileIdxPath));
        TEST_ASSERT(!player.open(hostileHeightPath));
        TEST_ASSERT(g_oversizedAllocs.load() == beforeOversized);

        std::filesystem::remove(hostileIdxPath);
        std::filesystem::remove(hostileHeightPath);
    }

    // #57, #58, #157: Uncompressed BGRA with bottom-left pixel (B=0xFF, G=0xD8) decodes as BGRA (not MJPEG),
    // SessionRecorder restarts safely across resolutions, and callback updates are synchronized
    {
        const std::string recPath1 = "cppdesk_test_fixe_bgra_ffd8.avi";
        const std::string recPath2 = "cppdesk_test_fixe_restart.avi";
        std::filesystem::remove(recPath1);
        std::filesystem::remove(recPath2);

        SessionRecorder rec;
        TEST_ASSERT(rec.startRecording(recPath1, 16, 16, 30));
        std::vector<uint8_t> frame(16 * 16 * 4, 0x20);
        // Bottom-left pixel in top-down BGRA is row 15, col 0 -> first bytes of bottom-up DIB payload!
        // Set B=0xFF, G=0xD8, R=0x42, A=0xFF so payload starts with 0xFF 0xD8.
        size_t bl = (15 * 16 + 0) * 4;
        frame[bl + 0] = 0xFF;
        frame[bl + 1] = 0xD8;
        frame[bl + 2] = 0x42;
        frame[bl + 3] = 0xFF;
        rec.pushFrame(frame.data(), 16, 16);
        rec.pushFrame(frame.data(), 16, 16);
        rec.stopRecording();
        TEST_ASSERT(rec.recordedFrames() == 2);

        // #57: Restart recorder at a larger resolution (32x32)
        TEST_ASSERT(rec.startRecording(recPath2, 32, 32, 30));
        TEST_ASSERT(rec.droppedFrames() == 0);
        std::vector<uint8_t> frameBig(32 * 32 * 4, 0x77);
        rec.pushFrame(frameBig.data(), 32, 32);
        rec.stopRecording();
        TEST_ASSERT(rec.recordedFrames() == 1);

        // #58: Player must decode both frames as BGRA, NOT fail as corrupt MJPEG
        SessionRecordingPlayer player;
        std::atomic<int> cbCount{0};
        player.setOnFrameReady([&]() { ++cbCount; });
        TEST_ASSERT(player.open(recPath1));
        TEST_ASSERT(player.totalFrames() == 2);
        TEST_ASSERT(player.seekToFrame(1));
        std::vector<uint8_t> outBgra;
        int ow = 0, oh = 0;
        player.copyCurrentFrameBgra(outBgra, ow, oh);
        TEST_ASSERT(ow == 16 && oh == 16);
        TEST_ASSERT(outBgra[bl + 0] == 0xFF && outBgra[bl + 1] == 0xD8 && outBgra[bl + 2] == 0x42);
        TEST_ASSERT(cbCount.load() >= 2);
        player.setOnFrameReady(nullptr);
        player.close();

        std::filesystem::remove(recPath1);
        std::filesystem::remove(recPath2);
    }

    // #158: VoiceIntercom handles 2-byte stereo chunk without null-memcpy UB or crash
    {
        VoiceIntercom intercom;
        uint8_t shortStereo[2] = { 0x01, 0x02 };
        intercom.enqueuePlaybackChunk(shortStereo, sizeof(shortStereo), 2);
        TEST_ASSERT(!intercom.isPlaying());
    }
}
