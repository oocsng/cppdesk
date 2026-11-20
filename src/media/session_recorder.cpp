#include "session_recorder.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <chrono>

namespace {
static inline uint64_t getNowTickMs() {
#if defined(_WIN32)
    return GetTickCount64();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
} // namespace
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <limits>

namespace cppdesk {

namespace {
constexpr uint64_t kMaxAviFileBytes = 0xFFFFFFF0ULL;
constexpr uint64_t kMaxRecorderFrameBytes = 256ULL * 1024ULL * 1024ULL;
} // namespace

SessionRecorder::SessionRecorder() = default;

SessionRecorder::~SessionRecorder() {
    stopRecording();
}

bool SessionRecorder::startRecording(const std::string& outputPath, int width, int height, int fps) {
    stopRecording();

    if (width <= 0 || height <= 0) return false;
    uint64_t frameBytes64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
    if (frameBytes64 == 0 || frameBytes64 > kMaxRecorderFrameBytes) return false;
    if (fps <= 0) fps = 30;

    std::filesystem::path p(outputPath);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    file_.open(outputPath, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        while (!frameQueue_.empty()) frameQueue_.pop();
        filePath_ = outputPath;
        width_ = width;
        height_ = height;
        fps_ = fps;
        workerStopping_.store(false);
    }

    sizeLimitHit_.store(false);
    frameCount_.store(0);
    droppedFrames_.store(0);
    bytesWritten_.store(0);
    startTick_ = getNowTickMs();
    indexEntries_.clear();

    writeAviHeaders(width, height, fps);

    recording_.store(true);
    workerThread_ = std::thread(&SessionRecorder::workerLoop, this);
    return true;
}

void SessionRecorder::pushFrame(const uint8_t* bgra, int width, int height) {
    if (!recording_.load() || !bgra || width <= 0 || height <= 0) return;

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (!recording_.load() || workerStopping_.load() || width != width_ || height != height_) return;
    }

    size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::vector<uint8_t> frameBuf(bgra, bgra + frameBytes);

    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (!recording_.load() || workerStopping_.load() || width != width_ || height != height_) return;
        if (frameQueue_.size() >= 60) {
            queueCv_.wait_for(lock, std::chrono::milliseconds(20), [&]() {
                return frameQueue_.size() < 60 || !recording_.load() || workerStopping_.load();
            });
        }
        if (!recording_.load() || workerStopping_.load() || width != width_ || height != height_) return;
        if (frameQueue_.size() >= 60) {
            droppedFrames_++;
            return;
        }
        frameQueue_.push(std::move(frameBuf));
    }
    queueCv_.notify_one();
}

void SessionRecorder::stopRecording() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        recording_.store(false);
        workerStopping_.store(true);
    }
    queueCv_.notify_all();
    try {
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    } catch (...) {}

    if (file_.is_open()) {
        updateAviHeadersOnClose();
    }
}

uint32_t SessionRecorder::elapsedSeconds() const {
    if (!recording_.load() || startTick_ == 0) return 0;
    return static_cast<uint32_t>((getNowTickMs() - startTick_) / 1000);
}

std::string SessionRecorder::currentFilePath() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return filePath_;
}

