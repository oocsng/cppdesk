#pragma once

#include "socket_compat.hpp"
#include "../core/protocol.hpp"
#include "../core/crypto_identity.hpp"
#include "../capture/screen_capture.hpp"
#include "../control/input_injector.hpp"
#include "../control/clipboard_file_manager.hpp"
#include "../control/whiteboard_manager.hpp"
#include "../media/voice_intercom.hpp"
#include "../capture/display_manager.hpp"
#include "../control/file_sync_manager.hpp"
#include "../control/scratchpad_manager.hpp"
#include "../control/command_palette.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <condition_variable>

namespace cppdesk {

struct DiscoveredPeer {
    uint64_t    deskId = 0;
    std::string hostname;
    std::string ip;
    uint16_t    port = DEFAULT_HOST_PORT;
    uint64_t    lastSeenTickMs = 0;
    bool        viaRelay = false;
};

struct PendingIncomingRequest {
    bool        active = false;
    uint64_t    callerDeskId = 0;
    std::string callerHostname;
    std::string callerIp;
    uint8_t     proposedPermissions = PERM_ALL;
};

struct HostSessionStatus {
    bool        active = false;
    uint64_t    viewerDeskId = 0;
    std::string viewerHostname;
    std::string viewerIp;
    uint8_t     permissions = PERM_ALL;
    uint64_t    connectedSinceTickMs = 0;
    std::string securityFingerprint;
    std::string viewerAppVersion;
};

struct ChatMessageEntry {
    std::string senderName;
    std::string text;
    bool        fromLocal = false;
    uint64_t    timestampMs = 0;
    bool        hasImage = false;
    std::vector<uint8_t> imageJpegData;
    uint32_t    imgWidth = 0;
    uint32_t    imgHeight = 0;

    ChatMessageEntry() = default;
    ChatMessageEntry(std::string sender, std::string txt, bool local, uint64_t ts,
                     bool hasImg = false, std::vector<uint8_t> jpeg = {}, uint32_t w = 0, uint32_t h = 0)
        : senderName(std::move(sender)), text(std::move(txt)), fromLocal(local), timestampMs(ts),
          hasImage(hasImg), imageJpegData(std::move(jpeg)), imgWidth(w), imgHeight(h) {}
};

struct PortForwardRule {
    uint32_t    ruleId = 0;
    uint16_t    localPort = 0;
    uint16_t    targetPort = 0;
    std::string description;
    bool        active = false;
    uint64_t    bytesTransferredIn = 0;
    uint64_t    bytesTransferredOut = 0;
};

struct ViewerSessionStats {
    ViewerConnectionState state = ViewerConnectionState::Disconnected;
    std::string           statusMessage = "Ready";
    uint64_t              remoteDeskId = 0;
    std::string           remoteHostname;
    std::string           remoteAddress;
    std::string           securityFingerprint;
    uint64_t              connectedSinceTickMs = 0;
    uint8_t               grantedPermissions = PERM_ALL;
    QualityPreset         qualityPreset = QualityPreset::Balanced;
    ConnectionProfile     connectionProfile = ConnectionProfile::BalancedBandwidth;
    uint8_t               targetFps = 30;
    uint8_t               effectiveFpsCap = 30;
    bool                  adaptiveFps = true;
    bool                  networkThrottled = false;
    int                   activeMonitorIndex = 0;
    int                   monitorCount = 1;
    std::vector<MonitorDesc> monitors;
    int                   frameWidth = 0;
    int                   frameHeight = 0;
    float                 fps = 0.0f;
    uint32_t              rttMs = 0;
    float                 kbps = 0.0f;
    CursorState           remoteCursor;
    uint32_t              reconnectAttempt = 0;
    bool                  privacyModeEngaged = false;
    bool                  audioMuted = false;
    int                   audioVolume = 100;
    float                 captureLatencyMs = 0.0f;
    float                 encodeLatencyMs = 0.0f;
    float                 decodeLatencyMs = 0.0f;
    float                 compressionRatio = 1.0f;
    uint32_t              deltaTilesCount = 0;
    float                 packetLossPercent = 0.0f;
    std::vector<float>    rttHistory;
    VirtualDisplayStatusPayload lastVirtualDisplayStatus{};
};

struct StunNatResult {
    bool        success = false;
    std::string publicIp;
    uint16_t    publicPort = 0;
    std::string natTypeDescription;
    int         rttMs = -1;
};

struct RelayProbeResult {
    bool        reachable = false;
    int         rttMs = -1;
    std::string message;
};

struct NetworkDiagnosticResult {
    bool             running = false;
    bool             completed = false;
    RelayProbeResult relay;
    StunNatResult    stun;
};

class RelayServer {
public:
    RelayServer();
    ~RelayServer();

