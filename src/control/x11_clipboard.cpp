#include "x11_clipboard.hpp"

#include "../capture/xlib_threads.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <poll.h>

#include <chrono>

namespace cppdesk {

X11Clipboard& X11Clipboard::instance() {
    static X11Clipboard clipboard;
    return clipboard;
}

X11Clipboard::X11Clipboard() {
    ensureXlibThreads();
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) return;
    win_ = XCreateSimpleWindow(dpy_, DefaultRootWindow(dpy_), 0, 0, 1, 1, 0, 0, 0);
    clip_ = XInternAtom(dpy_, "CLIPBOARD", False);
    utf8_ = XInternAtom(dpy_, "UTF8_STRING", False);
    targets_ = XInternAtom(dpy_, "TARGETS", False);
    text_ = XInternAtom(dpy_, "TEXT", False);
    incr_ = XInternAtom(dpy_, "INCR", False);
    prop_ = XInternAtom(dpy_, "CPPDESK_CLIPBOARD", False);
    thread_ = std::thread([this] { run(); });
}

X11Clipboard::~X11Clipboard() {
    if (!dpy_) return;
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    XDestroyWindow(dpy_, win_);
    XCloseDisplay(dpy_);
}

bool X11Clipboard::setText(const std::string& utf8) {
    if (!dpy_) return false;
    {
        std::lock_guard<std::mutex> lock(m_);
        ours_ = utf8;
        owned_ = true;
    }
    XSetSelectionOwner(dpy_, clip_, win_, CurrentTime);
    const bool owner = XGetSelectionOwner(dpy_, clip_) == win_;
    if (!owner) {
        std::lock_guard<std::mutex> lock(m_);
        owned_ = false;
    }
    return owner;
}

std::optional<std::string> X11Clipboard::getText(int timeoutMs) {
    if (!dpy_) return std::nullopt;
    std::unique_lock<std::mutex> lock(m_);
    if (owned_) return ours_;
    replied_ = false;
    reply_.reset();
    XConvertSelection(dpy_, clip_, utf8_, prop_, win_, CurrentTime);
    XFlush(dpy_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return replied_; })) return std::nullopt;
    return reply_;
}

void X11Clipboard::run() {
    pollfd pfd{ ConnectionNumber(dpy_), POLLIN, 0 };
    while (!stop_.load()) {
        if (XPending(dpy_) == 0) {
            poll(&pfd, 1, 100);
            continue;
        }
        XEvent ev;
        XNextEvent(dpy_, &ev);
        if (ev.type == SelectionRequest) {
            const XSelectionRequestEvent& req = ev.xselectionrequest;
            XEvent reply{};
            reply.xselection.type = SelectionNotify;
            reply.xselection.requestor = req.requestor;
            reply.xselection.selection = req.selection;
            reply.xselection.target = req.target;
            reply.xselection.time = req.time;
            reply.xselection.property = None;
            const Atom property = req.property != None ? req.property : req.target;   // obsolete clients
            std::string text;
            bool own = false;
            {
                std::lock_guard<std::mutex> lock(m_);
                own = owned_;
                text = ours_;
            }
            // shortcut: no INCR transfers, so text larger than one X request is refused; add INCR when someone
            // copies more than ~256 KB.
            const bool fits = text.size() + 1024 < static_cast<size_t>(XMaxRequestSize(dpy_)) * 4;
            if (own && req.target == targets_) {
                const Atom offered[] = { static_cast<Atom>(targets_), static_cast<Atom>(utf8_), XA_STRING,
                                         static_cast<Atom>(text_) };
                XChangeProperty(dpy_, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(offered), 4);
                reply.xselection.property = property;
            } else if (own && fits && (req.target == utf8_ || req.target == text_ || req.target == XA_STRING)) {
                XChangeProperty(dpy_, req.requestor, property, req.target == XA_STRING ? XA_STRING : utf8_, 8,
                                PropModeReplace, reinterpret_cast<const unsigned char*>(text.data()),
                                static_cast<int>(text.size()));
                reply.xselection.property = property;
            }
            XSendEvent(dpy_, req.requestor, False, NoEventMask, &reply);
            XFlush(dpy_);
        } else if (ev.type == SelectionClear) {
            std::lock_guard<std::mutex> lock(m_);
            owned_ = false;
        } else if (ev.type == SelectionNotify) {
            std::optional<std::string> text;
            if (ev.xselection.property != None) {
                Atom type = 0;
                int format = 0;
                unsigned long count = 0, after = 0;
                unsigned char* data = nullptr;
                if (XGetWindowProperty(dpy_, win_, prop_, 0, 1 << 24, True, AnyPropertyType, &type, &format, &count,
                                       &after, &data) == Success && type != incr_ && format == 8 && data) {
                    text.emplace(reinterpret_cast<char*>(data), count);
                }
                if (data) XFree(data);
            }
            {
                std::lock_guard<std::mutex> lock(m_);
                reply_ = std::move(text);
                replied_ = true;
            }
            cv_.notify_all();
        }
    }
}

} // namespace cppdesk
