#pragma once

#include "../core/protocol.hpp"
#include "../core/crypto_identity.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <mutex>
#include <functional>
#include <memory>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <deque>

namespace cppdesk {

#ifdef DeleteFile
#undef DeleteFile
#endif

enum class SyncMode : uint8_t {
    PushMirror = 0, // Local -> Remote
    PullMirror = 1, // Remote -> Local
    TwoWay     = 2  // Bidirectional reconcile (newest wins)
};

enum class SyncState : uint8_t {
    Idle         = 0,
    Scanning     = 1,
    DiffReady    = 2,
    Comparing    = 2,
    Syncing      = 3,
    Transferring = 3,
    Completed    = 4,
    Failed       = 5,
    Cancelled    = 6
};

enum class SyncDiffStatus : uint8_t {
    Match          = 0,
    NewLocal       = 1,
    NewRemote      = 2,
    ModifiedLocal  = 3,
    ModifiedRemote = 4,
    DeleteTarget   = 5
};

struct SyncDiffItem {
    std::string    relativePath;
    SyncDiffStatus status = SyncDiffStatus::Match;
    SyncActionType action = SyncActionType::CreateDir;
    uint64_t       localSize = 0;
    uint64_t       remoteSize = 0;
    uint64_t       sizeBytes = 0;
    uint64_t       localMtime = 0;
    uint64_t       remoteMtime = 0;
    bool           isDirectory = false;
};

struct SyncStatus {
    SyncState   state = SyncState::Idle;
    uint32_t    filesCompleted = 0;
    uint32_t    totalFiles = 0;
    uint64_t    bytesTransferred = 0;
    uint64_t    totalBytes = 0;
    double      bytesPerSec = 0.0;
    std::string currentFileName;
    std::string errorMessage;
};

struct SyncMetrics {
    uint32_t totalFiles = 0;
    uint32_t newCount = 0;
    uint32_t modCount = 0;
    uint32_t matchCount = 0;
    uint32_t delCount = 0;
    uint64_t bytesToTransfer = 0;
};

struct SyncTelemetry {
    SyncState   state = SyncState::Idle;
    uint64_t    transferredBytes = 0;
    uint64_t    totalBytes = 0;
    double      speedBps = 0.0;
    double      etaSeconds = 0.0;
    uint32_t    currentFileIndex = 0;
    uint32_t    totalFiles = 0;
    std::string currentFileName;
    std::string errorMessage;
};

class FileSyncManager {
public:
    static constexpr size_t SYNC_BLOCK_SIZE = 65536; // 64 KB

    using SendPacketFn = std::function<bool(PacketType type, const std::vector<uint8_t>& payload)>;

    FileSyncManager();
    ~FileSyncManager();

    // Static Helpers
    static std::string normalizePath(const std::string& p);
    static bool scanLocalDirectory(const std::string& rootPath, std::vector<SyncFileEntry>& outEntries);
    static std::vector<SyncFileEntry> scanLocalDirectory(const std::string& rootPath) {
        std::vector<SyncFileEntry> entries;
        scanLocalDirectory(rootPath, entries);
        return entries;
    }
    static bool computeBlockHashes(const std::string& fullPath, uint64_t fileSize, std::vector<std::array<uint8_t, 32>>& outHashes);
    static std::vector<std::array<uint8_t, 32>> calculateBlockHashes(const std::string& fullPath, size_t blockSize = SYNC_BLOCK_SIZE) {
        (void)blockSize;
        std::vector<std::array<uint8_t, 32>> hashes;
        std::error_code ec;
        uint64_t sz = std::filesystem::file_size(fullPath, ec);
        if (!ec) {
            computeBlockHashes(fullPath, sz, hashes);
        }
        return hashes;
    }
    static std::vector<SyncDiffItem> generateDiffPlan(
        const std::vector<SyncFileEntry>& localEntries,
        const std::vector<SyncFileEntry>& remoteEntries,
        SyncMode mode,
        bool purge = false);
    static std::string formatSpeed(double bps);
    static std::string formatEta(double etaSec);

    // Configuration & Local State
    void setLocalRoot(const std::string& path);
    std::string localRoot() const;

    void setRemoteRoot(const std::string& path);
    std::string remoteRoot() const;

    void setSyncMode(SyncMode mode);
    SyncMode syncMode() const;

    void setMirrorPurge(bool purge);
    bool mirrorPurge() const;

    SyncState syncState() const;
    void setSyncState(SyncState state);

    SyncMetrics metrics() const;
    SyncTelemetry telemetry() const;
    SyncStatus status() const {
        auto t = telemetry();
        SyncStatus s;
        s.state = t.state;
        s.filesCompleted = t.currentFileIndex;
        s.totalFiles = t.totalFiles;
        s.bytesTransferred = t.transferredBytes;
        s.totalBytes = t.totalBytes;
        s.bytesPerSec = t.speedBps;
        s.currentFileName = t.currentFileName;
        s.errorMessage = t.errorMessage;
        return s;
    }
    std::vector<SyncDiffItem> diffItems() const;
    std::vector<SyncDiffItem> currentDiffPlan() const { return diffItems(); }