    bool start(uint16_t port = DEFAULT_RELAY_PORT);
    void stop();
    bool isRunning() const { return running_.load(); }
    uint16_t port() const { return port_; }
    size_t registeredPeerCount() const;

private:
    void acceptLoop();
    void handleClientSocket(uintptr_t sock, std::string peerIp);

    struct RegisteredHost {
        uint64_t    deskId = 0;
        std::string hostname;
        std::string ip;
        uint16_t    tcpPort = 0;
        uintptr_t   controlSock = ~uintptr_t(0);
        uint64_t    lastSeenMs = 0;
    };

    struct PendingBridge {
        uint64_t  token = 0;
        uintptr_t viewerSock = ~uintptr_t(0);
        uintptr_t hostSock = ~uintptr_t(0);
        bool      ready = false;
    };

    std::atomic<bool>           running_{false};
    uint16_t                    port_ = DEFAULT_RELAY_PORT;
    uintptr_t                   listenSock_ = ~uintptr_t(0);
    std::thread                 acceptThread_;

    mutable std::mutex          mutex_;
    std::condition_variable     bridgeCv_;
    std::vector<RegisteredHost> hosts_;
    std::vector<PendingBridge>  bridges_;
};

class NetworkEngine {
public:
    explicit NetworkEngine(IdentityManager& identity);
    ~NetworkEngine();

    bool start();
    void stop();

    // Local IP & listening status
    std::string localIpAddress() const { return localIp_; }
    uint16_t hostListenPort() const { return identity_.listenPort(); }

    // Discovery
    std::vector<DiscoveredPeer> discoveredPeers() const;
    void sendDiscoveryQuery(uint64_t targetDeskId = 0);

    // Built-in Relay Server control
    bool startLocalRelayServer(uint16_t port = DEFAULT_RELAY_PORT);
    void stopLocalRelayServer();
    bool isLocalRelayRunning() const { return localRelay_.isRunning(); }
    size_t localRelayPeerCount() const { return localRelay_.registeredPeerCount(); }

    // Host: Incoming connection approval & active host session control
    PendingIncomingRequest pendingIncomingRequest() const;
    void respondToIncomingRequest(bool accept, uint8_t permissions);
    void setAutoAcceptIncoming(bool autoAccept, uint8_t defaultPerms = PERM_ALL);

    HostSessionStatus hostSessionStatus() const;
    void updateHostSessionPermissions(uint8_t newPermissions);
    void disconnectHostClient();
    void clearRateLimitRecords();

    // Viewer: Outgoing connection & remote session control
    bool connectToRemote(const std::string& targetIdOrAddr, const std::string& password);
    void disconnectViewer();
    bool isViewerActive() const { return viewerActive_.load(); }

    // Viewer 2FA synchronization
    void submitViewerTotpCode(const std::string& code);
    void cancelViewerTotp();
    bool isWaitingForTotp() const;

    ViewerSessionStats viewerStats() const;

    // Copy latest decoded remote frame if frameSeq > lastSeenSeq
    bool copyLatestViewerFrame(
        uint64_t& inOutSeq,
        std::vector<uint8_t>& outBgra,
        int& outW,
        int& outH,
        CursorState& outCursor,
        RECT* outDirtyBounds = nullptr) const;
    void markViewerCanvasFullDirty();

    // Frame notification callback for event-driven 60 FPS viewer rendering
    void setOnFrameDecodedCallback(std::function<void()> cb);

