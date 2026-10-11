#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <mutex>

namespace cppdesk {

struct RewindFrame {
    uint64_t timestampMs = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> jpegData;
};

class SessionRewindBuffer {
public:
    static constexpr size_t MAX_SNAPSHOTS = 120; // 60s @ 500ms intervals
    static constexpr uint64_t INTERVAL_MS  = 500;
    static constexpr uint64_t WINDOW_MS    = 60000;

    SessionRewindBuffer();
    ~SessionRewindBuffer() = default;

    // Push decoded frame; encodes BGRA to JPEG and inserts into ring buffer
    void pushFrame(uint64_t timestampMs, const uint8_t* bgra, int width, int height);

    // Retrieve historical frame by offset relative to live stream (-60000ms to 0ms)
    bool getFrameAtOffset(int64_t offsetMs, std::vector<uint8_t>& outBgra, int& outW, int& outH, uint64_t& outActualTs) const;

    // Retrieve historical frame by linear chronological index [0 .. size()-1]
    bool getFrameAtIndex(size_t index, std::vector<uint8_t>& outBgra, int& outW, int& outH, uint64_t& outActualTs) const;

    size_t size() const;
    bool empty() const;
    uint64_t oldestTimestampMs() const;
    uint64_t newestTimestampMs() const;
    size_t totalMemoryBytes() const;
    void clear();

private:
    std::vector<RewindFrame> buffer_;
    size_t start_ = 0;
    size_t count_ = 0;
    uint64_t lastPushedTs_ = 0;
    mutable std::mutex mutex_;
};

} // namespace cppdesk
