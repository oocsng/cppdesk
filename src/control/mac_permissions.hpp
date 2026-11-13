#pragma once

// macOS permissions the host needs (implemented in mac_permissions.mm).

#include "../ui/platform_shell.hpp"

namespace cppdesk {

// Reads the current state without showing any prompt.
ui::HostPermissionStatus macPermissionsNow();
// Shows the system prompt for a permission (it appears once; after that the user must use System Settings).
void macRequestPermission(ui::HostPermission permission);
// Opens the matching System Settings -> Privacy & Security page.
void macOpenPrivacyPane(ui::HostPermission permission);

} // namespace cppdesk
