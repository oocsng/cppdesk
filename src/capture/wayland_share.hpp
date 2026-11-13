#pragma once

// One screen share on a Wayland desktop for the length of a host session: the permission dialog, the video from
// PipeWire and the input sent back through the portal. The capturer and the input injector both talk to it.
// The parts that touch GLib and PipeWire live in wayland_portal.* and wayland_pipewire.* (Linux only).

#include "latest_frame.hpp"
#include "wayland_helpers.hpp"
#include "wayland_portal.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace cppdesk::wayland {

class WaylandShare {
public:
    // Where the video goes once the desktop has allowed the share (PipeWire, or a stand-in in tests).
    struct FrameSource {
        virtual ~FrameSource() = default;
        virtual bool available() const { return true; }    // false when the video part is missing on this machine
        virtual bool start(int fd, uint32_t nodeId) = 0;   // takes over the descriptor
        virtual void stop() = 0;
        virtual std::shared_ptr<LatestFrame> sink() = 0;
        virtual bool broken() const { return false; }      // true when the video stopped working
    };

    WaylandShare(PortalOptions opts, std::unique_ptr<FrameSource> source);
    ~WaylandShare();
    WaylandShare(const WaylandShare&) = delete;
    WaylandShare& operator=(const WaylandShare&) = delete;

    void begin();                    // asks the desktop (once); returns at once
    ShareState state() const;
    ShareNotice notice() const;
    // The newest frame as packed BGRA, or false while there is none (waiting for the person, refused, ended).
    bool latestFrame(std::vector<uint8_t>& bgra, int& width, int& height);
    // Only when there was no portal to ask (so no consent exists to honour) may the X11 way be used instead.
    bool x11FallbackAllowed() const { return state() == ShareState::Unavailable; }
    // True once per session when the desktop gave its answer; `token` is the restore token to keep ("" = none).
    bool takeTokenChange(std::string& token);

    // Input from the peer; each returns whether it was sent (false unless the share runs with control allowed).
    bool pointerMove(float normX, float normY);
    bool pointerButton(MouseButtonId button, bool down, float normX, float normY);
    bool wheel(int32_t vertical, int32_t horizontal);
    bool key(uint16_t vk, bool down);
    void releaseAll();               // lets go of every key and button this share pressed
    void close();                    // idempotent; ends the session and withdraws a dialog still on screen

    // The share the input injector sends to: the current session's, or null.
    static std::shared_ptr<WaylandShare> active();
    static void setActive(const std::shared_ptr<WaylandShare>& share);
    // A share for a new session (PipeWire video); tests substitute their own.
    static std::shared_ptr<WaylandShare> make(const std::string& restoreToken);
    static void setFactoryForTest(std::function<std::shared_ptr<WaylandShare>(const std::string&)> factory);

private:
    bool controlLive();              // caller holds mu_
    bool resolveStream();            // caller holds mu_
    void fail(ShareState state, const std::string& reason);   // caller holds mu_

    PortalOptions opts_;
    PortalSession portal_;
    std::unique_ptr<FrameSource> source_;

    mutable std::mutex mu_;
    bool begun_ = false, closed_ = false, sourceStarted_ = false, haveStream_ = false;
    ShareState override_ = ShareState::Idle;   // set when the share ended for a reason of its own (video unusable)
    std::string overrideReason_;
    StreamInfo stream_;
    int frameW_ = 0, frameH_ = 0;
    bool gotFrame_ = false;
    std::chrono::steady_clock::time_point sourceStartedAt_;
    bool tokenChange_ = false;
    std::string newToken_;
    std::set<uint32_t> keysDown_, buttonsDown_;
};

} // namespace cppdesk::wayland
