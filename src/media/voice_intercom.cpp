#include "voice_intercom.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propidl.h>

namespace {
static const CLSID CLSID_MMDeviceEnumerator_local = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID IID_IMMDeviceEnumerator_local = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const PROPERTYKEY PKEY_Device_FriendlyName_local = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

static std::vector<std::string> getWasapiFullDeviceNames(bool isCapture) {
    std::vector<std::string> names;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coInited = SUCCEEDED(hrCo);

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_local, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator_local, (void**)&pEnum);
    if (SUCCEEDED(hr) && pEnum) {
        IMMDeviceCollection* pCol = nullptr;
        hr = pEnum->EnumAudioEndpoints(isCapture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &pCol);
        if (SUCCEEDED(hr) && pCol) {
            UINT count = 0;
            pCol->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                    IPropertyStore* pProps = nullptr;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                        PROPVARIANT var;
                        PropVariantInit(&var);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName_local, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
                            int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                            if (len > 1) {
                                std::string s(len - 1, 0);
                                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, &s[0], len, nullptr, nullptr);
                                names.push_back(s);
                            }
                        }
                        PropVariantClear(&var);
                        pProps->Release();
                    }
                    pDev->Release();
                }
            }
            pCol->Release();
        }
        pEnum->Release();
    }
    if (coInited) {
        CoUninitialize();
    }
    return names;
}
} // namespace
#endif

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

std::vector<AudioDeviceDesc> VoiceIntercom::enumerateInputDevices() {
    std::vector<AudioDeviceDesc> list;
    AudioDeviceDesc defDev;
    defDev.id = -1;
    defDev.name = "Default Recording Device";
    defDev.isDefault = true;
    list.push_back(std::move(defDev));

#if defined(_WIN32)
    auto wasapiNames = getWasapiFullDeviceNames(true);
    std::vector<bool> wasapiUsed(wasapiNames.size(), false);

    UINT numDevs = waveInGetNumDevs();
    for (UINT i = 0; i < numDevs; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            AudioDeviceDesc desc;
            desc.id = static_cast<int>(i);
            std::string winmmName;
            int len = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                winmmName.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, &winmmName[0], len, nullptr, nullptr);
            }

            std::string trimmed = winmmName;
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
                trimmed.pop_back();
            }

            std::string resolvedName = winmmName;
            // 1. Exact prefix match against full WASAPI endpoint names
            for (size_t w = 0; w < wasapiNames.size(); ++w) {
                if (!wasapiUsed[w] && !trimmed.empty()) {
                    if (wasapiNames[w].rfind(trimmed, 0) == 0) {
                        resolvedName = wasapiNames[w];
                        wasapiUsed[w] = true;
                        break;
                    }
                }
            }
            // 2. Substring match if prefix had minor variations
            if (resolvedName == winmmName && !trimmed.empty()) {
                std::string sub = (trimmed.size() >= 16) ? trimmed.substr(0, 16) : trimmed;
                for (size_t w = 0; w < wasapiNames.size(); ++w) {
                    if (!wasapiUsed[w] && wasapiNames[w].find(sub) != std::string::npos) {
                        resolvedName = wasapiNames[w];
                        wasapiUsed[w] = true;
                        break;
                    }
                }
            }
            // 3. Fallback to index alignment if counts match
            if (resolvedName == winmmName && i < wasapiNames.size() && !wasapiUsed[i]) {
                resolvedName = wasapiNames[i];
                wasapiUsed[i] = true;
            }

            desc.name = resolvedName.empty() ? ("Audio Input #" + std::to_string(i)) : resolvedName;
            desc.isDefault = false;
            list.push_back(std::move(desc));
        }
    }
#endif
    return list;
}

