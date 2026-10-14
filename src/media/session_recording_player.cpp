#include "session_recording_player.hpp"
#include "../capture/screen_capture.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace cppdesk {

namespace {

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<uint8_t> buffer(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return static_cast<int>(j);
        }
    }
    return -1;
}

} // namespace

SessionRecordingPlayer::SessionRecordingPlayer() {
    TileCodec::initGdiPlus();
}

SessionRecordingPlayer::~SessionRecordingPlayer() {
    close();
}

void SessionRecordingPlayer::close() {
    stopPlaybackThread();

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (fileStream_.is_open()) {
            fileStream_.close();
        }
        isOpen_.store(false);
        frames_.clear();
        totalFrames_ = 0;
        frameWidth_ = 0;
        frameHeight_ = 0;
        fps_ = 30;
        durationUs_ = 0;
        currentFrameIndex_.store(0);
        hasNewFrame_.store(false);
        currentFrameBgra_.clear();
        filePath_.clear();
        moviListHeaderOffset_ = 0;
        moviDataOffset_ = 0;
        moviDataSize_ = 0;
    }
}

bool SessionRecordingPlayer::open(const std::string& filePath) {
    close();

    fileStream_.open(filePath, std::ios::binary);
    if (!fileStream_.is_open()) {
        return false;
    }

    // 1. Validate RIFF Header
    uint32_t riffTag = 0;
    uint32_t riffSize = 0;
    uint32_t aviTag = 0;

    fileStream_.read(reinterpret_cast<char*>(&riffTag), 4);
    fileStream_.read(reinterpret_cast<char*>(&riffSize), 4);
    fileStream_.read(reinterpret_cast<char*>(&aviTag), 4);

    if (riffTag != FOURCC_RIFF || aviTag != FOURCC_AVI) {
        fileStream_.close();
        return false;
    }

    fileStream_.seekg(0, std::ios::end);
    auto endPos = fileStream_.tellg();
    if (endPos < 0) {
        fileStream_.close();
        return false;
    }
    uint64_t fileLen = static_cast<uint64_t>(endPos);
    fileStream_.seekg(12);

    std::vector<AviIndexEntry> rawIndexEntries;

    // 2. Traverse top-level RIFF chunks
    while (fileStream_.good()) {
        auto curTell = fileStream_.tellg();
        if (curTell < 0 || curTell >= static_cast<std::streamoff>(fileLen)) break;
        uint64_t chunkPos = static_cast<uint64_t>(curTell);
        uint32_t ckid = 0;
        uint32_t ckSize = 0;
        fileStream_.read(reinterpret_cast<char*>(&ckid), 4);
        fileStream_.read(reinterpret_cast<char*>(&ckSize), 4);
        if (!fileStream_.good()) break;

        uint64_t chunkDataPos = fileStream_.tellg();
        uint64_t nextChunkPos = chunkDataPos + ((ckSize + 1ULL) & ~1ULL);

        if (ckid == FOURCC_LIST) {
            uint32_t listType = 0;
            fileStream_.read(reinterpret_cast<char*>(&listType), 4);
            uint64_t listEnd = chunkDataPos + ckSize;

            if (listType == FOURCC_hdrl) {
                while (fileStream_.tellg() < static_cast<std::streamoff>(listEnd) && fileStream_.good()) {
                    uint32_t subId = 0;
                    uint32_t subSize = 0;
                    fileStream_.read(reinterpret_cast<char*>(&subId), 4);
                    fileStream_.read(reinterpret_cast<char*>(&subSize), 4);
                    if (!fileStream_.good()) break;

                    uint64_t subDataPos = fileStream_.tellg();
                    uint64_t subNext = subDataPos + ((subSize + 1ULL) & ~1ULL);

                    if (subId == FOURCC_avih) {
                        fileStream_.read(reinterpret_cast<char*>(&mainHeader_),
                                         std::min<size_t>(subSize, sizeof(MainAviHeader)));
                    } else if (subId == FOURCC_LIST) {
                        uint32_t subListType = 0;
                        fileStream_.read(reinterpret_cast<char*>(&subListType), 4);
                        uint64_t subListEnd = subDataPos + subSize;

                        if (subListType == FOURCC_strl) {
                            while (fileStream_.tellg() < static_cast<std::streamoff>(subListEnd) && fileStream_.good()) {
                                uint32_t sId = 0;
                                uint32_t sSize = 0;
                                fileStream_.read(reinterpret_cast<char*>(&sId), 4);
                                fileStream_.read(reinterpret_cast<char*>(&sSize), 4);
                                if (!fileStream_.good()) break;

                                uint64_t sDataPos = fileStream_.tellg();
                                uint64_t sNext = sDataPos + ((sSize + 1ULL) & ~1ULL);

                                if (sId == FOURCC_strh) {
                                    fileStream_.read(reinterpret_cast<char*>(&streamHeader_),
                                                     std::min<size_t>(sSize, sizeof(AviStreamHeader)));
                                } else if (sId == FOURCC_strf) {
                                    fileStream_.read(reinterpret_cast<char*>(&bmpInfoHeader_),
                                                     std::min<size_t>(sSize, sizeof(AviBmpInfoHeader)));
                                }

                                fileStream_.seekg(sNext);
                            }
                        }
                    }

                    fileStream_.seekg(subNext);
                }
            } else if (listType == FOURCC_movi) {
                moviListHeaderOffset_ = chunkPos;
                moviDataOffset_ = chunkDataPos + 4; // Right after 'movi' FourCC
                moviDataSize_ = ckSize >= 4 ? (ckSize - 4) : 0;
            }
        } else if (ckid == FOURCC_idx1) {
            size_t entryCount = ckSize / sizeof(AviIndexEntry);
            rawIndexEntries.resize(entryCount);
            if (entryCount > 0) {
                fileStream_.read(reinterpret_cast<char*>(rawIndexEntries.data()),
                                 entryCount * sizeof(AviIndexEntry));
            }
        }

        fileStream_.seekg(nextChunkPos);
    }

    // 3. Extract dimensions and frame rate
    if (bmpInfoHeader_.biWidth > 0) {
        frameWidth_ = bmpInfoHeader_.biWidth;
    } else if (mainHeader_.dwWidth > 0) {
        frameWidth_ = static_cast<int>(mainHeader_.dwWidth);
    } else {
        frameWidth_ = 0;
    }

    if (bmpInfoHeader_.biHeight != 0) {
        frameHeight_ = std::abs(bmpInfoHeader_.biHeight);
    } else if (mainHeader_.dwHeight > 0) {
        frameHeight_ = static_cast<int>(mainHeader_.dwHeight);
    } else {
        frameHeight_ = 0;
    }

    if (streamHeader_.dwScale > 0 && streamHeader_.dwRate > 0) {
        fps_ = static_cast<int>(streamHeader_.dwRate / streamHeader_.dwScale);
    } else if (mainHeader_.dwMicroSecPerFrame > 0) {
        fps_ = static_cast<int>(1000000ULL / mainHeader_.dwMicroSecPerFrame);
    } else {
        fps_ = 30;
    }
    if (fps_ <= 0) fps_ = 30;

    uint64_t frameDurationUs = (mainHeader_.dwMicroSecPerFrame > 0)
        ? mainHeader_.dwMicroSecPerFrame
        : (1000000ULL / fps_);

    // 4. Index Offset Disambiguation
    if (!rawIndexEntries.empty()) {
        enum class OffsetMode { RelativeToMoviData, Absolute, RelativeToMoviList };
        OffsetMode detectedMode = OffsetMode::RelativeToMoviData;

        for (const auto& entry : rawIndexEntries) {
            if (entry.ckid == FOURCC_00dc || entry.ckid == FOURCC_00db) {
                uint64_t candA = moviDataOffset_ + entry.offset;
                uint64_t candB = static_cast<uint64_t>(entry.offset);
                uint64_t candC = moviListHeaderOffset_ + entry.offset;

                auto testCandidate = [this, &entry](uint64_t pos) -> bool {
                    auto curG = fileStream_.tellg();
                    if (curG < 0 || pos + 4 > static_cast<uint64_t>(curG)) {
                        fileStream_.clear();
                    }
                    fileStream_.seekg(pos);
                    uint32_t tag = 0;
                    fileStream_.read(reinterpret_cast<char*>(&tag), 4);
                    return (tag == entry.ckid);
                };

                if (moviDataOffset_ > 0 && testCandidate(candA)) {
                    detectedMode = OffsetMode::RelativeToMoviData;
                    break;
                } else if (testCandidate(candB)) {
                    detectedMode = OffsetMode::Absolute;
                    break;
                } else if (moviListHeaderOffset_ > 0 && testCandidate(candC)) {
                    detectedMode = OffsetMode::RelativeToMoviList;
                    break;
                }
            }
        }

        frames_.clear();
        for (const auto& entry : rawIndexEntries) {
            if (entry.ckid == FOURCC_00dc || entry.ckid == FOURCC_00db) {
                uint64_t chunkStart = 0;
                switch (detectedMode) {
                    case OffsetMode::RelativeToMoviData:
                        chunkStart = moviDataOffset_ + entry.offset;
                        break;
                    case OffsetMode::Absolute:
                        chunkStart = entry.offset;
                        break;
                    case OffsetMode::RelativeToMoviList:
                        chunkStart = moviListHeaderOffset_ + entry.offset;
                        break;
                }

                AviFrameIndex fi;
                fi.frameIndex = static_cast<uint32_t>(frames_.size());
                fi.filePayloadPos = chunkStart + 8;
                fi.payloadSize = entry.size;
                fi.isKeyframe = (entry.flags & 0x00000010) != 0;
                fi.timestampUs = static_cast<uint64_t>(fi.frameIndex) * frameDurationUs;
                frames_.push_back(fi);
            }
        }
    }

    // 5. Fallback movi scanner if idx1 was missing, empty, or unfinalized
    if (frames_.empty() || mainHeader_.dwTotalFrames == 0) {
        recoverIndexByScanningMovi();
    } else {
        totalFrames_ = static_cast<uint32_t>(frames_.size());
        durationUs_ = static_cast<uint64_t>(totalFrames_) * frameDurationUs;
    }

    if (frames_.empty()) {
        fileStream_.close();
        return false;
    }

    filePath_ = filePath;
    isOpen_.store(true);

    seekToFrame(0);
    startPlaybackThread();
    return true;
}

