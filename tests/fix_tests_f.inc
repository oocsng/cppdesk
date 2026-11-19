// Regression tests for full-review fix worker F. Only worker F edits this file.
void testFixesF() {
    // #27: openUrl scheme validation rejects non-http(s), control chars, and whitespace.
    {
        TEST_ASSERT(UiPlatform::isSafeExternalUrl("https://github.com/oocsng/cppdesk/releases/latest"));
        TEST_ASSERT(UiPlatform::isSafeExternalUrl("http://example.com/path?x=1&y=2"));
        TEST_ASSERT(UiPlatform::isSafeExternalUrl("HTTPS://EXAMPLE.COM/release"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl(""));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("http://"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("https://"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("file:///C:/Windows/System32/cmd.exe"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("\\\\attacker\\share\\payload.exe"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("ms-msdt:/id"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("smb://server/share"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("javascript:alert(1)"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("-help"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("https://example.com/has space"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl("https://example.com/line\nbreak"));
        TEST_ASSERT(!UiPlatform::isSafeExternalUrl(std::string("https://example.com/ctrl\x07")));
    }

    // #120: parseFileUriList rejects %00 and embedded NUL bytes.
    {
        auto ok = UiPlatform::parseFileUriList(
            "file:///tmp/hello%20world.txt\r\n"
            "file://localhost/tmp/local.txt\n"
            "file:///tmp/safe%00.txt\n");
        TEST_ASSERT(ok.size() == 2);
        TEST_ASSERT(ok[0] == "/tmp/hello world.txt");
        TEST_ASSERT(ok[1] == "/tmp/local.txt");

        std::string rawNul = "file:///tmp/a";
        rawNul.push_back('\0');
        rawNul += "b.txt\n";
        TEST_ASSERT(UiPlatform::parseFileUriList(rawNul).empty());
    }

    // #199: stepSpring stays finite and stable for large dt and rejects non-positive/NaN dt.
    {
        float pos = 0.0f;
        float vel = 0.0f;
        for (int i = 0; i < 120; ++i) {
            UiPlatform::stepSpring(pos, vel, 1.0f, 38.0f, 0.7f, 1.0f);
            TEST_ASSERT(std::isfinite(pos));
            TEST_ASSERT(std::isfinite(vel));
        }
        TEST_ASSERT(std::fabs(pos - 1.0f) < 0.01f);

        float p2 = 0.25f, v2 = 0.5f;
        TEST_ASSERT(!UiPlatform::stepSpring(p2, v2, 1.0f, 38.0f, 0.7f, -0.5f));
        TEST_ASSERT(!UiPlatform::stepSpring(p2, v2, 1.0f, 38.0f, 0.7f, std::numeric_limits<float>::quiet_NaN()));
        TEST_ASSERT(p2 == 0.25f && v2 == 0.5f);
    }

    // #200: dpiScaleFromXftDpi rejects NaN/Inf and returns 1.0f.
    {
        TEST_ASSERT(UiPlatform::dpiScaleFromXftDpi("nan") == 1.0f);
        TEST_ASSERT(UiPlatform::dpiScaleFromXftDpi("NaN") == 1.0f);
        TEST_ASSERT(UiPlatform::dpiScaleFromXftDpi("inf") == 1.0f);
        TEST_ASSERT(UiPlatform::dpiScaleFromXftDpi("-inf") == 1.0f);
        TEST_ASSERT(UiPlatform::dpiScaleFromXftDpi("0") == 1.0f);
        TEST_ASSERT(UiPlatform::dpiScaleFromXftDpi("-96") == 1.0f);
        TEST_ASSERT(std::fabs(UiPlatform::dpiScaleFromXftDpi("192") - 2.0f) < 0.001f);
    }

    // #197: glideAlong and glideEdgeAt handle empty/degenerate rects without NaN/Inf.
    {
        const ui::Rect empty{ 0.0f, 0.0f, 0.0f, 0.0f };
        const ui::Rect inverted{ 20.0f, 20.0f, 10.0f, 10.0f };
        for (GlideEdge e : { GlideEdge::Left, GlideEdge::Right, GlideEdge::Top, GlideEdge::Bottom }) {
            const float a = glideAlong(empty, e, { 0.0f, 0.0f });
            const float b = glideAlong(inverted, e, { 15.0f, 15.0f });
            TEST_ASSERT(std::isfinite(a) && a == 0.0f);
            TEST_ASSERT(std::isfinite(b) && b == 0.0f);
        }
        TEST_ASSERT(glideEdgeAt(empty, { 0.0f, 0.0f }) == GlideEdge::Off);
        TEST_ASSERT(glideEdgeAt(inverted, { 15.0f, 15.0f }) == GlideEdge::Off);
    }

    // #108: NotificationManager rate-limiting, UTF-8-safe truncation, and Pango markup escaping.
    {
        TEST_ASSERT(NotificationManager::escapePangoMarkup("<b>hi & bye</b>") == "&lt;b&gt;hi &amp; bye&lt;/b&gt;");
        TEST_ASSERT(NotificationManager::escapePangoMarkup("plain text") == "plain text");

        // 3-byte UTF-8 character (EUR sign U+20AC: E2 82 AC) must not be split mid-sequence.
        const std::string euros = "\xE2\x82\xAC\xE2\x82\xAC";
        TEST_ASSERT(NotificationManager::truncateForNotification(euros, 4) == "\xE2\x82\xAC");
        TEST_ASSERT(NotificationManager::truncateForNotification("abcdef", 4) == "abcd");

        NotificationManager nm;
        TEST_ASSERT(nm.allowExternalSpawn(1000));
        TEST_ASSERT(nm.allowExternalSpawn(1010));
        TEST_ASSERT(nm.allowExternalSpawn(1020));
        TEST_ASSERT(!nm.allowExternalSpawn(1030));
        TEST_ASSERT(!nm.allowExternalSpawn(1999));
        TEST_ASSERT(nm.allowExternalSpawn(2000));
        TEST_ASSERT(!nm.allowExternalSpawn(2500));
        TEST_ASSERT(nm.allowExternalSpawn(3000));
    }

#if defined(_WIN32)
    // #109: D2DCanvas::unwindState pops interleaved opacity layers and clips in LIFO order.
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ID2D1Factory* d2d = nullptr;
        IDWriteFactory* dwrite = nullptr;
        IWICImagingFactory* wic = nullptr;
        IWICBitmap* bmp = nullptr;
        ID2D1RenderTarget* rt = nullptr;
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) && d2d &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(&dwrite))) && dwrite &&
            SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && wic &&
            SUCCEEDED(wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) && bmp) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            if (SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bmp, props, &rt)) && rt) {
                ui::D2DCanvas canvas(d2d, dwrite);
                canvas.attach(rt, 1.0f);

                // Leave pushOpacity -> pushClip unpopped; LIFO unwind must succeed without D2DERR_POP_CALL_DID_NOT_MATCH_PUSH.
                canvas.beginFrame(64.0f, 64.0f, ui::Color{ 0, 0, 0, 1 });
                canvas.pushOpacity(0.5f);
                canvas.pushClip({ 0.0f, 0.0f, 32.0f, 32.0f });
                canvas.fillRect({ 4.0f, 4.0f, 20.0f, 20.0f }, ui::Color{ 1, 0, 0, 1 });
                TEST_ASSERT(!canvas.frameBalanced());
                canvas.endFrame();
                TEST_ASSERT(canvas.frameBalanced());
                TEST_ASSERT(!canvas.consumeRecreateNeeded());

                // Verify nested clip -> opacity -> clip also unwinds cleanly and leaves the target usable.
                canvas.beginFrame(64.0f, 64.0f, ui::Color{ 0, 0, 0, 1 });
                canvas.pushClip({ 0.0f, 0.0f, 64.0f, 64.0f });
                canvas.pushOpacity(0.8f);
                canvas.pushClip({ 8.0f, 8.0f, 40.0f, 40.0f });
                canvas.endFrame();
                TEST_ASSERT(canvas.frameBalanced());
                TEST_ASSERT(!canvas.consumeRecreateNeeded());

                canvas.detach();
                rt->Release();
            }
        }
        if (bmp) bmp->Release();
        if (wic) wic->Release();
        if (dwrite) dwrite->Release();
        if (d2d) d2d->Release();
        CoUninitialize();
    }
#endif

#if defined(__linux__)
    // #110: CairoCanvas::roundRectPath handles zero/negative-size rects without std::clamp UB.
    {
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 32, 32);
        ui::CairoCanvas canvas;
        canvas.attach(s, 1.0f);
        canvas.beginFrame(32.0f, 32.0f, ui::Color{ 0, 0, 0, 1 });
        canvas.fillRoundRect({ 10.0f, 10.0f, 5.0f, 5.0f }, 4.0f, ui::Color{ 1, 1, 1, 1 });
        canvas.strokeRoundRect({ 0.0f, 0.0f, 0.0f, 0.0f }, 4.0f, ui::Color{ 1, 1, 1, 1 }, 1.0f);
        canvas.endFrame();
        canvas.detach();
        TEST_ASSERT(cairo_surface_status(s) == CAIRO_STATUS_SUCCESS);
        cairo_surface_destroy(s);
    }

    // #216: spawn::onPath must reject directories that match the program name.
    {
        namespace fs = std::filesystem;
        const fs::path tmpDir = fs::temp_directory_path() / "cppdesk_fix_f_path_test";
        const fs::path subDir = tmpDir / "cppdesk_fake_dir_tool";
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
        fs::create_directories(subDir, ec);
        const char* origPath = std::getenv("PATH");
        const std::string savedPath = origPath ? origPath : "";
        setenv("PATH", tmpDir.string().c_str(), 1);
        TEST_ASSERT(!spawn::onPath("cppdesk_fake_dir_tool"));
        if (origPath) setenv("PATH", savedPath.c_str(), 1);
        else unsetenv("PATH");
        fs::remove_all(tmpDir, ec);
    }
#endif

#if defined(__APPLE__)
    // #122 & #194: CGCanvas attach(nullptr), singular transform recovery, and trailing whitespace width.
    {
        ui::CGCanvas canvas;
        canvas.attach(nullptr, 1.0f);
        TEST_ASSERT(canvas.measureTextWidth("hello ", ui::FontRole::Body) >
                    canvas.measureTextWidth("hello", ui::FontRole::Body));

        std::vector<uint8_t> px(32 * 32 * 4, 0);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(px.data(), 32, 32, 8, 32 * 4, cs,
                                                 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(cs);
        if (ctx) {
            canvas.attach(ctx, 1.0f);
            canvas.beginFrame(32.0f, 32.0f, ui::Color{ 0, 0, 0, 1 });
            canvas.setTransform(ui::Transform2D::scale(0.0f, 0.0f, 16.0f, 16.0f));
            canvas.fillRect({ 0.0f, 0.0f, 32.0f, 32.0f }, ui::Color{ 1, 0, 0, 1 });
            canvas.setTransform(ui::Transform2D::identity());
            canvas.fillRect({ 0.0f, 0.0f, 32.0f, 32.0f }, ui::Color{ 0, 1, 0, 1 });
            canvas.endFrame();
            canvas.detach();
            CGContextRelease(ctx);
            // Green pixel must have been drawn after recovering from the singular scale(0, 0) transform.
            TEST_ASSERT(px[1] > 200);
        }
    }
#endif
}
