#include "privacy_cloak_manager.hpp"
#include <dwmapi.h>
#include <cwctype>
#include <algorithm>
#include <sstream>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace cppdesk {

namespace {

std::wstring toLowerW(const std::wstring& str) {
    std::wstring lower = str;
    for (auto& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return lower;
}

std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), w.data(), len);
    return w;
}

std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string str(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), str.data(), len, nullptr, nullptr);
    return str;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

const std::vector<std::wstring>& PrivacyCloakManager::defaultSensitiveKeywords() {
    static const std::vector<std::wstring> s_defaults = {
        L"KeePass",
        L"Bitwarden",
        L"1Password",
        L"Authenticator",
        L"Credential",
        L"Wallet",
        L".env",
        L"Banking",
        L"Chase",
        L"Wells Fargo",
        L"MetaMask",
        L"Ledger Live",
        L"Incognito",
        L"Private",
        L"WireGuard",
        L"VPN"
    };
    return s_defaults;
}

PrivacyCloakManager::PrivacyCloakManager() {
    keywords_ = defaultSensitiveKeywords();
}

void PrivacyCloakManager::setCustomKeywords(const std::string& commaSeparated) {
    std::lock_guard<std::mutex> lock(mutex_);
    keywords_.clear();
    std::stringstream ss(commaSeparated);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::string t = trim(token);
        if (!t.empty()) {
            keywords_.push_back(utf8ToWide(t));
        }
    }
    if (keywords_.empty()) {
        keywords_ = defaultSensitiveKeywords();
    }
}

std::string PrivacyCloakManager::customKeywords() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result;
    for (size_t i = 0; i < keywords_.size(); ++i) {
        if (i > 0) result += ", ";
        result += wideToUtf8(keywords_[i]);
    }
    return result;
}

bool PrivacyCloakManager::matchesSensitiveTitle(const std::wstring& title) const {
    if (title.empty()) return false;
    std::wstring lowerTitle = toLowerW(title);

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kw : keywords_) {
        if (kw.empty()) continue;
        if (lowerTitle.find(toLowerW(kw)) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

struct CloakEnumContext {
    const PrivacyCloakManager* self = nullptr;
    std::vector<RECT> rects;
};

std::vector<RECT> PrivacyCloakManager::findSensitiveWindowRects() const {
    CloakEnumContext ctx;
    ctx.self = this;

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<CloakEnumContext*>(lParam);
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            return TRUE;
        }

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
            if (cloaked != 0) return TRUE;
        }

        RECT rc{};
        if (!GetWindowRect(hwnd, &rc)) return TRUE;
        if ((rc.right - rc.left) < 30 || (rc.bottom - rc.top) < 30) {
            return TRUE;
        }

        wchar_t titleBuf[256] = {};
        if (GetWindowTextW(hwnd, titleBuf, 256) > 0) {
            if (c->self->matchesSensitiveTitle(titleBuf)) {
                c->rects.push_back(rc);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.rects;
}

void PrivacyCloakManager::redactCaptureBuffer(
    uint8_t* bgra,
    int width,
    int height,
    int monitorX,
    int monitorY,
    const std::vector<RECT>& sensitiveRects) const
{
    if (!bgra || width <= 0 || height <= 0 || sensitiveRects.empty()) {
        return;
    }

    uint8_t style = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_) return;
        style = redactionStyle_;
    }

    for (const auto& rc : sensitiveRects) {
        int left = std::max<int>(static_cast<int>(rc.left), monitorX);
        int right = std::min<int>(static_cast<int>(rc.right), monitorX + width);
        int top = std::max<int>(static_cast<int>(rc.top), monitorY);
        int bottom = std::min<int>(static_cast<int>(rc.bottom), monitorY + height);

        if (left >= right || top >= bottom) continue;

        int lx = left - monitorX;
        int ly = top - monitorY;
        int lw = right - left;
        int lh = bottom - top;

        if (style == 0) {
            // Style 0: Solid Charcoal (0x1E1E1E, Alpha 0xFF)
            constexpr uint32_t CHARCOAL_PIXEL = 0xFF1E1E1E; // BGRA format
            for (int y = ly; y < ly + lh; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(bgra + (static_cast<size_t>(y) * width + lx) * 4);
                std::fill_n(row, lw, CHARCOAL_PIXEL);
            }
        } else {
            // Style 1: Mosaic / 16x16 Pixelation Block
            constexpr int BLOCK_SIZE = 16;
            for (int by = ly; by < ly + lh; by += BLOCK_SIZE) {
                int blockH = std::min(BLOCK_SIZE, ly + lh - by);
                for (int bx = lx; bx < lx + lw; bx += BLOCK_SIZE) {
                    int blockW = std::min(BLOCK_SIZE, lx + lw - bx);

                    // Compute average B, G, R
                    uint64_t sumB = 0;
                    uint64_t sumG = 0;
                    uint64_t sumR = 0;
                    int pixelCount = blockW * blockH;

                    for (int dy = 0; dy < blockH; ++dy) {
                        const uint8_t* p = bgra + (static_cast<size_t>(by + dy) * width + bx) * 4;
                        for (int dx = 0; dx < blockW; ++dx) {
                            sumB += p[dx * 4 + 0];
                            sumG += p[dx * 4 + 1];
                            sumR += p[dx * 4 + 2];
                        }
                    }

                    uint8_t avgB = static_cast<uint8_t>(sumB / pixelCount);
                    uint8_t avgG = static_cast<uint8_t>(sumG / pixelCount);
                    uint8_t avgR = static_cast<uint8_t>(sumR / pixelCount);
                    uint32_t mosaicPixel = (0xFF << 24) | (avgR << 16) | (avgG << 8) | avgB;

                    for (int dy = 0; dy < blockH; ++dy) {
                        uint32_t* row = reinterpret_cast<uint32_t*>(bgra + (static_cast<size_t>(by + dy) * width + bx) * 4);
                        std::fill_n(row, blockW, mosaicPixel);
                    }
                }
            }
        }
    }
}

} // namespace cppdesk
