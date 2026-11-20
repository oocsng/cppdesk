// Regression tests for full-review fix worker A. Only worker A edits this file.

#if defined(_WIN32)
#include <sddl.h>
#endif

static void testFixesA_ProtocolAndQr() {
    // #180: version constants agree
    TEST_ASSERT(PROTOCOL_VERSION == 4);
    TEST_ASSERT(CPP_DESK_VERSION_NUM == 0x030C00);

    // #217: ByteWriter is non-copyable
    static_assert(!std::is_copy_constructible_v<ByteWriter>, "ByteWriter must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<ByteWriter>, "ByteWriter must not be copy-assignable");

    // #177: deserializePerformanceHud accepts legacy 16-byte payload
    {
        ByteWriter w;
        w.writeF32(1.5f);
        w.writeF32(2.5f);
        w.writeU32(42);
        w.writeF32(0.75f);
        TEST_ASSERT(w.buffer().size() == 16);
        PerformanceHudPayload hud{};
        hud.hostFps = 99.0f;
        TEST_ASSERT(deserializePerformanceHud(w.buffer().data(), w.buffer().size(), hud));
        TEST_ASSERT(hud.dirtyTilesCount == 42);
        TEST_ASSERT(hud.hostFps == 0.0f);
    }

    // #178: unbounded reserve rejected for SystemDiagnostics and MonitorList
    {
        ByteWriter w;
        w.writeF32(10.0f);
        w.writeU64(1); w.writeU64(2); w.writeU64(3); w.writeU64(4);
        w.writeU16(65535);
        SystemDiagnosticsPayload diag{};
        TEST_ASSERT(!deserializeSystemDiagnostics(w.buffer().data(), w.buffer().size(), diag));

        uint8_t tinyMon[2] = { 0xFF, 0xFF };
        std::vector<MonitorDesc> mons;
        TEST_ASSERT(!deserializeMonitorList(tinyMon, sizeof(tinyMon), mons));
    }

    // #210: multi-monitor legacy list without isVirtual/virtualId parses cleanly
    {
        ByteWriter legacy;
        legacy.writeU16(2);
        for (int i = 0; i < 2; ++i) {
            legacy.writeI32(i);
            legacy.writeI32(i * 1920);
            legacy.writeI32(0);
            legacy.writeI32(1920);
            legacy.writeI32(1080);
            legacy.writeU8(i == 0 ? 1 : 0);
            legacy.writeString("Monitor" + std::to_string(i));
        }
        std::vector<MonitorDesc> parsed;
        TEST_ASSERT(deserializeMonitorList(legacy.buffer().data(), legacy.buffer().size(), parsed));
        TEST_ASSERT(parsed.size() == 2);
        TEST_ASSERT(parsed[0].name == "Monitor0" && !parsed[0].isVirtual && parsed[0].virtualId == 0);
        TEST_ASSERT(parsed[1].name == "Monitor1" && parsed[1].x == 1920 && !parsed[1].isVirtual);

        // Extended multi-monitor list also round-trips
        std::vector<MonitorDesc> ext = parsed;
        ext[1].isVirtual = true;
        ext[1].virtualId = 77;
        std::vector<uint8_t> extBuf;
        serializeMonitorList(ext, extBuf);
        std::vector<MonitorDesc> extOut;
        TEST_ASSERT(deserializeMonitorList(extBuf.data(), extBuf.size(), extOut));
        TEST_ASSERT(extOut.size() == 2 && extOut[1].isVirtual && extOut[1].virtualId == 77);
    }

    // #70: QR split format information strip matches primary top-left copy bit-for-bit
    {
        QrMatrix qr = QrMatrix::generate("otpauth://totp/CppDesk:123456789?secret=JBSWY3DPEHPK3PXP", 1, 6);
        int N = qr.size();
        TEST_ASSERT(N >= 21);
        // Primary copy bits 14..0: (0,8)..(5,8), (7,8), (8,8), (8,7), (8,5)..(8,0)
        int primaryBits[15] = {
            qr.getModule(0, 8) ? 1 : 0,
            qr.getModule(1, 8) ? 1 : 0,
            qr.getModule(2, 8) ? 1 : 0,
            qr.getModule(3, 8) ? 1 : 0,
            qr.getModule(4, 8) ? 1 : 0,
            qr.getModule(5, 8) ? 1 : 0,
            qr.getModule(7, 8) ? 1 : 0,
            qr.getModule(8, 8) ? 1 : 0,
            qr.getModule(8, 7) ? 1 : 0,
            qr.getModule(8, 5) ? 1 : 0,
            qr.getModule(8, 4) ? 1 : 0,
            qr.getModule(8, 3) ? 1 : 0,
            qr.getModule(8, 2) ? 1 : 0,
            qr.getModule(8, 1) ? 1 : 0,
            qr.getModule(8, 0) ? 1 : 0,
        };
        // Split copy: bottom-left (8, N-1)..(8, N-7) holds bits 14..8; top-right (N-8, 8)..(N-1, 8) holds bits 7..0
        for (int k = 0; k < 7; ++k) {
            TEST_ASSERT((qr.getModule(8, N - 1 - k) ? 1 : 0) == primaryBits[k]);
        }
        for (int k = 0; k < 8; ++k) {
            TEST_ASSERT((qr.getModule(N - 8 + k, 8) ? 1 : 0) == primaryBits[7 + k]);
        }
    }

    // #209: QrMatrix rejects inputs exceeding maxVersion capacity
    {
        std::string tooLongV1(50, 'A');
        QrMatrix qr1 = QrMatrix::generate(tooLongV1, 1, 1);
        TEST_ASSERT(qr1.size() == 0);

        std::string tooLongV6(200, 'B');
        QrMatrix qr6 = QrMatrix::generate(tooLongV6, 1, 6);
        TEST_ASSERT(qr6.size() == 0);
    }
}

static void testFixesA_CryptoAndIdentity() {
    // #215: TotpManager::decodeBase32 rejects trailing garbage after '=' and empty inputs
    {
        std::vector<uint8_t> out;
        TEST_ASSERT(!TotpManager::decodeBase32("JBSWY3DP=INVALID!!!", out));
        TEST_ASSERT(!TotpManager::decodeBase32("====", out));
        TEST_ASSERT(!TotpManager::decodeBase32("", out));
        TEST_ASSERT(!TotpManager::decodeBase32("   ", out));
        TEST_ASSERT(!TotpManager::decodeBase32("A", out));
        TEST_ASSERT(TotpManager::decodeBase32("MZXW6YTB==", out) && !out.empty());
    }

    // #68: PBKDF2-HMAC-SHA256 verifier format & unique random salt per call
    {
        std::string v1 = CryptoUtils::hashPassword("TestPass!2026");
        std::string v2 = CryptoUtils::hashPassword("TestPass!2026");
        TEST_ASSERT(v1.rfind("pbkdf2_sha256$", 0) == 0);
        TEST_ASSERT(v1 != v2);
        uint32_t iter = 0;
        std::array<uint8_t, 16> salt{};
        std::array<uint8_t, 32> dk{};
        TEST_ASSERT(CryptoUtils::parsePasswordVerifier(v1, iter, salt, dk));
        TEST_ASSERT(iter >= CryptoUtils::kMinPbkdf2Iterations);
        std::array<uint8_t, 32> dkCheck{};
        TEST_ASSERT(CryptoUtils::pbkdf2Sha256("TestPass!2026", salt.data(), salt.size(), iter, dkCheck));
        TEST_ASSERT(dk == dkCheck);
    }

    auto tempBase = std::filesystem::temp_directory_path() / "cppdesk_fix_tests_a";
    std::error_code ec;
    std::filesystem::remove_all(tempBase, ec);
    std::filesystem::create_directories(tempBase, ec);

    // #4: regenerateSessionCode must NOT overwrite persisted unattendedVerifier_
    {
        auto cfgPath = tempBase / "fix4.ini";
        IdentityManager id1(91);
        id1.setConfigPathForTest(cfgPath);
        TEST_ASSERT(id1.loadOrCreate());
        id1.setUnattendedPassword("StrongUnattended#1");
        std::string savedVerifier = id1.passwordVerifier();
        TEST_ASSERT(!savedVerifier.empty());

        // Simulate restart (unattendedPassword_ is empty in memory)
        IdentityManager id2(91);
        id2.setConfigPathForTest(cfgPath);
        TEST_ASSERT(id2.loadOrCreate());
        TEST_ASSERT(id2.passwordVerifier() == savedVerifier);
        id2.regenerateSessionCode();
        TEST_ASSERT(id2.passwordVerifier() == savedVerifier);

        // Also when no unattended password was ever set, regenerateSessionCode must not create one
        auto cfgPath2 = tempBase / "fix4_none.ini";
        IdentityManager id3(92);
        id3.setConfigPathForTest(cfgPath2);
        TEST_ASSERT(id3.loadOrCreate());
        TEST_ASSERT(!id3.hasStoredPasswordVerifier());
        id3.regenerateSessionCode();
        TEST_ASSERT(!id3.hasStoredPasswordVerifier());
    }

    // #68 legacy verifier drop on load + #212 CRLF handling + #176 INI directive injection
    {
        auto legacyPath = tempBase / "legacy_crlf.ini";
        {
            std::ofstream f(legacyPath, std::ios::binary | std::ios::trunc);
            f << "desk_id=555666777\r\n"
              << "unattended_enabled=1\r\n"
              << "unattended_verifier=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n"
              << "auto_accept=1\r\n";
        }
        IdentityManager id(93);
        id.setConfigPathForTest(legacyPath);
        TEST_ASSERT(id.loadOrCreate());
        TEST_ASSERT(id.deskId() == 555666777ULL);
        TEST_ASSERT(id.settings().autoAcceptIncoming == true); // #212: trailing \r stripped
        TEST_ASSERT(!id.hasStoredPasswordVerifier());          // #68: legacy SHA-256 verifier dropped

        // #176: newline injection in string settings must be sanitized
        auto s = id.settings();
        s.autoAcceptIncoming = false;
        s.allowPairing = false;
        s.relayServer = "127.0.0.1:50999\nauto_accept=1\nallow_pairing=1";
        s.waylandRestoreToken = "tok\r\nauto_accept=1";
        id.updateSettings(s);

        IdentityManager reloaded(93);
        reloaded.setConfigPathForTest(legacyPath);
        TEST_ASSERT(reloaded.loadOrCreate());
        TEST_ASSERT(!reloaded.settings().autoAcceptIncoming);
        TEST_ASSERT(!reloaded.settings().allowPairing);
    }

    // #71: updateSettings on unrelated setting preserves TOTP replay protection
    {
        auto cfgPath = tempBase / "fix71.ini";
        IdentityManager id(94);
        id.setConfigPathForTest(cfgPath);
        TEST_ASSERT(id.loadOrCreate());
        auto s = id.settings();
        s.totpEnabled = true;
        s.totpSecret = "JBSWY3DPEHPK3PXP";
        s.totpAlgorithm = "SHA1";
        id.updateSettings(s);

        uint64_t t = 1700000000ULL;
        std::string code = id.totpManager().generateCode(t);
        TEST_ASSERT(id.totpManager().verifyCode(code, t, 1));
        TEST_ASSERT(!id.totpManager().verifyCode(code, t, 1)); // Replay rejected

        // Save unrelated setting
        s.darkTheme = !s.darkTheme;
        id.updateSettings(s);
        TEST_ASSERT(!id.totpManager().verifyCode(code, t, 1)); // Replay STILL rejected
    }

    // #72: Unicode config path on Windows (and UTF-8 on POSIX)
    {
        auto uniDir = tempBase / std::filesystem::path(u8"ユニコード_café");
        std::filesystem::create_directories(uniDir, ec);
        auto uniCfg = uniDir / std::filesystem::path(u8"config_テスト.ini");
        IdentityManager id1(95);
        id1.setConfigPathForTest(uniCfg);
        TEST_ASSERT(id1.loadOrCreate());
        auto s = id1.settings();
        s.darkTheme = true;
        id1.updateSettings(s);

        IdentityManager id2(95);
        id2.setConfigPathForTest(uniCfg);
        TEST_ASSERT(id2.loadOrCreate());
        TEST_ASSERT(id2.deskId() == id1.deskId());
        TEST_ASSERT(id2.settings().darkTheme == true);
    }

    // #213 & #208: AesGcmSessionCipher null-check + concurrent encrypt/decrypt stress
    {
        std::array<uint8_t, 32> key{};
        TEST_ASSERT(CryptoUtils::randomBytes(key.data(), key.size()));
        AesGcmSessionCipher hostC, viewerC;
        TEST_ASSERT(hostC.initialize(key, true));
        TEST_ASSERT(viewerC.initialize(key, false));

        std::vector<uint8_t> out;
        TEST_ASSERT(!hostC.decrypt(nullptr, 28, nullptr, 0, out));
        TEST_ASSERT(!hostC.encrypt(nullptr, 10, 1, nullptr, 0, out));

        std::atomic<bool> ok{true};
        std::thread tEnc([&]() {
            for (uint64_t seq = 1; seq <= 200; ++seq) {
                std::vector<uint8_t> enc;
                const char msg[] = "concurrent-host-to-viewer";
                if (!hostC.encrypt(msg, sizeof(msg), seq, nullptr, 0, enc)) {
                    ok.store(false);
                    break;
                }
            }
        });
        std::thread tDec([&]() {
            for (uint64_t seq = 1; seq <= 200; ++seq) {
                std::vector<uint8_t> enc, dec;
                const char msg[] = "concurrent-viewer-to-host";
                if (!viewerC.encrypt(msg, sizeof(msg), seq, nullptr, 0, enc) ||
                    !hostC.decrypt(enc.data(), enc.size(), nullptr, 0, dec)) {
                    ok.store(false);
                    break;
                }
            }
        });
        tEnc.join();
        tDec.join();
        TEST_ASSERT(ok.load());
    }

    // #42: Concurrent IdentityManager + TotpManager access
    {
        auto cfgPath = tempBase / "fix42.ini";
        IdentityManager id(96);
        id.setConfigPathForTest(cfgPath);
        TEST_ASSERT(id.loadOrCreate());
        TEST_ASSERT(id.totpManager().generateNewSecret());

        std::thread t1([&]() {
            for (int i = 0; i < 20; ++i) {
                auto s = id.settings();
                s.darkTheme = (i % 2 == 0);
                id.updateSettings(s);
            }
        });
        std::thread t2([&]() {
            for (int i = 0; i < 20; ++i) {
                id.addOrUpdateRecentSession(100000000ULL + i, "host", "127.0.0.1:50990");
                auto list = id.recentSessions();
                (void)list.size();
                auto code = id.totpManager().generateCode(1700000000ULL + static_cast<uint64_t>(i) * 30);
                (void)id.totpManager().verifyCode(code, 1700000000ULL + static_cast<uint64_t>(i) * 30, 1);
            }
        });
        t1.join();
        t2.join();
    }

    std::filesystem::remove_all(tempBase, ec);
}

#if defined(_WIN32)
static void testFixesA_WindowsServiceAndRebootToken() {
    // #10 & #137: disabled service install/start/stop return clear failure
    TEST_ASSERT(!WindowsServiceManager::installService());
    TEST_ASSERT(GetLastError() == ERROR_NOT_SUPPORTED);
    TEST_ASSERT(!WindowsServiceManager::startService());
    TEST_ASSERT(GetLastError() == ERROR_NOT_SUPPORTED);
    TEST_ASSERT(!WindowsServiceManager::stopService());
    TEST_ASSERT(GetLastError() == ERROR_NOT_SUPPORTED);

    // #1: Reboot token with test override: (a) save & single-use consume; (b) permissive dir/file refused
    auto base = std::filesystem::temp_directory_path() / L"cppdesk_fix1_reboot";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);

    // (a) Clean dir created with strict DACL under test override
    auto strictDir = base / L"strict";
    WindowsServiceManager::setRebootTokenDirForTest(strictDir.wstring());
    std::string tok = WindowsServiceManager::generateRebootTokenHex();
    TEST_ASSERT(tok.size() == 64);
    TEST_ASSERT(WindowsServiceManager::saveRebootToken(tok, 777888999ULL, 300));
    TEST_ASSERT(WindowsServiceManager::hasPendingRebootToken());
    TEST_ASSERT(WindowsServiceManager::validateAndConsumeRebootToken(tok, 777888999ULL));
    TEST_ASSERT(!WindowsServiceManager::validateAndConsumeRebootToken(tok, 777888999ULL));

    // (b) Pre-existing directory with permissive ACL (Everyone full access) must be refused
    auto permissiveDir = base / L"permissive_dir";
    {
        PSECURITY_DESCRIPTOR pSD = nullptr;
        TEST_ASSERT(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;GA;;;WD)", SDDL_REVISION_1, &pSD, nullptr) != FALSE);
        SECURITY_ATTRIBUTES sa{ sizeof(sa), pSD, FALSE };
        TEST_ASSERT(CreateDirectoryW(permissiveDir.c_str(), &sa) != FALSE);
        LocalFree(pSD);
    }
    WindowsServiceManager::setRebootTokenDirForTest(permissiveDir.wstring());
    TEST_ASSERT(!WindowsServiceManager::saveRebootToken(tok, 777888999ULL, 300));

    // (c) Pre-existing file with permissive ACL inside strict dir must be refused
    auto strictDir2 = base / L"strict_with_bad_file";
    WindowsServiceManager::setRebootTokenDirForTest(strictDir2.wstring());
    TEST_ASSERT(WindowsServiceManager::saveRebootToken(tok, 777888999ULL, 300));
    TEST_ASSERT(WindowsServiceManager::validateAndConsumeRebootToken(tok, 777888999ULL));
    {
        auto badFile = strictDir2 / L"reboot_token.bin";
        PSECURITY_DESCRIPTOR pSD = nullptr;
        TEST_ASSERT(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;WD)", SDDL_REVISION_1, &pSD, nullptr) != FALSE);
        SECURITY_ATTRIBUTES sa{ sizeof(sa), pSD, FALSE };
        HANDLE h = CreateFileW(badFile.c_str(), GENERIC_WRITE, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LocalFree(pSD);
        TEST_ASSERT(h != INVALID_HANDLE_VALUE);
        CloseHandle(h);
    }
    TEST_ASSERT(!WindowsServiceManager::saveRebootToken(tok, 777888999ULL, 300));
    TEST_ASSERT(!WindowsServiceManager::hasPendingRebootToken());
    TEST_ASSERT(!WindowsServiceManager::validateAndConsumeRebootToken(tok, 777888999ULL));

    WindowsServiceManager::setRebootTokenDirForTest(L"");
    std::filesystem::remove_all(base, ec);
}
#endif

static void testFixesA_HandshakeAndRateLimiter() {
    // End-to-end loopback handshake with PBKDF2-HMAC-SHA256 bound auth (#12, #68)
    auto tempBase = std::filesystem::temp_directory_path() / "cppdesk_fix_tests_a_net";
    std::error_code ec;
    std::filesystem::remove_all(tempBase, ec);
    std::filesystem::create_directories(tempBase, ec);

    IdentityManager hostId(97);
    hostId.setConfigPathForTest(tempBase / "host.ini");
    TEST_ASSERT(hostId.loadOrCreate());
    hostId.setUnattendedEnabled(true);
    hostId.setUnattendedPassword("BoundPbkdf2#2026");

    IdentityManager viewerId(98);
    viewerId.setConfigPathForTest(tempBase / "viewer.ini");
    TEST_ASSERT(viewerId.loadOrCreate());

    NetworkEngine hostEng(hostId);
    NetworkEngine viewerEng(viewerId);
    hostEng.setAutoAcceptIncoming(false);
    TEST_ASSERT(hostEng.start());
    TEST_ASSERT(viewerEng.start());
    uint16_t port = hostId.listenPort();
    TEST_ASSERT(port != 0);

    // Valid PBKDF2 bound password connects
    std::string target = "127.0.0.1:" + std::to_string(port);
    TEST_ASSERT(viewerEng.connectToRemote(target, "BoundPbkdf2#2026"));
    bool connected = false;
    for (int i = 0; i < 60; ++i) {
        if (viewerEng.viewerStats().state == ViewerConnectionState::Connected) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TEST_ASSERT(connected);
    viewerEng.disconnectViewer();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Wrong password fails
    TEST_ASSERT(viewerEng.connectToRemote(target, "WrongPassword!99"));
    bool sawError = false;
    for (int i = 0; i < 60; ++i) {
        if (viewerEng.viewerStats().state == ViewerConnectionState::Error) {
            sawError = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TEST_ASSERT(sawError);
    viewerEng.disconnectViewer();
    viewerEng.stop();
    hostEng.stop();
    std::filesystem::remove_all(tempBase, ec);
}

void testFixesA() {
    std::cout << "[TEST FIXES A] Auth, crypto, protocol core and reboot token...\n";
    testFixesA_ProtocolAndQr();
    testFixesA_CryptoAndIdentity();
#if defined(_WIN32)
    testFixesA_WindowsServiceAndRebootToken();
#endif
    testFixesA_HandshakeAndRateLimiter();
}
