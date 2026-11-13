#pragma once

// A ScreenCaptureKit stream of one display (macOS). The interface is plain C++; the Objective-C lives in the .mm.

#include <cstdint>
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cppdesk