bool SessionRecordingPlayer::recoverIndexByScanningMovi() {
    if (!fileStream_.is_open()) return false;

    fileStream_.clear();
    fileStream_.seekg(0, std::ios::end);
    uint64_t fileLen = fileStream_.tellg();

    uint64_t scanPos = moviDataOffset_;
    if (scanPos == 0) {
        // Search file for 'movi'
        fileStream_.seekg(0);
        char buf[4096];
        uint64_t readPos = 0;
        bool found = false;
        while (readPos + 4 <= fileLen && !found) {
            fileStream_.seekg(readPos);
            fileStream_.read(buf, sizeof(buf));
            std::streamsize bytesRead = fileStream_.gcount();
            if (bytesRead < 4) break;
            for (std::streamsize i = 0; i <= bytesRead - 4; ++i) {
                if (std::memcmp(buf + i, "movi", 4) == 0) {
                    scanPos = readPos + i + 4;
                    moviDataOffset_ = scanPos;
                    found = true;
                    break;
                }
            }
            if (!found) {
                readPos += (bytesRead > 3 ? bytesRead - 3 : bytesRead);
            }
        }
        if (!found) return false;
    }

    uint64_t frameDurationUs = (fps_ > 0) ? (1000000ULL / fps_) : 33333ULL;
    frames_.clear();

    while (scanPos + 8 <= fileLen) {
        fileStream_.seekg(scanPos);
        uint32_t ckid = 0;
        uint32_t ckSize = 0;
        fileStream_.read(reinterpret_cast<char*>(&ckid), 4);
        fileStream_.read(reinterpret_cast<char*>(&ckSize), 4);
        if (!fileStream_.good()) break;

        if (ckid == FOURCC_idx1) {
            break; // Finished movi section
        }

        if (ckid == FOURCC_00dc || ckid == FOURCC_00db) {
            if (scanPos + 8 + ckSize > fileLen + 1) {
                break; // Truncated final frame
            }

            AviFrameIndex fi;
            fi.frameIndex = static_cast<uint32_t>(frames_.size());
            fi.filePayloadPos = scanPos + 8;
            fi.payloadSize = ckSize;
            fi.isKeyframe = true;
            fi.timestampUs = static_cast<uint64_t>(fi.frameIndex) * frameDurationUs;
            frames_.push_back(fi);

            scanPos += 8 + ((ckSize + 1ULL) & ~1ULL);
        } else {
            scanPos += 2; // Alignment crawl
        }
    }

    if (!frames_.empty()) {
        totalFrames_ = static_cast<uint32_t>(frames_.size());
        durationUs_ = static_cast<uint64_t>(totalFrames_) * frameDurationUs;
        if (mainHeader_.dwTotalFrames == 0) {
            mainHeader_.dwTotalFrames = totalFrames_;
        }
        if (streamHeader_.dwLength == 0) {
            streamHeader_.dwLength = totalFrames_;
        }
        return true;
    }

    return false;
}

