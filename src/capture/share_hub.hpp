#pragma once

// What the host session thread tells the rest of the app about screen sharing on Wayland: the notice to show and
// the restore token to keep. Written by the host session thread, read by the UI thread.

#include "wayland_helpers.hpp"

#include <mutex>
#include <string>

namespace cppdesk::wayland {

class ShareHub {
public:
    void setNotice(const ShareNotice& n) {
        std::lock_guard<std::mutex> lk(mu_);
        notice_ = n;
    }
    ShareNotice notice() const {
        std::lock_guard<std::mutex> lk(mu_);
        return notice_;
    }

    // The token kept in the settings (set once at start); the next session sends it to the desktop.
    void setRestoreToken(const std::string& token) {
        std::lock_guard<std::mutex> lk(mu_);
        token_ = saved_ = token;
        pending_ = false;
    }
    std::string restoreToken() const {
        std::lock_guard<std::mutex> lk(mu_);
        return token_;
    }

    // A session ended up with this token ("" when the desktop gave none, which makes the old one stale).
    void pushToken(const std::string& token) {
        std::lock_guard<std::mutex> lk(mu_);
        token_ = token;
        pending_ = token_ != saved_;
    }

    // True once per change that the settings do not have yet; `token` is the new value (empty clears it).
    bool takeTokenForSave(std::string& token) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!pending_) return false;
        token = saved_ = token_;
        pending_ = false;
        return true;
    }

private:
    mutable std::mutex mu_;
    ShareNotice notice_;
    std::string token_, saved_;
    bool pending_ = false;
};

} // namespace cppdesk::wayland
