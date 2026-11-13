#pragma once

// Asks the desktop for permission to share the screen and to send mouse and keyboard input (the xdg-desktop-portal
// RemoteDesktop and ScreenCast interfaces over D-Bus). Linux only; all D-Bus work runs on the session's own thread.

#include "wayland_helpers.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cppdesk::wayland {

struct PortalOptions {
    std::string restoreToken;        // from an earlier session; sent only where the portal supports it
    int startTimeoutMs = 120000;     // how long to wait for the person to answer the desktop's dialog
    std::string busAddress;          // tests: a private bus; empty = the user's session bus
};

struct PortalResult {
    ShareState state = ShareState::Idle;
    std::string reason;
    std::vector<StreamInfo> streams;
    bool inputGranted = false;
    std::string newRestoreToken;     // empty when the portal gave none (the stored one is then stale)
};

class PortalSession {
public:
    PortalSession();
    ~PortalSession();                // closes the session
    PortalSession(const PortalSession&) = delete;
    PortalSession& operator=(const PortalSession&) = delete;

    void startAsync(const PortalOptions& opts);   // once; returns at once, the work runs on the session's thread
    ShareState state() const;
    ShareNotice notice() const;
    bool result(PortalResult& out) const;         // true once the share is running or has ended
    int pipeWireFd();                              // the video connection, handed over once (the caller owns it); -1 if none

    // Input; each returns whether it was sent. False unless the share runs with that device allowed.
    bool pointerMotionAbsolute(uint32_t node, double x, double y);
    bool pointerButton(uint32_t evdevButton, bool down);
    bool pointerAxisDiscrete(int axis /* 0 vertical, 1 horizontal; positive steps scroll down/right */, int steps);
    bool keyboardKeysym(uint32_t keysym, bool down);

    void close();                                  // idempotent; a dialog still open on screen goes away

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cppdesk::wayland
