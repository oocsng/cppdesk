#include "audio_convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cppdesk::audio {

namespace {

constexpr int kWireRate = 48000;

float sampleAt(const PcmBlock& b, size_t frame, int channel) {
    const size_t index = b.planar ? static_cast<size_t>(channel) * b.frames + frame
                                  : frame * static_cast<size_t>(b.channels) + static_cast<size_t>(channel);
    if (b.format == SampleFormat::S16) return static_cast<float>(static_cast<const int16_t*>(b.data)[index]);
    const float v = static_cast<const float*>(b.data)[index];
    if (!std::isfinite(v)) return 0.0f;
    return std::clamp(v, -1.0f, 1.0f) * 32767.0f;
}

int16_t toS16(float v) {
    return static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
}

} // namespace

void toWire(const PcmBlock& b, ConvertState& st, std::vector<int16_t>& out) {
    if (!b.data || b.frames == 0 || b.channels <= 0 || b.sampleRate <= 0) return;

    // Every frame as a stereo pair of floats on the 16-bit scale.
    thread_local std::vector<float> stereo;
    stereo.resize(b.frames * 2);
    for (size_t f = 0; f < b.frames; ++f) {
        float l, r;
        if (b.channels == 1) {
            l = r = sampleAt(b, f, 0);
        } else {
            l = sampleAt(b, f, 0);
            r = sampleAt(b, f, 1);
            if (b.channels > 2) {
                const float centre = 0.707f * sampleAt(b, f, 2);
                l += centre;
                r += centre;
            }
        }
        stereo[f * 2] = std::clamp(l, -32768.0f, 32767.0f);
        stereo[f * 2 + 1] = std::clamp(r, -32768.0f, 32767.0f);
    }

    if (b.sampleRate == kWireRate) {
        const size_t at = out.size();
        out.resize(at + b.frames * 2);
        for (size_t i = 0; i < b.frames * 2; ++i) out[at + i] = toS16(stereo[i]);
        return;
    }

    // Streaming linear resampling. `pos` is where the next output sample falls on this block's input frames, with
    // index -1 standing for the last frame of the previous block.
    const double step = static_cast<double>(b.sampleRate) / kWireRate;
    if (!st.primed) {
        st.primed = true;
        st.pos = 0;
        st.lastL = stereo[0];
        st.lastR = stereo[1];
    }
    const double frames = static_cast<double>(b.frames);
    while (true) {
        const double base = std::floor(st.pos);
        const long i0 = static_cast<long>(base);
        if (i0 + 1 >= static_cast<long>(b.frames)) break;
        const float t = static_cast<float>(st.pos - base);
        const float l0 = i0 < 0 ? st.lastL : stereo[static_cast<size_t>(i0) * 2];
        const float r0 = i0 < 0 ? st.lastR : stereo[static_cast<size_t>(i0) * 2 + 1];
        const float l1 = stereo[static_cast<size_t>(i0 + 1) * 2];
        const float r1 = stereo[static_cast<size_t>(i0 + 1) * 2 + 1];
        out.push_back(toS16(l0 + t * (l1 - l0)));
        out.push_back(toS16(r0 + t * (r1 - r0)));
        st.pos += step;
    }
    st.pos -= frames;
    st.lastL = stereo[(b.frames - 1) * 2];
    st.lastR = stereo[(b.frames - 1) * 2 + 1];
}

void adaptChannels(const int16_t* in, size_t frames, int from, int to, std::vector<int16_t>& out) {
    out.clear();
    if (!in || frames == 0 || from <= 0) return;
    if (from == 1 && to == 2) {
        out.resize(frames * 2);
        for (size_t f = 0; f < frames; ++f) out[f * 2] = out[f * 2 + 1] = in[f];
    } else if (from == 2 && to == 1) {
        out.resize(frames);
        for (size_t f = 0; f < frames; ++f) out[f] = static_cast<int16_t>((static_cast<int>(in[f * 2]) + in[f * 2 + 1]) / 2);
    } else {
        out.assign(in, in + frames * static_cast<size_t>(from));
    }
}

BoundedPcmQueue::BoundedPcmQueue(size_t maxBytes) : buf_(std::max<size_t>(1, maxBytes)) {}

void BoundedPcmQueue::push(const uint8_t* data, size_t bytes) {
    if (!data || bytes == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    const size_t cap = buf_.size();
    if (bytes >= cap) {
        dropped_ += size_ + (bytes - cap);
        std::memcpy(buf_.data(), data + (bytes - cap), cap);
        head_ = 0;
        size_ = cap;
        return;
    }
    if (size_ + bytes > cap) {
        const size_t drop = size_ + bytes - cap;
        head_ = (head_ + drop) % cap;
        size_ -= drop;
        dropped_ += drop;
    }
    const size_t tail = (head_ + size_) % cap;
    const size_t first = std::min(bytes, cap - tail);
    std::memcpy(buf_.data() + tail, data, first);
    if (first < bytes) std::memcpy(buf_.data(), data + first, bytes - first);
    size_ += bytes;
}

size_t BoundedPcmQueue::pop(uint8_t* dst, size_t maxBytes) {
    if (!dst) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    const size_t cap = buf_.size();
    const size_t n = std::min(size_, maxBytes);
    const size_t first = std::min(n, cap - head_);
    std::memcpy(dst, buf_.data() + head_, first);
    if (first < n) std::memcpy(dst + first, buf_.data(), n - first);
    head_ = (head_ + n) % cap;
    size_ -= n;
    return n;
}

size_t BoundedPcmQueue::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return size_;
}

uint64_t BoundedPcmQueue::droppedBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dropped_;
}

void BoundedPcmQueue::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    head_ = size_ = 0;
}

} // namespace cppdesk::audio
