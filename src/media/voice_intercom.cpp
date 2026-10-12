#include "voice_intercom.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace cppdesk {

VoiceIntercom::VoiceIntercom() = default;

VoiceIntercom::~VoiceIntercom() {
    stopCapture();
    stopPlayback();
}

float VoiceIntercom::calculateRmsLevel(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return 0.0f;

    double sumSq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double s = static_cast<double>(samples[i]);
        sumSq += s * s;
    }
    double meanSq = sumSq / static_cast<double>(count);
    double rms = std::sqrt(meanSq) / 32768.0;
    return static_cast<float>(std::clamp(rms, 0.0, 1.0));
}

void VoiceIntercom::applyGain(int16_t* samples, size_t count, float gain) {
    if (!samples || count == 0) return;
    for (size_t i = 0; i < count; ++i) {
        float v = static_cast<float>(samples[i]) * gain;
        samples[i] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
    }
}

#if defined(_WIN32)

bool VoiceIntercom::startCapture(AudioChunkCallback onChunk, uint32_t sampleRate, uint8_t channels) {
    stopCapture();
    sampleRate_ = sampleRate;
    channels_ = channels;
    onChunkCallback_ = std::move(onChunk);

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels_;
    wfx.nSamplesPerSec = sampleRate_;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    hCaptureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hCaptureEvent_) return false;

    MMRESULT res = waveInOpen(&hWaveIn_, WAVE_MAPPER, &wfx,
                              reinterpret_cast<DWORD_PTR>(hCaptureEvent_),
                              0, CALLBACK_EVENT);
    if (res != MMSYSERR_NOERROR) {
        CloseHandle(hCaptureEvent_);
        hCaptureEvent_ = nullptr;
        hWaveIn_ = nullptr;
        return false;
    }

    // 20ms buffer size
    DWORD bufBytes = (sampleRate_ / 50) * wfx.nBlockAlign;
    if (bufBytes == 0) bufBytes = 1920;

    for (size_t i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
        captureBuffers_[i].assign(bufBytes, 0);
        std::memset(&captureHeaders_[i], 0, sizeof(WAVEHDR));
        captureHeaders_[i].lpData = reinterpret_cast<LPSTR>(captureBuffers_[i].data());
        captureHeaders_[i].dwBufferLength = bufBytes;
        waveInPrepareHeader(hWaveIn_, &captureHeaders_[i], sizeof(WAVEHDR));
        waveInAddBuffer(hWaveIn_, &captureHeaders_[i], sizeof(WAVEHDR));
    }

    capturing_.store(true);
    captureStopping_.store(false);
    waveInStart(hWaveIn_);

    captureThread_ = std::thread(&VoiceIntercom::captureWorker, this);
    return true;
}

void VoiceIntercom::captureWorker() {
    while (!captureStopping_.load() && hWaveIn_) {
        WaitForSingleObject(hCaptureEvent_, 30);
        if (captureStopping_.load() || !hWaveIn_) break;

        for (size_t i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
            WAVEHDR& hdr = captureHeaders_[i];
            if (hdr.dwFlags & WHDR_DONE) {
                DWORD bytesRecorded = hdr.dwBytesRecorded;
                if (bytesRecorded > 0 && bytesRecorded <= hdr.dwBufferLength) {
                    const int16_t* pcmSamples = reinterpret_cast<const int16_t*>(hdr.lpData);
                    size_t sampleCount = bytesRecorded / sizeof(int16_t);

                    float level = calculateRmsLevel(pcmSamples, sampleCount);
                    inputLevel_.store(level);

                    if (!micMuted_.load() && onChunkCallback_) {
                        onChunkCallback_(reinterpret_cast<const uint8_t*>(hdr.lpData),
                                         bytesRecorded, sampleRate_, channels_);
                    }
                }

                // Re-queue buffer
                hdr.dwFlags &= ~WHDR_DONE;
                if (hWaveIn_ && !captureStopping_.load()) {
                    waveInAddBuffer(hWaveIn_, &hdr, sizeof(WAVEHDR));
                }
            }
        }
    }
}

