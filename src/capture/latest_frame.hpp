#pragma once

// The newest frame from a capture thread, handed to whoever asks. The producer calls accept(true) when it starts and
// accept(false) before it stops, so a frame that is still in flight at that moment is dropped instead of stored.

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace cppdesk {

class LatestFrame {
public:
    void accept(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        accepting_ = on;
    }

    // Stores a w x h BGRA frame; `fill` writes its w * h * 4 bytes straight into the slot (no second copy).
    template <class Fill>
    void write(int w, int h, Fill&& fill) {
        if (w <= 0 || h <= 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (!accepting_) return;
        data_.resize(static_cast<size_t>(w) * h * 4);
        fill(data_.data());
        w_ = w;
        h_ = h;
        ++seq_;
    }

    void push(const uint8_t* bgra, int w, int h) {
        write(w, h, [&](uint8_t* dst) { std::memcpy(dst, bgra, static_cast<size_t>(w) * h * 4); });
    }

    // Copies the newest frame; isNew says whether it is a different one than the last copy returned.
    bool copy(std::vector<uint8_t>& bgra, int& w, int& h, bool& isNew) {
        std::lock_guard<std::mutex> lk(mu_);
        if (seq_ == 0) return false;
        bgra = data_;
        w = w_;
        h = h_;
        isNew = seq_ != seen_;
        seen_ = seq_;
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        data_.clear();
        w_ = h_ = 0;
        seq_ = seen_ = 0;
    }

private:
    std::mutex mu_;
    bool accepting_ = false;
    std::vector<uint8_t> data_;
    int w_ = 0, h_ = 0;
    uint64_t seq_ = 0, seen_ = 0;
};

} // namespace cppdesk
