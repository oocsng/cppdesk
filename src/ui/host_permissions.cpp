#include "host_permissions.hpp"

namespace cppdesk::ui {

HostPermissionCard describeHostPermissions(const HostPermissionStatus& s, bool returnedFromSettings) {
    HostPermissionCard card;
    if (s.screenRecording && s.accessibility) return card;
    card.visible = true;
    card.title = "Allow screen sharing";
    if (!s.screenRecording) {
        card.rows.push_back({ HostPermission::ScreenRecording, false, "Screen Recording: lets others see your screen" });
    }
    if (!s.accessibility) {
        card.rows.push_back({ HostPermission::Accessibility, false, "Accessibility: lets others use your mouse and keyboard" });
    }
    if (!s.screenRecording && !s.accessibility) {
        card.body = "macOS asks you to allow two things before CppDesk can show your screen to someone and let them "
                    "use your mouse and keyboard. Turn both on below.";
        card.approvalWarning = "Screen Recording and Accessibility are off in macOS, so the other person will not see your "
                               "screen or be able to control it until you turn them on.";
    } else if (!s.screenRecording) {
        card.body = "macOS needs your permission before CppDesk can show your screen to someone. Turn it on below.";
        card.approvalWarning = "Screen Recording is off in macOS, so the other person will not see your screen until you "
                               "turn it on.";
    } else {
        card.body = "macOS needs your permission before someone can use your mouse and keyboard. Turn it on below.";
        card.approvalWarning = "Accessibility is off in macOS, so the other person can see your screen but not control it "
                               "until you turn it on.";
    }
    if (!s.screenRecording && returnedFromSettings) {
        card.needsRestart = true;
        card.body += " If you just turned Screen Recording on, restart CppDesk so macOS applies it.";
    }
    return card;
}

} // namespace cppdesk::ui
