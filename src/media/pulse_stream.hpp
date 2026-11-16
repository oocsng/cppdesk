#pragma once

// One PulseAudio stream (the client library also used by PipeWire's compatible server), loaded at run time: nothing
// is linked, and without the library or a sound server every call just fails. Linux only.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace cppdesk {

class PulseStream {
public:
    enum class Dir { Record, Playback };

    PulseStream();
    ~PulseStream();
    PulseStream(const PulseStream&) = delete;
    PulseStream& operator=(const PulseStream&) = delete;

    // The client library loads. CPPDESK_PULSE_LIB names the file to load instead of libpulse-simple.so.0 (tests).
    static bool available();

    // `device` nullptr is the default; "@DEFAULT_MONITOR@" is what the default output plays. 16-bit interleaved.
    // Returns false when the library, the server or the device is missing.
    bool open(Dir dir, const char* device, uint32_t rate, uint8_t channels, const char* appName, int latencyMs = 40);

    // Record: >0 bytes copied, 0 when nothing arrived within about 100 ms, -1 on error or when not open.
    int read(uint8_t* buf, size_t bytes);
    // Playback: queues the data for the stream's own thread and never blocks; the newest ~250 ms win.
    bool write(const uint8_t* buf, size_t bytes);
    void flush();   // drops what is queued and not yet played

    // Idempotent. Returns within about 500 ms even when the server is silent or gone (a thread stuck in the library
    // is left to finish on its own).
    void close();
    bool isOpen() const;

    struct State;   // what the stream's own thread shares with this object

private:
    mutable std::mutex stMu_;
    std::shared_ptr<State> st_;
    std::thread worker_;
};

} // namespace cppdesk
