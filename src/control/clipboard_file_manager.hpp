#pragma once

#include "../core/protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <memory>
#include <functional>
#include <algorithm>
#include "shell_clipboard.hpp"

namespace cppdesk {

enum class TransferStatus : uint8_t {
    InProgress = 0,
    Completed  = 1,
    Failed     = 2,
    Cancelled  = 3
};

struct FileTransferItem {
    uint32_t       transferId = 0;
    std::string    fileName;
    std::string    savedPath;
    uint64_t       totalBytes = 0;
    uint64_t       transferredBytes = 0;
    bool           isOutgoing = false;
    TransferStatus status = TransferStatus::InProgress;
    std::string    statusText;
    std::string    sha256Hex;
    double         speedBps = 0.0;
    double         etaSeconds = -1.0;
    double         avgSpeedBps = 0.0;
    uint64_t       startTimeMs = 0;

    float progressFraction() const {
        if (totalBytes == 0) return status == TransferStatus::Completed ? 1.0f : 0.0f;
        float frac = static_cast<float>(static_cast<double>(transferredBytes) / static_cast<double>(totalBytes));
        return std::clamp(frac, 0.0f, 1.0f);
    }

    static std::string formatSpeed(double bytesPerSec);
    static std::string formatEta(double etaSec);
};

struct ClipboardHistoryItem {
    uint32_t    id = 0;
    std::string text;
    std::string previewText;
    std::string typeBadge; // "URL", "Path", "Code", "Text"
    uint64_t    timestampMs = 0;
    bool        isFromRemote = false;
    size_t      charCount = 0;
};

class ClipboardHistoryManager {
public:
    ClipboardHistoryManager() = default;

    void addItem(const std::string& text, bool isFromRemote);
    std::vector<ClipboardHistoryItem> items() const;
    std::vector<ClipboardHistoryItem> search(const std::string& query) const;
    bool copyItemToClipboard(uint32_t id);
    void clear();
    bool deleteItem(uint32_t id);
    size_t count() const;

private:
    mutable std::mutex                  mutex_;
    std::vector<ClipboardHistoryItem>   items_;
    uint32_t                            nextId_ = 1;
};

class ClipboardManager {
public:
    ClipboardManager();

    static std::string getClipboardUtf8();
    static bool setClipboardUtf8(const std::string& utf8Text);
    static bool getClipboardImageJpeg(std::vector<uint8_t>& outJpeg, uint32_t& outW, uint32_t& outH, int maxDim = 640);

    // Returns true if user copied new text locally (ignores changes we applied from remote)
    bool pollLocalChange(std::string& outNewText);

    // Apply incoming remote clipboard text and record hash to avoid echo loop
    void applyRemoteClipboard(const std::string& utf8Text);

    ClipboardHistoryManager& history() { return history_; }
    const ClipboardHistoryManager& history() const { return history_; }

private:
    uint32_t                lastSeqNumber_ = 0;
    std::string             lastAppliedHash_;
    ClipboardHistoryManager history_;
};

class FileTransferManager {
public:
    using SendPacketFn = std::function<bool(PacketType, const std::vector<uint8_t>&)>;

    FileTransferManager();
    ~FileTransferManager();

    void setReceiveDirectory(const std::string& dirPath);
    std::string receiveDirectory() const;
    void openReceiveDirectoryInExplorer() const;

    // Queue a local file to be sent to the connected peer
    uint32_t startOutgoingFile(const std::string& filePath, const SendPacketFn& sendPacket,
                               FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads,
                               float dropNx = 0.0f, float dropNy = 0.0f);
    // For paths already decoded from UTF-8 (the remote file browser); a std::string path is read as the ANSI code page.
    uint32_t startOutgoingFile(const std::filesystem::path& path, const SendPacketFn& sendPacket,
                               FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads,
                               float dropNx = 0.0f, float dropNy = 0.0f);

    // Recursively queue a file or directory tree for transfer
    int startOutgoingPath(const std::string& path, const SendPacketFn& sendPacket,
                          FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads,
                          float dropNx = 0.0f, float dropNy = 0.0f);

    // Pump up to maxChunks outgoing chunks (called periodically by session worker)
    bool pumpOutgoingChunks(const SendPacketFn& sendPacket, int maxChunks = 4);

    // Cancel an active outgoing or incoming transfer and notify peer
    bool cancelTransfer(uint32_t transferId, const SendPacketFn& sendPacket = nullptr);

    // Abort all in-progress transfers on unexpected session disconnect and purge .part files
    void abortActiveTransfers();

    // Handle incoming file transfer packets from remote peer
    void handleFileOffer(uint32_t transferId, uint64_t totalBytes, const std::string& fileName,
                         FileOfferTarget targetHint = FileOfferTarget::DefaultDownloads,
                         float dropNx = 0.0f, float dropNy = 0.0f);
    void handleFileChunk(uint32_t transferId, uint64_t offset, const uint8_t* chunkData, size_t chunkLen);
    void handleFileComplete(uint32_t transferId, const std::string& sha256Hex);
    void handleFileCancel(uint32_t transferId);