    // Viewer input & session actions
    void sendMouseMove(float normX, float normY);
    void sendMouseButton(MouseButtonId button, bool isDown, float normX, float normY);
    void sendMouseWheel(int32_t verticalDelta, int32_t horizontalDelta = 0);
    void sendKeyEvent(uint16_t vkCode, uint16_t scanCode, bool isDown, bool isExtended);
    void sendReleaseAllModifiers();
    void sendSystemAction(SystemActionType action);
    bool requestRemoteReboot(bool safeMode = false, uint32_t countdownSeconds = 5);
    bool isRebootPending() const { return rebootPending_.load(); }
    uint32_t rebootCountdown() const { return rebootCountdown_.load(); }
    bool isAutoReconnectingWithToken() const { return autoReconnectingWithToken_.load(); }
    std::string rebootResumeToken() const;
    void cancelAutoReconnection();
    void requestVideoSettings(QualityPreset preset, int monitorIndex, bool forceKeyframe, uint8_t targetFps = 0, int adaptiveFps = -1);
    void setSessionFpsConfig(uint8_t targetFps, bool adaptiveFps);
    void selectRemoteMonitor(int monitorIndex);
    void updateQualitySettings(QualityPreset preset, uint8_t targetFps, bool adaptiveFps);
    void applyConnectionProfile(ConnectionProfile profile);

    // Virtual Multi-Display Control APIs
    bool requestCreateVirtualDisplay(uint32_t width = 1920, uint32_t height = 1080, uint32_t refreshRate = 60, bool preferIddCx = true);
    bool requestDestroyVirtualDisplay(uint32_t displayId);
    bool requestSetVirtualDisplayMode(uint32_t displayId, uint32_t width, uint32_t height, uint32_t refreshRate);

    bool isClipboardSyncEnabled() const { return clipboardSyncEnabled_.load(); }
    void setClipboardSyncEnabled(bool enabled) { clipboardSyncEnabled_.store(enabled); }
    ClipboardHistoryManager& clipboardHistory() { return clipboardManager_.history(); }
    const ClipboardHistoryManager& clipboardHistory() const { return clipboardManager_.history(); }

    // File transfer, clipboard & live encrypted chat
    uint32_t sendFile(const std::string& filePath,
                      FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads,
                      float dropNx = 0.0f, float dropNy = 0.0f);
    int sendDropPath(const std::string& path,
                     FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads,
                     float dropNx = 0.0f, float dropNy = 0.0f);
    bool cancelFileTransfer(uint32_t transferId);
    void pushLocalClipboardNow();
    bool sendChatMessage(const std::string& text);
    bool sendChatImage(const std::vector<uint8_t>& jpegData, uint32_t w, uint32_t h, const std::string& caption = "");
    std::vector<ChatMessageEntry> chatMessages() const;
    uint32_t unreadChatCount() const { return unreadChatCount_.load(); }
    void markChatRead() { unreadChatCount_.store(0); }

    FileTransferManager& fileTransferManager() { return fileManager_; }
    const FileTransferManager& fileTransferManager() const { return fileManager_; }
    ClipboardFileTransferManager& clipboardFileTransferManager() { return clipFileMgr_; }
    const ClipboardFileTransferManager& clipboardFileTransferManager() const { return clipFileMgr_; }
    ClipboardManager& clipboardManager() { return clipboardManager_; }
    const ClipboardManager& clipboardManager() const { return clipboardManager_; }
    FileSyncManager& fileSyncManager() { return fileSyncMgr_; }
    const FileSyncManager& fileSyncManager() const { return fileSyncMgr_; }

    // Real-Time Collaborative Dual-Peer Scratchpad
    ScratchpadManager& scratchpadManager() { return scratchpadMgr_; }
    const ScratchpadManager& scratchpadManager() const { return scratchpadMgr_; }
    void setOnScratchpadUpdateCallback(std::function<void(const ScratchpadUpdatePayload&)> cb);
    bool sendScratchpadText(const std::string& text, uint32_t cursorPos = 0);

    // Spotlight Omnibar Command Palette
    CommandPaletteManager& commandPaletteManager() { return commandPaletteMgr_; }
    const CommandPaletteManager& commandPaletteManager() const { return commandPaletteMgr_; }
    void setOnCommandPaletteOutputCallback(std::function<void(const CommandPaletteOutputPayload&)> cb);
    bool sendCommandPaletteExec(const std::string& commandLine, uint32_t execId = 0);