std::vector<AudioDeviceDesc> VoiceIntercom::enumerateOutputDevices() {
    std::vector<AudioDeviceDesc> list;
    AudioDeviceDesc defDev;
    defDev.id = -1;
    defDev.name = "Default Playback Device";
    defDev.isDefault = true;
    list.push_back(std::move(defDev));

#if defined(_WIN32)
    auto wasapiNames = getWasapiFullDeviceNames(false);
    std::vector<bool> wasapiUsed(wasapiNames.size(), false);

    UINT numDevs = waveOutGetNumDevs();
    for (UINT i = 0; i < numDevs; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            AudioDeviceDesc desc;
            desc.id = static_cast<int>(i);
            std::string winmmName;
            int len = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                winmmName.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, &winmmName[0], len, nullptr, nullptr);
            }

            std::string trimmed = winmmName;
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
                trimmed.pop_back();
            }

            std::string resolvedName = winmmName;
            // 1. Exact prefix match against full WASAPI endpoint names
            for (size_t w = 0; w < wasapiNames.size(); ++w) {
                if (!wasapiUsed[w] && !trimmed.empty()) {
                    if (wasapiNames[w].rfind(trimmed, 0) == 0) {
                        resolvedName = wasapiNames[w];
                        wasapiUsed[w] = true;
                        break;
                    }
                }
            }
            // 2. Substring match if prefix had minor variations
            if (resolvedName == winmmName && !trimmed.empty()) {
                std::string sub = (trimmed.size() >= 16) ? trimmed.substr(0, 16) : trimmed;
                for (size_t w = 0; w < wasapiNames.size(); ++w) {
                    if (!wasapiUsed[w] && wasapiNames[w].find(sub) != std::string::npos) {
                        resolvedName = wasapiNames[w];
                        wasapiUsed[w] = true;
                        break;
                    }
                }
            }
            // 3. Fallback to index alignment if counts match
            if (resolvedName == winmmName && i < wasapiNames.size() && !wasapiUsed[i]) {
                resolvedName = wasapiNames[i];
                wasapiUsed[i] = true;
            }

            desc.name = resolvedName.empty() ? ("Audio Output #" + std::to_string(i)) : resolvedName;
            desc.isDefault = false;
            list.push_back(std::move(desc));
        }
    }
#endif
    return list;
}

#if defined(_WIN32)

