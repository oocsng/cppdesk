#pragma once

// Receiving file drops from other X apps (the XDND protocol, version 5): file managers offer text/uri-list.

#include <X11/Xlib.h>

#include <optional>
#include <string>
#include <vector>

namespace cppdesk {

class XdndTarget {
public:
    struct DropResult {
        std::vector<std::string> paths;
        int x = 0, y = 0;   // window pixels
    };

    XdndTarget(Display* dpy, Window win);   // marks the window XdndAware

    // True when the event was an XDND message (it is then fully handled).
    bool handleClientMessage(const XEvent& ev);
    // The dropped paths once the source has sent the data; nullopt for selections that are not ours.
    std::optional<DropResult> handleSelectionNotify(const XEvent& ev);

private:
    void sendToSource(Atom type, long l1, long l2);

    Display* dpy_;
    Window win_;
    Atom enter_, position_, status_, leave_, drop_, finished_, selection_, uriList_, actionCopy_, typeList_, prop_;
    Window source_ = 0;
    bool offersUris_ = false;
    int x_ = 0, y_ = 0;
};

} // namespace cppdesk
