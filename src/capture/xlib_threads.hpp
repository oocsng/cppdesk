#pragma once

// Xlib is not thread-safe until XInitThreads() has run, and it must run before any other Xlib call.
// Capture, input injection and the window all use X from different threads, so each one calls this
// before it first opens a display.

#if __has_include(<X11/Xlib.h>)
#include <X11/Xlib.h>
#include <cstdio>
#include <mutex>

inline void ensureXlibThreads() {
    static std::once_flag once;
    std::call_once(once, []() {
        XInitThreads();
        // Xlib's default handler exits the process. Requests aimed at other apps' windows (a clipboard requester,
        // a drag source, a tray panel) fail whenever those windows vanish first; log and carry on instead.
        XSetErrorHandler([](Display*, XErrorEvent* e) {
            char text[128] = {};
            XGetErrorText(e->display, e->error_code, text, sizeof(text));
            std::fprintf(stderr, "CppDesk: X error ignored: %s (request %d)\n", text, e->request_code);
            return 0;
        });
    });
}
#else
inline void ensureXlibThreads() {}
#endif