void SessionRecorder::writeAviHeaders(int width, int height, int fps) {
    uint32_t zero = 0;
    const uint64_t frameBytes64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
    const uint64_t maxBps64 = frameBytes64 * static_cast<uint64_t>(fps);
    const uint32_t frameBytes32 = static_cast<uint32_t>((std::min<uint64_t>)(frameBytes64, (std::numeric_limits<uint32_t>::max)()));
    const uint32_t maxBps32 = static_cast<uint32_t>((std::min<uint64_t>)(maxBps64, (std::numeric_limits<uint32_t>::max)()));

    // 1. RIFF Header
    file_.write("RIFF", 4);
    file_.write(reinterpret_cast<const char*>(&zero), 4); // Placeholder for riffSize
    file_.write("AVI ", 4);

    // 2. LIST 'hdrl'
    uint32_t hdrlSize = 4 + (8 + sizeof(MainAviHeader)) + (12 + (8 + sizeof(AviStreamHeader)) + (8 + sizeof(AviBmpInfoHeader)));
    file_.write("LIST", 4);
    file_.write(reinterpret_cast<const char*>(&hdrlSize), 4);
    file_.write("hdrl", 4);

    // 'avih' chunk
    MainAviHeader avih{};
    avih.dwMicroSecPerFrame = 1000000 / fps;
    avih.dwMaxBytesPerSec = maxBps32;
    avih.dwFlags = 0x810; // AVIF_HASINDEX | AVIF_TRUSTCKTYPE
    avih.dwTotalFrames = 0;
    avih.dwStreams = 1;
    avih.dwSuggestedBufferSize = frameBytes32;
    avih.dwWidth = static_cast<uint32_t>(width);
    avih.dwHeight = static_cast<uint32_t>(height);

    file_.write("avih", 4);
    uint32_t avihSize = sizeof(MainAviHeader);
    file_.write(reinterpret_cast<const char*>(&avihSize), 4);
    file_.write(reinterpret_cast<const char*>(&avih), sizeof(avih));

    // LIST 'strl'
    uint32_t strlSize = 4 + (8 + sizeof(AviStreamHeader)) + (8 + sizeof(AviBmpInfoHeader));
    file_.write("LIST", 4);
    file_.write(reinterpret_cast<const char*>(&strlSize), 4);
    file_.write("strl", 4);

    // 'strh' chunk
    AviStreamHeader strh{};
    strh.fccType = 0x73646976; // 'vids'
    strh.fccHandler = 0x20424944; // 'DIB '
    strh.dwScale = 1;
    strh.dwRate = static_cast<uint32_t>(fps);
    strh.dwLength = 0;
    strh.dwSuggestedBufferSize = frameBytes32;
    strh.dwQuality = 10000;
    strh.dwSampleSize = frameBytes32;
    strh.rcFrame.left = 0;
    strh.rcFrame.top = 0;
    strh.rcFrame.right = static_cast<int16_t>(width);
    strh.rcFrame.bottom = static_cast<int16_t>(height);

    file_.write("strh", 4);
    uint32_t strhSize = sizeof(AviStreamHeader);
    file_.write(reinterpret_cast<const char*>(&strhSize), 4);
    file_.write(reinterpret_cast<const char*>(&strh), sizeof(strh));

    // 'strf' chunk
    AviBmpInfoHeader bih{};
    bih.biSize = sizeof(AviBmpInfoHeader);
    bih.biWidth = width;
    bih.biHeight = height; // Positive = bottom-up DIB (universal standard in AVI)
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = 0; // BI_RGB
    bih.biSizeImage = frameBytes32;

    file_.write("strf", 4);
    uint32_t strfSize = sizeof(AviBmpInfoHeader);
    file_.write(reinterpret_cast<const char*>(&strfSize), 4);
    file_.write(reinterpret_cast<const char*>(&bih), sizeof(bih));

    // 3. LIST 'movi'
    file_.write("LIST", 4);
    moviListSizeOffset_ = static_cast<uint32_t>(file_.tellp());
    file_.write(reinterpret_cast<const char*>(&zero), 4); // Placeholder for movi size
    file_.write("movi", 4);
    moviDataOffset_ = static_cast<uint32_t>(file_.tellp());
}

