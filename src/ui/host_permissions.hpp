#pragma once

// What the "allow screen sharing" card says, from the permission state. Plain words for people who have never
// opened a privacy settings page; no platform headers.

#include "platform_shell.hpp"

#include <string>
#include <vector>

namespace cppdesk::ui {

struct HostPermissionCard {
    bool visible = false;        // false when nothing is missing
    bool needsRestart = false;   // show a "Restart CppDesk" button
    std::string title;
    std::string body;
    struct Row {
        HostPermission which;
        bool granted;
        std::string label;
    };
    std::vector<Row> rows;       // the permissions still missing
    std::string approvalWarning; // one line for the incoming-request dialog; empty when nothing is missing
};

// `returnedFromSettings`: the user opened a Screen Recording page and came back. macOS only applies that permission
// to an app after it restarts, so the card then offers a restart.
HostPermissionCard describeHostPermissions(const HostPermissionStatus& status, bool returnedFromSettings);

} // namespace cppdesk::ui
