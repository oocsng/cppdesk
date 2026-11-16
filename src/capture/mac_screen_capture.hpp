#pragma once

// A ScreenCaptureKit stream of one display (macOS). The interface is plain C++; the Objective-C lives in the .mm.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace cppdesk {

enum class MacCaptureState { Stopped, Starting, Running, NoPermission, Failed };

class MacCaptureStream {
public:
    MacCaptureStream();
    ~MacCaptureStream();
    MacCaptureStream(const MacCaptureStream&) = delete;
    MacCaptureStream& operator=(const MacCaptureStream&) = delete;

    // Starts capturing the display (CGDirectDisplayID) at up to `fps`, scaled so its long edge is at most
    // maxLongEdge pixels. Returns within a few seconds; false with state() NoPermission or Failed when it cannot.
    bool start(uint32_t displayId, int fps, int maxLongEdge);
    void stop();
    // Also becomes NoPermission or Failed later if macOS stops the stream (permission revoked, display change).
    MacCaptureState state() const;
    // Copies the newest frame (BGRA, tightly packed, opaque); false if none has arrived. `isNew` says whether it
    // differs from the one copied last.
    bool copyLatest(std::vector<uint8_t>& bgra, int& width, int& height, bool& isNew);

    // What the Mac is playing, as 48 kHz 16-bit interleaved stereo, delivered on the stream's own thread. Set before
    // start(); it takes effect on macOS 13 and later (earlier versions have no sound capture and the screen stream is
    // unchanged). CppDesk's own sound is left out.
    using AudioSink = std::function<void(const int16_t* interleavedStereo, size_t frames)>;
    void setAudioSink(AudioSink sink);
    bool audioAvailable() const;   // the running stream also delivers sound

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cppdesk
