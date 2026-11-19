// Regression tests for full-review fix worker D. Only worker D edits this file.
#include <limits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>

void testFixesD() {
    std::cout << "[TEST FIXES D] Control: file sync, clipboard, input, file browser, palette...\n";
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path baseTmp = fs::temp_directory_path(ec) / "cppdesk_fix_tests_d";
    fs::remove_all(baseTmp, ec);
    fs::create_directories(baseTmp, ec);

    // #5 & #220: RemoteFileBrowserManager root traversal, drive-relative paths, timestamp errors
    {
        using RFB = RemoteFileBrowserManager;
        std::string err;
#if defined(_WIN32)
        TEST_ASSERT(!RFB::deleteLocalItem("C:\\Users\\..", err));
        TEST_ASSERT(!RFB::deleteLocalItem("C:\\.", err));
        TEST_ASSERT(!RFB::deleteLocalItem("C:\\Windows\\..\\..", err));
        TEST_ASSERT(!RFB::renameLocalItem("C:\\Users\\..", "x", err));
#else
        TEST_ASSERT(!RFB::deleteLocalItem("/tmp/..", err));
        TEST_ASSERT(!RFB::deleteLocalItem("/.", err));
        TEST_ASSERT(!RFB::renameLocalItem("/tmp/..", "x", err));
#endif
        // Out-of-range timestamps must return "-" instead of formatting zeroed tm as 1900-01-00
        TEST_ASSERT(RFB::formatFileTimestamp(0) == "-");
        TEST_ASSERT(RFB::formatFileTimestamp(std::numeric_limits<uint64_t>::max()) == "-");
        TEST_ASSERT(RFB::formatFileTimestamp(1ULL << 60) == "-");
#if defined(_WIN32)
        // Drive-relative "C:" must list C:\, not process CWD
        std::vector<FileBrowserItemEntry> entriesC, entriesCRoot;
        std::string canonC, canonCRoot, errC, errCRoot;
        bool okC = RFB::listLocalDirectory("C:", entriesC, canonC, errC);
        bool okCRoot = RFB::listLocalDirectory("C:\\", entriesCRoot, canonCRoot, errCRoot);
        TEST_ASSERT(okC && okCRoot);
        TEST_ASSERT(canonC == "C:\\");
        TEST_ASSERT(entriesC.size() == entriesCRoot.size());
#endif
    }

    // #6, #8, #44, #46, #64, #73, #139, #140, #141, #142, #183, #219: FileSyncManager
    {
        const fs::path syncLocal = baseTmp / "sync_local";
        const fs::path syncRemote = baseTmp / "sync_remote";
        fs::create_directories(syncLocal, ec);
        fs::create_directories(syncRemote, ec);

        // #73: normalizePath(".") rejected and DeleteDir "." must not delete sync root
        TEST_ASSERT(FileSyncManager::normalizePath(".").empty());
        TEST_ASSERT(FileSyncManager::normalizePath("./").empty());
        TEST_ASSERT(FileSyncManager::normalizePath("a/./b").empty());
        {
            FileSyncManager host;
            host.setLocalRoot(syncRemote.string());
            SyncActionReqPayload req{};
            req.actionId = 1;
            req.actionType = static_cast<uint8_t>(SyncActionType::DeleteDir);
            req.targetPath = ".";
            SyncActionRespPayload resp{};
            TEST_ASSERT(!host.handleActionReq(req, resp));
            TEST_ASSERT(fs::exists(syncRemote));
        }

        // #142: AbortSync succeeds without any target root configured
        {
            FileSyncManager unconfigured;
            SyncActionReqPayload abortReq{};
            abortReq.actionId = 2;
            abortReq.actionType = static_cast<uint8_t>(SyncActionType::AbortSync);
            SyncActionRespPayload abortResp{};
            TEST_ASSERT(unconfigured.handleActionReq(abortReq, abortResp));
            TEST_ASSERT(unconfigured.syncState() == SyncState::Cancelled);
        }

        // #64: handleScanResp drops traversal / absolute paths
        {
            FileSyncManager viewer;
            SyncScanRespPayload scanResp{};
            scanResp.statusCode = 0;
            scanResp.entries.push_back({ "../../escape.txt", 10, 100, 0 });
            scanResp.entries.push_back({ "C:/Windows/Temp/pwn", 0, 100, 1 });
            scanResp.entries.push_back({ ".", 0, 100, 1 });
            scanResp.entries.push_back({ "safe/ok.txt", 5, 100, 0 });
            viewer.handleScanResp(scanResp);
            auto entries = viewer.remoteEntries(); // #139 copy by value
            TEST_ASSERT(entries.size() == 1);
            TEST_ASSERT(entries[0].relativePath == "safe/ok.txt");
        }

        // #6, #8, #140, #183: pumpOutgoingSync awaits hash resp without spinning, handles 0-byte files,
        // synchronizes activeSyncId_ across peers, does not deadlock on re-entrant telemetry(), and updates transferredBytes_
        {
            // Create a modified file and a zero-byte file in local; older larger version in remote
            std::ofstream(syncLocal / "mod.bin", std::ios::binary) << std::string(1000, 'X');
            std::ofstream(syncRemote / "mod.bin", std::ios::binary) << std::string(2000, 'Y');
            std::ofstream(syncLocal / "empty.txt", std::ios::binary); // 0 bytes
            std::ofstream(syncRemote / "empty.txt", std::ios::binary) << "old_nonempty";

            FileSyncManager viewer;
            FileSyncManager host;
            viewer.setLocalRoot(syncLocal.string());
            host.setLocalRoot(syncRemote.string());
            viewer.setSyncMode(SyncMode::PushMirror);

            TEST_ASSERT(viewer.startLocalScan());
            SyncScanReqPayload sreq{};
            SyncScanRespPayload sresp{};
            TEST_ASSERT(host.handleScanReqHost(sreq, sresp));
            viewer.handleScanResp(sresp);
            viewer.generateDiffPlan();
            TEST_ASSERT(viewer.startSync());

            // Pump #1: sends SYNC_HASH_REQ for first file
            std::vector<SyncHashReqPayload> hashReqs;
            std::vector<SyncDeltaBlockPayload> deltaBlocks;
            bool reentrantOk = false;
            auto sendFn = [&](PacketType type, const std::vector<uint8_t>& payload) {
                // #140: Calling telemetry() inside sendPacket must not deadlock mutex_
                (void)viewer.telemetry();
                reentrantOk = true;
                if (type == PacketType::SYNC_HASH_REQ) {
                    SyncHashReqPayload r;
                    if (deserializeSyncHashReq(payload.data(), payload.size(), r)) hashReqs.push_back(std::move(r));
                } else if (type == PacketType::SYNC_DELTA_BLOCK) {
                    SyncDeltaBlockPayload b;
                    if (deserializeSyncDeltaBlock(payload.data(), payload.size(), b)) deltaBlocks.push_back(std::move(b));
                }
                return true;
            };

            viewer.pumpOutgoingSync(sendFn, 4);
            TEST_ASSERT(reentrantOk);
            TEST_ASSERT(hashReqs.size() == 1);

            // #6: Second pump BEFORE hash response arrives must NOT spin/deadlock!
            viewer.pumpOutgoingSync(sendFn, 4);
            TEST_ASSERT(hashReqs.size() == 1);

            // Deliver hash requests and delta blocks end-to-end across both files (including 0-byte shrink!)
            for (int iter = 0; iter < 16 && viewer.syncState() == SyncState::Syncing; ++iter) {
                while (!hashReqs.empty()) {
                    auto req = std::move(hashReqs.front());
                    hashReqs.erase(hashReqs.begin());
                    SyncHashRespPayload hresp{};
                    TEST_ASSERT(host.handleHashReqHost(req, hresp));
                    TEST_ASSERT(viewer.handleHashResp(hresp));
                }
                viewer.pumpOutgoingSync(sendFn, 4);
                while (!deltaBlocks.empty()) {
                    auto blk = std::move(deltaBlocks.front());
                    deltaBlocks.erase(deltaBlocks.begin());
                    // #8: Host adopted viewer's syncId in handleHashReqHost even with distinct startup times
                    TEST_ASSERT(host.handleDeltaBlock(blk));
                }
            }
            TEST_ASSERT(viewer.syncState() == SyncState::Completed);
            // #183: transferredBytes_ must be non-zero (1000 bytes for mod.bin)
            TEST_ASSERT(viewer.telemetry().transferredBytes == 1000);
            // #46: Shrinking files (mod.bin 2000->1000, empty.txt 12->0) must be truncated cleanly
            TEST_ASSERT(fs::file_size(syncRemote / "mod.bin") == 1000);
            TEST_ASSERT(fs::file_size(syncRemote / "empty.txt") == 0);
        }

        // #46: Out-of-order blocks, huge blockOffset rejection, unregistered fileId rejection
        {
            const fs::path oooDir = baseTmp / "sync_ooo";
            fs::create_directories(oooDir, ec);
            FileSyncManager dst;
            dst.setLocalRoot(oooDir.string());
            dst.setSyncState(SyncState::Syncing);
            dst.registerIncomingFile(42, "ooo.bin", 2ULL * FileSyncManager::SYNC_BLOCK_SIZE, 1700000000);

            std::vector<uint8_t> b0(FileSyncManager::SYNC_BLOCK_SIZE, 0x11);
            std::vector<uint8_t> b1(FileSyncManager::SYNC_BLOCK_SIZE, 0x22);

            SyncDeltaBlockPayload p1{};
            p1.syncId = 777;
            p1.fileId = 42;
            p1.blockIndex = 1;
            p1.blockOffset = FileSyncManager::SYNC_BLOCK_SIZE;
            p1.totalFileSize = 2ULL * FileSyncManager::SYNC_BLOCK_SIZE;
            p1.totalBlocks = 2;
            p1.blockData = b1;
            p1.blockHash = CryptoUtils::sha256(b1.data(), b1.size());

            // Deliver block 1 BEFORE block 0: must NOT finalize prematurely
            TEST_ASSERT(dst.handleDeltaBlock(p1));
            TEST_ASSERT(!fs::exists(oooDir / "ooo.bin"));
            TEST_ASSERT(fs::exists(oooDir / "ooo.bin.part"));

            // Reject mismatched syncId
            SyncDeltaBlockPayload badSync = p1;
            badSync.syncId = 888;
            TEST_ASSERT(!dst.handleDeltaBlock(badSync));

            // Reject unvalidated huge blockOffset
            SyncDeltaBlockPayload badOff = p1;
            badOff.blockOffset = 0x7FFFFFFF0000ULL;
            TEST_ASSERT(!dst.handleDeltaBlock(badOff));

            // Reject unregistered fileId (even with matching size)
            SyncDeltaBlockPayload unreg = p1;
            unreg.fileId = 9999;
            TEST_ASSERT(!dst.handleDeltaBlock(unreg));

            // Now deliver block 0: must finalize cleanly
            SyncDeltaBlockPayload p0{};
            p0.syncId = 777;
            p0.fileId = 42;
            p0.blockIndex = 0;
            p0.blockOffset = 0;
            p0.totalFileSize = 2ULL * FileSyncManager::SYNC_BLOCK_SIZE;
            p0.totalBlocks = 2;
            p0.blockData = b0;
            p0.blockHash = CryptoUtils::sha256(b0.data(), b0.size());
            TEST_ASSERT(dst.handleDeltaBlock(p0));
            TEST_ASSERT(fs::exists(oooDir / "ooo.bin"));
            TEST_ASSERT(!fs::exists(oooDir / "ooo.bin.part"));
            TEST_ASSERT(fs::file_size(oooDir / "ooo.bin") == 2ULL * FileSyncManager::SYNC_BLOCK_SIZE);
        }

        // #141: Non-ASCII UTF-8 path in FileSyncManager::handleDeltaBlock on Windows
        {
            const fs::path utf8Dir = baseTmp / "sync_utf8";
            fs::create_directories(utf8Dir, ec);
            FileSyncManager dst;
            dst.setLocalRoot(utf8Dir.string());
            dst.setSyncState(SyncState::Syncing);
            const std::string utf8Name = "caf\xC3\xA9.txt";
            dst.registerIncomingFile(55, utf8Name, 5, 1700000000);

            SyncDeltaBlockPayload p{};
            p.syncId = 900;
            p.fileId = 55;
            p.blockIndex = 0;
            p.blockOffset = 0;
            p.totalFileSize = 5;
            p.totalBlocks = 1;
            p.blockData = { 'h', 'e', 'l', 'l', 'o' };
            p.blockHash = CryptoUtils::sha256(p.blockData.data(), p.blockData.size());
            TEST_ASSERT(dst.handleDeltaBlock(p));
#if defined(_WIN32)
            TEST_ASSERT(fs::exists(utf8Dir / L"caf\u00e9.txt"));
#else
            TEST_ASSERT(fs::exists(utf8Dir / utf8Name));
#endif
        }

        // #219: reset() deletes orphaned .part files; handleStatusUpdate rejects invalid enum & wrong syncId;
        // generateDiffPlan handles UINT64_MAX mtimeSec without signed overflow
        {
            const fs::path rstDir = baseTmp / "sync_reset";
            fs::create_directories(rstDir, ec);
            FileSyncManager dst;
            dst.setLocalRoot(rstDir.string());
            dst.setSyncState(SyncState::Syncing);
            dst.registerIncomingFile(7, "half.bin", 2ULL * FileSyncManager::SYNC_BLOCK_SIZE, 0);

            SyncDeltaBlockPayload p0{};
            p0.syncId = 321;
            p0.fileId = 7;
            p0.blockIndex = 0;
            p0.blockOffset = 0;
            p0.totalFileSize = 2ULL * FileSyncManager::SYNC_BLOCK_SIZE;
            p0.totalBlocks = 2;
            p0.blockData.assign(FileSyncManager::SYNC_BLOCK_SIZE, 0x33);
            p0.blockHash = CryptoUtils::sha256(p0.blockData.data(), p0.blockData.size());
            TEST_ASSERT(dst.handleDeltaBlock(p0));
            TEST_ASSERT(fs::exists(rstDir / "half.bin.part"));

            SyncStatusUpdatePayload badState{};
            badState.syncId = 321;
            badState.syncState = 255;
            TEST_ASSERT(!dst.handleStatusUpdate(badState));

            SyncStatusUpdatePayload wrongId{};
            wrongId.syncId = 999;
            wrongId.syncState = static_cast<uint8_t>(SyncState::Completed);
            TEST_ASSERT(!dst.handleStatusUpdate(wrongId));

            dst.reset();
            TEST_ASSERT(!fs::exists(rstDir / "half.bin.part"));

            // Unsigned timeMatch boundary test
            std::vector<SyncFileEntry> l = { { "t.txt", 10, std::numeric_limits<uint64_t>::max(), 0 } };
            std::vector<SyncFileEntry> r = { { "t.txt", 10, 0, 0 } };
            auto diff = FileSyncManager::generateDiffPlan(l, r, SyncMode::PushMirror, false);
            TEST_ASSERT(diff.size() == 1 && diff[0].status == SyncDiffStatus::ModifiedLocal);
        }
    }

    // #47 & #143: FileTransferManager byte-count verification, rename error, collision reservation,
    // duplicate transferId, items_ cap, sendPacket failure, premature EOF
    {
        const fs::path ftDir = baseTmp / "ft_recv";
        fs::create_directories(ftDir, ec);
        FileTransferManager mgr;
        mgr.setReceiveDirectory(ftDir.string());

        // Incomplete transfer must fail even with empty SHA-256
        mgr.handleFileOffer(10, 1024, "trunc.bin");
        const std::string emptySha = CryptoUtils::sha256Hex("");
        mgr.handleFileComplete(10, emptySha);
        auto snap = mgr.snapshotTransfers();
        TEST_ASSERT(!snap.empty() && snap[0].transferId == 10 && snap[0].status == TransferStatus::Failed);
        TEST_ASSERT(!fs::exists(ftDir / "trunc.bin"));

        // Duplicate active transferId must be ignored; concurrent same-name offers must reserve distinct paths
        mgr.handleFileOffer(20, 4, "dup.txt");
        mgr.handleFileOffer(20, 4, "dup.txt"); // duplicate ID -> ignored
        mgr.handleFileOffer(21, 4, "dup.txt"); // distinct ID, same name -> reserved as dup (1).txt
        const uint8_t d1[4] = { 'A', 'A', 'A', 'A' };
        const uint8_t d2[4] = { 'B', 'B', 'B', 'B' };
        mgr.handleFileChunk(20, 0, d1, 4);
        mgr.handleFileChunk(21, 0, d2, 4);
        mgr.handleFileComplete(20, CryptoUtils::sha256Hex("AAAA"));
        mgr.handleFileComplete(21, CryptoUtils::sha256Hex("BBBB"));
        TEST_ASSERT(fs::exists(ftDir / "dup.txt"));
        TEST_ASSERT(fs::exists(ftDir / "dup (1).txt"));

        // Spamming oversized offers must respect the 150-item cap
        for (uint32_t i = 1000; i < 1200; ++i) {
            mgr.handleFileOffer(i, 5ULL * 1024 * 1024 * 1024, "huge.bin");
        }
        TEST_ASSERT(mgr.snapshotTransfers().size() <= 150);

        // #143: startOutgoingFile fails cleanly when sendPacket returns false
        const fs::path outSrc = baseTmp / "ft_src.txt";
        std::ofstream(outSrc, std::ios::binary) << "hello world";
        uint32_t badTid = mgr.startOutgoingFile(outSrc.string(), [](PacketType, const std::vector<uint8_t>&) {
            return false;
        });
        TEST_ASSERT(badTid == 0);
        TEST_ASSERT(!mgr.hasActiveTransfers());

        // #143: pumpOutgoingChunks marks Failed when sendPacket fails on FILE_CHUNK
        uint32_t okTid = mgr.startOutgoingFile(outSrc.string(), [](PacketType, const std::vector<uint8_t>&) {
            return true;
        });
        TEST_ASSERT(okTid > 0);
        mgr.pumpOutgoingChunks([](PacketType, const std::vector<uint8_t>&) { return false; }, 4);
        auto afterFail = mgr.snapshotTransfers();
        bool sawFailed = false;
        for (const auto& it : afterFail) {
            if (it.transferId == okTid && it.status == TransferStatus::Failed) sawFailed = true;
        }
        TEST_ASSERT(sawFailed);
    }

    // #43, #74, #144, #149, #218, #226, #244: Clipboard managers & ShellClipboard
    {
        // #144: non-ASCII UTF-8 search in ClipboardHistoryManager
        ClipboardHistoryManager hist;
        hist.addItem("Hello \xF0\x9F\x9A\x80 caf\xC3\xA9 WORLD", false);
        auto found = hist.search("world");
        TEST_ASSERT(found.size() == 1);
        auto foundUtf8 = hist.search("caf\xC3\xA9");
        TEST_ASSERT(foundUtf8.size() == 1);

        // #74 & #244: Unsolicited huge offset chunk is ignored and creates no .part files;
        // #218: UTF-16 -> UTF-8 display name preserved
        ClipboardFileTransferManager cfm;
        std::vector<VirtualFileEntry> vfiles;
        vfiles.push_back({ 0, L"caf\u00e9.txt", 16, 0x80 });
        cfm.handleRemoteFileList(505, vfiles, [](PacketType, const std::vector<uint8_t>&) { return true; });
        TEST_ASSERT(cfm.activeFileName() == "caf\xC3\xA9.txt");

        uint8_t oneByte = 0x42;
        auto oneHash = CryptoUtils::sha256(&oneByte, 1);
        cfm.handleFileChunk(505, 0, 0x7FFFFFFFFFFFF000ULL, &oneByte, 1, oneHash.data());
        // Must not create any .part file in stagingDir_
        bool anyPart = false;
        for (const auto& e : fs::directory_iterator(fs::path(cfm.stagingDirectory()), ec)) {
            if (e.path().extension() == ".part") anyPart = true;
        }
        TEST_ASSERT(!anyPart);

        // #149: resolveConflictFilename does not throw
        std::string safeName = ShellClipboard::resolveConflictFilename((baseTmp / "does_not_exist").string(), "x.txt");
        TEST_ASSERT(safeName == "x.txt");

#if defined(_WIN32)
        // #226: ShellEnumFormatEtc::Skip & Clone, VirtualFileStream::Seek overflow & Read early EOF
        FORMATETC fmts[3] = {};
        fmts[0].cfFormat = 1; fmts[1].cfFormat = 2; fmts[2].cfFormat = 3;
        ShellEnumFormatEtc* en = new ShellEnumFormatEtc({ fmts[0], fmts[1], fmts[2] });
        TEST_ASSERT(en->Skip(1) == S_OK);
        IEnumFORMATETC* cloned = nullptr;
        TEST_ASSERT(en->Clone(&cloned) == S_OK && cloned != nullptr);
        FORMATETC got{};
        ULONG fetched = 0;
        TEST_ASSERT(cloned->Next(1, &got, &fetched) == S_OK && fetched == 1 && got.cfFormat == 2);
        TEST_ASSERT(cloned->Skip(10) == S_FALSE);
        cloned->Release();
        en->Release();

        VirtualFileEntry desc{ 0, L"a.bin", 100, FILE_ATTRIBUTE_NORMAL };
        VirtualFileStream* stm = new VirtualFileStream(desc, [](uint32_t, uint64_t, uint32_t, std::vector<uint8_t>& out) {
            out.clear(); // unexpected empty chunk before EOF
            return true;
        });
        LARGE_INTEGER mv{};
        mv.QuadPart = std::numeric_limits<int64_t>::min();
        TEST_ASSERT(stm->Seek(mv, STREAM_SEEK_CUR, nullptr) == STG_E_INVALIDFUNCTION);
        mv.QuadPart = std::numeric_limits<int64_t>::max();
        TEST_ASSERT(stm->Seek(mv, STREAM_SEEK_END, nullptr) == S_OK);
        // 2 * INT64_MAX + 100 > UINT64_MAX -> must fail with STG_E_INVALIDFUNCTION
        TEST_ASSERT(stm->Seek(mv, STREAM_SEEK_CUR, nullptr) == STG_E_INVALIDFUNCTION);
        mv.QuadPart = 0;
        TEST_ASSERT(stm->Seek(mv, STREAM_SEEK_SET, nullptr) == S_OK);
        uint8_t buf[16] = {};
        ULONG nRead = 0;
        // #218: unexpected empty chunk before EOF must return E_FAIL, not S_OK
        TEST_ASSERT(stm->Read(buf, sizeof(buf), &nRead) == E_FAIL);
        stm->Release();
#endif
    }

    // #76 & #77: InputInjector Mac keycodes (OEM punctuation + numpad) and NaN coordinate guards
    {
        const uint16_t oemAndNum[] = {
            0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE,
            0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
            0x6A, 0x6B, 0x6D, 0x6E, 0x6F
        };
        for (uint16_t vk : oemAndNum) {
            uint16_t mac = InputInjector::mapVkToMacKeyCode(vk);
            TEST_ASSERT(mac != 0xFFFF);
            TEST_ASSERT(InputInjector::mapMacKeyCodeToVk(mac) == vk);
        }

        const float nanVal = std::numeric_limits<float>::quiet_NaN();
        PointXY pt = macPointForNormalized(nanVal, nanVal, PointRect{ 10.0, 20.0, 100.0, 200.0 });
        TEST_ASSERT(std::isfinite(pt.x) && std::isfinite(pt.y));
        TEST_ASSERT(pt.x == 10.0 && pt.y == 20.0);

        MonitorDesc mon{};
        mon.x = 0; mon.y = 0; mon.width = 1920; mon.height = 1080;
        InputInjector::injectMouseMove(nanVal, nanVal, mon); // must not crash/UB
    }

    // #78, #139, #146: CommandPaletteManager catalog & timeout
    {
        CommandPaletteManager pal;
        auto cat = pal.getCatalog();
        TEST_ASSERT(cat.size() >= 10);
#if !defined(_WIN32)
        for (const auto& cmd : cat) {
            TEST_ASSERT(cmd.commandLine.find("cmd.exe") == std::string::npos);
        }
#endif
    }

    // #49 & #222: WhiteboardManager cross-thread overlay lifecycle and laser pointer
    {
        WhiteboardManager wb;
        std::thread t([&wb]() {
            wb.showHostOverlay();
            AnnotationStroke laser{};
            laser.tool = WhiteboardTool::LaserPointer;
            laser.points.push_back({ 0.5f, 0.5f });
            wb.applyRemoteStroke(laser);
        });
        t.join();
        float lx = 0.0f, ly = 0.0f, la = 0.0f;
        TEST_ASSERT(wb.getLaserPointer(lx, ly, la));
        wb.hideHostOverlay();
        TEST_ASSERT(!wb.isHostOverlayActive());
    }

    // #223: ScratchpadManager UINT32_MAX revision poisoning guard & saturation
    {
        ScratchpadManager sp;
        auto local1 = sp.updateLocalText("v1", 1001, 2);
        TEST_ASSERT(local1.revision == 1);

        ScratchpadUpdatePayload poison{};
        poison.revision = std::numeric_limits<uint32_t>::max();
        poison.authorDeskId = 2002;
        poison.text = "poison";
        TEST_ASSERT(!sp.handleRemoteUpdate(poison));
        TEST_ASSERT(sp.getRevision() == 1);
        TEST_ASSERT(sp.getText() == "v1");

        auto local2 = sp.updateLocalText("v2", 1001, 2);
        TEST_ASSERT(local2.revision == 2);
    }

    fs::remove_all(baseTmp, ec);
}