    bool requestRemoteScan(const std::string& remotePath);
    bool requestRemoteBlockHashes(const std::string& relativePath);
    bool startFolderSync(const std::string& localPath, const std::string& remotePath, SyncMode mode, bool purge = false);
    void cancelFolderSync();

    // Audio streaming controls (v2.1.0)
    void setAudioVolume(int percent);
    int audioVolume() const;
    void setAudioMuted(bool muted);
    bool isAudioMuted() const;
    void syncAudioControl();
    bool isHostAudioSuspended() const;

    // Privacy screen controls (v2.1.0, v3.2.0 & DPMS power management)
    void requestTogglePrivacyMode(ScreenBlankMode blankMode = ScreenBlankMode::Unified);
    bool isPrivacyModeEngaged() const;
    bool isHostPrivacyModeActive() const;
    void setHostPrivacyMode(bool enable, const std::string& notice = "", const std::string& brand = "", bool showId = true, ScreenBlankMode blankMode = ScreenBlankMode::Unified);
    void configurePrivacyCurtain(const std::string& notice, const std::string& brand, bool showId);

    // TCP Port Forwarding & Tunneling Manager (v2.1.0)
    uint32_t addPortForwardRule(uint16_t localPort, uint16_t targetPort, const std::string& desc, bool startActive = true);
    void removePortForwardRule(uint32_t ruleId);
    void setPortForwardRuleActive(uint32_t ruleId, bool active);
    std::vector<PortForwardRule> portForwardRules() const;
    void startPortForwarding();
    void stopPortForwarding();

    // Interactive Remote Terminal Console (v2.1.0)
    void sendTerminalCommand(const std::string& cmd);
    void resetRemoteTerminal(bool usePowerShell = false);
    std::vector<std::string> getTerminalScrollback() const;
    void clearTerminalScrollback();

    // Whiteboard & Screen Annotation (v2.1.0)
    WhiteboardManager& whiteboardManager() { return whiteboardMgr_; }
    const WhiteboardManager& whiteboardManager() const { return whiteboardMgr_; }
    void sendWhiteboardStroke(const AnnotationStroke& stroke);
    void sendWhiteboardClear();
    void sendWhiteboardLaser(float normX, float normY);

    // Remote Hardware Diagnostics & Live Process Telemetry (Feature 1)
    static SystemDiagnosticsPayload sampleHostDiagnostics();
    bool executeProcessKill(uint32_t pid, uint8_t callerPermissions);
    void setDiagnosticsActive(bool active);
    bool isDiagnosticsActive() const { return viewerDiagnosticsActive_.load(); }
    void sendProcessKill(uint32_t pid);
    SystemDiagnosticsPayload latestDiagnostics() const;

    // Bidirectional Voice Intercom (VoIP Microphone & Playback - Feature 4)
    bool startVoiceIntercom();
    void stopVoiceIntercom();
    bool isVoiceIntercomActive() const;
    void setVoiceIntercomMicMuted(bool muted);
    bool isVoiceIntercomMicMuted() const;
    float voiceIntercomInputLevel() const;
    void setVoiceInputDevice(int id);
    void setVoiceOutputDevice(int id);
    void handleIncomingVoiceChunk(const uint8_t* payload, size_t len);
    bool startVoiceMonitor();
    void stopVoiceMonitor();
    bool isVoiceMonitoring() const;

    // Virtual Display Fit & Dynamic Resolution Matching (Feature 5)
    bool requestHostResolution(uint32_t width, uint32_t height, uint8_t mode = 2);
    bool restoreHostResolution();
    bool isHostResolutionChanged() const;
    void handleIncomingResolutionChangeReq(const uint8_t* payload, size_t len, uint8_t callerPermissions);

    // Relay & STUN Network Diagnostics (v3.2.0)
    static StunNatResult queryStunServer(const std::string& hostPort, uint32_t timeoutMs = 2000);
    static RelayProbeResult probeRelayServer(const std::string& hostPort, uint32_t timeoutMs = 2000);
    void setRelayAddressAndReconnect(const std::string& newAddr);
    void startNetworkDiagnostics(const std::string& relayAddr, const std::string& stunAddr);
    bool isNetworkDiagnosticRunning() const;
    bool getNetworkDiagnosticResult(RelayProbeResult& outRelay, StunNatResult& outStun) const;

