#include "wayland_portal.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace cppdesk::wayland {

namespace {

constexpr const char* kDest = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kRemoteDesktop = "org.freedesktop.portal.RemoteDesktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequest = "org.freedesktop.portal.Request";
constexpr const char* kSession = "org.freedesktop.portal.Session";
constexpr int kStepTimeoutMs = 15000;   // every step but the dialog answers at once

constexpr uint32_t kDeviceKeyboard = 1;
constexpr uint32_t kDevicePointer = 2;

GVariant* emptyOptions() { return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0); }

struct CallState {
    bool done = false;
    GVariant* reply = nullptr;
    GUnixFDList* fds = nullptr;
    GError* error = nullptr;
    ~CallState() {
        if (reply) g_variant_unref(reply);
        if (fds) g_object_unref(fds);
        if (error) g_error_free(error);
    }
};

struct Response {
    bool got = false;
    uint32_t code = 2;
    GVariant* results = nullptr;
    Response() = default;
    Response(const Response&) = delete;
    Response& operator=(const Response&) = delete;
    ~Response() { reset(); }
    void reset() {
        got = false;
        code = 2;
        if (results) g_variant_unref(results);
        results = nullptr;
    }
};

}   // namespace

struct PortalSession::Impl {
    PortalOptions opts;
    std::thread worker;
    GMainContext* ctx = nullptr;
    std::atomic<bool> abort{false};
    bool started = false;

    mutable std::mutex mu;               // everything below up to "worker only"
    ShareState state = ShareState::Idle;
    std::string reason;
    PortalResult result;
    bool haveResult = false;
    int fd = -1;
    GDBusConnection* conn = nullptr;     // valid while inputReady
    std::string sessionPath;
    uint32_t devices = 0;
    bool inputReady = false;

    // worker only
    GDBusConnection* wconn = nullptr;
    bool sessionClosed = false;
    guint closedSub = 0;
    std::string pendingRequest;          // the request whose Response we are waiting for
    uint32_t tokenCounter = 0;
    bool hasRemoteDesktop = false;
    uint32_t remoteDesktopVersion = 0;

    ~Impl() { if (ctx) g_main_context_unref(ctx); }

    void publish(ShareState s, const std::string& why) {
        std::lock_guard<std::mutex> lk(mu);
        state = s;
        reason = why;
        result.state = s;
        result.reason = why;
        haveResult = s != ShareState::Idle && s != ShareState::WaitingForDialog;
    }

    // Waits, running the thread's own main context, until done() holds, the share is stopped or closed, or the time is up.
    bool waitFor(const std::function<bool()>& done, int timeoutMs) {
        bool timedOut = false;
        GSource* timer = g_timeout_source_new(static_cast<guint>(timeoutMs));
        g_source_set_callback(timer, [](gpointer p) -> gboolean { *static_cast<bool*>(p) = true; return G_SOURCE_REMOVE; },
                              &timedOut, nullptr);
        g_source_attach(timer, ctx);
        while (!done() && !abort.load() && !sessionClosed && !timedOut) g_main_context_iteration(ctx, TRUE);
        g_source_destroy(timer);
        g_source_unref(timer);
        return done();
    }

