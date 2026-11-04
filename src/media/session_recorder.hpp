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
struct AviIndexEntry {
    uint32_t ckid;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
};
#pragma pack(pop)

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
    std::mutex                      queueMutex_;
    std::condition_variable         queueCv_;
    std::queue<std::vector<uint8_t>> frameQueue_;
    std::vector<uint8_t>            flippedBuffer_;
};

} // namespace cppdesk
