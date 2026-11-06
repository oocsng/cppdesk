#include "display_manager.hpp"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace cppdesk {

DisplayResolutionManager::DisplayResolutionManager() {
#if defined(_WIN32)
    DEVMODEW dm{};
    dm.dmSize = sizeof(DEVMODEW);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        originalWidth_ = dm.dmPelsWidth;
        originalHeight_ = dm.dmPelsHeight;
    } else {
        originalWidth_ = static_cast<uint32_t>(std::max(800, GetSystemMetrics(SM_CXSCREEN)));
        originalHeight_ = static_cast<uint32_t>(std::max(600, GetSystemMetrics(SM_CYSCREEN)));
    }
#else
    originalWidth_ = 1920;
    originalHeight_ = 1080;
#endif
    currentWidth_.store(originalWidth_);
    currentHeight_.store(originalHeight_);
}

DisplayResolutionManager::~DisplayResolutionManager() {
    restoreResolution();
}

std::vector<DisplayModeEntry> DisplayResolutionManager::enumerateDisplayModes(const std::wstring& deviceName) {
    std::vector<DisplayModeEntry> results;
#if defined(_WIN32)
    const wchar_t* pDev = deviceName.empty() ? nullptr : deviceName.c_str();

    DEVMODEW dm{};
    dm.dmSize = sizeof(DEVMODEW);
    DWORD modeNum = 0;
    while (EnumDisplaySettingsW(pDev, modeNum, &dm)) {
        if (dm.dmPelsWidth >= 640 && dm.dmPelsHeight >= 480 && dm.dmBitsPerPel >= 16) {
            bool exists = false;
            for (const auto& item : results) {
                if (item.width == dm.dmPelsWidth && item.height == dm.dmPelsHeight && item.refreshRate == dm.dmDisplayFrequency) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                results.push_back({ dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency });
            }
        }
        ++modeNum;
    }
#else
    (void)deviceName;
#endif

    if (results.empty()) {
        results.push_back({ 1920, 1080, 60 });
        results.push_back({ 1600, 900, 60 });
        results.push_back({ 1366, 768, 60 });
        results.push_back({ 1280, 720, 60 });
    }
    return results;
}

DisplayModeEntry DisplayResolutionManager::findBestResolutionMatch(
    uint32_t targetWidth,
    uint32_t targetHeight,
    const std::vector<DisplayModeEntry>& modes)
{
    if (targetWidth == 0) targetWidth = 1920;
    if (targetHeight == 0) targetHeight = 1080;

    if (modes.empty()) {
        return { targetWidth, targetHeight, 60 };
    }

    // 1. Direct exact match check
    for (const auto& m : modes) {
        if (m.width == targetWidth && m.height == targetHeight) {
            return m;
        }
    }

    // 2. Aspect ratio and resolution optimization
    double targetAspect = (targetHeight > 0) ? (static_cast<double>(targetWidth) / static_cast<double>(targetHeight)) : (16.0 / 9.0);
    double bestScore = 1e9;
    DisplayModeEntry bestEntry = modes[0];

    for (const auto& m : modes) {
        if (m.width == 0 || m.height == 0) continue;
        double aspect = static_cast<double>(m.width) / static_cast<double>(m.height);
        double aspectDiff = std::abs(aspect - targetAspect);

        double wDiff = std::abs(static_cast<double>(m.width) - static_cast<double>(targetWidth)) / static_cast<double>(targetWidth);
        double hDiff = std::abs(static_cast<double>(m.height) - static_cast<double>(targetHeight)) / static_cast<double>(targetHeight);

        // Aspect ratio is heavily weighted to eliminate letterboxing
        double score = aspectDiff * 4.0 + (wDiff + hDiff) * 0.5;
        if (score < bestScore) {
            bestScore = score;
            bestEntry = m;
        }
    }

    return bestEntry;
}

bool DisplayResolutionManager::changeResolution(uint32_t width, uint32_t height, const std::wstring& deviceName) {
#if !defined(_WIN32)
    (void)width; (void)height; (void)deviceName;
    return false;
#else
    if (width == 0 || height == 0) {
        return restoreResolution(deviceName);
    }

    activeDeviceName_ = deviceName;
    const wchar_t* pDev = activeDeviceName_.empty() ? nullptr : activeDeviceName_.c_str();

    DEVMODEW dm{};
    dm.dmSize = sizeof(DEVMODEW);
    if (!EnumDisplaySettingsW(pDev, ENUM_CURRENT_SETTINGS, &dm)) {
        return false;
    }

    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

    LONG testRes = ChangeDisplaySettingsExW(pDev, &dm, nullptr, CDS_TEST, nullptr);
    if (testRes != DISP_CHANGE_SUCCESSFUL) {
        // Try finding best supported match if exact mode is unsupported
        auto modes = enumerateDisplayModes(deviceName);
        auto best = findBestResolutionMatch(width, height, modes);
        if (best.width != width || best.height != height) {
            dm.dmPelsWidth = best.width;
            dm.dmPelsHeight = best.height;
            testRes = ChangeDisplaySettingsExW(pDev, &dm, nullptr, CDS_TEST, nullptr);
        }
    }

    if (testRes == DISP_CHANGE_SUCCESSFUL) {
        LONG applyRes = ChangeDisplaySettingsExW(pDev, &dm, nullptr, 0, nullptr);
        if (applyRes == DISP_CHANGE_SUCCESSFUL) {
            currentWidth_.store(dm.dmPelsWidth);
            currentHeight_.store(dm.dmPelsHeight);
            resolutionChanged_.store(true);
            return true;
        }
    }

    return false;
#endif
}

bool DisplayResolutionManager::restoreResolution(const std::wstring& deviceName) {
#if !defined(_WIN32)
    (void)deviceName;
    return true;
#else
    if (!resolutionChanged_.load()) return true;

    const wchar_t* pDev = deviceName.empty() ? (activeDeviceName_.empty() ? nullptr : activeDeviceName_.c_str()) : deviceName.c_str();
    LONG res = ChangeDisplaySettingsExW(pDev, nullptr, nullptr, 0, nullptr);
    if (res == DISP_CHANGE_SUCCESSFUL) {
        currentWidth_.store(originalWidth_);
        currentHeight_.store(originalHeight_);
        resolutionChanged_.store(false);
        return true;
    }
    return false;
#endif
}

} // namespace cppdesk