bool SessionRecordingPlayer::decodeFramePayload(
    const uint8_t* payload, size_t size,
    std::vector<uint8_t>& outBgra, int& outW, int& outH)
{
    if (!payload || size == 0) return false;

    // 1. MJPEG branch
    bool isMjpeg = (bmpInfoHeader_.biCompression == FOURCC_MJPG ||
                    bmpInfoHeader_.biCompression == FOURCC_mjpg ||
                    (size >= 2 && payload[0] == 0xFF && payload[1] == 0xD8));
    if (isMjpeg) {
        return TileCodec::decodeJpeg(payload, size, outBgra, outW, outH);
    }

    // 2. Uncompressed DIB branch
    int w = bmpInfoHeader_.biWidth > 0 ? bmpInfoHeader_.biWidth : frameWidth_;
    int rawH = bmpInfoHeader_.biHeight != 0 ? bmpInfoHeader_.biHeight : frameHeight_;
    if (w <= 0 || rawH == 0) return false;

    bool isBottomUp = (rawH > 0);
    int h = std::abs(rawH);
    outW = w;
    outH = h;
    outBgra.resize(static_cast<size_t>(w * h * 4));

    if (bmpInfoHeader_.biBitCount == 32) {
        size_t expectedSize = static_cast<size_t>(w * h * 4);
        if (size < expectedSize) return false;
        size_t rowBytes = static_cast<size_t>(w * 4);
        uint8_t* dst = outBgra.data();
        if (isBottomUp) {
            for (int y = 0; y < h; ++y) {
                const uint8_t* srcRow = payload + static_cast<size_t>(h - 1 - y) * rowBytes;
                uint8_t* dstRow = dst + static_cast<size_t>(y) * rowBytes;
                std::memcpy(dstRow, srcRow, rowBytes);
            }
        } else {
            std::memcpy(dst, payload, expectedSize);
        }
        // Ensure opaque alpha for rendering
        for (int i = 3; i < static_cast<int>(expectedSize); i += 4) {
            if (dst[i] == 0) dst[i] = 0xFF;
        }
        return true;
    } else if (bmpInfoHeader_.biBitCount == 24) {
        size_t srcStride = ((static_cast<size_t>(w) * 3 + 3) / 4) * 4;
        if (size < srcStride * h) return false;
        uint8_t* dst = outBgra.data();
        for (int y = 0; y < h; ++y) {
            size_t srcRowIdx = isBottomUp ? static_cast<size_t>(h - 1 - y) : static_cast<size_t>(y);
            const uint8_t* srcRow = payload + srcRowIdx * srcStride;
            uint8_t* dstRow = dst + static_cast<size_t>(y) * (w * 4);
            for (int x = 0; x < w; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0]; // B
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2]; // R
                dstRow[x * 4 + 3] = 0xFF;              // A
            }
        }
        return true;
    }

    return false;
}