bool VoiceIntercom::startCapture(AudioChunkCallback onChunk, uint32_t sampleRate, uint8_t channels) {
    stopCapture();
    sampleRate_ = sampleRate;
    channels_ = channels;
    onChunkCallback_ = std::move(onChunk);

    hCaptureEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hCaptureEvent_) return false;

    UINT devId = WAVE_MAPPER;
    if (selectedInputDeviceId_ >= 0) {
        UINT numDevs = waveInGetNumDevs();
        if (static_cast<UINT>(selectedInputDeviceId_) < numDevs) {
            devId = static_cast<UINT>(selectedInputDeviceId_);
        }
    }

    struct FormatCandidate {
        uint32_t rate;
        uint8_t channels;
    };
    FormatCandidate candidates[] = {
        { sampleRate_, channels_ },
        { 48000, 1 },
        { 48000, 2 },
        { 44100, 1 },
        { 44100, 2 },
        { 32000, 1 },
        { 32000, 2 },
        { 24000, 1 },
        { 24000, 2 },
        { 16000, 1 },
        { 16000, 2 },
        { 8000, 1 },
        { 8000, 2 }
    };

    MMRESULT res = MMSYSERR_ERROR;
    WAVEFORMATEX wfx{};
    auto tryOpen = [&](UINT targetDev) -> bool {
        for (const auto& cand : candidates) {
            std::memset(&wfx, 0, sizeof(wfx));
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = cand.channels;
            wfx.nSamplesPerSec = cand.rate;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            wfx.cbSize = 0;

            res = waveInOpen(&hWaveIn_, targetDev, &wfx,
                             reinterpret_cast<DWORD_PTR>(hCaptureEvent_),
                             0, CALLBACK_EVENT);
            if (res == MMSYSERR_NOERROR) {
                hwChannels_ = cand.channels;
                hwSampleRate_ = cand.rate;
                return true;
            }
        }
        return false;
    };

    if (!tryOpen(devId)) {
        if (devId != WAVE_MAPPER) {
            tryOpen(WAVE_MAPPER);
        }
    }

    if (res != MMSYSERR_NOERROR) {
        CloseHandle(hCaptureEvent_);
        hCaptureEvent_ = nullptr;
        hWaveIn_ = nullptr;
        return false;
    }

    // 20ms buffer size
    DWORD bufBytes = (hwSampleRate_ / 50) * wfx.nBlockAlign;
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

                    if (hwChannels_ == 2 && channels_ == 1) {
                        size_t frameCount = sampleCount / 2;
                        thread_local std::vector<int16_t> monoBuf;
                        monoBuf.resize(frameCount);
                        for (size_t f = 0; f < frameCount; ++f) {
                            int32_t sum = static_cast<int32_t>(pcmSamples[f * 2]) + static_cast<int32_t>(pcmSamples[f * 2 + 1]);
                            monoBuf[f] = static_cast<int16_t>(sum / 2);
                        }
                        float level = calculateRmsLevel(monoBuf.data(), frameCount);
                        inputLevel_.store(level);

                        if (!micMuted_.load() && onChunkCallback_) {
                            onChunkCallback_(reinterpret_cast<const uint8_t*>(monoBuf.data()),
                                             frameCount * sizeof(int16_t), hwSampleRate_, 1);
                        }
                    } else {
                        float level = calculateRmsLevel(pcmSamples, sampleCount);
                        inputLevel_.store(level);

                        if (!micMuted_.load() && onChunkCallback_) {
                            onChunkCallback_(reinterpret_cast<const uint8_t*>(hdr.lpData),
                                             bytesRecorded, hwSampleRate_, hwChannels_);
                        }
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

    UINT devId = WAVE_MAPPER;
    if (selectedOutputDeviceId_ >= 0) {
        UINT numDevs = waveOutGetNumDevs();
        if (static_cast<UINT>(selectedOutputDeviceId_) < numDevs) {
            devId = static_cast<UINT>(selectedOutputDeviceId_);
        }
    }

    struct FormatCandidate {
        uint32_t rate;
        uint8_t channels;
    };
    FormatCandidate candidates[] = {
        { sampleRate, channels },
        { 48000, 1 },
        { 48000, 2 },
        { 44100, 1 },
        { 44100, 2 },
        { 32000, 1 },
        { 32000, 2 },
        { 24000, 1 },
        { 24000, 2 },
        { 16000, 1 },
        { 16000, 2 },
        { 8000, 1 },
        { 8000, 2 }
    };

    MMRESULT res = MMSYSERR_ERROR;
    WAVEFORMATEX wfx{};
    auto tryOpen = [&](UINT targetDev) -> bool {
        for (const auto& cand : candidates) {
            std::memset(&wfx, 0, sizeof(wfx));
            wfx.wFormatTag = WAVE_FORMAT_PCM;
            wfx.nChannels = cand.channels;
            wfx.nSamplesPerSec = cand.rate;
            wfx.wBitsPerSample = 16;
            wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
            wfx.cbSize = 0;

            res = waveOutOpen(&hWaveOut_, targetDev, &wfx, 0, 0, CALLBACK_NULL);
            if (res == MMSYSERR_NOERROR) {
                playbackChannels_ = cand.channels;
                playbackSampleRate_ = cand.rate;
                return true;
            }
        }
        return false;
    };

    if (!tryOpen(devId)) {
        if (devId != WAVE_MAPPER) {
            tryOpen(WAVE_MAPPER);
        }
    }

    if (res != MMSYSERR_NOERROR) {
        hWaveOut_ = nullptr;
        playing_.store(false);
        return false;
    }

    DWORD bufBytes = (playbackSampleRate_ / 50) * wfx.nBlockAlign;
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

    size_t chosenIdx = size_t(-1);
    for (size_t step = 0; step < NUM_PLAYBACK_BUFFERS; ++step) {
        size_t idx = (currentPlayIdx_ + step) % NUM_PLAYBACK_BUFFERS;
        WAVEHDR& h = playbackHeaders_[idx];
        if (!(h.dwFlags & WHDR_PREPARED)) {
            chosenIdx = idx;
            break;
        } else if (h.dwFlags & WHDR_DONE) {
            waveOutUnprepareHeader(hWaveOut_, &h, sizeof(WAVEHDR));
            h.dwFlags = 0;
            chosenIdx = idx;
            break;
        }
    }
    if (chosenIdx == size_t(-1)) {
        return; // All 8 buffers are currently busy in hardware playback
    }
    currentPlayIdx_ = chosenIdx;

    int vol = outputVolume_.load();
    float gain = static_cast<float>(vol) / 100.0f;

    std::vector<uint8_t> convertedBuf;
    const uint8_t* sendPcm = pcm;
    size_t sendBytes = bytes;
    if (playbackChannels_ == 2) {
        // Mono to stereo upmixing
        size_t sampleCount = bytes / sizeof(int16_t);
        convertedBuf.resize(sampleCount * 2 * sizeof(int16_t));
        const int16_t* inSamples = reinterpret_cast<const int16_t*>(pcm);
        int16_t* outSamples = reinterpret_cast<int16_t*>(convertedBuf.data());
        for (size_t i = 0; i < sampleCount; ++i) {
            outSamples[i * 2] = inSamples[i];
            outSamples[i * 2 + 1] = inSamples[i];
        }
        sendPcm = convertedBuf.data();
        sendBytes = convertedBuf.size();
    }

    WAVEHDR& hdr = playbackHeaders_[currentPlayIdx_];
    if (playbackBuffers_[currentPlayIdx_].size() < sendBytes) {
        playbackBuffers_[currentPlayIdx_].resize(sendBytes);
    }

    std::memcpy(playbackBuffers_[currentPlayIdx_].data(), sendPcm, sendBytes);
    if (vol != 100) {
        applyGain(reinterpret_cast<int16_t*>(playbackBuffers_[currentPlayIdx_].data()),
                  sendBytes / sizeof(int16_t), gain);
    }

    hdr.lpData = reinterpret_cast<LPSTR>(playbackBuffers_[currentPlayIdx_].data());
    hdr.dwBufferLength = static_cast<DWORD>(sendBytes);
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
