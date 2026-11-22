#include "profile.hpp"

namespace cppdesk::profile {

namespace {
const std::string kSep = " \xC2\xB7 ";

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

std::string cutUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && isContinuation(s[cut])) --cut;
    return s.substr(0, cut);
}

std::string trimSpaces(const std::string& s) {
    const char* ws = " \t\r\n\v\f";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}
} // namespace

std::string stripFormatting(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        size_t n = 1;
        while (i + n < utf8.size() && isContinuation(utf8[i + n])) ++n;
        const unsigned char b1 = n > 1 ? static_cast<unsigned char>(utf8[i + 1]) : 0;
        const unsigned char b2 = n > 2 ? static_cast<unsigned char>(utf8[i + 2]) : 0;
        const bool fmt = (n == 3 && static_cast<unsigned char>(utf8[i]) == 0xE2 &&
                          ((b1 == 0x80 && ((b2 >= 0x8B && b2 <= 0x8F) || (b2 >= 0xAA && b2 <= 0xAE))) ||   // U+200B-200F, U+202A-202E
                           (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9))) ||                                   // U+2066-2069
                        (n == 3 && static_cast<unsigned char>(utf8[i]) == 0xEF && b1 == 0xBB && b2 == 0xBF);   // U+FEFF
        if (!fmt) out.append(utf8, i, n);
        i += n;
    }
    return out;
}

std::string normaliseName(const std::string& utf8Raw) {
    const std::string utf8 = stripFormatting(utf8Raw);
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        size_t n = 1;
        while (i + n < utf8.size() && isContinuation(utf8[i + n])) ++n;
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        bool control = c < 0x20 || c == 0x7F || (c == 0xC2 && n == 2 && static_cast<unsigned char>(utf8[i + 1]) < 0xA0);
        if (!control) {
            out.append(utf8, i, n);
        }
        i += n;
    }
    out = trimSpaces(out);
    size_t kept = 0, end = 0;
    while (end < out.size() && kept < 32) {
        ++end;
        while (end < out.size() && isContinuation(out[end])) ++end;
        ++kept;
    }
    return trimSpaces(out.substr(0, end));
}

std::string composeWireName(const std::string& name, const std::string& hostname, size_t maxBytes) {
    if (name.empty()) return cutUtf8(hostname, maxBytes);
    if (hostname.empty() || name.size() + kSep.size() >= maxBytes) return cutUtf8(name, maxBytes);
    return name + kSep + cutUtf8(hostname, maxBytes - name.size() - kSep.size());
}

WireName splitWireName(const std::string& wire) {
    const std::string clean = stripFormatting(wire);   // every display path goes through here
    size_t pos = clean.rfind(kSep);
    if (pos == std::string::npos) return {clean, ""};
    return {clean.substr(0, pos), clean.substr(pos + kSep.size())};
}

std::filesystem::path ownPicturePath(const std::filesystem::path& configFile, int instanceId) {
    return configFile.parent_path() / ("profile_" + std::to_string(instanceId) + ".jpg");
}

std::filesystem::path peerPicturePath(const std::filesystem::path& configFile, int instanceId, uint64_t deskId) {
    return configFile.parent_path() / ("avatars_" + std::to_string(instanceId)) / (std::to_string(deskId) + ".jpg");
}

} // namespace cppdesk::profile
