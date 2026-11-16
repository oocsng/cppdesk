#pragma once

// The macOS general pasteboard: UTF-8 text and file URLs (implemented in mac_clipboard.mm).

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cppdesk {

// False when the pasteboard holds no text.
bool macClipboardGetText(std::string& utf8);
bool macClipboardSetText(const std::string& utf8);

// Paths of the files another app copied; false when there are none, or when they are the ones offered below.
bool macClipboardGetFiles(std::vector<std::string>& paths);
// Promises `count` files on the pasteboard. `fetch` runs when an app pastes, blocking until the files are here, and
// returns their paths (nullopt when they could not be fetched).
bool macClipboardOfferFiles(size_t count, std::function<std::optional<std::vector<std::string>>()> fetch);
// Takes the promised files off the pasteboard if nothing has replaced them.
void macClipboardClearFiles();

} // namespace cppdesk
