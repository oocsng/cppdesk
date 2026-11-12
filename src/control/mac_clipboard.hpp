#pragma once

// The macOS general pasteboard as UTF-8 text (implemented in mac_clipboard.mm).

#include <string>

namespace cppdesk {

// False when the pasteboard holds no text.
bool macClipboardGetText(std::string& utf8);
bool macClipboardSetText(const std::string& utf8);

} // namespace cppdesk
