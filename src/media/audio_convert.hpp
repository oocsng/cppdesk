#pragma once

// Plain helpers for moving sound between what a platform delivers and what the wire carries (48 kHz 16-bit stereo),
// and a queue that keeps playback current. No platform headers, so every platform's tests can check them.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace cppdesk::audio {

enum class SampleFormat { S16, F32 };

struct PcmBlock {
    const void* data;
    size_t frames;
    int channels;
    int sampleRate;
    SampleFormat format;
    bool planar;   // true: one run of `frames` samples per channel; false: interleaved
};

// The resampler's memory between blocks, so chunk edges leave no click.
struct ConvertState {
    double pos = 0;
    float lastL = 0, lastR = 0;
    bool primed = false;
};

// Appends `block` as interleaved 48 kHz 16-bit stereo to `out`. One channel is duplicated, two pass as they are, more
// fold the centre channel (the third) into both sides at 0.707; any other sample rate is resampled linearly
// (streaming); NaN and infinity become silence; everything is clamped. An empty or malformed block appends nothing.
void toWire(const PcmBlock& block, ConvertState& state, std::vector<int16_t>& out);

// One channel to two (duplicated) or two to one (averaged); any other combination is copied unchanged.
void adaptChannels(const int16_t* in, size_t frames, int fromChannels, int toChannels, std::vector<int16_t>& out);

// A byte ring where the newest data wins: playback never lags behind by more than `maxBytes`.
class BoundedPcmQueue {
public:
    explicit BoundedPcmQueue(size_t maxBytes);
    // Drops the oldest bytes beyond maxBytes; a push larger than that keeps its last maxBytes.
    void push(const uint8_t* data, size_t bytes);
    size_t pop(uint8_t* dst, size_t maxBytes);   // oldest first; returns the bytes copied
    size_t size() const;
    uint64_t droppedBytes() const;
    void clear();

private:
    mutable std::mutex mu_;
    std::vector<uint8_t> buf_;
    size_t head_ = 0, size_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace cppdesk::audio
