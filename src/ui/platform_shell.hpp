#pragma once

// What the platform-neutral screens need from the platform they run on. Each platform's window code
// implements this; the screens never call the operating system directly. No platform headers.

#include <cstdint>
#include <optional>
#include <string>

namespace cppdesk::ui {

// Features that only some platforms have. Screens hide the controls for anything the shell lacks.
enum class Capability {
    WindowsService,      // install/start/stop the background service from settings
    Elevation,           // relaunch with administrator rights
    DesktopShortcut,     // create a desktop shortcut for a desk
    ActionCenterToasts,  // Windows Action Center notifications toggle
    TrayIcon,            // minimize to the tray / menu bar
    NativeNotifications, // OS notifications for incoming requests
};

enum class CursorShape { Arrow, Hand, IBeam, ResizeH, ResizeV };

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

    // Dialogs and launching. Paths are UTF-8; nullopt when the user cancels.
    // filterName/filterPattern describe one file type, e.g. "AVI Session Recordings" / "*.avi".
    virtual std::optional<std::string> chooseOpenFile(const std::string& title, const std::string& filterName,
                                                      const std::string& filterPattern) = 0;
    virtual std::optional<std::string> chooseFolder(const std::string& title) = 0;
    virtual void openUrl(const std::string& url) = 0;
    virtual void revealPath(const std::string& path) = 0;
    // Start a fresh copy of the app and close this one.
    virtual void relaunchApp() = 0;

    virtual void showNotification(const std::string& title, const std::string& body) = 0;
    virtual void setTrayStatus(const std::string& tooltip) = 0;

    virtual bool has(Capability c) const = 0;
};

} // namespace cppdesk::ui
