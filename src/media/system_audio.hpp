#pragma once

// What the engine needs from a platform to carry the host's system sound to a viewer, behind two small interfaces so
// the engine does not know which platform it runs on (and tests can stand in for the sound hardware).
// Windows keeps its own capture and playback code inside the engine; these serve Linux and macOS.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace cppdesk {

// What the host's speakers play, as 48 kHz 16-bit interleaved stereo.
class SystemAudioSource {
public:
    using Sink = std::function<void(const int16_t* interleavedStereo, size_t frames)>;
    virtual ~SystemAudioSource() = default;
    virtual bool start(Sink sink) = 0;   // false: nothing can be captured here
    virtual void stop() = 0;             // returns within about 500 ms; the sink is not called after it returns
};

// Plays the host's sound on the viewer (48 kHz 16-bit interleaved stereo).
class AudioPlayer {
public:
    virtual ~AudioPlayer() = default;
    virtual bool start() = 0;
    virtual void play(const int16_t* interleavedStereo, size_t frames) = 0;   // never blocks; the newest ~250 ms win
    virtual void stop() = 0;
};

// Linux: the monitor of the default output through PulseAudio. Elsewhere nullptr (macOS delivers its sound with the
// screen stream; Windows captures inside the engine).
std::unique_ptr<SystemAudioSource> makeSystemAudioSource();

// Linux and macOS: a VoiceIntercom speaker. Windows: nullptr (the engine plays through waveOut itself).
std::unique_ptr<AudioPlayer> makeAudioPlayer();

} // namespace cppdesk
