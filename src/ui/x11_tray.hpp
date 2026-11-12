#pragma once

// A system tray icon over the freedesktop XEmbed tray protocol, with a small popup menu drawn by CairoCanvas.
// Panels without a tray (GNOME without the AppIndicator extension) leave it undocked; the app then never hides.

#include <X11/Xlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cppdesk {

class X11Tray {
public:
    enum class Action { Nothing, Restore, ToggleMute, Quit };
    struct MenuInfo {
        std::string status;   // first, greyed line
        bool muted = false;
    };

    X11Tray(Display* dpy, int screen, float scale, std::vector<uint8_t> iconBgra, int iconW, int iconH);
    ~X11Tray();

    bool docked() const { return icon_ != 0; }
    void setTooltip(const std::string& text);
    // Handles tray, manager and menu events. consumed is set when the event belonged to the tray; the returned
    // action is what the user picked. lostTray is set when the panel went away.
    Action handleEvent(const XEvent& ev, const MenuInfo& info, bool& consumed, bool& lostTray);

private:
    void dock();
    void undock();
    void drawIcon();
    void openMenu(int rootX, int rootY, const MenuInfo& info);
    void closeMenu();
    void drawMenu();
    int itemAt(int y) const;

    Display* dpy_;
    int screen_;
    float scale_;
    std::vector<uint8_t> iconBgra_;
    int iconW_, iconH_;
    Atom selection_, opcode_, manager_, visualAtom_, xembedInfo_;
    Window managerWin_ = 0;
    Window icon_ = 0;
    Visual* iconVisual_ = nullptr;
    int iconDepth_ = 0;
    Colormap iconColormap_ = 0;
    int iconSize_ = 24;
    std::string tooltip_;

    Window menu_ = 0;
    int menuW_ = 0, menuH_ = 0;
    int hover_ = -1;
    std::vector<std::string> labels_;   // status line, then the three items
};

} // namespace cppdesk