    // One D-Bus call. The state is shared with the callback so that giving up early leaves nothing dangling.
    std::shared_ptr<CallState> call(const char* iface, const char* method, GVariant* params) {
        auto state = std::make_shared<CallState>();
        auto* holder = new std::shared_ptr<CallState>(state);
        g_dbus_connection_call_with_unix_fd_list(
            wconn, kDest, kPath, iface, method, params, nullptr, G_DBUS_CALL_FLAGS_NONE, kStepTimeoutMs, nullptr, nullptr,
            [](GObject* src, GAsyncResult* res, gpointer data) {
                auto* h = static_cast<std::shared_ptr<CallState>*>(data);
                CallState& c = **h;
                c.reply = g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(src), &c.fds, res, &c.error);
                c.done = true;
                delete h;
            },
            holder);
        waitFor([&] { return state->done; }, kStepTimeoutMs + 1000);
        return state;
    }

    std::string requestPath(const std::string& token) const {
        std::string sender = g_dbus_connection_get_unique_name(wconn);
        std::string clean;
        for (char c : sender) if (c != ':') clean += (c == '.') ? '_' : c;
        return std::string(kPath) + "/request/" + clean + "/" + token;
    }

    std::string nextToken() { return "cppdesk" + std::to_string(getpid()) + "_" + std::to_string(++tokenCounter); }

    // A portal method that answers through a Response signal. The signal is subscribed before the call goes out.
    // `options` are the method's own options; the handle token is added here and `build` wraps them into the arguments.
    bool request(const char* iface, const char* method, const std::function<GVariant*(GVariant* options)>& build,
                 GVariantBuilder* options, int timeoutMs, Response& out, std::string& failure) {
        const std::string token = nextToken();
        g_variant_builder_add(options, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        const std::string path = requestPath(token);
        guint sub = g_dbus_connection_signal_subscribe(
            wconn, kDest, kRequest, "Response", path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer data) {
                auto* r = static_cast<Response*>(data);
                GVariant* results = nullptr;
                guint32 code = 2;
                g_variant_get(params, "(u@a{sv})", &code, &results);
                if (r->results) g_variant_unref(r->results);
                r->code = code;
                r->results = results;
                r->got = true;
            },
            &out, nullptr);
        pendingRequest = path;
        auto c = call(iface, method, build(g_variant_builder_end(options)));
        bool ok = true;
        if (!c->done || c->error) {
            ok = false;
            failure = c->error ? c->error->message : "The desktop did not answer.";
            if (c->error) pendingRequest.clear();   // the request never existed
        } else if (!waitFor([&] { return out.got; }, timeoutMs)) {
            ok = false;
        }
        g_dbus_connection_signal_unsubscribe(wconn, sub);
        if (ok) pendingRequest.clear();   // otherwise the cleanup closes it, so a dialog still on screen goes away
        return ok;
    }

    // Why a wait ended without an answer: stopped, closed by the desktop, or just too slow.
    void bailUnanswered(const std::string& failure, bool dialog) {
        if (abort.load()) return publish(ShareState::Closed, "Screen sharing was stopped.");
        if (sessionClosed) return publish(ShareState::Closed, "Your desktop ended screen sharing.");
        if (!failure.empty()) return publish(ShareState::Failed, "The desktop could not start screen sharing.");
        publish(ShareState::Failed, dialog ? "Nobody answered the desktop's question in time."
                                           : "The desktop did not answer.");
    }

    bool getVersion(const char* iface, uint32_t& version) {
        auto c = call("org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, "version"));
        if (!c->done || c->error || !c->reply) return false;
        GVariant* v = nullptr;
        g_variant_get(c->reply, "(v)", &v);
        bool ok = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32);
        if (ok) version = g_variant_get_uint32(v);
        if (v) g_variant_unref(v);
        return ok;
    }

    bool connect() {
        GError* err = nullptr;
        std::string address = opts.busAddress;
        if (address.empty()) {
            gchar* a = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &err);
            if (a) { address = a; g_free(a); }
        }
        if (!address.empty())
            wconn = g_dbus_connection_new_for_address_sync(
                address.c_str(),
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, nullptr, &err);
        if (err) g_error_free(err);
        return wconn != nullptr;
    }

    // Everything up to a running share. On any other outcome the state is already published.
    bool setup() {
        publish(ShareState::WaitingForDialog, "");
        if (!connect()) { publish(ShareState::Unavailable, "no desktop message bus was found"); return false; }

        uint32_t screenCastVersion = 0;
        if (!getVersion(kScreenCast, screenCastVersion)) {
            if (abort.load()) { publish(ShareState::Closed, "Screen sharing was stopped."); return false; }
            publish(ShareState::Unavailable, "this desktop has no screen sharing service");
            return false;
        }
        hasRemoteDesktop = getVersion(kRemoteDesktop, remoteDesktopVersion);
        const PortalPolicy policy = policyForVersions(hasRemoteDesktop, remoteDesktopVersion);
        const char* sessionIface = policy.useRemoteDesktop ? kRemoteDesktop : kScreenCast;

        Response r;
        std::string failure;

        // 1. the session
        {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(nextToken().c_str()));
            if (!request(sessionIface, "CreateSession", [](GVariant* o) { return g_variant_new("(@a{sv})", o); }, &b,
                         kStepTimeoutMs, r, failure)) {
                bailUnanswered(failure, false);
                return false;
            }
            const char* handle = nullptr;
            if (r.code != 0 || !r.results || !g_variant_lookup(r.results, "session_handle", "&s", &handle)) {
                publish(ShareState::Failed, "The desktop could not start screen sharing.");
                return false;
            }
            std::lock_guard<std::mutex> lk(mu);
            sessionPath = handle;
        }
        closedSub = g_dbus_connection_signal_subscribe(
            wconn, kDest, kSession, "Closed", sessionPath.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer data) {
                static_cast<Impl*>(data)->sessionClosed = true;
            },
            this, nullptr);

        auto step = [&](const char* iface, const char* method, GVariantBuilder* b, bool isStart) {
            const std::string session = sessionPath;
            r.reset();
            failure.clear();
            if (!request(iface, method,
                         [&](GVariant* o) {
                             return isStart ? g_variant_new("(os@a{sv})", session.c_str(), "", o)
                                            : g_variant_new("(o@a{sv})", session.c_str(), o);
                         },
                         b, isStart ? opts.startTimeoutMs : kStepTimeoutMs, r, failure)) {
                bailUnanswered(failure, isStart);
                return false;
            }
            if (r.code == 1) { publish(ShareState::Denied, "You did not allow it when your desktop asked."); return false; }
            if (r.code != 0) { publish(ShareState::Failed, "The desktop could not start screen sharing."); return false; }
            return true;
        };

        // 2. which devices and which screen
        if (policy.useRemoteDesktop) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(kDeviceKeyboard | kDevicePointer));
            if (policy.persist) {
                g_variant_builder_add(&b, "{sv}", "persist_mode", g_variant_new_uint32(2));
                if (!opts.restoreToken.empty())
                    g_variant_builder_add(&b, "{sv}", "restore_token", g_variant_new_string(opts.restoreToken.c_str()));
            }
            if (!step(kRemoteDesktop, "SelectDevices", &b, false)) return false;
        }
        {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(1));          // monitors
            g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(FALSE));
            g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(2));    // drawn into the picture
            if (!step(kScreenCast, "SelectSources", &b, false)) return false;
        }

        // 3. the desktop's own question
        {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
            if (!step(sessionIface, "Start", &b, true)) return false;
        }

        PortalResult out;
        uint32_t granted = 0;
        if (policy.useRemoteDesktop) g_variant_lookup(r.results, "devices", "u", &granted);
        if (r.results) {
            const char* token = nullptr;
            if (g_variant_lookup(r.results, "restore_token", "&s", &token) && token) out.newRestoreToken = token;
            GVariant* streams = g_variant_lookup_value(r.results, "streams", G_VARIANT_TYPE("a(ua{sv})"));
            if (streams) {
                const gsize n = g_variant_n_children(streams);
                for (gsize i = 0; i < n; ++i) {
                    GVariant* child = g_variant_get_child_value(streams, i);
                    guint32 node = 0;
                    GVariant* props = nullptr;
                    g_variant_get(child, "(u@a{sv})", &node, &props);
                    StreamInfo info;
                    info.nodeId = node;
                    gint32 a = 0, c = 0;
                    if (g_variant_lookup(props, "position", "(ii)", &a, &c)) { info.x = a; info.y = c; }
                    if (g_variant_lookup(props, "size", "(ii)", &a, &c)) { info.logicalW = a; info.logicalH = c; }
                    out.streams.push_back(info);
                    g_variant_unref(props);
                    g_variant_unref(child);
                }
                g_variant_unref(streams);
            }
        }
        if (out.streams.empty()) {
            publish(ShareState::Failed, "The desktop did not share any screen.");
            return false;
        }

        // 4. the video connection
        auto fdCall = call(kScreenCast, "OpenPipeWireRemote", g_variant_new("(o@a{sv})", sessionPath.c_str(), emptyOptions()));
        int fd = -1;
        if (fdCall->done && !fdCall->error && fdCall->reply && fdCall->fds) {
            gint32 index = -1;
            g_variant_get(fdCall->reply, "(h)", &index);
            fd = g_unix_fd_list_get(fdCall->fds, index, nullptr);
        }
        if (fd < 0) {
            if (abort.load() || sessionClosed) bailUnanswered("", false);
            else publish(ShareState::Failed, "The desktop did not hand over the screen.");
            return false;
        }

        const bool control = policy.useRemoteDesktop && (granted & (kDeviceKeyboard | kDevicePointer)) != 0;
        out.inputGranted = control;
        {
            std::lock_guard<std::mutex> lk(mu);
            this->fd = fd;
            devices = granted;
            conn = wconn;
            inputReady = control;
            result.streams = out.streams;
            result.inputGranted = control;
            result.newRestoreToken = out.newRestoreToken;
        }
        publish(control ? ShareState::Running : ShareState::ViewOnly,
                control ? "" : (policy.useRemoteDesktop ? "remote control was not allowed" : "this desktop does not offer remote control"));
        return true;
    }

    void run() {
        g_main_context_push_thread_default(ctx);
        const bool running = setup();
        if (running) {
            while (!abort.load() && !sessionClosed) g_main_context_iteration(ctx, TRUE);
            publish(ShareState::Closed, sessionClosed && !abort.load() ? "Your desktop ended screen sharing."
                                                                         : "Screen sharing was stopped.");
        }
        {   // no more input from here on
            std::lock_guard<std::mutex> lk(mu);
            inputReady = false;
            conn = nullptr;
        }
        if (wconn) {
            if (!pendingRequest.empty())
                g_dbus_connection_call(wconn, kDest, pendingRequest.c_str(), kRequest, "Close", nullptr, nullptr,
                                       G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            if (!sessionPath.empty() && !sessionClosed)
                g_dbus_connection_call(wconn, kDest, sessionPath.c_str(), kSession, "Close", nullptr, nullptr,
                                       G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            if (closedSub) g_dbus_connection_signal_unsubscribe(wconn, closedSub);
            g_dbus_connection_flush_sync(wconn, nullptr, nullptr);
            g_dbus_connection_close_sync(wconn, nullptr, nullptr);
            g_object_unref(wconn);
            wconn = nullptr;
        }
        g_main_context_pop_thread_default(ctx);
    }

    // Input goes out from other threads; a call is only queued, so holding the lock is short.
    bool notify(const char* method, uint32_t needDevice, const std::function<GVariant*(const char* session)>& build) {
        std::lock_guard<std::mutex> lk(mu);
        if (!inputReady || !conn || !(devices & needDevice)) return false;
        g_dbus_connection_call(conn, kDest, kPath, kRemoteDesktop, method, build(sessionPath.c_str()), nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        return true;
    }
};

PortalSession::PortalSession() : impl_(new Impl) {}

PortalSession::~PortalSession() { close(); }

void PortalSession::startAsync(const PortalOptions& opts) {
    if (impl_->started) return;
    impl_->started = true;
    impl_->opts = opts;
    impl_->ctx = g_main_context_new();
    impl_->publish(ShareState::WaitingForDialog, "");
    impl_->worker = std::thread([this] { impl_->run(); });
}

ShareState PortalSession::state() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->state;
}

ShareNotice PortalSession::notice() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return {impl_->state, impl_->reason};
}

