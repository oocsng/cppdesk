#include "fake_portal.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <condition_variable>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace cppdesk::wayland::test {

namespace {

const char* kRemoteDesktopXml = R"(<node><interface name="org.freedesktop.portal.RemoteDesktop">
  <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="SelectDevices"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="NotifyPointerMotionAbsolute"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="in"/><arg type="d" direction="in"/><arg type="d" direction="in"/></method>
  <method name="NotifyPointerButton"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
  <method name="NotifyPointerAxisDiscrete"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="u" direction="in"/><arg type="i" direction="in"/></method>
  <method name="NotifyKeyboardKeysym"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="in"/></method>
  <property name="version" type="u" access="read"/>
</interface></node>)";

const char* kScreenCastXml = R"(<node><interface name="org.freedesktop.portal.ScreenCast">
  <method name="CreateSession"><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="SelectSources"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="Start"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/></method>
  <method name="OpenPipeWireRemote"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="h" direction="out"/></method>
  <property name="version" type="u" access="read"/>
</interface></node>)";

const char* kPath = "/org/freedesktop/portal/desktop";

std::string senderPart(const std::string& sender) {
    std::string out;
    for (char c : sender) if (c != ':') out += (c == '.') ? '_' : c;
    return out;
}

}   // namespace

struct FakePortal::Impl {
    FakeScenario scenario;
    std::string address;
    std::thread thread;
    GMainContext* ctx = nullptr;
    GMainLoop* loop = nullptr;
    GDBusConnection* conn = nullptr;
    GDBusNodeInfo* rdInfo = nullptr;
    GDBusNodeInfo* scInfo = nullptr;

    mutable std::mutex mu;
    std::condition_variable ready;
    bool started = false;
    bool good = false;
    std::vector<FakeCall> calls;
    std::string lastSession;

    void record(FakeCall c) {
        std::lock_guard<std::mutex> lk(mu);
        calls.push_back(std::move(c));
    }

    // Answers a request-style method: the reply carries the request handle, the Response signal carries the outcome.
    void answerRequest(GDBusMethodInvocation* inv, const std::string& iface, const std::string& method, GVariant* params,
                       const std::string& token, const std::string& sessionToken) {
        const std::string sender = g_dbus_method_invocation_get_sender(inv);
        const std::string requestPath = std::string(kPath) + "/request/" + senderPart(sender) + "/" + token;

        uint32_t code = 0;
        GVariantBuilder results;
        g_variant_builder_init(&results, G_VARIANT_TYPE("a{sv}"));
        bool respond = true;
        if (method == "CreateSession") {
            const std::string session = std::string(kPath) + "/session/" + senderPart(sender) + "/" + sessionToken;
            { std::lock_guard<std::mutex> lk(mu); lastSession = session; }
            g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(session.c_str()));
        } else if (method == "Start") {
            code = scenario.startResponse;
            respond = !scenario.silentStart;
            if (code == 0) {
                if (iface == "org.freedesktop.portal.RemoteDesktop")
                    g_variant_builder_add(&results, "{sv}", "devices", g_variant_new_uint32(scenario.devices));
                GVariantBuilder streams;
                g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
                for (const auto& s : scenario.streams) {
                    GVariantBuilder props;
                    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
                    g_variant_builder_add(&props, "{sv}", "position", g_variant_new("(ii)", s.x, s.y));
                    if (s.logicalW > 0)
                        g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", s.logicalW, s.logicalH));
                    g_variant_builder_add(&streams, "(u@a{sv})", s.nodeId, g_variant_builder_end(&props));
                }
                g_variant_builder_add(&results, "{sv}", "streams", g_variant_builder_end(&streams));
                if (!scenario.newToken.empty() && iface == "org.freedesktop.portal.RemoteDesktop")
                    g_variant_builder_add(&results, "{sv}", "restore_token", g_variant_new_string(scenario.newToken.c_str()));
            }
        }
        (void)params;

