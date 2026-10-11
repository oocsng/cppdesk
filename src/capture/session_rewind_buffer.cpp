#include "session_rewind_buffer.hpp"
#include "screen_capture.hpp"
#include <cmath>
#include <algorithm>

namespace cppdesk {

SessionRewindBuffer::SessionRewindBuffer() {
    TileCodec::initGdiPlus();
    buffer_.resize(MAX_SNAPSHOTS);
}

void SessionRewindBuffer::pushFrame(uint64_t timestampMs, const uint8_t* bgra, int width, int height) {
    if (!bgra || width <= 0 || height <= 0) return;

    // Throttle to approximately INTERVAL_MS intervals
    if (count_ > 0 && timestampMs > lastPushedTs_ && (timestampMs - lastPushedTs_) < (INTERVAL_MS - 50)) {
        return;
    }

    std::vector<uint8_t> jpeg = TileCodec::encodeJpeg(bgra, width, height, 70);
    if (jpeg.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    size_t slot = 0;
    if (count_ < MAX_SNAPSHOTS) {
        slot = (start_ + count_) % MAX_SNAPSHOTS;
        count_++;
    } else {
        slot = start_;
        start_ = (start_ + 1) % MAX_SNAPSHOTS;
    }

    buffer_[slot].timestampMs = timestampMs;
    buffer_[slot].width = width;
    buffer_[slot].height = height;
    buffer_[slot].jpegData = std::move(jpeg);
    lastPushedTs_ = timestampMs;
}

bool SessionRewindBuffer::getFrameAtIndex(size_t index, std::vector<uint8_t>& outBgra, int& outW, int& outH, uint64_t& outActualTs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= count_) return false;

    size_t slot = (start_ + index) % MAX_SNAPSHOTS;
    const auto& f = buffer_[slot];
    if (f.jpegData.empty()) return false;

    outActualTs = f.timestampMs;
    return TileCodec::decodeJpeg(f.jpegData.data(), f.jpegData.size(), outBgra, outW, outH);
}

bool SessionRewindBuffer::getFrameAtOffset(int64_t offsetMs, std::vector<uint8_t>& outBgra, int& outW, int& outH, uint64_t& outActualTs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return false;

    uint64_t oldestTs = buffer_[start_].timestampMs;
    uint64_t newestTs = buffer_[(start_ + count_ - 1) % MAX_SNAPSHOTS].timestampMs;
    int64_t targetTs = static_cast<int64_t>(newestTs) + offsetMs;

    if (targetTs <= static_cast<int64_t>(oldestTs)) {
        size_t slot = start_;
        const auto& f = buffer_[slot];
        if (f.jpegData.empty()) return false;
        outActualTs = f.timestampMs;
        return TileCodec::decodeJpeg(f.jpegData.data(), f.jpegData.size(), outBgra, outW, outH);
    }
    if (targetTs >= static_cast<int64_t>(newestTs)) {
        size_t slot = (start_ + count_ - 1) % MAX_SNAPSHOTS;
        const auto& f = buffer_[slot];
        if (f.jpegData.empty()) return false;
        outActualTs = f.timestampMs;
        return TileCodec::decodeJpeg(f.jpegData.data(), f.jpegData.size(), outBgra, outW, outH);
    }

    size_t bestIdx = count_ - 1;
    int64_t bestDiff = std::abs(static_cast<int64_t>(newestTs) - targetTs);

    size_t low = 0;
    size_t high = count_ - 1;
    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        uint64_t curTs = buffer_[(start_ + mid) % MAX_SNAPSHOTS].timestampMs;
        int64_t diff = std::abs(static_cast<int64_t>(curTs) - targetTs);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = mid;
        }

        if (static_cast<int64_t>(curTs) < targetTs) {
            low = mid + 1;
        } else if (static_cast<int64_t>(curTs) > targetTs) {
            if (mid == 0) break;
            high = mid - 1;
        } else {
            bestIdx = mid;
            break;
        }
    }

    size_t slot = (start_ + bestIdx) % MAX_SNAPSHOTS;
    const auto& f = buffer_[slot];
    if (f.jpegData.empty()) return false;

    outActualTs = f.timestampMs;
    return TileCodec::decodeJpeg(f.jpegData.data(), f.jpegData.size(), outBgra, outW, outH);
}

size_t SessionRewindBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

bool SessionRewindBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

uint64_t SessionRewindBuffer::oldestTimestampMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return 0;
    return buffer_[start_].timestampMs;
}

uint64_t SessionRewindBuffer::newestTimestampMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return 0;
    return buffer_[(start_ + count_ - 1) % MAX_SNAPSHOTS].timestampMs;
}

size_t SessionRewindBuffer::totalMemoryBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return 0;
    size_t total = 0;
    for (size_t i = 0; i < count_; ++i) {
        size_t slot = (start_ + i) % MAX_SNAPSHOTS;
        total += buffer_[slot].jpegData.size();
    }
    return total;
}

void SessionRewindBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    start_ = 0;
    count_ = 0;
    lastPushedTs_ = 0;
    for (auto& f : buffer_) {
        f.jpegData.clear();
        f.timestampMs = 0;
        f.width = 0;
        f.height = 0;
    }
}

} // namespace cppdesk