void VoiceIntercom::stopCapture() {
    if (!capturing_.load() && !hWaveIn_) return;
    captureStopping_.store(true);
    if (hCaptureEvent_) SetEvent(hCaptureEvent_);

    try {
        if (captureThread_.joinable()) {
            captureThread_.join();
        }
    } catch (...) {}

    if (hWaveIn_) {
        waveInReset(hWaveIn_);
        for (size_t i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
            if (captureHeaders_[i].dwFlags & WHDR_PREPARED) {
                waveInUnprepareHeader(hWaveIn_, &captureHeaders_[i], sizeof(WAVEHDR));
            }
            captureHeaders_[i].dwFlags = 0;
        }
        waveInClose(hWaveIn_);
        hWaveIn_ = nullptr;
    }

    if (hCaptureEvent_) {
        CloseHandle(hCaptureEvent_);
        hCaptureEvent_ = nullptr;
    }

    capturing_.store(false);
    inputLevel_.store(0.0f);
}

bool VoiceIntercom::startPlayback(uint32_t sampleRate, uint8_t channels) {
    std::lock_guard<std::mutex> lk(playbackMutex_);
    if (hWaveOut_) return true;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    MMRESULT res = waveOutOpen(&hWaveOut_, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR) {
        hWaveOut_ = nullptr;
        playing_.store(false);
        return false;
    }

    DWORD bufBytes = (sampleRate / 50) * wfx.nBlockAlign;
    if (bufBytes == 0) bufBytes = 1920;

    for (size_t i = 0; i < NUM_PLAYBACK_BUFFERS; ++i) {
        playbackBuffers_[i].assign(bufBytes * 4, 0);
        std::memset(&playbackHeaders_[i], 0, sizeof(WAVEHDR));
        playbackHeaders_[i].lpData = reinterpret_cast<LPSTR>(playbackBuffers_[i].data());
        playbackHeaders_[i].dwBufferLength = static_cast<DWORD>(playbackBuffers_[i].size());
    }
    currentPlayIdx_ = 0;
    playing_.store(true);
    return true;
}