        GVariant* response = g_variant_new("(u@a{sv})", code, g_variant_builder_end(&results));
        auto emit = [&] {
            if (respond)
                g_dbus_connection_emit_signal(conn, sender.c_str(), requestPath.c_str(), "org.freedesktop.portal.Request",
                                              "Response", g_variant_ref(response), nullptr);
        };
        if (scenario.immediateResponses) emit();
        g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", requestPath.c_str()));
        if (!scenario.immediateResponses) emit();
        g_variant_unref(response);
    }

    static void onMethod(GDBusConnection*, const gchar*, const gchar*, const gchar* iface, const gchar* method,
                         GVariant* params, GDBusMethodInvocation* inv, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        const std::string m = method;
        FakeCall call;
        call.iface = iface;
        call.method = m;
        call.path = kPath;
        gchar* printed = g_variant_print(params, FALSE);
        call.args = printed;
        g_free(printed);

        std::string token, sessionToken;
        const bool request = m == "CreateSession" || m == "SelectDevices" || m == "SelectSources" || m == "Start";
        if (request) {
            GVariant* options = g_variant_get_child_value(params, g_variant_n_children(params) - 1);
            const char* t = nullptr;
            if (g_variant_lookup(options, "handle_token", "&s", &t) && t) token = t;
            if (g_variant_lookup(options, "session_handle_token", "&s", &t) && t) sessionToken = t;
            g_variant_unref(options);
            call.token = token;
        }
        self->record(call);

        if (request) {
            self->answerRequest(inv, iface, m, params, token, sessionToken);
        } else if (m == "OpenPipeWireRemote") {
            GUnixFDList* list = g_unix_fd_list_new();
            const int fd = open("/dev/null", O_RDONLY);
            g_unix_fd_list_append(list, fd, nullptr);
            ::close(fd);
            g_dbus_method_invocation_return_value_with_unix_fd_list(inv, g_variant_new("(h)", 0), list);
            g_object_unref(list);
        } else {
            g_dbus_method_invocation_return_value(inv, nullptr);   // the Notify* input methods
        }
    }

    static GVariant* onProperty(GDBusConnection*, const gchar*, const gchar*, const gchar* iface, const gchar* name,
                                GError**, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        if (std::string(name) != "version") return nullptr;
        return g_variant_new_uint32(std::string(iface) == "org.freedesktop.portal.RemoteDesktop"
                                        ? self->scenario.remoteDesktopVersion : self->scenario.screenCastVersion);
    }

    // Request.Close and Session.Close are plain calls on paths nobody registered: answer and note them.
    static GDBusMessage* onMessage(GDBusConnection* c, GDBusMessage* message, gboolean incoming, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        if (incoming && g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_METHOD_CALL) {
            const char* iface = g_dbus_message_get_interface(message);
            const char* member = g_dbus_message_get_member(message);
            if (iface && member && std::string(member) == "Close" &&
                (std::string(iface) == "org.freedesktop.portal.Request" || std::string(iface) == "org.freedesktop.portal.Session")) {
                FakeCall call;
                call.iface = iface;
                call.method = member;
                call.path = g_dbus_message_get_path(message);
                self->record(call);
                GDBusMessage* reply = g_dbus_message_new_method_reply(message);
                g_dbus_connection_send_message(c, reply, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
                g_object_unref(reply);
                g_object_unref(message);
                return nullptr;
            }
        }
        return message;
    }

    void run() {
        g_main_context_push_thread_default(ctx);
        GError* err = nullptr;
        conn = g_dbus_connection_new_for_address_sync(
            address.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &err);
        bool ok = conn != nullptr;
        if (ok) {
            rdInfo = g_dbus_node_info_new_for_xml(kRemoteDesktopXml, nullptr);
            scInfo = g_dbus_node_info_new_for_xml(kScreenCastXml, nullptr);
            GDBusInterfaceVTable vtable = {&Impl::onMethod, &Impl::onProperty, nullptr, {}};
            g_dbus_connection_add_filter(conn, &Impl::onMessage, this, nullptr);
            if (scenario.remoteDesktopVersion > 0)
                ok = g_dbus_connection_register_object(conn, kPath, rdInfo->interfaces[0], &vtable, this, nullptr, &err) != 0;
            if (ok)
                ok = g_dbus_connection_register_object(conn, kPath, scInfo->interfaces[0], &vtable, this, nullptr, &err) != 0;
            if (ok) {
                GVariant* r = g_dbus_connection_call_sync(
                    conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                    g_variant_new("(su)", "org.freedesktop.portal.Desktop", 0u), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000,
                    nullptr, &err);
                guint32 reply = 0;
                if (r) { g_variant_get(r, "(u)", &reply); g_variant_unref(r); }
                ok = reply == 1;
            }
        }
        if (err) g_error_free(err);
        {
            std::lock_guard<std::mutex> lk(mu);
            good = ok;
            started = true;
        }
        ready.notify_all();
        if (ok) g_main_loop_run(loop);
        if (conn) {
            g_dbus_connection_close_sync(conn, nullptr, nullptr);
            g_object_unref(conn);
            conn = nullptr;
        }
        g_main_context_pop_thread_default(ctx);
    }
};

FakePortal::FakePortal(const std::string& busAddress, const FakeScenario& scenario) : impl_(new Impl) {
    impl_->scenario = scenario;
    impl_->address = busAddress;
    impl_->ctx = g_main_context_new();
    impl_->loop = g_main_loop_new(impl_->ctx, FALSE);
    impl_->thread = std::thread([this] { impl_->run(); });
    std::unique_lock<std::mutex> lk(impl_->mu);
    impl_->ready.wait(lk, [this] { return impl_->started; });
}

FakePortal::~FakePortal() {
    if (impl_->loop) g_main_loop_quit(impl_->loop);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->rdInfo) g_dbus_node_info_unref(impl_->rdInfo);
    if (impl_->scInfo) g_dbus_node_info_unref(impl_->scInfo);
    if (impl_->loop) g_main_loop_unref(impl_->loop);
    if (impl_->ctx) g_main_context_unref(impl_->ctx);
}

bool FakePortal::ok() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->good;
}

std::vector<FakeCall> FakePortal::calls() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->calls;
}

int FakePortal::count(const std::string& iface, const std::string& method) const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    int n = 0;
    for (const auto& c : impl_->calls) if (c.iface == iface && c.method == method) ++n;
    return n;
}

void FakePortal::emitSessionClosed() {
    std::string session;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        session = impl_->lastSession;
    }
    if (session.empty() || !impl_->conn) return;
    g_dbus_connection_emit_signal(impl_->conn, nullptr, session.c_str(), "org.freedesktop.portal.Session", "Closed",
                                  g_variant_new("(@a{sv})", g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)), nullptr);
}

}   // namespace cppdesk::wayland::test
