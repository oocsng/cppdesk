#pragma once

// The X11 CLIPBOARD selection for text and files: what CppDesk copies reaches other apps, and what they copy
// reaches CppDesk (and clipboard sync with a remote peer). A hidden window on its own X connection serves requests.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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

    // Files for other apps to paste (text/uri-list, and GNOME's list for file managers). `fetch` runs on a worker
    // thread when an app pastes and returns the files' local paths, or nullopt when they could not be fetched.
    using FetchFiles = std::function<std::optional<std::vector<std::string>>()>;
    bool setFiles(FetchFiles fetch);
    // Gives up the clipboard if it still holds the files from setFiles.
    void clearFiles();
    // Local paths of the files another app copied; nullopt when it offers none, or when the files are CppDesk's own.
    std::optional<std::vector<std::string>> getFiles(int timeoutMs = 500);

private:
    X11Clipboard();
    ~X11Clipboard();
    void run();
    std::optional<std::string> request(unsigned long target, int timeoutMs);

    Display* dpy_ = nullptr;
    unsigned long win_ = 0;
    unsigned long clip_ = 0, utf8_ = 0, targets_ = 0, text_ = 0, incr_ = 0, prop_ = 0, uriList_ = 0, gnomeFiles_ = 0;

    std::mutex getMutex_;   // serializes getText
    std::mutex m_;
    std::condition_variable cv_;
    std::string ours_;
    FetchFiles files_;   // set while CppDesk offers files instead of text
    bool owned_ = false;
    bool waitingReply_ = false;
    unsigned long pendingTarget_ = 0;
    bool replied_ = false;
    std::optional<std::string> reply_;
    bool incoming_ = false;      // the owner answered getText with INCR: its text arrives in pieces
    std::string incomingText_;

    // Text larger than one X request goes to other apps in pieces (INCR); only run() touches these.
    struct Outgoing {
        unsigned long requestor = 0, property = 0, type = 0;
        std::string text;
        size_t sent = 0;
        std::chrono::steady_clock::time_point lastActivity;
    };
    std::vector<Outgoing> outgoing_;

    struct PendingFetch {
        unsigned long requestor = 0;
        unsigned long selection = 0;
        unsigned long target = 0;
        unsigned long property = 0;
        unsigned long time = 0;
        FetchFiles files;
    };
    void runFetchWorker();
    std::mutex fetchMutex_;
    std::condition_variable fetchCv_;
    std::deque<PendingFetch> fetchQueue_;

    std::atomic<bool> stop_{false};
    std::thread fetchWorker_;
    std::thread thread_;
};

} // namespace cppdesk