bool PortalSession::result(PortalResult& out) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->haveResult) return false;
    out = impl_->result;
    return true;
}

int PortalSession::pipeWireFd() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const int fd = impl_->fd;
    impl_->fd = -1;
    return fd;
}

bool PortalSession::pointerMotionAbsolute(uint32_t node, double x, double y) {
    return impl_->notify("NotifyPointerMotionAbsolute", kDevicePointer, [&](const char* s) {
        return g_variant_new("(o@a{sv}udd)", s, emptyOptions(), node, x, y);
    });
}

bool PortalSession::pointerButton(uint32_t evdevButton, bool down) {
    return impl_->notify("NotifyPointerButton", kDevicePointer, [&](const char* s) {
        return g_variant_new("(o@a{sv}iu)", s, emptyOptions(), static_cast<gint32>(evdevButton), down ? 1u : 0u);
    });
}

bool PortalSession::pointerAxisDiscrete(int axis, int steps) {
    return impl_->notify("NotifyPointerAxisDiscrete", kDevicePointer, [&](const char* s) {
        return g_variant_new("(o@a{sv}ui)", s, emptyOptions(), static_cast<guint32>(axis), static_cast<gint32>(steps));
    });
}

bool PortalSession::keyboardKeysym(uint32_t keysym, bool down) {
    return impl_->notify("NotifyKeyboardKeysym", kDeviceKeyboard, [&](const char* s) {
        return g_variant_new("(o@a{sv}iu)", s, emptyOptions(), static_cast<gint32>(keysym), down ? 1u : 0u);
    });
}

void PortalSession::close() {
    if (!impl_->worker.joinable()) return;
    impl_->abort.store(true);
    g_main_context_wakeup(impl_->ctx);
    impl_->worker.join();
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
}

} // namespace cppdesk::wayland