    void clearCompleted();
    std::vector<FileTransferItem> snapshotTransfers() const;
    bool hasActiveTransfers() const;
    double aggregateActiveBandwidthBps() const;

private:
    struct IncrementalSha256;

    struct ActiveOutgoing {
        uint32_t                           transferId = 0;
        std::string                        filePath;
        std::string                        fileName;
        uint64_t                           totalBytes = 0;
        uint64_t                           offset = 0;
        uint64_t                           startTimeMs = 0;
        uint64_t                           lastSampleTimeMs = 0;
        uint64_t                           lastSampleBytes = 0;
        double                             emaSpeedBps = 0.0;
        std::ifstream                      stream;
        std::unique_ptr<IncrementalSha256> hasher;
    };

    struct ActiveIncoming {
        uint32_t                           transferId = 0;
        std::string                        fileName;
        std::string                        partPath;
        std::string                        savePath;
        uint64_t                           totalBytes = 0;
        uint64_t                           receivedBytes = 0;
        uint64_t                           startTimeMs = 0;
        uint64_t                           lastSampleTimeMs = 0;
        uint64_t                           lastSampleBytes = 0;
        double                             emaSpeedBps = 0.0;
        std::ofstream                      stream;
        std::unique_ptr<IncrementalSha256> hasher;
    };

    mutable std::mutex                             mutex_;
    std::string                                    receiveDir_;
    uint32_t                                       nextTransferId_ = 1;
    std::vector<FileTransferItem>                  items_;
    std::vector<std::unique_ptr<ActiveOutgoing>>   outgoingQueue_;
    std::vector<std::unique_ptr<ActiveIncoming>>   incomingStreams_;
};

class ClipboardFileTransferManager {
public:
    using SendPacketFn = std::function<bool(PacketType, const std::vector<uint8_t>&)>;

    ClipboardFileTransferManager();
    ~ClipboardFileTransferManager();

    // Polling local clipboard for file copy (CF_HDROP)
    bool pollLocalClipboardFiles(const SendPacketFn& sendPacket);

    // Remote peer advertised virtual clipboard files (0x37)
    void handleRemoteFileList(uint32_t transferId, const std::vector<VirtualFileEntry>& files, const SendPacketFn& sendPacket);

    // Remote peer requested a specific file chunk (0x38)
    void handleFileRequest(uint32_t transferId, uint32_t fileIndex, uint64_t offset, uint32_t length, const SendPacketFn& sendPacket);

    // Incoming file chunk delivered with SHA-256 (0x39)
    void handleFileChunk(uint32_t transferId, uint32_t fileIndex, uint64_t offset, const uint8_t* data, size_t len, const uint8_t sha256[32]);

    // Remote peer or local user cancelled transfer (0x3A)
    void handleFileCancel(uint32_t transferId, uint32_t reasonCode = 1);
    void cancelActiveTransfer(const SendPacketFn& sendPacket = nullptr);

    // Clean up staging directory on session disconnect or reset
    void reset();

    // UI Progress Pill Telemetry & Metrics
    bool isTransferActive() const;
    std::string activeFileName() const;
    float activeProgressFraction() const;
    float activeTransferRateMBs() const;
    uint64_t activeTransferredBytes() const;
    uint64_t activeTotalBytes() const;
    uint32_t activeTransferId() const;

    // Called on demand by VirtualFileStream::Read to synchronously retrieve chunk
    bool fetchChunk(uint32_t fileIndex, uint64_t offset, uint32_t length, std::vector<uint8_t>& outData);

    // Staging and conflict helpers
    std::wstring stagingDirectory() const { return stagingDir_; }
    void purgeStaging();

private:
    struct OutgoingState {
        uint32_t                      transferId = 0;
        std::vector<VirtualFileEntry> files;
        std::vector<std::wstring>     fullPaths;
        uint64_t                      totalBytes = 0;
        std::string                   manifestHash;
    };

    struct IncomingState {
        uint32_t                      transferId = 0;
        std::vector<VirtualFileEntry> files;
        uint64_t                      totalBytes = 0;
        uint64_t                      transferredBytes = 0;
        uint32_t                      currentFileIndex = 0;
        std::string                   currentFileName;
        bool                          inProgress = false;
        bool                          cancelled = false;
        uint64_t                      startTimeMs = 0;
        float                         transferRateMBs = 0.0f;
    };

    struct PendingChunkRequest {
        uint32_t             transferId = 0;
        uint32_t             fileIndex = 0;
        uint64_t             offset = 0;
        uint32_t             length = 0;
        bool                 completed = false;
        bool                 failed = false;
        std::vector<uint8_t> data;
    };

    mutable std::mutex       mutex_;
    std::condition_variable  chunkCv_;
    uint32_t                 lastSequenceNumber_ = 0;
    uint32_t                 nextTransferId_ = 1001;
    std::wstring             stagingDir_;
    SendPacketFn             activeSendFn_;

    OutgoingState            outgoing_;
    IncomingState            incoming_;
    PendingChunkRequest      pendingReq_;
};

} // namespace cppdesk