bool SessionRecordingPlayer::seekToFrame(uint32_t frameIndex) {
    if (frames_.empty()) return false;
    if (frameIndex >= totalFrames_) {
        frameIndex = totalFrames_ - 1;
    }

    const auto& fi = frames_[frameIndex];
    std::vector<uint8_t> payload(fi.payloadSize);
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!fileStream_.is_open()) return false;
        fileStream_.clear();
        fileStream_.seekg(fi.filePayloadPos);
        fileStream_.read(reinterpret_cast<char*>(payload.data()), fi.payloadSize);
        if (!fileStream_.good()) return false;

        int w = 0, h = 0;
        if (!decodeFramePayload(payload.data(), fi.payloadSize, currentFrameBgra_, w, h)) {
            return false;
        }
        frameWidth_ = w;
        frameHeight_ = h;
        currentFrameIndex_.store(frameIndex);
        hasNewFrame_.store(true);
    }

    if (onFrameReadyCallback_) {
        onFrameReadyCallback_();
    }
    return true;
}

bool SessionRecordingPlayer::seekToTimestampUs(uint64_t timestampUs) {
    if (frames_.empty()) return false;
    auto it = std::lower_bound(frames_.begin(), frames_.end(), timestampUs,
        [](const AviFrameIndex& a, uint64_t ts) {
            return a.timestampUs < ts;
        });

    if (it == frames_.end()) {
        return seekToFrame(static_cast<uint32_t>(frames_.size() - 1));
    }
    if (it != frames_.begin()) {
        auto prev = it - 1;
        if (timestampUs - prev->timestampUs < it->timestampUs - timestampUs) {
            return seekToFrame(prev->frameIndex);
        }
    }
    return seekToFrame(it->frameIndex);
}

