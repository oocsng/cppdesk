#pragma once

// Receives the shared screen's video from PipeWire (Linux). libpipewire is loaded when the stream starts, so the app
// runs on systems without it; the stream then reports Unavailable.

#include "latest_frame.hpp"

#include <cstdint>
#include <memory>

namespace cppdesk::wayland {

enum class PwState { Stopped, Starting, Streaming, Unavailable, Failed };

class PipeWireStream {
public:
    explicit PipeWireStream(std::shared_ptr<LatestFrame> sink);
    ~PipeWireStream();
    PipeWireStream(const PipeWireStream&) = delete;
    PipeWireStream& operator=(const PipeWireStream&) = delete;

    // Connects to the video connection `fd` (taken over, closed on failure) and the stream `nodeId`; frames land in
    // the sink as packed BGRA. False when PipeWire is missing or the connection fails (state says which).
    // The environment variable CPPDESK_PIPEWIRE_LIB names another library file (tests).
    // Whether PipeWire can be loaded here at all (known before anyone is asked for permission).
    static bool available();

    bool start(int fd, uint32_t nodeId);
    void stop();   // idempotent
    PwState state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cppdesk::wayland
