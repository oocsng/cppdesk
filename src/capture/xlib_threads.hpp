#pragma once

// Xlib is not thread-safe until XInitThreads() has run, and it must run before any other Xlib call.
// Capture, input injection and the window all use X from different threads, so each one calls this
// before it first opens a display.

#if __has_include(<X11/Xlib.h>)
#include <X11/Xlib.h>
#include <mutex>

inline void ensureXlibThreads() {
    static std::once_flag once;
    std::call_once(once, []() { XInitThreads(); });
}
#else
inline void ensureXlibThreads() {}
#endif