bool SessionRecordingPlayer::stepFrame(int delta) {
    if (frames_.empty()) return false;
    int cur = static_cast<int>(currentFrameIndex_.load());
    int target = std::clamp(cur + delta, 0, static_cast<int>(totalFrames_) - 1);
    pause();
    return seekToFrame(static_cast<uint32_t>(target));
}

uint64_t SessionRecordingPlayer::currentTimestampUs() const {
    uint32_t idx = currentFrameIndex_.load();
    if (idx < frames_.size()) {
        return frames_[idx].timestampUs;
    }
    return static_cast<uint64_t>(idx) * (fps_ > 0 ? (1000000ULL / fps_) : 33333ULL);
}

void SessionRecordingPlayer::copyCurrentFrameBgra(std::vector<uint8_t>& outBuf, int& outW, int& outH) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    outBuf = currentFrameBgra_;
    outW = frameWidth_;
    outH = frameHeight_;
}

void SessionRecordingPlayer::setOnFrameReady(std::function<void()> cb) {
    onFrameReadyCallback_ = std::move(cb);
}

void SessionRecordingPlayer::play() {
    if (frames_.empty()) return;
    if (currentFrameIndex_.load() >= totalFrames_ - 1) {
        seekToFrame(0);
    }
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        isPlaying_.store(true);
    }
    playbackCv_.notify_all();
}

void SessionRecordingPlayer::pause() {
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        isPlaying_.store(false);
    }
    playbackCv_.notify_all();
}

void SessionRecordingPlayer::togglePlayPause() {
    if (isPlaying_.load()) {
        pause();
    } else {
        play();
    }
}

void SessionRecordingPlayer::setSpeed(float speed) {
    if (speed <= 0.05f) speed = 1.0f;
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        speedMultiplier_.store(speed);
    }
    playbackCv_.notify_all();
}

void SessionRecordingPlayer::playbackWorkerLoop() {
    while (!workerStopping_.load()) {
        std::unique_lock<std::mutex> lock(playbackMutex_);
        playbackCv_.wait(lock, [this]() {
            return isPlaying_.load() || workerStopping_.load();
        });

        if (workerStopping_.load()) break;

        float spd = std::max(0.25f, speedMultiplier_.load());
        int curFps = std::max(1, fps_);
        double intervalMs = (1000.0 / curFps) / spd;
        auto nextTick = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(static_cast<int64_t>(intervalMs));

        uint32_t cur = currentFrameIndex_.load();
        uint32_t next = cur + 1;
        if (next >= totalFrames_) {
            isPlaying_.store(false);
        } else {
            lock.unlock();
            seekToFrame(next);
            lock.lock();
        }

        playbackCv_.wait_until(lock, nextTick, [this]() {
            return !isPlaying_.load() || workerStopping_.load();
        });
    }
}

