#pragma once

// The X11 CLIPBOARD selection for text: what CppDesk copies reaches other apps, and what they copy reaches
// CppDesk (and clipboard sync with a remote peer). A hidden window on its own X connection serves requests.

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <atomic>

typedef struct _XDisplay Display;

namespace cppdesk {

class X11Clipboard {
public:
    static X11Clipboard& instance();

    // False without an X display (headless); callers then keep the text in the process.
    bool available() const { return dpy_ != nullptr; }
    bool setText(const std::string& utf8);
    // The current clipboard text; nullopt when nobody offers text or the owner did not answer in time.
    std::optional<std::string> getText(int timeoutMs = 500);

private:
    X11Clipboard();
    ~X11Clipboard();
    void run();

    Display* dpy_ = nullptr;
    unsigned long win_ = 0;
    unsigned long clip_ = 0, utf8_ = 0, targets_ = 0, text_ = 0, incr_ = 0, prop_ = 0;

    std::mutex getMutex_;   // serializes getText
    std::mutex m_;
    std::condition_variable cv_;
    std::string ours_;
    bool owned_ = false;
    bool replied_ = false;
    std::optional<std::string> reply_;

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace cppdesk
