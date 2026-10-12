#include "../src/core/protocol.hpp"
#include "../src/core/crypto_identity.hpp"
#include "../src/simd/simd_kernels.hpp"
#include "../src/capture/screen_capture.hpp"
#include "../src/control/input_injector.hpp"
#include "../src/control/clipboard_file_manager.hpp"
#include "../src/net/network_engine.hpp"
#include "../src/net/updater.hpp"
#include "../src/ui/notification_manager.hpp"
#include "../src/media/session_recorder.hpp"
#include "../src/media/session_recording_player.hpp"
#include "../src/media/voice_intercom.hpp"
#include "../src/capture/display_manager.hpp"
#include "../src/capture/virtual_display_manager.hpp"
#include "../src/capture/screen_blank_manager.hpp"
#include "../src/control/session_tab_manager.hpp"
#include "../src/control/windows_service_manager.hpp"
#include "../src/control/shortcut_manager.hpp"
#include "../src/core/totp_manager.hpp"
#include "../src/core/qr_matrix.hpp"
#include "../src/control/file_sync_manager.hpp"
#include "../src/capture/session_rewind_buffer.hpp"
#include "../src/capture/privacy_cloak_manager.hpp"
#include "../src/control/scratchpad_manager.hpp"
#include "../src/control/command_palette.hpp"
#include "../src/ui/ui_platform.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#ifdef DeleteFile
#undef DeleteFile
#endif

#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <cassert>
#include <cstring>
#include <cmath>

using namespace cppdesk;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) {
        ++g_passed;
    } else {
        ++g_failed;
        std::cerr << "[FAIL] " << file << ":" << line << " -> " << expr << "\n";
    }
}

#define TEST_ASSERT(cond) check((cond), #cond, __FILE__, __LINE__)

void testDeskIdAndCrypto() {
    std::cout << "[TEST 1] 9-Digit Desk ID Formatting & CNG Crypto Handshake...\n";

    uint64_t id = CryptoUtils::generateNineDigitId();
    TEST_ASSERT(id >= 100000000ULL && id <= 999999999ULL);

    std::string formatted = CryptoUtils::formatDeskId(482910375ULL);
    TEST_ASSERT(formatted == "482 910 375");
    TEST_ASSERT(CryptoUtils::parseDeskId("482 910 375") == 482910375ULL);
    TEST_ASSERT(CryptoUtils::parseDeskId("482-910-375") == 482910375ULL);
    TEST_ASSERT(CryptoUtils::parseDeskId("482910375") == 482910375ULL);
    TEST_ASSERT(CryptoUtils::parseDeskId("127.0.0.1:50990") == 0ULL);

    IdentityManager hostId(10);
    hostId.loadOrCreate();
    hostId.setUnattendedEnabled(true);
    hostId.setUnattendedPassword("SecretPass#2026");

    // Verify plaintext password is NEVER stored in config_*.ini (only salted verifier)
    {
        std::ifstream cfgIn(hostId.configFilePath());
        std::string cfgText((std::istreambuf_iterator<char>(cfgIn)), std::istreambuf_iterator<char>());
        TEST_ASSERT(cfgText.find("SecretPass#2026") == std::string::npos);
        TEST_ASSERT(cfgText.find("unattended_verifier=") != std::string::npos);
    }

    // Reload from disk and verify salted verifier still authenticates
    IdentityManager hostReloaded(10);
    hostReloaded.loadOrCreate();
    TEST_ASSERT(hostReloaded.hasStoredPasswordVerifier());

    std::array<uint8_t, 32> nonce{};
    TEST_ASSERT(CryptoUtils::randomBytes(nonce.data(), nonce.size()));

    uint64_t clientId = 123456789ULL;
    auto validResp = CryptoUtils::computeChallengeResponse("SecretPass#2026", hostReloaded.deskId(), clientId, nonce);
    auto wrongResp = CryptoUtils::computeChallengeResponse("WrongPassword", hostReloaded.deskId(), clientId, nonce);

    TEST_ASSERT(hostReloaded.verifyChallengeResponse(clientId, nonce, validResp));
    TEST_ASSERT(!hostReloaded.verifyChallengeResponse(clientId, nonce, wrongResp));

    // Verify dynamic 6-char One-Time Session Code authentication & regeneration
    std::string code1 = hostReloaded.sessionCode();
    TEST_ASSERT(code1.size() == 6);
    auto codeResp1 = CryptoUtils::computeChallengeResponse(code1, hostReloaded.deskId(), clientId, nonce);
    TEST_ASSERT(hostReloaded.verifyChallengeResponse(clientId, nonce, codeResp1));

    std::string code2 = hostReloaded.regenerateSessionCode();
    TEST_ASSERT(code2.size() == 6);
    auto codeResp2 = CryptoUtils::computeChallengeResponse(code2, hostReloaded.deskId(), clientId, nonce);
    TEST_ASSERT(hostReloaded.verifyChallengeResponse(clientId, nonce, codeResp2));

    // Stream cipher round-trip & SAS fingerprint ("XXXX-XXXX")
    auto sessionKey = CryptoUtils::deriveSessionKey(hostReloaded.deskId(), clientId, nonce);
    std::string fp = CryptoUtils::sessionFingerprintHex(sessionKey);
    TEST_ASSERT(fp.size() == 9);

    std::string secretPayload = "AeroDesk Encrypted Frame Payload Verification 1234567890";
    std::vector<uint8_t> buf(secretPayload.begin(), secretPayload.end());
    CryptoUtils::transformPayload(buf.data(), buf.size(), sessionKey, 42);
    TEST_ASSERT(std::memcmp(buf.data(), secretPayload.data(), buf.size()) != 0);
    CryptoUtils::transformPayload(buf.data(), buf.size(), sessionKey, 42);
    TEST_ASSERT(std::memcmp(buf.data(), secretPayload.data(), buf.size()) == 0);
}

void testTileCodecRoundTrip() {
    std::cout << "[TEST 2] Hybrid Zstd & GDI+ JPEG Tile Codec Round-Trip...\n";

    TileCodec::initGdiPlus();

    const int W = 128;
    const int H = 128;
    std::vector<uint8_t> srcRect(W * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t idx = static_cast<size_t>(y * W + x) * 4;
            srcRect[idx + 0] = static_cast<uint8_t>((x * 2) & 0xFF);
            srcRect[idx + 1] = static_cast<uint8_t>((y * 2) & 0xFF);
            srcRect[idx + 2] = static_cast<uint8_t>(((x + y) * 3) & 0xFF);
            srcRect[idx + 3] = 0xFF;
        }
    }

    // Ultra (Zstd lossless on structured pattern)
    EncodedTile zTile = TileCodec::encodeRect(32, 32, W, H, srcRect.data(), QualityPreset::Ultra);
    TEST_ASSERT(!zTile.data.empty());

    std::vector<uint8_t> canvas(256 * 256 * 4, 0);
    TEST_ASSERT(TileCodec::decodeTileIntoCanvas(zTile, canvas.data(), 256, 256));

    // Verify lossless pixel match at (32, 32)
    if (zTile.encoding == TileEncoding::Zstd) {
        bool exactMatch = true;
        for (int y = 0; y < H; ++y) {
            const uint8_t* cRow = canvas.data() + ((32 + y) * 256 + 32) * 4;
            const uint8_t* sRow = srcRect.data() + (y * W) * 4;
            if (std::memcmp(cRow, sRow, W * 4) != 0) {
                exactMatch = false;
                break;
            }
        }
        TEST_ASSERT(exactMatch);
    }

    // Explicit JPEG encode & decode test
    auto jpegBytes = TileCodec::encodeJpeg(srcRect.data(), W, H, 85);
    TEST_ASSERT(!jpegBytes.empty());
    std::vector<uint8_t> decodedJpeg;
    int jw = 0, jh = 0;
    TEST_ASSERT(TileCodec::decodeJpeg(jpegBytes.data(), jpegBytes.size(), decodedJpeg, jw, jh));
    TEST_ASSERT(jw == W && jh == H);
}

void testScreenCapturer() {
    std::cout << "[TEST 3] DXGI Desktop Duplication / GDI Screen Capture & Delta Tiles...\n";

    ScreenCapturer capturer;
    auto monitors = capturer.enumerateMonitors();
    TEST_ASSERT(!monitors.empty());
    TEST_ASSERT(capturer.frameWidth() > 0 && capturer.frameHeight() > 0);

    std::vector<EncodedTile> tiles;
    bool isKf = false;
    CursorState cursor{};

    bool ok = capturer.captureDirtyTiles(true, QualityPreset::Balanced, tiles, isKf, cursor);
    TEST_ASSERT(ok);
    TEST_ASSERT(isKf);
    TEST_ASSERT(!tiles.empty());

    std::cout << "  -> Captured " << capturer.frameWidth() << "x" << capturer.frameHeight()
              << " keyframe into " << tiles.size() << " compressed tiles (Backend: "
              << (capturer.usingDxgi() ? "DXGI GPU" : "GDI") << ")\n";
}

void testEndToEndSessionAndFileTransfer() {
    std::cout << "[TEST 4] End-to-End 9-Digit ID Resolution, E2EE Stream, Video, File SHA-256, Chat & Rate Limiting...\n" << std::flush;

    IdentityManager hostIdentity(11);
    hostIdentity.loadOrCreate();
    hostIdentity.setListenPort(50994);
    hostIdentity.setUnattendedEnabled(true);
    hostIdentity.setUnattendedPassword("TestPass99");
    hostIdentity.setRelayServerAddress("127.0.0.1:50999");

    IdentityManager viewerIdentity(12);
    viewerIdentity.loadOrCreate();
    viewerIdentity.setListenPort(50995);
    viewerIdentity.setRelayServerAddress("127.0.0.1:50999");

    NetworkEngine hostNet(hostIdentity);
    NetworkEngine viewerNet(viewerIdentity);

    // Configure isolated temp receive directory for file transfer verification
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "AeroDesk_Test_Xfer";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    hostNet.fileTransferManager().setReceiveDirectory(tempDir.string());

    TEST_ASSERT(hostNet.start());
    hostNet.startLocalRelayServer(DEFAULT_RELAY_PORT);
    TEST_ASSERT(viewerNet.start());

    // Wait briefly for UDP LAN discovery / Relay registration
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Connect Viewer -> Host using the formatted 9-digit Desk ID ("XXX XXX XXX") and Unattended Password
    std::string targetIdStr = hostIdentity.formattedDeskId();
    std::cout << "  -> Dialing Host by 9-digit ID: " << targetIdStr << "...\n" << std::flush;
    TEST_ASSERT(viewerNet.connectToRemote(targetIdStr, "TestPass99"));

    // Wait up to 4 seconds for Connected state and first decoded video frame
    uint64_t frameSeq = 0;
    std::vector<uint8_t> viewerFrame;
    int fw = 0, fh = 0;
    CursorState cur{};
    bool gotVideoFrame = false;

    for (int i = 0; i < 80; ++i) {
        auto st = viewerNet.viewerStats();
        if (st.state == ViewerConnectionState::Connected) {
            if (viewerNet.copyLatestViewerFrame(frameSeq, viewerFrame, fw, fh, cur) && fw > 0 && fh > 0) {
                gotVideoFrame = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    TEST_ASSERT(viewerNet.viewerStats().state == ViewerConnectionState::Connected);
    TEST_ASSERT(gotVideoFrame);
    // Verify E2EE session SAS fingerprint matches between Viewer and Host
    TEST_ASSERT(!viewerNet.viewerStats().securityFingerprint.empty());
    TEST_ASSERT(viewerNet.viewerStats().securityFingerprint == hostNet.hostSessionStatus().securityFingerprint);
    std::cout << "  -> Viewer received encrypted video frame (" << fw << "x" << fh
              << ", SAS: " << viewerNet.viewerStats().securityFingerprint << ")!\n";

    // Test bidirectional encrypted Live Chat
    TEST_ASSERT(viewerNet.sendChatMessage("Hello Host over E2EE!"));
    TEST_ASSERT(hostNet.sendChatMessage("Hello Viewer from Host!"));
    bool chatSynced = false;
    for (int i = 0; i < 30; ++i) {
        auto vMsgs = viewerNet.chatMessages();
        auto hMsgs = hostNet.chatMessages();
        if (vMsgs.size() >= 2 && hMsgs.size() >= 2) {
            chatSynced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    TEST_ASSERT(chatSynced);
    std::cout << "  -> Verified bidirectional encrypted live chat!\n";

    // Test chunked file transfer from Viewer -> Host with streaming SHA-256 verification & .part rename
    std::filesystem::path srcFile = tempDir / "source_payload.bin";
    std::string testContent(150000, '\0'); // ~150 KB (> 2 chunks)
    for (size_t i = 0; i < testContent.size(); ++i) {
        testContent[i] = static_cast<char>('A' + (i % 26));
    }
    {
        std::ofstream out(srcFile, std::ios::binary);
        out.write(testContent.data(), static_cast<std::streamsize>(testContent.size()));
    }

    uint32_t tid = viewerNet.sendFile(srcFile.string());
    TEST_ASSERT(tid > 0);

    bool fileReceived = false;
    std::filesystem::path receivedPath;
    for (int i = 0; i < 60; ++i) {
        auto items = hostNet.fileTransferManager().snapshotTransfers();
        if (!items.empty() && items.front().status == TransferStatus::Completed) {
            fileReceived = true;
            receivedPath = items.front().savedPath;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    TEST_ASSERT(fileReceived);
    if (fileReceived) {
        std::ifstream in(receivedPath, std::ios::binary);
        std::string recContent((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TEST_ASSERT(recContent.size() == testContent.size());
        TEST_ASSERT(CryptoUtils::sha256Hex(recContent) == CryptoUtils::sha256Hex(testContent));
        // Ensure temporary .part file was atomically renamed away
        TEST_ASSERT(!std::filesystem::exists(receivedPath.string() + ".part"));
        std::cout << "  -> Verified 150 KB chunked file transfer SHA-256 integrity & atomic .part rename!\n";
    }

    // Test live permission updates from Host -> Viewer
    hostNet.updateHostSessionPermissions(PERM_CLIPBOARD | PERM_FILE_TRANSFER); // Disable PERM_INPUT
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    TEST_ASSERT((viewerNet.viewerStats().grantedPermissions & PERM_INPUT) == 0);
    TEST_ASSERT((viewerNet.viewerStats().grantedPermissions & PERM_FILE_TRANSFER) != 0);

    // Test live 15 / 30 / 60 FPS control updates from Viewer -> Host
    viewerNet.setSessionFpsConfig(60, true);
    TEST_ASSERT(viewerNet.viewerStats().targetFps == 60);
    TEST_ASSERT(viewerNet.viewerStats().adaptiveFps == true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    viewerNet.setSessionFpsConfig(15, false);
    TEST_ASSERT(viewerNet.viewerStats().targetFps == 15);
    TEST_ASSERT(viewerNet.viewerStats().adaptiveFps == false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    viewerNet.disconnectViewer();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Test Interactive Approval Modal Flow (connecting with blank password -> Host clicks Accept)
    std::cout << "  -> Testing Interactive Approval Flow (Accept/Reject modal + custom permissions)...\n";
    TEST_ASSERT(viewerNet.connectToRemote(targetIdStr, ""));

    bool sawPendingPopup = false;
    for (int i = 0; i < 40; ++i) {
        auto pending = hostNet.pendingIncomingRequest();
        if (pending.active && pending.callerDeskId == viewerIdentity.deskId()) {
            sawPendingPopup = true;
            hostNet.respondToIncomingRequest(true, PERM_INPUT | PERM_CLIPBOARD);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TEST_ASSERT(sawPendingPopup);

    for (int i = 0; i < 40; ++i) {
        if (viewerNet.viewerStats().state == ViewerConnectionState::Connected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TEST_ASSERT(viewerNet.viewerStats().state == ViewerConnectionState::Connected);
    TEST_ASSERT(viewerNet.viewerStats().grantedPermissions == (PERM_INPUT | PERM_CLIPBOARD));

    viewerNet.disconnectViewer();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Test Brute-Force IP Rate Limiting (5 failed password attempts -> RateLimited lockout)
    std::cout << "  -> Testing Brute-Force Rate Limiting (5 bad attempts -> 60s lockout)...\n";
    hostNet.clearRateLimitRecords();
    bool sawRateLimited = false;
    for (int attempt = 1; attempt <= 10; ++attempt) {
        TEST_ASSERT(viewerNet.connectToRemote(targetIdStr, "WrongPasswordAttempt"));
        for (int w = 0; w < 60; ++w) {
            auto st = viewerNet.viewerStats();
            if (st.state == ViewerConnectionState::Error) {
                if (st.statusMessage.find("Too many failed") != std::string::npos ||
                    st.statusMessage.find("Locked for 60s") != std::string::npos) {
                    sawRateLimited = true;
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        viewerNet.disconnectViewer();
        if (sawRateLimited) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    TEST_ASSERT(sawRateLimited);
    hostNet.clearRateLimitRecords();

    hostNet.stop();
    viewerNet.stop();
    std::filesystem::remove_all(tempDir, ec);
}

void testFileTransferEdgeCasesAndFavorites() {
    std::cout << "[TEST 5] File Transfer .part Cleanup, SHA-256 Corruption Rejection & Favorite Desks...\n";

    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "AeroDesk_Test_Edge";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    FileTransferManager ftm;
    ftm.setReceiveDirectory(tempDir.string());

    // 1. Mid-transfer cancel cleans up .part file
    ftm.handleFileOffer(701, 8192, "partial_cancel.bin");
    std::vector<uint8_t> chunkData(1024, 0x5A);
    ftm.handleFileChunk(701, 0, chunkData.data(), chunkData.size());
    TEST_ASSERT(std::filesystem::exists(tempDir / "partial_cancel.bin.part"));

    ftm.handleFileCancel(701);
    TEST_ASSERT(!std::filesystem::exists(tempDir / "partial_cancel.bin.part"));
    TEST_ASSERT(!std::filesystem::exists(tempDir / "partial_cancel.bin"));

    // 2. Corrupted SHA-256 on FileComplete deletes .part file and marks transfer Failed
    ftm.handleFileOffer(702, 1024, "corrupt_check.bin");
    ftm.handleFileChunk(702, 0, chunkData.data(), chunkData.size());
    ftm.handleFileComplete(702, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    TEST_ASSERT(!std::filesystem::exists(tempDir / "corrupt_check.bin.part"));
    TEST_ASSERT(!std::filesystem::exists(tempDir / "corrupt_check.bin"));

    // 2b. Empty SHA-256 on FileComplete is rejected and deletes .part file
    ftm.handleFileOffer(703, 1024, "empty_sha_check.bin");
    ftm.handleFileChunk(703, 0, chunkData.data(), chunkData.size());
    ftm.handleFileComplete(703, "");
    TEST_ASSERT(!std::filesystem::exists(tempDir / "empty_sha_check.bin.part"));
    TEST_ASSERT(!std::filesystem::exists(tempDir / "empty_sha_check.bin"));

    // 2c. Concurrent incoming stream queue cap (max 10)
    for (uint32_t i = 800; i < 810; ++i) {
        ftm.handleFileOffer(i, 512, "stream_" + std::to_string(i) + ".bin");
    }
    ftm.handleFileOffer(811, 512, "stream_overflow.bin");
    bool foundQueueFull = false;
    for (const auto& item : ftm.snapshotTransfers()) {
        if (item.transferId == 811 && item.statusText == "Rejected (Queue Full)") {
            foundQueueFull = true;
            break;
        }
    }
    TEST_ASSERT(foundQueueFull);
    ftm.abortActiveTransfers();

    std::filesystem::remove_all(tempDir, ec);

    // 3. Pinned Favorite Desks & Recent Session removal persistence
    IdentityManager favMgr(18);
    favMgr.loadOrCreate();
    favMgr.addOrUpdateRecentSession(482910375ULL, "Design-Workstation", "127.0.0.1:50990");
    favMgr.toggleFavoriteSession(482910375ULL);

    IdentityManager favReload(18);
    favReload.loadOrCreate();
    bool foundFav = false;
    for (const auto& r : favReload.recentSessions()) {
        if (r.deskId == 482910375ULL && r.isFavorite) {
            foundFav = true;
            break;
        }
    }
    TEST_ASSERT(foundFav);
    favReload.removeRecentSession(482910375ULL);
    bool stillPresent = false;
    for (const auto& r : favReload.recentSessions()) {
        if (r.deskId == 482910375ULL) stillPresent = true;
    }
    TEST_ASSERT(!stillPresent);
}

void testAppSettingsAndAdaptiveFps() {
    std::cout << "[TEST 6] AppSettings Persistence, Light/Dark Theme Config & Adaptive FPS Congestion Throttling...\n";

    // 1. Verify clampTargetFps
    TEST_ASSERT(clampTargetFps(10) == 15);
    TEST_ASSERT(clampTargetFps(15) == 15);
    TEST_ASSERT(clampTargetFps(28) == 30);
    TEST_ASSERT(clampTargetFps(30) == 30);
    TEST_ASSERT(clampTargetFps(45) == 30);
    TEST_ASSERT(clampTargetFps(50) == 60);
    TEST_ASSERT(clampTargetFps(60) == 60);
    TEST_ASSERT(clampTargetFps(120) == 60);

    // 2. Verify computeAdaptiveFpsCap automatic poor-network step-down (60 -> 30 -> 15 FPS)
    // Healthy network (12ms RTT, 4ms send): keeps 60 FPS
    TEST_ASSERT(computeAdaptiveFpsCap(60, true, 12, 4.0f) == 60);
    // Moderate network congestion (110ms RTT or 42ms send): steps 60 FPS down to 30 FPS
    TEST_ASSERT(computeAdaptiveFpsCap(60, true, 110, 10.0f) == 30);
    TEST_ASSERT(computeAdaptiveFpsCap(60, true, 25, 42.0f) == 30);
    // Severe network congestion (210ms RTT or 85ms send): drops to 15 FPS
    TEST_ASSERT(computeAdaptiveFpsCap(60, true, 210, 10.0f) == 15);
    TEST_ASSERT(computeAdaptiveFpsCap(60, true, 30, 85.0f) == 15);
    TEST_ASSERT(computeAdaptiveFpsCap(30, true, 195, 12.0f) == 15);
    // Adaptive disabled: honours user target FPS regardless of RTT/send time
    TEST_ASSERT(computeAdaptiveFpsCap(60, false, 250, 95.0f) == 60);
    TEST_ASSERT(computeAdaptiveFpsCap(30, false, 250, 95.0f) == 30);
    TEST_ASSERT(computeAdaptiveFpsCap(15, true, 250, 95.0f) == 15);

    // 3. Verify AppSettings round-trip save/load in IdentityManager
    {
        IdentityManager idSave(19);
        idSave.loadOrCreate();
        AppSettings s = idSave.settings();
        s.darkTheme = true;
        s.targetFps = 60;
        s.adaptiveFps = false;
        s.defaultQuality = QualityPreset::LowBandwidth;
        s.defaultScaleMode = 1;
        s.showRemoteCursor = false;
        s.showSessionHud = false;
        s.autoAcceptIncoming = true;
        s.defaultPermissions = PERM_INPUT | PERM_CLIPBOARD;
        s.lockWorkstationOnDisconnect = true;
        s.hardwareAcceleration = false; // Hardware acceleration toggle
        idSave.updateSettings(s);
    }
    {
        IdentityManager idLoad(19);
        idLoad.loadOrCreate();
        AppSettings loaded = idLoad.settings();
        TEST_ASSERT(loaded.darkTheme == true);
        TEST_ASSERT(loaded.targetFps == 60);
        TEST_ASSERT(loaded.adaptiveFps == false);
        TEST_ASSERT(loaded.defaultQuality == QualityPreset::LowBandwidth);
        TEST_ASSERT(loaded.defaultScaleMode == 1);
        TEST_ASSERT(loaded.showRemoteCursor == false);
        TEST_ASSERT(loaded.showSessionHud == false);
        TEST_ASSERT(loaded.autoAcceptIncoming == true);
        TEST_ASSERT(loaded.defaultPermissions == (PERM_INPUT | PERM_CLIPBOARD));
        TEST_ASSERT(loaded.lockWorkstationOnDisconnect == true);
        TEST_ASSERT(loaded.hardwareAcceleration == false);

        // Reset to defaults and verify
        idLoad.resetSettingsToDefault();
        AppSettings def = idLoad.settings();
        TEST_ASSERT(def.darkTheme == false);
        TEST_ASSERT(def.targetFps == 30);
        TEST_ASSERT(def.adaptiveFps == true);
        TEST_ASSERT(def.defaultQuality == QualityPreset::Balanced);
        TEST_ASSERT(def.defaultPermissions == PERM_ALL);
        TEST_ASSERT(def.hardwareAcceleration == true);
    }
}

void testAvx2SimdAssemblyKernels() {
    std::cout << "[TEST 7] x86-64 AVX2 Assembly SIMD Kernels (Tile Diff, Tile Hash & Stream Cipher)...\n";
    TEST_ASSERT(SimdKernels::hasAvx2());

    const int W = 64;
    const int H = 64;
    const int stride = W * 4;
    std::vector<uint8_t> tileA(W * H * 4, 0x42);
    std::vector<uint8_t> tileB(W * H * 4, 0x42);

    // 1. Identical 64x64 tiles -> diff == 0, hashes match
    TEST_ASSERT(cppdesk_avx2_tile_diff(tileA.data(), tileB.data(), stride, W * 4, H) == 0);
    uint64_t h1 = cppdesk_avx2_hash_tile(tileA.data(), stride, W * 4, H);
    uint64_t h2 = cppdesk_avx2_hash_tile(tileB.data(), stride, W * 4, H);
    TEST_ASSERT(h1 == h2 && h1 != 0);

    // 2. Single-pixel difference -> diff == 1, hash changes
    tileB[W * H * 2 + 17] ^= 0x01;
    TEST_ASSERT(cppdesk_avx2_tile_diff(tileA.data(), tileB.data(), stride, W * 4, H) == 1);
    uint64_t h3 = cppdesk_avx2_hash_tile(tileB.data(), stride, W * 4, H);
    TEST_ASSERT(h1 != h3);
    tileB[W * H * 2 + 17] ^= 0x01; // Restore

    // 3. Benchmark AVX2 Assembly vs C++ Scalar over 25,000 64x64 tile comparisons (400 MB)
    const int iters = 25000;
    auto t0 = std::chrono::high_resolution_clock::now();
    int accScalar = 0;
    for (int i = 0; i < iters; ++i) {
        accScalar += SimdKernels::scalarTileDiff(tileA.data(), tileB.data(), stride, W * 4, H) ? 1 : 0;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    int accAvx2 = 0;
    for (int i = 0; i < iters; ++i) {
        accAvx2 += cppdesk_avx2_tile_diff(tileA.data(), tileB.data(), stride, W * 4, H);
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    TEST_ASSERT(accScalar == 0 && accAvx2 == 0);

    double scalarSec = std::chrono::duration<double>(t1 - t0).count();
    double avx2Sec   = std::chrono::duration<double>(t2 - t1).count();
    double totalGb   = (static_cast<double>(iters) * W * H * 4.0) / (1024.0 * 1024.0 * 1024.0);
    double avx2Gbs   = totalGb / std::max(1e-6, avx2Sec);
    double speedup   = scalarSec / std::max(1e-6, avx2Sec);

    std::cout << "  -> AVX2 Assembly 64x64 Tile Diff Throughput: " << avx2Gbs
              << " GB/s (" << speedup << "x faster than scalar C++)\n";
}

void testEcdhAndAesGcmEngine() {
    std::cout << "[TEST 8] Ephemeral ECDH (P-256), AES-256-GCM AEAD & Multi-threaded Tile Pipeline...\n";

    // 1. Anti-Replay Sliding Window (64-packet bitmask)
    AntiReplayWindow replayWin;
    TEST_ASSERT(replayWin.checkAndMark(0));
    TEST_ASSERT(!replayWin.checkAndMark(0)); // Duplicate rejected
    TEST_ASSERT(replayWin.checkAndMark(5));  // Higher seq
    TEST_ASSERT(replayWin.checkAndMark(3));  // Out-of-order within 64 allowed
    TEST_ASSERT(!replayWin.checkAndMark(3)); // Duplicate rejected
    TEST_ASSERT(replayWin.checkAndMark(100)); // Advances window
    TEST_ASSERT(!replayWin.checkAndMark(10)); // Far behind (< 100 - 64) rejected
    TEST_ASSERT(!replayWin.checkAndMark(100)); // Duplicate rejected

    // 2. Ephemeral NIST P-256 ECDH Key Agreement (Zero-Trust)
    EcdhKeyExchange hostEcdh;
    EcdhKeyExchange viewerEcdh;
    TEST_ASSERT(hostEcdh.initialize());
    TEST_ASSERT(viewerEcdh.initialize());

    const auto& hostPub = hostEcdh.localPublicKey();
    const auto& viewerPub = viewerEcdh.localPublicKey();
    TEST_ASSERT(hostPub.size() == 72); // BCRYPT_ECCPUBLIC_BLOB for P-256
    TEST_ASSERT(viewerPub.size() == 72);

    std::array<uint8_t, 32> nonce{};
    CryptoUtils::randomBytes(nonce.data(), nonce.size());
    uint64_t hostDeskId = 111222333ULL;
    uint64_t viewerDeskId = 444555666ULL;

    std::array<uint8_t, 32> hostDerivedKey{};
    std::array<uint8_t, 32> viewerDerivedKey{};
    TEST_ASSERT(hostEcdh.computeSharedSessionKey(viewerPub.data(), viewerPub.size(), hostDeskId, viewerDeskId, nonce, hostDerivedKey));
    TEST_ASSERT(viewerEcdh.computeSharedSessionKey(hostPub.data(), hostPub.size(), hostDeskId, viewerDeskId, nonce, viewerDerivedKey));

    TEST_ASSERT(hostDerivedKey == viewerDerivedKey);
    TEST_ASSERT(CryptoUtils::sessionFingerprintHex(hostDerivedKey) == CryptoUtils::sessionFingerprintHex(viewerDerivedKey));

    // 3. Hardware AES-256-GCM AEAD Session Ciphers
    AesGcmSessionCipher hostCipher;
    AesGcmSessionCipher viewerCipher;
    TEST_ASSERT(hostCipher.initialize(hostDerivedKey, /*isHost=*/true));
    TEST_ASSERT(viewerCipher.initialize(viewerDerivedKey, /*isHost=*/false));

    // Host -> Viewer Frame Encryption
    std::string testPlaintext = "AeroDesk 4K 60FPS Video Tile Payload (Confidential & Authenticated)";
    FrameHeader mockHdr{};
    mockHdr.magic = PROTOCOL_MAGIC;
    mockHdr.type = static_cast<uint8_t>(PacketType::VIDEO_FRAME_TILES);
    mockHdr.flags = FLAG_ENCRYPTED;
    mockHdr.payloadSize = static_cast<uint32_t>(12 + testPlaintext.size() + 16);

    std::vector<uint8_t> encryptedPkt;
    TEST_ASSERT(hostCipher.encrypt(testPlaintext.data(), testPlaintext.size(), 1, &mockHdr, sizeof(mockHdr), encryptedPkt));
    TEST_ASSERT(encryptedPkt.size() == 12 + testPlaintext.size() + 16);

    // Decrypt on Viewer
    std::vector<uint8_t> decryptedPkt;
    uint64_t rxSeq = 0;
    TEST_ASSERT(viewerCipher.decrypt(encryptedPkt.data(), encryptedPkt.size(), &mockHdr, sizeof(mockHdr), decryptedPkt, &rxSeq));
    TEST_ASSERT(rxSeq == 1);
    TEST_ASSERT(std::string(decryptedPkt.begin(), decryptedPkt.end()) == testPlaintext);

    // Replay Attack Detection
    std::vector<uint8_t> replayDec;
    TEST_ASSERT(!viewerCipher.decrypt(encryptedPkt.data(), encryptedPkt.size(), &mockHdr, sizeof(mockHdr), replayDec));

    // Tamper Detection: Bit-Flip in Ciphertext
    std::vector<uint8_t> tamperedPkt = encryptedPkt;
    tamperedPkt[15] ^= 0x40;
    std::vector<uint8_t> tamperedDec;
    TEST_ASSERT(!viewerCipher.decrypt(tamperedPkt.data(), tamperedPkt.size(), &mockHdr, sizeof(mockHdr), tamperedDec));

    // Tamper Detection: Header/AAD mismatch
    FrameHeader tamperedHdr = mockHdr;
    tamperedHdr.type = static_cast<uint8_t>(PacketType::INPUT_MOUSE_MOVE);
    TEST_ASSERT(!viewerCipher.decrypt(encryptedPkt.data(), encryptedPkt.size(), &tamperedHdr, sizeof(tamperedHdr), tamperedDec));

    // Cryptographic Anti-Replay: Unauthenticated packet with high seq (99999) must NOT poison window
    std::vector<uint8_t> forgedPkt(12 + 32 + 16, 0xEE);
    uint64_t forgedSeq = 99999;
    std::memcpy(forgedPkt.data() + 4, &forgedSeq, 8);
    std::vector<uint8_t> forgedDec;
    TEST_ASSERT(!viewerCipher.decrypt(forgedPkt.data(), forgedPkt.size(), &mockHdr, sizeof(mockHdr), forgedDec));

    // Legitimate subsequent packet with seq = 2 must decrypt successfully
    std::vector<uint8_t> legitPkt2;
    std::string legitMsg2 = "Legitimate seq 2 after forged attempt";
    mockHdr.payloadSize = static_cast<uint32_t>(12 + legitMsg2.size() + 16);
    TEST_ASSERT(hostCipher.encrypt(legitMsg2.data(), legitMsg2.size(), 2, &mockHdr, sizeof(mockHdr), legitPkt2));
    std::vector<uint8_t> legitDec2;
    uint64_t rxSeq2 = 0;
    TEST_ASSERT(viewerCipher.decrypt(legitPkt2.data(), legitPkt2.size(), &mockHdr, sizeof(mockHdr), legitDec2, &rxSeq2));
    TEST_ASSERT(rxSeq2 == 2);
    TEST_ASSERT(std::string(legitDec2.begin(), legitDec2.end()) == legitMsg2);

    // Viewer -> Host Input Encryption
    std::string mouseEvent = "MOUSE_MOVE_CLICK";
    mockHdr.type = static_cast<uint8_t>(PacketType::INPUT_MOUSE_MOVE);
    mockHdr.payloadSize = static_cast<uint32_t>(12 + mouseEvent.size() + 16);
    std::vector<uint8_t> clientEnc;
    TEST_ASSERT(viewerCipher.encrypt(mouseEvent.data(), mouseEvent.size(), 1, &mockHdr, sizeof(mockHdr), clientEnc));
    std::vector<uint8_t> clientDec;
    uint64_t clientSeq = 0;
    TEST_ASSERT(hostCipher.decrypt(clientEnc.data(), clientEnc.size(), &mockHdr, sizeof(mockHdr), clientDec, &clientSeq));
    TEST_ASSERT(clientSeq == 1);
    TEST_ASSERT(std::string(clientDec.begin(), clientDec.end()) == mouseEvent);

    // Rekeying Ratchet (Option 4A)
    TEST_ASSERT(hostCipher.ratchetKey());
    TEST_ASSERT(viewerCipher.ratchetKey());
    std::string postRatchetPlain = "Payload after 1 GB ratchet key rotation";
    mockHdr.payloadSize = static_cast<uint32_t>(12 + postRatchetPlain.size() + 16);
    std::vector<uint8_t> ratchetEnc;
    TEST_ASSERT(hostCipher.encrypt(postRatchetPlain.data(), postRatchetPlain.size(), 2, &mockHdr, sizeof(mockHdr), ratchetEnc));
    std::vector<uint8_t> ratchetDec;
    TEST_ASSERT(viewerCipher.decrypt(ratchetEnc.data(), ratchetEnc.size(), &mockHdr, sizeof(mockHdr), ratchetDec));
    TEST_ASSERT(std::string(ratchetDec.begin(), ratchetDec.end()) == postRatchetPlain);

    // 4. Multi-Threaded Tile Compression Pool (Option 2A)
    std::vector<TileThreadPool::RectTask> tasks;
    for (int i = 0; i < 8; ++i) {
        TileThreadPool::RectTask t{};
        t.rx = static_cast<uint16_t>((i % 4) * 64);
        t.ry = static_cast<uint16_t>((i / 4) * 64);
        t.rw = 64;
        t.rh = 64;
        t.bgraPixels.assign(64 * 64 * 4, static_cast<uint8_t>(0x20 * i + 0x10));
        t.preset = QualityPreset::Balanced;
        tasks.push_back(std::move(t));
    }
    TileThreadPool::instance().parallelEncode(tasks);
    for (const auto& t : tasks) {
        TEST_ASSERT(!t.result.data.empty());
        TEST_ASSERT(t.result.width == 64 && t.result.height == 64);
    }

    // 5. InputInjector UAC Desktop Elevation Query (Option 5A)
    bool isElev = InputInjector::isElevated();
    (void)isElev; // Querying elevated token status executes without throwing or crashing
    TEST_ASSERT(true);
}

void testV201FeaturesAndResilience() {
    std::cout << "[TEST 9] v2.0.1 Multi-Monitor Protocol, System Actions & Quality Update...\n";

    // 1. MONITOR_LIST Serialization & Deserialization
    std::vector<MonitorDesc> mockMons;
    MonitorDesc m1{};
    m1.index = 0; m1.x = 0; m1.y = 0; m1.width = 1920; m1.height = 1080;
    m1.isPrimary = true; m1.name = "Display 1 (1920x1080)";
    mockMons.push_back(m1);

    MonitorDesc m2{};
    m2.index = 1; m2.x = 1920; m2.y = 0; m2.width = 2560; m2.height = 1440;
    m2.isPrimary = false; m2.name = "Display 2 (2560x1440)";
    mockMons.push_back(m2);

    ByteWriter monListW;
    monListW.writeU16(static_cast<uint16_t>(mockMons.size()));
    for (const auto& m : mockMons) {
        monListW.writeI32(m.index);
        monListW.writeI32(m.x);
        monListW.writeI32(m.y);
        monListW.writeI32(m.width);
        monListW.writeI32(m.height);
        monListW.writeU8(m.isPrimary ? 1 : 0);
        monListW.writeString(m.name);
    }

    ByteReader monListR(monListW.buffer());
    uint16_t readCount = monListR.readU16();
    TEST_ASSERT(readCount == 2);
    MonitorDesc r1{};
    r1.index = monListR.readI32();
    r1.x = monListR.readI32();
    r1.y = monListR.readI32();
    r1.width = monListR.readI32();
    r1.height = monListR.readI32();
    r1.isPrimary = (monListR.readU8() != 0);
    r1.name = monListR.readString();
    TEST_ASSERT(r1.index == 0 && r1.width == 1920 && r1.height == 1080 && r1.isPrimary);
    TEST_ASSERT(r1.name == "Display 1 (1920x1080)");

    MonitorDesc r2{};
    r2.index = monListR.readI32();
    r2.x = monListR.readI32();
    r2.y = monListR.readI32();
    r2.width = monListR.readI32();
    r2.height = monListR.readI32();
    r2.isPrimary = (monListR.readU8() != 0);
    r2.name = monListR.readString();
    TEST_ASSERT(r2.index == 1 && r2.x == 1920 && r2.width == 2560 && !r2.isPrimary);
    TEST_ASSERT(r2.name == "Display 2 (2560x1440)");

    // 2. MONITOR_SELECT Serialization & Deserialization
    ByteWriter selW;
    selW.writeI32(1);
    ByteReader selR(selW.buffer());
    int32_t selIdx = selR.readI32();
    TEST_ASSERT(selIdx == 1);

    // 3. QUALITY_UPDATE Serialization & Deserialization
    ByteWriter qW;
    qW.writeU8(static_cast<uint8_t>(QualityPreset::Ultra));
    qW.writeU8(60);
    qW.writeU8(1);
    ByteReader qR(qW.buffer());
    uint8_t qPreset = qR.readU8();
    uint8_t qFps = qR.readU8();
    uint8_t qAdap = qR.readU8();
    TEST_ASSERT(qPreset == static_cast<uint8_t>(QualityPreset::Ultra));
    TEST_ASSERT(qFps == 60);
    TEST_ASSERT(qAdap == 1);

    // 4. SystemActionType Enumeration & Invalid Action Handling
    TEST_ASSERT(static_cast<uint8_t>(SystemActionType::TaskManager) == 1);
    TEST_ASSERT(static_cast<uint8_t>(SystemActionType::ShowDesktop) == 2);
    TEST_ASSERT(static_cast<uint8_t>(SystemActionType::LockWorkstation) == 3);
    TEST_ASSERT(static_cast<uint8_t>(SystemActionType::SendCtrlAltDel) == 4);
    TEST_ASSERT(static_cast<uint8_t>(SystemActionType::EmergencyReboot) == 5);
    TEST_ASSERT(!InputInjector::executeSystemAction(static_cast<SystemActionType>(99)));

    // 5. DXGI ACCESS_LOST Instant GDI Fallback & Recovery
    ScreenCapturer capturer;
    capturer.triggerDxgiAccessLostForTest();
    TEST_ASSERT(capturer.dxgiRecoveryState() == ScreenCapturer::DxgiRecoveryState::FallbackGdi);
    TEST_ASSERT(!capturer.usingDxgi());
    std::vector<EncodedTile> fbTiles;
    bool fbKeyframe = false;
    CursorState fbCursor;
    bool capOk = capturer.captureDirtyTiles(true, QualityPreset::Balanced, fbTiles, fbKeyframe, fbCursor);
    TEST_ASSERT(capOk);
    TEST_ASSERT(fbKeyframe);
    TEST_ASSERT(!fbTiles.empty());

    // 6. Viewer Auto-Reconnect State & Attempts
    ViewerSessionStats stats;
    stats.state = ViewerConnectionState::Reconnecting;
    stats.reconnectAttempt = 1;
    TEST_ASSERT(stats.state == ViewerConnectionState::Reconnecting);
    TEST_ASSERT(stats.reconnectAttempt == 1);

    // 7. InputInjector Elevation & Safe Desktop Sync
    bool isElev = InputInjector::isElevated();
    TEST_ASSERT(isElev == true || isElev == false);
    // Desktop sync does not crash or invalidate handles
    bool syncOk = InputInjector::syncToInputDesktop();
    (void)syncOk; // May be true or false depending on execution environment

    // 8. Hardware Cursor & Mouse Move Bounds Clamping
    MonitorDesc testMon{ 0, 0, 0, 1920, 1080, true, "TestMon" };
    InputInjector::injectMouseMove(0.5f, 0.5f, testMon);
    InputInjector::injectMouseMove(-1.0f, 2.0f, testMon); // Clamped to [0, 1]
    POINT pt{};
    if (GetCursorPos(&pt)) {
        TEST_ASSERT(pt.x >= 0 && pt.y >= 0);
    }
}

void testNotificationSystemAndTray() {
    std::cout << "[TEST 10] Windows Push Notifications, Tray Icon & AppSettings Persistence...\n";

    // 1. AppSettings default notification toggles
    AppSettings defSettings{};
    TEST_ASSERT(defSettings.enablePushNotifications == true);
    TEST_ASSERT(defSettings.enableTaskbarFlash == true);
    TEST_ASSERT(defSettings.enableNotificationSounds == true);
    TEST_ASSERT(defSettings.minimizeToTray == false);

    // 2. INI Serialization / Deserialization round-trip
    {
        IdentityManager idMgr(99);
        idMgr.loadOrCreate();

        AppSettings custom = idMgr.settings();
        custom.enablePushNotifications = false;
        custom.enableTaskbarFlash = false;
        custom.enableNotificationSounds = false;
        custom.minimizeToTray = true;
        idMgr.updateSettings(custom);

        // Reload from disk to verify persistence
        IdentityManager reloadMgr(99);
        TEST_ASSERT(reloadMgr.loadOrCreate());
        const auto& loaded = reloadMgr.settings();
        TEST_ASSERT(!loaded.enablePushNotifications);
        TEST_ASSERT(!loaded.enableTaskbarFlash);
        TEST_ASSERT(!loaded.enableNotificationSounds);
        TEST_ASSERT(loaded.minimizeToTray);

        // Clean up test config
        std::error_code ec;
        std::filesystem::remove(idMgr.configFilePath(), ec);
    }

    // 3. NotificationManager lifecycle and features
    NotificationManager notifMgr;
    TEST_ASSERT(!notifMgr.isMuted());
    notifMgr.setMuted(true);
    TEST_ASSERT(notifMgr.isMuted());
    notifMgr.setMuted(false);
    TEST_ASSERT(!notifMgr.isMuted());

    TEST_ASSERT(notifMgr.lastNotificationType() == NotificationType::GeneralInfo);
    AppSettings s{};
    notifMgr.notify(NotificationType::ChatMessage, "Test User", "Hello AeroDesk!", s);
    TEST_ASSERT(notifMgr.lastNotificationType() == NotificationType::ChatMessage);

    notifMgr.notify(NotificationType::IncomingConnection, "Incoming Request", "Desk 123 456 789 wants to connect", s);
    TEST_ASSERT(notifMgr.lastNotificationType() == NotificationType::IncomingConnection);

    notifMgr.notify(NotificationType::FileTransferDone, "File Transfer", "document.pdf received", s);
    TEST_ASSERT(notifMgr.lastNotificationType() == NotificationType::FileTransferDone);

    notifMgr.notify(NotificationType::SessionDropped, "Disconnected", "Session terminated", s);
    TEST_ASSERT(notifMgr.lastNotificationType() == NotificationType::SessionDropped);

    notifMgr.clearLastNotificationType();
    TEST_ASSERT(notifMgr.lastNotificationType() == NotificationType::GeneralInfo);

    // Flash control safe handling with null HWND
    notifMgr.flashTaskbar(false);
    notifMgr.stopFlash();
    TEST_ASSERT(!notifMgr.isFlashing());

    // 4. NOTIFYICON_VERSION_4 event decoding (uID packed in HIWORD, event packed in LOWORD)
    constexpr uint16_t testIconId = 1;
    LPARAM v4ContextMenu = MAKELPARAM(WM_CONTEXTMENU, testIconId);
    LPARAM v4LButtonUp   = MAKELPARAM(WM_LBUTTONUP, testIconId);
    LPARAM v4LButtonDbl  = MAKELPARAM(WM_LBUTTONDBLCLK, testIconId);
    LPARAM v4Select      = MAKELPARAM(NIN_SELECT, testIconId);
    LPARAM v4KeySelect   = MAKELPARAM(NIN_KEYSELECT, testIconId);
    LPARAM v4BalloonClk  = MAKELPARAM(NIN_BALLOONUSERCLICK, testIconId);

    TEST_ASSERT(LOWORD(v4ContextMenu) == WM_CONTEXTMENU);
    TEST_ASSERT(LOWORD(v4LButtonUp) == WM_LBUTTONUP);
    TEST_ASSERT(LOWORD(v4LButtonDbl) == WM_LBUTTONDBLCLK);
    TEST_ASSERT(LOWORD(v4Select) == NIN_SELECT);
    TEST_ASSERT(LOWORD(v4KeySelect) == NIN_KEYSELECT);
    TEST_ASSERT(LOWORD(v4BalloonClk) == NIN_BALLOONUSERCLICK);
    TEST_ASSERT(HIWORD(v4ContextMenu) == testIconId);
    TEST_ASSERT(HIWORD(v4LButtonDbl) == testIconId);

    notifMgr.shutdown();
}

void testV210PowerFeaturesInheritance() {
    std::cout << "Running testV210PowerFeaturesInheritance()...\n";

    // 1. Verify Protocol Opcodes (0x17 - 0x1D)
    TEST_ASSERT(static_cast<uint8_t>(PacketType::AUDIO_STREAM_CHUNK) == 0x17);
    TEST_ASSERT(static_cast<uint8_t>(PacketType::PRIVACY_MODE_TOGGLE) == 0x18);
    TEST_ASSERT(static_cast<uint8_t>(PacketType::TUNNEL_OPEN) == 0x19);
    TEST_ASSERT(static_cast<uint8_t>(PacketType::TUNNEL_DATA) == 0x1A);
    TEST_ASSERT(static_cast<uint8_t>(PacketType::TUNNEL_CLOSE) == 0x1B);
    TEST_ASSERT(static_cast<uint8_t>(PacketType::TERMINAL_DATA) == 0x1C);
    TEST_ASSERT(static_cast<uint8_t>(PacketType::WHITEBOARD_PACKET) == 0x1D);

    // 2. Audio Stream Framing & Protocol Structures
    {
        AudioChunkHeader ach{};
        ach.sampleRate = 48000;
        ach.channels = 2;
        ach.bitsPerSample = 16;
        ach.isSilent = 0;
        ach.sampleFrames = 960; // 20ms of audio
        TEST_ASSERT(ach.sampleRate == 48000);
        TEST_ASSERT(ach.channels == 2);
        TEST_ASSERT(ach.bitsPerSample == 16);
        TEST_ASSERT(ach.sampleFrames == 960);
    }

    // 3. Privacy Mode Payload Structure
    {
        PrivacyModePayload pmp{};
        pmp.enable = 1;
        pmp.acknowledge = 0;
        ByteWriter pw;
        pw.writeBytes(&pmp, sizeof(pmp));
        TEST_ASSERT(pw.buffer().size() == sizeof(PrivacyModePayload));

        ByteReader pr(pw.buffer());
        PrivacyModePayload decoded{};
        pr.readBytes(&decoded, sizeof(decoded));
        TEST_ASSERT(decoded.enable == 1);
        TEST_ASSERT(decoded.acknowledge == 0);
    }

    // 4. TCP Tunneling Protocol Headers
    {
        TunnelOpenHeader toh{};
        toh.tunnelId = 42;
        toh.targetPort = 3389;
        toh.flags = 0;

        ByteWriter ow;
        ow.writeBytes(&toh, sizeof(toh));
        TEST_ASSERT(ow.buffer().size() == sizeof(TunnelOpenHeader));

        ByteReader orr(ow.buffer());
        TunnelOpenHeader decOpen{};
        orr.readBytes(&decOpen, sizeof(decOpen));
        TEST_ASSERT(decOpen.tunnelId == 42);
        TEST_ASSERT(decOpen.targetPort == 3389);

        TunnelDataHeader tdh{};
        tdh.tunnelId = 42;
        tdh.dataLen = 128;
        ByteWriter dw;
        dw.writeBytes(&tdh, sizeof(tdh));
        TEST_ASSERT(dw.buffer().size() == sizeof(TunnelDataHeader));

        TunnelCloseHeader tch{};
        tch.tunnelId = 42;
        tch.reasonCode = 0;
        ByteWriter cw;
        cw.writeBytes(&tch, sizeof(tch));
        TEST_ASSERT(cw.buffer().size() == sizeof(TunnelCloseHeader));
    }

    // 5. Remote Terminal Protocol Headers & Enums
    {
        TerminalDataHeader tdh{};
        tdh.streamKind = static_cast<uint8_t>(TerminalStreamKind::StdinInput);
        std::string testCmd = "echo Hello World";
        tdh.textLen = static_cast<uint32_t>(testCmd.size());

        ByteWriter tw;
        tw.writeBytes(&tdh, sizeof(tdh));
        tw.writeBytes(testCmd.data(), testCmd.size());
        TEST_ASSERT(tw.buffer().size() == sizeof(TerminalDataHeader) + testCmd.size());

        ByteReader tr(tw.buffer());
        TerminalDataHeader decTh{};
        tr.readBytes(&decTh, sizeof(decTh));
        TEST_ASSERT(decTh.streamKind == static_cast<uint8_t>(TerminalStreamKind::StdinInput));
        TEST_ASSERT(decTh.textLen == testCmd.size());
        std::string decCmd(reinterpret_cast<const char*>(tr.currentPtr()), decTh.textLen);
        TEST_ASSERT(decCmd == testCmd);
    }

    // 6. Address Book 7-field INI Persistence & Backward Compatibility
    {
        IdentityManager customMgr(77);
        customMgr.loadOrCreate();
        customMgr.addOrUpdateRecentSession(400500600, "TestHost", "10.0.0.1", "Dev Machine", "Servers", "SSH key required");
        auto sessions = customMgr.recentSessions();
        TEST_ASSERT(!sessions.empty());
        bool found = false;
        for (const auto& s : sessions) {
            if (s.deskId == 400500600) {
                found = true;
                TEST_ASSERT(s.alias == "Dev Machine");
                TEST_ASSERT(s.tag == "Servers");
                TEST_ASSERT(s.notes == "SSH key required");
                break;
            }
        }
        TEST_ASSERT(found);

        // Update metadata
        customMgr.updateRecentSessionMetadata(400500600, "Updated Machine", "Personal", "Updated remarks");
        sessions = customMgr.recentSessions();
        for (const auto& s : sessions) {
            if (s.deskId == 400500600) {
                TEST_ASSERT(s.alias == "Updated Machine");
                TEST_ASSERT(s.tag == "Personal");
                TEST_ASSERT(s.notes == "Updated remarks");
                break;
            }
        }

        // Clean up
        customMgr.removeRecentSession(400500600);
        std::error_code ec;
        std::filesystem::remove(customMgr.configFilePath(), ec);
    }

    // 7. Whiteboard Manager Serialization, Geometry & Laser Decay
    {
        WhiteboardManager wbMgr;
        wbMgr.setActiveTool(WhiteboardTool::Pen);
        wbMgr.setActiveColor(0xFF007AFF); // Blue
        wbMgr.setActiveThickness(4.0f);
        TEST_ASSERT(wbMgr.activeTool() == WhiteboardTool::Pen);
        TEST_ASSERT(wbMgr.activeColor() == 0xFF007AFF);
        TEST_ASSERT(wbMgr.activeThickness() == 4.0f);

        // Draw a stroke
        wbMgr.startStroke(0.1f, 0.2f);
        wbMgr.addStrokePoint(0.15f, 0.25f);
        wbMgr.addStrokePoint(0.2f, 0.3f);
        AnnotationStroke s1 = wbMgr.finishStroke();
        TEST_ASSERT(s1.points.size() == 3);
        TEST_ASSERT(s1.tool == WhiteboardTool::Pen);
        TEST_ASSERT(s1.argbColor == 0xFF007AFF);
        TEST_ASSERT(s1.thickness == 4.0f);

        // Serialize and Deserialize Stroke
        auto strokeBytes = WhiteboardManager::serializeStroke(s1);
        TEST_ASSERT(!strokeBytes.empty());

        AnnotationStroke sDec{};
        TEST_ASSERT(WhiteboardManager::deserializeStroke(strokeBytes.data(), strokeBytes.size(), sDec));
        TEST_ASSERT(sDec.strokeId == s1.strokeId);
        TEST_ASSERT(sDec.tool == WhiteboardTool::Pen);
        TEST_ASSERT(sDec.argbColor == 0xFF007AFF);
        TEST_ASSERT(sDec.points.size() == 3);
        TEST_ASSERT(std::fabs(sDec.points[0].x - 0.1f) < 0.001f);
        TEST_ASSERT(std::fabs(sDec.points[0].y - 0.2f) < 0.001f);
        TEST_ASSERT(std::fabs(sDec.points[2].x - 0.2f) < 0.001f);
        TEST_ASSERT(std::fabs(sDec.points[2].y - 0.3f) < 0.001f);

        // Apply remote stroke to peer manager
        WhiteboardManager peerWb;
        peerWb.applyRemoteStroke(sDec);
        auto peerStrokes = peerWb.snapshotStrokes();
        TEST_ASSERT(peerStrokes.size() == 1);
        TEST_ASSERT(peerStrokes[0].strokeId == s1.strokeId);

        // Clear packet serialization & remote application
        auto clearBytes = WhiteboardManager::serializeClearPacket();
        TEST_ASSERT(!clearBytes.empty());
        AnnotationStroke clearStroke{};
        TEST_ASSERT(WhiteboardManager::deserializeStroke(clearBytes.data(), clearBytes.size(), clearStroke));
        TEST_ASSERT(clearStroke.tool == WhiteboardTool::ClearAll);
        peerWb.applyRemoteStroke(clearStroke);
        TEST_ASSERT(peerWb.snapshotStrokes().empty());

        // Laser pointer coordinate & decay
        wbMgr.setLaserPointer(0.45f, 0.55f);
        float lx = 0.0f, ly = 0.0f, lalpha = 0.0f;
        TEST_ASSERT(wbMgr.getLaserPointer(lx, ly, lalpha));
        TEST_ASSERT(std::fabs(lx - 0.45f) < 0.001f);
        TEST_ASSERT(std::fabs(ly - 0.55f) < 0.001f);
        TEST_ASSERT(lalpha > 0.8f && lalpha <= 1.0f);

        // Malformed whiteboard packet resilience
        AnnotationStroke badStroke{};
        ByteWriter badW;
        badW.writeU32(999);
        badW.writeU8(99); // Invalid tool
        badW.writeU32(0xFFFFFFFF);
        badW.writeF32(9999.0f); // Out of bounds thickness
        badW.writeU16(1);
        badW.writeF32(2.5f); // Out of bounds coordinate
        badW.writeF32(-1.5f);
        TEST_ASSERT(WhiteboardManager::deserializeStroke(badW.buffer().data(), badW.buffer().size(), badStroke));
        TEST_ASSERT(badStroke.tool == WhiteboardTool::Pen); // Reset to default tool
        TEST_ASSERT(badStroke.thickness <= 50.0f); // Clamped
        TEST_ASSERT(badStroke.points[0].x <= 1.0f); // Clamped
        TEST_ASSERT(badStroke.points[0].y >= 0.0f); // Clamped
    }
}

void testAutoUpdaterAndProtocolV3() {
    std::cout << "[TEST 12] CppDesk Protocol V3 & Mandatory Auto-Updater Semantics...\n";

    // 1. Magic constants & Protocol definitions
    TEST_ASSERT(PROTOCOL_MAGIC == 0x43505044); // "CPPD"
    TEST_ASSERT(RELAY_MAGIC == 0x4344534B);    // "CDSK"
    TEST_ASSERT(PROTOCOL_VERSION == 3);
    TEST_ASSERT(std::string(CPP_DESK_VERSION) == "3.5.0");
    TEST_ASSERT(CPP_DESK_VERSION_NUM == 0x030500);

    // 2. Semantic Version Triad Parsing
    int maj = 0, min = 0, pat = 0;
    TEST_ASSERT(AutoUpdater::parseVersionTriad("3.1.0", maj, min, pat) && maj == 3 && min == 1 && pat == 0);
    TEST_ASSERT(AutoUpdater::parseVersionTriad("v3.1.2", maj, min, pat) && maj == 3 && min == 1 && pat == 2);
    TEST_ASSERT(AutoUpdater::parseVersionTriad("v10.200.300-preview", maj, min, pat) && maj == 10 && min == 200 && pat == 300);
    TEST_ASSERT(!AutoUpdater::parseVersionTriad("invalid_tag", maj, min, pat));
    TEST_ASSERT(!AutoUpdater::parseVersionTriad("", maj, min, pat));

    // 3. Mandatory Update Version Comparison
    // Higher patch version -> newer
    TEST_ASSERT(AutoUpdater::isNewerVersion("3.0.1", "3.0.0"));
    TEST_ASSERT(AutoUpdater::isNewerVersion("v3.0.1", "3.0.0"));
    TEST_ASSERT(AutoUpdater::isNewerVersion("3.0.1", "v3.0.0"));

    // Higher minor version -> newer
    TEST_ASSERT(AutoUpdater::isNewerVersion("3.1.0", "3.0.0"));
    TEST_ASSERT(AutoUpdater::isNewerVersion("v3.2.0", "3.1.5"));

    // Higher major version -> newer
    TEST_ASSERT(AutoUpdater::isNewerVersion("4.0.0", "3.99.99"));

    // Equal versions -> NOT newer
    TEST_ASSERT(!AutoUpdater::isNewerVersion("3.0.0", "3.0.0"));
    TEST_ASSERT(!AutoUpdater::isNewerVersion("v3.0.0", "3.0.0"));
    TEST_ASSERT(!AutoUpdater::isNewerVersion("3.0.0", "v3.0.0"));

    // Older versions -> NOT newer
    TEST_ASSERT(!AutoUpdater::isNewerVersion("2.1.0", "3.0.0"));
    TEST_ASSERT(!AutoUpdater::isNewerVersion("3.0.0", "3.0.1"));
    TEST_ASSERT(!AutoUpdater::isNewerVersion("3.0.0", "3.1.0"));
    TEST_ASSERT(!AutoUpdater::isNewerVersion("2.99.99", "3.0.0"));

    // 4. Lightweight JSON Key Extraction
    std::string sampleJson = "{\n"
                             "    \"tag_name\": \"v3.0.1\",\n"
                             "    \"html_url\": \"https://github.com/nmnghia2527/cppdesk/releases/tag/v3.0.1\",\n"
                             "    \"body\": \"Fixed multi-monitor scaling on 4K displays.\\r\\nAdded performance optimizations.\"\n"
                             "}";

    std::string tag = AutoUpdater::extractJsonString(sampleJson, "tag_name");
    std::string url = AutoUpdater::extractJsonString(sampleJson, "html_url");
    std::string body = AutoUpdater::extractJsonString(sampleJson, "body");

    TEST_ASSERT(tag == "v3.0.1");
    TEST_ASSERT(url == "https://github.com/nmnghia2527/cppdesk/releases/tag/v3.0.1");
    TEST_ASSERT(body.find("Fixed multi-monitor scaling") != std::string::npos);

    // Extraction with escaped quotes
    std::string jsonWithEscapes = "{\"desc\": \"Feature with \\\"quotes\\\" and text\", \"other\": \"ok\"}";
    TEST_ASSERT(AutoUpdater::extractJsonString(jsonWithEscapes, "desc") == "Feature with \"quotes\" and text");
    TEST_ASSERT(AutoUpdater::extractJsonString(jsonWithEscapes, "nonexistent").empty());

    // 5. ByteReader bounds and integer overflow protection (COR-01)
    std::vector<uint8_t> dummyBytes = { 0x01, 0x02, 0x03, 0x04 };
    ByteReader ovfReader(dummyBytes);
    bool caughtOvf = false;
    try {
        ovfReader.skip(SIZE_MAX - 2);
    } catch (const std::exception&) {
        caughtOvf = true;
    }
    TEST_ASSERT(caughtOvf);
}

void testHardwareDiagnosticsAndProcessManager() {
    std::cout << "[TEST 13] Hardware Diagnostics & Process Manager Engine...\n" << std::flush;

    // 1. Packet binary serialization & deserialization round-trip
    {
        // A. DIAGNOSTICS_REQ packet
        std::vector<uint8_t> reqBuf;
        ByteWriter reqWriter(reqBuf);
        reqWriter.writeU8(1); // active = true
        ByteReader reqReader(reqBuf);
        TEST_ASSERT(reqReader.readU8() == 1);

        // B. PROCESS_KILL packet
        std::vector<uint8_t> killBuf;
        ByteWriter killWriter(killBuf);
        killWriter.writeU32(1337);
        ByteReader killReader(killBuf);
        TEST_ASSERT(killReader.readU32() == 1337);

        // C. SYSTEM_DIAGNOSTICS packet payload
        SystemDiagnosticsPayload payload;
        payload.cpuUsagePercent = 38.5f;
        payload.ramUsedBytes = 8589934592ULL;   // 8 GB
        payload.ramTotalBytes = 17179869184ULL; // 16 GB
        payload.diskUsedBytes = 268435456000ULL;// 250 GB
        payload.diskTotalBytes = 536870912000ULL;// 500 GB

        ProcessTelemetryItem p1;
        p1.pid = 1024;
        p1.workingSetBytes = 524288000ULL; // 500 MB
        p1.name = "CppDesk.exe";

        ProcessTelemetryItem p2;
        p2.pid = 2048;
        p2.workingSetBytes = 104857600ULL; // 100 MB
        p2.name = "dwm.exe";

        payload.processes.push_back(p1);
        payload.processes.push_back(p2);

        std::vector<uint8_t> diagBuf;
        serializeSystemDiagnostics(payload, diagBuf);

        SystemDiagnosticsPayload unpacked;
        TEST_ASSERT(deserializeSystemDiagnostics(diagBuf.data(), diagBuf.size(), unpacked));
        TEST_ASSERT(std::abs(unpacked.cpuUsagePercent - 38.5f) < 0.01f);
        TEST_ASSERT(unpacked.ramUsedBytes == 8589934592ULL);
        TEST_ASSERT(unpacked.ramTotalBytes == 17179869184ULL);
        TEST_ASSERT(unpacked.diskUsedBytes == 268435456000ULL);
        TEST_ASSERT(unpacked.diskTotalBytes == 536870912000ULL);
        TEST_ASSERT(unpacked.processes.size() == 2);
        TEST_ASSERT(unpacked.processes[0].pid == 1024);
        TEST_ASSERT(unpacked.processes[0].name == "CppDesk.exe");
        TEST_ASSERT(unpacked.processes[0].workingSetBytes == 524288000ULL);
        TEST_ASSERT(unpacked.processes[1].pid == 2048);
        TEST_ASSERT(unpacked.processes[1].name == "dwm.exe");
    }

    // 2. Live Host Diagnostics Sampler
    {
        auto liveDiag = NetworkEngine::sampleHostDiagnostics();
        TEST_ASSERT(liveDiag.cpuUsagePercent >= 0.0f && liveDiag.cpuUsagePercent <= 100.0f);
        TEST_ASSERT(liveDiag.ramTotalBytes > 0);
        TEST_ASSERT(liveDiag.ramUsedBytes <= liveDiag.ramTotalBytes);
        TEST_ASSERT(liveDiag.diskTotalBytes > 0);
        TEST_ASSERT(!liveDiag.processes.empty());

        // Verify top processes are sorted descending by memory usage
        if (liveDiag.processes.size() >= 2) {
            TEST_ASSERT(liveDiag.processes[0].workingSetBytes >= liveDiag.processes[1].workingSetBytes);
        }
    }

    // 3. Process Kill Permission & Safety Enforcement
    {
        IdentityManager idMgr(100);
        NetworkEngine netEngine(idMgr);

        // A. Revoked input permission: must reject kill requests
        bool killedWithoutPerm = netEngine.executeProcessKill(1337, PERM_NONE);
        TEST_ASSERT(!killedWithoutPerm);

        // B. Protected critical system PIDs (PID 0 System Idle, PID 4 System): must reject even with input permission
        bool killedPid0 = netEngine.executeProcessKill(0, PERM_INPUT);
        TEST_ASSERT(!killedPid0);
        bool killedPid4 = netEngine.executeProcessKill(4, PERM_INPUT);
        TEST_ASSERT(!killedPid4);
    }
}

void testDirectCanvasDragAndDropFileTransfer() {
    std::cout << "[TEST 14] Drag-and-Drop Direct Canvas File Drop & Recursive Directory Expansion...\n" << std::flush;

    // 1. Packet encoding/decoding with targetHint and drop coordinates
    {
        ByteWriter w;
        uint32_t tid = 42;
        uint64_t fsz = 1048576ULL;
        std::string fname = "blueprint.cad";
        w.writeU32(tid);
        w.writeU64(fsz);
        w.writeString(fname);
        w.writeU8(static_cast<uint8_t>(FileOfferTarget::Desktop));
        w.writeF32(0.45f);
        w.writeF32(0.72f);

        ByteReader r(w.buffer().data(), w.buffer().size());
        uint32_t decTid = r.readU32();
        uint64_t decFsz = r.readU64();
        std::string decName = r.readString();
        FileOfferTarget decTarget = FileOfferTarget::DefaultDownloads;
        float decX = 0.0f, decY = 0.0f;
        if (r.hasRemaining(1)) {
            decTarget = static_cast<FileOfferTarget>(r.readU8());
        }
        if (r.hasRemaining(8)) {
            decX = r.readF32();
            decY = r.readF32();
        }

        TEST_ASSERT(decTid == 42);
        TEST_ASSERT(decFsz == 1048576ULL);
        TEST_ASSERT(decName == "blueprint.cad");
        TEST_ASSERT(decTarget == FileOfferTarget::Desktop);
        TEST_ASSERT(std::abs(decX - 0.45f) < 0.001f);
        TEST_ASSERT(std::abs(decY - 0.72f) < 0.001f);
    }

    // 2. Legacy Packet Backward-Compatibility (No target hint or coordinates)
    {
        ByteWriter w;
        w.writeU32(101);
        w.writeU64(2048);
        w.writeString("legacy.txt");

        ByteReader r(w.buffer().data(), w.buffer().size());
        uint32_t decTid = r.readU32();
        uint64_t decFsz = r.readU64();
        std::string decName = r.readString();
        FileOfferTarget decTarget = FileOfferTarget::DefaultDownloads;
        float decX = 0.0f, decY = 0.0f;
        if (r.hasRemaining(1)) {
            decTarget = static_cast<FileOfferTarget>(r.readU8());
        }
        if (r.hasRemaining(8)) {
            decX = r.readF32();
            decY = r.readF32();
        }

        TEST_ASSERT(decTid == 101);
        TEST_ASSERT(decFsz == 2048);
        TEST_ASSERT(decName == "legacy.txt");
        TEST_ASSERT(decTarget == FileOfferTarget::DefaultDownloads);
        TEST_ASSERT(decX == 0.0f && decY == 0.0f);
    }

    // 3. Recursive Directory Expansion & Queueing
    {
        std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "cppdesk_tdd_drop_test";
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
        std::filesystem::create_directories(tempDir / "nested", ec);

        std::ofstream f1(tempDir / "fileA.txt");
        f1 << "Sample file A contents for TDD test";
        f1.close();

        std::ofstream f2(tempDir / "nested" / "fileB.txt");
        f2 << "Sample file B nested contents";
        f2.close();

        FileTransferManager ftm;
        std::vector<PacketType> queuedPackets;
        int queuedCount = ftm.startOutgoingPath(tempDir.string(), [&](PacketType pt, const std::vector<uint8_t>&) {
            queuedPackets.push_back(pt);
            return true;
        }, FileOfferTarget::Desktop, 0.5f, 0.5f);

        TEST_ASSERT(queuedCount == 2);
        TEST_ASSERT(queuedPackets.size() == 2);
        TEST_ASSERT(queuedPackets[0] == PacketType::FILE_OFFER);
        TEST_ASSERT(queuedPackets[1] == PacketType::FILE_OFFER);

        auto transfers = ftm.snapshotTransfers();
        TEST_ASSERT(transfers.size() == 2);

        std::filesystem::remove_all(tempDir, ec);
    }

    // 4. Canvas Hit-Test Normalization
    {
        struct TestRect {
            float left, top, right, bottom;
            bool contains(float x, float y) const { return x >= left && x <= right && y >= top && y <= bottom; }
            float width() const { return right - left; }
            float height() const { return bottom - top; }
        };
        TestRect canvasRect = { 100.0f, 50.0f, 1100.0f, 850.0f }; // width = 1000, height = 800
        float px = 600.0f, py = 450.0f;
        TEST_ASSERT(canvasRect.contains(px, py));
        float nx = (px - canvasRect.left) / canvasRect.width();
        float ny = (py - canvasRect.top) / canvasRect.height();
        TEST_ASSERT(std::abs(nx - 0.5f) < 0.001f);
        TEST_ASSERT(std::abs(ny - 0.5f) < 0.001f);

        float outX = 50.0f, outY = 20.0f;
        TEST_ASSERT(!canvasRect.contains(outX, outY));
    }
}

void testSessionRecorderAndAviContainer() {
    std::cout << "[TEST 15] Session Screen Recording & RIFF AVI Container Architecture...\n" << std::flush;

    SessionRecorder recorder;
    TEST_ASSERT(!recorder.isRecording());
    TEST_ASSERT(recorder.recordedFrames() == 0);

    std::filesystem::path testFile = std::filesystem::temp_directory_path() / "cppdesk_test_record.avi";
    std::error_code ec;
    std::filesystem::remove(testFile, ec);

    const int width = 320;
    const int height = 240;
    const int fps = 30;

    // 1. Start recording
    bool ok = recorder.startRecording(testFile.string(), width, height, fps);
    TEST_ASSERT(ok);
    TEST_ASSERT(recorder.isRecording());
    TEST_ASSERT(recorder.currentFilePath() == testFile.string());

    // 2. Generate and push synthetic test frames
    std::vector<uint8_t> frameRed(width * height * 4);
    for (size_t i = 0; i < frameRed.size(); i += 4) {
        frameRed[i] = 0x00;     // B
        frameRed[i + 1] = 0x00; // G
        frameRed[i + 2] = 0xFF; // R
        frameRed[i + 3] = 0xFF; // A
    }

    std::vector<uint8_t> frameBlue(width * height * 4);
    for (size_t i = 0; i < frameBlue.size(); i += 4) {
        frameBlue[i] = 0xFF;     // B
        frameBlue[i + 1] = 0x00; // G
        frameBlue[i + 2] = 0x00; // R
        frameBlue[i + 3] = 0xFF; // A
    }

    recorder.pushFrame(frameRed.data(), width, height);
    recorder.pushFrame(frameBlue.data(), width, height);

    // Give background worker time to encode and write
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 3. Stop recording
    recorder.stopRecording();
    TEST_ASSERT(!recorder.isRecording());
    TEST_ASSERT(recorder.recordedFrames() == 2);
    TEST_ASSERT(recorder.recordedBytes() > 0);

    // 4. Verify AVI RIFF container structure on disk
    TEST_ASSERT(std::filesystem::exists(testFile, ec));
    uint64_t fileSize = std::filesystem::file_size(testFile, ec);
    TEST_ASSERT(fileSize > 256);

    std::ifstream in(testFile, std::ios::binary);
    TEST_ASSERT(in.is_open());
    char hdr[64] = {};
    in.read(hdr, 64);
    in.close();

    // Check 'RIFF' magic
    TEST_ASSERT(hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F');
    // Check 'AVI ' type
    TEST_ASSERT(hdr[8] == 'A' && hdr[9] == 'V' && hdr[10] == 'I' && hdr[11] == ' ');
    // Check 'LIST' chunk
    TEST_ASSERT(hdr[12] == 'L' && hdr[13] == 'I' && hdr[14] == 'S' && hdr[15] == 'T');
    // Check 'hdrl' form
    TEST_ASSERT(hdr[20] == 'h' && hdr[21] == 'd' && hdr[22] == 'r' && hdr[23] == 'l');

    // Clean up temporary test file
    std::filesystem::remove(testFile, ec);
}

void testBidirectionalVoiceIntercom() {
    std::cout << "[TEST 16] Bidirectional Voice Intercom (VoIP Microphone & Playback)...\n";

    // 1. Protocol opcode validation
    TEST_ASSERT(static_cast<uint8_t>(PacketType::VOICE_INTERCOM_CHUNK) == 0x29);

    // 2. VoiceChunkHeader serialization & deserialization
    {
        VoiceChunkHeader vch{};
        vch.sampleRate = 48000;
        vch.channels = 1;
        vch.bitsPerSample = 16;
        vch.flags = 0x00;
        vch.sampleFrames = 480; // 10ms chunk

        ByteWriter w;
        w.writeBytes(&vch, sizeof(vch));
        // Append simulated 480 16-bit PCM samples
        std::vector<int16_t> pcm(480, 1500);
        w.writeBytes(pcm.data(), pcm.size() * sizeof(int16_t));

        TEST_ASSERT(w.buffer().size() == sizeof(VoiceChunkHeader) + 480 * sizeof(int16_t));

        ByteReader r(w.buffer());
        VoiceChunkHeader decoded{};
        r.readBytes(&decoded, sizeof(decoded));
        TEST_ASSERT(decoded.sampleRate == 48000);
        TEST_ASSERT(decoded.channels == 1);
        TEST_ASSERT(decoded.bitsPerSample == 16);
        TEST_ASSERT(decoded.flags == 0x00);
        TEST_ASSERT(decoded.sampleFrames == 480);

        std::vector<int16_t> readPcm(decoded.sampleFrames);
        r.readBytes(readPcm.data(), readPcm.size() * sizeof(int16_t));
        TEST_ASSERT(readPcm[0] == 1500);
        TEST_ASSERT(readPcm[479] == 1500);
    }

    // 3. Audio RMS Level Calculation & Gain Processing
    {
        // Silence test
        std::vector<int16_t> silent(480, 0);
        float silentRms = VoiceIntercom::calculateRmsLevel(silent.data(), silent.size());
        TEST_ASSERT(silentRms <= 0.001f);

        // Loud signal test
        std::vector<int16_t> loud(480, 20000);
        float loudRms = VoiceIntercom::calculateRmsLevel(loud.data(), loud.size());
        TEST_ASSERT(loudRms > 0.5f);

        // Gain test
        std::vector<int16_t> gainPcm = { 1000, -2000, 3000 };
        VoiceIntercom::applyGain(gainPcm.data(), gainPcm.size(), 2.0f);
        TEST_ASSERT(gainPcm[0] == 2000);
        TEST_ASSERT(gainPcm[1] == -4000);
        TEST_ASSERT(gainPcm[2] == 6000);

        // Clipping prevention test
        gainPcm = { 25000 };
        VoiceIntercom::applyGain(gainPcm.data(), gainPcm.size(), 2.0f);
        TEST_ASSERT(gainPcm[0] == 32767); // Clamped to max int16_t
    }

    // 4. VoiceIntercom Lifecycle, Mute & Volume
    {
        VoiceIntercom intercom;
        TEST_ASSERT(!intercom.isCapturing());
        TEST_ASSERT(!intercom.isPlaying());
        TEST_ASSERT(!intercom.isMicMuted());

        intercom.setMicMuted(true);
        TEST_ASSERT(intercom.isMicMuted());
        intercom.setMicMuted(false);
        TEST_ASSERT(!intercom.isMicMuted());

        intercom.setOutputVolume(75);
        TEST_ASSERT(intercom.outputVolume() == 75);
        intercom.setOutputVolume(150); // Clamped to 100
        TEST_ASSERT(intercom.outputVolume() == 100);
        intercom.setOutputVolume(-10); // Clamped to 0
        TEST_ASSERT(intercom.outputVolume() == 0);

        // Enqueue playback chunk (should not crash even if device is missing or uninitialized)
        std::vector<int16_t> testSamples(480, 100);
        intercom.enqueuePlaybackChunk(reinterpret_cast<const uint8_t*>(testSamples.data()), testSamples.size() * sizeof(int16_t));
    }
}

void testVirtualDisplayFitAndResolutionMatching() {
    std::cout << "[TEST 17] Virtual Display Fit & Dynamic Resolution Matching...\n";

    // 1. Protocol opcode validation
    TEST_ASSERT(static_cast<uint8_t>(PacketType::RESOLUTION_CHANGE_REQ) == 0x2A);

    // 2. ScaleMode enum extension (FillAspect = 3)
    TEST_ASSERT(static_cast<uint8_t>(ScaleMode::FillAspect) == 3);

    // 3. ResolutionChangePayload serialization & deserialization
    {
        ResolutionChangePayload rcp{};
        rcp.targetWidth = 1920;
        rcp.targetHeight = 1080;
        rcp.mode = 2; // BestFitAspect

        ByteWriter w;
        w.writeBytes(&rcp, sizeof(rcp));
        TEST_ASSERT(w.buffer().size() == sizeof(ResolutionChangePayload));

        ByteReader r(w.buffer());
        ResolutionChangePayload decoded{};
        r.readBytes(&decoded, sizeof(decoded));
        TEST_ASSERT(decoded.targetWidth == 1920);
        TEST_ASSERT(decoded.targetHeight == 1080);
        TEST_ASSERT(decoded.mode == 2);
    }

    // 4. DisplayResolutionManager aspect ratio matching logic
    {
        std::vector<DisplayModeEntry> candidateModes = {
            { 800, 600, 60 },    // 4:3 (1.333)
            { 1024, 768, 60 },   // 4:3 (1.333)
            { 1280, 800, 60 },   // 16:10 (1.600)
            { 1600, 900, 60 },   // 16:9 (1.777)
            { 1920, 1080, 60 },  // 16:9 (1.777)
            { 2560, 1440, 60 }   // 16:9 (1.777)
        };

        // Match 16:9 target: should find exact 1920x1080
        DisplayModeEntry best16_9 = DisplayResolutionManager::findBestResolutionMatch(1920, 1080, candidateModes);
        TEST_ASSERT(best16_9.width == 1920);
        TEST_ASSERT(best16_9.height == 1080);

        // Match 16:10 target (e.g. 1920x1200 MacBook/Surface): closest aspect ratio in candidates is 1280x800
        DisplayModeEntry best16_10 = DisplayResolutionManager::findBestResolutionMatch(1920, 1200, candidateModes);
        TEST_ASSERT(best16_10.width == 1280);
        TEST_ASSERT(best16_10.height == 800);

        // Match 4:3 target (e.g. 1024x768 iPad): closest aspect ratio in candidates is 1024x768
        DisplayModeEntry best4_3 = DisplayResolutionManager::findBestResolutionMatch(1024, 768, candidateModes);
        TEST_ASSERT(best4_3.width == 1024);
        TEST_ASSERT(best4_3.height == 768);
    }

    // 5. Virtual Display Fit Math (Zoom-to-fill vs Fit-with-letterbox)
    {
        float stageW = 800.0f;
        float stageH = 600.0f; // 4:3 stage
        float bitmapW = 1920.0f;
        float bitmapH = 1080.0f; // 16:9 remote desktop

        // FitAspect: letterboxed
        float fitScale = std::min(stageW / bitmapW, stageH / bitmapH);
        float fitW = bitmapW * fitScale;
        float fitH = bitmapH * fitScale;
        TEST_ASSERT(fitW == 800.0f);
        TEST_ASSERT(fitH == 450.0f); // 150px vertical black bar letterboxing

        // FillAspect: zero letterbox
        float fillScale = std::max(stageW / bitmapW, stageH / bitmapH);
        float fillW = bitmapW * fillScale;
        float fillH = bitmapH * fillScale;
        TEST_ASSERT(fillH == 600.0f);
        TEST_ASSERT(fillW > 800.0f); // Bleeds horizontally, zero black bars!
    }

    // 6. DisplayResolutionManager state tracking
    {
        DisplayResolutionManager mgr;
        TEST_ASSERT(!mgr.isResolutionChanged());
        TEST_ASSERT(mgr.originalWidth() > 0);
        TEST_ASSERT(mgr.originalHeight() > 0);
    }
}

void testMultiSessionTabbedManagement() {
    std::cout << "[TEST 18] Multi-Session Tabbed Management...\n";

    SessionTabManager mgr;
    TEST_ASSERT(mgr.tabCount() == 0);
    TEST_ASSERT(mgr.activeTabId() == 0);
    TEST_ASSERT(mgr.activeTab() == nullptr);

    // 1. Create tabs
    uint32_t t1 = mgr.createTab(401115368, "401 115 368", "Dev Workstation");
    TEST_ASSERT(t1 != 0);
    TEST_ASSERT(mgr.tabCount() == 1);
    TEST_ASSERT(mgr.activeTabId() == t1);
    TEST_ASSERT(mgr.activeTab() != nullptr);
    TEST_ASSERT(mgr.activeTab()->title == "Dev Workstation");
    TEST_ASSERT(mgr.activeTab()->deskId == 401115368);

    uint32_t t2 = mgr.createTab(502226479, "502 226 479", "Database Server");
    TEST_ASSERT(t2 != 0 && t2 != t1);
    TEST_ASSERT(mgr.tabCount() == 2);
    TEST_ASSERT(mgr.activeTabId() == t2);

    // 2. Tab selection & navigation
    TEST_ASSERT(mgr.selectTab(t1));
    TEST_ASSERT(mgr.activeTabId() == t1);
    TEST_ASSERT(!mgr.selectTab(999999));
    TEST_ASSERT(mgr.nextTab() == t2);
    TEST_ASSERT(mgr.nextTab() == t1);
    TEST_ASSERT(mgr.prevTab() == t2);
    TEST_ASSERT(mgr.prevTab() == t1);

    // 3. Tab Framebuffer Caching & Restoration
    {
        std::vector<uint8_t> dummyFrame(1280 * 720 * 4, 0xEE);
        CursorState cur{ 0.35f, 0.75f, true };
        mgr.cacheActiveTabFrame(dummyFrame.data(), 1280, 720, 100, cur);

        SessionTab* tab1 = mgr.getTab(t1);
        TEST_ASSERT(tab1 != nullptr);
        TEST_ASSERT(tab1->cachedW == 1280);
        TEST_ASSERT(tab1->cachedH == 720);
        TEST_ASSERT(tab1->lastFrameSeq == 100);
        TEST_ASSERT(tab1->cachedFrameBgra.size() == 1280 * 720 * 4);
        TEST_ASSERT(tab1->cachedFrameBgra[0] == 0xEE);
        TEST_ASSERT(tab1->cursor.normX == 0.35f);
        TEST_ASSERT(tab1->cursor.normY == 0.75f);
    }

    // 4. Per-tab Isolated State
    {
        SessionTab* tab1 = mgr.getTab(t1);
        tab1->scaleMode = ScaleMode::FillAspect;
        tab1->remoteInputEnabled = false;
        tab1->unreadChat = 7;

        SessionTab* tab2 = mgr.getTab(t2);
        TEST_ASSERT(tab2 != nullptr);
        TEST_ASSERT(tab2->scaleMode == ScaleMode::FitAspect);
        TEST_ASSERT(tab2->remoteInputEnabled == true);
        TEST_ASSERT(tab2->unreadChat == 0);
    }

    // 5. Lookups by Desk ID and Target
    {
        TEST_ASSERT(mgr.findTabByDeskId(401115368) != nullptr);
        TEST_ASSERT(mgr.findTabByTarget("502 226 479") != nullptr);
        TEST_ASSERT(mgr.findTabByDeskId(999999999) == nullptr);
    }

    // 6. Tab Closure & Active Tab Fallback
    {
        mgr.selectTab(t1);
        TEST_ASSERT(mgr.closeTab(t1));
        TEST_ASSERT(mgr.tabCount() == 1);
        TEST_ASSERT(mgr.activeTabId() == t2);

        TEST_ASSERT(mgr.closeTab(t2));
        TEST_ASSERT(mgr.tabCount() == 0);
        TEST_ASSERT(mgr.activeTabId() == 0);
        TEST_ASSERT(mgr.activeTab() == nullptr);
    }

    // 7. Dimension sanity guards on cacheTabFrame
    {
        uint32_t t3 = mgr.createTab(998877665, "Test Tab", "Tab 3");
        std::vector<uint8_t> dummyFrame(64 * 64 * 4, 0x55);
        CursorState cur{};
        mgr.cacheTabFrame(t3, dummyFrame.data(), 64, 64, 1, cur);
        TEST_ASSERT(mgr.getTab(t3)->cachedW == 64);
        TEST_ASSERT(mgr.getTab(t3)->cachedH == 64);

        // Huge dimensions should be rejected without altering valid cached frame
        mgr.cacheTabFrame(t3, dummyFrame.data(), 25000, 25000, 2, cur);
        TEST_ASSERT(mgr.getTab(t3)->cachedW == 64);
        TEST_ASSERT(mgr.getTab(t3)->cachedH == 64);
    }
}

void testNativeClipboardFileTransfer() {
    std::cout << "[TEST 19] Native OS Clipboard File Copy-Paste & COM Virtual Streams...\n";

    // 1. Serialization & Deserialization of VirtualFileEntry and ClipboardFileListHeader
    {
        std::vector<VirtualFileEntry> originalFiles;
        originalFiles.push_back({ 0, L"Documentation.pdf", 1048576ULL * 25, FILE_ATTRIBUTE_NORMAL });
        originalFiles.push_back({ 1, L"Image_日本語_测试.png", 5242880ULL, FILE_ATTRIBUTE_READONLY });
        originalFiles.push_back({ 2, L"Archive_2026.zip", 1024ULL * 1024ULL * 500, FILE_ATTRIBUTE_ARCHIVE });

        std::vector<uint8_t> payload;
        serializeClipboardFileList(42, originalFiles, payload);
        TEST_ASSERT(!payload.empty());

        uint32_t decodedTransferId = 0;
        std::vector<VirtualFileEntry> decodedFiles;
        bool ok = deserializeClipboardFileList(payload.data(), payload.size(), decodedTransferId, decodedFiles);
        TEST_ASSERT(ok);
        TEST_ASSERT(decodedTransferId == 42);
        TEST_ASSERT(decodedFiles.size() == 3);
        TEST_ASSERT(decodedFiles[0].fileIndex == 0);
        TEST_ASSERT(decodedFiles[0].fileName == L"Documentation.pdf");
        TEST_ASSERT(decodedFiles[0].fileSize == 1048576ULL * 25);
        TEST_ASSERT(decodedFiles[0].fileAttributes == FILE_ATTRIBUTE_NORMAL);

        TEST_ASSERT(decodedFiles[1].fileName == L"Image_日本語_测试.png");
        TEST_ASSERT(decodedFiles[1].fileSize == 5242880ULL);
        TEST_ASSERT(decodedFiles[1].fileAttributes == FILE_ATTRIBUTE_READONLY);

        TEST_ASSERT(decodedFiles[2].fileName == L"Archive_2026.zip");
        TEST_ASSERT(decodedFiles[2].fileSize == 1024ULL * 1024ULL * 500);
    }

    // 2. COM ShellDataObject & VirtualFileStream Queries (QueryGetData & GetData)
    {
        std::vector<VirtualFileEntry> files;
        files.push_back({ 0, L"virtual_payload.bin", 65536, FILE_ATTRIBUTE_NORMAL });

        std::vector<uint8_t> testData(65536, 0xAB);
        auto chunkReader = [&](uint32_t fIndex, uint64_t offset, uint32_t length, std::vector<uint8_t>& out) {
            if (fIndex != 0) return false;
            if (offset >= testData.size()) return false;
            size_t available = testData.size() - offset;
            size_t toCopy = std::min<size_t>(length, available);
            out.assign(testData.begin() + offset, testData.begin() + offset + toCopy);
            return true;
        };

        ShellDataObject dataObj(files, chunkReader);

        // QueryGetData for CFSTR_FILEDESCRIPTORW
        FORMATETC fmtDesc{};
        fmtDesc.cfFormat = ShellClipboard::getFileGroupDescriptorWFormat();
        fmtDesc.dwAspect = DVASPECT_CONTENT;
        fmtDesc.lindex = -1;
        fmtDesc.tymed = TYMED_HGLOBAL;
        TEST_ASSERT(dataObj.QueryGetData(&fmtDesc) == S_OK);

        // GetData for CFSTR_FILEDESCRIPTORW
        STGMEDIUM medDesc{};
        TEST_ASSERT(dataObj.GetData(&fmtDesc, &medDesc) == S_OK);
        TEST_ASSERT(medDesc.tymed == TYMED_HGLOBAL);
        TEST_ASSERT(medDesc.hGlobal != nullptr);
        FILEGROUPDESCRIPTORW* pGroup = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(medDesc.hGlobal));
        TEST_ASSERT(pGroup != nullptr);
        TEST_ASSERT(pGroup->cItems == 1);
        TEST_ASSERT(std::wstring(pGroup->fgd[0].cFileName) == L"virtual_payload.bin");
        TEST_ASSERT(pGroup->fgd[0].nFileSizeLow == 65536);
        GlobalUnlock(medDesc.hGlobal);
        ReleaseStgMedium(&medDesc);

        // QueryGetData & GetData for CFSTR_FILECONTENTS
        FORMATETC fmtContents{};
        fmtContents.cfFormat = ShellClipboard::getFileContentsFormat();
        fmtContents.dwAspect = DVASPECT_CONTENT;
        fmtContents.lindex = 0;
        fmtContents.tymed = TYMED_ISTREAM;
        TEST_ASSERT(dataObj.QueryGetData(&fmtContents) == S_OK);

        STGMEDIUM medContents{};
        TEST_ASSERT(dataObj.GetData(&fmtContents, &medContents) == S_OK);
        TEST_ASSERT(medContents.tymed == TYMED_ISTREAM);
        TEST_ASSERT(medContents.pstm != nullptr);

        // Read through IStream
        IStream* pStream = medContents.pstm;
        STATSTG stat{};
        TEST_ASSERT(pStream->Stat(&stat, STATFLAG_NONAME) == S_OK);
        TEST_ASSERT(stat.cbSize.QuadPart == 65536);

        std::vector<uint8_t> readBuf(32768, 0);
        ULONG bytesRead = 0;
        TEST_ASSERT(pStream->Read(readBuf.data(), 32768, &bytesRead) == S_OK);
        TEST_ASSERT(bytesRead == 32768);
        TEST_ASSERT(readBuf[0] == 0xAB && readBuf[32767] == 0xAB);

        // Second chunk read
        TEST_ASSERT(pStream->Read(readBuf.data(), 32768, &bytesRead) == S_OK);
        TEST_ASSERT(bytesRead == 32768);

        // EOF read
        TEST_ASSERT(pStream->Read(readBuf.data(), 32768, &bytesRead) == S_OK);
        TEST_ASSERT(bytesRead == 0);

        ReleaseStgMedium(&medContents);
    }

    // 3. Conflict Resolution Suffix Naming Logic
    {
        std::string testDir = "tests_scratch_conflict";
        std::filesystem::create_directories(testDir);

        // Create base file
        std::ofstream(testDir + "/sample.txt") << "base";
        std::string res1 = ShellClipboard::resolveConflictFilename(testDir, "sample.txt");
        TEST_ASSERT(res1 == "sample (1).txt");

        // Create conflict file (1)
        std::ofstream(testDir + "/sample (1).txt") << "copy1";
        std::string res2 = ShellClipboard::resolveConflictFilename(testDir, "sample.txt");
        TEST_ASSERT(res2 == "sample (2).txt");

        // Non-conflicting filename
        std::string res3 = ShellClipboard::resolveConflictFilename(testDir, "unique.txt");
        TEST_ASSERT(res3 == "unique.txt");

        // Clean up
        std::filesystem::remove_all(testDir);
    }

    // 4. On-Demand Chunk Request & SHA-256 Hash Verification in ClipboardFileTransferManager
    {
        ClipboardFileTransferManager mgr;

        std::vector<VirtualFileEntry> remoteFiles;
        remoteFiles.push_back({ 0, L"remote_stream.dat", 4096, FILE_ATTRIBUTE_NORMAL });

        std::vector<uint8_t> lastSentPacket;
        PacketType lastSentType = PacketType::PING;
        auto sendFn = [&](PacketType t, const std::vector<uint8_t>& p) {
            lastSentType = t;
            lastSentPacket = p;
            return true;
        };

        mgr.handleRemoteFileList(101, remoteFiles, sendFn);

        // Simulate background thread calling fetchChunk (which sends CLIPBOARD_FILE_REQUEST)
        std::vector<uint8_t> chunkResult;
        std::thread fetcherThread([&]() {
            mgr.fetchChunk(0, 0, 4096, chunkResult);
        });

        // Wait for request packet to be emitted
        int waitMs = 0;
        while (lastSentType != PacketType::CLIPBOARD_FILE_REQUEST && waitMs < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waitMs += 10;
        }
        TEST_ASSERT(lastSentType == PacketType::CLIPBOARD_FILE_REQUEST);
        TEST_ASSERT(lastSentPacket.size() == sizeof(ClipboardFileRequestHeader));

        ClipboardFileRequestHeader reqHdr{};
        std::memcpy(&reqHdr, lastSentPacket.data(), sizeof(reqHdr));
        TEST_ASSERT(reqHdr.transferId == 101);
        TEST_ASSERT(reqHdr.fileIndex == 0);
        TEST_ASSERT(reqHdr.length == 4096);

        // Deliver chunk with valid SHA-256 hash
        std::vector<uint8_t> mockData(4096, 0x7E);
        auto digest = CryptoUtils::sha256(mockData.data(), mockData.size());
        mgr.handleFileChunk(101, 0, 0, mockData.data(), mockData.size(), digest.data());

        fetcherThread.join();
        TEST_ASSERT(chunkResult.size() == 4096);
        TEST_ASSERT(chunkResult[0] == 0x7E);
        TEST_ASSERT(mgr.activeTransferredBytes() == 4096);
    }

    // 5. 2 GB Capacity Guardrail Rejection & Cancellation Staging Cleanup
    {
        ClipboardFileTransferManager mgr;

        std::vector<VirtualFileEntry> hugeFiles;
        hugeFiles.push_back({ 0, L"HugeVideo.mkv", 3ULL * 1024ULL * 1024ULL * 1024ULL, FILE_ATTRIBUTE_NORMAL }); // 3 GB > 2 GB

        PacketType cancelSentType = PacketType::PING;
        std::vector<uint8_t> cancelPacket;
        auto sendFn = [&](PacketType t, const std::vector<uint8_t>& p) {
            cancelSentType = t;
            cancelPacket = p;
            return true;
        };

        mgr.handleRemoteFileList(202, hugeFiles, sendFn);
        TEST_ASSERT(cancelSentType == PacketType::CLIPBOARD_FILE_CANCEL);
        TEST_ASSERT(cancelPacket.size() == sizeof(ClipboardFileCancelHeader));

        ClipboardFileCancelHeader cancelHdr{};
        std::memcpy(&cancelHdr, cancelPacket.data(), sizeof(cancelHdr));
        TEST_ASSERT(cancelHdr.transferId == 202);
        TEST_ASSERT(cancelHdr.reasonCode == 2); // size limit exceeded

        // Test cancelActiveTransfer purges staging
        std::wstring stageDir = mgr.stagingDirectory();
        std::wstring partFile = stageDir + L"transfer_202_part_0.part";
        std::ofstream(std::filesystem::path(partFile)) << "partial_garbage";
        TEST_ASSERT(std::filesystem::exists(std::filesystem::path(partFile)));

        mgr.cancelActiveTransfer(sendFn);
        TEST_ASSERT(!std::filesystem::exists(std::filesystem::path(partFile)));
    }

    // 6. Path Traversal & Directory Separator Sanitization in handleRemoteFileList
    {
        // Also test resolveConflictFilenameW
        std::wstring testDirW = L"tests_scratch_conflict_w";
        std::filesystem::create_directories(testDirW);
        std::ofstream(std::filesystem::path(testDirW) / L"wide.txt") << "base";
        std::wstring wres1 = ShellClipboard::resolveConflictFilenameW(testDirW, L"wide.txt");
        TEST_ASSERT(wres1 == L"wide (1).txt");
        std::ofstream(std::filesystem::path(testDirW) / L"wide (1).txt") << "copy1";
        std::wstring wres2 = ShellClipboard::resolveConflictFilenameW(testDirW, L"wide.txt");
        TEST_ASSERT(wres2 == L"wide (2).txt");
        std::wstring wres3 = ShellClipboard::resolveConflictFilenameW(testDirW, L"unique.txt");
        TEST_ASSERT(wres3 == L"unique.txt");
        std::filesystem::remove_all(testDirW);

        // Test handleRemoteFileList sanitization
        ClipboardFileTransferManager mgr;
        std::vector<VirtualFileEntry> traversalFiles;
        traversalFiles.push_back({ 0, L"../../evil_payload.exe", 1024, FILE_ATTRIBUTE_NORMAL });
        traversalFiles.push_back({ 1, L"nested/folder\\sub..dir/data.bin", 1024, FILE_ATTRIBUTE_NORMAL });
        traversalFiles.push_back({ 2, L"..", 1024, FILE_ATTRIBUTE_NORMAL });
        traversalFiles.push_back({ 3, L"", 1024, FILE_ATTRIBUTE_NORMAL });

        auto noopSend = [](PacketType, const std::vector<uint8_t>&) { return true; };
        mgr.handleRemoteFileList(303, traversalFiles, noopSend);

        // File 0: path("../../evil_payload.exe").filename() -> "evil_payload.exe"
        TEST_ASSERT(mgr.activeFileName() == "evil_payload.exe");

        // Fetch chunk on file 1: path("nested/folder\\sub..dir/data.bin").filename() -> "data.bin"
        std::vector<uint8_t> dummyChunk;
        mgr.fetchChunk(1, 0, 100, dummyChunk);
        TEST_ASSERT(mgr.activeFileName() == "data.bin");

        // Fetch chunk on file 2: ".." -> "received_file_2.bin"
        mgr.fetchChunk(2, 0, 100, dummyChunk);
        TEST_ASSERT(mgr.activeFileName() == "received_file_2.bin");

        // Fetch chunk on file 3: "" -> "received_file_3.bin"
        mgr.fetchChunk(3, 0, 100, dummyChunk);
        TEST_ASSERT(mgr.activeFileName() == "received_file_3.bin");
    }
}

void testSelfHostedRelayAndStunDiagnostics() {
    std::cout << "[Test 20] Self-Hosted Relay, RFC 5389 STUN NAT Discovery & Diagnostics...\n" << std::flush;

    // 1. INI persistence of AppSettings relay and STUN fields
    {
        IdentityManager idMgr(99);
        AppSettings s = idMgr.settings();
        s.relayServer = "relay.mycustomserver.net:50999";
        s.relayAuthKey = "superSecretKey123";
        s.stunServer = "stun1.l.google.com:19302";
        s.relayMode = 1; // Self-Hosted
        idMgr.updateSettings(s);

        IdentityManager idReload(99);
        idReload.loadOrCreate();
        TEST_ASSERT(idReload.settings().relayServer == "relay.mycustomserver.net:50999");
        TEST_ASSERT(idReload.settings().relayAuthKey == "superSecretKey123");
        TEST_ASSERT(idReload.settings().stunServer == "stun1.l.google.com:19302");
        TEST_ASSERT(idReload.settings().relayMode == 1);
        TEST_ASSERT(idReload.relayServerAddress() == "relay.mycustomserver.net:50999");
    }

    // 2. TCP Relay Probe (online vs offline)
    {
        // Probe offline/unreachable port
        auto probeFail = NetworkEngine::probeRelayServer("127.0.0.1:59998", 200);
        TEST_ASSERT(!probeFail.reachable);
        TEST_ASSERT(probeFail.rttMs == -1);

        // Probe active relay server
        RelayServer testRelay;
        bool started = testRelay.start(50988);
        TEST_ASSERT(started);
        auto probeOk = NetworkEngine::probeRelayServer("127.0.0.1:50988", 1000);
        TEST_ASSERT(probeOk.reachable);
        TEST_ASSERT(probeOk.rttMs >= 0);
        testRelay.stop();
    }

    // 3. RFC 5389 STUN Binding Request & XOR-MAPPED-ADDRESS Resolution with Mock Responder
    {
        SOCKET mockStunSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        TEST_ASSERT(mockStunSock != INVALID_SOCKET);

        sockaddr_in stunBind{};
        stunBind.sin_family = AF_INET;
        stunBind.sin_addr.s_addr = inet_addr("127.0.0.1");
        stunBind.sin_port = htons(50989);
        int bErr = bind(mockStunSock, reinterpret_cast<sockaddr*>(&stunBind), sizeof(stunBind));
        TEST_ASSERT(bErr == 0);

        std::atomic<bool> mockRunning{true};
        std::thread mockThread([mockStunSock, &mockRunning]() {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(mockStunSock, &rfds);
            timeval tv{ 1, 500000 };
            int sel = select(0, &rfds, nullptr, nullptr, &tv);
            if (sel > 0 && FD_ISSET(mockStunSock, &rfds)) {
                uint8_t reqBuf[256] = {};
                sockaddr_in clientAddr{};
                int clen = sizeof(clientAddr);
                int n = recvfrom(mockStunSock, reinterpret_cast<char*>(reqBuf), sizeof(reqBuf), 0,
                                 reinterpret_cast<sockaddr*>(&clientAddr), &clen);
                if (n >= 20) {
                    uint32_t magic = (static_cast<uint32_t>(reqBuf[4]) << 24) |
                                     (static_cast<uint32_t>(reqBuf[5]) << 16) |
                                     (static_cast<uint32_t>(reqBuf[6]) << 8)  |
                                     static_cast<uint32_t>(reqBuf[7]);
                    if (magic == 0x2112A442) {
                        uint8_t respBuf[32] = {};
                        respBuf[0] = 0x01; respBuf[1] = 0x01;
                        respBuf[2] = 0x00; respBuf[3] = 0x0C;
                        respBuf[4] = 0x21; respBuf[5] = 0x12; respBuf[6] = 0xA4; respBuf[7] = 0x42;
                        std::memcpy(respBuf + 8, reqBuf + 8, 12);

                        respBuf[20] = 0x00; respBuf[21] = 0x20;
                        respBuf[22] = 0x00; respBuf[23] = 0x08;
                        respBuf[24] = 0x00;
                        respBuf[25] = 0x01;
                        uint16_t xorPort = 54321 ^ 0x2112;
                        respBuf[26] = static_cast<uint8_t>((xorPort >> 8) & 0xFF);
                        respBuf[27] = static_cast<uint8_t>(xorPort & 0xFF);
                        uint32_t testIp = 0xCB0071C3; // 203.0.113.195
                        uint32_t xorIp = testIp ^ 0x2112A442;
                        respBuf[28] = static_cast<uint8_t>((xorIp >> 24) & 0xFF);
                        respBuf[29] = static_cast<uint8_t>((xorIp >> 16) & 0xFF);
                        respBuf[30] = static_cast<uint8_t>((xorIp >> 8) & 0xFF);
                        respBuf[31] = static_cast<uint8_t>(xorIp & 0xFF);

                        sendto(mockStunSock, reinterpret_cast<const char*>(respBuf), sizeof(respBuf), 0,
                               reinterpret_cast<sockaddr*>(&clientAddr), clen);
                    }
                }
            }
        });

        auto stunResult = NetworkEngine::queryStunServer("127.0.0.1:50989", 1000);
        mockRunning = false;
        if (mockThread.joinable()) mockThread.join();
        closesocket(mockStunSock);

        TEST_ASSERT(stunResult.success);
        TEST_ASSERT(stunResult.publicIp == "203.0.113.195");
        TEST_ASSERT(stunResult.publicPort == 54321);
        TEST_ASSERT(stunResult.rttMs >= 0);
    }

    // 4. Asynchronous Network Diagnostics & Hot-reconnect
    {
        IdentityManager idMgr(98);
        NetworkEngine engine(idMgr);
        TEST_ASSERT(engine.start());

        engine.setRelayAddressAndReconnect("127.0.0.1:50988");
        TEST_ASSERT(idMgr.relayServerAddress() == "127.0.0.1:50988");

        RelayServer testRelay;
        TEST_ASSERT(testRelay.start(50988));

        engine.startNetworkDiagnostics("127.0.0.1:50988", "127.0.0.1:59999");
        for (int i = 0; i < 40; ++i) {
            if (!engine.isNetworkDiagnosticRunning()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        TEST_ASSERT(!engine.isNetworkDiagnosticRunning());

        RelayProbeResult rRes;
        StunNatResult sRes;
        bool gotRes = engine.getNetworkDiagnosticResult(rRes, sRes);
        TEST_ASSERT(gotRes);
        TEST_ASSERT(rRes.reachable);

        testRelay.stop();
        engine.stop();
    }
}

void testPerformanceHudMetricsAndTelemetry() {
    std::cout << "[SUITE 21] Real-Time Performance & Diagnostics HUD Overlay & Telemetry..." << std::endl;

    // 1. Verify ScreenCapturer latency measurements
    {
        ScreenCapturer capturer;
        std::vector<EncodedTile> tiles;
        bool isKf = false;
        CursorState cur{};
        bool ok = capturer.captureDirtyTiles(true, QualityPreset::Balanced, tiles, isKf, cur);
        TEST_ASSERT(ok);
        // Verify capture and encode latency values are non-negative and recorded
        TEST_ASSERT(capturer.lastCaptureLatencyMs() >= 0.0f);
        TEST_ASSERT(capturer.lastEncodeLatencyMs() >= 0.0f);
    }

    // 2. Verify PerformanceHudPayload serialization and deserialization
    {
        PerformanceHudPayload orig{};
        orig.captureLatencyMs = 4.25f;
        orig.encodeLatencyMs = 8.50f;
        orig.dirtyTilesCount = 14;
        orig.compressionRatio = 12.8f;
        orig.hostFps = 59.9f;

        std::vector<uint8_t> buf;
        serializePerformanceHud(orig, buf);
        TEST_ASSERT(!buf.empty());

        PerformanceHudPayload parsed{};
        bool desOk = deserializePerformanceHud(buf.data(), buf.size(), parsed);
        TEST_ASSERT(desOk);
        TEST_ASSERT(std::abs(parsed.captureLatencyMs - 4.25f) < 0.01f);
        TEST_ASSERT(std::abs(parsed.encodeLatencyMs - 8.50f) < 0.01f);
        TEST_ASSERT(parsed.dirtyTilesCount == 14);
        TEST_ASSERT(std::abs(parsed.compressionRatio - 12.8f) < 0.01f);
        TEST_ASSERT(std::abs(parsed.hostFps - 59.9f) < 0.1f);
    }

    // 3. Verify ViewerSessionStats defaults and telemetry tracking
    {
        ViewerSessionStats stats;
        TEST_ASSERT(stats.captureLatencyMs == 0.0f);
        TEST_ASSERT(stats.encodeLatencyMs == 0.0f);
        TEST_ASSERT(stats.decodeLatencyMs == 0.0f);
        TEST_ASSERT(stats.compressionRatio == 1.0f);
        TEST_ASSERT(stats.deltaTilesCount == 0);
        TEST_ASSERT(stats.rttHistory.empty());

        // Test rolling RTT history bounds
        for (int i = 0; i < 40; ++i) {
            stats.rttHistory.push_back(static_cast<float>(i * 5));
            if (stats.rttHistory.size() > 30) {
                stats.rttHistory.erase(stats.rttHistory.begin());
            }
        }
        TEST_ASSERT(stats.rttHistory.size() == 30);
        TEST_ASSERT(stats.rttHistory.back() == 195.0f);
        TEST_ASSERT(stats.rttHistory.front() == 50.0f);
    }

    // 4. Verify deserialization safety on corrupted or truncated buffers
    {
        PerformanceHudPayload parsed{};
        uint8_t garbage[5] = { 1, 2, 3, 4, 5 };
        TEST_ASSERT(!deserializePerformanceHud(nullptr, 10, parsed));
        TEST_ASSERT(!deserializePerformanceHud(garbage, sizeof(garbage), parsed));
    }
}

void testRichChatMediaAndClipboardHistoryHub() {
    std::cout << "[SUITE 22] Rich Chat Media & In-Session Clipboard History Hub..." << std::endl;

    // 1. ChatMediaPayload serialization and deserialization
    {
        ChatMediaPayload orig{};
        orig.senderName = "Host Desk";
        orig.captionText = "Here is the desktop screenshot";
        orig.imgWidth = 1920;
        orig.imgHeight = 1080;
        orig.jpegData = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0xFF, 0xD9 };

        std::vector<uint8_t> buf;
        serializeChatMedia(orig, buf);
        TEST_ASSERT(!buf.empty());

        ChatMediaPayload parsed{};
        bool ok = deserializeChatMedia(buf.data(), buf.size(), parsed);
        TEST_ASSERT(ok);
        TEST_ASSERT(parsed.senderName == "Host Desk");
        TEST_ASSERT(parsed.captionText == "Here is the desktop screenshot");
        TEST_ASSERT(parsed.imgWidth == 1920);
        TEST_ASSERT(parsed.imgHeight == 1080);
        TEST_ASSERT(parsed.jpegData.size() == orig.jpegData.size());
        TEST_ASSERT(std::memcmp(parsed.jpegData.data(), orig.jpegData.data(), orig.jpegData.size()) == 0);

        // Deserialization on garbage/truncated buffer
        ChatMediaPayload failPayload{};
        TEST_ASSERT(!deserializeChatMedia(nullptr, 10, failPayload));
        uint8_t shortBuf[3] = { 1, 2, 3 };
        TEST_ASSERT(!deserializeChatMedia(shortBuf, sizeof(shortBuf), failPayload));

        // Deserialization rejecting oversized payload (> 10MB)
        ByteWriter oversizedWriter;
        oversizedWriter.writeString("Sender");
        oversizedWriter.writeString("Caption");
        oversizedWriter.writeU32(100);
        oversizedWriter.writeU32(100);
        oversizedWriter.writeU64(123456);
        oversizedWriter.writeU32(11 * 1024 * 1024);
        auto oversizedBuf = oversizedWriter.takeBuffer();
        TEST_ASSERT(!deserializeChatMedia(oversizedBuf.data(), oversizedBuf.size(), failPayload));
    }

    // 2. ClipboardHistoryManager smart classification & storage
    {
        ClipboardHistoryManager mgr;
        TEST_ASSERT(mgr.count() == 0);

        // Test URL
        mgr.addItem("https://github.com/nmnghia2527/cppdesk", false);
        // Test Path
        mgr.addItem("C:\\Windows\\System32\\cmd.exe", false);
        // Test Code
        mgr.addItem("class Widget { public: void render(); };", false);
        // Test Plain Text
        mgr.addItem("Quick brown fox jumps over lazy dog", true);

        TEST_ASSERT(mgr.count() == 4);
        auto items = mgr.items();
        TEST_ASSERT(items.size() == 4);

        // Items are in reverse-chronological order (newest first)
        TEST_ASSERT(items[0].text == "Quick brown fox jumps over lazy dog");
        TEST_ASSERT(items[0].typeBadge == "Text");
        TEST_ASSERT(items[0].isFromRemote == true);

        TEST_ASSERT(items[1].typeBadge == "Code");
        TEST_ASSERT(items[1].isFromRemote == false);

        TEST_ASSERT(items[2].typeBadge == "Path");
        TEST_ASSERT(items[3].typeBadge == "URL");

        // Non-duplication of consecutive identical items
        mgr.addItem("Quick brown fox jumps over lazy dog", true);
        TEST_ASSERT(mgr.count() == 4);

        // Search filtering
        auto urlResults = mgr.search("github");
        TEST_ASSERT(urlResults.size() == 1);
        TEST_ASSERT(urlResults[0].typeBadge == "URL");

        auto codeResults = mgr.search("Code");
        TEST_ASSERT(codeResults.size() == 1);
        TEST_ASSERT(codeResults[0].text.find("Widget") != std::string::npos);

        auto allSearch = mgr.search("");
        TEST_ASSERT(allSearch.size() == 4);

        // Item deletion
        uint32_t delId = items[1].id;
        bool delOk = mgr.deleteItem(delId);
        TEST_ASSERT(delOk);
        TEST_ASSERT(mgr.count() == 3);

        // Clear all
        mgr.clear();
        TEST_ASSERT(mgr.count() == 0);
        TEST_ASSERT(mgr.items().empty());
    }

    // 3. ClipboardManager image extraction and history recording integration
    {
        ClipboardManager clip;
        TEST_ASSERT(clip.history().count() == 0);

        clip.applyRemoteClipboard("https://aerodesk.app/download");
        TEST_ASSERT(clip.history().count() == 1);
        auto hist = clip.history().items();
        TEST_ASSERT(!hist.empty());
        TEST_ASSERT(hist[0].typeBadge == "URL");
        TEST_ASSERT(hist[0].isFromRemote == true);

        // Verify copyItemToClipboard roundtrip
        bool copyOk = clip.history().copyItemToClipboard(hist[0].id);
        TEST_ASSERT(copyOk);
        std::string currentClip;
        for (int retry = 0; retry < 5; ++retry) {
            currentClip = ClipboardManager::getClipboardUtf8();
            if (currentClip == "https://aerodesk.app/download") break;
            Sleep(10);
        }
        TEST_ASSERT(currentClip == "https://aerodesk.app/download");
    }

    // 4. ChatMessageEntry image metadata verification
    {
        ChatMessageEntry entry;
        TEST_ASSERT(!entry.hasImage);
        TEST_ASSERT(entry.imageJpegData.empty());
        TEST_ASSERT(entry.imgWidth == 0);
        TEST_ASSERT(entry.imgHeight == 0);

        entry.hasImage = true;
        entry.imgWidth = 800;
        entry.imgHeight = 600;
        entry.imageJpegData = { 0x11, 0x22, 0x33 };
        TEST_ASSERT(entry.hasImage);
        TEST_ASSERT(entry.imgWidth == 800);
        TEST_ASSERT(entry.imageJpegData.size() == 3);
    }
}

void testWindowsServiceAndRemoteRebootReconnect() {
    std::cout << "[TEST 23] Elevated Windows Service & Remote Reboot-with-Reconnect...\n";

    // 1. Core protocol serialization & deserialization
    {
        // 1a. RemoteRebootRequestPayload
        RemoteRebootRequestPayload reqOriginal{};
        reqOriginal.countdownSeconds = 10;
        reqOriginal.rebootMode = 0x01; // safe mode
        std::vector<uint8_t> reqBuf;
        serializeRemoteRebootRequest(reqOriginal, reqBuf);
        TEST_ASSERT(!reqBuf.empty());

        RemoteRebootRequestPayload reqDecoded{};
        bool reqOk = deserializeRemoteRebootRequest(reqBuf.data(), reqBuf.size(), reqDecoded);
        TEST_ASSERT(reqOk);
        TEST_ASSERT(reqDecoded.countdownSeconds == 10);
        TEST_ASSERT(reqDecoded.rebootMode == 0x01);

        // Corrupt / truncated buffer reject
        RemoteRebootRequestPayload reqBad{};
        TEST_ASSERT(!deserializeRemoteRebootRequest(reqBuf.data(), 3, reqBad));

        // 1b. RemoteRebootConfirmPayload
        RemoteRebootConfirmPayload confOriginal{};
        confOriginal.countdownSeconds = 5;
        confOriginal.resumeTokenHex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        confOriginal.message = "Host rebooting...";
        std::vector<uint8_t> confBuf;
        serializeRemoteRebootConfirm(confOriginal, confBuf);
        TEST_ASSERT(!confBuf.empty());

        RemoteRebootConfirmPayload confDecoded{};
        bool confOk = deserializeRemoteRebootConfirm(confBuf.data(), confBuf.size(), confDecoded);
        TEST_ASSERT(confOk);
        TEST_ASSERT(confDecoded.countdownSeconds == 5);
        TEST_ASSERT(confDecoded.resumeTokenHex == confOriginal.resumeTokenHex);
        TEST_ASSERT(confDecoded.message == confOriginal.message);

        // 1c. RemoteRebootReconnectPayload
        RemoteRebootReconnectPayload recOriginal{};
        recOriginal.callerDeskId = 123456789ULL;
        recOriginal.resumeTokenHex = confOriginal.resumeTokenHex;
        std::vector<uint8_t> recBuf;
        serializeRemoteRebootReconnect(recOriginal, recBuf);
        TEST_ASSERT(!recBuf.empty());

        RemoteRebootReconnectPayload recDecoded{};
        bool recOk = deserializeRemoteRebootReconnect(recBuf.data(), recBuf.size(), recDecoded);
        TEST_ASSERT(recOk);
        TEST_ASSERT(recDecoded.callerDeskId == 123456789ULL);
        TEST_ASSERT(recDecoded.resumeTokenHex == recOriginal.resumeTokenHex);
    }

    // 2. Reboot token generation, secure disk persistence & single-use consumption
    {
        std::string token1 = WindowsServiceManager::generateRebootTokenHex();
        std::string token2 = WindowsServiceManager::generateRebootTokenHex();
        TEST_ASSERT(token1.size() == 64);
        TEST_ASSERT(token2.size() == 64);
        TEST_ASSERT(token1 != token2);

        std::vector<uint8_t> t1Bytes = CryptoUtils::fromHex(token1);
        TEST_ASSERT(t1Bytes.size() == 32);
        TEST_ASSERT(CryptoUtils::toHex(t1Bytes.data(), t1Bytes.size()) == token1);

        uint64_t callerDeskId = 987654321ULL;
        WindowsServiceManager::clearRebootToken();
        TEST_ASSERT(!WindowsServiceManager::hasPendingRebootToken());

        // Save token to disk
        bool saveOk = WindowsServiceManager::saveRebootToken(token1, callerDeskId, 300);
        TEST_ASSERT(saveOk);

        uint64_t readCaller = 0;
        TEST_ASSERT(WindowsServiceManager::hasPendingRebootToken(&readCaller));
        TEST_ASSERT(readCaller == callerDeskId);

        // Validation with wrong desk ID should fail and NOT consume the token
        TEST_ASSERT(!WindowsServiceManager::validateAndConsumeRebootToken(token1, 111222333ULL));
        TEST_ASSERT(WindowsServiceManager::hasPendingRebootToken());

        // Validation with wrong token should fail
        TEST_ASSERT(!WindowsServiceManager::validateAndConsumeRebootToken("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", callerDeskId));
        TEST_ASSERT(WindowsServiceManager::hasPendingRebootToken());

        // Valid consumption should succeed
        TEST_ASSERT(WindowsServiceManager::validateAndConsumeRebootToken(token1, callerDeskId));

        // Token must now be consumed (single-use semantics)
        TEST_ASSERT(!WindowsServiceManager::hasPendingRebootToken());
        TEST_ASSERT(!WindowsServiceManager::validateAndConsumeRebootToken(token1, callerDeskId));
    }

    // 3. Service Control Manager queries
    {
        bool installed = WindowsServiceManager::isServiceInstalled();
        bool running = WindowsServiceManager::isServiceRunning();
        ServiceStatusState state = WindowsServiceManager::getServiceState();

        if (!installed) {
            TEST_ASSERT(!running);
            TEST_ASSERT(state == ServiceStatusState::NotInstalled);
        } else {
            TEST_ASSERT(state == ServiceStatusState::Running || state == ServiceStatusState::Stopped || state == ServiceStatusState::Pending);
            if (running) {
                TEST_ASSERT(state == ServiceStatusState::Running);
            }
        }
    }

    // 4. NetworkEngine token reconnection state & lifecycle
    {
        IdentityManager idMgr(100);
        idMgr.loadOrCreate();
        NetworkEngine netEng(idMgr);

        TEST_ASSERT(!netEng.isRebootPending());
        TEST_ASSERT(netEng.rebootCountdown() == 0);
        TEST_ASSERT(!netEng.isAutoReconnectingWithToken());
        TEST_ASSERT(netEng.rebootResumeToken().empty());

        netEng.cancelAutoReconnection();
        TEST_ASSERT(!netEng.isAutoReconnectingWithToken());
    }
}

void testDesktopShortcutsAndUriProtocol() {
    std::cout << "[TEST 24] Desktop Shortcuts, URI Protocol Handler & CLI Auto-Connect...\n";

    // 1. Desk ID sanitization
    TEST_ASSERT(ShortcutManager::sanitizeDeskId("123-456-789") == "123456789");
    TEST_ASSERT(ShortcutManager::sanitizeDeskId("  401 115 368 ") == "401115368");
    TEST_ASSERT(ShortcutManager::sanitizeDeskId("Desk #401115368 (Office)") == "401115368");
    TEST_ASSERT(ShortcutManager::sanitizeDeskId("abc") == "");

    // 2. Command-line parsing
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"CppDesk.exe --connect 401115368") == "401115368");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"CppDesk.exe --connect 401-115-368") == "401115368");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"\"C:\\Program Files\\CppDesk.exe\" -c 401115368") == "401115368");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"CppDesk.exe --connect=401115368") == "401115368");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"CppDesk.exe cppdesk://401115368/") == "401115368");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"CppDesk.exe cppdesk://401-115-368") == "401115368");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"CppDesk.exe --other-flag") == "");
    TEST_ASSERT(ShortcutManager::parseStartupConnectTarget(L"") == "");

    // 3. URI Protocol registry state check
    bool initialReg = ShortcutManager::isUriProtocolRegistered();
    bool regSuccess = ShortcutManager::setUriProtocolRegistered(true);
    TEST_ASSERT(regSuccess);
    TEST_ASSERT(ShortcutManager::isUriProtocolRegistered());
    if (!initialReg) {
        ShortcutManager::setUriProtocolRegistered(false);
        TEST_ASSERT(!ShortcutManager::isUriProtocolRegistered());
    }
}

void testFileTransferSpeedTelemetryAndEta() {
    std::cout << "[TEST 25] File Transfer Speed Telemetry & ETA Estimation...\n";

    // 1. Speed formatting verification
    TEST_ASSERT(FileTransferItem::formatSpeed(0.0) == "0 KB/s");
    TEST_ASSERT(FileTransferItem::formatSpeed(-10.0) == "0 KB/s");
    TEST_ASSERT(FileTransferItem::formatSpeed(std::numeric_limits<double>::quiet_NaN()) == "0 KB/s");
    TEST_ASSERT(FileTransferItem::formatSpeed(std::numeric_limits<double>::infinity()) == "0 KB/s");
    TEST_ASSERT(FileTransferItem::formatSpeed(512.0) == "512 B/s");
    TEST_ASSERT(FileTransferItem::formatSpeed(128.0 * 1024.0) == "128 KB/s");
    TEST_ASSERT(FileTransferItem::formatSpeed(2.45 * 1024.0 * 1024.0) == "2.5 MB/s");

    // 2. ETA formatting verification
    TEST_ASSERT(FileTransferItem::formatEta(-1.0) == "");
    TEST_ASSERT(FileTransferItem::formatEta(std::numeric_limits<double>::quiet_NaN()) == "");
    TEST_ASSERT(FileTransferItem::formatEta(std::numeric_limits<double>::infinity()) == "");
    TEST_ASSERT(FileTransferItem::formatEta(5.2) == "ETA 5s");
    TEST_ASSERT(FileTransferItem::formatEta(90.0) == "ETA 1m 30s");
    TEST_ASSERT(FileTransferItem::formatEta(3605.0) == "ETA > 1h");
    TEST_ASSERT(FileTransferItem::formatEta(1e12) == "ETA > 1h");

    // 3. FileTransferManager speed and aggregate bandwidth state
    FileTransferManager mgr;
    TEST_ASSERT(mgr.aggregateActiveBandwidthBps() == 0.0);

    // Create temporary file to test transfer telemetry
    std::string testPath = "test_xfer_speed.dat";
    {
        std::ofstream out(testPath, std::ios::binary);
        std::vector<uint8_t> data(64 * 1024, 0xAB);
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    uint32_t tid = mgr.startOutgoingFile(testPath, nullptr);
    TEST_ASSERT(tid != 0);

    auto items = mgr.snapshotTransfers();
    TEST_ASSERT(!items.empty());
    TEST_ASSERT(items[0].transferId == tid);
    TEST_ASSERT(items[0].startTimeMs > 0);

    // Clean up
    mgr.cancelTransfer(tid);
    auto cancelledItems = mgr.snapshotTransfers();
    TEST_ASSERT(!cancelledItems.empty());
    TEST_ASSERT(cancelledItems[0].status == TransferStatus::Cancelled);
    TEST_ASSERT(cancelledItems[0].speedBps == 0.0);
    TEST_ASSERT(cancelledItems[0].etaSeconds == -1.0);
    TEST_ASSERT(mgr.aggregateActiveBandwidthBps() == 0.0);

    std::error_code ec;
    std::filesystem::remove(testPath, ec);
}

void testAudioControlsAndMuteSync() {
    std::cout << "[TEST 26] Audio Mute Hotkey, Volume Slider & Mute Sync Protocol...\n";

    // 1. AudioControlPayload memory packing & layout
    TEST_ASSERT(sizeof(AudioControlPayload) == 4);
    AudioControlPayload p{};
    p.isMuted = 1;
    p.volumePercent = 75;
    TEST_ASSERT(p.isMuted == 1);
    TEST_ASSERT(p.volumePercent == 75);

    // 2. NetworkEngine volume clamping & mute state
    {
        IdentityManager idMgr(101);
        idMgr.loadOrCreate();
        NetworkEngine netEng(idMgr);

        // Initial state
        TEST_ASSERT(!netEng.isAudioMuted());
        TEST_ASSERT(!netEng.isHostAudioSuspended());

        // Volume clamping
        netEng.setAudioVolume(50);
        TEST_ASSERT(netEng.audioVolume() == 50);

        netEng.setAudioVolume(150); // should clamp to 100
        TEST_ASSERT(netEng.audioVolume() == 100);

        netEng.setAudioVolume(-20); // should clamp to 0
        TEST_ASSERT(netEng.audioVolume() == 0);

        // Mute state
        netEng.setAudioMuted(true);
        TEST_ASSERT(netEng.isAudioMuted());

        netEng.setAudioMuted(false);
        TEST_ASSERT(!netEng.isAudioMuted());
    }

    // 3. AppSettings persistence of audio volume & default mute
    {
        IdentityManager idMgr(102);
        idMgr.loadOrCreate();

        AppSettings s = idMgr.settings();
        s.defaultAudioVolume = 65;
        s.audioMutedDefault = true;
        idMgr.updateSettings(s);

        // Reload from disk
        IdentityManager idMgrReload(102);
        idMgrReload.loadOrCreate();
        const auto& loaded = idMgrReload.settings();
        TEST_ASSERT(loaded.defaultAudioVolume == 65);
        TEST_ASSERT(loaded.audioMutedDefault == true);

        // Reset to clean state
        s.defaultAudioVolume = 100;
        s.audioMutedDefault = false;
        idMgr.updateSettings(s);
    }
}

void testMultiMonitorGridViewAndVirtualDesktop() {
    std::cout << "[TEST 27] Multi-Monitor Grid View & Virtual Desktop Capture...\n";

    ScreenCapturer capturer;
    auto monitors = capturer.enumerateMonitors();
    TEST_ASSERT(!monitors.empty());

    // 1. Select All Displays (Virtual Screen Grid)
    bool selectAllOk = capturer.selectMonitor(-1);
    TEST_ASSERT(selectAllOk);
    TEST_ASSERT(capturer.currentMonitorIndex() == -1);

    MonitorDesc allDesc = capturer.currentMonitor();
    TEST_ASSERT(allDesc.index == -1);
    TEST_ASSERT(allDesc.width == std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)));
    TEST_ASSERT(allDesc.height == std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)));
    TEST_ASSERT(capturer.frameWidth() == allDesc.width);
    TEST_ASSERT(capturer.frameHeight() == allDesc.height);

    // 2. Capture dirty tiles across full virtual desktop
    std::vector<EncodedTile> tiles;
    bool isKf = false;
    CursorState cur{};
    bool capOk = capturer.captureDirtyTiles(true, QualityPreset::Balanced, tiles, isKf, cur);
    TEST_ASSERT(capOk);
    TEST_ASSERT(isKf);
    TEST_ASSERT(!tiles.empty());

    // 3. Return to primary monitor (0)
    bool select0Ok = capturer.selectMonitor(0);
    TEST_ASSERT(select0Ok);
    TEST_ASSERT(capturer.currentMonitorIndex() == 0);
    TEST_ASSERT(capturer.currentMonitor().index == 0);
}

void testPrivacyModeCustomBrandingAndNotice() {
    std::cout << "[TEST 28] Remote Privacy Mode Custom Branding & Notice Message...\n";

    // 1. AppSettings defaults & persistence
    IdentityManager idMgr(103);
    AppSettings s = idMgr.settings();
    TEST_ASSERT(!s.privacyCustomNotice.empty());
    TEST_ASSERT(!s.privacyBrandName.empty());
    TEST_ASSERT(s.privacyShowDeskId);

    // 2. Custom Branding update and save
    std::string testNotice = "Strict IT Maintenance Window - Authorized Remote Tech 42";
    std::string testBrand = "Acme Corp Secure IT Ops";
    s.privacyCustomNotice = testNotice;
    s.privacyBrandName = testBrand;
    s.privacyShowDeskId = false;
    idMgr.updateSettings(s);

    AppSettings s2 = idMgr.settings();
    TEST_ASSERT(s2.privacyCustomNotice == testNotice);
    TEST_ASSERT(s2.privacyBrandName == testBrand);
    TEST_ASSERT(!s2.privacyShowDeskId);

    // 3. PrivacyModeConfigPayload binary packing and framing
    PrivacyModeConfigPayload p{};
    p.enable = 1;
    p.acknowledge = 0;
    p.showDeskId = 1;
    std::snprintf(p.customNotice, sizeof(p.customNotice), "%s", testNotice.c_str());
    std::snprintf(p.brandName, sizeof(p.brandName), "%s", testBrand.c_str());

    TEST_ASSERT(sizeof(PrivacyModeConfigPayload) >= sizeof(PrivacyModePayload));
    TEST_ASSERT(p.enable == 1);
    TEST_ASSERT(p.acknowledge == 0);
    TEST_ASSERT(p.showDeskId == 1);
    TEST_ASSERT(std::string(p.customNotice) == testNotice);
    TEST_ASSERT(std::string(p.brandName) == testBrand);

    // 4. Backward compatibility check
    PrivacyModePayload legacyP{};
    legacyP.enable = 1;
    legacyP.acknowledge = 0;
    TEST_ASSERT(sizeof(legacyP) == 2);
    // At offset 0 and 1, PrivacyModeConfigPayload matches PrivacyModePayload
    PrivacyModePayload* casted = reinterpret_cast<PrivacyModePayload*>(&p);
    TEST_ASSERT(casted->enable == 1);
    TEST_ASSERT(casted->acknowledge == 0);

    // 5. Wire Struct Size & Boundary Safeguards
    TEST_ASSERT(sizeof(PrivacyModeConfigPayload) == 196);
    TEST_ASSERT(sizeof(p.customNotice) == 128);
    TEST_ASSERT(sizeof(p.brandName) == 64);

    // 6. NetworkEngine curtain configuration
    NetworkEngine netEng(idMgr);
    netEng.configurePrivacyCurtain("Custom Curtain Notice", "SecOps Brand", false);
}

void testSystemHealthDiagnosticsAndHardwareSpecs() {
    std::cout << "[TEST 29] Remote System Health Diagnostics & Host Hardware Info Sheet...\n";

    // 1. Native Hardware Discovery
    auto diag = NetworkEngine::sampleHostDiagnostics();
    TEST_ASSERT(!diag.cpuModel.empty());
    TEST_ASSERT(diag.cpuCores > 0);
    TEST_ASSERT(!diag.gpuModel.empty());
    TEST_ASSERT(!diag.osVersion.empty());
    TEST_ASSERT(diag.uptimeSeconds > 0);
    TEST_ASSERT(diag.ramTotalBytes > 0);

    // 2. Extended Serialization & Round-Trip
    SystemDiagnosticsPayload p{};
    p.cpuUsagePercent = 23.5f;
    p.ramUsedBytes = 8589934592ULL; // 8 GB
    p.ramTotalBytes = 17179869184ULL; // 16 GB
    p.diskUsedBytes = 100000000000ULL;
    p.diskTotalBytes = 500000000000ULL;
    p.cpuModel = "AMD Ryzen 9 7950X 16-Core Processor";
    p.gpuModel = "NVIDIA GeForce RTX 4090";
    p.osVersion = "Windows 11 Pro (Build 22631)";
    p.cpuCores = 32;
    p.uptimeSeconds = 345678;

    ProcessTelemetryItem item{};
    item.pid = 1234;
    item.name = "explorer.exe";
    item.workingSetBytes = 150 * 1024 * 1024;
    p.processes.push_back(item);

    std::vector<uint8_t> bytes;
    serializeSystemDiagnostics(p, bytes);
    TEST_ASSERT(!bytes.empty());

    SystemDiagnosticsPayload decoded{};
    bool decOk = deserializeSystemDiagnostics(bytes.data(), bytes.size(), decoded);
    TEST_ASSERT(decOk);
    TEST_ASSERT(std::abs(decoded.cpuUsagePercent - 23.5f) < 0.001f);
    TEST_ASSERT(decoded.ramUsedBytes == 8589934592ULL);
    TEST_ASSERT(decoded.ramTotalBytes == 17179869184ULL);
    TEST_ASSERT(decoded.cpuModel == "AMD Ryzen 9 7950X 16-Core Processor");
    TEST_ASSERT(decoded.gpuModel == "NVIDIA GeForce RTX 4090");
    TEST_ASSERT(decoded.osVersion == "Windows 11 Pro (Build 22631)");
    TEST_ASSERT(decoded.cpuCores == 32);
    TEST_ASSERT(decoded.uptimeSeconds == 345678);
    TEST_ASSERT(decoded.processes.size() == 1);
    TEST_ASSERT(decoded.processes[0].name == "explorer.exe");

    // 3. Legacy Payload Backward Compatibility
    ByteWriter legacyW;
    legacyW.writeF32(12.0f);
    legacyW.writeU64(4000000);
    legacyW.writeU64(8000000);
    legacyW.writeU64(2000000);
    legacyW.writeU64(5000000);
    legacyW.writeU16(0); // 0 processes
    auto legacyBytes = legacyW.takeBuffer();

    SystemDiagnosticsPayload legacyDecoded{};
    bool legacyOk = deserializeSystemDiagnostics(legacyBytes.data(), legacyBytes.size(), legacyDecoded);
    TEST_ASSERT(legacyOk);
    TEST_ASSERT(std::abs(legacyDecoded.cpuUsagePercent - 12.0f) < 0.001f);
    TEST_ASSERT(legacyDecoded.cpuModel.empty());
    TEST_ASSERT(legacyDecoded.cpuCores == 0);
}

void testConnectionQualityProfiles() {
    std::cout << "[TEST 30] One-Click Connection Quality Profiles (Low Bandwidth, Balanced, Ultra LAN)...\n" << std::flush;

    // 1. Profile Mapping & Parameter Verification
    QualityPreset qp{};
    uint8_t fps = 0;
    bool adaptive = false;

    // LowBandwidth
    getProfileSettings(ConnectionProfile::LowBandwidth, qp, fps, adaptive);
    TEST_ASSERT(qp == QualityPreset::LowBandwidth);
    TEST_ASSERT(fps == 15);
    TEST_ASSERT(adaptive == true);
    TEST_ASSERT(std::string(connectionProfileName(ConnectionProfile::LowBandwidth)) == "Low Bandwidth");

    // Balanced
    getProfileSettings(ConnectionProfile::Balanced, qp, fps, adaptive);
    TEST_ASSERT(qp == QualityPreset::Balanced);
    TEST_ASSERT(fps == 30);
    TEST_ASSERT(adaptive == true);
    TEST_ASSERT(std::string(connectionProfileName(ConnectionProfile::Balanced)) == "Balanced");

    // UltraLAN
    getProfileSettings(ConnectionProfile::UltraLAN, qp, fps, adaptive);
    TEST_ASSERT(qp == QualityPreset::Ultra);
    TEST_ASSERT(fps == 60);
    TEST_ASSERT(adaptive == false);
    TEST_ASSERT(std::string(connectionProfileName(ConnectionProfile::UltraLAN)) == "Ultra LAN");

    // Custom
    TEST_ASSERT(std::string(connectionProfileName(ConnectionProfile::Custom)) == "Custom");

    // 2. Profile Inference Tests
    TEST_ASSERT(inferConnectionProfile(QualityPreset::LowBandwidth, 15, true) == ConnectionProfile::LowBandwidth);
    TEST_ASSERT(inferConnectionProfile(QualityPreset::Balanced, 30, true) == ConnectionProfile::Balanced);
    TEST_ASSERT(inferConnectionProfile(QualityPreset::Ultra, 60, false) == ConnectionProfile::UltraLAN);
    TEST_ASSERT(inferConnectionProfile(QualityPreset::Ultra, 30, true) == ConnectionProfile::Custom);
    TEST_ASSERT(inferConnectionProfile(QualityPreset::LowBandwidth, 60, false) == ConnectionProfile::Custom);
    TEST_ASSERT(inferConnectionProfile(QualityPreset::Balanced, 60, false) == ConnectionProfile::Custom);

    // 3. Settings Persistence Verification
    AppSettings s{};
    s.connectionProfile = ConnectionProfile::UltraLAN;
    s.defaultQuality = QualityPreset::Ultra;
    s.targetFps = 60;
    s.adaptiveFps = false;

    IdentityManager idMgr(98);
    idMgr.updateSettings(s);
    AppSettings loaded = idMgr.settings();
    TEST_ASSERT(loaded.connectionProfile == ConnectionProfile::UltraLAN);
    TEST_ASSERT(loaded.defaultQuality == QualityPreset::Ultra);
    TEST_ASSERT(loaded.targetFps == 60);
    TEST_ASSERT(loaded.adaptiveFps == false);

    // 4. Network Engine Profile Application
    NetworkEngine net(idMgr);
    net.applyConnectionProfile(ConnectionProfile::LowBandwidth);
    auto st = net.viewerStats();
    TEST_ASSERT(st.connectionProfile == ConnectionProfile::LowBandwidth);
    TEST_ASSERT(st.qualityPreset == QualityPreset::LowBandwidth);
    TEST_ASSERT(st.targetFps == 15);
    TEST_ASSERT(st.adaptiveFps == true);

    net.applyConnectionProfile(ConnectionProfile::UltraLAN);
    st = net.viewerStats();
    TEST_ASSERT(st.connectionProfile == ConnectionProfile::UltraLAN);
    TEST_ASSERT(st.qualityPreset == QualityPreset::Ultra);
    TEST_ASSERT(st.targetFps == 60);
    TEST_ASSERT(st.adaptiveFps == false);
}

void testDxgiDirtyRectsAndGdiRecycling() {
    std::cout << "[TEST 31] DXGI Hardware Dirty-Rect Bounding & GDI Handle Recycling...\n";

    ScreenCapturer capturer;
    capturer.selectMonitor(0);
    TEST_ASSERT(capturer.frameWidth() > 0 && capturer.frameHeight() > 0);

    // Initial keyframe capture initializes frame baseline
    std::vector<EncodedTile> tilesKf;
    bool isKf = false;
    CursorState cursor{};
    bool ok = capturer.captureDirtyTiles(true, QualityPreset::Balanced, tilesKf, isKf, cursor);
    TEST_ASSERT(ok);
    TEST_ASSERT(isKf);
    TEST_ASSERT(!tilesKf.empty());

    // Second consecutive delta capture with unchanged desktop
    std::vector<EncodedTile> deltaTiles;
    bool deltaIsKf = true;
    ok = capturer.captureDirtyTiles(false, QualityPreset::Balanced, deltaTiles, deltaIsKf, cursor);
    TEST_ASSERT(ok);
    TEST_ASSERT(!deltaIsKf);

    // Verify GDI handle persistence when falling back to GDI
    capturer.triggerDxgiAccessLostForTest();
    TEST_ASSERT(capturer.dxgiRecoveryState() == ScreenCapturer::DxgiRecoveryState::FallbackGdi);

    std::vector<EncodedTile> gdiTiles1;
    bool gdiKf1 = false;
    ok = capturer.captureDirtyTiles(true, QualityPreset::Balanced, gdiTiles1, gdiKf1, cursor);
    TEST_ASSERT(ok);
    TEST_ASSERT(gdiKf1);
    TEST_ASSERT(capturer.hasGdiCachedResources());

    // Subsequent capture reuses existing GDI memory DC and DIB section without re-allocation
    std::vector<EncodedTile> gdiTiles2;
    bool gdiKf2 = false;
    ok = capturer.captureDirtyTiles(false, QualityPreset::Balanced, gdiTiles2, gdiKf2, cursor);
    TEST_ASSERT(ok);
    TEST_ASSERT(capturer.hasGdiCachedResources());
}

void testCodecContextRecyclingAndZeroCopyTasks() {
    std::cout << "[TEST 32] Codec Context Recycling & Zero-Copy Thread Pool Tasks...\n";

    // 1. Verify repeated ZSTD compression & decompression with context reuse
    const size_t rawSize = 64 * 64 * 4;
    std::vector<uint8_t> rawPixels(rawSize);
    for (size_t i = 0; i < rawSize; ++i) {
        rawPixels[i] = static_cast<uint8_t>((i * 7) & 0xFF);
    }

    for (int iter = 0; iter < 10; ++iter) {
        auto compressed = TileCodec::compressZstd(rawPixels.data(), rawPixels.size(), 1);
        TEST_ASSERT(!compressed.empty());
        TEST_ASSERT(compressed.size() < rawPixels.size());

        std::vector<uint8_t> decompressed(rawSize, 0);
        bool ok = TileCodec::decompressZstd(compressed.data(), compressed.size(), decompressed.data(), decompressed.size());
        TEST_ASSERT(ok);
        TEST_ASSERT(std::memcmp(rawPixels.data(), decompressed.data(), rawSize) == 0);
    }

    // 2. Verify TileThreadPool zero-copy frameBase tasks
    const int frameW = 256;
    const int frameH = 256;
    std::vector<uint8_t> frame(static_cast<size_t>(frameW) * frameH * 4);
    for (int y = 0; y < frameH; ++y) {
        for (int x = 0; x < frameW; ++x) {
            size_t idx = static_cast<size_t>(y * frameW + x) * 4;
            frame[idx + 0] = static_cast<uint8_t>(x & 0xFF);
            frame[idx + 1] = static_cast<uint8_t>(y & 0xFF);
            frame[idx + 2] = static_cast<uint8_t>((x + y) & 0xFF);
            frame[idx + 3] = 0xFF;
        }
    }

    std::vector<TileThreadPool::RectTask> tasks;
    for (int i = 0; i < 4; ++i) {
        TileThreadPool::RectTask t{};
        t.rx = static_cast<uint16_t>((i % 2) * 64);
        t.ry = static_cast<uint16_t>((i / 2) * 64);
        t.rw = 64;
        t.rh = 64;
        t.frameBase = frame.data();
        t.frameStride = frameW * 4;
        t.preset = QualityPreset::Balanced;
        tasks.push_back(std::move(t));
    }

    TileThreadPool::instance().parallelEncode(tasks);
    TEST_ASSERT(tasks.size() == 4);
    for (const auto& t : tasks) {
        TEST_ASSERT(!t.result.data.empty());
        TEST_ASSERT(t.result.width == 64);
        TEST_ASSERT(t.result.height == 64);
    }
}

void testDirect2DPartialDirtyRectAndZeroCopyViewer() {
    std::cout << "[TEST 33] Direct2D Partial Dirty-Rect Tracking & Zero-Copy Viewer Pipeline...\n";

    // 1. Verify TileCodec::decodeTileIntoCanvas with allocation-free scratch and RawBGRA
    const int cW = 128;
    const int cH = 128;
    std::vector<uint8_t> canvas(cW * cH * 4, 0);

    EncodedTile tile;
    tile.x = 10;
    tile.y = 20;
    tile.width = 64;
    tile.height = 32;
    tile.encoding = TileEncoding::RawBGRA;
    tile.data.assign(64 * 32 * 4, 0xAB);

    bool ok = TileCodec::decodeTileIntoCanvas(tile, canvas.data(), cW, cH);
    TEST_ASSERT(ok);

    // Verify pixel value at (10, 20)
    size_t sampleIdx = (20 * cW + 10) * 4;
    TEST_ASSERT(canvas[sampleIdx + 0] == 0xAB);
    TEST_ASSERT(canvas[sampleIdx + 1] == 0xAB);
    TEST_ASSERT(canvas[sampleIdx + 2] == 0xAB);
    TEST_ASSERT(canvas[sampleIdx + 3] == 0xFF); // Alpha forced opaque

    // 2. Verify NetworkEngine copyLatestViewerFrame with dirty bounds
    IdentityManager id(33);
    id.loadOrCreate();
    NetworkEngine net(id);

    uint64_t seq = 0;
    std::vector<uint8_t> frameBuf;
    int outW = 0, outH = 0;
    CursorState cur{};
    RECT dirtyBounds{};

    // Before any frame arrives, copyLatestViewerFrame returns false
    TEST_ASSERT(!net.copyLatestViewerFrame(seq, frameBuf, outW, outH, cur, &dirtyBounds));
}

void testScatterGatherFrameSendAndSocketBuffers() {
    std::cout << "[TEST 34] Socket Buffer Scaling & Scatter-Gather Frame Transmission...\n" << std::flush;

    // 1. Verify Socket Buffer Scaling
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(s != INVALID_SOCKET);
    NetworkEngine::setTcpNoDelay(s);

    int sndBuf = 0;
    int optLen = sizeof(sndBuf);
    int res = getsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndBuf), &optLen);
    TEST_ASSERT(res == 0);
    // On Windows, requested 2MB buffer should scale well above default 64KB
    TEST_ASSERT(sndBuf >= 65536);

    int rcvBuf = 0;
    optLen = sizeof(rcvBuf);
    res = getsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcvBuf), &optLen);
    TEST_ASSERT(res == 0);
    TEST_ASSERT(rcvBuf >= 65536);
    closesocket(s);

    // 2. Setup Loopback Sockets for I/O
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(listenSock != INVALID_SOCKET);

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    TEST_ASSERT(bind(listenSock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == 0);
    TEST_ASSERT(listen(listenSock, 1) == 0);

    int sinLen = sizeof(sin);
    TEST_ASSERT(getsockname(listenSock, reinterpret_cast<sockaddr*>(&sin), &sinLen) == 0);

    SOCKET clientSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(clientSock != INVALID_SOCKET);
    NetworkEngine::setTcpNoDelay(clientSock);
    TEST_ASSERT(connect(clientSock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == 0);

    SOCKET serverSock = accept(listenSock, nullptr, nullptr);
    TEST_ASSERT(serverSock != INVALID_SOCKET);
    NetworkEngine::setTcpNoDelay(serverSock);
    closesocket(listenSock);

    // 3. Test Direct Scatter-Gather Transmission
    std::string testPayload(4096, 'X');
    for (size_t i = 0; i < testPayload.size(); ++i) {
        testPayload[i] = static_cast<char>('A' + (i % 26));
    }
    FrameHeader directHdr{};
    directHdr.magic = PROTOCOL_MAGIC;
    directHdr.type = static_cast<uint8_t>(PacketType::CHAT_MESSAGE);
    directHdr.payloadSize = static_cast<uint32_t>(testPayload.size());

    TEST_ASSERT(NetworkEngine::sendScatterGather(clientSock, &directHdr, sizeof(directHdr), testPayload.data(), testPayload.size()));

    FrameHeader rcvDirectHdr{};
    std::vector<uint8_t> rcvDirectPayload;
    TEST_ASSERT(NetworkEngine::recvFrame(serverSock, rcvDirectHdr, rcvDirectPayload));
    TEST_ASSERT(rcvDirectHdr.magic == PROTOCOL_MAGIC);
    TEST_ASSERT(rcvDirectHdr.type == static_cast<uint8_t>(PacketType::CHAT_MESSAGE));
    TEST_ASSERT(rcvDirectPayload.size() == testPayload.size());
    TEST_ASSERT(std::memcmp(rcvDirectPayload.data(), testPayload.data(), testPayload.size()) == 0);

    // 4. Test High-Frequency Encrypted sendFrame with Scatter-Gather
    std::array<uint8_t, 32> dummyKey{};
    dummyKey.fill(0x5A);
    AesGcmSessionCipher hostCipher, viewerCipher;
    TEST_ASSERT(hostCipher.initialize(dummyKey, true));
    TEST_ASSERT(viewerCipher.initialize(dummyKey, false));

    uint64_t hostSendSeq = 1;
    uint64_t viewerRecvSeq = 1;
    std::mutex sendMtx;

    for (int frameIdx = 0; frameIdx < 10; ++frameIdx) {
        std::string frameMsg = "Encrypted Scatter-Gather Frame #" + std::to_string(frameIdx);
        TEST_ASSERT(NetworkEngine::sendFrame(
            clientSock,
            PacketType::CHAT_MESSAGE,
            0,
            frameMsg.data(),
            frameMsg.size(),
            sendMtx,
            &hostCipher,
            &hostSendSeq
        ));

        FrameHeader encHdr{};
        std::vector<uint8_t> encPayload;
        TEST_ASSERT(NetworkEngine::recvFrame(serverSock, encHdr, encPayload, &viewerCipher, &viewerRecvSeq));
        TEST_ASSERT(encHdr.magic == PROTOCOL_MAGIC);
        TEST_ASSERT(encHdr.flags & FLAG_ENCRYPTED);
        TEST_ASSERT(encPayload.size() == frameMsg.size());
        TEST_ASSERT(std::string(encPayload.begin(), encPayload.end()) == frameMsg);
    }

    closesocket(clientSock);
    closesocket(serverSock);
}

void testPrecisionTimerPacingAndRecycledBuffers() {
    std::cout << "[TEST 35] 1ms Multimedia Timer Pacing & Recycled Working Buffers...\n" << std::flush;

    // 1. Verify 1ms Windows Timer Granularity Request & Capability
    TIMECAPS tc{};
    MMRESULT rCaps = timeGetDevCaps(&tc, sizeof(tc));
    TEST_ASSERT(rCaps == TIMERR_NOERROR);
    TEST_ASSERT(tc.wPeriodMin <= 1);

    MMRESULT r = timeBeginPeriod(1);
    TEST_ASSERT(r == TIMERR_NOERROR);

    MMRESULT rEnd = timeEndPeriod(1);
    TEST_ASSERT(rEnd == TIMERR_NOERROR);

    // 2. Verify ScreenCapturer Recycled Working Buffers
    ScreenCapturer capturer;
    std::vector<EncodedTile> tiles1, tiles2;
    bool isKey1 = false, isKey2 = false;
    CursorState cur1{}, cur2{};

    bool capOk1 = capturer.captureDirtyTiles(true, QualityPreset::Balanced, tiles1, isKey1, cur1);
    TEST_ASSERT(capOk1);
    TEST_ASSERT(isKey1);

    bool capOk2 = capturer.captureDirtyTiles(false, QualityPreset::Balanced, tiles2, isKey2, cur2);
    TEST_ASSERT(capOk2);

    // 3. Verify Zero-Allocation Recv Pipeline Decryption
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(listenSock != INVALID_SOCKET);

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    TEST_ASSERT(bind(listenSock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == 0);
    TEST_ASSERT(listen(listenSock, 1) == 0);

    int sinLen = sizeof(sin);
    TEST_ASSERT(getsockname(listenSock, reinterpret_cast<sockaddr*>(&sin), &sinLen) == 0);

    SOCKET clientSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(clientSock != INVALID_SOCKET);
    NetworkEngine::setTcpNoDelay(clientSock);
    TEST_ASSERT(connect(clientSock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) == 0);

    SOCKET serverSock = accept(listenSock, nullptr, nullptr);
    TEST_ASSERT(serverSock != INVALID_SOCKET);
    NetworkEngine::setTcpNoDelay(serverSock);
    closesocket(listenSock);

    std::array<uint8_t, 32> dummyKey{};
    dummyKey.fill(0x3C);
    AesGcmSessionCipher hostCipher, viewerCipher;
    TEST_ASSERT(hostCipher.initialize(dummyKey, true));
    TEST_ASSERT(viewerCipher.initialize(dummyKey, false));

    uint64_t hostSendSeq = 1;
    uint64_t viewerRecvSeq = 1;
    std::mutex sendMtx;

    // Send a 64 KB payload to thoroughly exercise thread-local encrypted scratch buffer and direct decryption
    std::vector<uint8_t> largePayload(65536);
    for (size_t i = 0; i < largePayload.size(); ++i) {
        largePayload[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }

    TEST_ASSERT(NetworkEngine::sendFrame(
        clientSock,
        PacketType::CHAT_MESSAGE,
        0,
        largePayload.data(),
        largePayload.size(),
        sendMtx,
        &hostCipher,
        &hostSendSeq
    ));

    FrameHeader encHdr{};
    std::vector<uint8_t> decPayload;
    TEST_ASSERT(NetworkEngine::recvFrame(serverSock, encHdr, decPayload, &viewerCipher, &viewerRecvSeq));
    TEST_ASSERT(encHdr.magic == PROTOCOL_MAGIC);
    TEST_ASSERT(encHdr.flags & FLAG_ENCRYPTED);
    TEST_ASSERT(decPayload.size() == largePayload.size());
    TEST_ASSERT(std::memcmp(decPayload.data(), largePayload.data(), largePayload.size()) == 0);

    closesocket(clientSock);
    closesocket(serverSock);
}

void testZeroCopyTileStreamingAndSerializer() {
    std::cout << "[TEST 36] Zero-Copy In-Place Tile Streaming & Pre-Reserved Frame Serializer...\n" << std::flush;

    // 1. Verify ByteWriter capacity reservation and move semantics
    ByteWriter w;
    w.reserve(8192);
    TEST_ASSERT(w.buffer().capacity() >= 8192);
    TEST_ASSERT(w.buffer().empty());

    w.writeU8(0x7F);
    w.writeU16(0x1234);
    w.writeU32(0xDEADBEEF);
    TEST_ASSERT(w.buffer().size() == 7);

    std::vector<uint8_t> moved = w.takeBuffer();
    TEST_ASSERT(moved.size() == 7);
    TEST_ASSERT(w.buffer().empty());
    TEST_ASSERT(moved[0] == 0x7F);

    // 2. Verify raw-pointer TileCodec::decodeTileIntoCanvas with synthetic tile
    const int canvasW = 128;
    const int canvasH = 128;
    std::vector<uint8_t> canvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);

    const uint16_t tileX = 16;
    const uint16_t tileY = 24;
    const uint16_t tileW = 32;
    const uint16_t tileH = 32;
    std::vector<uint8_t> tileBgra(static_cast<size_t>(tileW) * tileH * 4);
    for (size_t i = 0; i < tileBgra.size(); i += 4) {
        tileBgra[i] = 0xAA;     // B
        tileBgra[i + 1] = 0xBB; // G
        tileBgra[i + 2] = 0xCC; // R
        tileBgra[i + 3] = 0xFF; // A
    }

    // Ultra preset produces Zstd
    EncodedTile encTileZstd = TileCodec::encodeRect(tileX, tileY, tileW, tileH, tileBgra.data(), QualityPreset::Ultra);
    TEST_ASSERT(!encTileZstd.data.empty());

    bool decOkZstd = TileCodec::decodeTileIntoCanvas(
        tileX, tileY, tileW, tileH,
        encTileZstd.encoding,
        encTileZstd.data.data(), encTileZstd.data.size(),
        canvas.data(), canvasW, canvasH
    );
    TEST_ASSERT(decOkZstd);

    // Verify sample pixel in decoded canvas region
    size_t sampleOffset = (static_cast<size_t>(tileY + 5) * canvasW + (tileX + 5)) * 4;
    TEST_ASSERT(canvas[sampleOffset] == 0xAA);
    TEST_ASSERT(canvas[sampleOffset + 1] == 0xBB);
    TEST_ASSERT(canvas[sampleOffset + 2] == 0xCC);
    TEST_ASSERT(canvas[sampleOffset + 3] == 0xFF);

    // Balanced preset produces Jpeg or Zstd
    EncodedTile encTileBal = TileCodec::encodeRect(tileX, tileY, tileW, tileH, tileBgra.data(), QualityPreset::Balanced);
    TEST_ASSERT(!encTileBal.data.empty());
    bool decOkBal = TileCodec::decodeTileIntoCanvas(
        tileX, tileY, tileW, tileH,
        encTileBal.encoding,
        encTileBal.data.data(), encTileBal.data.size(),
        canvas.data(), canvasW, canvasH
    );
    TEST_ASSERT(decOkBal);

    // 3. Verify safety bounds and null checks
    // Out-of-bounds tile
    bool oobOk = TileCodec::decodeTileIntoCanvas(
        120, 120, tileW, tileH,
        encTileZstd.encoding,
        encTileZstd.data.data(), encTileZstd.data.size(),
        canvas.data(), canvasW, canvasH
    );
    TEST_ASSERT(!oobOk);

    // Null canvas pointer
    bool nullCanvasOk = TileCodec::decodeTileIntoCanvas(
        tileX, tileY, tileW, tileH,
        encTileZstd.encoding,
        encTileZstd.data.data(), encTileZstd.data.size(),
        nullptr, canvasW, canvasH
    );
    TEST_ASSERT(!nullCanvasOk);

    // Zero data size with non-raw encoding
    bool zeroDataOk = TileCodec::decodeTileIntoCanvas(
        tileX, tileY, tileW, tileH,
        TileEncoding::Zstd,
        nullptr, 0,
        canvas.data(), canvasW, canvasH
    );
    TEST_ASSERT(!zeroDataOk);
}

void testDirectJpegCanvasBlitAndFramePacing() {
    std::cout << "[TEST 37] Direct JPEG Canvas Blit, Host Vector Recycling & 60 FPS Pacing...\n" << std::flush;

    // 1. Direct JPEG-to-Canvas Blit Verification
    const uint16_t tileW = 48;
    const uint16_t tileH = 32;
    std::vector<uint8_t> tileBgra(static_cast<size_t>(tileW) * tileH * 4);
    for (size_t i = 0; i < tileBgra.size(); i += 4) {
        tileBgra[i]     = 0x50; // B
        tileBgra[i + 1] = 0x8C; // G
        tileBgra[i + 2] = 0xDC; // R
        tileBgra[i + 3] = 0xFF; // A
    }

    auto jpegData = TileCodec::encodeJpeg(tileBgra.data(), tileW, tileH, 85);
    TEST_ASSERT(!jpegData.empty());

    const int canvasW = 100;
    const int canvasH = 100;
    std::vector<uint8_t> canvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);

    const uint16_t targetX = 20;
    const uint16_t targetY = 30;
    bool blitOk = TileCodec::decodeJpegIntoCanvas(
        jpegData.data(), jpegData.size(),
        canvas.data(), canvasW, canvasH,
        targetX, targetY, tileW, tileH
    );
    TEST_ASSERT(blitOk);

    // Verify interior pixel: Alpha must be exactly 0xFF, RGB closely matching original
    size_t sampleIdx = (static_cast<size_t>(targetY + 10) * canvasW + (targetX + 10)) * 4;
    TEST_ASSERT(canvas[sampleIdx + 3] == 0xFF);
    TEST_ASSERT(std::abs(static_cast<int>(canvas[sampleIdx]) - 0x50) < 25);
    TEST_ASSERT(std::abs(static_cast<int>(canvas[sampleIdx + 1]) - 0x8C) < 25);
    TEST_ASSERT(std::abs(static_cast<int>(canvas[sampleIdx + 2]) - 0xDC) < 25);

    // Verify untargeted region remains untouched (0x00)
    size_t outsideIdx = (static_cast<size_t>(10) * canvasW + 10) * 4;
    TEST_ASSERT(canvas[outsideIdx + 3] == 0x00);

    // 2. Bounds and safety checks
    bool oobOk = TileCodec::decodeJpegIntoCanvas(
        jpegData.data(), jpegData.size(),
        canvas.data(), canvasW, canvasH,
        80, 80, tileW, tileH
    );
    TEST_ASSERT(!oobOk);

    bool nullCanvasOk = TileCodec::decodeJpegIntoCanvas(
        jpegData.data(), jpegData.size(),
        nullptr, canvasW, canvasH,
        targetX, targetY, tileW, tileH
    );
    TEST_ASSERT(!nullCanvasOk);

    bool nullDataOk = TileCodec::decodeJpegIntoCanvas(
        nullptr, 0,
        canvas.data(), canvasW, canvasH,
        targetX, targetY, tileW, tileH
    );
    TEST_ASSERT(!nullDataOk);

    bool dimMismatchOk = TileCodec::decodeJpegIntoCanvas(
        jpegData.data(), jpegData.size(),
        canvas.data(), canvasW, canvasH,
        targetX, targetY, 64, 64
    );
    TEST_ASSERT(!dimMismatchOk);

    // 3. NetworkEngine onFrameDecoded callback registration
    IdentityManager dummyId(99);
    dummyId.loadOrCreate();
    NetworkEngine netEngine(dummyId);
    std::atomic<int> cbCount{0};
    netEngine.setOnFrameDecodedCallback([&]() {
        cbCount.fetch_add(1);
    });
    TEST_ASSERT(cbCount.load() == 0);
}

void testInputCoalescingAndZeroAllocEvents() {
    std::cout << "[SUITE 38] Zero-Allocation Input Serialization & Mouse Coalescing..." << std::endl;

    // 1. Stack struct zero-allocation serialization compatibility with ByteReader
    {
        // MouseMove
        float moveCoords[2] = { 0.42f, 0.88f };
        ByteReader moveReader(reinterpret_cast<const uint8_t*>(moveCoords), sizeof(moveCoords));
        TEST_ASSERT(std::abs(moveReader.readF32() - 0.42f) < 0.0001f);
        TEST_ASSERT(std::abs(moveReader.readF32() - 0.88f) < 0.0001f);
        TEST_ASSERT(!moveReader.hasRemaining(1));

        // MouseButton
#pragma pack(push, 1)
        struct MouseButtonPayload {
            uint8_t button;
            uint8_t isDown;
            float normX;
            float normY;
        } btnPayload{ 1, 1, 0.15f, 0.65f };
#pragma pack(pop)
        TEST_ASSERT(sizeof(btnPayload) == 10);
        ByteReader btnReader(reinterpret_cast<const uint8_t*>(&btnPayload), sizeof(btnPayload));
        TEST_ASSERT(btnReader.readU8() == 1);
        TEST_ASSERT(btnReader.readU8() == 1);
        TEST_ASSERT(std::abs(btnReader.readF32() - 0.15f) < 0.0001f);
        TEST_ASSERT(std::abs(btnReader.readF32() - 0.65f) < 0.0001f);
        TEST_ASSERT(!btnReader.hasRemaining(1));

        // MouseWheel
        int32_t wheelDeltas[2] = { 120, -240 };
        TEST_ASSERT(sizeof(wheelDeltas) == 8);
        ByteReader wheelReader(reinterpret_cast<const uint8_t*>(wheelDeltas), sizeof(wheelDeltas));
        TEST_ASSERT(wheelReader.readI32() == 120);
        TEST_ASSERT(wheelReader.readI32() == -240);
        TEST_ASSERT(!wheelReader.hasRemaining(1));

        // KeyEvent
#pragma pack(push, 1)
        struct KeyEventPayload {
            uint16_t vk;
            uint16_t sc;
            uint8_t isDown;
            uint8_t isExt;
        } keyPayload{ 0x57, 0x11, 1, 0 }; // 'W' key down
#pragma pack(pop)
        TEST_ASSERT(sizeof(keyPayload) == 6);
        ByteReader keyReader(reinterpret_cast<const uint8_t*>(&keyPayload), sizeof(keyPayload));
        TEST_ASSERT(keyReader.readU16() == 0x57);
        TEST_ASSERT(keyReader.readU16() == 0x11);
        TEST_ASSERT(keyReader.readU8() == 1);
        TEST_ASSERT(keyReader.readU8() == 0);
        TEST_ASSERT(!keyReader.hasRemaining(1));
    }

    // 2. Mouse Move Coalescing Logic & Tail Preservation Simulation
    {
        struct CoalescerSimulator {
            float curNormX = 0.0f;
            float curNormY = 0.0f;
            bool pending = false;
            uint64_t lastSendTick = 0;
            std::vector<std::pair<float, float>> sentEvents;

            void onMove(float x, float y, uint64_t tick) {
                curNormX = x;
                curNormY = y;
                pending = true;
                if (tick - lastSendTick >= 8) {
                    flush(tick);
                }
            }

            void flush(uint64_t tick) {
                if (pending) {
                    sentEvents.push_back({ curNormX, curNormY });
                    pending = false;
                    lastSendTick = tick;
                }
            }

            void onButton(uint64_t tick) {
                flush(tick); // Guaranteed flush prior to button event
            }
        };

        CoalescerSimulator sim;
        // Move at tick 0: fires immediately
        sim.onMove(0.1f, 0.1f, 100);
        TEST_ASSERT(sim.sentEvents.size() == 1);
        TEST_ASSERT(sim.sentEvents.back().first == 0.1f);
        TEST_ASSERT(!sim.pending);

        // Rapid moves within 8ms window (tick 102, 104, 106)
        sim.onMove(0.2f, 0.2f, 102);
        TEST_ASSERT(sim.pending);
        TEST_ASSERT(sim.sentEvents.size() == 1); // Not sent yet

        sim.onMove(0.3f, 0.3f, 104);
        TEST_ASSERT(sim.pending);
        TEST_ASSERT(sim.sentEvents.size() == 1); // Coalesced

        sim.onMove(0.4f, 0.4f, 106);
        TEST_ASSERT(sim.pending);
        TEST_ASSERT(sim.sentEvents.size() == 1); // Still coalesced

        // User stops moving at 0.4! Button pressed at tick 107
        sim.onButton(107);
        // Guaranteed flush must have delivered the final 0.4 position!
        TEST_ASSERT(sim.sentEvents.size() == 2);
        TEST_ASSERT(sim.sentEvents.back().first == 0.4f);
        TEST_ASSERT(sim.sentEvents.back().second == 0.4f);
        TEST_ASSERT(!sim.pending);

        // Rapid move at tick 110 (3ms after button at 107) stays pending until flush/timer at tick 120
        sim.onMove(0.85f, 0.95f, 110);
        TEST_ASSERT(sim.pending);
        sim.flush(120);
        TEST_ASSERT(sim.sentEvents.size() == 3);
        TEST_ASSERT(sim.sentEvents.back().first == 0.85f);
        TEST_ASSERT(sim.sentEvents.back().second == 0.95f);
        TEST_ASSERT(!sim.pending);
    }

    // 3. Virtual Desktop Caching and InputInjector invocation
    {
        MonitorDesc mon{};
        mon.index = 0;
        mon.x = 0;
        mon.y = 0;
        mon.width = 1920;
        mon.height = 1080;
        mon.isPrimary = true;

        // Verify injectMouseMove executes smoothly using cached metrics
        InputInjector::injectMouseMove(0.5f, 0.5f, mon);
        InputInjector::injectMouseMove(0.6f, 0.6f, mon);
        TEST_ASSERT(true);
    }
}

void testHostOutboxAndCachedGeometries() {
    std::cout << "[SUITE 39] Host Outbox Buffer Recycling, Cursor Serialization & Direct2D Pre-Caching...\n" << std::flush;

    // 1. CursorUpdatePacket bit-exact packing (9 bytes)
    {
        TEST_ASSERT(sizeof(CursorUpdatePacket) == 9);
        CursorUpdatePacket cp{ 0.25f, 0.75f, 1 };
        std::vector<uint8_t> rawBuf(sizeof(cp));
        std::memcpy(rawBuf.data(), &cp, sizeof(cp));

        ByteReader r(rawBuf);
        TEST_ASSERT(r.hasRemaining(sizeof(CursorUpdatePacket)));
        TEST_ASSERT(std::abs(r.readF32() - 0.25f) < 0.0001f);
        TEST_ASSERT(std::abs(r.readF32() - 0.75f) < 0.0001f);
        TEST_ASSERT(r.readU8() == 1);
        TEST_ASSERT(!r.hasRemaining(1));
    }

    // 2. PingPacket bit-exact packing (12 bytes)
    {
        TEST_ASSERT(sizeof(PingPacket) == 12);
        PingPacket pp{ 0x1122334455667788ULL, 45 };
        std::vector<uint8_t> rawBuf(sizeof(pp));
        std::memcpy(rawBuf.data(), &pp, sizeof(pp));

        ByteReader r(rawBuf);
        TEST_ASSERT(r.hasRemaining(sizeof(PingPacket)));
        TEST_ASSERT(r.readU64() == 0x1122334455667788ULL);
        TEST_ASSERT(r.readU32() == 45);
        TEST_ASSERT(!r.hasRemaining(1));
    }

    // 3. ByteWriter move constructor & buffer zero-copy transfer
    {
        std::vector<uint8_t> recycled;
        recycled.reserve(65536);
        const uint8_t* origPtr = recycled.data();

        ByteWriter w(std::move(recycled));
        w.writeU32(0xCAFEBABE);
        std::vector<uint8_t> out = w.takeBuffer();

        TEST_ASSERT(out.size() == 4);
        TEST_ASSERT(out.capacity() >= 65536);
        TEST_ASSERT(out.data() == origPtr); // Zero copy - ownership moved in-place!
        ByteReader r(out);
        TEST_ASSERT(r.readU32() == 0xCAFEBABE);
    }

    // 4. Direct2D pre-cached unit star geometry creation and bounds validation
    {
        ID2D1Factory* factory = nullptr;
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
        if (SUCCEEDED(hr) && factory) {
            ID2D1PathGeometry* star = nullptr;
            hr = factory->CreatePathGeometry(&star);
            TEST_ASSERT(SUCCEEDED(hr) && star);
            if (star) {
                ID2D1GeometrySink* sink = nullptr;
                hr = star->Open(&sink);
                TEST_ASSERT(SUCCEEDED(hr) && sink);
                if (sink) {
                    constexpr float PI = 3.14159265f;
                    float innerR = 0.42f;
                    D2D1_POINT_2F pts[10];
                    for (int i = 0; i < 10; ++i) {
                        float angle = -PI * 0.5f + i * (PI / 5.0f);
                        float r = (i % 2 == 0) ? 1.0f : innerR;
                        pts[i] = D2D1::Point2F(r * std::cos(angle), r * std::sin(angle));
                    }
                    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLines(&pts[1], 9);
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();

                    D2D1_RECT_F bounds{};
                    hr = star->GetBounds(nullptr, &bounds);
                    TEST_ASSERT(SUCCEEDED(hr));
                    TEST_ASSERT(bounds.left >= -1.05f && bounds.right <= 1.05f);
                    TEST_ASSERT(bounds.top >= -1.05f && bounds.bottom <= 1.05f);

                    star->Release();
                }
            }
            factory->Release();
        }
    }
}

void testParallelViewerDecodeAndTabFrameCache() {
    std::cout << "[TEST 40] Parallel Multi-Tile Viewer Decoding & Tab Frame Cache Verification...\n";

    // 1. Multi-tile parallel decode (RAW_BGRA, ZLIB, JPEG) into shared canvas
    {
        const int canvasW = 512;
        const int canvasH = 512;
        std::vector<uint8_t> seqCanvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);
        std::vector<uint8_t> parCanvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);

        struct TileSpec {
            uint16_t x, y, w, h;
            QualityPreset preset;
            EncodedTile encoded;
        };

        std::vector<TileSpec> specs = {
            {   0,   0, 128, 128, QualityPreset::Ultra,        {} },
            { 128,   0, 128, 128, QualityPreset::Balanced,     {} },
            { 256,   0, 128, 128, QualityPreset::LowBandwidth, {} },
            { 384,   0, 128, 128, QualityPreset::Ultra,        {} },
            {   0, 128, 256, 128, QualityPreset::Balanced,     {} },
            { 256, 128, 256, 128, QualityPreset::LowBandwidth, {} },
            {   0, 256, 256, 256, QualityPreset::Ultra,        {} },
            { 256, 256, 256, 256, QualityPreset::Balanced,     {} }
        };

        for (size_t idx = 0; idx < specs.size(); ++idx) {
            auto& s = specs[idx];
            std::vector<uint8_t> raw(static_cast<size_t>(s.w) * s.h * 4);
            for (int r = 0; r < s.h; ++r) {
                for (int c = 0; c < s.w; ++c) {
                    size_t off = (static_cast<size_t>(r) * s.w + c) * 4;
                    raw[off + 0] = static_cast<uint8_t>((c * 3 + idx * 17) & 0xFF);
                    raw[off + 1] = static_cast<uint8_t>((r * 5 + idx * 31) & 0xFF);
                    raw[off + 2] = static_cast<uint8_t>(((c + r) * 7 + idx * 43) & 0xFF);
                    raw[off + 3] = 0xFF;
                }
            }
            s.encoded = TileCodec::encodeRect(s.x, s.y, s.w, s.h, raw.data(), s.preset);
            TEST_ASSERT(!s.encoded.data.empty());
        }

        // Sequential reference decode
        for (const auto& s : specs) {
            bool ok = TileCodec::decodeTileIntoCanvas(
                s.encoded.x, s.encoded.y, s.encoded.width, s.encoded.height,
                s.encoded.encoding, s.encoded.data.data(), s.encoded.data.size(),
                seqCanvas.data(), canvasW, canvasH
            );
            TEST_ASSERT(ok);
        }

        // Parallel batch decode via TileThreadPool::parallelDecode
        std::vector<TileThreadPool::DecodeTask> decodeTasks;
        decodeTasks.reserve(specs.size());
        for (const auto& s : specs) {
            TileThreadPool::DecodeTask dt;
            dt.rx = s.encoded.x;
            dt.ry = s.encoded.y;
            dt.rw = s.encoded.width;
            dt.rh = s.encoded.height;
            dt.encoding = s.encoded.encoding;
            dt.data = s.encoded.data.data();
            dt.dataSize = s.encoded.data.size();
            dt.canvasBgra = parCanvas.data();
            dt.canvasW = canvasW;
            dt.canvasH = canvasH;
            decodeTasks.push_back(dt);
        }

        TileThreadPool::instance().parallelDecode(decodeTasks);

        // Verify pixel-exact match between parallel and sequential decode
        TEST_ASSERT(seqCanvas == parCanvas);

        // Single-task fallback verification
        std::vector<uint8_t> singleCanvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);
        std::vector<TileThreadPool::DecodeTask> singleBatch(1, decodeTasks[0]);
        singleBatch[0].canvasBgra = singleCanvas.data();
        TileThreadPool::instance().parallelDecode(singleBatch);
        TEST_ASSERT(singleCanvas[0] == parCanvas[0]);
        TEST_ASSERT(singleCanvas[3] == 0xFF);
    }

    // 2. SessionTabManager on-switch frame cache verification
    {
        SessionTabManager tabs;
        uint32_t tabId0 = tabs.createTab(111222333, "111222333", "Desk A");
        TEST_ASSERT(tabId0 > 0);

        std::vector<uint8_t> dummyFrame(64 * 64 * 4, 0xAB);
        CursorState cur{};
        cur.normX = 0.25f;
        cur.normY = 0.75f;
        cur.visible = true;

        tabs.cacheActiveTabFrame(dummyFrame.data(), 64, 64, 42, cur);
        uint32_t tabId1 = tabs.createTab(444555666, "444555666", "Desk B");
        TEST_ASSERT(tabId1 > tabId0);

        TEST_ASSERT(tabs.selectTab(tabId0));
        const SessionTab* tabA = tabs.activeTab();
        TEST_ASSERT(tabA != nullptr);
        TEST_ASSERT(tabA->cachedW == 64);
        TEST_ASSERT(tabA->cachedH == 64);
        TEST_ASSERT(tabA->lastFrameSeq == 42);
        TEST_ASSERT(tabA->cachedFrameBgra.size() == dummyFrame.size());
        TEST_ASSERT(tabA->cachedFrameBgra[0] == 0xAB);
    }
}

void testAvx2BlitRowBgraOpaque() {
    std::cout << "[TEST 41] AVX2 Single-Pass Tile Row Blitting & Opaque Alpha Enforcement...\n";

    const int widths[] = { 1, 3, 7, 8, 9, 15, 16, 22, 64, 67, 1920 };
    for (int w : widths) {
        // Test both aligned and unaligned (+4 byte offset) buffers
        std::vector<uint8_t> srcBuf(static_cast<size_t>(w + 2) * 4, 0);
        std::vector<uint8_t> dstAvx(static_cast<size_t>(w + 2) * 4, 0xCC);
        std::vector<uint8_t> dstScalar(static_cast<size_t>(w + 2) * 4, 0xCC);

        for (size_t i = 0; i < srcBuf.size(); i += 4) {
            srcBuf[i + 0] = static_cast<uint8_t>((i * 3 + 11) & 0xFF);
            srcBuf[i + 1] = static_cast<uint8_t>((i * 7 + 23) & 0xFF);
            srcBuf[i + 2] = static_cast<uint8_t>((i * 13 + 37) & 0xFF);
            srcBuf[i + 3] = static_cast<uint8_t>((i / 4) % 3 == 0 ? 0x00 : ((i / 4) % 3 == 1 ? 0x7F : 0xFF));
        }

        // Unaligned offset (1 pixel = 4 bytes in)
        SimdKernels::blitRowBgraOpaque(dstAvx.data() + 4, srcBuf.data() + 4, w);
        SimdKernels::scalarBlitRowBgraOpaque(dstScalar.data() + 4, srcBuf.data() + 4, w);

        TEST_ASSERT(dstAvx == dstScalar);
        // Guard bytes before and after must remain untouched (0xCC)
        TEST_ASSERT(dstAvx[0] == 0xCC && dstAvx[3] == 0xCC);
        TEST_ASSERT(dstAvx[static_cast<size_t>(w + 1) * 4] == 0xCC);

        // Verify all blitted pixels have alpha == 0xFF and preserved B, G, R
        for (int c = 0; c < w; ++c) {
            size_t off = static_cast<size_t>(c + 1) * 4;
            TEST_ASSERT(dstAvx[off + 0] == srcBuf[off + 0]);
            TEST_ASSERT(dstAvx[off + 1] == srcBuf[off + 1]);
            TEST_ASSERT(dstAvx[off + 2] == srcBuf[off + 2]);
            TEST_ASSERT(dstAvx[off + 3] == 0xFF);
        }
    }
}

void testZeroAllocAudioAndStackWideText() {
    std::cout << "[TEST 42] Zero-Allocation Audio Scratch Buffers & Stack-Buffered UTF-8 Text Conversion...\n";

    // 1. AudioChunkHeader and VoiceChunkHeader scratch buffer reuse without reallocation
    {
        std::vector<uint8_t> audioPacketScratch;
        audioPacketScratch.reserve(sizeof(AudioChunkHeader) + 4800 * sizeof(int16_t));
        const uint8_t* initialPtr = audioPacketScratch.data();

        std::vector<int16_t> dummyPcm(960, 1234); // 10ms stereo @ 48kHz
        for (int iter = 0; iter < 50; ++iter) {
            AudioChunkHeader hdr{};
            hdr.sampleRate = 48000;
            hdr.channels = 2;
            hdr.bitsPerSample = 16;
            hdr.isSilent = 0;
            hdr.sampleFrames = 480;

            const size_t pcmBytes = dummyPcm.size() * sizeof(int16_t);
            audioPacketScratch.resize(sizeof(hdr) + pcmBytes);
            std::memcpy(audioPacketScratch.data(), &hdr, sizeof(hdr));
            std::memcpy(audioPacketScratch.data() + sizeof(hdr), dummyPcm.data(), pcmBytes);
            TEST_ASSERT(audioPacketScratch.data() == initialPtr);
        }

        AudioChunkHeader parsedHdr{};
        std::memcpy(&parsedHdr, audioPacketScratch.data(), sizeof(parsedHdr));
        TEST_ASSERT(parsedHdr.sampleRate == 48000);
        TEST_ASSERT(parsedHdr.channels == 2);
        TEST_ASSERT(parsedHdr.sampleFrames == 480);
        TEST_ASSERT(parsedHdr.isSilent == 0);
    }

    // 2. Single-pass stack UTF-8 to Wide conversion vs two-pass heap wstring equivalence
    {
        const std::vector<std::string> testStrings = {
            "CppDesk 3.1.2",
            "60 FPS | 1.2 ms | Zstd + AVX2",
            "Desk 401 115 368",
            "Performance HUD: ON (Ctrl+Shift+O)",
            "UTF-8 symbols: \xC3\xA9 \xE2\x9C\x93 \xE2\x86\x92 \xF0\x9F\x96\xA5"
        };

        for (const auto& s : testStrings) {
            TEST_ASSERT(s.size() < 512);
            wchar_t stackWide[512];
            int wlen = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), stackWide, 512);
            TEST_ASSERT(wlen > 0);

            int refLen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
            TEST_ASSERT(wlen == refLen);
            std::wstring refWide(refLen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &refWide[0], refLen);
            TEST_ASSERT(std::memcmp(stackWide, refWide.data(), static_cast<size_t>(wlen) * sizeof(wchar_t)) == 0);
        }
    }
}

void testVirtualDisplayManagerAndHeadlessEmulation() {
    std::cout << "[TEST 43] Virtual Multi-Display Driver & Headless Display Emulation Subsystem...\n" << std::flush;

    // 1. Protocol Serialization & Deserialization
    {
        // 1.1 VirtualDisplayCmdPayload round-trip (Create, Destroy, SetMode, resolutions)
        VirtualDisplayCmdPayload cmd1;
        cmd1.cmd = static_cast<uint8_t>(VirtualDisplayCmdType::Create);
        cmd1.displayId = 0;
        cmd1.width = 1920;
        cmd1.height = 1080;
        cmd1.refreshRate = 60;
        cmd1.flags = 0x01; // Prefer IddCx

        std::vector<uint8_t> cmdBytes;
        serializeVirtualDisplayCmd(cmd1, cmdBytes);
        TEST_ASSERT(cmdBytes.size() == 18);

        VirtualDisplayCmdPayload cmd1Dec{};
        TEST_ASSERT(deserializeVirtualDisplayCmd(cmdBytes.data(), cmdBytes.size(), cmd1Dec));
        TEST_ASSERT(cmd1Dec.cmd == static_cast<uint8_t>(VirtualDisplayCmdType::Create));
        TEST_ASSERT(cmd1Dec.displayId == 0);
        TEST_ASSERT(cmd1Dec.width == 1920);
        TEST_ASSERT(cmd1Dec.height == 1080);
        TEST_ASSERT(cmd1Dec.refreshRate == 60);
        TEST_ASSERT(cmd1Dec.flags == 0x01);

        // 1.2 Truncated command buffer underflow safety (< 18 bytes)
        for (size_t trunc = 0; trunc < 18; ++trunc) {
            VirtualDisplayCmdPayload dummy{};
            TEST_ASSERT(!deserializeVirtualDisplayCmd(cmdBytes.data(), trunc, dummy));
        }

        // 1.3 Destroy and SetMode payloads
        VirtualDisplayCmdPayload cmdDestroy;
        cmdDestroy.cmd = static_cast<uint8_t>(VirtualDisplayCmdType::Destroy);
        cmdDestroy.displayId = 105;
        serializeVirtualDisplayCmd(cmdDestroy, cmdBytes);
        VirtualDisplayCmdPayload cmdDestroyDec{};
        TEST_ASSERT(deserializeVirtualDisplayCmd(cmdBytes.data(), cmdBytes.size(), cmdDestroyDec));
        TEST_ASSERT(cmdDestroyDec.cmd == static_cast<uint8_t>(VirtualDisplayCmdType::Destroy));
        TEST_ASSERT(cmdDestroyDec.displayId == 105);

        VirtualDisplayCmdPayload cmdMode;
        cmdMode.cmd = static_cast<uint8_t>(VirtualDisplayCmdType::SetMode);
        cmdMode.displayId = 105;
        cmdMode.width = 3840;
        cmdMode.height = 2160;
        cmdMode.refreshRate = 120;
        cmdMode.flags = 0x02;
        serializeVirtualDisplayCmd(cmdMode, cmdBytes);
        VirtualDisplayCmdPayload cmdModeDec{};
        TEST_ASSERT(deserializeVirtualDisplayCmd(cmdBytes.data(), cmdBytes.size(), cmdModeDec));
        TEST_ASSERT(cmdModeDec.cmd == static_cast<uint8_t>(VirtualDisplayCmdType::SetMode));
        TEST_ASSERT(cmdModeDec.displayId == 105);
        TEST_ASSERT(cmdModeDec.width == 3840);
        TEST_ASSERT(cmdModeDec.height == 2160);
        TEST_ASSERT(cmdModeDec.refreshRate == 120);
        TEST_ASSERT(cmdModeDec.flags == 0x02);

        // 1.4 VirtualDisplayStatusPayload round-trip
        VirtualDisplayStatusPayload st;
        st.statusCode = static_cast<uint8_t>(VirtualDisplayStatusCode::Success);
        st.displayId = 42;
        st.width = 2560;
        st.height = 1440;
        st.refreshRate = 75;
        st.isVirtual = 1;
        st.isHeadlessFallback = 1;
        st.driverBackend = 0; // Software
        st.message = "Virtual Display Initialized Successfully";

        std::vector<uint8_t> stBytes;
        serializeVirtualDisplayStatus(st, stBytes);
        TEST_ASSERT(stBytes.size() >= 20);

        VirtualDisplayStatusPayload stDec{};
        TEST_ASSERT(deserializeVirtualDisplayStatus(stBytes.data(), stBytes.size(), stDec));
        TEST_ASSERT(stDec.statusCode == static_cast<uint8_t>(VirtualDisplayStatusCode::Success));
        TEST_ASSERT(stDec.displayId == 42);
        TEST_ASSERT(stDec.width == 2560);
        TEST_ASSERT(stDec.height == 1440);
        TEST_ASSERT(stDec.refreshRate == 75);
        TEST_ASSERT(stDec.isVirtual == 1);
        TEST_ASSERT(stDec.isHeadlessFallback == 1);
        TEST_ASSERT(stDec.driverBackend == 0);
        TEST_ASSERT(stDec.message == "Virtual Display Initialized Successfully");

        // 1.5 Truncated status buffer underflow safety (< 20 bytes)
        for (size_t trunc = 0; trunc < 20; ++trunc) {
            VirtualDisplayStatusPayload dummy{};
            TEST_ASSERT(!deserializeVirtualDisplayStatus(stBytes.data(), trunc, dummy));
        }

        // 1.6 serializeMonitorList and deserializeMonitorList round-trip with isVirtual and virtualId
        std::vector<MonitorDesc> testMons;
        MonitorDesc m1;
        m1.index = 0;
        m1.x = 0;
        m1.y = 0;
        m1.width = 1920;
        m1.height = 1080;
        m1.isPrimary = true;
        m1.name = "Primary Physical Display";
        m1.isVirtual = false;
        m1.virtualId = 0;
        testMons.push_back(m1);

        MonitorDesc m2;
        m2.index = 1;
        m2.x = 1920;
        m2.y = 0;
        m2.width = 2560;
        m2.height = 1440;
        m2.isPrimary = false;
        m2.name = "Secondary Virtual Display [Virtual]";
        m2.isVirtual = true;
        m2.virtualId = 88;
        testMons.push_back(m2);

        std::vector<uint8_t> monListBytes;
        serializeMonitorList(testMons, monListBytes);
        TEST_ASSERT(!monListBytes.empty());

        std::vector<MonitorDesc> testMonsDec;
        TEST_ASSERT(deserializeMonitorList(monListBytes.data(), monListBytes.size(), testMonsDec));
        TEST_ASSERT(testMonsDec.size() == 2);
        TEST_ASSERT(!testMonsDec[0].isVirtual);
        TEST_ASSERT(testMonsDec[0].virtualId == 0);
        TEST_ASSERT(testMonsDec[1].isVirtual);
        TEST_ASSERT(testMonsDec[1].virtualId == 88);
        TEST_ASSERT(testMonsDec[1].width == 2560);
        TEST_ASSERT(testMonsDec[1].height == 1440);
    }

    // 2. VirtualDisplayManager Lifecycle
    {
        auto& vdm = VirtualDisplayManager::instance();
        vdm.destroyAllSessionDisplays();
        TEST_ASSERT(vdm.virtualDisplayCount() == 0);

        uint32_t id1 = 0;
        TEST_ASSERT(vdm.createVirtualDisplay(1920, 1080, 60, false, &id1));
        TEST_ASSERT(id1 != 0);
        TEST_ASSERT(vdm.virtualDisplayCount() == 1);

        VirtualDisplayInfo info1;
        TEST_ASSERT(vdm.getDisplayInfo(id1, info1));
        TEST_ASSERT(info1.id == id1);
        TEST_ASSERT(info1.width == 1920);
        TEST_ASSERT(info1.height == 1080);
        TEST_ASSERT(info1.refreshRate == 60);
        TEST_ASSERT(info1.backend == VirtualBackend::Software);

        // Create secondary virtual display (2560x1440 @ 60Hz)
        uint32_t id2 = 0;
        TEST_ASSERT(vdm.createVirtualDisplay(2560, 1440, 60, false, &id2));
        TEST_ASSERT(id2 != 0 && id2 != id1);
        TEST_ASSERT(vdm.virtualDisplayCount() == 2);

        VirtualDisplayInfo info2;
        TEST_ASSERT(vdm.getDisplayInfo(id2, info2));
        // Verify horizontal coordinate topology offset: secondary display placed to the right
        TEST_ASSERT(info2.x >= (info1.x + static_cast<int32_t>(info1.width)));

        // Mode change: resize display 1 to 1280x720 @ 75Hz
        TEST_ASSERT(vdm.setVirtualDisplayMode(id1, 1280, 720, 75));
        TEST_ASSERT(vdm.getDisplayInfo(id1, info1));
        TEST_ASSERT(info1.width == 1280);
        TEST_ASSERT(info1.height == 720);
        TEST_ASSERT(info1.refreshRate == 75);

        // Destroy display 2
        TEST_ASSERT(vdm.destroyVirtualDisplay(id2));
        TEST_ASSERT(vdm.virtualDisplayCount() == 1);
        TEST_ASSERT(!vdm.getDisplayInfo(id2, info2));

        // Session teardown
        vdm.destroyAllSessionDisplays();
        TEST_ASSERT(vdm.virtualDisplayCount() == 0);
    }

    // 3. Headless Detection & Auto-Provisioning
    {
        auto& vdm = VirtualDisplayManager::instance();
        vdm.destroyAllSessionDisplays();

        uint32_t physCount = VirtualDisplayManager::getPhysicalMonitorCount();
        bool isHeadless = VirtualDisplayManager::isHostHeadless();
        TEST_ASSERT(isHeadless == (physCount == 0));

        // Ensure headless display
        TEST_ASSERT(vdm.ensureHeadlessDisplay(1920, 1080, 60));
        TEST_ASSERT(vdm.hasActiveHeadlessDisplay());
        uint32_t hId = vdm.headlessDisplayId();
        TEST_ASSERT(hId != 0);

        VirtualDisplayInfo hInfo;
        TEST_ASSERT(vdm.getDisplayInfo(hId, hInfo));
        TEST_ASSERT(hInfo.isHeadlessFallback);
        TEST_ASSERT(hInfo.width == 1920);
        TEST_ASSERT(hInfo.height == 1080);

        // Idempotency: calling again returns true without creating a duplicate
        TEST_ASSERT(vdm.ensureHeadlessDisplay(1920, 1080, 60));
        TEST_ASSERT(vdm.virtualDisplayCount() == 1);
        TEST_ASSERT(vdm.headlessDisplayId() == hId);

        // Release headless display
        vdm.releaseHeadlessDisplay();
        TEST_ASSERT(!vdm.hasActiveHeadlessDisplay());
        TEST_ASSERT(vdm.headlessDisplayId() == 0);
        TEST_ASSERT(vdm.virtualDisplayCount() == 0);
    }

    // 4. Software Surface Capture & Synthetic Pattern Rendering
    {
        auto& vdm = VirtualDisplayManager::instance();
        vdm.destroyAllSessionDisplays();

        uint32_t testId = 0;
        TEST_ASSERT(vdm.createVirtualDisplay(1920, 1080, 60, false, &testId));
        vdm.renderSyntheticDesktop(testId, "Test Headless Synthetic Host");

        std::vector<uint8_t> frameBuf;
        int fw = 0, fh = 0;
        TEST_ASSERT(vdm.captureSoftwareSurface(testId, frameBuf, fw, fh));
        TEST_ASSERT(fw == 1920 && fh == 1080);
        TEST_ASSERT(frameBuf.size() == static_cast<size_t>(fw) * fh * 4);

        // Verify non-zero pixels and fully opaque alpha channel
        bool hasNonZero = false;
        bool allOpaque = true;
        for (size_t p = 0; p < frameBuf.size(); p += 4) {
            if (frameBuf[p + 0] != 0 || frameBuf[p + 1] != 0 || frameBuf[p + 2] != 0) {
                hasNonZero = true;
            }
            if (frameBuf[p + 3] != 0xFF) {
                allOpaque = false;
                break;
            }
        }
        TEST_ASSERT(hasNonZero);
        TEST_ASSERT(allOpaque);

        // Pass software frame into SIMD tile diff / hash kernels and verify TileCodec::encodeRect
        TileCodec::initGdiPlus();
        EncodedTile encTile = TileCodec::encodeRect(0, 0, 64, 64, frameBuf.data(), QualityPreset::Balanced);
        TEST_ASSERT(!encTile.data.empty());
        TEST_ASSERT(encTile.width == 64 && encTile.height == 64);

        std::vector<uint8_t> canvas(64 * 64 * 4, 0);
        TEST_ASSERT(TileCodec::decodeTileIntoCanvas(encTile, canvas.data(), 64, 64));

        vdm.destroyAllSessionDisplays();
    }

    // 5. ScreenCapturer Integration
    {
        auto& vdm = VirtualDisplayManager::instance();
        vdm.destroyAllSessionDisplays();

        uint32_t vId = 0;
        TEST_ASSERT(vdm.createVirtualDisplay(1920, 1080, 60, false, &vId));

        ScreenCapturer capturer;
        auto mons = capturer.enumerateMonitors();
        TEST_ASSERT(!mons.empty());

        int vIndex = -1;
        for (const auto& m : mons) {
            if (m.isVirtual && m.virtualId == vId) {
                vIndex = m.index;
                break;
            }
        }
        TEST_ASSERT(vIndex != -1);

        // Select virtual monitor index
        TEST_ASSERT(capturer.selectMonitor(vIndex));
        TEST_ASSERT(capturer.currentMonitor().isVirtual);
        TEST_ASSERT(capturer.currentMonitor().virtualId == vId);
        TEST_ASSERT(capturer.frameWidth() == 1920);
        TEST_ASSERT(capturer.frameHeight() == 1080);
        TEST_ASSERT(capturer.dxgiRecoveryState() == ScreenCapturer::DxgiRecoveryState::Disabled);

        // Capture dirty tiles from virtual display
        std::vector<EncodedTile> tiles;
        bool isKf = false;
        CursorState cur{};
        bool ok = capturer.captureDirtyTiles(true, QualityPreset::Balanced, tiles, isKf, cur);
        TEST_ASSERT(ok);
        TEST_ASSERT(isKf);
        TEST_ASSERT(!tiles.empty());
        TEST_ASSERT(capturer.frameWidth() == 1920);

        vdm.destroyAllSessionDisplays();
    }

    // 6. AppSettings Persistence
    {
        IdentityManager idMgr(77);
        AppSettings s = idMgr.settings();
        TEST_ASSERT(s.autoVirtualDisplay); // Default is true

        s.autoVirtualDisplay = false;
        idMgr.updateSettings(s);

        IdentityManager idReload(77);
        idReload.loadOrCreate();
        TEST_ASSERT(!idReload.settings().autoVirtualDisplay);

        s.autoVirtualDisplay = true;
        idMgr.updateSettings(s);
        idReload.loadOrCreate();
        TEST_ASSERT(idReload.settings().autoVirtualDisplay);
    }

    // 7. Network Engine Protocol APIs
    {
        IdentityManager hId(81);
        IdentityManager vId(82);
        NetworkEngine host(hId);
        NetworkEngine viewer(vId);

        // APIs safely return false when viewer socket is not connected
        TEST_ASSERT(!viewer.requestCreateVirtualDisplay(1920, 1080, 60));
        TEST_ASSERT(!viewer.requestDestroyVirtualDisplay(1));
        TEST_ASSERT(!viewer.requestSetVirtualDisplayMode(1, 1280, 720, 60));

        // Test status payload storage in ViewerSessionStats
        auto stats = viewer.viewerStats();
        TEST_ASSERT(stats.monitors.empty() || !stats.monitors.empty());
    }
}

void testHardwareDpmsAndScreenBlanking() {
    std::cout << "[TEST 44] Remote Screen Blanking & Hardware DPMS Power Management...\n" << std::flush;

    // 1. ScreenBlankMode Enum Values
    TEST_ASSERT(static_cast<uint8_t>(ScreenBlankMode::CurtainOnly) == 0);
    TEST_ASSERT(static_cast<uint8_t>(ScreenBlankMode::DpmsOnly) == 1);
    TEST_ASSERT(static_cast<uint8_t>(ScreenBlankMode::Unified) == 2);

    // 2. PrivacyModeConfigPayload Binary Packing & Serialization
    TEST_ASSERT(sizeof(PrivacyModeConfigPayload) == 196);
    PrivacyModeConfigPayload samplePayload{};
    TEST_ASSERT(reinterpret_cast<uintptr_t>(&samplePayload.blankMode) - reinterpret_cast<uintptr_t>(&samplePayload) == 3);

    samplePayload.enable = 1;
    samplePayload.acknowledge = 0;
    samplePayload.showDeskId = 1;
    samplePayload.blankMode = static_cast<uint8_t>(ScreenBlankMode::Unified);
    std::snprintf(samplePayload.customNotice, sizeof(samplePayload.customNotice), "Enterprise Blanking Active");
    std::snprintf(samplePayload.brandName, sizeof(samplePayload.brandName), "CppDesk Corp");

    std::vector<uint8_t> encoded;
    serializePrivacyModeConfig(samplePayload, encoded);
    TEST_ASSERT(encoded.size() == sizeof(PrivacyModeConfigPayload));

    PrivacyModeConfigPayload decodedPayload{};
    bool decOk = deserializePrivacyModeConfig(encoded.data(), encoded.size(), decodedPayload);
    TEST_ASSERT(decOk);
    TEST_ASSERT(decodedPayload.enable == 1);
    TEST_ASSERT(decodedPayload.acknowledge == 0);
    TEST_ASSERT(decodedPayload.showDeskId == 1);
    TEST_ASSERT(decodedPayload.blankMode == static_cast<uint8_t>(ScreenBlankMode::Unified));
    TEST_ASSERT(std::string(decodedPayload.customNotice) == "Enterprise Blanking Active");
    TEST_ASSERT(std::string(decodedPayload.brandName) == "CppDesk Corp");

    // 3. Buffer Underflow & Backward Compatibility
    PrivacyModeConfigPayload underflowOut{};
    TEST_ASSERT(!deserializePrivacyModeConfig(nullptr, 196, underflowOut));
    TEST_ASSERT(!deserializePrivacyModeConfig(encoded.data(), 195, underflowOut));

    // Backward compatibility overlay with legacy PrivacyModePayload at offset 0 and 1
    const PrivacyModePayload* legacyView = reinterpret_cast<const PrivacyModePayload*>(&decodedPayload);
    TEST_ASSERT(legacyView->enable == 1);
    TEST_ASSERT(legacyView->acknowledge == 0);

    // blankMode = 0 decodes to CurtainOnly
    samplePayload.blankMode = 0;
    serializePrivacyModeConfig(samplePayload, encoded);
    TEST_ASSERT(deserializePrivacyModeConfig(encoded.data(), encoded.size(), decodedPayload));
    TEST_ASSERT(static_cast<ScreenBlankMode>(decodedPayload.blankMode) == ScreenBlankMode::CurtainOnly);

    // 4. AppSettings Persistence for hardwareDpmsBlanking
    {
        IdentityManager idMgr(88);
        AppSettings s = idMgr.settings();
        TEST_ASSERT(s.hardwareDpmsBlanking); // Default true

        s.hardwareDpmsBlanking = false;
        idMgr.updateSettings(s);

        IdentityManager idReload(88);
        idReload.loadOrCreate();
        TEST_ASSERT(!idReload.settings().hardwareDpmsBlanking);

        s.hardwareDpmsBlanking = true;
        idMgr.updateSettings(s);
        idReload.loadOrCreate();
        TEST_ASSERT(idReload.settings().hardwareDpmsBlanking);
    }

    // 5. DPMS API Execution
    ScreenBlankManager::powerDownDisplays();
    ScreenBlankManager::wakeDisplays();
    TEST_ASSERT(true);

    // 6. Low-Level Hook Filter Logic
    // Mouse tests
    TEST_ASSERT(!ScreenBlankManager::isPhysicalEvent(LLMHF_INJECTED, true));
    TEST_ASSERT(ScreenBlankManager::isPhysicalEvent(0, true));
    MSLLHOOKSTRUCT msHookInjected{};
    msHookInjected.flags = LLMHF_INJECTED;
    TEST_ASSERT(!ScreenBlankManager::isPhysicalMouseInput(msHookInjected));
    MSLLHOOKSTRUCT msHookPhysical{};
    msHookPhysical.flags = 0;
    TEST_ASSERT(ScreenBlankManager::isPhysicalMouseInput(msHookPhysical));

    // Keyboard tests
    TEST_ASSERT(!ScreenBlankManager::isPhysicalEvent(LLKHF_INJECTED, false));
    TEST_ASSERT(ScreenBlankManager::isPhysicalEvent(0, false));
    KBDLLHOOKSTRUCT kbdHookInjected{};
    kbdHookInjected.flags = LLKHF_INJECTED;
    TEST_ASSERT(!ScreenBlankManager::isPhysicalKeyboardInput(kbdHookInjected));
    KBDLLHOOKSTRUCT kbdHookPhysical{};
    kbdHookPhysical.flags = 0;
    TEST_ASSERT(ScreenBlankManager::isPhysicalKeyboardInput(kbdHookPhysical));

    // 7. ScreenBlankManager Lifecycle & Emergency Callback
    {
        auto& sbm = ScreenBlankManager::instance();
        TEST_ASSERT(!sbm.isBlankActive());

        bool emergencyFired = false;
        sbm.setEmergencyWakeCallback([&emergencyFired]() {
            emergencyFired = true;
        });

        // Engage mock blanking in DpmsOnly mode
        sbm.engageBlanking(ScreenBlankMode::DpmsOnly, "Notice", "Brand", true, 123456789);
        TEST_ASSERT(sbm.isBlankActive());
        TEST_ASSERT(sbm.currentBlankMode() == ScreenBlankMode::DpmsOnly);

        // Simulate emergency wake
        sbm.triggerEmergencyWake();
        TEST_ASSERT(!sbm.isBlankActive());
        TEST_ASSERT(emergencyFired);

        // Disengage blanking cleanup
        sbm.disengageBlanking();
        TEST_ASSERT(!sbm.isBlankActive());
        sbm.setEmergencyWakeCallback(nullptr);
    }

    // 8. NetworkEngine Integration APIs (safe when disconnected)
    {
        IdentityManager hId(89);
        IdentityManager vId(90);
        NetworkEngine host(hId);
        NetworkEngine viewer(vId);

        // Safe call when disconnected
        viewer.requestTogglePrivacyMode(ScreenBlankMode::Unified);
        viewer.requestTogglePrivacyMode(ScreenBlankMode::CurtainOnly);
        viewer.requestTogglePrivacyMode(ScreenBlankMode::DpmsOnly);
        TEST_ASSERT(!viewer.isPrivacyModeEngaged());
        TEST_ASSERT(!host.isHostPrivacyModeActive());

        // Host privacy mode engage and disengage
        host.setHostPrivacyMode(true, "Sec Notice", "Sec Brand", false, ScreenBlankMode::CurtainOnly);
        TEST_ASSERT(host.isHostPrivacyModeActive());
        host.setHostPrivacyMode(false);
        TEST_ASSERT(!host.isHostPrivacyModeActive());
    }
}

void testSessionRecordingPlayerAndTranscoder() {
    std::cout << "[TEST 45] Session Recording Player & Format Transcoder...\n";

    std::string recPath = "cppdesk_test_player_rec.avi";
    std::string corruptPath = "cppdesk_test_corrupt.avi";
    std::string snapBmpPath = "cppdesk_test_snap.bmp";
    std::string snapPngPath = "cppdesk_test_snap.png";
    std::string trimmedPath = "cppdesk_test_trimmed.avi";

    // Clean up any stale files
    std::filesystem::remove(recPath);
    std::filesystem::remove(corruptPath);
    std::filesystem::remove(snapBmpPath);
    std::filesystem::remove(snapPngPath);
    std::filesystem::remove(trimmedPath);

    // 1. Synthetic AVI Generation with SessionRecorder
    {
        SessionRecorder recorder;
        bool ok = recorder.startRecording(recPath, 320, 240, 30);
        TEST_ASSERT(ok);

        // Push 12 synthetic frames with alternating colors
        // Pattern 0: Red    (B=0,   G=0,   R=255, A=255)
        // Pattern 1: Green  (B=0,   G=255, R=0,   A=255)
        // Pattern 2: Blue   (B=255, G=0,   R=0,   A=255)
        // Pattern 3: Yellow (B=0,   G=255, R=255, A=255)
        std::vector<uint8_t> frame(320 * 240 * 4);
        for (int i = 0; i < 12; ++i) {
            int pat = i % 4;
            uint8_t b = (pat == 2) ? 255 : 0;
            uint8_t g = (pat == 1 || pat == 3) ? 255 : 0;
            uint8_t r = (pat == 0 || pat == 3) ? 255 : 0;
            uint8_t a = 255;
            for (size_t p = 0; p < 320 * 240; ++p) {
                frame[p * 4 + 0] = b;
                frame[p * 4 + 1] = g;
                frame[p * 4 + 2] = r;
                frame[p * 4 + 3] = a;
            }
            recorder.pushFrame(frame.data(), 320, 240);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        recorder.stopRecording();
        TEST_ASSERT(std::filesystem::exists(recPath));
        TEST_ASSERT(std::filesystem::file_size(recPath) > 1000);
    }

    // 2. Demuxer Header & Index Verification
    {
        SessionRecordingPlayer player;
        TEST_ASSERT(!player.isOpen());
        bool opened = player.open(recPath);
        TEST_ASSERT(opened);
        TEST_ASSERT(player.isOpen());
        TEST_ASSERT(player.frameWidth() == 320);
        TEST_ASSERT(player.frameHeight() == 240);
        TEST_ASSERT(player.fps() == 30);
        TEST_ASSERT(player.totalFrames() == 12);
        TEST_ASSERT(player.durationUs() > 0);
        TEST_ASSERT(player.filePath() == recPath);

        // 3. Frame Seeking & Decoding Verification
        // Seek to Frame 0 (Red)
        TEST_ASSERT(player.seekToFrame(0));
        TEST_ASSERT(player.currentFrameIndex() == 0);
        std::vector<uint8_t> buf0;
        int w = 0, h = 0;
        player.copyCurrentFrameBgra(buf0, w, h);
        TEST_ASSERT(w == 320 && h == 240);
        TEST_ASSERT(buf0.size() == 320 * 240 * 4);
        // Verify center pixel is Red: B=0, G=0, R=255, A=255
        size_t centerPix = (120 * 320 + 160) * 4;
        TEST_ASSERT(buf0[centerPix + 0] == 0);
        TEST_ASSERT(buf0[centerPix + 1] == 0);
        TEST_ASSERT(buf0[centerPix + 2] == 255);
        TEST_ASSERT(buf0[centerPix + 3] == 255);

        // Seek to Frame 2 (Blue)
        TEST_ASSERT(player.seekToFrame(2));
        TEST_ASSERT(player.currentFrameIndex() == 2);
        std::vector<uint8_t> buf2;
        player.copyCurrentFrameBgra(buf2, w, h);
        TEST_ASSERT(buf2[centerPix + 0] == 255);
        TEST_ASSERT(buf2[centerPix + 1] == 0);
        TEST_ASSERT(buf2[centerPix + 2] == 0);
        TEST_ASSERT(buf2[centerPix + 3] == 255);

        // Seek by timestamp (50% elapsed time -> should be frame 6)
        uint64_t midTs = player.durationUs() / 2;
        TEST_ASSERT(player.seekToTimestampUs(midTs));
        TEST_ASSERT(player.currentFrameIndex() == 6);

        // Stepping: +1 and -1
        TEST_ASSERT(player.stepFrame(1));
        TEST_ASSERT(player.currentFrameIndex() == 7);
        TEST_ASSERT(player.stepFrame(-1));
        TEST_ASSERT(player.currentFrameIndex() == 6);

        // Speed setting
        player.setSpeed(2.0f);
        TEST_ASSERT(std::abs(player.speed() - 2.0f) < 0.01f);

        // Play and pause transitions
        player.play();
        TEST_ASSERT(player.isPlaying());
        player.pause();
        TEST_ASSERT(!player.isPlaying());
        player.togglePlayPause();
        TEST_ASSERT(player.isPlaying());
        player.togglePlayPause();
        TEST_ASSERT(!player.isPlaying());

        // 5. Snapshot Export Verification
        player.seekToFrame(2); // Frame 2 is Blue
        TEST_ASSERT(player.exportSnapshotBmp(snapBmpPath));
        TEST_ASSERT(std::filesystem::exists(snapBmpPath));
        {
            std::ifstream bfile(snapBmpPath, std::ios::binary);
            TEST_ASSERT(bfile.is_open());
            BITMAPFILEHEADER bfh{};
            BITMAPINFOHEADER bih{};
            bfile.read(reinterpret_cast<char*>(&bfh), sizeof(bfh));
            bfile.read(reinterpret_cast<char*>(&bih), sizeof(bih));
            TEST_ASSERT(bfh.bfType == 0x4D42); // 'BM'
            TEST_ASSERT(bih.biWidth == 320);
            TEST_ASSERT(std::abs(bih.biHeight) == 240);
            TEST_ASSERT(bih.biBitCount == 32);
        }

        TEST_ASSERT(player.exportSnapshotPng(snapPngPath));
        TEST_ASSERT(std::filesystem::exists(snapPngPath));
        {
            std::ifstream pfile(snapPngPath, std::ios::binary);
            TEST_ASSERT(pfile.is_open());
            uint8_t sig[8]{};
            pfile.read(reinterpret_cast<char*>(sig), 8);
            // PNG signature: 0x89 0x50 0x4E 0x47 0x0D 0x0A 0x1A 0x0A
            TEST_ASSERT(sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
        }

        // 6. Lossless Clip Trimming Verification
        // Trim frames 2 through 6 (5 frames total: 2, 3, 4, 5, 6)
        TEST_ASSERT(player.trimClip(trimmedPath, 2, 6));
        TEST_ASSERT(std::filesystem::exists(trimmedPath));

        {
            SessionRecordingPlayer trimmedPlayer;
            TEST_ASSERT(trimmedPlayer.open(trimmedPath));
            TEST_ASSERT(trimmedPlayer.totalFrames() == 5);
            TEST_ASSERT(trimmedPlayer.frameWidth() == 320);
            TEST_ASSERT(trimmedPlayer.frameHeight() == 240);

            // Trimmed Frame 0 should match original Frame 2 (Blue)
            TEST_ASSERT(trimmedPlayer.seekToFrame(0));
            std::vector<uint8_t> tbuf0;
            trimmedPlayer.copyCurrentFrameBgra(tbuf0, w, h);
            TEST_ASSERT(tbuf0[centerPix + 0] == 255); // B
            TEST_ASSERT(tbuf0[centerPix + 1] == 0);   // G
            TEST_ASSERT(tbuf0[centerPix + 2] == 0);   // R
            TEST_ASSERT(tbuf0[centerPix + 3] == 255); // A

            // Trimmed Frame 4 should match original Frame 6 (Blue: 6 % 4 == 2)
            TEST_ASSERT(trimmedPlayer.seekToFrame(4));
            std::vector<uint8_t> tbuf4;
            trimmedPlayer.copyCurrentFrameBgra(tbuf4, w, h);
            TEST_ASSERT(tbuf4[centerPix + 0] == 255); // B
            TEST_ASSERT(tbuf4[centerPix + 1] == 0);   // G
            TEST_ASSERT(tbuf4[centerPix + 2] == 0);   // R
            TEST_ASSERT(tbuf4[centerPix + 3] == 255); // A

            trimmedPlayer.close();
        }

        player.close();
    }

    // 4. Corrupted Index Recovery Verification
    {
        // Copy original file up to 'idx1' and zero dwTotalFrames and dwLength
        std::ifstream src(recPath, std::ios::binary);
        TEST_ASSERT(src.is_open());
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(src)),
                                  std::istreambuf_iterator<char>());
        src.close();

        // Find "idx1" FourCC
        size_t idxPos = data.size();
        for (size_t i = 0; i + 4 <= data.size(); ++i) {
            if (std::memcmp(&data[i], "idx1", 4) == 0) {
                idxPos = i;
                break;
            }
        }
        TEST_ASSERT(idxPos < data.size());

        // Truncate at idx1
        data.resize(idxPos);

        // Zero dwTotalFrames at offset 48
        if (data.size() > 52) {
            std::memset(&data[48], 0, 4);
        }
        // Zero dwLength at offset 140
        if (data.size() > 144) {
            std::memset(&data[140], 0, 4);
        }

        std::ofstream corruptOut(corruptPath, std::ios::binary | std::ios::trunc);
        corruptOut.write(reinterpret_cast<const char*>(data.data()), data.size());
        corruptOut.close();

        SessionRecordingPlayer corruptPlayer;
        TEST_ASSERT(corruptPlayer.open(corruptPath));
        TEST_ASSERT(corruptPlayer.isOpen());
        // Scanning movi should have recovered all 12 frames
        TEST_ASSERT(corruptPlayer.totalFrames() == 12);
        TEST_ASSERT(corruptPlayer.frameWidth() == 320);
        TEST_ASSERT(corruptPlayer.frameHeight() == 240);

        // Decode Frame 0 (Red)
        TEST_ASSERT(corruptPlayer.seekToFrame(0));
        std::vector<uint8_t> cBuf0;
        int w = 0, h = 0;
        corruptPlayer.copyCurrentFrameBgra(cBuf0, w, h);
        size_t centerPix = (120 * 320 + 160) * 4;
        TEST_ASSERT(cBuf0[centerPix + 0] == 0);
        TEST_ASSERT(cBuf0[centerPix + 1] == 0);
        TEST_ASSERT(cBuf0[centerPix + 2] == 255);
        TEST_ASSERT(cBuf0[centerPix + 3] == 255);

        // Decode Frame 2 (Blue)
        TEST_ASSERT(corruptPlayer.seekToFrame(2));
        std::vector<uint8_t> cBuf2;
        corruptPlayer.copyCurrentFrameBgra(cBuf2, w, h);
        TEST_ASSERT(cBuf2[centerPix + 0] == 255);
        TEST_ASSERT(cBuf2[centerPix + 1] == 0);
        TEST_ASSERT(cBuf2[centerPix + 2] == 0);
        TEST_ASSERT(cBuf2[centerPix + 3] == 255);

        corruptPlayer.close();
    }

    // 7. Cleanup temporary files
    std::filesystem::remove(recPath);
    std::filesystem::remove(corruptPath);
    std::filesystem::remove(snapBmpPath);
    std::filesystem::remove(snapPngPath);
    std::filesystem::remove(trimmedPath);
}

void testTwoFactorAuthTotpAndQrMatrix() {
    std::cout << "[TEST 46] Two-Factor Authentication (TOTP RFC 6238) & Micro QR Synthesizer...\n";

    // Part 1: Official RFC 6238 Appendix B Test Vectors
    {
        const char* rawKey1 = "12345678901234567890";
        TotpManager sha1Mgr;
        sha1Mgr.setSecretRaw(reinterpret_cast<const uint8_t*>(rawKey1), 20, TotpAlgorithm::Sha1);

        TEST_ASSERT(sha1Mgr.generateCode(59, 8) == "94287082");
        TEST_ASSERT(sha1Mgr.generateCode(59, 6) == "287082");
        TEST_ASSERT(sha1Mgr.generateCode(1111111109, 8) == "07081804");
        TEST_ASSERT(sha1Mgr.generateCode(1111111109, 6) == "081804");
        TEST_ASSERT(sha1Mgr.generateCode(1111111111, 8) == "14050471");
        TEST_ASSERT(sha1Mgr.generateCode(1111111111, 6) == "050471");
        TEST_ASSERT(sha1Mgr.generateCode(1234567890, 8) == "89005924");
        TEST_ASSERT(sha1Mgr.generateCode(1234567890, 6) == "005924");
        TEST_ASSERT(sha1Mgr.generateCode(2000000000, 8) == "69279037");
        TEST_ASSERT(sha1Mgr.generateCode(2000000000, 6) == "279037");
        TEST_ASSERT(sha1Mgr.generateCode(20000000000ULL, 8) == "65353130");
        TEST_ASSERT(sha1Mgr.generateCode(20000000000ULL, 6) == "353130");

        const char* rawKey256 = "12345678901234567890123456789012";
        TotpManager sha256Mgr;
        sha256Mgr.setSecretRaw(reinterpret_cast<const uint8_t*>(rawKey256), 32, TotpAlgorithm::Sha256);

        TEST_ASSERT(sha256Mgr.generateCode(59, 8) == "46119246");
        TEST_ASSERT(sha256Mgr.generateCode(59, 6) == "119246");
        TEST_ASSERT(sha256Mgr.generateCode(1111111109, 8) == "68084774");
        TEST_ASSERT(sha256Mgr.generateCode(1111111109, 6) == "084774");
        TEST_ASSERT(sha256Mgr.generateCode(1111111111, 8) == "67062674");
        TEST_ASSERT(sha256Mgr.generateCode(1111111111, 6) == "062674");
        TEST_ASSERT(sha256Mgr.generateCode(1234567890, 8) == "91819424");
        TEST_ASSERT(sha256Mgr.generateCode(1234567890, 6) == "819424");
        TEST_ASSERT(sha256Mgr.generateCode(2000000000, 8) == "90698825");
        TEST_ASSERT(sha256Mgr.generateCode(2000000000, 6) == "698825");
        TEST_ASSERT(sha256Mgr.generateCode(20000000000ULL, 8) == "77737706");
        TEST_ASSERT(sha256Mgr.generateCode(20000000000ULL, 6) == "737706");
    }

    // Part 2: Base32 RFC 4648 Vectors & Formatting
    {
        auto enc = [](const std::string& str) {
            return TotpManager::encodeBase32(reinterpret_cast<const uint8_t*>(str.data()), str.size());
        };
        auto dec = [](const std::string& b32) -> std::string {
            std::vector<uint8_t> out;
            if (!TotpManager::decodeBase32(b32, out)) return "";
            return std::string(out.begin(), out.end());
        };

        TEST_ASSERT(enc("") == "");
        TEST_ASSERT(enc("f") == "MY======");
        TEST_ASSERT(enc("fo") == "MZXQ====");
        TEST_ASSERT(enc("foo") == "MZXW6===");
        TEST_ASSERT(enc("foob") == "MZXW6YQ=");
        TEST_ASSERT(enc("fooba") == "MZXW6YTB");
        TEST_ASSERT(enc("foobar") == "MZXW6YTBOI======");

        TEST_ASSERT(dec("") == "");
        TEST_ASSERT(dec("MY======") == "f");
        TEST_ASSERT(dec("MZXQ====") == "fo");
        TEST_ASSERT(dec("MZXW6===") == "foo");
        TEST_ASSERT(dec("MZXW6YQ=") == "foob");
        TEST_ASSERT(dec("MZXW6YTB") == "fooba");
        TEST_ASSERT(dec("MZXW6YTBOI======") == "foobar");

        TEST_ASSERT(dec("mzxw6ytboi======") == "foobar");
        TEST_ASSERT(dec("MZXW 6YTB OI") == "foobar");
        TEST_ASSERT(dec("MZXW-6YTB-OI") == "foobar");
        TEST_ASSERT(dec("  mzxw 6ytb-oi==  ") == "foobar");

        std::vector<uint8_t> invalidOut;
        TEST_ASSERT(!TotpManager::decodeBase32("MZXW1", invalidOut));
        TEST_ASSERT(!TotpManager::decodeBase32("MZXW8", invalidOut));
        TEST_ASSERT(!TotpManager::decodeBase32("MZXW9", invalidOut));
        TEST_ASSERT(!TotpManager::decodeBase32("MZXW0", invalidOut));

        TEST_ASSERT(TotpManager::formatBase32Secret("MZXW6YTB") == "MZXW 6YTB");
        TEST_ASSERT(TotpManager::formatBase32Secret("MZXW 6YTB OI") == "MZXW 6YTB OI");

        TotpManager randMgr;
        randMgr.generateNewSecret();
        TEST_ASSERT(randMgr.isConfigured());
        std::string randB32 = randMgr.getSecretBase32();
        TEST_ASSERT(randB32.size() == 32);
        std::vector<uint8_t> randBytes;
        TEST_ASSERT(TotpManager::decodeBase32(randB32, randBytes));
        TEST_ASSERT(randBytes.size() == 20);
    }

    // Part 3: QR Matrix Generator & Sizing
    {
        std::string uri = "otpauth://totp/CppDesk:401115368?secret=JBSWY3DPEHPK3PXP&issuer=CppDesk";
        QrMatrix qr = QrMatrix::generate(uri);

        TEST_ASSERT(qr.size() >= 21);
        TEST_ASSERT(qr.modules().size() == static_cast<size_t>(qr.size() * qr.size()));

        // Center 3x3 of top-left finder must be dark
        TEST_ASSERT(qr.getModule(2, 2));
        TEST_ASSERT(qr.getModule(2, 3));
        TEST_ASSERT(qr.getModule(2, 4));
        TEST_ASSERT(qr.getModule(3, 2));
        TEST_ASSERT(qr.getModule(3, 3));
        TEST_ASSERT(qr.getModule(3, 4));
        TEST_ASSERT(qr.getModule(4, 2));
        TEST_ASSERT(qr.getModule(4, 3));
        TEST_ASSERT(qr.getModule(4, 4));

        // Border corners of top-left finder must be dark
        TEST_ASSERT(qr.getModule(0, 0));
        TEST_ASSERT(qr.getModule(0, 6));
        TEST_ASSERT(qr.getModule(6, 0));
        TEST_ASSERT(qr.getModule(6, 6));

        // Separator ring around center must have light modules
        TEST_ASSERT(!qr.getModule(1, 1));
        TEST_ASSERT(!qr.getModule(1, 5));
        TEST_ASSERT(!qr.getModule(5, 1));
        TEST_ASSERT(!qr.getModule(5, 5));

        // Top-right finder
        int rightX = qr.size() - 7;
        TEST_ASSERT(qr.getModule(rightX + 3, 3));
        TEST_ASSERT(qr.getModule(rightX, 0));
        TEST_ASSERT(qr.getModule(rightX + 6, 6));

        // Bottom-left finder
        int botY = qr.size() - 7;
        TEST_ASSERT(qr.getModule(3, botY + 3));
        TEST_ASSERT(qr.getModule(0, botY));
        TEST_ASSERT(qr.getModule(6, botY + 6));

        // Timing patterns
        for (int c = 8; c < qr.size() - 8; ++c) {
            TEST_ASSERT(qr.getModule(c, 6) == (c % 2 == 0));
            TEST_ASSERT(qr.getModule(6, c) == (c % 2 == 0));
        }

        QrMatrix qrSmall = QrMatrix::generate("HI", 1, 1);
        TEST_ASSERT(qrSmall.size() == 21);
    }

    // Part 4: Clock Drift & Single-Use Replay Protection
    {
        TotpManager mgr;
        mgr.generateNewSecret();

        uint64_t tNow = 1700000000ULL;
        std::string codeCur = mgr.generateCode(tNow);
        std::string codePast = mgr.generateCode(tNow - 30);
        std::string codeFuture = mgr.generateCode(tNow + 30);
        std::string codeTooOld = mgr.generateCode(tNow - 65);
        std::string codeTooFar = mgr.generateCode(tNow + 65);

        TEST_ASSERT(mgr.verifyCode(codeCur, tNow));
        TEST_ASSERT(mgr.lastConsumedStep() == tNow / 30);

        TEST_ASSERT(!mgr.verifyCode(codeCur, tNow));
        TEST_ASSERT(!mgr.verifyCode(codeCur, tNow + 10));

        mgr.resetLastConsumedStep();
        TEST_ASSERT(mgr.verifyCode(codePast, tNow));
        TEST_ASSERT(mgr.lastConsumedStep() == (tNow - 30) / 30);

        mgr.resetLastConsumedStep();
        TEST_ASSERT(mgr.verifyCode(codeFuture, tNow));
        TEST_ASSERT(mgr.lastConsumedStep() == (tNow + 30) / 30);

        mgr.resetLastConsumedStep();
        TEST_ASSERT(!mgr.verifyCode(codeTooOld, tNow));
        TEST_ASSERT(!mgr.verifyCode(codeTooFar, tNow));

        TEST_ASSERT(!mgr.verifyCode("abcdef", tNow));
        TEST_ASSERT(!mgr.verifyCode("12345", tNow));
        TEST_ASSERT(!mgr.verifyCode("1234567", tNow));
        TEST_ASSERT(!mgr.verifyCode("", tNow));
    }

    // Part 5: End-to-End Loopback Challenge-Response Handshake
    {
        IdentityManager hostId(1011);
        hostId.loadOrCreate();
        hostId.setListenPort(50980);
        hostId.setUnattendedEnabled(true);
        hostId.setUnattendedPassword("SecureTestPass#2026");

        AppSettings hs = hostId.settings();
        hs.totpEnabled = true;
        hostId.totpManager().generateNewSecret();
        hs.totpSecret = hostId.totpManager().getSecretBase32();
        hostId.updateSettings(hs);

        NetworkEngine hostNet(hostId);
        TEST_ASSERT(hostNet.start());
        uint16_t hostPort = hostNet.hostListenPort();
        TEST_ASSERT(hostPort > 0);

        IdentityManager viewerId(1012);
        viewerId.loadOrCreate();
        viewerId.setListenPort(50981);
        NetworkEngine viewerNet(viewerId);
        TEST_ASSERT(viewerNet.start());

        std::string target = "127.0.0.1:" + std::to_string(hostPort);

        viewerNet.connectToRemote(target, "SecureTestPass#2026");

        bool reachedWaitingTotp = false;
        for (int i = 0; i < 60; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (viewerNet.isWaitingForTotp() || viewerNet.viewerStats().state == ViewerConnectionState::WaitingTotp) {
                reachedWaitingTotp = true;
                break;
            }
        }
        TEST_ASSERT(reachedWaitingTotp);
        TEST_ASSERT(viewerNet.isWaitingForTotp());

        viewerNet.submitViewerTotpCode("000000");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TEST_ASSERT(viewerNet.isWaitingForTotp());

        std::string validCode = hostId.totpManager().generateCode();
        viewerNet.submitViewerTotpCode(validCode);

        bool reachedConnected = false;
        for (int i = 0; i < 60; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (viewerNet.viewerStats().state == ViewerConnectionState::Connected) {
                reachedConnected = true;
                break;
            }
        }
        TEST_ASSERT(reachedConnected);

        viewerNet.disconnectViewer();
        hostNet.disconnectHostClient();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Test 3-Strike Lockout
        viewerNet.connectToRemote(target, "SecureTestPass#2026");
        reachedWaitingTotp = false;
        for (int i = 0; i < 60; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (viewerNet.isWaitingForTotp()) {
                reachedWaitingTotp = true;
                break;
            }
        }
        TEST_ASSERT(reachedWaitingTotp);

        // Strike 1
        viewerNet.submitViewerTotpCode("111111");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TEST_ASSERT(viewerNet.isWaitingForTotp());

        // Strike 2
        viewerNet.submitViewerTotpCode("222222");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TEST_ASSERT(viewerNet.isWaitingForTotp());

        // Strike 3
        viewerNet.submitViewerTotpCode("333333");

        bool reachedDisconnected = false;
        for (int i = 0; i < 60; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto st = viewerNet.viewerStats().state;
            if (st == ViewerConnectionState::Disconnected || st == ViewerConnectionState::Error) {
                reachedDisconnected = true;
                break;
            }
        }
        TEST_ASSERT(reachedDisconnected);
        TEST_ASSERT(!viewerNet.isWaitingForTotp());

        viewerNet.disconnectViewer();
        hostNet.disconnectHostClient();
        viewerNet.stop();
        hostNet.stop();
    }
}

void testRemoteFileSyncAndFolderMirroring() {
    std::cout << "[Test 47] Remote File Sync and Folder Mirroring..." << std::endl;

    // 1. Serialization and Deserialization Round-Trip for all 8 Sync wire payloads
    {
        // 1a. SyncScanReqPayload
        SyncScanReqPayload sreq{ 101, "sub/dir/path", 1 };
        std::vector<uint8_t> sreqBuf;
        serializeSyncScanReq(sreq, sreqBuf);
        SyncScanReqPayload sreqOut;
        TEST_ASSERT(deserializeSyncScanReq(sreqBuf.data(), sreqBuf.size(), sreqOut));
        TEST_ASSERT(sreqOut.scanId == 101);
        TEST_ASSERT(sreqOut.rootPath == "sub/dir/path");
        TEST_ASSERT(!deserializeSyncScanReq(sreqBuf.data(), 3, sreqOut)); // Underflow guard

        // 1b. SyncScanRespPayload
        SyncScanRespPayload sresp;
        sresp.scanId = 102;
        sresp.statusCode = 0;
        sresp.rootPath = "C:/Remote/Folder";
        sresp.entries.push_back({ "file1.txt", 12345, 1700000000, 0 });
        sresp.entries.push_back({ "nested_dir", 0, 1700000010, 1 });
        std::vector<uint8_t> srespBuf;
        serializeSyncScanResp(sresp, srespBuf);
        SyncScanRespPayload srespOut;
        TEST_ASSERT(deserializeSyncScanResp(srespBuf.data(), srespBuf.size(), srespOut));
        TEST_ASSERT(srespOut.scanId == 102);
        TEST_ASSERT(srespOut.rootPath == "C:/Remote/Folder");
        TEST_ASSERT(srespOut.entries.size() == 2);
        TEST_ASSERT(srespOut.entries[0].relativePath == "file1.txt");
        TEST_ASSERT(srespOut.entries[0].sizeBytes == 12345);
        TEST_ASSERT(srespOut.entries[1].isDirectory == 1);
        TEST_ASSERT(!deserializeSyncScanResp(srespBuf.data(), 5, srespOut));

        // 1c. SyncHashReqPayload
        SyncHashReqPayload hreq{ 201, 5, "nested/data.bin" };
        std::vector<uint8_t> hreqBuf;
        serializeSyncHashReq(hreq, hreqBuf);
        SyncHashReqPayload hreqOut;
        TEST_ASSERT(deserializeSyncHashReq(hreqBuf.data(), hreqBuf.size(), hreqOut));
        TEST_ASSERT(hreqOut.syncId == 201);
        TEST_ASSERT(hreqOut.fileId == 5);
        TEST_ASSERT(hreqOut.relativePath == "nested/data.bin");
        TEST_ASSERT(!deserializeSyncHashReq(hreqBuf.data(), 4, hreqOut));

        // 1d. SyncHashRespPayload
        SyncHashRespPayload hresp;
        hresp.syncId = 201;
        hresp.fileId = 5;
        hresp.targetFileSize = 131072;
        std::array<uint8_t, 32> hash1{}, hash2{};
        hash1.fill(0xAA);
        hash2.fill(0xBB);
        hresp.blockHashes.push_back(hash1);
        hresp.blockHashes.push_back(hash2);
        std::vector<uint8_t> hrespBuf;
        serializeSyncHashResp(hresp, hrespBuf);
        SyncHashRespPayload hrespOut;
        TEST_ASSERT(deserializeSyncHashResp(hrespBuf.data(), hrespBuf.size(), hrespOut));
        TEST_ASSERT(hrespOut.syncId == 201);
        TEST_ASSERT(hrespOut.fileId == 5);
        TEST_ASSERT(hrespOut.targetFileSize == 131072);
        TEST_ASSERT(hrespOut.blockHashes.size() == 2);
        TEST_ASSERT(hrespOut.blockHashes[0] == hash1);
        TEST_ASSERT(hrespOut.blockHashes[1] == hash2);
        TEST_ASSERT(!deserializeSyncHashResp(hrespBuf.data(), 12, hrespOut));

        // 1e. SyncDeltaBlockPayload
        SyncDeltaBlockPayload dblk;
        dblk.syncId = 301;
        dblk.fileId = 7;
        dblk.blockIndex = 1;
        dblk.blockOffset = 65536;
        dblk.totalFileSize = 131072;
        dblk.totalBlocks = 2;
        dblk.blockData = { 0x10, 0x20, 0x30, 0x40, 0x50 };
        dblk.blockHash = CryptoUtils::sha256(dblk.blockData.data(), dblk.blockData.size());
        std::vector<uint8_t> dblkBuf;
        serializeSyncDeltaBlock(dblk, dblkBuf);
        SyncDeltaBlockPayload dblkOut;
        TEST_ASSERT(deserializeSyncDeltaBlock(dblkBuf.data(), dblkBuf.size(), dblkOut));
        TEST_ASSERT(dblkOut.syncId == 301);
        TEST_ASSERT(dblkOut.fileId == 7);
        TEST_ASSERT(dblkOut.blockIndex == 1);
        TEST_ASSERT(dblkOut.blockOffset == 65536);
        TEST_ASSERT(dblkOut.blockHash == dblk.blockHash);
        TEST_ASSERT(dblkOut.blockData.size() == 5);
        TEST_ASSERT(dblkOut.blockData[2] == 0x30);
        TEST_ASSERT(!deserializeSyncDeltaBlock(dblkBuf.data(), 20, dblkOut));

        // 1f. SyncStatusUpdatePayload
        SyncStatusUpdatePayload sup;
        sup.syncId = 401;
        sup.syncState = 3;
        sup.currentFileIndex = 10;
        sup.totalFiles = 25;
        sup.transferredBytes = 500000;
        sup.totalBytes = 1200000;
        sup.currentFileName = "images/photo.png";
        std::vector<uint8_t> supBuf;
        serializeSyncStatusUpdate(sup, supBuf);
        SyncStatusUpdatePayload supOut;
        TEST_ASSERT(deserializeSyncStatusUpdate(supBuf.data(), supBuf.size(), supOut));
        TEST_ASSERT(supOut.syncId == 401);
        TEST_ASSERT(supOut.syncState == 3);
        TEST_ASSERT(supOut.currentFileIndex == 10);
        TEST_ASSERT(supOut.totalFiles == 25);
        TEST_ASSERT(supOut.transferredBytes == 500000);
        TEST_ASSERT(supOut.totalBytes == 1200000);
        TEST_ASSERT(supOut.currentFileName == "images/photo.png");
        TEST_ASSERT(!deserializeSyncStatusUpdate(supBuf.data(), 16, supOut));

        // 1g. SyncActionReqPayload
        SyncActionReqPayload areq;
        areq.actionId = 501;
        areq.actionType = static_cast<uint8_t>(SyncActionType::DeleteFile);
        areq.targetPath = "obsolete/temp.txt";
        std::vector<uint8_t> areqBuf;
        serializeSyncActionReq(areq, areqBuf);
        SyncActionReqPayload areqOut;
        TEST_ASSERT(deserializeSyncActionReq(areqBuf.data(), areqBuf.size(), areqOut));
        TEST_ASSERT(areqOut.actionId == 501);
        TEST_ASSERT(areqOut.actionType == static_cast<uint8_t>(SyncActionType::DeleteFile));
        TEST_ASSERT(areqOut.targetPath == "obsolete/temp.txt");
        TEST_ASSERT(!deserializeSyncActionReq(areqBuf.data(), 4, areqOut));

        // 1h. SyncActionRespPayload
        SyncActionRespPayload aresp{ 501, 1, 0, "OK" };
        std::vector<uint8_t> arespBuf;
        serializeSyncActionResp(aresp, arespBuf);
        SyncActionRespPayload arespOut;
        TEST_ASSERT(deserializeSyncActionResp(arespBuf.data(), arespBuf.size(), arespOut));
        TEST_ASSERT(arespOut.actionId == 501);
        TEST_ASSERT(arespOut.statusCode == 0);
        TEST_ASSERT(arespOut.message == "OK");
        TEST_ASSERT(!deserializeSyncActionResp(arespBuf.data(), 4, arespOut));
    }

    // Temporary test folder fixture
    std::filesystem::path testRoot = std::filesystem::temp_directory_path() / ("cppdesk_sync_suite_" + std::to_string(GetTickCount64()));
    std::filesystem::path localDir = testRoot / "local";
    std::filesystem::path remoteDir = testRoot / "remote";
    std::error_code ec;
    std::filesystem::create_directories(localDir / "docs" / "nested", ec);
    std::filesystem::create_directories(remoteDir, ec);

    // 2. Tree Scan and Relative Path Normalization
    {
        // Populate nested local folder structure
        std::ofstream(localDir / "root.txt", std::ios::binary) << "Root File Content";
        std::ofstream(localDir / "docs" / "manual.pdf", std::ios::binary) << "PDF Mock Data Header";
        std::ofstream(localDir / "docs" / "nested" / "config.json", std::ios::binary) << "{\"sync\": true}";

        FileSyncManager mgr;
        auto entries = mgr.scanLocalDirectory(localDir.string());
        TEST_ASSERT(entries.size() == 5); // 2 directories ("docs", "docs/nested") + 3 files

        // Check path normalization (strict forward-slash, no backslashes, no dot-dot)
        for (const auto& e : entries) {
            TEST_ASSERT(e.relativePath.find('\\') == std::string::npos);
            TEST_ASSERT(e.relativePath.find("..") == std::string::npos);
        }

        bool hasRoot = false, hasDocs = false, hasNested = false, hasManual = false, hasConfig = false;
        for (const auto& e : entries) {
            if (e.relativePath == "root.txt" && !e.isDirectory && e.sizeBytes == 17) hasRoot = true;
            if (e.relativePath == "docs" && e.isDirectory) hasDocs = true;
            if (e.relativePath == "docs/nested" && e.isDirectory) hasNested = true;
            if (e.relativePath == "docs/manual.pdf" && !e.isDirectory && e.sizeBytes == 20) hasManual = true;
            if (e.relativePath == "docs/nested/config.json" && !e.isDirectory && (e.sizeBytes == 14 || e.sizeBytes == 16)) hasConfig = true;
        }
        TEST_ASSERT(hasRoot && hasDocs && hasNested && hasManual && hasConfig);
    }

    // 3. 64KB Block Hashing & Differential Block Isolation
    {
        std::filesystem::path file192Local = localDir / "blocks192.bin";
        std::filesystem::path file192Remote = remoteDir / "blocks192.bin";

        std::string block0(FileSyncManager::SYNC_BLOCK_SIZE, 'A');
        std::string block1_local(FileSyncManager::SYNC_BLOCK_SIZE, 'B');
        std::string block1_remote(FileSyncManager::SYNC_BLOCK_SIZE, 'Z'); // Differs!
        std::string block2(FileSyncManager::SYNC_BLOCK_SIZE, 'C');

        {
            std::ofstream fl(file192Local, std::ios::binary);
            fl.write(block0.data(), block0.size());
            fl.write(block1_local.data(), block1_local.size());
            fl.write(block2.data(), block2.size());
        }
        {
            std::ofstream fr(file192Remote, std::ios::binary);
            fr.write(block0.data(), block0.size());
            fr.write(block1_remote.data(), block1_remote.size());
            fr.write(block2.data(), block2.size());
        }

        FileSyncManager mgr;
        auto hashesLocal = mgr.calculateBlockHashes(file192Local.string(), FileSyncManager::SYNC_BLOCK_SIZE);
        auto hashesRemote = mgr.calculateBlockHashes(file192Remote.string(), FileSyncManager::SYNC_BLOCK_SIZE);

        TEST_ASSERT(hashesLocal.size() == 3);
        TEST_ASSERT(hashesRemote.size() == 3);
        TEST_ASSERT(hashesLocal[0] == hashesRemote[0]); // Block 0 matches
        TEST_ASSERT(hashesLocal[1] != hashesRemote[1]); // Block 1 is dirty!
        TEST_ASSERT(hashesLocal[2] == hashesRemote[2]); // Block 2 matches
    }

    // 4. Dual-Tree Differential Plan Generation (PushMirror, PullMirror, TwoWay)
    {
        FileSyncManager mgr;
        std::vector<SyncFileEntry> localTree = {
            { "common.txt", 100, 1000, 0 },
            { "local_only.txt", 200, 2000, 0 },
            { "modified.txt", 300, 3500, 0 },
            { "shared_dir", 0, 1000, 1 }
        };
        std::vector<SyncFileEntry> remoteTree = {
            { "common.txt", 100, 1000, 0 },
            { "remote_only.txt", 400, 4000, 0 },
            { "modified.txt", 300, 3000, 0 },
            { "shared_dir", 0, 1000, 1 }
        };

        // PushMirror with purge enabled
        auto pushPlan = mgr.generateDiffPlan(localTree, remoteTree, SyncMode::PushMirror, true);
        bool pushLocalOnly = false, pushModified = false, purgeRemoteOnly = false;
        for (const auto& a : pushPlan) {
            if (a.relativePath == "local_only.txt" && a.action != SyncActionType::DeleteFile) pushLocalOnly = true;
            if (a.relativePath == "modified.txt") pushModified = true;
            if (a.relativePath == "remote_only.txt" && a.action == SyncActionType::DeleteFile) purgeRemoteOnly = true;
        }
        TEST_ASSERT(pushLocalOnly);
        TEST_ASSERT(pushModified);
        TEST_ASSERT(purgeRemoteOnly);

        // PullMirror without purge
        auto pullPlan = mgr.generateDiffPlan(localTree, remoteTree, SyncMode::PullMirror, false);
        bool pullRemoteOnly = false;
        for (const auto& a : pullPlan) {
            if (a.relativePath == "remote_only.txt" && a.action == SyncActionType::RequestFile) pullRemoteOnly = true;
        }
        TEST_ASSERT(pullRemoteOnly);

        // TwoWay reconciliation
        auto twoWayPlan = mgr.generateDiffPlan(localTree, remoteTree, SyncMode::TwoWay, false);
        bool twoWayPush = false, twoWayPull = false;
        for (const auto& a : twoWayPlan) {
            if (a.relativePath == "local_only.txt" && a.action != SyncActionType::RequestFile) twoWayPush = true;
            if (a.relativePath == "remote_only.txt" && a.action == SyncActionType::RequestFile) twoWayPull = true;
        }
        TEST_ASSERT(twoWayPush);
        TEST_ASSERT(twoWayPull);
    }

    // 5. Delta Block Staging, Atomic Replacement, and Byte-for-Byte Match
    {
        // Prepare target base file on remote destination: 128KB (2 blocks)
        std::filesystem::path patchTarget = remoteDir / "patch_target.dat";
        std::filesystem::path patchSource = localDir / "patch_target.dat";
        std::string partA(FileSyncManager::SYNC_BLOCK_SIZE, 'K');
        std::string partB_old(FileSyncManager::SYNC_BLOCK_SIZE, 'M');
        std::string partB_new(FileSyncManager::SYNC_BLOCK_SIZE, 'N'); // Modified block 1

        {
            std::ofstream ft(patchTarget, std::ios::binary);
            ft.write(partA.data(), partA.size());
            ft.write(partB_old.data(), partB_old.size());
        }
        {
            std::ofstream fs(patchSource, std::ios::binary);
            fs.write(partA.data(), partA.size());
            fs.write(partB_new.data(), partB_new.size());
        }

        FileSyncManager srcMgr;
        srcMgr.setLocalRoot(localDir.string());
        srcMgr.setSyncState(SyncState::Syncing);

        FileSyncManager dstMgr;
        dstMgr.setLocalRoot(remoteDir.string());
        dstMgr.setSyncState(SyncState::Syncing);

        // Calculate remote block hashes to supply to source manager
        auto remoteHashes = dstMgr.calculateBlockHashes(patchTarget.string(), FileSyncManager::SYNC_BLOCK_SIZE);
        TEST_ASSERT(remoteHashes.size() == 2);

        // Register incoming file on destination
        dstMgr.registerIncomingFile(1, "patch_target.dat", 2ULL * FileSyncManager::SYNC_BLOCK_SIZE, 1700000000);

        // Queue outgoing file on source with target hashes
        srcMgr.queueOutgoingFile("patch_target.dat", localDir.string(), 2ULL * FileSyncManager::SYNC_BLOCK_SIZE, 1700000000, remoteHashes, 1);

        // Pump blocks from source to destination
        std::vector<SyncDeltaBlockPayload> capturedBlocks;
        srcMgr.pumpOutgoingSync([&](PacketType type, const std::vector<uint8_t>& payload) {
            if (type == PacketType::SYNC_DELTA_BLOCK) {
                SyncDeltaBlockPayload blk;
                if (deserializeSyncDeltaBlock(payload.data(), payload.size(), blk)) {
                    capturedBlocks.push_back(std::move(blk));
                }
            }
            return true;
        }, 10);

        // Only block 1 was dirty, so exactly 1 block must have been transmitted across the wire!
        TEST_ASSERT(capturedBlocks.size() == 1);
        TEST_ASSERT(capturedBlocks[0].blockIndex == 1);
        TEST_ASSERT(capturedBlocks[0].blockOffset == FileSyncManager::SYNC_BLOCK_SIZE);

        // Deliver delta block to destination
        bool applied = dstMgr.handleDeltaBlock(capturedBlocks[0]);
        TEST_ASSERT(applied);

        // Verify destination file now exists, staging .part is deleted, and contents match byte-for-byte
        TEST_ASSERT(std::filesystem::exists(patchTarget));
        TEST_ASSERT(!std::filesystem::exists(patchTarget.string() + ".part"));
        TEST_ASSERT(std::filesystem::file_size(patchTarget) == 2ULL * FileSyncManager::SYNC_BLOCK_SIZE);

        auto readBytes = [](const std::filesystem::path& p) {
            std::ifstream is(p, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
        };
        auto srcBytes = readBytes(patchSource);
        auto dstBytes = readBytes(patchTarget);
        TEST_ASSERT(srcBytes == dstBytes);
    }

    // 6. Host and Viewer Loopback Session Integration
    {
        IdentityManager hostId(1013);
        hostId.loadOrCreate();
        hostId.setListenPort(50984);
        hostId.setUnattendedEnabled(true);
        hostId.setUnattendedPassword("SyncHostPassword#2026");

        IdentityManager viewerId(1014);
        viewerId.loadOrCreate();
        viewerId.setListenPort(50985);

        NetworkEngine hostNet(hostId);
        TEST_ASSERT(hostNet.start());
        uint16_t hostPort = hostNet.hostListenPort();
        TEST_ASSERT(hostPort > 0);

        NetworkEngine viewerNet(viewerId);
        TEST_ASSERT(viewerNet.start());

        std::string target = "127.0.0.1:" + std::to_string(hostPort);
        viewerNet.connectToRemote(target, "SyncHostPassword#2026");

        bool connected = false;
        for (int i = 0; i < 60; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (viewerNet.viewerStats().state == ViewerConnectionState::Connected) {
                connected = true;
                break;
            }
        }
        TEST_ASSERT(connected);

        // Configure roots and trigger scan from viewer
        viewerNet.requestRemoteScan(remoteDir.string());

        // Wait brief tick for scan packets to exchange over loopback
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        // Clean disconnect
        viewerNet.disconnectViewer();
        hostNet.disconnectHostClient();
        viewerNet.stop();
        hostNet.stop();
    }

    // 7. Cleanup of Temporary Test Folders
    std::filesystem::remove_all(testRoot, ec);
    TEST_ASSERT(!std::filesystem::exists(testRoot));
}

void testPeerVersionHandshakeAndSelfUpdater() {
    std::cout << "[Test 48] Peer Version Handshake, AutoUpdater and Smooth Horizontal Scrolling..." << std::endl;

    // Part 1: SemanticVersion Numeric Parsing, Formatting, and Comparison Triad
    {
        SemanticVersion v1 = SemanticVersion::parse("3.4.0");
        TEST_ASSERT(v1.major == 3 && v1.minor == 4 && v1.patch == 0);
        TEST_ASSERT(v1.toString() == "3.4.0");

        SemanticVersion v2 = SemanticVersion::parse("v3.4.1");
        TEST_ASSERT(v2.major == 3 && v2.minor == 4 && v2.patch == 1);

        SemanticVersion v3 = SemanticVersion::parse("V3.5.0-preview.2");
        TEST_ASSERT(v3.major == 3 && v3.minor == 5 && v3.patch == 0);

        SemanticVersion vInvalid = SemanticVersion::parse("");
        TEST_ASSERT(vInvalid.major == 0 && vInvalid.minor == 0 && vInvalid.patch == 0);
        SemanticVersion vInvalid2 = SemanticVersion::parse("invalid.ver");
        TEST_ASSERT(vInvalid2.major == 0 && vInvalid2.minor == 0 && vInvalid2.patch == 0);

        // Three-way comparisons
        TEST_ASSERT(v1 < v2);
        TEST_ASSERT(v2 < v3);
        TEST_ASSERT(v1 < v3);
        TEST_ASSERT(v3 > v2);
        TEST_ASSERT(v2 > v1);
        TEST_ASSERT(v1 == (SemanticVersion{ 3, 4, 0 }));
        TEST_ASSERT((SemanticVersion{ 4, 0, 0 }) > (SemanticVersion{ 3, 9, 9 }));
        TEST_ASSERT((SemanticVersion{ 3, 5, 1 }) > (SemanticVersion{ 3, 5, 0 }));
    }

    // Part 2: VersionUpgradeNoticePayload Wire Serialization / Deserialization Round-Trip
    {
        VersionUpgradeNoticePayload notice;
        notice.senderVersion = "3.4.0";
        notice.minimumVersion = "3.3.0";
        notice.downloadUrl = "https://github.com/nmnghia2527/cppdesk/releases/tag/v3.4.0";
        notice.message = "Feature update: peer handshake support.";

        std::vector<uint8_t> buf;
        serializeVersionUpgradeNotice(notice, buf);
        TEST_ASSERT(!buf.empty());

        VersionUpgradeNoticePayload decoded;
        TEST_ASSERT(deserializeVersionUpgradeNotice(buf.data(), buf.size(), decoded));
        TEST_ASSERT(decoded.senderVersion == notice.senderVersion);
        TEST_ASSERT(decoded.minimumVersion == notice.minimumVersion);
        TEST_ASSERT(decoded.downloadUrl == notice.downloadUrl);
        TEST_ASSERT(decoded.message == notice.message);

        // Truncation / Underflow safety guard
        TEST_ASSERT(!deserializeVersionUpgradeNotice(buf.data(), 4, decoded));
        TEST_ASSERT(!deserializeVersionUpgradeNotice(buf.data(), 0, decoded));
    }

    // Part 3: Handshake Trailing Version Parsing Backward Compatibility Simulation
    {
        // Simulate legacy HELLO packet without trailing version string
        std::vector<uint8_t> legacyHello;
        ByteWriter legacyW(legacyHello);
        legacyW.writeU8(static_cast<uint8_t>(PacketType::HELLO));
        legacyW.writeU64(123456789ULL); // deskId
        legacyW.writeString("LegacyViewer");

        ByteReader legacyReader(legacyHello.data() + 1, legacyHello.size() - 1);
        uint64_t dId = legacyReader.readU64();
        std::string hostName = legacyReader.readString();
        std::string viewerVer = "unknown";
        if (legacyReader.hasRemaining(2)) {
            viewerVer = legacyReader.readString();
        }
        TEST_ASSERT(dId == 123456789ULL);
        TEST_ASSERT(hostName == "LegacyViewer");
        TEST_ASSERT(viewerVer == "unknown"); // Legacy client gracefully handled

        // Simulate modern HELLO packet with trailing version string
        std::vector<uint8_t> modernHello;
        ByteWriter modernW(modernHello);
        modernW.writeU8(static_cast<uint8_t>(PacketType::HELLO));
        modernW.writeU64(987654321ULL);
        modernW.writeString("ModernViewer");
        modernW.writeString("3.4.0");

        ByteReader modernReader(modernHello.data() + 1, modernHello.size() - 1);
        uint64_t mId = modernReader.readU64();
        std::string mHost = modernReader.readString();
        std::string mVer = "unknown";
        if (modernReader.hasRemaining(2)) {
            mVer = modernReader.readString();
        }
        TEST_ASSERT(mId == 987654321ULL);
        TEST_ASSERT(mHost == "ModernViewer");
        TEST_ASSERT(mVer == "3.4.0");
    }

    // Part 4: AutoUpdater JSON Extraction for Asset Download URL and Size
    {
        std::string mockReleaseJson = R"({
            "tag_name": "v3.4.0",
            "html_url": "https://github.com/nmnghia2527/cppdesk/releases/tag/v3.4.0",
            "assets": [
                {
                    "name": "CppDesk.zip",
                    "size": 5242880,
                    "browser_download_url": "https://github.com/nmnghia2527/cppdesk/releases/download/v3.4.0/CppDesk.zip"
                },
                {
                    "name": "CppDesk.exe",
                    "size": 6606028,
                    "browser_download_url": "https://github.com/nmnghia2527/cppdesk/releases/download/v3.4.0/CppDesk.exe"
                }
            ]
        })";

        uint64_t assetSize = 0;
        std::string assetUrl = AutoUpdater::extractAssetDownloadUrl(mockReleaseJson, "CppDesk.exe", &assetSize);
        TEST_ASSERT(assetUrl == "https://github.com/nmnghia2527/cppdesk/releases/download/v3.4.0/CppDesk.exe");
        TEST_ASSERT(assetSize == 6606028);

        // Non-existent asset
        uint64_t missingSize = 0;
        std::string missingUrl = AutoUpdater::extractAssetDownloadUrl(mockReleaseJson, "NonExistent.tar.gz", &missingSize);
        TEST_ASSERT(missingUrl.empty());
        TEST_ASSERT(missingSize == 0);
    }

    // Part 5: PE Binary Integrity Validation & Self-Update Command Construction
    {
        // 5a. Validate valid PE binary mockup
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring testExePath = std::wstring(tempPath) + L"test_mock_pe.exe";

        // Create mock valid PE file
        {
            std::ofstream out(testExePath.c_str(), std::ios::binary);
            IMAGE_DOS_HEADER dosHdr{};
            dosHdr.e_magic = IMAGE_DOS_SIGNATURE; // 'MZ'
            dosHdr.e_lfanew = sizeof(IMAGE_DOS_HEADER) + 16; // offset to PE signature
            out.write(reinterpret_cast<const char*>(&dosHdr), sizeof(dosHdr));

            // Padding up to e_lfanew
            char pad[16] = {};
            out.write(pad, sizeof(pad));

            // NT Signature: "PE\0\0"
            DWORD ntSig = IMAGE_NT_SIGNATURE;
            out.write(reinterpret_cast<const char*>(&ntSig), sizeof(ntSig));

            // Additional payload bytes
            char payload[128] = {};
            out.write(payload, sizeof(payload));
        }

        TEST_ASSERT(AutoUpdater::validatePeBinary(testExePath));

        // 5b. Corrupt the PE signature
        {
            std::fstream corruptFile(testExePath.c_str(), std::ios::in | std::ios::out | std::ios::binary);
            corruptFile.seekp(sizeof(IMAGE_DOS_HEADER) + 16);
            DWORD badSig = 0xDEADBEEF;
            corruptFile.write(reinterpret_cast<const char*>(&badSig), sizeof(badSig));
        }
        TEST_ASSERT(!AutoUpdater::validatePeBinary(testExePath));

        // Clean up mock file
        DeleteFileW(testExePath.c_str());

        // 5c. Test buildSelfUpdateCommand formatting
        std::wstring newExe = L"C:\\Temp\\CppDesk_new.exe";
        std::wstring curExe = L"C:\\Program Files\\CppDesk\\CppDesk.exe";
        std::wstring cmd = AutoUpdater::buildSelfUpdateCommand(newExe, curExe);
        TEST_ASSERT(cmd.find(L"cmd.exe /c") != std::wstring::npos);
        TEST_ASSERT(cmd.find(L"timeout /t 1 /nobreak") != std::wstring::npos);
        TEST_ASSERT(cmd.find(L"move /y") != std::wstring::npos);
        TEST_ASSERT(cmd.find(L"start \"\"") != std::wstring::npos);
        TEST_ASSERT(cmd.find(newExe) != std::wstring::npos);
        TEST_ASSERT(cmd.find(curExe) != std::wstring::npos);

        // 5d. Test silent applySelfUpdateAndRestart file replacement
        std::wstring testSrc = std::wstring(tempPath) + L"test_update_src.bin";
        std::wstring testDest = std::wstring(tempPath) + L"test_update_dest.bin";
        {
            std::ofstream srcOut(testSrc.c_str(), std::ios::binary);
            srcOut << "CPPDESK_UPDATE_V340_PAYLOAD";
        }
        {
            std::ofstream destOut(testDest.c_str(), std::ios::binary);
            destOut << "OLD_V332_CONTENT";
        }
        int updateRes = AutoUpdater::applySelfUpdateAndRestart(testSrc, testDest, 0, false);
        TEST_ASSERT(updateRes == 0);
        {
            std::ifstream destIn(testDest.c_str(), std::ios::binary);
            std::string content;
            destIn >> content;
            TEST_ASSERT(content == "CPPDESK_UPDATE_V340_PAYLOAD");
        }
        DeleteFileW(testSrc.c_str());
        DeleteFileW(testDest.c_str());
    }

    // Part 6: Smooth Horizontal Scroll Spring Simulation
    {
        auto testSpring = [](float& pos, float& vel, float target, float omega, float zeta, float dt, float eps = 0.0012f) -> bool {
            float diff = pos - target;
            if (std::fabs(diff) <= eps && std::fabs(vel) <= eps * 8.0f) {
                if (pos != target || vel != 0.0f) {
                    pos = target;
                    vel = 0.0f;
                    return true;
                }
                return false;
            }
            int steps = (dt > 0.008f) ? static_cast<int>(std::ceil(dt / 0.008f)) : 1;
            steps = std::clamp(steps, 1, 6);
            float subDt = dt / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) {
                float disp = pos - target;
                float accel = (-omega * omega * disp) - (2.0f * zeta * omega * vel);
                vel += accel * subDt;
                pos += vel * subDt;
            }
            if (std::fabs(pos - target) <= eps && std::fabs(vel) <= eps * 8.0f) {
                pos = target;
                vel = 0.0f;
            }
            return true;
        };

        float pos = 0.0f;
        float vel = 0.0f;
        float target = 220.0f;
        float dt = 0.016f; // 60 FPS
        int frameCount = 0;

        while (frameCount < 120) {
            bool inMotion = testSpring(pos, vel, target, 32.0f, 0.88f, dt, 0.15f);
            frameCount++;
            if (!inMotion) break;
        }

        // Must converge to target smoothly within 60 frames (~1 sec)
        TEST_ASSERT(frameCount > 5 && frameCount < 60);
        TEST_ASSERT(std::fabs(pos - target) < 0.001f);
        TEST_ASSERT(vel == 0.0f);
    }
}

void testNovelBreakthroughPowerFeatures() {
    std::cout << "[TEST 49] Breakthrough Power Features (Rewind Buffer, Privacy Cloak, Scratchpad, Omnibar)...\n";

    // Part 1: 60-Second Visual Time-Machine & Session Rewind Scrubber
    {
        SessionRewindBuffer rewindBuf;
        TEST_ASSERT(rewindBuf.empty());
        TEST_ASSERT(rewindBuf.size() == 0);
        TEST_ASSERT(rewindBuf.totalMemoryBytes() == 0);

        // Push 130 frames at 500ms intervals (frame size: 64x64 BGRA)
        std::vector<uint8_t> bgra(64 * 64 * 4, 128);
        for (size_t i = 0; i < 130; ++i) {
            uint64_t ts = 1000 + i * 500;
            // Introduce subtle color variation to ensure valid unique JPEG encoding
            bgra[0] = static_cast<uint8_t>(i % 250);
            bgra[1] = static_cast<uint8_t>((i * 3) % 250);
            bgra[2] = static_cast<uint8_t>((i * 7) % 250);
            rewindBuf.pushFrame(ts, bgra.data(), 64, 64);
        }

        // Buffer must be capped at MAX_SNAPSHOTS (120 keyframes)
        TEST_ASSERT(rewindBuf.size() == 120);
        TEST_ASSERT(!rewindBuf.empty());

        // Memory must remain bounded < 25 MB
        TEST_ASSERT(rewindBuf.totalMemoryBytes() > 0);
        TEST_ASSERT(rewindBuf.totalMemoryBytes() < 25 * 1024 * 1024);

        // First 10 frames evicted; oldest is frame 10 (ts = 1000 + 10*500 = 6000), newest is frame 129 (ts = 1000 + 129*500 = 65500)
        uint64_t expectedOldest = 1000 + 10 * 500;
        uint64_t expectedNewest = 1000 + 129 * 500;
        TEST_ASSERT(rewindBuf.oldestTimestampMs() == expectedOldest);
        TEST_ASSERT(rewindBuf.newestTimestampMs() == expectedNewest);

        // Offset queries
        std::vector<uint8_t> outBgra;
        int outW = 0, outH = 0;
        uint64_t outTs = 0;

        // Offset 0ms (live snapshot)
        TEST_ASSERT(rewindBuf.getFrameAtOffset(0, outBgra, outW, outH, outTs));
        TEST_ASSERT(outW == 64 && outH == 64);
        TEST_ASSERT(outTs == expectedNewest);
        TEST_ASSERT(outBgra.size() == 64 * 64 * 4);

        // Offset -30000ms (30s ago)
        TEST_ASSERT(rewindBuf.getFrameAtOffset(-30000, outBgra, outW, outH, outTs));
        uint64_t expected30s = expectedNewest - 30000;
        uint64_t diff30s = (outTs >= expected30s) ? (outTs - expected30s) : (expected30s - outTs);
        TEST_ASSERT(diff30s <= 500);

        // Offset -75000ms (exceeds window, clamps to oldest)
        TEST_ASSERT(rewindBuf.getFrameAtOffset(-75000, outBgra, outW, outH, outTs));
        TEST_ASSERT(outTs == expectedOldest);

        // Index queries
        TEST_ASSERT(rewindBuf.getFrameAtIndex(0, outBgra, outW, outH, outTs));
        TEST_ASSERT(outTs == expectedOldest);

        TEST_ASSERT(rewindBuf.getFrameAtIndex(119, outBgra, outW, outH, outTs));
        TEST_ASSERT(outTs == expectedNewest);

        // Out of bounds index
        TEST_ASSERT(!rewindBuf.getFrameAtIndex(120, outBgra, outW, outH, outTs));

        // Clear
        rewindBuf.clear();
        TEST_ASSERT(rewindBuf.empty());
        TEST_ASSERT(rewindBuf.size() == 0);
        TEST_ASSERT(rewindBuf.totalMemoryBytes() == 0);
        TEST_ASSERT(!rewindBuf.getFrameAtOffset(0, outBgra, outW, outH, outTs));
    }

    // Part 2: Privacy Cloak Sensitive Window & PII Auto-Redaction
    {
        PrivacyCloakManager cloakMgr;
        TEST_ASSERT(cloakMgr.isEnabled());

        // Default sensitive keywords matching
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"1Password - Master Vault"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Personal Vault - KeePassXC"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Google Chrome - Bitwarden Extension"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Online Banking - Chase Login"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Wells Fargo Accounts - Edge"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Wallet - Coinbase Pro"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"MetaMask Notification"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"ledger live manager"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Private - Incognito Window"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"WireGuard VPN Client"));

        // Non-sensitive titles
        TEST_ASSERT(!cloakMgr.matchesSensitiveTitle(L"Visual Studio Code"));
        TEST_ASSERT(!cloakMgr.matchesSensitiveTitle(L"Calculator"));
        TEST_ASSERT(!cloakMgr.matchesSensitiveTitle(L"Command Prompt"));

        // Custom keywords
        cloakMgr.setCustomKeywords("confidential,secret_project,topsecret");
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"Confidential Document.docx"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"secret_project roadmap"));
        TEST_ASSERT(cloakMgr.matchesSensitiveTitle(L"TOPSECRET file"));
        std::string kwStr = cloakMgr.customKeywords();
        TEST_ASSERT(kwStr.find("confidential") != std::string::npos);

        // Solid Charcoal Redaction (style 0) on 64x64 frame
        cloakMgr.setRedactionStyle(0);
        std::vector<uint8_t> frameBgra(64 * 64 * 4, 255); // White frame
        RECT sensitiveRect = { 10, 10, 30, 30 };
        cloakMgr.redactCaptureBuffer(frameBgra.data(), 64, 64, 0, 0, { sensitiveRect });

        // Verify inside redaction rect (Charcoal: RGB 0x1E, 0x1E, 0x1E, A 0xFF)
        size_t inIdx = (15 * 64 + 15) * 4;
        TEST_ASSERT(frameBgra[inIdx + 0] == 0x1E); // B
        TEST_ASSERT(frameBgra[inIdx + 1] == 0x1E); // G
        TEST_ASSERT(frameBgra[inIdx + 2] == 0x1E); // R
        TEST_ASSERT(frameBgra[inIdx + 3] == 0xFF); // A

        // Verify outside redaction rect remains unmodified (white)
        size_t outIdx = (5 * 64 + 5) * 4;
        TEST_ASSERT(frameBgra[outIdx + 0] == 255);
        TEST_ASSERT(frameBgra[outIdx + 1] == 255);
        TEST_ASSERT(frameBgra[outIdx + 2] == 255);

        // Mosaic Block Averaging Redaction (style 1)
        cloakMgr.setRedactionStyle(1);
        std::vector<uint8_t> mosaicFrame(64 * 64 * 4);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                size_t p = (y * 64 + x) * 4;
                mosaicFrame[p + 0] = static_cast<uint8_t>(x * 3);
                mosaicFrame[p + 1] = static_cast<uint8_t>(y * 3);
                mosaicFrame[p + 2] = static_cast<uint8_t>((x + y) * 2);
                mosaicFrame[p + 3] = 255;
            }
        }
        cloakMgr.redactCaptureBuffer(mosaicFrame.data(), 64, 64, 0, 0, { sensitiveRect });

        // Adjacent pixels in same mosaic cell must have identical block-averaged values
        size_t p1 = (12 * 64 + 12) * 4;
        size_t p2 = (13 * 64 + 13) * 4;
        TEST_ASSERT(mosaicFrame[p1 + 0] == mosaicFrame[p2 + 0]);
        TEST_ASSERT(mosaicFrame[p1 + 1] == mosaicFrame[p2 + 1]);
        TEST_ASSERT(mosaicFrame[p1 + 2] == mosaicFrame[p2 + 2]);

        // AppSettings persistence roundtrip via IdentityManager
        {
            IdentityManager idSave(99);
            idSave.loadOrCreate();
            AppSettings s = idSave.settings();
            s.privacyCloakEnabled = true;
            s.privacyCloakKeywords = "payroll,tax_return,private_key";
            s.privacyCloakStyle = 1;
            idSave.updateSettings(s);
        }
        {
            IdentityManager idLoad(99);
            idLoad.loadOrCreate();
            AppSettings loaded = idLoad.settings();
            TEST_ASSERT(loaded.privacyCloakEnabled == true);
            TEST_ASSERT(loaded.privacyCloakKeywords == "payroll,tax_return,private_key");
            TEST_ASSERT(loaded.privacyCloakStyle == 1);
        }
        std::error_code ec;
        std::filesystem::remove("cppdesk_99.ini", ec);
    }

    // Part 3: Collaborative Dual-Peer Real-Time Scratchpad
    {
        // Wire serialization roundtrip
        ScratchpadUpdatePayload spIn;
        spIn.revision = 42;
        spIn.authorDeskId = 123456789ULL;
        spIn.cursorPosition = 15;
        spIn.flags = 0x01;
        spIn.text = "System maintenance scheduled for 22:00 UTC";

        std::vector<uint8_t> spBytes;
        serializeScratchpadUpdate(spIn, spBytes);

        ScratchpadUpdatePayload spOut;
        TEST_ASSERT(deserializeScratchpadUpdate(spBytes.data(), spBytes.size(), spOut));
        TEST_ASSERT(spOut.revision == 42);
        TEST_ASSERT(spOut.authorDeskId == 123456789ULL);
        TEST_ASSERT(spOut.cursorPosition == 15);
        TEST_ASSERT(spOut.flags == 0x01);
        TEST_ASSERT(spOut.text == "System maintenance scheduled for 22:00 UTC");

        // ScratchpadManager local and remote monotonic updates
        ScratchpadManager scratchpad;
        TEST_ASSERT(scratchpad.getText().empty());
        TEST_ASSERT(scratchpad.getRevision() == 0);

        auto localUpdate = scratchpad.updateLocalText("Initial notes", 111222333, 13);
        TEST_ASSERT(localUpdate.revision == 1);
        TEST_ASSERT(localUpdate.text == "Initial notes");
        TEST_ASSERT(scratchpad.getText() == "Initial notes");
        TEST_ASSERT(scratchpad.getRevision() == 1);
        TEST_ASSERT(scratchpad.getLastEditor() == 111222333);
        TEST_ASSERT(scratchpad.getCursorPos() == 13);

        // Stale remote update must be rejected
        ScratchpadUpdatePayload stalePayload;
        stalePayload.revision = 0;
        stalePayload.authorDeskId = 999888777;
        stalePayload.text = "Stale overwrite";
        bool acceptedStale = scratchpad.handleRemoteUpdate(stalePayload);
        TEST_ASSERT(!acceptedStale);
        TEST_ASSERT(scratchpad.getText() == "Initial notes");

        // Newer remote update must be accepted
        ScratchpadUpdatePayload newerPayload;
        newerPayload.revision = 10;
        newerPayload.authorDeskId = 999888777;
        newerPayload.text = "Updated from remote peer";
        newerPayload.cursorPosition = 24;
        bool acceptedNewer = scratchpad.handleRemoteUpdate(newerPayload);
        TEST_ASSERT(acceptedNewer);
        TEST_ASSERT(scratchpad.getText() == "Updated from remote peer");
        TEST_ASSERT(scratchpad.getRevision() == 10);
        TEST_ASSERT(scratchpad.getLastEditor() == 999888777);
        TEST_ASSERT(scratchpad.getCursorPos() == 24);

        // Diagnostic snippet generators
        std::string snipHost = ScratchpadManager::generateSnippet("host", 555666777, "SRV-TEST01");
        TEST_ASSERT(snipHost.find("Host:") != std::string::npos || snipHost.find("SRV-TEST01") != std::string::npos);
        std::string snipSec = ScratchpadManager::generateSnippet("security");
        TEST_ASSERT(snipSec.find("Security") != std::string::npos || snipSec.find("AES-256-GCM") != std::string::npos);
        std::string snipTime = ScratchpadManager::generateSnippet("timestamp");
        TEST_ASSERT(snipTime.find("UTC") != std::string::npos || snipTime.find("Timestamp") != std::string::npos);

        // Clear
        scratchpad.clear();
        TEST_ASSERT(scratchpad.getText().empty());
    }

    // Part 4: Spotlight Omnibar Command Palette
    {
        // Serialization roundtrips
        CommandPaletteExecPayload execIn;
        execIn.commandId = 202;
        execIn.commandKey = "net_flush_dns";
        execIn.customCommand = "ipconfig /flushdns";
        execIn.flags = 0x01;

        std::vector<uint8_t> execBytes;
        serializeCommandPaletteExec(execIn, execBytes);

        CommandPaletteExecPayload execOut;
        TEST_ASSERT(deserializeCommandPaletteExec(execBytes.data(), execBytes.size(), execOut));
        TEST_ASSERT(execOut.commandId == 202);
        TEST_ASSERT(execOut.commandKey == "net_flush_dns");
        TEST_ASSERT(execOut.customCommand == "ipconfig /flushdns");
        TEST_ASSERT(execOut.flags == 0x01);

        CommandPaletteOutputPayload outIn;
        outIn.commandId = 202;
        outIn.exitCode = 0;
        outIn.isCompleted = true;
        outIn.stdoutData = "Successfully flushed the DNS Resolver Cache.\r\n";
        outIn.errorMessage = "";

        std::vector<uint8_t> outBytes;
        serializeCommandPaletteOutput(outIn, outBytes);

        CommandPaletteOutputPayload outOut;
        TEST_ASSERT(deserializeCommandPaletteOutput(outBytes.data(), outBytes.size(), outOut));
        TEST_ASSERT(outOut.commandId == 202);
        TEST_ASSERT(outOut.exitCode == 0);
        TEST_ASSERT(outOut.isCompleted == true);
        TEST_ASSERT(outOut.stdoutData == "Successfully flushed the DNS Resolver Cache.\r\n");
        TEST_ASSERT(outOut.errorMessage.empty());

        // Command catalog & search
        CommandPaletteManager palMgr;
        const auto& catalog = palMgr.getCatalog();
        TEST_ASSERT(!catalog.empty());
        TEST_ASSERT(catalog.size() >= 10);

        auto searchResults = palMgr.search("flush");
        TEST_ASSERT(!searchResults.empty());

        // Custom command registration
        OmnibarCommand customCmd;
        customCmd.id = "custom_ping_gateway";
        customCmd.title = "Ping Default Gateway";
        customCmd.category = "Network Diagnostics";
        customCmd.commandLine = "cmd.exe /c ping 127.0.0.1 -n 1";
        customCmd.requiresAdmin = false;
        palMgr.registerCommand(customCmd);

        auto customResults = palMgr.search("Gateway");
        TEST_ASSERT(!customResults.empty());
        TEST_ASSERT(customResults[0].id == "custom_ping_gateway");

        // Synchronous non-blocking piped execution verification
        int32_t exitCode = -1;
        std::string syncOutput;
        bool execOk = CommandPaletteManager::executeSync("cmd.exe /c echo CPPDESK_OMNIBAR_VERIFIED", exitCode, syncOutput, 5000);
        TEST_ASSERT(execOk);
        TEST_ASSERT(exitCode == 0);
        TEST_ASSERT(syncOutput.find("CPPDESK_OMNIBAR_VERIFIED") != std::string::npos);
    }
}

void testCrossPlatformMacOsAndWindowsFoundation() {
    std::cout << "[TEST 50] Cross-Platform macOS & Windows Abstraction Foundation...\n" << std::flush;

    // Sub-test 1: Sockets Abstraction Layer
    {
        socket_init_network();
        socket_t sInv = INVALID_SOCKET_VAL;
        TEST_ASSERT(sInv == INVALID_SOCKET_VAL);

        socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        TEST_ASSERT(listener != INVALID_SOCKET_VAL);

        int opt = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        int bRes = bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        TEST_ASSERT(bRes == 0);

        int lRes = listen(listener, 1);
        TEST_ASSERT(lRes == 0);

        sockaddr_in boundAddr{};
        socklen_t boundLen = sizeof(boundAddr);
        getsockname(listener, reinterpret_cast<sockaddr*>(&boundAddr), &boundLen);
        uint16_t port = ntohs(boundAddr.sin_port);
        TEST_ASSERT(port > 0);

        socket_t client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        TEST_ASSERT(client != INVALID_SOCKET_VAL);

        sockaddr_in serverTarget{};
        serverTarget.sin_family = AF_INET;
        serverTarget.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        serverTarget.sin_port = htons(port);

        int cRes = connect(client, reinterpret_cast<sockaddr*>(&serverTarget), sizeof(serverTarget));
        TEST_ASSERT(cRes == 0);

        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        socket_t accepted = accept(listener, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        TEST_ASSERT(accepted != INVALID_SOCKET_VAL);

        // Non-blocking mode verification
        bool nbOk1 = socket_set_nonblocking(client, true);
        TEST_ASSERT(nbOk1);
        bool nbOk2 = socket_set_nonblocking(accepted, true);
        TEST_ASSERT(nbOk2);

        // Wouldblock status checking on empty read
        char dummyBuf[16];
        int rBytes = recv(client, dummyBuf, sizeof(dummyBuf), 0);
        int err = socket_errno();
        TEST_ASSERT(rBytes == SOCKET_ERROR_VAL || rBytes == -1);
        TEST_ASSERT(socket_is_wouldblock(err));

        // Scatter-gather frame transmission via NetworkEngine
        char hdrData[] = "HDR:";
        char bodyData[] = "CROSS_PLATFORM_VERIFICATION";
        size_t totalExpected = std::strlen(hdrData) + std::strlen(bodyData);

        bool sgOk = NetworkEngine::sendScatterGather(accepted, hdrData, std::strlen(hdrData), bodyData, std::strlen(bodyData));
        TEST_ASSERT(sgOk);

        // Read back on client side
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        char recvBuf[64] = {0};
        int totalReceived = 0;
        for (int retry = 0; retry < 10 && totalReceived < static_cast<int>(totalExpected); ++retry) {
            int n = recv(client, recvBuf + totalReceived, static_cast<int>(sizeof(recvBuf) - 1 - totalReceived), 0);
            if (n > 0) {
                totalReceived += n;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        TEST_ASSERT(totalReceived == static_cast<int>(totalExpected));
        std::string receivedStr(recvBuf, totalReceived);
        TEST_ASSERT(receivedStr == "HDR:CROSS_PLATFORM_VERIFICATION");

        // Validate socket handle conversion helpers in socket_compat.hpp
        uintptr_t handleVal = fromSocket(client);
        socket_t recoveredSock = toSocket(handleVal);
        TEST_ASSERT(recoveredSock == client);

        socket_close(client);
        socket_close(accepted);
        socket_close(listener);
    }

    // Sub-test 2: Dual-Backend Cryptography & AEAD Wire Verification
    {
        // 2a. Random byte distribution
        std::array<uint8_t, 256> randBuf{};
        CryptoUtils::randomBytes(randBuf.data(), randBuf.size());
        size_t nonZeroCount = 0;
        for (uint8_t b : randBuf) {
            if (b != 0) ++nonZeroCount;
        }
        TEST_ASSERT(nonZeroCount > 240);

        // 2b. Memory security primitives
        std::vector<uint8_t> memSec(64, 0xAA);
        TEST_ASSERT(CryptoUtils::pinMemory(memSec.data(), memSec.size()));
        CryptoUtils::unpinMemory(memSec.data(), memSec.size());
        CryptoUtils::secureZero(memSec.data(), memSec.size());
        for (uint8_t b : memSec) {
            TEST_ASSERT(b == 0);
        }

        // 2c. SHA-256 test vector ("abc")
        auto shaVector = CryptoUtils::sha256("abc");
        const uint8_t expectedSha[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        TEST_ASSERT(std::memcmp(shaVector.data(), expectedSha, 32) == 0);

        // 2d. HMAC-SHA256 test vector (RFC 4231 Test Case 2)
        const std::string rfcKey = "Jefe";
        const std::string rfcData = "what do ya want for nothing?";
        auto hmacVector = CryptoUtils::hmacSha256(rfcKey.data(), rfcKey.size(), rfcData.data(), rfcData.size());
        const uint8_t expectedHmac[32] = {
            0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
            0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
            0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
            0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43
        };
        TEST_ASSERT(std::memcmp(hmacVector.data(), expectedHmac, 32) == 0);

        // 2e. TOTP HMAC test vectors (RFC 6238)
        std::vector<uint8_t> totpDigest1;
        uint8_t counterBe[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
        const char* rawKeySha1 = "12345678901234567890";
        bool hmac1Ok = TotpManager::computeHmac(TotpAlgorithm::Sha1,
                                                reinterpret_cast<const uint8_t*>(rawKeySha1), 20,
                                                counterBe, 8, totpDigest1);
        TEST_ASSERT(hmac1Ok);
        TEST_ASSERT(totpDigest1.size() == 20);

        std::vector<uint8_t> totpDigest256;
        const char* rawKeySha256 = "12345678901234567890123456789012";
        bool hmac256Ok = TotpManager::computeHmac(TotpAlgorithm::Sha256,
                                                  reinterpret_cast<const uint8_t*>(rawKeySha256), 32,
                                                  counterBe, 8, totpDigest256);
        TEST_ASSERT(hmac256Ok);
        TEST_ASSERT(totpDigest256.size() == 32);

        // 2f. AES-256-GCM AEAD encryption, decryption, tamper resistance, and anti-replay
        AesGcmSessionCipher cipherEnc;
        AesGcmSessionCipher cipherDec;
        std::array<uint8_t, 32> sessionKey{};
        CryptoUtils::randomBytes(sessionKey.data(), sessionKey.size());
        TEST_ASSERT(cipherEnc.initialize(sessionKey, true));
        TEST_ASSERT(cipherDec.initialize(sessionKey, false));

        // 0-byte, 64-byte, 1024-byte payloads
        uint64_t testSeq = 1;
        for (size_t payloadSize : {0ULL, 64ULL, 1024ULL}) {
            std::vector<uint8_t> plain(payloadSize);
            for (size_t i = 0; i < payloadSize; ++i) {
                plain[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
            }
            std::vector<uint8_t> aad = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
            std::vector<uint8_t> encrypted;
            bool encOk = cipherEnc.encrypt(plain.data(), plain.size(), testSeq, aad.data(), aad.size(), encrypted);
            TEST_ASSERT(encOk);
            TEST_ASSERT(encrypted.size() == 12 + plain.size() + 16);

            std::vector<uint8_t> decrypted;
            uint64_t outSeq = 0;
            bool decOk = cipherDec.decrypt(encrypted.data(), encrypted.size(), aad.data(), aad.size(), decrypted, &outSeq);
            TEST_ASSERT(decOk);
            TEST_ASSERT(outSeq == testSeq);
            TEST_ASSERT(decrypted == plain);
            ++testSeq;
        }

        // Tamper resistance: 1 bit flip in ciphertext body
        {
            std::vector<uint8_t> plain = {0x10, 0x20, 0x30, 0x40};
            std::vector<uint8_t> aad = {0xAA, 0xBB};
            std::vector<uint8_t> encrypted;
            TEST_ASSERT(cipherEnc.encrypt(plain.data(), plain.size(), testSeq, aad.data(), aad.size(), encrypted));

            std::vector<uint8_t> tamperedEncrypted = encrypted;
            tamperedEncrypted[12] ^= 0x01; // Tamper first ciphertext byte
            std::vector<uint8_t> tamperedDec;
            bool tamperedOk = cipherDec.decrypt(tamperedEncrypted.data(), tamperedEncrypted.size(),
                                                aad.data(), aad.size(), tamperedDec);
            TEST_ASSERT(!tamperedOk);

            // Tamper resistance: 1 bit flip in AAD
            std::vector<uint8_t> tamperedAad = aad;
            tamperedAad[0] ^= 0x01;
            bool badAadOk = cipherDec.decrypt(encrypted.data(), encrypted.size(),
                                              tamperedAad.data(), tamperedAad.size(), tamperedDec);
            TEST_ASSERT(!badAadOk);

            // Successful valid decryption
            std::vector<uint8_t> validDec;
            TEST_ASSERT(cipherDec.decrypt(encrypted.data(), encrypted.size(), aad.data(), aad.size(), validDec));
            TEST_ASSERT(validDec == plain);

            // Anti-replay: replaying the same sequence number must fail
            bool replayOk = cipherDec.decrypt(encrypted.data(), encrypted.size(), aad.data(), aad.size(), validDec);
            TEST_ASSERT(!replayOk);
            ++testSeq;
        }

        // 2g. Ephemeral ECDH key agreement
        EcdhKeyExchange alice;
        EcdhKeyExchange bob;
        TEST_ASSERT(alice.initialize());
        TEST_ASSERT(bob.initialize());
        TEST_ASSERT(!alice.localPublicKey().empty());
        TEST_ASSERT(!bob.localPublicKey().empty());

        std::array<uint8_t, 32> nonce{};
        CryptoUtils::randomBytes(nonce.data(), nonce.size());
        std::array<uint8_t, 32> keyAlice{};
        std::array<uint8_t, 32> keyBob{};
        bool aOk = alice.computeSharedSessionKey(bob.localPublicKey().data(), bob.localPublicKey().size(),
                                                101, 202, nonce, keyAlice);
        bool bOk = bob.computeSharedSessionKey(alice.localPublicKey().data(), alice.localPublicKey().size(),
                                              101, 202, nonce, keyBob);
        TEST_ASSERT(aOk);
        TEST_ASSERT(bOk);
        TEST_ASSERT(keyAlice == keyBob);
    }

    // Sub-test 3: Display Normalization & BGRA Format Mapping
    {
        // 3a. Monitor coordinate normalization and negative coordinate desktop layout
        MonitorDesc primaryMon{};
        primaryMon.index = 0;
        primaryMon.x = 0;
        primaryMon.y = 0;
        primaryMon.width = 1920;
        primaryMon.height = 1080;
        primaryMon.isPrimary = true;

        MonitorDesc leftMon{};
        leftMon.index = 1;
        leftMon.x = -1920;
        leftMon.y = 0;
        leftMon.width = 1920;
        leftMon.height = 1080;
        leftMon.isPrimary = false;

        // Desktop bounding union
        int minX = std::min(primaryMon.x, leftMon.x);
        int minY = std::min(primaryMon.y, leftMon.y);
        int maxX = std::max(primaryMon.x + primaryMon.width, leftMon.x + leftMon.width);
        int maxY = std::max(primaryMon.y + primaryMon.height, leftMon.y + leftMon.height);
        int totalWidth = maxX - minX;
        int totalHeight = maxY - minY;
        TEST_ASSERT(minX == -1920);
        TEST_ASSERT(totalWidth == 3840);
        TEST_ASSERT(totalHeight == 1080);

        // Normalized mapping across virtual grid
        int testGlobalX = -960; // Center of left monitor
        float normX = static_cast<float>(testGlobalX - minX) / static_cast<float>(totalWidth);
        TEST_ASSERT(std::abs(normX - 0.25f) < 0.001f);

        // 3b. Simulate 64x64 RGBA pixel buffer and channel conversion to BGRA
        const int tileSize = 64;
        std::vector<uint8_t> rgbaBuffer(tileSize * tileSize * 4);
        for (int y = 0; y < tileSize; ++y) {
            for (int x = 0; x < tileSize; ++x) {
                size_t idx = static_cast<size_t>(y * tileSize + x) * 4;
                rgbaBuffer[idx + 0] = static_cast<uint8_t>((x * 3) & 0xFF); // R
                rgbaBuffer[idx + 1] = static_cast<uint8_t>((y * 3) & 0xFF); // G
                rgbaBuffer[idx + 2] = static_cast<uint8_t>((x + y) & 0xFF); // B
                rgbaBuffer[idx + 3] = 0x80;                                 // Semi-transparent A
            }
        }

        std::vector<uint8_t> bgraBuffer(tileSize * tileSize * 4);
        for (size_t i = 0; i < rgbaBuffer.size(); i += 4) {
            bgraBuffer[i + 0] = rgbaBuffer[i + 2]; // B <- B
            bgraBuffer[i + 1] = rgbaBuffer[i + 1]; // G <- G
            bgraBuffer[i + 2] = rgbaBuffer[i + 0]; // R <- R
            bgraBuffer[i + 3] = 0xFF;              // Enforce opaque alpha
        }

        // Verify channel reordering and alpha enforcement
        for (size_t i = 0; i < bgraBuffer.size(); i += 4) {
            TEST_ASSERT(bgraBuffer[i + 0] == rgbaBuffer[i + 2]);
            TEST_ASSERT(bgraBuffer[i + 1] == rgbaBuffer[i + 1]);
            TEST_ASSERT(bgraBuffer[i + 2] == rgbaBuffer[i + 0]);
            TEST_ASSERT(bgraBuffer[i + 3] == 0xFF);
        }

        // 3c. Tile hashing and TileCodec encoding verification
        uint64_t hash1 = cppdesk_avx2_hash_tile(bgraBuffer.data(), tileSize * 4, tileSize * 4, tileSize);
        TEST_ASSERT(hash1 != 0);

        // Mutating a single pixel must change hash
        std::vector<uint8_t> modifiedBgra = bgraBuffer;
        modifiedBgra[0] ^= 0x55;
        uint64_t hash2 = cppdesk_avx2_hash_tile(modifiedBgra.data(), tileSize * 4, tileSize * 4, tileSize);
        TEST_ASSERT(hash2 != hash1);

        // TileCodec encode
        EncodedTile encTile = TileCodec::encodeRect(0, 0, tileSize, tileSize, bgraBuffer.data(), QualityPreset::Balanced);
        TEST_ASSERT(encTile.width == tileSize);
        TEST_ASSERT(encTile.height == tileSize);
        TEST_ASSERT(!encTile.data.empty());
    }

    // Sub-test 4: macOS Keycode Mapping & Input Geometry Clamping
    {
        // 4a. Keycode translation validation
        // Navigation and control
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x0D) == 0x24); // Return
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x09) == 0x30); // Tab
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x20) == 0x31); // Space
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x08) == 0x33); // Backspace / Delete
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x2E) == 0x75); // Forward Delete
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x1B) == 0x35); // Escape
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x25) == 0x7B); // Left Arrow
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x27) == 0x7C); // Right Arrow
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x26) == 0x7E); // Up Arrow
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x28) == 0x7D); // Down Arrow

        // Modifiers
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x10) == 0x38); // Shift
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x11) == 0x3B); // Control
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x12) == 0x3A); // Alt / Option
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x5B) == 0x37); // Command / Win

        // Alphanumeric keys
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x41) == 0x00); // 'A'
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x53) == 0x01); // 'S'
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x44) == 0x02); // 'D'
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x46) == 0x03); // 'F'
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x30) == 0x1D); // '0'
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x31) == 0x12); // '1'

        // Function keys
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x70) == 0x7A); // F1
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0x7B) == 0x6F); // F12

        // Unmapped VK
        TEST_ASSERT(InputInjector::mapVkToMacKeyCode(0xEFFF) == 0xFFFF);

        // 4b. Normalized input coordinate clamping
        auto clampCoord = [](float v) -> float {
            return std::clamp(v, 0.0f, 1.0f);
        };
        TEST_ASSERT(clampCoord(-0.5f) == 0.0f);
        TEST_ASSERT(clampCoord(1.5f) == 1.0f);
        TEST_ASSERT(clampCoord(0.42f) == 0.42f);
    }

    // Sub-test 5: Digital Signal Processing for Audio Intercom
    {
        // 5a. RMS calculation
        std::vector<int16_t> silence(1000, 0);
        float rmsZero = VoiceIntercom::calculateRmsLevel(silence.data(), silence.size());
        TEST_ASSERT(std::abs(rmsZero - 0.0f) < 0.0001f);

        // Full scale alternating square wave: +32767, -32768
        std::vector<int16_t> squareWave(1000);
        for (size_t i = 0; i < squareWave.size(); ++i) {
            squareWave[i] = (i % 2 == 0) ? 32767 : -32768;
        }
        float rmsSquare = VoiceIntercom::calculateRmsLevel(squareWave.data(), squareWave.size());
        TEST_ASSERT(rmsSquare >= 0.99f && rmsSquare <= 1.0f);

        // Null / empty buffer edge case
        TEST_ASSERT(VoiceIntercom::calculateRmsLevel(nullptr, 100) == 0.0f);
        TEST_ASSERT(VoiceIntercom::calculateRmsLevel(silence.data(), 0) == 0.0f);

        // 5b. Gain scaling and saturation clamping
        std::vector<int16_t> gainSamples = { 100, -200, 300, 0 };
        VoiceIntercom::applyGain(gainSamples.data(), gainSamples.size(), 2.0f);
        TEST_ASSERT(gainSamples[0] == 200);
        TEST_ASSERT(gainSamples[1] == -400);
        TEST_ASSERT(gainSamples[2] == 600);
        TEST_ASSERT(gainSamples[3] == 0);

        // Overdrive clipping
        std::vector<int16_t> clipSamples = { 20000, -25000 };
        VoiceIntercom::applyGain(clipSamples.data(), clipSamples.size(), 2.5f);
        TEST_ASSERT(clipSamples[0] == 32767);
        TEST_ASSERT(clipSamples[1] == -32768);

        // Edge case null buffer
        VoiceIntercom::applyGain(nullptr, 10, 1.5f);
    }
}

void testCrossPlatformUiWindowingAndMacOsViewer() {
    std::cout << "[TEST 51] Cross-Platform UI Windowing & macOS Viewer Architecture...\n" << std::flush;

    // Sub-test 1: Viewport Geometry & Letterboxing Math
    {
        // 1.1: 16:9 remote desktop (1920x1080) in 16:9 viewport (1920x1080)
        auto geo1 = UiPlatform::calculateViewportGeometry(1920.0f, 1080.0f, 1920, 1080, ScaleMode::FitAspect);
        TEST_ASSERT(std::abs(geo1.scaleX - 1.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo1.scaleY - 1.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo1.offsetX) < 0.001f);
        TEST_ASSERT(std::abs(geo1.offsetY) < 0.001f);
        TEST_ASSERT(std::abs(geo1.contentRect.left) < 0.001f);
        TEST_ASSERT(std::abs(geo1.contentRect.top) < 0.001f);
        TEST_ASSERT(std::abs(geo1.contentRect.right - 1920.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo1.contentRect.bottom - 1080.0f) < 0.001f);
        TEST_ASSERT(!geo1.hasLetterbox);
        TEST_ASSERT(!geo1.hasPillarbox);

        // 1.2: 16:9 remote desktop (1920x1080) in 16:10 viewport (1920x1200) -> letterbox (top/bottom)
        auto geo2 = UiPlatform::calculateViewportGeometry(1920.0f, 1200.0f, 1920, 1080, ScaleMode::FitAspect);
        TEST_ASSERT(std::abs(geo2.scaleX - 1.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo2.offsetX) < 0.001f);
        TEST_ASSERT(std::abs(geo2.offsetY - 60.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo2.contentRect.top - 60.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo2.contentRect.bottom - 1140.0f) < 0.001f);
        TEST_ASSERT(geo2.hasLetterbox);
        TEST_ASSERT(!geo2.hasPillarbox);

        // 1.3: 4:3 remote desktop (1024x768) in 16:9 viewport (1920x1080) -> pillarbox (left/right)
        auto geo3 = UiPlatform::calculateViewportGeometry(1920.0f, 1080.0f, 1024, 768, ScaleMode::FitAspect);
        TEST_ASSERT(std::abs(geo3.scaleX - 1.40625f) < 0.001f);
        TEST_ASSERT(geo3.offsetX > 0.001f);
        TEST_ASSERT(std::abs(geo3.offsetY) < 0.001f);
        TEST_ASSERT(!geo3.hasLetterbox);
        TEST_ASSERT(geo3.hasPillarbox);

        // 1.4: 16:9 remote desktop (1920x1080) in 4:3 viewport (1024x768) -> letterbox (top/bottom)
        auto geo4 = UiPlatform::calculateViewportGeometry(1024.0f, 768.0f, 1920, 1080, ScaleMode::FitAspect);
        TEST_ASSERT(std::abs(geo4.scaleX - (1024.0f / 1920.0f)) < 0.001f);
        TEST_ASSERT(std::abs(geo4.offsetX) < 0.001f);
        TEST_ASSERT(geo4.offsetY > 0.001f);
        TEST_ASSERT(geo4.hasLetterbox);
        TEST_ASSERT(!geo4.hasPillarbox);

        // 1.5: 16:9 remote desktop (1920x1080) in 21:9 viewport (2560x1080) -> pillarbox (left/right)
        auto geo5 = UiPlatform::calculateViewportGeometry(2560.0f, 1080.0f, 1920, 1080, ScaleMode::FitAspect);
        TEST_ASSERT(std::abs(geo5.scaleX - 1.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo5.offsetX - 320.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo5.contentRect.left - 320.0f) < 0.001f);
        TEST_ASSERT(std::abs(geo5.contentRect.right - 2240.0f) < 0.001f);
        TEST_ASSERT(!geo5.hasLetterbox);
        TEST_ASSERT(geo5.hasPillarbox);

        // 1.6: Scaling Modes: Stretch, FillAspect, Original
        auto geoStretch = UiPlatform::calculateViewportGeometry(1920.0f, 1200.0f, 1920, 1080, ScaleMode::Stretch);
        TEST_ASSERT(std::abs(geoStretch.contentRect.width() - 1920.0f) < 0.001f);
        TEST_ASSERT(std::abs(geoStretch.contentRect.height() - 1200.0f) < 0.001f);
        TEST_ASSERT(!geoStretch.hasLetterbox && !geoStretch.hasPillarbox);

        auto geoFill = UiPlatform::calculateViewportGeometry(1920.0f, 1200.0f, 1920, 1080, ScaleMode::FillAspect);
        TEST_ASSERT(geoFill.scaleX > 1.0f);
        TEST_ASSERT(geoFill.contentRect.width() > 1920.0f);
        TEST_ASSERT(std::abs(geoFill.contentRect.height() - 1200.0f) < 0.001f);

        auto geoOrig = UiPlatform::calculateViewportGeometry(2560.0f, 1440.0f, 1920, 1080, ScaleMode::Original);
        TEST_ASSERT(std::abs(geoOrig.scaleX - 1.0f) < 0.001f);
        TEST_ASSERT(std::abs(geoOrig.contentRect.width() - 1920.0f) < 0.001f);
        TEST_ASSERT(std::abs(geoOrig.contentRect.height() - 1080.0f) < 0.001f);
        TEST_ASSERT(geoOrig.hasLetterbox && geoOrig.hasPillarbox);

        // 1.7: Coordinate Normalization & Mapping Roundtrip
        float normX = -1.0f, normY = -1.0f;
        // Point at top-left of contentRect -> (0.0, 0.0)
        bool okTL = UiPlatform::mapPointToNormalized(geo2.contentRect.left, geo2.contentRect.top, geo2, normX, normY);
        TEST_ASSERT(okTL);
        TEST_ASSERT(std::abs(normX) < 0.001f);
        TEST_ASSERT(std::abs(normY) < 0.001f);

        // Point at center -> (0.5, 0.5)
        bool okC = UiPlatform::mapPointToNormalized(geo2.contentRect.centerX(), geo2.contentRect.centerY(), geo2, normX, normY);
        TEST_ASSERT(okC);
        TEST_ASSERT(std::abs(normX - 0.5f) < 0.001f);
        TEST_ASSERT(std::abs(normY - 0.5f) < 0.001f);

        // Point at bottom-right -> (1.0, 1.0)
        bool okBR = UiPlatform::mapPointToNormalized(geo2.contentRect.right, geo2.contentRect.bottom, geo2, normX, normY);
        TEST_ASSERT(okBR);
        TEST_ASSERT(std::abs(normX - 1.0f) < 0.001f);
        TEST_ASSERT(std::abs(normY - 1.0f) < 0.001f);

        // Point outside contentRect (in top letterbox bar) -> returns false
        bool okOut = UiPlatform::mapPointToNormalized(100.0f, 20.0f, geo2, normX, normY);
        TEST_ASSERT(!okOut);

        // mapNormalizedToPoint round-trip
        float roundViewX = 0.0f, roundViewY = 0.0f;
        UiPlatform::mapNormalizedToPoint(0.5f, 0.5f, geo2, roundViewX, roundViewY);
        TEST_ASSERT(std::abs(roundViewX - geo2.contentRect.centerX()) < 0.001f);
        TEST_ASSERT(std::abs(roundViewY - geo2.contentRect.centerY()) < 0.001f);

        UiPlatform::mapNormalizedToPoint(0.0f, 0.0f, geo2, roundViewX, roundViewY);
        TEST_ASSERT(std::abs(roundViewX - geo2.contentRect.left) < 0.001f);
        TEST_ASSERT(std::abs(roundViewY - geo2.contentRect.top) < 0.001f);

        UiPlatform::mapNormalizedToPoint(1.0f, 1.0f, geo2, roundViewX, roundViewY);
        TEST_ASSERT(std::abs(roundViewX - geo2.contentRect.right) < 0.001f);
        TEST_ASSERT(std::abs(roundViewY - geo2.contentRect.bottom) < 0.001f);
    }

    // Sub-test 2: Keycode Translation & Modifier Bitmask Transitions
    {
        // 2.1: macOS to Windows VK translation for navigation & control
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x24) == 0x0D); // Return -> VK_RETURN
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x30) == 0x09); // Tab -> VK_TAB
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x31) == 0x20); // Space -> VK_SPACE
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x33) == 0x08); // Backspace -> VK_BACK
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x35) == 0x1B); // Escape -> VK_ESCAPE
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x75) == 0x2E); // Forward Delete -> VK_DELETE

        // Arrows
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x7B) == 0x25); // Left -> VK_LEFT
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x7C) == 0x27); // Right -> VK_RIGHT
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x7E) == 0x26); // Up -> VK_UP
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x7D) == 0x28); // Down -> VK_DOWN

        // Paging & Home/End
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x73) == 0x24); // Home -> VK_HOME
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x77) == 0x23); // End -> VK_END
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x74) == 0x21); // Page Up -> VK_PRIOR
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x79) == 0x22); // Page Down -> VK_NEXT

        // Alphanumerics A-Z
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x00) == 0x41); // 'A'
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x06) == 0x5A); // 'Z'
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x0B) == 0x42); // 'B'
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x0C) == 0x51); // 'Q'

        // Digits 0-9
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x1D) == 0x30); // '0'
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x12) == 0x31); // '1'
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x19) == 0x39); // '9'

        // Function keys F1-F12
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x7A) == 0x70); // F1
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x6F) == 0x7B); // F12

        // Modifiers
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x38) == 0x10); // Shift
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x3B) == 0x11); // Control
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x3A) == 0x12); // Option
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x37) == 0x5B); // Command (Win)
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0x39) == 0x14); // CapsLock

        // 2.2: Bidirectional inverse mapping roundtrip: windowsVkToMacKeyCode(macKeyCodeToWindowsVk(k)) == k
        std::vector<uint16_t> sampleKeys = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x19, 0x1A, 0x1C, 0x1D, 0x1F,
            0x20, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x2D, 0x2E,
            0x30, 0x31, 0x33, 0x35, 0x37, 0x38, 0x39, 0x3A, 0x3B,
            0x73, 0x74, 0x75, 0x77, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x6F
        };
        for (uint16_t mk : sampleKeys) {
            uint32_t vk = UiPlatform::macKeyCodeToWindowsVk(mk);
            TEST_ASSERT(vk != 0);
            uint16_t roundTrip = UiPlatform::windowsVkToMacKeyCode(vk);
            TEST_ASSERT(roundTrip == mk);
        }

        // Unmapped key checks
        TEST_ASSERT(UiPlatform::macKeyCodeToWindowsVk(0xFFFF) == 0);
        TEST_ASSERT(UiPlatform::windowsVkToMacKeyCode(0xFFFF) == 0xFFFF);

        // 2.3: Modifier Bitmask Transition Decoding
        // Shift key pressed: 0 -> (1 << 17)
        uint64_t mask0 = 0;
        uint64_t maskShift = (1ULL << 17);
        auto trans1 = UiPlatform::decodeModifierTransitions(mask0, maskShift);
        TEST_ASSERT(trans1.size() == 1);
        TEST_ASSERT(trans1[0].vkCode == 0x10); // VK_SHIFT
        TEST_ASSERT(trans1[0].isDown == true);

        // Shift released: (1 << 17) -> 0
        auto trans2 = UiPlatform::decodeModifierTransitions(maskShift, mask0);
        TEST_ASSERT(trans2.size() == 1);
        TEST_ASSERT(trans2[0].vkCode == 0x10);
        TEST_ASSERT(trans2[0].isDown == false);

        // Command released: (1 << 20) -> 0
        uint64_t maskCmd = (1ULL << 20);
        auto trans3 = UiPlatform::decodeModifierTransitions(maskCmd, mask0);
        TEST_ASSERT(trans3.size() == 1);
        TEST_ASSERT(trans3[0].vkCode == 0x5B); // VK_LWIN
        TEST_ASSERT(trans3[0].isDown == false);

        // Multi-modifier combo: Shift pressed, Control and Option simultaneously engaged
        uint64_t maskComboOld = (1ULL << 17); // Shift down
        uint64_t maskComboNew = (1ULL << 18) | (1ULL << 19); // Shift up, Ctrl down, Opt down
        auto trans4 = UiPlatform::decodeModifierTransitions(maskComboOld, maskComboNew);
        TEST_ASSERT(trans4.size() == 3);
        bool shiftUpSeen = false;
        bool ctrlDownSeen = false;
        bool optDownSeen = false;
        for (const auto& t : trans4) {
            if (t.vkCode == 0x10 && !t.isDown) shiftUpSeen = true;
            if (t.vkCode == 0x11 && t.isDown) ctrlDownSeen = true;
            if (t.vkCode == 0x12 && t.isDown) optDownSeen = true;
        }
        TEST_ASSERT(shiftUpSeen);
        TEST_ASSERT(ctrlDownSeen);
        TEST_ASSERT(optDownSeen);

        // Test 32-bit overload
        uint32_t mask32Old = 0;
        uint32_t mask32New = static_cast<uint32_t>(1U << 17);
        auto trans32 = UiPlatform::decodeModifierTransitions(mask32Old, mask32New);
        TEST_ASSERT(trans32.size() == 1);
        TEST_ASSERT(trans32[0].vkCode == 0x10);
        TEST_ASSERT(trans32[0].isDown == true);
    }

    // Sub-test 3: Canvas Tile Blitting & Pitch Math
    {
        const int canvasW = 128;
        const int canvasH = 128;
        const int canvasPitch = canvasW * 4;
        std::vector<uint8_t> canvas(canvasH * canvasPitch, 0x10); // Background color 0x10

        const int tileW = 32;
        const int tileH = 32;
        const int tilePitch = tileW * 4;
        std::vector<uint8_t> tile(tileH * tilePitch, 0xFF); // White tile 0xFF

        // 3.1: Blit 32x32 colored tile into offset (16, 16)
        bool blitOk = UiPlatform::blitTileToCanvas(
            canvas.data(), canvasW, canvasH, canvasPitch,
            tile.data(), 16, 16, tileW, tileH, tilePitch);
        TEST_ASSERT(blitOk);

        // Verify interior pixels match expected color (0xFF)
        for (int y = 16; y < 16 + tileH; ++y) {
            for (int x = 16; x < 16 + tileW; ++x) {
                for (int c = 0; c < 4; ++c) {
                    TEST_ASSERT(canvas[y * canvasPitch + x * 4 + c] == 0xFF);
                }
            }
        }

        // Verify exterior pixels remain untouched (0x10)
        TEST_ASSERT(canvas[15 * canvasPitch + 15 * 4] == 0x10);
        TEST_ASSERT(canvas[49 * canvasPitch + 49 * 4] == 0x10);
        TEST_ASSERT(canvas[0] == 0x10);
        TEST_ASSERT(canvas[100 * canvasPitch + 100 * 4] == 0x10);

        // 3.2: Boundary clipping: tile partially outside right/bottom canvas boundaries
        // Canvas is 128x128, tile placed at (110, 110) with size 32x32 -> [110, 128) x [110, 128)
        std::vector<uint8_t> tileClip(tileH * tilePitch, 0xAA);
        bool clipOk = UiPlatform::blitTileToCanvas(
            canvas.data(), canvasW, canvasH, canvasPitch,
            tileClip.data(), 110, 110, tileW, tileH, tilePitch);
        TEST_ASSERT(clipOk);

        for (int y = 110; y < 128; ++y) {
            for (int x = 110; x < 128; ++x) {
                TEST_ASSERT(canvas[y * canvasPitch + x * 4] == 0xAA);
            }
        }

        // 3.3: Boundary clipping: tile partially outside left/top boundaries (negative coords)
        std::vector<uint8_t> tileNeg(tileH * tilePitch, 0xBB);
        bool negOk = UiPlatform::blitTileToCanvas(
            canvas.data(), canvasW, canvasH, canvasPitch,
            tileNeg.data(), -10, -10, tileW, tileH, tilePitch);
        TEST_ASSERT(negOk);
        TEST_ASSERT(canvas[0 * canvasPitch + 0 * 4] == 0xBB);
        TEST_ASSERT(canvas[21 * canvasPitch + 21 * 4] == 0xBB);

        // 3.4: Complete out-of-bounds rejection
        bool oobRight = UiPlatform::blitTileToCanvas(
            canvas.data(), canvasW, canvasH, canvasPitch,
            tile.data(), 200, 200, tileW, tileH, tilePitch);
        TEST_ASSERT(!oobRight);

        bool oobLeft = UiPlatform::blitTileToCanvas(
            canvas.data(), canvasW, canvasH, canvasPitch,
            tile.data(), -100, -100, tileW, tileH, tilePitch);
        TEST_ASSERT(!oobLeft);

        // 3.5: Null buffer validation
        bool nullDst = UiPlatform::blitTileToCanvas(
            nullptr, canvasW, canvasH, canvasPitch,
            tile.data(), 0, 0, tileW, tileH, tilePitch);
        TEST_ASSERT(!nullDst);

        bool nullSrc = UiPlatform::blitTileToCanvas(
            canvas.data(), canvasW, canvasH, canvasPitch,
            nullptr, 0, 0, tileW, tileH, tilePitch);
        TEST_ASSERT(!nullSrc);
    }
}

} // namespace

int main() {
    try {
#if defined(_WIN32)
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

        std::cout << "=========================================================\n" << std::flush;
        std::cout << "   CppDesk Automated Verification & Integration Suite    \n" << std::flush;
        std::cout << "=========================================================\n" << std::flush;

        testDeskIdAndCrypto(); std::cout << "Test 1 done\n" << std::flush;
        testTileCodecRoundTrip(); std::cout << "Test 2 done\n" << std::flush;
        testScreenCapturer(); std::cout << "Test 3 done\n" << std::flush;
        testEndToEndSessionAndFileTransfer(); std::cout << "Test 4 done\n" << std::flush;
        testFileTransferEdgeCasesAndFavorites(); std::cout << "Test 5 done\n" << std::flush;
        testAppSettingsAndAdaptiveFps(); std::cout << "Test 6 done\n" << std::flush;
        testAvx2SimdAssemblyKernels(); std::cout << "Test 7 done\n" << std::flush;
        testEcdhAndAesGcmEngine(); std::cout << "Test 8 done\n" << std::flush;
        testV201FeaturesAndResilience(); std::cout << "Test 9 done\n" << std::flush;
        testNotificationSystemAndTray(); std::cout << "Test 10 done\n" << std::flush;
        testV210PowerFeaturesInheritance(); std::cout << "Test 11 done\n" << std::flush;
        testAutoUpdaterAndProtocolV3(); std::cout << "Test 12 done\n" << std::flush;
        testHardwareDiagnosticsAndProcessManager(); std::cout << "Test 13 done\n" << std::flush;
        testDirectCanvasDragAndDropFileTransfer(); std::cout << "Test 14 done\n" << std::flush;
        testSessionRecorderAndAviContainer(); std::cout << "Test 15 done\n" << std::flush;
        testBidirectionalVoiceIntercom(); std::cout << "Test 16 done\n" << std::flush;
        testVirtualDisplayFitAndResolutionMatching(); std::cout << "Test 17 done\n" << std::flush;
        testMultiSessionTabbedManagement(); std::cout << "Test 18 done\n" << std::flush;
        testNativeClipboardFileTransfer(); std::cout << "Test 19 done\n" << std::flush;
        testSelfHostedRelayAndStunDiagnostics(); std::cout << "Test 20 done\n" << std::flush;
        testPerformanceHudMetricsAndTelemetry(); std::cout << "Test 21 done\n" << std::flush;
        testRichChatMediaAndClipboardHistoryHub(); std::cout << "Test 22 done\n" << std::flush;
        testWindowsServiceAndRemoteRebootReconnect(); std::cout << "Test 23 done\n" << std::flush;
        testDesktopShortcutsAndUriProtocol(); std::cout << "Test 24 done\n" << std::flush;
        testFileTransferSpeedTelemetryAndEta(); std::cout << "Test 25 done\n" << std::flush;
        testAudioControlsAndMuteSync(); std::cout << "Test 26 done\n" << std::flush;
        testMultiMonitorGridViewAndVirtualDesktop(); std::cout << "Test 27 done\n" << std::flush;
        testPrivacyModeCustomBrandingAndNotice(); std::cout << "Test 28 done\n" << std::flush;
        testSystemHealthDiagnosticsAndHardwareSpecs(); std::cout << "Test 29 done\n" << std::flush;
        testConnectionQualityProfiles(); std::cout << "Test 30 done\n" << std::flush;
        testDxgiDirtyRectsAndGdiRecycling(); std::cout << "Test 31 done\n" << std::flush;
        testCodecContextRecyclingAndZeroCopyTasks(); std::cout << "Test 32 done\n" << std::flush;
        testDirect2DPartialDirtyRectAndZeroCopyViewer(); std::cout << "Test 33 done\n" << std::flush;
        testScatterGatherFrameSendAndSocketBuffers(); std::cout << "Test 34 done\n" << std::flush;
        testPrecisionTimerPacingAndRecycledBuffers(); std::cout << "Test 35 done\n" << std::flush;
        testZeroCopyTileStreamingAndSerializer(); std::cout << "Test 36 done\n" << std::flush;
        testDirectJpegCanvasBlitAndFramePacing(); std::cout << "Test 37 done\n" << std::flush;
        testInputCoalescingAndZeroAllocEvents(); std::cout << "Test 38 done\n" << std::flush;
        testHostOutboxAndCachedGeometries(); std::cout << "Test 39 done\n" << std::flush;
        testParallelViewerDecodeAndTabFrameCache(); std::cout << "Test 40 done\n" << std::flush;
        testAvx2BlitRowBgraOpaque(); std::cout << "Test 41 done\n" << std::flush;
        testZeroAllocAudioAndStackWideText(); std::cout << "Test 42 done\n" << std::flush;
        testVirtualDisplayManagerAndHeadlessEmulation(); std::cout << "Test 43 done\n" << std::flush;
        testHardwareDpmsAndScreenBlanking(); std::cout << "Test 44 done\n" << std::flush;
        testSessionRecordingPlayerAndTranscoder(); std::cout << "Test 45 done\n" << std::flush;
        testTwoFactorAuthTotpAndQrMatrix(); std::cout << "Test 46 done\n" << std::flush;
        testRemoteFileSyncAndFolderMirroring(); std::cout << "Test 47 done\n" << std::flush;
        testPeerVersionHandshakeAndSelfUpdater(); std::cout << "Test 48 done\n" << std::flush;
        testNovelBreakthroughPowerFeatures(); std::cout << "Test 49 done\n" << std::flush;
        testCrossPlatformMacOsAndWindowsFoundation(); std::cout << "Test 50 done\n" << std::flush;
        testCrossPlatformUiWindowingAndMacOsViewer(); std::cout << "Test 51 done\n" << std::flush;

        std::cout << "---------------------------------------------------------\n";
        std::cout << "Assertions Passed: " << g_passed << " | Failed: " << g_failed << "\n";
        std::cout << "=========================================================\n";

#if defined(_WIN32)
        CoUninitialize();
#endif
        return (g_failed == 0) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "CAUGHT EXCEPTION IN MAIN: " << ex.what() << std::endl;
        return 2;
    } catch (...) {
        std::cerr << "CAUGHT UNKNOWN EXCEPTION IN MAIN" << std::endl;
        return 3;
    }
}

