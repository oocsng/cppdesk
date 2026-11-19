// Regression tests for full-review fix worker B. Only worker B edits this file.

namespace {

bool sendRawFrameForTestB(socket_t s, PacketType type, const std::vector<uint8_t>& payload) {
    FrameHeader hdr{};
    hdr.magic = PROTOCOL_MAGIC;
    hdr.type = static_cast<uint8_t>(type);
    hdr.flags = 0;
    hdr.payloadSize = static_cast<uint32_t>(payload.size());
    const uint8_t* hp = reinterpret_cast<const uint8_t*>(&hdr);
    size_t sent = 0;
    while (sent < sizeof(hdr)) {
        int n = ::send(s, reinterpret_cast<const char*>(hp + sent), static_cast<int>(sizeof(hdr) - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    sent = 0;
    while (sent < payload.size()) {
        int n = ::send(s, reinterpret_cast<const char*>(payload.data() + sent), static_cast<int>(payload.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

void testFixesB() {
    // #13: CommandPaletteManager bounded worker pool & clean shutdown
    {
        CommandPaletteManager mgr;
        std::atomic<int> completed{0};
        std::string capturedOut;
        std::mutex outMu;
#if defined(_WIN32)
        const std::string cmd = "cmd.exe /c echo fix_b_ok";
#else
        const std::string cmd = "echo fix_b_ok";
#endif
        mgr.executeAsync(cmd, [&](int32_t code, const std::string& out) {
            std::lock_guard<std::mutex> lk(outMu);
            if (code == 0) capturedOut = out;
            completed.fetch_add(1);
        });
        for (int i = 0; i < 200 && completed.load() == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        mgr.shutdown();
        TEST_ASSERT(completed.load() == 1);
        TEST_ASSERT(capturedOut.find("fix_b_ok") != std::string::npos);
    }

    // #14: RelayServer rejects oversized post-registration payloadSize (0xFFFFFFFF) without OOM
    {
        RelayServer relay;
        TEST_ASSERT(relay.start(0));
        uint16_t rport = relay.port();
        TEST_ASSERT(rport != 0);

        socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        TEST_ASSERT(s != INVALID_SOCKET_VAL);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(rport);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        TEST_ASSERT(::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        // Register a host peer on the relay (deskId, hostname, reportedIp, tcpPort)
        ByteWriter reg;
        reg.writeU64(900111222ULL);
        reg.writeString("test-host-b");
        reg.writeString("127.0.0.1");
        reg.writeU16(49999);
        TEST_ASSERT(sendRawFrameForTestB(s, PacketType::RELAY_REGISTER, reg.buffer()));

        // Read RELAY_REGISTER_ACK header + payload
        FrameHeader ackHdr{};
        int got = ::recv(s, reinterpret_cast<char*>(&ackHdr), sizeof(ackHdr), MSG_WAITALL);
        TEST_ASSERT(got == static_cast<int>(sizeof(ackHdr)));
        TEST_ASSERT(ackHdr.type == static_cast<uint8_t>(PacketType::RELAY_REGISTER_ACK));
        if (ackHdr.payloadSize > 0 && ackHdr.payloadSize < 1024) {
            std::vector<uint8_t> ackPay(ackHdr.payloadSize);
            (void)::recv(s, reinterpret_cast<char*>(ackPay.data()), static_cast<int>(ackPay.size()), MSG_WAITALL);
        }

        // Send a malicious 4 GB frame header on the keepalive loop; relay must drop the socket immediately
        FrameHeader badHdr{};
        badHdr.magic = PROTOCOL_MAGIC;
        badHdr.type = static_cast<uint8_t>(PacketType::PING);
        badHdr.flags = 0;
        badHdr.payloadSize = 0xFFFFFFFFu;
        int sent = ::send(s, reinterpret_cast<const char*>(&badHdr), sizeof(badHdr), 0);
        TEST_ASSERT(sent == static_cast<int>(sizeof(badHdr)));

        char probe = 0;
        int r = ::recv(s, &probe, 1, 0);
        TEST_ASSERT(r <= 0); // Peer closed connection instead of allocating 4 GB

        socket_close(s);
        relay.stop();
        TEST_ASSERT(!relay.isRunning());
    }

    // #67: FileTransferManager reserved names, incomplete FILE_COMPLETE, duplicate active transferId
    {
        auto tempDir = std::filesystem::temp_directory_path() / "cppdesk_fix_b_ft_test";
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
        std::filesystem::create_directories(tempDir, ec);

        FileTransferManager ftm;
        ftm.setReceiveDirectory(tempDir.string());

        // 1. Reserved Windows device names are prefixed with '_'
        ftm.handleFileOffer(101, 4, "CON");
        ftm.handleFileOffer(102, 4, "NUL.txt");
        ftm.handleFileOffer(103, 4, "com1.exe");
        auto items = ftm.snapshotTransfers();
        bool sawCon = false, sawNul = false, sawCom1 = false;
        for (const auto& it : items) {
            if (it.transferId == 101 && it.fileName == "_CON") sawCon = true;
            if (it.transferId == 102 && it.fileName == "_NUL.txt") sawNul = true;
            if (it.transferId == 103 && it.fileName == "_com1.exe") sawCom1 = true;
        }
        TEST_ASSERT(sawCon && sawNul && sawCom1);
        ftm.abortActiveTransfers();

        // 2. Incomplete FILE_COMPLETE (receivedBytes < totalBytes) must fail even with matching partial SHA-256
        const uint8_t partial[4] = {'t', 'e', 's', 't'};
        auto partialHash = CryptoUtils::sha256(partial, sizeof(partial));
        std::string partialHex = CryptoUtils::toHex(partialHash.data(), partialHash.size());
        ftm.handleFileOffer(201, 100, "incomplete.bin");
        ftm.handleFileChunk(201, 0, partial, sizeof(partial));
        ftm.handleFileComplete(201, partialHex);
        items = ftm.snapshotTransfers();
        bool incompleteFailed = false;
        for (const auto& it : items) {
            if (it.transferId == 201 && it.status == TransferStatus::Failed) {
                incompleteFailed = true;
            }
        }
        TEST_ASSERT(incompleteFailed);
        TEST_ASSERT(!std::filesystem::exists(tempDir / "incomplete.bin", ec));

        // 3. Duplicate active transferId is ignored without corrupting the active transfer
        ftm.handleFileOffer(301, 4, "valid.bin");
        ftm.handleFileOffer(301, 9999, "overwrite_attempt.bin");
        ftm.handleFileChunk(301, 0, partial, sizeof(partial));
        ftm.handleFileComplete(301, partialHex);
        items = ftm.snapshotTransfers();
        bool validCompleted = false;
        for (const auto& it : items) {
            if (it.transferId == 301 && it.fileName == "valid.bin" && it.status == TransferStatus::Completed) {
                validCompleted = true;
            }
        }
        TEST_ASSERT(validCompleted);
        TEST_ASSERT(std::filesystem::exists(tempDir / "valid.bin", ec));

        std::filesystem::remove_all(tempDir, ec);
    }

    // #129: startNetworkDiagnostics() on non-running NetworkEngine joins cleanly in destructor
    {
        IdentityManager id(91);
        id.loadOrCreate();
        {
            NetworkEngine net(id);
            net.startNetworkDiagnostics("", "");
            // Destroy without ever calling start(); must not std::terminate()
        }
        TEST_ASSERT(true);
    }

    // #171: TileCodec rejects non-JPEG SOI magic and oversized tile dimensions
    {
        const uint8_t fakePng[16] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0};
        std::vector<uint8_t> outBgra;
        int w = 0, h = 0;
        TEST_ASSERT(!TileCodec::decodeJpeg(fakePng, sizeof(fakePng), outBgra, w, h));

        const uint8_t fakeBmp[16] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        TEST_ASSERT(!TileCodec::decodeJpeg(fakeBmp, sizeof(fakeBmp), outBgra, w, h));

        // Oversized tile (> 1M pixels) rejected before allocating scratch
        std::vector<uint8_t> smallCanvas(64 * 64 * 4, 0);
        const uint8_t dummy[8] = {0};
        TEST_ASSERT(!TileCodec::decodeTileIntoCanvas(
            0, 0, 2048, 2048, TileEncoding::Zstd, dummy, sizeof(dummy),
            smallCanvas.data(), 2048, 2048));
    }

    // #204: Concurrent configurePrivacyCurtain calls are synchronized
    {
        IdentityManager id(92);
        id.loadOrCreate();
        NetworkEngine net(id);
        std::thread t1([&]() {
            for (int i = 0; i < 100; ++i) {
                net.configurePrivacyCurtain("Notice " + std::to_string(i), "BrandA", (i & 1) != 0);
            }
        });
        std::thread t2([&]() {
            for (int i = 0; i < 100; ++i) {
                net.configurePrivacyCurtain("Other " + std::to_string(i), "BrandB", (i & 1) == 0);
            }
        });
        t1.join();
        t2.join();
        TEST_ASSERT(true);
    }
}