void SessionRecordingPlayer::startPlaybackThread() {
    stopPlaybackThread();
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        workerStopping_.store(false);
    }
    playbackThread_ = std::thread(&SessionRecordingPlayer::playbackWorkerLoop, this);
}

void SessionRecordingPlayer::stopPlaybackThread() {
    {
        std::lock_guard<std::mutex> lock(playbackMutex_);
        workerStopping_.store(true);
        isPlaying_.store(false);
    }
    playbackCv_.notify_all();
    if (playbackThread_.joinable()) {
        playbackThread_.join();
    }
}

bool SessionRecordingPlayer::exportSnapshotBmp(const std::string& outputPath) {
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    copyCurrentFrameBgra(bgra, w, h);
    if (bgra.empty() || w <= 0 || h <= 0) return false;

    std::filesystem::path p(outputPath);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    uint32_t imgSize = static_cast<uint32_t>(w * h * 4);
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + imgSize;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = w;
    bih.biHeight = -h; // negative for top-down DIB
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = 0; // BI_RGB
    bih.biSizeImage = imgSize;
    bih.biXPelsPerMeter = 2835;
    bih.biYPelsPerMeter = 2835;

    out.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    out.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    out.write(reinterpret_cast<const char*>(bgra.data()), imgSize);
    out.flush();
    return out.good();
}

bool SessionRecordingPlayer::exportSnapshotPng(const std::string& outputPath) {
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    copyCurrentFrameBgra(bgra, w, h);
    if (bgra.empty() || w <= 0 || h <= 0) return false;

    TileCodec::initGdiPlus();

    std::filesystem::path p(outputPath);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, outputPath.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring widePath(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, outputPath.c_str(), -1, widePath.data(), wlen);

    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppARGB, bgra.data());
    Gdiplus::Status st = bmp.Save(widePath.c_str(), &pngClsid, nullptr);
    return (st == Gdiplus::Ok);
}

bool SessionRecordingPlayer::exportSnapshot(const std::string& outputPath) {
    std::filesystem::path p(outputPath);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".png") {
        return exportSnapshotPng(outputPath);
    }
    return exportSnapshotBmp(outputPath);
}