    // Peer Version Tracking & Compatibility
    std::string remotePeerAppVersion() const;
    bool isLocalVersionOutdated() const;
    bool isRemoteVersionOutdated() const;

    void setOnVersionUpgradeNoticeCallback(std::function<void(const VersionUpgradeNoticePayload&)> cb);
    void setOnVersionMismatchCallback(std::function<void(const std::string& hostVer, const std::string& viewerVer)> cb);

    // Socket options and scatter-gather transmission helpers
    static void setTcpNoDelay(socket_t s);
    static bool sendScatterGather(
        socket_t s,
        const void* hdrBuf,
        size_t hdrLen,
        const void* payloadBuf = nullptr,
        size_t payloadLen = 0);

    // Socket framing helpers (AES-256-GCM AEAD encryption + tamper verification)
    static bool sendFrame(
        uintptr_t sock,
        PacketType type,
        uint8_t flags,
        const void* payload,
        size_t payloadLen,
        std::mutex& sendMutex,
        AesGcmSessionCipher* cipher = nullptr,
        uint64_t* sendSeq = nullptr);

    static bool recvFrame(
        uintptr_t sock,
        FrameHeader& outHeader,
        std::vector<uint8_t>& outPayload,
        AesGcmSessionCipher* cipher = nullptr,
        uint64_t* recvSeq = nullptr);

private:
    // Background worker loops
    void discoveryLoop();
    void relayRegistrationLoop();
    void hostAcceptLoop();
    void runHostSession(uintptr_t clientSock, std::string clientIp);
    void runViewerSession(std::string targetInput, std::string password);

    // Brute-force protection helpers
    bool isIpRateLimited(const std::string& ip);
    void recordAuthResultForIp(const std::string& ip, bool success);

    // Encrypted send helpers for active sessions
    bool sendHostEncryptedPacket(PacketType type, uint8_t flags, const void* payload, size_t payloadLen);
    bool sendViewerEncryptedPacket(PacketType type, uint8_t flags, const void* payload, size_t payloadLen);

    IdentityManager&            identity_;
    std::string                 localIp_ = "127.0.0.1";
    std::atomic<bool>           running_{false};

    // Discovery
    uintptr_t                   udpSock_ = ~uintptr_t(0);
    std::thread                 discoveryThread_;
    mutable std::mutex          peersMutex_;
    std::vector<DiscoveredPeer> peers_;

    // Relay registration & built-in server
    RelayServer                 localRelay_;
    std::thread                 relayRegThread_;
    uintptr_t                   relayControlSock_ = ~uintptr_t(0);

    // Host server & session
    uintptr_t                   hostListenSock_ = ~uintptr_t(0);
    std::thread                 hostAcceptThread_;
    mutable std::mutex          hostSessionThreadMutex_;
    std::thread                 hostSessionThread_;
    std::atomic<uintptr_t>      activeHostClientSock_{~uintptr_t(0)};
    std::mutex                  hostSendMutex_;
    mutable std::mutex          hostCipherMutex_;
    AesGcmSessionCipher         hostCipher_;
    std::array<uint8_t, 32>     hostSessionKey_{};
    uint64_t                    hostSendSeq_ = 0;
    std::atomic<bool>           hostEncrypted_{false};

    struct BruteForceRecord {
        int      failedCount = 0;
        uint64_t windowStartMs = 0;
        uint64_t lockedUntilMs = 0;
    };
    mutable std::mutex                              authRateMutex_;
    std::unordered_map<std::string, BruteForceRecord> authRateMap_;

    mutable std::mutex          approvalMutex_;
    std::condition_variable     approvalCv_;
    PendingIncomingRequest      pendingReq_;
    bool                        approvalDecided_ = false;
    bool                        approvalAccepted_ = false;
    uint8_t                     approvalPermissions_ = PERM_ALL;
    std::atomic<bool>           autoAcceptIncoming_{false};
    std::atomic<uint8_t>        autoAcceptPerms_{PERM_ALL};

