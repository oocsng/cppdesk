// Regression tests for full-review fix worker C. Only worker C edits this file.

static void testFixesC_RelayAuthAndOwnership() {
    using namespace relayowner;

    // #15: computeAuthTag / stripAndVerifyAuthTag
    {
        std::vector<uint8_t> body = { 0x01, 0x02, 0x03, 0x04 };
        const std::string key = "relay-secret-42";
        Key tag = computeAuthTag(key, static_cast<uint8_t>(PacketType::RELAY_LOOKUP), body.data(), body.size());

        std::vector<uint8_t> tagged = body;
        tagged.insert(tagged.end(), tag.begin(), tag.end());
        TEST_ASSERT(stripAndVerifyAuthTag(key, static_cast<uint8_t>(PacketType::RELAY_LOOKUP), FLAG_RELAY_AUTH, tagged));
        TEST_ASSERT(tagged == body);

        // Missing auth flag rejected when relay key is configured.
        std::vector<uint8_t> untagged = body;
        TEST_ASSERT(!stripAndVerifyAuthTag(key, static_cast<uint8_t>(PacketType::RELAY_LOOKUP), 0, untagged));

        // Wrong key rejected.
        std::vector<uint8_t> wrongKey = body;
        Key badTag = computeAuthTag("wrong-key", static_cast<uint8_t>(PacketType::RELAY_LOOKUP), body.data(), body.size());
        wrongKey.insert(wrongKey.end(), badTag.begin(), badTag.end());
        TEST_ASSERT(!stripAndVerifyAuthTag(key, static_cast<uint8_t>(PacketType::RELAY_LOOKUP), FLAG_RELAY_AUTH, wrongKey));

        // Truncated tag rejected even when relay key is empty.
        std::vector<uint8_t> shortBuf = { 0xAA, 0xBB };
        TEST_ASSERT(!stripAndVerifyAuthTag("", static_cast<uint8_t>(PacketType::RELAY_LOOKUP), FLAG_RELAY_AUTH, shortBuf));

        // Empty relay key accepts untagged and cleanly strips tagged frames.
        std::vector<uint8_t> openTagged = body;
        openTagged.insert(openTagged.end(), tag.begin(), tag.end());
        TEST_ASSERT(stripAndVerifyAuthTag("", static_cast<uint8_t>(PacketType::RELAY_LOOKUP), FLAG_RELAY_AUTH, openTagged));
        TEST_ASSERT(openTagged == body);
    }

    // #66: OwnerRegistry full table fails closed after pruning expired entries.
    {
        OwnerRegistry reg(2);
        Key k1{}, k2{}, k3{};
        k1.fill(0x11);
        k2.fill(0x22);
        k3.fill(0x33);

        TEST_ASSERT(reg.enroll(10, k1, 0));
        TEST_ASSERT(reg.enroll(20, k2, 1000));
        TEST_ASSERT(!reg.enroll(30, k3, 2000));
        TEST_ASSERT(!reg.hasOwner(30, 2000));

        // Once ID 10 expires, pruning frees a slot while preserving live ID 20.
        const uint64_t afterExpiry = TTL_MS + 500;
        TEST_ASSERT(reg.enroll(30, k3, afterExpiry));
        TEST_ASSERT(reg.hasOwner(30, afterExpiry));
        TEST_ASSERT(reg.hasOwner(20, afterExpiry));
        TEST_ASSERT(!reg.hasOwner(10, afterExpiry));
    }

    // #15: Live TCP RelayServer enforces relayAuthKey on REGISTER, LOOKUP, and CONNECT_REQ.
    {
        socket_init_network();
        RelayServer relay;
        const std::string secret = "unit-test-relay-auth";
        relay.setAuthKey(secret);
        TEST_ASSERT(relay.start(0));
        const uint16_t port = relay.port();

        auto dial = [&]() -> uintptr_t {
            socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET_VAL) return ~uintptr_t(0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
#if defined(_WIN32)
            DWORD tv = 3000;
#else
            timeval tv{3, 0};
#endif
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
            if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                socket_close(s);
                return ~uintptr_t(0);
            }
            return fromSocket(s);
        };

        std::mutex mu;
        auto sendTagged = [&](uintptr_t s, PacketType pt, const std::vector<uint8_t>& body, const std::string& authKey) {
            if (authKey.empty()) {
                return NetworkEngine::sendFrame(s, pt, 0, body.data(), body.size(), mu);
            }
            std::vector<uint8_t> tagged = body;
            Key tag = computeAuthTag(authKey, static_cast<uint8_t>(pt), body.data(), body.size());
            tagged.insert(tagged.end(), tag.begin(), tag.end());
            return NetworkEngine::sendFrame(s, pt, FLAG_RELAY_AUTH, tagged.data(), tagged.size(), mu);
        };

        const uint64_t deskId = 812345679ULL;
        ByteWriter regW;
        regW.writeU64(deskId);
        regW.writeString("AuthTestHost");
        regW.writeString("127.0.0.1");
        regW.writeU16(50995);

        // Untagged register rejected.
        {
            uintptr_t s = dial();
            TEST_ASSERT(s != ~uintptr_t(0));
            TEST_ASSERT(sendTagged(s, PacketType::RELAY_REGISTER, regW.buffer(), ""));
            FrameHeader h{};
            std::vector<uint8_t> p;
            TEST_ASSERT(NetworkEngine::recvFrame(s, h, p));
            TEST_ASSERT(h.type == static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK));
            TEST_ASSERT(p.size() >= 1 && p[0] == 0);
            closeSocketHandle(s);
        }

        // Wrong key register rejected.
        {
            uintptr_t s = dial();
            TEST_ASSERT(s != ~uintptr_t(0));
            TEST_ASSERT(sendTagged(s, PacketType::RELAY_REGISTER, regW.buffer(), "wrong-secret"));
            FrameHeader h{};
            std::vector<uint8_t> p;
            TEST_ASSERT(NetworkEngine::recvFrame(s, h, p));
            TEST_ASSERT(h.type == static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK));
            TEST_ASSERT(p.size() >= 1 && p[0] == 0);
            closeSocketHandle(s);
        }

        // Correct key register accepted.
        uintptr_t hostSock = dial();
        TEST_ASSERT(hostSock != ~uintptr_t(0));
        TEST_ASSERT(sendTagged(hostSock, PacketType::RELAY_REGISTER, regW.buffer(), secret));
        {
            FrameHeader h{};
            std::vector<uint8_t> p;
            TEST_ASSERT(NetworkEngine::recvFrame(hostSock, h, p));
            TEST_ASSERT(h.type == static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK));
            TEST_ASSERT(p.size() >= 1 && p[0] == 1);
        }

        ByteWriter lookW;
        lookW.writeU64(deskId);

        // Untagged lookup rejected (found == 0).
        {
            uintptr_t s = dial();
            TEST_ASSERT(s != ~uintptr_t(0));
            TEST_ASSERT(sendTagged(s, PacketType::RELAY_LOOKUP, lookW.buffer(), ""));
            FrameHeader h{};
            std::vector<uint8_t> p;
            TEST_ASSERT(NetworkEngine::recvFrame(s, h, p));
            TEST_ASSERT(h.type == static_cast<uint8_t>(PacketType::RELAY_LOOKUP_RESP));
            TEST_ASSERT(p.size() >= 1 && p[0] == 0);
            closeSocketHandle(s);
        }

        // Authenticated lookup succeeds.
        {
            uintptr_t s = dial();
            TEST_ASSERT(s != ~uintptr_t(0));
            TEST_ASSERT(sendTagged(s, PacketType::RELAY_LOOKUP, lookW.buffer(), secret));
            FrameHeader h{};
            std::vector<uint8_t> p;
            TEST_ASSERT(NetworkEngine::recvFrame(s, h, p));
            TEST_ASSERT(h.type == static_cast<uint8_t>(PacketType::RELAY_LOOKUP_RESP));
            ByteReader r(p);
            TEST_ASSERT(r.readU8() == 1);
            (void)r.readString();
            TEST_ASSERT(r.readU16() == 50995);
            TEST_ASSERT(r.readString() == "AuthTestHost");
            closeSocketHandle(s);
        }

        // Untagged connect request rejected (ready == 0).
        {
            uintptr_t s = dial();
            TEST_ASSERT(s != ~uintptr_t(0));
            TEST_ASSERT(sendTagged(s, PacketType::RELAY_CONNECT_REQ, lookW.buffer(), ""));
            FrameHeader h{};
            std::vector<uint8_t> p;
            TEST_ASSERT(NetworkEngine::recvFrame(s, h, p));
            TEST_ASSERT(h.type == static_cast<uint8_t>(PacketType::RELAY_BRIDGE_READY));
            TEST_ASSERT(p.size() >= 1 && p[0] == 0);
            closeSocketHandle(s);
        }

        closeSocketHandle(hostSock);
        relay.stop();
    }
}

