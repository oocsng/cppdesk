#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace cppdesk::profile {

// Removes bidi overrides/isolates, zero-width and BOM code points (UTF-8 in, UTF-8 out); they can spoof a displayed name.
std::string stripFormatting(const std::string& utf8);

// Trims, strips control characters and caps at 32 code points (UTF-8 in, UTF-8 out).
std::string normaliseName(const std::string& utf8);

// "name \xC2\xB7 hostname", or just hostname when name is empty. The hostname part is shortened on a UTF-8 boundary
// to fit maxBytes; if even "name \xC2\xB7 " does not fit, returns name cut to maxBytes.
std::string composeWireName(const std::string& name, const std::string& hostname, size_t maxBytes = 255);

struct WireName {
    std::string name;      // the username ("" when the peer sent only a hostname)
    std::string computer;  // the hostname part
};
// Splits at the last " \xC2\xB7 ". No separator means a plain hostname from a 1.0 peer: {wire, ""}.
WireName splitWireName(const std::string& wire);

// Per instance, next to the config file: profile_<instanceId>.jpg and avatars_<instanceId>/<deskId>.jpg
std::filesystem::path ownPicturePath(const std::filesystem::path& configFile, int instanceId);
std::filesystem::path peerPicturePath(const std::filesystem::path& configFile, int instanceId, uint64_t deskId);

} // namespace cppdesk::profile