    bool requestScan(const std::string& path) {
        setLocalRoot(path);
        return startLocalScan();
    }

    // Scanning & Differential Planning
    bool startLocalScan();
    void setLocalEntries(std::vector<SyncFileEntry> entries);
    const std::vector<SyncFileEntry>& localEntries() const;

    void handleScanResp(const SyncScanRespPayload& resp);
    void setRemoteEntries(std::vector<SyncFileEntry> entries);
    const std::vector<SyncFileEntry>& remoteEntries() const;

    void generateDiffPlan();

    // Sync Execution Control
    bool startSync();
    void cancelSync();
    void reset();

    // Outgoing Block & Packet Dispatcher (Non-blocking Session Pump)
    bool pumpOutgoingSync(const SendPacketFn& sendPacket, int maxBlocks = 4);

    // Incoming Packet Handlers
    bool handleHashResp(const SyncHashRespPayload& resp);
    bool handleDeltaBlock(const SyncDeltaBlockPayload& block);
    bool handleStatusUpdate(const SyncStatusUpdatePayload& status);
    bool handleActionReq(const SyncActionReqPayload& req, SyncActionRespPayload& resp);
    bool handleActionResp(const SyncActionRespPayload& resp);

    // Host-Side Handlers
    bool handleScanReqHost(const SyncScanReqPayload& req, SyncScanRespPayload& resp);
    bool handleHashReqHost(const SyncHashReqPayload& req, SyncHashRespPayload& resp);

    // Staging and Queue Helpers
    void registerIncomingFile(uint32_t fileId, const std::string& relativePath, uint64_t totalSize, uint64_t mtimeSec);
    void queueOutgoingFile(const std::string& relativePath, const std::string& sourceRoot, uint64_t totalSize, uint64_t mtimeSec, const std::vector<std::array<uint8_t, 32>>& targetHashes = {}, uint32_t fileId = 0);

    void setStagingRootOverride(const std::string& path);
    std::string stagingRootOverride() const;

private:
    struct OutgoingBlock {
        uint32_t syncId = 0;
        uint32_t fileId = 0;
        uint32_t blockIndex = 0;
        uint64_t blockOffset = 0;
        uint64_t totalFileSize = 0;
        uint32_t totalBlocks = 0;
        std::array<uint8_t, 32> blockHash{};
        std::vector<uint8_t> blockData;
    };

    struct OutgoingFileTask {
        uint32_t fileId = 0;
        std::string relativePath;
        std::string fullSourcePath;
        uint64_t totalSize = 0;
        uint64_t mtimeSec = 0;
        uint32_t totalBlocks = 0;
        std::vector<std::array<uint8_t, 32>> targetHashes;
        bool hashesRequested = false;
        bool hashesReceived = false;
        bool queuedBlocks = false;
    };

    struct IncomingStagingFile {
        uint32_t syncId = 0;
        uint32_t fileId = 0;
        std::string relativePath;
        std::string targetFullPath;
        std::string partFullPath;
        uint64_t totalSize = 0;
        uint64_t mtimeSec = 0;
        uint32_t totalBlocks = 0;
        uint32_t receivedBlocks = 0;
        std::vector<bool> blockReceived;
        bool initialized = false;
    };

    void queueBlocksForTask(OutgoingFileTask& task);
    std::string resolveTargetRoot() const;
    void cancelSyncLocked();

    mutable std::mutex mutex_;
    std::string localRoot_;
    std::string remoteRoot_;
    std::string stagingRootOverride_;
    SyncMode mode_ = SyncMode::PushMirror;
    bool mirrorPurge_ = false;
    SyncState state_ = SyncState::Idle;
    uint32_t activeSyncId_ = 0;
    uint32_t nextFileId_ = 1;

    std::vector<SyncFileEntry> localEntries_;
    std::vector<SyncFileEntry> remoteEntries_;
    std::vector<SyncDiffItem> diffItems_;
    SyncMetrics metrics_;

    // Telemetry tracking
    uint64_t transferredBytes_ = 0;
    uint64_t totalBytes_ = 0;
    uint32_t currentFileIndex_ = 0;
    uint32_t totalFiles_ = 0;
    std::string currentFileName_;
    std::string errorMessage_;
    uint64_t syncStartTickMs_ = 0;
    uint64_t lastTelemetryTickMs_ = 0;
    double lastSpeedBps_ = 0.0;
    double smoothedBps_ = 0.0;

    // Outgoing Queues
    std::deque<SyncActionReqPayload> pendingActions_;
    std::deque<OutgoingFileTask> outgoingFiles_;
    std::deque<OutgoingBlock> pendingBlocks_;

    // Incoming Staging
    std::unordered_map<uint32_t, IncomingStagingFile> incomingFiles_;
    std::unordered_map<std::string, uint32_t> pathToFileIdMap_;
};

} // namespace cppdesk