    mutable std::mutex          hostStatusMutex_;
    HostSessionStatus           hostStatus_;
    std::atomic<uint8_t>        hostLivePermissions_{PERM_ALL};

    // Viewer session
    mutable std::mutex          viewerThreadMutex_;
    std::thread                 viewerThread_;
    std::atomic<bool>           viewerActive_{false};
    std::atomic<uintptr_t>      viewerSock_{~uintptr_t(0)};
    std::mutex                  viewerSendMutex_;
    mutable std::mutex          viewerCipherMutex_;
    AesGcmSessionCipher         viewerCipher_;
    std::array<uint8_t, 32>     viewerSessionKey_{};
    uint64_t                    viewerSendSeq_ = 0;
    std::atomic<bool>           viewerEncrypted_{false};

    mutable std::mutex          viewerStatsMutex_;
    ViewerSessionStats          viewerStats_;

    // Viewer 2FA synchronization
    mutable std::mutex          viewerTotpMutex_;
    std::condition_variable     viewerTotpCv_;
    std::string                 viewerTotpCode_;
    bool                        viewerTotpSubmitted_ = false;
    bool                        viewerTotpCancelled_ = false;

    mutable std::mutex          viewerFrameMutex_;
    std::vector<uint8_t>        viewerCanvasBgra_;
    int                         viewerCanvasW_ = 0;
    int                         viewerCanvasH_ = 0;
    uint64_t                    viewerFrameSeq_ = 0;
    CursorState                 viewerCursor_;
    mutable RECT                viewerDirtyBounds_{0, 0, 0, 0};
    mutable bool                viewerFullCanvasDirty_ = true;
    mutable std::mutex          onFrameDecodedMutex_;
    std::function<void()>       onFrameDecoded_;

    // Shared FileTransfer, Clipboard & Chat state
    FileTransferManager             fileManager_;
    ClipboardManager                clipboardManager_;
    ClipboardFileTransferManager    clipFileMgr_;
    std::atomic<bool>               clipboardSyncEnabled_{true};
    mutable std::mutex              chatMutex_;
    std::vector<ChatMessageEntry>   chatHistory_;
    std::atomic<uint32_t>           unreadChatCount_{0};

    // Host Audio Capture (WASAPI Loopback)
    void startHostAudioCapture();
    void stopHostAudioCapture();
    void hostAudioCaptureLoop();

    // Viewer Audio Playback (Win32 waveOut)
    void initViewerAudioPlayback();
    void shutdownViewerAudioPlayback();
    void enqueueViewerAudioChunk(const uint8_t* payload, size_t len);

    // Host Privacy Curtain Window
    void createPrivacyCurtainWindow();
    void destroyPrivacyCurtainWindow();

    // Audio Capture & Playback state
    std::thread             hostAudioThread_;
    std::atomic<bool>       hostAudioActive_{false};
    HWAVEOUT                hWaveOut_ = nullptr;
    WAVEHDR                 waveHeaders_[3]{};
    std::vector<uint8_t>    waveBuffers_[3];
    size_t                  currentWaveIdx_ = 0;
    mutable std::mutex      audioPlaybackMutex_;
    std::atomic<int>        audioVolumePercent_{100};
    std::atomic<bool>       audioMuted_{false};
    std::atomic<bool>       hostAudioSuspended_{false};

    // Privacy Mode state
    std::atomic<bool>       hostPrivacyModeActive_{false};
    HWND                    hwndPrivacyCurtain_ = nullptr;
    std::atomic<bool>       viewerPrivacyModeActive_{false};
    std::string             privacyCurtainNotice_;
    std::string             privacyCurtainBrand_;
    bool                    privacyCurtainShowId_ = true;

    // Host Terminal Process & Anonymous Pipes
    void startHostTerminal(bool usePowerShell = false);
    void stopHostTerminal();
    void hostTerminalReaderLoop();
    void injectHostTerminalStdin(const std::string& input);

    HANDLE              hChildStdinWrite_ = nullptr;
    HANDLE              hChildStdoutRead_ = nullptr;
    HANDLE              hChildProcess_ = nullptr;
    HANDLE              hChildThread_ = nullptr;
    std::thread         hostTerminalThread_;
    std::atomic<bool>   hostTerminalActive_{false};

