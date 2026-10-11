#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>

namespace cppdesk {

class PrivacyCloakManager {
public:
    PrivacyCloakManager();
    ~PrivacyCloakManager() = default;

    static const std::vector<std::wstring>& defaultSensitiveKeywords();

    void setEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
    }
    bool isEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }

    void setCustomKeywords(const std::string& commaSeparated);
    std::string customKeywords() const;

    void setRedactionStyle(uint8_t style) {
        std::lock_guard<std::mutex> lock(mutex_);
        redactionStyle_ = style;
    }
    uint8_t redactionStyle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return redactionStyle_;
    }

    // Tests window title against sensitive patterns (case-insensitive)
    bool matchesSensitiveTitle(const std::wstring& title) const;

    // Scans top-level visible windows and returns screen-space rectangles
    std::vector<RECT> findSensitiveWindowRects() const;

    // Redacts sensitive rectangles directly within raw BGRA capture frame
    void redactCaptureBuffer(
        uint8_t* bgra,
        int width,
        int height,
        int monitorX,
        int monitorY,
        const std::vector<RECT>& sensitiveRects) const;

private:
    bool enabled_ = true;
    uint8_t redactionStyle_ = 0; // 0 = Solid Charcoal, 1 = Mosaic
    std::vector<std::wstring> keywords_;
    mutable std::mutex mutex_;
};

} // namespace cppdesk
