#pragma once

// Plain helpers for screen sharing on a Wayland desktop (no GLib, no PipeWire, no Linux headers), so every
// platform's tests can check them.

#include "../control/input_injector.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace cppdesk::wayland {

// True in a Wayland desktop session: XDG_SESSION_TYPE is "wayland", or WAYLAND_DISPLAY is set while DISPLAY is not.
// CPPDESK_FORCE_X11 (set and not "0") always gives false. `getenv` is injectable for tests.
bool isWaylandSession(const std::function<const char*(const char*)>& getenv);

enum class PixelFormat { BGRx, BGRA, RGBx, RGBA, Unsupported };

// Packs a frame whose rows are `srcStride` bytes apart into tightly packed BGRA (w * h * 4 bytes), alpha 0xFF.
// False (and nothing written) for an unsupported format, a non-positive size or a stride shorter than a row.
bool convertToBgra(PixelFormat f, const uint8_t* src, size_t srcStride, int w, int h, uint8_t* dst);

// One shared screen. x/y are the stream's position in the compositor space, logicalW/H its size in that space
// (0 when the portal did not say).
struct StreamInfo { uint32_t nodeId = 0; int x = 0, y = 0, logicalW = 0, logicalH = 0; };
struct StreamPoint { uint32_t nodeId; double x, y; };

// A normalized position (0..1, clamped) as a point inside the stream, in its logical size (the frame size when
// that is unknown), the last pixel reachable. Relative to the stream, so its own x/y offset is not added.
StreamPoint pointForNormalized(float nx, float ny, const StreamInfo& s, int frameW, int frameH);

uint32_t evdevButton(MouseButtonId b);   // Left 0x110, Right 0x111, Middle 0x112
// Wheel units (120 per notch) as notches, keeping the sign; a small non-zero delta still moves one notch.
int wheelSteps(int32_t wheelDelta);

struct PortalPolicy { bool useRemoteDesktop; bool persist; };
// Restore tokens exist only from RemoteDesktop version 2.
PortalPolicy policyForVersions(bool hasRemoteDesktop, uint32_t remoteDesktopVersion);

enum class ShareState { Idle, WaitingForDialog, Running, ViewOnly, Denied, Unavailable, Closed, Failed };
struct ShareNotice { ShareState state = ShareState::Idle; std::string reason; };
struct NoticeText { bool visible = false; std::string title, body; };
// What to tell the person, in plain words; invisible while there is nothing to say (Idle, Running).
NoticeText describeShareNotice(const ShareNotice& n);

// A share that was refused or has ended: the host ends the session instead of leaving the viewer on a blank picture.
bool isShareEnd(ShareState s);
// What the viewer is told when the host ends the session for that reason ("" for any other state).
std::string endMessageForViewer(ShareState s);

} // namespace cppdesk::wayland
