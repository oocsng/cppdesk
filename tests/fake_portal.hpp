#pragma once

// A stand-in for the desktop's portal service (RemoteDesktop and ScreenCast) on a private D-Bus bus, so the real
// protocol code can be driven end to end and every call it makes can be checked. Linux only.

#include "../src/capture/wayland_helpers.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cppdesk::wayland::test {

struct FakeCall {
    std::string iface, method, path;
    std::string args;    // the arguments, printed (g_variant_print)
    std::string token;   // the handle_token the caller chose, if any
};

struct FakeScenario {
    uint32_t remoteDesktopVersion = 2;     // 0: the RemoteDesktop interface does not exist
    uint32_t screenCastVersion = 4;
    uint32_t startResponse = 0;            // the answer to the dialog: 0 allowed, 1 cancelled, 2 failed
    bool silentStart = false;              // the person never answers
    bool immediateResponses = false;       // every Response goes out before the method's own reply
    uint32_t devices = 3;                  // what the dialog allowed: 1 keyboard, 2 pointer
    std::vector<StreamInfo> streams = {{57, 0, 0, 1440, 900}};
    std::string newToken = "token-new";    // "" gives none
};

class FakePortal {
public:
    FakePortal(const std::string& busAddress, const FakeScenario& scenario);   // owns the portal name once this returns
    ~FakePortal();
    FakePortal(const FakePortal&) = delete;
    FakePortal& operator=(const FakePortal&) = delete;

    bool ok() const;
    std::vector<FakeCall> calls() const;
    int count(const std::string& iface, const std::string& method) const;
    void emitSessionClosed();              // the desktop ends the session

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace cppdesk::wayland::test