    // Viewer Terminal State
    mutable std::mutex       terminalMutex_;
    std::vector<std::string> terminalLines_;
    std::vector<std::string> terminalHistory_;

    // TCP Tunneling State
    struct ActiveTunnel {
        uint32_t tunnelId = 0;
        uint32_t ruleId = 0;
        socket_t sock = INVALID_SOCKET_VAL;
        uint16_t targetPort = 0;
    };

    struct ListenerState {
        uint32_t ruleId = 0;
        uint16_t localPort = 0;
        uint16_t targetPort = 0;
        socket_t listenSock = INVALID_SOCKET_VAL;
    };

    void viewerTunnelMultiplexerLoop();
    void hostTunnelProxyLoop();
    void startViewerTunnelMultiplexer();
    void stopViewerTunnelMultiplexer();
    void startHostTunnelProxy();
    void stopHostTunnelProxy();

    mutable std::mutex                         tunnelMutex_;
    std::vector<PortForwardRule>               tunnelRules_;
    uint32_t                                   nextRuleId_ = 1;
    uint32_t                                   nextTunnelId_ = 1;
    std::vector<ListenerState>                 tunnelListeners_;
    std::unordered_map<uint32_t, ActiveTunnel> viewerTunnels_;
    std::unordered_map<uint32_t, ActiveTunnel> hostTunnels_;
    std::thread                                viewerTunnelThread_;
    std::atomic<bool>                          viewerTunnelActive_{false};
    std::thread                                hostTunnelThread_;
    std::atomic<bool>                          hostTunnelActive_{false};

    // Whiteboard Manager (v2.1.0)
    WhiteboardManager                          whiteboardMgr_;

    // Diagnostics Telemetry State (Feature 1)
    std::atomic<bool>                          viewerDiagnosticsActive_{false};
    std::atomic<bool>                          hostDiagnosticsStreamActive_{false};
    mutable std::mutex                         diagnosticsMutex_;
    SystemDiagnosticsPayload                   latestDiagnostics_;

    // Voice Intercom State (Feature 4)
    VoiceIntercom                              voiceIntercom_;

    // Virtual Display & Dynamic Resolution Manager (Feature 5)
    DisplayResolutionManager                   displayManager_;

    // Relay & STUN diagnostic worker (v3.2.0)
    mutable std::mutex                         netDiagMutex_;
    std::thread                                netDiagThread_;
    NetworkDiagnosticResult                    netDiagResult_;

    // Remote Reboot with Auto-Reconnect (v3.2.0)
    std::atomic<bool>                          rebootPending_{false};
    std::atomic<uint32_t>                      rebootCountdown_{0};
    std::atomic<bool>                          autoReconnectingWithToken_{false};
    mutable std::mutex                         rebootTokenMutex_;
    std::string                                rebootResumeToken_;
    uint64_t                                   rebootResumeDeskId_ = 0;
    std::string                                rebootResumeTargetInput_;

    // Remote File Synchronization & Folder Mirroring
    FileSyncManager                            fileSyncMgr_;

    // Peer Version Tracking & Compatibility
    mutable std::mutex                         peerVersionMutex_;
    std::string                                remotePeerAppVersion_;
    std::atomic<bool>                          localIsOutdated_{false};
    std::atomic<bool>                          remoteIsOutdated_{false};
    std::function<void(const VersionUpgradeNoticePayload&)> onVersionUpgradeNotice_;
    std::function<void(const std::string& hostVer, const std::string& viewerVer)> onVersionMismatch_;

    // Real-Time Collaborative Dual-Peer Scratchpad
    ScratchpadManager                                       scratchpadMgr_;
    mutable std::mutex                                      scratchpadCbMutex_;
    std::function<void(const ScratchpadUpdatePayload&)>     onScratchpadUpdate_;

    // Spotlight Omnibar Command Palette
    CommandPaletteManager                                   commandPaletteMgr_;
    mutable std::mutex                                      commandPaletteCbMutex_;
    std::function<void(const CommandPaletteOutputPayload&)> onCommandPaletteOutput_;
    std::atomic<uint32_t>                                   nextExecId_{1};
};

} // namespace cppdesk
