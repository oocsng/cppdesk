#pragma once

// Caps concurrent connections overall and per key (the peer IP). The relay spawns a thread per connection,
// so without a cap one address can exhaust threads and file handles. No platform headers.

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cppdesk {

class ConnectionLimiter {
public:
    ConnectionLimiter(size_t maxTotal, size_t maxPerKey) : maxTotal_(maxTotal), maxPerKey_(maxPerKey) {}

    // True and counted when both caps allow it; every true must be paired with release().
    bool tryAcquire(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t& n = perKey_[key];
        if (total_ >= maxTotal_ || n >= maxPerKey_) {
            if (n == 0) perKey_.erase(key);
            return false;
        }
        ++n;
        ++total_;
        return true;
    }

    // Releasing a key that holds nothing is a no-op.
    void release(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = perKey_.find(key);
        if (it == perKey_.end()) return;
        if (--it->second == 0) perKey_.erase(it);
        --total_;
    }

    size_t total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

private:
    const size_t maxTotal_;
    const size_t maxPerKey_;
    mutable std::mutex mutex_;
    size_t total_ = 0;
    std::unordered_map<std::string, size_t> perKey_;
};

} // namespace cppdesk
