#include "x11_clipboard.hpp"

#include "../capture/xlib_threads.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <poll.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <vector>

namespace cppdesk {

namespace {

// UTF-8 to Latin-1; characters outside Latin-1 (and malformed bytes) become '?'.
std::string toLatin1(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            const unsigned cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3Fu);
            out.push_back(cp < 0x100 ? static_cast<char>(cp) : '?');
            i += 2;
        } else {
            out.push_back('?');
            const size_t len = (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            i += len;
        }
    }
    return out;
}

// file:///a%20b -> /a b. Lines starting with # are comments; other schemes are skipped.
std::vector<std::string> pathsFromUriList(const std::string& list) {
    std::vector<std::string> paths;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find('\n', pos);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.compare(0, 7, "file://") != 0) continue;
        const size_t slash = line.find('/', 7);   // after an optional host (file://localhost/...)
        if (slash == std::string::npos) continue;
        std::string path;
        for (size_t i = slash; i < line.size(); ++i) {
            if (line[i] == '%' && i + 2 < line.size() && std::isxdigit(static_cast<unsigned char>(line[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(line[i + 2]))) {
                path.push_back(static_cast<char>(std::stoi(line.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                path.push_back(line[i]);
            }
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

std::string uriFromPath(const std::string& path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string uri = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '.' || c == '_' || c == '~') {
            uri.push_back(static_cast<char>(c));
        } else {
            uri.push_back('%');
            uri.push_back(hex[c >> 4]);
            uri.push_back(hex[c & 15]);
        }
    }
    return uri;
}

} // namespace

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
    uriList_ = XInternAtom(dpy_, "text/uri-list", False);
    gnomeFiles_ = XInternAtom(dpy_, "x-special/gnome-copied-files", False);
    XSelectInput(dpy_, win_, PropertyChangeMask);   // INCR replies arrive as property changes
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
        files_ = nullptr;
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

bool X11Clipboard::setFiles(FetchFiles fetch) {
    if (!dpy_) return false;
    {
        std::lock_guard<std::mutex> lock(m_);
        ours_.clear();
        files_ = std::move(fetch);
        owned_ = true;
    }
    XSetSelectionOwner(dpy_, clip_, win_, CurrentTime);
    const bool owner = XGetSelectionOwner(dpy_, clip_) == win_;
    if (!owner) {
        std::lock_guard<std::mutex> lock(m_);
        owned_ = false;
        files_ = nullptr;
    }
    return owner;
}

void X11Clipboard::clearFiles() {
    if (!dpy_) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!owned_ || !files_) return;
        owned_ = false;
        files_ = nullptr;
    }
    if (XGetSelectionOwner(dpy_, clip_) == win_) XSetSelectionOwner(dpy_, clip_, None, CurrentTime);
    XFlush(dpy_);
}

std::optional<std::vector<std::string>> X11Clipboard::getFiles(int timeoutMs) {
    if (!dpy_) return std::nullopt;
    if (XGetSelectionOwner(dpy_, clip_) == win_) return std::nullopt;   // our own text or the peer's files
    auto list = request(uriList_, timeoutMs);
    if (!list) return std::nullopt;
    auto paths = pathsFromUriList(*list);
    if (paths.empty()) return std::nullopt;
    return paths;
}

std::optional<std::string> X11Clipboard::getText(int timeoutMs) {
    if (!dpy_) return std::nullopt;
    return request(utf8_, timeoutMs);
}

std::optional<std::string> X11Clipboard::request(unsigned long target, int timeoutMs) {
    // One request at a time: replies share one property and one reply slot.
    std::lock_guard<std::mutex> one(getMutex_);
    // Ask the server: our SelectionClear may not have been handled yet when another app has just copied.
    const bool ownedNow = XGetSelectionOwner(dpy_, clip_) == win_;
    std::unique_lock<std::mutex> lock(m_);
    if (ownedNow && owned_) return ours_;
    replied_ = false;
    reply_.reset();
    incoming_ = false;
    incomingText_.clear();
    XConvertSelection(dpy_, clip_, target, prop_, win_, CurrentTime);
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
            FetchFiles files;
            bool own = false;
            {
                std::lock_guard<std::mutex> lock(m_);
                own = owned_;
                text = ours_;
                files = files_;
            }
            if (own && files && (req.target == uriList_ || req.target == gnomeFiles_)) {
                // The files are fetched from the peer first, so the answer comes from a worker thread when they are
                // here; the requesting app waits for it.
                std::thread([this, req, property, files = std::move(files)] {
                    const auto paths = files();
                    XEvent late{};
                    late.xselection.type = SelectionNotify;
                    late.xselection.requestor = req.requestor;
                    late.xselection.selection = req.selection;
                    late.xselection.target = req.target;
                    late.xselection.time = req.time;
                    late.xselection.property = None;
                    if (paths) {
                        const bool gnome = req.target == gnomeFiles_;
                        std::string body = gnome ? "copy" : "";
                        for (const auto& p : *paths) body += gnome ? "\n" + uriFromPath(p) : uriFromPath(p) + "\r\n";
                        XChangeProperty(dpy_, req.requestor, property, req.target, 8, PropModeReplace,
                                        reinterpret_cast<const unsigned char*>(body.data()), static_cast<int>(body.size()));
                        late.xselection.property = property;
                    }
                    XSendEvent(dpy_, req.requestor, False, NoEventMask, &late);
                    XFlush(dpy_);
                }).detach();
                continue;
            }
            const bool fits = text.size() + 1024 < static_cast<size_t>(XMaxRequestSize(dpy_)) * 4;
            if (own && files && req.target == targets_) {
                const Atom offered[] = { static_cast<Atom>(targets_), static_cast<Atom>(uriList_),
                                         static_cast<Atom>(gnomeFiles_) };
                XChangeProperty(dpy_, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(offered), 3);
                reply.xselection.property = property;
            } else if (own && !files && req.target == targets_) {
                const Atom offered[] = { static_cast<Atom>(targets_), static_cast<Atom>(utf8_), XA_STRING,
                                         static_cast<Atom>(text_) };
                XChangeProperty(dpy_, req.requestor, property, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(offered), 4);
                reply.xselection.property = property;
            } else if (own && !files && (req.target == utf8_ || req.target == text_ || req.target == XA_STRING)) {
                if (req.target == XA_STRING) text = toLatin1(text);   // STRING is Latin-1 by definition
                const Atom type = req.target == XA_STRING ? XA_STRING : static_cast<Atom>(utf8_);
                if (fits) {
                    XChangeProperty(dpy_, req.requestor, property, type, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
                } else {
                    // INCR: announce the size; each time the requestor deletes the property, the next piece follows.
                    XSelectInput(dpy_, req.requestor, PropertyChangeMask);
                    const long size = static_cast<long>(text.size());
                    XChangeProperty(dpy_, req.requestor, property, incr_, 32, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(&size), 1);
                    std::erase_if(outgoing_, [&](const Outgoing& o) {
                        return o.requestor == req.requestor && o.property == property;
                    });
                    outgoing_.push_back({ req.requestor, property, type, std::move(text), 0 });
                }
                reply.xselection.property = property;
            }
            XSendEvent(dpy_, req.requestor, False, NoEventMask, &reply);
            XFlush(dpy_);
        } else if (ev.type == PropertyNotify && ev.xproperty.state == PropertyDelete) {
            // A requestor took the last piece of an INCR transfer: send the next one, or an empty one to finish.
            auto it = std::find_if(outgoing_.begin(), outgoing_.end(), [&](const Outgoing& o) {
                return o.requestor == ev.xproperty.window && o.property == ev.xproperty.atom;
            });
            if (it != outgoing_.end()) {
                const size_t piece = std::min<size_t>(it->text.size() - it->sent,
                                                      static_cast<size_t>(XMaxRequestSize(dpy_)) * 4 - 1024);
                XChangeProperty(dpy_, it->requestor, it->property, it->type, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char*>(it->text.data() + it->sent),
                                static_cast<int>(piece));
                it->sent += piece;
                if (piece == 0) {
                    XSelectInput(dpy_, it->requestor, NoEventMask);
                    outgoing_.erase(it);
                }
                XFlush(dpy_);
            }
        } else if (ev.type == PropertyNotify && ev.xproperty.window == win_ && ev.xproperty.atom == prop_ &&
                   ev.xproperty.state == PropertyNewValue) {
            // The next piece of an INCR reply to getText; an empty piece ends it.
            std::unique_lock<std::mutex> lock(m_);
            if (!incoming_) continue;
            Atom type = 0;
            int format = 0;
            unsigned long count = 0, after = 0;
            unsigned char* data = nullptr;
            const bool ok = XGetWindowProperty(dpy_, win_, prop_, 0, 1 << 24, True, AnyPropertyType, &type, &format,
                                               &count, &after, &data) == Success;
            if (ok && data && format == 8) incomingText_.append(reinterpret_cast<char*>(data), count);
            if (data) XFree(data);
            if (ok && type == None) continue;   // a change for a property already read, not a piece
            if (!ok || count == 0) {
                incoming_ = false;
                if (ok) reply_ = std::move(incomingText_);
                incomingText_.clear();
                replied_ = true;
                lock.unlock();
                cv_.notify_all();
            }
        } else if (ev.type == SelectionClear) {
            std::lock_guard<std::mutex> lock(m_);
            owned_ = false;
            files_ = nullptr;
        } else if (ev.type == SelectionNotify) {
            std::optional<std::string> text;
            bool incr = false;
            if (ev.xselection.property != None) {
                Atom type = 0;
                int format = 0;
                unsigned long count = 0, after = 0;
                unsigned char* data = nullptr;
                // Deleting the property also tells an INCR owner to send its first piece.
                if (XGetWindowProperty(dpy_, win_, prop_, 0, 1 << 24, True, AnyPropertyType, &type, &format, &count,
                                       &after, &data) == Success) {
                    incr = type == incr_;
                    if (!incr && format == 8 && data) text.emplace(reinterpret_cast<char*>(data), count);
                }
                if (data) XFree(data);
            }
            if (incr) {
                std::lock_guard<std::mutex> lock(m_);
                incoming_ = true;
                incomingText_.clear();
                continue;
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
