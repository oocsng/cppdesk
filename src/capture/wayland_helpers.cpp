#include "wayland_helpers.hpp"

#include <algorithm>
#include <cstring>

namespace cppdesk::wayland {

bool isWaylandSession(const std::function<const char*(const char*)>& getenv) {
    auto value = [&](const char* name) -> const char* {
        const char* v = getenv(name);
        return (v && *v) ? v : nullptr;
    };
    if (const char* force = value("CPPDESK_FORCE_X11"); force && std::strcmp(force, "0") != 0) return false;
    if (const char* type = value("XDG_SESSION_TYPE")) {
        if (std::strcmp(type, "wayland") == 0) return true;
        if (std::strcmp(type, "x11") == 0) return false;
    }
    return value("WAYLAND_DISPLAY") && !value("DISPLAY");
}

bool convertToBgra(PixelFormat f, const uint8_t* src, size_t srcStride, int w, int h, uint8_t* dst) {
    if (f == PixelFormat::Unsupported || !src || !dst || w <= 0 || h <= 0 || srcStride < static_cast<size_t>(w) * 4)
        return false;
    const bool swap = (f == PixelFormat::RGBx || f == PixelFormat::RGBA);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * srcStride;
        uint8_t* d = dst + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x, s += 4, d += 4) {
            d[0] = swap ? s[2] : s[0];
            d[1] = s[1];
            d[2] = swap ? s[0] : s[2];
            d[3] = 0xFF;
        }
    }
    return true;
}

StreamPoint pointForNormalized(float nx, float ny, const StreamInfo& s, int frameW, int frameH) {
    const int w = s.logicalW > 0 ? s.logicalW : frameW;
    const int h = s.logicalH > 0 ? s.logicalH : frameH;
    auto axis = [](float n, int size) {
        return size > 1 ? static_cast<double>(std::clamp(n, 0.0f, 1.0f)) * (size - 1) : 0.0;
    };
    return {s.nodeId, axis(nx, w), axis(ny, h)};
}

uint32_t evdevButton(MouseButtonId b) {
    switch (b) {
        case MouseButtonId::Left:   return 0x110;
        case MouseButtonId::Right:  return 0x111;
        case MouseButtonId::Middle: return 0x112;
    }
    return 0;
}

int wheelSteps(int32_t wheelDelta) {
    if (wheelDelta == 0) return 0;
    const int steps = wheelDelta / 120;
    return steps != 0 ? steps : (wheelDelta > 0 ? 1 : -1);
}

PortalPolicy policyForVersions(bool hasRemoteDesktop, uint32_t remoteDesktopVersion) {
    return {hasRemoteDesktop, hasRemoteDesktop && remoteDesktopVersion >= 2};
}

NoticeText describeShareNotice(const ShareNotice& n) {
    const std::string& why = n.reason;
    switch (n.state) {
        case ShareState::WaitingForDialog:
            return {true, "Waiting for your permission",
                    "Your desktop will ask you to allow screen sharing. Nothing is shared until you say yes."};
        case ShareState::ViewOnly:
            return {true, "Screen shared, control is off",
                    "The other person can see your screen but cannot control it" +
                        (why.empty() ? std::string(".") : ", because " + why + ".")};
        case ShareState::Denied:
            return {true, "Screen sharing is off",
                    "Screen sharing stays off. " + (why.empty() ? "It was not allowed when your desktop asked." : why)};
        case ShareState::Unavailable:
            return {true, "Whole-screen sharing is not available",
                    "This desktop cannot share the whole screen" + (why.empty() ? std::string() : " (" + why + ")") +
                    ", so only apps that run on X11 are shown. To share everything, log in with an Xorg (X11) session."};
        case ShareState::Closed:
            return {true, "Screen sharing ended", why.empty() ? "Your desktop stopped the sharing." : why};
        case ShareState::Failed:
            return {true, "Screen sharing failed", why.empty() ? "Something went wrong while starting it." : why};
        case ShareState::Idle:
        case ShareState::Running:
            break;
    }
    return {};
}

} // namespace cppdesk::wayland
