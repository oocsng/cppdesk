#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <cstdint>

namespace cppdesk {

#pragma pack(push, 1)
struct MainAviHeader {
    uint32_t dwMicroSecPerFrame{0};
    uint32_t dwMaxBytesPerSec{0};
    uint32_t dwPaddingGranularity{0};
    uint32_t dwFlags{0};
    uint32_t dwTotalFrames{0};
    uint32_t dwInitialFrames{0};
    uint32_t dwStreams{0};
    uint32_t dwSuggestedBufferSize{0};
    uint32_t dwWidth{0};
    uint32_t dwHeight{0};
    uint32_t dwReserved[4]{0};
};

struct AviStreamHeader {
    uint32_t fccType{0};
    uint32_t fccHandler{0};
    uint32_t dwFlags{0};
    uint16_t wPriority{0};
    uint16_t wLanguage{0};
    uint32_t dwInitialFrames{0};
    uint32_t dwScale{0};
    uint32_t dwRate{0};
    uint32_t dwStart{0};
    uint32_t dwLength{0};
    uint32_t dwSuggestedBufferSize{0};
    uint32_t dwQuality{0};
    uint32_t dwSampleSize{0};
    struct {
        int16_t left{0};
        int16_t top{0};
        int16_t right{0};
        int16_t bottom{0};
    } rcFrame;
};

struct AviBmpInfoHeader {
    uint32_t biSize{sizeof(AviBmpInfoHeader)};
    int32_t  biWidth{0};
    int32_t  biHeight{0};
    uint16_t biPlanes{1};
    uint16_t biBitCount{32};
    uint32_t biCompression{0};
    uint32_t biSizeImage{0};
    int32_t  biXPelsPerMeter{0};
    int32_t  biYPelsPerMeter{0};
    uint32_t biClrUsed{0};
    uint32_t biClrImportant{0};
};

struct AviIndexEntry {
    uint32_t ckid;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
};
#pragma pack(pop)

static_assert(sizeof(MainAviHeader) == 56, "MainAviHeader must be 56 bytes");
static_assert(sizeof(AviStreamHeader) == 56, "AviStreamHeader must be 56 bytes");
static_assert(sizeof(AviBmpInfoHeader) == 40, "AviBmpInfoHeader must be 40 bytes");
static_assert(sizeof(AviIndexEntry) == 16, "AviIndexEntry must be 16 bytes");

class SessionRecorder {
public:
    SessionRecorder();
    ~SessionRecorder();

    // Start recording session to an AVI file
    bool startRecording(const std::string& outputPath, int width, int height, int fps = 30);

    // Push an uncompressed 32-bit BGRA video frame to the recording queue
    void pushFrame(const uint8_t* bgra, int width, int height);

    // Finalize recording, write AVI index/headers, and close file
    void stopRecording();

    bool isRecording() const { return recording_.load(); }
    // True once the recording ended on its own because the file reached the 4 GiB AVI limit.
    bool stoppedAtSizeLimit() const { return sizeLimitHit_.load(); }
    uint64_t recordedFrames() const { return frameCount_.load(); }
    uint64_t droppedFrames() const { return droppedFrames_.load(); }
    uint64_t recordedBytes() const { return bytesWritten_.load(); }
    uint32_t elapsedSeconds() const;
    uint32_t durationSeconds() const { return elapsedSeconds(); }
    std::string currentFilePath() const;

private:
    void workerLoop();
    void writeAviHeaders(int width, int height, int fps);
    void updateAviHeadersOnClose();

    std::atomic<bool>               recording_{false};
    std::atomic<bool>               workerStopping_{false};
    std::atomic<bool>               sizeLimitHit_{false};
    std::atomic<uint64_t>           frameCount_{0};
    std::atomic<uint64_t>           droppedFrames_{0};
    std::atomic<uint64_t>           bytesWritten_{0};
    uint64_t                        startTick_{0};

    int                             width_{0};
    int                             height_{0};
    int                             fps_{30};
    std::string                     filePath_;

    std::ofstream                   file_;
    std::vector<AviIndexEntry>      indexEntries_;
    uint32_t                        moviDataOffset_{0};
    uint32_t                        moviListSizeOffset_{0};

    std::thread                     workerThread_;
    mutable std::mutex              queueMutex_;
    std::condition_variable         queueCv_;
    std::queue<std::vector<uint8_t>> frameQueue_;
    std::vector<uint8_t>            flippedBuffer_;
};

} // namespace cppdesk