bool SessionRecordingPlayer::trimClip(const std::string& outputPath, uint32_t startFrame, uint32_t endFrame) {
    if (frames_.empty() || startFrame > endFrame || endFrame >= totalFrames_) {
        return false;
    }
    uint32_t clipFrames = endFrame - startFrame + 1;

    std::filesystem::path p(outputPath);
    std::error_code ec;
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    uint32_t zero = 0;

    // 1. RIFF Header
    out.write("RIFF", 4);
    uint32_t riffSizePos = static_cast<uint32_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(&zero), 4); // placeholder riffSize
    out.write("AVI ", 4);

    // 2. LIST 'hdrl'
    uint32_t hdrlPos = static_cast<uint32_t>(out.tellp());
    out.write("LIST", 4);
    uint32_t hdrlSizePos = static_cast<uint32_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(&zero), 4); // placeholder hdrlSize
    out.write("hdrl", 4);

    // 'avih' chunk
    out.write("avih", 4);
    uint32_t avihSize = sizeof(MainAviHeader);
    out.write(reinterpret_cast<const char*>(&avihSize), 4);
    MainAviHeader trimmedAvih = mainHeader_;
    trimmedAvih.dwTotalFrames = clipFrames;
    out.write(reinterpret_cast<const char*>(&trimmedAvih), sizeof(trimmedAvih));

    // LIST 'strl'
    uint32_t strlPos = static_cast<uint32_t>(out.tellp());
    out.write("LIST", 4);
    uint32_t strlSizePos = static_cast<uint32_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(&zero), 4); // placeholder strlSize
    out.write("strl", 4);

    // 'strh' chunk
    out.write("strh", 4);
    uint32_t strhSize = sizeof(AviStreamHeader);
    out.write(reinterpret_cast<const char*>(&strhSize), 4);
    AviStreamHeader trimmedStrh = streamHeader_;
    trimmedStrh.dwLength = clipFrames;
    out.write(reinterpret_cast<const char*>(&trimmedStrh), sizeof(trimmedStrh));

    // 'strf' chunk
    out.write("strf", 4);
    uint32_t strfSize = sizeof(AviBmpInfoHeader);
    out.write(reinterpret_cast<const char*>(&strfSize), 4);
    out.write(reinterpret_cast<const char*>(&bmpInfoHeader_), sizeof(bmpInfoHeader_));

    // Patch strl size
    uint32_t strlEnd = static_cast<uint32_t>(out.tellp());
    uint32_t strlSize = strlEnd - (strlPos + 8);
    out.seekp(strlSizePos);
    out.write(reinterpret_cast<const char*>(&strlSize), 4);
    out.seekp(strlEnd);

    // Patch hdrl size
    uint32_t hdrlEnd = static_cast<uint32_t>(out.tellp());
    uint32_t hdrlSize = hdrlEnd - (hdrlPos + 8);
    out.seekp(hdrlSizePos);
    out.write(reinterpret_cast<const char*>(&hdrlSize), 4);
    out.seekp(hdrlEnd);

    // 3. LIST 'movi'
    out.write("LIST", 4);
    uint32_t moviSizePos = static_cast<uint32_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(&zero), 4); // placeholder
    out.write("movi", 4);
    uint32_t newMoviDataOffset = static_cast<uint32_t>(out.tellp());

    std::vector<AviIndexEntry> newIndexEntries;
    newIndexEntries.reserve(clipFrames);
    std::vector<uint8_t> payloadBuf;

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!fileStream_.is_open()) return false;

        for (uint32_t i = startFrame; i <= endFrame; ++i) {
            const auto& fi = frames_[i];
            uint64_t chunkHeaderPos = fi.filePayloadPos - 8;
            fileStream_.clear();
            fileStream_.seekg(chunkHeaderPos);
            uint32_t ckid = 0;
            uint32_t ckSize = 0;
            fileStream_.read(reinterpret_cast<char*>(&ckid), 4);
            fileStream_.read(reinterpret_cast<char*>(&ckSize), 4);
            if (!fileStream_.good()) return false;

            if (payloadBuf.size() < ckSize) {
                payloadBuf.resize(ckSize);
            }
            fileStream_.read(reinterpret_cast<char*>(payloadBuf.data()), ckSize);
            if (!fileStream_.good()) return false;

            uint32_t newChunkStart = static_cast<uint32_t>(out.tellp());
            out.write(reinterpret_cast<const char*>(&ckid), 4);
            out.write(reinterpret_cast<const char*>(&ckSize), 4);
            out.write(reinterpret_cast<const char*>(payloadBuf.data()), ckSize);
            if (ckSize % 2 != 0) {
                char pad = 0;
                out.write(&pad, 1);
            }

            AviIndexEntry entry{};
            entry.ckid = ckid;
            entry.flags = fi.isKeyframe ? 0x00000010 : 0;
            entry.offset = newChunkStart - newMoviDataOffset;
            entry.size = ckSize;
            newIndexEntries.push_back(entry);
        }
    }

    // 4. Write idx1 chunk
    uint32_t idxStart = static_cast<uint32_t>(out.tellp());
    out.write("idx1", 4);
    uint32_t idxSize = static_cast<uint32_t>(newIndexEntries.size() * sizeof(AviIndexEntry));
    out.write(reinterpret_cast<const char*>(&idxSize), 4);
    if (!newIndexEntries.empty()) {
        out.write(reinterpret_cast<const char*>(newIndexEntries.data()), idxSize);
    }

    uint32_t totalFileSize = static_cast<uint32_t>(out.tellp());
    uint32_t riffSize = totalFileSize - 8;
    uint32_t moviSize = (idxStart - newMoviDataOffset) + 4;

    // Patch RIFF size
    out.seekp(riffSizePos);
    out.write(reinterpret_cast<const char*>(&riffSize), 4);

    // Patch movi size
    out.seekp(moviSizePos);
    out.write(reinterpret_cast<const char*>(&moviSize), 4);

    out.flush();
    out.close();
    return true;
}

} // namespace cppdesk