void VoiceIntercom::enqueuePlaybackChunk(const uint8_t* pcm, size_t bytes) {
    bytes = (bytes / sizeof(int16_t)) * sizeof(int16_t);
    if (!pcm || bytes < sizeof(int16_t) || bytes > 192000) return;
    std::lock_guard<std::mutex> lk(playbackMutex_);
    if (!hWaveOut_) return;

    int vol = outputVolume_.load();
    float gain = static_cast<float>(vol) / 100.0f;

    WAVEHDR& hdr = playbackHeaders_[currentPlayIdx_];
    if (hdr.dwFlags & WHDR_PREPARED) {
        if (!(hdr.dwFlags & WHDR_DONE)) {
            return; // Hardware buffer busy, drop to prevent latency build-up
        }
        waveOutUnprepareHeader(hWaveOut_, &hdr, sizeof(WAVEHDR));
        hdr.dwFlags = 0;
    }

    if (playbackBuffers_[currentPlayIdx_].size() < bytes) {
        playbackBuffers_[currentPlayIdx_].resize(bytes);
    }

    std::memcpy(playbackBuffers_[currentPlayIdx_].data(), pcm, bytes);
    if (vol != 100) {
        applyGain(reinterpret_cast<int16_t*>(playbackBuffers_[currentPlayIdx_].data()),
                  bytes / sizeof(int16_t), gain);
    }

    hdr.lpData = reinterpret_cast<LPSTR>(playbackBuffers_[currentPlayIdx_].data());
    hdr.dwBufferLength = static_cast<DWORD>(bytes);
    hdr.dwFlags = 0;

    if (waveOutPrepareHeader(hWaveOut_, &hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
        waveOutWrite(hWaveOut_, &hdr, sizeof(WAVEHDR));
        currentPlayIdx_ = (currentPlayIdx_ + 1) % NUM_PLAYBACK_BUFFERS;
    }
}

void VoiceIntercom::stopPlayback() {
    std::lock_guard<std::mutex> lk(playbackMutex_);
    if (!hWaveOut_) return;

    waveOutReset(hWaveOut_);
    for (size_t i = 0; i < NUM_PLAYBACK_BUFFERS; ++i) {
        if (playbackHeaders_[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(hWaveOut_, &playbackHeaders_[i], sizeof(WAVEHDR));
        }
        playbackHeaders_[i].dwFlags = 0;
    }
    waveOutClose(hWaveOut_);
    hWaveOut_ = nullptr;
    playing_.store(false);
}

#elif defined(__APPLE__)

void VoiceIntercom::MacAudioInputCallback(
    void* custom_data,
    AudioQueueRef queue,
    AudioQueueBufferRef buffer,
    const AudioTimeStamp* start_time,
    UInt32 num_packets,
    const AudioStreamPacketDescription* packet_desc) {
    (void)start_time;
    (void)num_packets;
    (void)packet_desc;
    auto* self = static_cast<VoiceIntercom*>(custom_data);
    if (!self || !buffer || buffer->mAudioDataByteSize == 0) {
        if (queue && buffer) {
            AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
        }
        return;
    }

    const int16_t* pcmSamples = reinterpret_cast<const int16_t*>(buffer->mAudioData);
    size_t sampleCount = buffer->mAudioDataByteSize / sizeof(int16_t);
    float level = calculateRmsLevel(pcmSamples, sampleCount);
    self->inputLevel_.store(level);

    if (!self->micMuted_.load() && self->onChunkCallback_) {
        self->onChunkCallback_(reinterpret_cast<const uint8_t*>(buffer->mAudioData),
                               buffer->mAudioDataByteSize,
                               self->sampleRate_,
                               self->channels_);
    }

    if (self->capturing_.load()) {
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
}

bool VoiceIntercom::startCapture(AudioChunkCallback onChunk, uint32_t sampleRate, uint8_t channels) {
    stopCapture();
    sampleRate_ = sampleRate;
    channels_ = channels;
    onChunkCallback_ = std::move(onChunk);

    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<Float64>(sampleRate_);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = channels_;
    format.mBytesPerFrame = (format.mBitsPerChannel / 8) * format.mChannelsPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame * format.mFramesPerPacket;

    OSStatus status = AudioQueueNewInput(&format, MacAudioInputCallback, this, nullptr, nullptr, 0, &inQueue_);
    if (status != noErr || !inQueue_) {
        inQueue_ = nullptr;
        return false;
    }

    UInt32 bufferByteSize = (sampleRate_ / 50) * format.mBytesPerFrame;
    if (bufferByteSize == 0) bufferByteSize = 1920;

    for (size_t i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
        status = AudioQueueAllocateBuffer(inQueue_, bufferByteSize, &inBuffers_[i]);
        if (status == noErr && inBuffers_[i]) {
            AudioQueueEnqueueBuffer(inQueue_, inBuffers_[i], 0, nullptr);
        }
    }

    capturing_.store(true);
    captureStopping_.store(false);
    AudioQueueStart(inQueue_, nullptr);
    return true;
}

void VoiceIntercom::captureWorker() {
    // AudioQueue manages its own audio servicing thread on macOS
}

void VoiceIntercom::stopCapture() {
    if (!capturing_.load() && !inQueue_) return;
    captureStopping_.store(true);
    capturing_.store(false);

    if (inQueue_) {
        AudioQueueStop(inQueue_, true);
        AudioQueueDispose(inQueue_, true);
        inQueue_ = nullptr;
    }
    for (size_t i = 0; i < NUM_CAPTURE_BUFFERS; ++i) {
        inBuffers_[i] = nullptr;
    }
    inputLevel_.store(0.0f);
}

bool VoiceIntercom::startPlayback(uint32_t sampleRate, uint8_t channels) {
    std::lock_guard<std::mutex> lk(playbackMutex_);
    if (outQueue_) return true;

    AudioStreamBasicDescription format{};
    format.mSampleRate = static_cast<Float64>(sampleRate);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = channels;
    format.mBytesPerFrame = (format.mBitsPerChannel / 8) * format.mChannelsPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame * format.mFramesPerPacket;

    OSStatus status = AudioQueueNewOutput(&format, nullptr, nullptr, nullptr, nullptr, 0, &outQueue_);
    if (status != noErr || !outQueue_) {
        outQueue_ = nullptr;
        playing_.store(false);
        return false;
    }

    UInt32 bufferByteSize = (sampleRate / 50) * format.mBytesPerFrame * 4;
    if (bufferByteSize == 0) bufferByteSize = 1920 * 4;

    for (size_t i = 0; i < NUM_PLAYBACK_BUFFERS; ++i) {
        AudioQueueAllocateBuffer(outQueue_, bufferByteSize, &outBuffers_[i]);
    }
    currentPlayIdx_ = 0;
    playing_.store(true);
    AudioQueueStart(outQueue_, nullptr);
    return true;
}

void VoiceIntercom::enqueuePlaybackChunk(const uint8_t* pcm, size_t bytes) {
    bytes = (bytes / sizeof(int16_t)) * sizeof(int16_t);
    if (!pcm || bytes < sizeof(int16_t) || bytes > 192000) return;
    std::lock_guard<std::mutex> lk(playbackMutex_);
    if (!outQueue_) return;

    int vol = outputVolume_.load();
    float gain = static_cast<float>(vol) / 100.0f;

    AudioQueueBufferRef buf = outBuffers_[currentPlayIdx_];
    if (!buf) return;

    if (buf->mAudioDataBytesCapacity < bytes) {
        bytes = buf->mAudioDataBytesCapacity;
    }

    std::memcpy(buf->mAudioData, pcm, bytes);
    buf->mAudioDataByteSize = static_cast<UInt32>(bytes);

    if (vol != 100) {
        applyGain(reinterpret_cast<int16_t*>(buf->mAudioData),
                  bytes / sizeof(int16_t), gain);
    }

    AudioQueueEnqueueBuffer(outQueue_, buf, 0, nullptr);
    currentPlayIdx_ = (currentPlayIdx_ + 1) % NUM_PLAYBACK_BUFFERS;
}

void VoiceIntercom::stopPlayback() {
    std::lock_guard<std::mutex> lk(playbackMutex_);
    if (!outQueue_) return;

    AudioQueueStop(outQueue_, true);
    AudioQueueDispose(outQueue_, true);
    outQueue_ = nullptr;
    for (size_t i = 0; i < NUM_PLAYBACK_BUFFERS; ++i) {
        outBuffers_[i] = nullptr;
    }
    playing_.store(false);
}

#else

bool VoiceIntercom::startCapture(AudioChunkCallback onChunk, uint32_t sampleRate, uint8_t channels) {
    (void)onChunk;
    (void)sampleRate;
    (void)channels;
    return false;
}

void VoiceIntercom::captureWorker() {}

void VoiceIntercom::stopCapture() {
    capturing_.store(false);
    inputLevel_.store(0.0f);
}

bool VoiceIntercom::startPlayback(uint32_t sampleRate, uint8_t channels) {
    (void)sampleRate;
    (void)channels;
    return false;
}

void VoiceIntercom::enqueuePlaybackChunk(const uint8_t* pcm, size_t bytes) {
    (void)pcm;
    (void)bytes;
}

void VoiceIntercom::stopPlayback() {
    playing_.store(false);
}

#endif

} // namespace cppdesk