static void testFixesC_UpdaterAndSimd() {
    // #133: extractJsonString must return "" for non-string values instead of returning the next key name.
    {
        TEST_ASSERT(AutoUpdater::extractJsonString("{\"body\": null, \"reactions\": \"oops\"}", "body").empty());
        TEST_ASSERT(AutoUpdater::extractJsonString("{\"count\": 42, \"next\": \"nope\"}", "count").empty());
        TEST_ASSERT(AutoUpdater::extractJsonString("{\"flag\": true, \"after\": \"nope\"}", "flag").empty());
        TEST_ASSERT(AutoUpdater::extractJsonString("{\"obj\": {\"inner\": \"val\"}, \"after\": \"nope\"}", "obj").empty());
        TEST_ASSERT(AutoUpdater::extractJsonString(
            "{\"note\": \"mentions \\\"tag_name\\\": \\\"fake\\\"\", \"tag_name\": \"v4.0.0\"}",
            "tag_name") == "v4.0.0");
    }

    // #81: extractAssetDownloadUrl ignores release body text and nested uploader objects.
    {
        const std::string goodHex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        const std::string json =
            "{\"tag_name\":\"v4.1.0\","
            "\"body\":\"Download CppDesk.exe from https://evil.test/body\","
            "\"assets\":["
            "{\"name\":\"other.zip\",\"uploader\":{\"name\":\"CppDesk.exe\",\"browser_download_url\":\"https://evil.test/nested\"},"
            "\"size\":10,\"digest\":\"sha256:" + std::string(64, 'a') + "\",\"browser_download_url\":\"https://example.com/other.zip\"},"
            "{\"name\":\"CppDesk.exe\",\"browser_download_url\":\"https://example.com/real/CppDesk.exe\","
            "\"uploader\":{\"login\":\"bot\",\"nested\":{\"a\":1}},\"size\":12345,\"digest\":\"sha256:" + goodHex + "\"}"
            "]}";
        uint64_t sz = 0;
        std::string sha;
        std::string url = AutoUpdater::extractAssetDownloadUrl(json, "CppDesk.exe", &sz, &sha);
        TEST_ASSERT(url == "https://example.com/real/CppDesk.exe");
        TEST_ASSERT(sz == 12345);
        TEST_ASSERT(sha == goodHex);
    }

    // #132: CallbackGate blocks execution after invalidate().
    {
        auto gate = std::make_shared<AutoUpdater::CallbackGate>();
        int calls = 0;
        TEST_ASSERT(gate->runIfAlive([&]() { ++calls; }));
        TEST_ASSERT(calls == 1);
        gate->invalidate();
        TEST_ASSERT(!gate->runIfAlive([&]() { ++calls; }));
        TEST_ASSERT(calls == 1);
    }

#if defined(_WIN32)
    // #81: downloadUpdateAssetAsync refuses plaintext HTTP and missing/malformed SHA-256 digests.
    {
        const std::string validHex(64, 'a');
        auto runSyncFail = [](const std::string& url, const std::string& sha) -> std::pair<bool, std::string> {
            std::mutex m;
            std::condition_variable cv;
            bool finished = false;
            bool ok = true;
            std::string err;
            AutoUpdater::downloadUpdateAssetAsync(
                url, L"C:\\nonexistent_cppdesk_test.exe", nullptr,
                [&](bool s, const std::string& e) {
                    std::lock_guard<std::mutex> lk(m);
                    ok = s;
                    err = e;
                    finished = true;
                    cv.notify_one();
                },
                sha);
            std::unique_lock<std::mutex> lk(m);
            cv.wait_for(lk, std::chrono::seconds(3), [&]() { return finished; });
            return { ok, err };
        };

        auto rHttp = runSyncFail("http://example.com/CppDesk.exe", validHex);
        TEST_ASSERT(!rHttp.first);
        TEST_ASSERT(rHttp.second.find("HTTPS") != std::string::npos);

        auto rEmptySha = runSyncFail("https://example.com/CppDesk.exe", "");
        TEST_ASSERT(!rEmptySha.first);
        TEST_ASSERT(rEmptySha.second.find("SHA-256") != std::string::npos);

        auto rBadSha = runSyncFail("https://example.com/CppDesk.exe", "not-a-valid-sha256");
        TEST_ASSERT(!rBadSha.first);
        TEST_ASSERT(rBadSha.second.find("SHA-256") != std::string::npos);
    }

    // #81: createStagingExePath returns unique owner-directory paths, not predictable %TEMP%\CppDesk_update.exe.
    {
        std::wstring p1 = AutoUpdater::createStagingExePath();
        std::wstring p2 = AutoUpdater::createStagingExePath();
        TEST_ASSERT(!p1.empty() && !p2.empty());
        TEST_ASSERT(p1 != p2);
        TEST_ASSERT(p1.size() > 4 && p1.substr(p1.size() - 4) == L".exe");
        TEST_ASSERT(p1.find(L"\\CppDesk_update.exe") == std::wstring::npos);
    }

    // #205: applySelfUpdateAndRestart returns non-zero when launchAfter=true and target cannot be launched.
    {
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring src = std::wstring(tempPath) + L"cppdesk_fix205_src.bin";
        std::wstring dst = std::wstring(tempPath) + L"cppdesk_fix205_dst.bin";
        {
            std::ofstream out(src.c_str(), std::ios::binary | std::ios::trunc);
            out << "NOT_A_PE_BINARY";
        }
        int rc = AutoUpdater::applySelfUpdateAndRestart(src, dst, 0, true);
        TEST_ASSERT(rc != 0);
        DeleteFileW(src.c_str());
        DeleteFileW(dst.c_str());
    }

    // #249: buildSelfUpdateCommand rejects shell metacharacters.
    {
        TEST_ASSERT(AutoUpdater::buildSelfUpdateCommand(L"C:\\Temp\\a&calc.exe", L"C:\\Temp\\b.exe").empty());
        TEST_ASSERT(AutoUpdater::buildSelfUpdateCommand(L"C:\\Temp\\a.exe", L"C:\\Temp\\b|calc.exe").empty());
        TEST_ASSERT(AutoUpdater::buildSelfUpdateCommand(L"C:\\Temp\\%TEMP%\\a.exe", L"C:\\Temp\\b.exe").empty());
        TEST_ASSERT(AutoUpdater::buildSelfUpdateCommand(L"C:\\Temp\\a\"b.exe", L"C:\\Temp\\b.exe").empty());
        TEST_ASSERT(AutoUpdater::buildSelfUpdateCommand(L"", L"C:\\Temp\\b.exe").empty());
    }
#endif

    // #167: blitRowBgraOpaque produces identical output to scalarBlitRowBgraOpaque across widths without global -mavx2.
    {
        for (int width : { 1, 7, 8, 9, 16, 23, 32 }) {
            std::vector<uint8_t> src(static_cast<size_t>(width) * 4);
            for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>((i * 37) & 0xFF);
            std::vector<uint8_t> dstSimd(src.size(), 0);
            std::vector<uint8_t> dstScalar(src.size(), 0);
            SimdKernels::blitRowBgraOpaque(dstSimd.data(), src.data(), width);
            SimdKernels::scalarBlitRowBgraOpaque(dstScalar.data(), src.data(), width);
            TEST_ASSERT(dstSimd == dstScalar);
        }
    }
}

void testFixesC() {
    std::cout << "[FIX C] Testing relay auth/ownership, updater hardening, and SIMD runtime dispatch...\n";
    testFixesC_RelayAuthAndOwnership();
    testFixesC_UpdaterAndSimd();
}
