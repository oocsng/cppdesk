#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <fstream>
#include <cstdint>
#include <memory>
#include <algorithm>

#include "session_recorder.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#ifdef FOURCC_RIFF
#undef FOURCC_RIFF
#endif
#ifdef FOURCC_LIST
#undef FOURCC_LIST
#endif
#endif

namespace cppdesk {

constexpr uint32_t FOURCC_RIFF = 0x46464952; // 'RIFF'
constexpr uint32_t FOURCC_AVI  = 0x20495641; // 'AVI '
constexpr uint32_t FOURCC_LIST = 0x5453494C; // 'LIST'
constexpr uint32_t FOURCC_hdrl = 0x6C726468; // 'hdrl'
constexpr uint32_t FOURCC_avih = 0x68697661; // 'avih'
constexpr uint32_t FOURCC_strl = 0x6C727473; // 'strl'
constexpr uint32_t FOURCC_strh = 0x68727473; // 'strh'
constexpr uint32_t FOURCC_strf = 0x66727473; // 'strf'
constexpr uint32_t FOURCC_movi = 0x69766F6D; // 'movi'
constexpr uint32_t FOURCC_idx1 = 0x31786469; // 'idx1'
constexpr uint32_t FOURCC_vids = 0x73646976; // 'vids'
constexpr uint32_t FOURCC_00dc = 0x63643030; // '00dc'
constexpr uint32_t FOURCC_00db = 0x62643030; // '00db'
constexpr uint32_t FOURCC_MJPG = 0x47504A4D; // 'MJPG'
constexpr uint32_t FOURCC_mjpg = 0x67706A6D; // 'mjpg'
constexpr uint32_t FOURCC_DIB  = 0x20424944; // 'DIB '

struct AviFrameIndex {
    uint32_t frameIndex{0};      // 0-based frame sequence
    uint64_t filePayloadPos{0};  // Byte offset in file where frame payload starts
    uint32_t payloadSize{0};     // Byte length of payload
    bool     isKeyframe{true};   // AVIIF_KEYFRAME (0x10)
    uint64_t timestampUs{0};     // Microseconds from start of recording
};

class SessionRecordingPlayer {
public:
    SessionRecordingPlayer();
    ~SessionRecordingPlayer();

    // Disable copy
    SessionRecordingPlayer(const SessionRecordingPlayer&) = delete;
    SessionRecordingPlayer& operator=(const SessionRecordingPlayer&) = delete;

    // Open and parse AVI file
    bool open(const std::string& filePath);
    void close();

    // Container metadata
    bool isOpen() const { return isOpen_.load(); }
    std::string filePath() const { return filePath_; }
    uint32_t totalFrames() const { return totalFrames_; }
    int frameWidth() const { return frameWidth_; }
    int frameHeight() const { return frameHeight_; }
    int fps() const { return fps_; }
    uint64_t durationUs() const { return durationUs_; }

    // Playback state and controls
    bool isPlaying() const { return isPlaying_.load(); }
    void play();
    void pause();
    void togglePlayPause();
    void setSpeed(float speed);
    float speed() const { return speedMultiplier_.load(); }

    // Frame seeking and stepping
    bool seekToFrame(uint32_t frameIndex);
    bool seekToTimestampUs(uint64_t timestampUs);
    bool stepFrame(int delta);
    uint32_t currentFrameIndex() const { return currentFrameIndex_.load(); }
    uint64_t currentTimestampUs() const;

    // Frame buffer access
    bool hasNewFrame() const { return hasNewFrame_.load(); }
    void clearNewFrameFlag() { hasNewFrame_.store(false); }
    void copyCurrentFrameBgra(std::vector<uint8_t>& outBuf, int& outW, int& outH);
    void setOnFrameReady(std::function<void()> cb);

    // Fallback scanner for incomplete or idx1-corrupted recordings
    bool recoverIndexByScanningMovi();

    // Snapshot and Transcoding export utilities
    bool exportSnapshotBmp(const std::string& outputPath);
    bool exportSnapshotPng(const std::string& outputPath);
    bool exportSnapshot(const std::string& outputPath);
    bool trimClip(const std::string& outputPath, uint32_t startFrame, uint32_t endFrame);

    // Decoding helper
    bool decodeFramePayload(const uint8_t* payload, size_t size, std::vector<uint8_t>& outBgra, int& outW, int& outH);

private:
    void playbackWorkerLoop();
    void startPlaybackThread();
    void stopPlaybackThread();

    std::string filePath_;
    mutable std::ifstream fileStream_;
    std::atomic<bool> isOpen_{false};

    // AVI container structures
    MainAviHeader mainHeader_{};
    AviStreamHeader streamHeader_{};
    AviBmpInfoHeader bmpInfoHeader_{};
    uint64_t moviListHeaderOffset_{0};
    uint64_t moviDataOffset_{0};
    uint64_t moviDataSize_{0};
    uint64_t fileLength_{0};

    uint32_t totalFrames_{0};
    int frameWidth_{0};
    int frameHeight_{0};
    int fps_{30};
    uint64_t durationUs_{0};
    std::vector<AviFrameIndex> frames_;

    // Current frame state
    mutable std::mutex frameMutex_;
    std::vector<uint8_t> currentFrameBgra_;
    std::atomic<uint32_t> currentFrameIndex_{0};
    std::atomic<bool> hasNewFrame_{false};
    std::function<void()> onFrameReadyCallback_;

    // Playback worker
    std::atomic<bool> isPlaying_{false};
    std::atomic<bool> workerStopping_{false};
    std::atomic<float> speedMultiplier_{1.0f};
    std::mutex playbackMutex_;
    std::condition_variable playbackCv_;
    std::thread playbackThread_;
};

} // namespace cppdesk
