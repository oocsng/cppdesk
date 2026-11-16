#include "system_audio.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include "voice_intercom.hpp"
#endif

#if defined(__linux__)
#include "pulse_stream.hpp"
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#endif

namespace cppdesk {

#if defined(__linux__)

namespace {

class PulseMonitorSource : public SystemAudioSource {
public:
    ~PulseMonitorSource() override { stop(); }

    bool start(Sink sink) override {
        stop();
        if (!stream_.open(PulseStream::Dir::Record, "@DEFAULT_MONITOR@", 48000, 2, "CppDesk host sound", 40)) return false;
        stop_.store(false);
        thread_ = std::thread([this, sink = std::move(sink)] {
            constexpr size_t kFrameBytes = 2 * sizeof(int16_t);
            std::vector<uint8_t> buf(48000 / 50 * kFrameBytes);   // 20 ms
            size_t carry = 0;   // bytes of a frame cut in two by the queue dropping old audio
            while (!stop_.load()) {
                const int n = stream_.read(buf.data() + carry, buf.size() - carry);
                if (n < 0) break;
                if (n == 0) continue;
                const size_t have = carry + static_cast<size_t>(n);
                const size_t frames = have / kFrameBytes;
                if (frames > 0 && !stop_.load()) sink(reinterpret_cast<const int16_t*>(buf.data()), frames);
                carry = have - frames * kFrameBytes;
                if (carry > 0 && frames > 0) std::memmove(buf.data(), buf.data() + frames * kFrameBytes, carry);
            }
        });
        return true;
    }

    void stop() override {
        stop_.store(true);
        stream_.close();   // wakes a read in progress
        if (thread_.joinable()) thread_.join();
    }

private:
    PulseStream stream_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

} // namespace

std::unique_ptr<SystemAudioSource> makeSystemAudioSource() { return std::make_unique<PulseMonitorSource>(); }

#else

std::unique_ptr<SystemAudioSource> makeSystemAudioSource() { return nullptr; }

#endif

#if defined(__linux__) || defined(__APPLE__)

namespace {

class VoiceIntercomPlayer : public AudioPlayer {
public:
    bool start() override { return speaker_.startPlayback(48000, 2); }
    void play(const int16_t* interleavedStereo, size_t frames) override {
        speaker_.enqueuePlaybackChunk(reinterpret_cast<const uint8_t*>(interleavedStereo), frames * 2 * sizeof(int16_t), 2);
    }
    void stop() override { speaker_.stopPlayback(); }

private:
    VoiceIntercom speaker_;   // its own instance: the host's sound never mixes with the intercom speaker
};

} // namespace

std::unique_ptr<AudioPlayer> makeAudioPlayer() { return std::make_unique<VoiceIntercomPlayer>(); }

#else

std::unique_ptr<AudioPlayer> makeAudioPlayer() { return nullptr; }

#endif

} // namespace cppdesk
