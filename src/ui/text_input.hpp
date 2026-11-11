#pragma once

// Text-field editing on UTF-8 buffers: WM_CHAR delivers UTF-16 code units, the fields store UTF-8.
// No platform headers, so it is testable anywhere. maxBytes limits are in bytes and never split a character.

#include <cstddef>
#include <cstdint>
#include <string>

namespace cppdesk {
namespace textinput {

// Printable text only: no C0/C1 controls, no surrogates, nothing past U+10FFFF, no U+FFFE/U+FFFF.
inline bool isAcceptableCodePoint(char32_t cp, bool allowNewline) {
    if (cp == U'\n') return allowNewline;
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    if (cp > 0x10FFFF || cp == 0xFFFE || cp == 0xFFFF) return false;
    return true;
}

inline std::string encodeUtf8(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

// Appends one character if it is acceptable and fits within maxBytes. Returns whether it was appended.
inline bool appendCodePoint(std::string& buf, char32_t cp, size_t maxBytes, bool allowNewline = false) {
    if (!isAcceptableCodePoint(cp, allowNewline)) return false;
    std::string enc = encodeUtf8(cp);
    if (buf.size() + enc.size() > maxBytes) return false;
    buf += enc;
    return true;
}

// Backspace: removes the last whole character, not the last byte.
inline void popLastCodePoint(std::string& buf) {
    while (!buf.empty() && (static_cast<unsigned char>(buf.back()) & 0xC0) == 0x80) buf.pop_back();
    if (!buf.empty()) buf.pop_back();
}

// Appends pasted UTF-8 text. Malformed bytes and unacceptable characters are skipped; stops at the first
// character that does not fit in maxBytes.
inline void appendUtf8Text(std::string& buf, const std::string& text, size_t maxBytes, bool allowNewline) {
    size_t i = 0;
    while (i < text.size()) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > text.size()) { ++i; continue; }

        char32_t cp = len == 1 ? lead : (lead & (0xFF >> (len + 1)));
        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (cont & 0x3F);
        }
        static const char32_t kMinForLen[5] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (!valid || (len > 1 && cp < kMinForLen[len])) { ++i; continue; } // malformed or overlong
        i += len;

        if (!isAcceptableCodePoint(cp, allowNewline)) continue;
        if (!appendCodePoint(buf, cp, maxBytes, allowNewline)) return;
    }
}

// Joins the UTF-16 units of WM_CHAR into code points; a surrogate pair arrives as two messages.
struct Utf16Assembler {
    char16_t pendingHigh = 0;

    // Returns true and sets outCp when a full code point is ready.
    bool push(char16_t unit, char32_t& outCp) {
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pendingHigh = unit;
            return false;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (pendingHigh == 0) return false; // stray low surrogate
            outCp = 0x10000 + ((static_cast<char32_t>(pendingHigh) - 0xD800) << 10) + (unit - 0xDC00);
            pendingHigh = 0;
            return true;
        }
        pendingHigh = 0; // a pending high surrogate followed by a normal unit is dropped
        outCp = unit;
        return true;
    }
};

} // namespace textinput
} // namespace cppdesk
