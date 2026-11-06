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

namespace cppdesk {

#pragma pack(push, 1)
struct MainAVIHeader {
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

struct AVIStreamHeader {
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
#pragma pack(pop)

SessionRecorder::SessionRecorder() = default;

SessionRecorder::~SessionRecorder() {
    stopRecording();
}

bool SessionRecorder::startRecording(const std::string& outputPath, int width, int height, int fps) {
    stopRecording();

    if (width <= 0 || height <= 0) return false;
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

    filePath_ = outputPath;
    width_ = width;
    height_ = height;
    fps_ = fps;
    frameCount_.store(0);
    bytesWritten_.store(0);
    startTick_ = getNowTickMs();
    indexEntries_.clear();

    writeAviHeaders(width, height, fps);

    workerStopping_.store(false);
    recording_.store(true);
    workerThread_ = std::thread(&SessionRecorder::workerLoop, this);
    return true;
}

void SessionRecorder::pushFrame(const uint8_t* bgra, int width, int height) {
    if (!recording_.load() || !bgra) return;
    if (width != width_ || height != height_) return;

    size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::vector<uint8_t> frameBuf(bgra, bgra + frameBytes);

    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (frameQueue_.size() >= 60) {
            queueCv_.wait_for(lock, std::chrono::milliseconds(20), [&]() {
                return frameQueue_.size() < 60 || !recording_.load();
            });
        }
        if (frameQueue_.size() >= 60) {
            droppedFrames_++;
            return;
        }
        frameQueue_.push(std::move(frameBuf));
    }
    queueCv_.notify_one();
}

void SessionRecorder::stopRecording() {
    if (!recording_.exchange(false)) {
        return;
    }

    workerStopping_.store(true);
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
    return filePath_;
}

void SessionRecorder::writeAviHeaders(int width, int height, int fps) {
    uint32_t zero = 0;

    // 1. RIFF Header
    file_.write("RIFF", 4);
    file_.write(reinterpret_cast<const char*>(&zero), 4); // Placeholder for riffSize
    file_.write("AVI ", 4);

    // 2. LIST 'hdrl'
    uint32_t hdrlSize = 4 + (8 + sizeof(MainAVIHeader)) + (12 + (8 + sizeof(AVIStreamHeader)) + (8 + sizeof(AviBmpInfoHeader)));
    file_.write("LIST", 4);
    file_.write(reinterpret_cast<const char*>(&hdrlSize), 4);
    file_.write("hdrl", 4);

    // 'avih' chunk
    MainAVIHeader avih{};
    avih.dwMicroSecPerFrame = 1000000 / fps;
    avih.dwMaxBytesPerSec = static_cast<uint32_t>(width * height * 4 * fps);
    avih.dwFlags = 0x810; // AVIF_HASINDEX | AVIF_TRUSTCKTYPE
    avih.dwTotalFrames = 0;
    avih.dwStreams = 1;
    avih.dwSuggestedBufferSize = static_cast<uint32_t>(width * height * 4);
    avih.dwWidth = static_cast<uint32_t>(width);
    avih.dwHeight = static_cast<uint32_t>(height);

    file_.write("avih", 4);
    uint32_t avihSize = sizeof(MainAVIHeader);
    file_.write(reinterpret_cast<const char*>(&avihSize), 4);
    file_.write(reinterpret_cast<const char*>(&avih), sizeof(avih));

    // LIST 'strl'
    uint32_t strlSize = 4 + (8 + sizeof(AVIStreamHeader)) + (8 + sizeof(AviBmpInfoHeader));
    file_.write("LIST", 4);
    file_.write(reinterpret_cast<const char*>(&strlSize), 4);
    file_.write("strl", 4);

    // 'strh' chunk
    AVIStreamHeader strh{};
    strh.fccType = 0x73646976; // 'vids'
    strh.fccHandler = 0x20424944; // 'DIB '
    strh.dwScale = 1;
    strh.dwRate = static_cast<uint32_t>(fps);
    strh.dwLength = 0;
    strh.dwSuggestedBufferSize = static_cast<uint32_t>(width * height * 4);
    strh.dwQuality = 10000;
    strh.dwSampleSize = static_cast<uint32_t>(width * height * 4);
    strh.rcFrame.left = 0;
    strh.rcFrame.top = 0;
    strh.rcFrame.right = static_cast<int16_t>(width);
    strh.rcFrame.bottom = static_cast<int16_t>(height);

    file_.write("strh", 4);
    uint32_t strhSize = sizeof(AVIStreamHeader);
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
    bih.biSizeImage = static_cast<uint32_t>(width * height * 4);

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

    // 1. Write idx1 chunk
    uint32_t idxStart = static_cast<uint32_t>(file_.tellp());
    file_.write("idx1", 4);
    uint32_t idxSize = static_cast<uint32_t>(indexEntries_.size() * sizeof(AviIndexEntry));
    file_.write(reinterpret_cast<const char*>(&idxSize), 4);
    if (!indexEntries_.empty()) {
        file_.write(reinterpret_cast<const char*>(indexEntries_.data()), idxSize);
    }

    uint32_t totalFileSize = static_cast<uint32_t>(file_.tellp());
    uint32_t riffSize = totalFileSize - 8;
    uint32_t moviSize = (idxStart - moviDataOffset_) + 4;
    uint32_t totalFrames = static_cast<uint32_t>(frameCount_.load());

    // 2. Patch RIFF size (offset 4)
    file_.seekp(4);
    file_.write(reinterpret_cast<const char*>(&riffSize), 4);

    // 3. Patch MainAVIHeader dwTotalFrames (offset 48)
    file_.seekp(48);
    file_.write(reinterpret_cast<const char*>(&totalFrames), 4);

    // 4. Patch AVIStreamHeader dwLength (offset 140)
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
            std::streampos pos = file_.tellp();
            if (pos == std::streampos(-1)) {
                break;
            }
            uint32_t chunkStart = static_cast<uint32_t>(pos);
            uint32_t relOffset = chunkStart - moviDataOffset_;

            file_.write("00dc", 4);
            uint32_t frameBytes = static_cast<uint32_t>(width_ * height_ * 4);
            file_.write(reinterpret_cast<const char*>(&frameBytes), 4);

            // Invert vertically for bottom-up DIB in memory and write in a single call
            if (flippedBuffer_.size() < frameBytes) {
                flippedBuffer_.resize(frameBytes);
            }
            const uint8_t* p = frame.data();
            uint8_t* dst = flippedBuffer_.data();
            size_t rowBytes = static_cast<size_t>(width_ * 4);
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
