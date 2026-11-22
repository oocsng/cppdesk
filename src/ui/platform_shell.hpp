#pragma once

// What the platform-neutral screens need from the platform they run on. Each platform's window code
// implements this; the screens never call the operating system directly. No platform headers.

#include "canvas.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cppdesk::ui {

// Features that only some platforms have. Screens hide the controls for anything the shell lacks.
enum class Capability {
    WindowsService,      // install/start/stop the background service from settings
    UrlProtocol,         // register the cppdesk:// URL scheme with the OS (Windows registry)
    Elevation,           // relaunch with administrator rights
    DesktopShortcut,     // create a desktop shortcut for a desk
    ActionCenterToasts,  // Windows Action Center notifications toggle
    TrayIcon,            // minimize to the tray / menu bar
    HostPermissions,     // the OS asks the user to allow screen sharing and control (macOS)
    AutoUpdate,          // download and install a new version from inside the app (Windows)
};

// Permissions a host needs before it can be seen and controlled. Platforms without them report everything allowed.
enum class HostPermission { ScreenRecording, Accessibility };
struct HostPermissionStatus {
    bool screenRecording = true;
    bool accessibility = true;
};

enum class CursorShape { Arrow, Hand, IBeam, ResizeH, ResizeV };

enum class WindowPresence : uint8_t { Shown, Minimized, Hidden };   // Hidden = in the tray

// Background service control (only where Capability::WindowsService is available).
enum class ServiceState { NotInstalled, Stopped, Running, Pending };
enum class ServiceAction { Install, Uninstall, Start, Stop };

class PlatformShell {
public:
    virtual ~PlatformShell() = default;

    // Ask for another frame to be drawn soon.
    virtual void invalidate() = 0;
    virtual void setCursor(CursorShape shape) = 0;
    virtual float dpiScale() const = 0;
    // Milliseconds from a monotonic clock, for UI timing.
    virtual uint64_t nowMs() const = 0;
    virtual void setTextInputActive(bool active) = 0;

    // Dialogs and launching. Paths are UTF-8. `chosen` runs on the UI thread once the dialog closes, with nullopt
    // when the user cancels; it may run before the call returns, or later while the window keeps drawing.
    // filterName/filterPattern describe one file type, e.g. "AVI Session Recordings" / "*.avi".
    using ChosenPath = std::function<void(std::optional<std::string>)>;
    virtual void chooseOpenFile(const std::string& title, const std::string& filterName,
                                const std::string& filterPattern, ChosenPath chosen) = 0;
    virtual void chooseFolder(const std::string& title, ChosenPath chosen) = 0;
    virtual void openUrl(const std::string& url) = 0;
    // Start a fresh copy of the app and close this one.
    virtual void relaunchApp() = 0;

    // How often to draw while idle-animating (the session view asks for a faster rate).
    virtual void setFrameIntervalMs(unsigned ms) = 0;
    // Switch the window to or from borderless fullscreen; false if the platform refused.
    virtual bool setFullscreen(bool on) = 0;
    // Match the window frame (title bar) to the dark or light theme.
    virtual void setDarkTitleBar(bool dark) = 0;
    // Close the app.
    virtual void quitApp() = 0;
    // Relaunch with administrator rights; true if the new instance started (this one should then quit).
    virtual bool relaunchElevated() = 0;
    virtual void playAlertSound() = 0;
    // True when the app window is the active, non-minimized window.
    virtual bool isWindowForeground() const = 0;
    // The OS asks apps to cut animation (Windows "Animation effects" off, macOS Reduce motion, GNOME animations off).
    virtual bool systemPrefersReducedMotion() const { return false; }
    // Raise the app window and give it focus.
    virtual void bringToFront() = 0;

    // Screen pixels, top-left origin, right/bottom exclusive.
    virtual std::optional<Point> cursorScreenPos() const = 0;   // nullopt: platform can't say (Wayland) -> Glide off
    virtual Rect desktopBounds() const = 0;                     // bounding box of all monitors
    virtual Rect monitorBoundsAt(Point p) const = 0;            // the monitor containing p (nearest if none)
    virtual void warpCursor(Point p) = 0;
    virtual void moveWindowToMonitorAt(Point p) = 0;            // before setFullscreen(true)
    virtual WindowPresence windowPresence() const = 0;
    virtual void setWindowPresence(WindowPresence p) = 0;

    // Keep receiving mouse events while a button is held outside the window (scrubber drags).
    virtual void setMouseCapture(bool capture) = 0;
    // Current state of a key or mouse button, by Windows virtual-key code (VK_LBUTTON, VK_CONTROL, ...).
    virtual bool isKeyDown(uint16_t vk) const = 0;
    // Ask for a mouse-leave notification once the pointer exits the window.
    virtual void trackMouseLeave() = 0;

    // The app icon as premultiplied BGRA; false if unavailable.
    virtual bool loadAppIcon(std::vector<uint8_t>& bgra, int& width, int& height) = 0;

    // Updates. Results come back to the UI through the shell's own message path.
    virtual void checkForUpdates(bool manual) = 0;
    // Downloads and verifies the update, then restarts into it. `done` runs on a worker thread with
    // ok == false and a message on failure, or ok == true just before the app restarts.
    virtual void startUpdateDownload(const std::string& url, uint64_t sizeBytes, const std::string& sha256,
                                     std::function<void(bool ok, const std::string& message)> done) = 0;

    virtual ServiceState serviceState() const = 0;
    virtual bool runServiceAction(ServiceAction action) = 0;

    virtual bool has(Capability c) const = 0;

    // Host permissions (only where Capability::HostPermissions is available). The defaults suit every platform
    // that has none.
    virtual HostPermissionStatus hostPermissions() const { return {}; }
    // Asks the OS to show its permission prompt (or does nothing if it cannot).
    virtual void requestHostPermission(HostPermission) {}
    // Opens the system settings page where the user turns the permission on.
    virtual void openHostPermissionSettings(HostPermission) {}
};

} // namespace cppdesk::ui