void SessionRecorder::updateAviHeadersOnClose() {
    if (!file_.is_open()) return;

    std::streampos idxPos = file_.tellp();
    if (idxPos < 0 || static_cast<uint64_t>(idxPos) < moviDataOffset_ ||
        static_cast<uint64_t>(idxPos) > kMaxAviFileBytes) {
        file_.close();
        return;
    }

    // 1. Write idx1 chunk
    uint32_t idxStart = static_cast<uint32_t>(idxPos);
    uint64_t idxSize64 = static_cast<uint64_t>(indexEntries_.size()) * sizeof(AviIndexEntry);
    if (idxSize64 > kMaxAviFileBytes - idxStart - 8ULL) {
        file_.close();
        return;
    }
    file_.write("idx1", 4);
    uint32_t idxSize = static_cast<uint32_t>(idxSize64);
    file_.write(reinterpret_cast<const char*>(&idxSize), 4);
    if (!indexEntries_.empty()) {
        file_.write(reinterpret_cast<const char*>(indexEntries_.data()), idxSize);
    }

    std::streampos endPos = file_.tellp();
    if (endPos < 8 || static_cast<uint64_t>(endPos) > (std::numeric_limits<uint32_t>::max)()) {
        file_.close();
        return;
    }
    uint32_t totalFileSize = static_cast<uint32_t>(endPos);
    uint32_t riffSize = totalFileSize - 8;
    uint32_t moviSize = (idxStart - moviDataOffset_) + 4;
    uint32_t totalFrames = static_cast<uint32_t>(frameCount_.load());

    // 2. Patch RIFF size (offset 4)
    file_.seekp(4);
    file_.write(reinterpret_cast<const char*>(&riffSize), 4);

    // 3. Patch MainAviHeader dwTotalFrames (offset 48)
    file_.seekp(48);
    file_.write(reinterpret_cast<const char*>(&totalFrames), 4);

    // 4. Patch AviStreamHeader dwLength (offset 140)
    file_.seekp(140);
    file_.write(reinterpret_cast<const char*>(&totalFrames), 4);

    // 5. Patch movi LIST size
    file_.seekp(moviListSizeOffset_);
    file_.write(reinterpret_cast<const char*>(&moviSize), 4);

    file_.flush();
    file_.close();
}

void SessionRecorder::workerLoop() {
    while (true) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this]() {
                return !frameQueue_.empty() || workerStopping_.load();
            });

            if (frameQueue_.empty() && workerStopping_.load()) {
                break;
            }

            if (!frameQueue_.empty()) {
                frame = std::move(frameQueue_.front());
                frameQueue_.pop();
                queueCv_.notify_one();
            }
        }

        if (file_.is_open() && file_.good() && !frame.empty()) {
            const size_t expectedBytes = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4;
            if (frame.size() < expectedBytes || expectedBytes == 0 ||
                expectedBytes > (std::numeric_limits<uint32_t>::max)()) {
                continue;
            }

            std::streampos pos = file_.tellp();
            if (pos == std::streampos(-1) || pos < 0) {
                break;
            }
            const uint64_t pos64 = static_cast<uint64_t>(pos);
            const uint64_t projectedIdxBytes = 8ULL + (static_cast<uint64_t>(indexEntries_.size()) + 1ULL) * sizeof(AviIndexEntry);
            if (pos64 + 8ULL + expectedBytes + projectedIdxBytes > kMaxAviFileBytes) {
                // Stop cleanly before the 4 GiB RIFF/AVI 1.0 offset limit so headers stay valid.
                {
                    std::lock_guard<std::mutex> lock(queueMutex_);
                    recording_.store(false);
                    workerStopping_.store(true);
                    sizeLimitHit_.store(true);
                    while (!frameQueue_.empty()) frameQueue_.pop();
                }
                // The UI sees recording_ go false and never calls stopRecording(), so close the file here.
                updateAviHeadersOnClose();
                break;
            }

            uint32_t chunkStart = static_cast<uint32_t>(pos64);
            uint32_t relOffset = chunkStart - moviDataOffset_;

            file_.write("00dc", 4);
            uint32_t frameBytes = static_cast<uint32_t>(expectedBytes);
            file_.write(reinterpret_cast<const char*>(&frameBytes), 4);

            // Invert vertically for bottom-up DIB in memory and write in a single call
            if (flippedBuffer_.size() < frameBytes) {
                flippedBuffer_.resize(frameBytes);
            }
            const uint8_t* p = frame.data();
            uint8_t* dst = flippedBuffer_.data();
            size_t rowBytes = static_cast<size_t>(width_) * 4;
            for (int y = 0; y < height_; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                            p + static_cast<size_t>(height_ - 1 - y) * rowBytes,
                            rowBytes);
            }
            file_.write(reinterpret_cast<const char*>(dst), frameBytes);

            AviIndexEntry entry{};
            entry.ckid = 0x63643030; // '00dc'
            entry.flags = 0x00000010; // AVIIF_KEYFRAME
            entry.offset = relOffset;
            entry.size = frameBytes;
            indexEntries_.push_back(entry);

            frameCount_++;
            bytesWritten_ += (8 + frameBytes);
        }
    }
}

} // namespace cppdesk
