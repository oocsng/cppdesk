#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace cppdesk {

struct AudioDeviceDesc {
    int id = -1;
    std::string name;
    bool isDefault = false;
};

class VoiceIntercom {
public:
    using AudioChunkCallback = std::function<void(const uint8_t* pcm, size_t bytes, uint32_t sampleRate, uint8_t channels)>;

    VoiceIntercom();
    ~VoiceIntercom();

    // Audio device enumeration
    static std::vector<AudioDeviceDesc> enumerateInputDevices();
    static std::vector<AudioDeviceDesc> enumerateOutputDevices();

    // Device selection
    void setInputDeviceId(int id) { selectedInputDeviceId_ = id; }
    void setOutputDeviceId(int id) { selectedOutputDeviceId_ = id; }
    int inputDeviceId() const { return selectedInputDeviceId_; }
    int outputDeviceId() const { return selectedOutputDeviceId_; }

    // Microphone capture (Win32 waveIn)
    bool startCapture(AudioChunkCallback onChunk, uint32_t sampleRate = 48000, uint8_t channels = 1);
    void stopCapture();
    bool isCapturing() const { return capturing_.load(); }

    // Intercom voice playback (Win32 waveOut)
    bool startPlayback(uint32_t sampleRate = 48000, uint8_t channels = 1);
    void stopPlayback();
    bool isPlaying() const { return playing_.load(); }
    void enqueuePlaybackChunk(const uint8_t* pcm, size_t bytes);

    // Mute & Volume controls
    void setMicMuted(bool muted) { micMuted_.store(muted); }
    bool isMicMuted() const { return micMuted_.load(); }

    void setOutputVolume(int percent) { outputVolume_.store(std::clamp(percent, 0, 100)); }
    int outputVolume() const { return outputVolume_.load(); }

    // Real-time audio input level (RMS: 0.0f .. 1.0f) for VU meter UI
    float inputLevel() const { return inputLevel_.load(); }

    // Digital signal processing helpers
    static float calculateRmsLevel(const int16_t* samples, size_t count);
    static void applyGain(int16_t* samples, size_t count, float gain);

private:
    void captureWorker();

    std::atomic<bool>           capturing_{false};
    std::atomic<bool>           captureStopping_{false};
    std::atomic<bool>           playing_{false};
    std::atomic<bool>           micMuted_{false};
    std::atomic<int>            outputVolume_{100};
    std::atomic<float>          inputLevel_{0.0f};

    uint32_t                    sampleRate_{48000};
    uint8_t                     channels_{1};
    AudioChunkCallback          onChunkCallback_;
    std::thread                 captureThread_;

    int                         selectedInputDeviceId_{-1};
    int                         selectedOutputDeviceId_{-1};
    uint8_t                     hwChannels_{1};
    uint32_t                    hwSampleRate_{48000};
    uint8_t                     playbackChannels_{1};
    uint32_t                    playbackSampleRate_{48000};

#if defined(_WIN32)
    // Win32 waveIn handles & buffers
    HWAVEIN                     hWaveIn_{nullptr};
    HANDLE                      hCaptureEvent_{nullptr};
    static constexpr size_t     NUM_CAPTURE_BUFFERS = 3;
    WAVEHDR                     captureHeaders_[NUM_CAPTURE_BUFFERS]{};
    std::vector<uint8_t>        captureBuffers_[NUM_CAPTURE_BUFFERS];

    // Win32 waveOut handles & buffers
    mutable std::mutex          playbackMutex_;
    HWAVEOUT                    hWaveOut_{nullptr};
    static constexpr size_t     NUM_PLAYBACK_BUFFERS = 8;
    WAVEHDR                     playbackHeaders_[NUM_PLAYBACK_BUFFERS]{};
    std::vector<uint8_t>        playbackBuffers_[NUM_PLAYBACK_BUFFERS];
    size_t                      currentPlayIdx_{0};
#elif defined(__APPLE__)
    static void MacAudioInputCallback(
        void* custom_data,
        AudioQueueRef queue,
        AudioQueueBufferRef buffer,
        const AudioTimeStamp* start_time,
        UInt32 num_packets,
        const AudioStreamPacketDescription* packet_desc);

    AudioQueueRef               inQueue_{nullptr};
    AudioQueueRef               outQueue_{nullptr};
    static constexpr size_t     NUM_CAPTURE_BUFFERS = 3;
    AudioQueueBufferRef         inBuffers_[NUM_CAPTURE_BUFFERS]{};
    static constexpr size_t     NUM_PLAYBACK_BUFFERS = 8;
    AudioQueueBufferRef         outBuffers_[NUM_PLAYBACK_BUFFERS]{};
    size_t                      currentPlayIdx_{0};
    mutable std::mutex          playbackMutex_;
#else
    mutable std::mutex          playbackMutex_;
#endif
};

} // namespace cppdesk
