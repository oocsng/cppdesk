#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cppdesk {

struct DisplayModeEntry {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshRate = 60;
};

class DisplayResolutionManager {
public:
    DisplayResolutionManager();
    ~DisplayResolutionManager();

    // Change host display resolution to specified dimensions
    bool changeResolution(uint32_t width, uint32_t height, const std::wstring& deviceName = L"");

    // Restore host display to registry default resolution
    bool restoreResolution(const std::wstring& deviceName = L"");

    bool isResolutionChanged() const { return resolutionChanged_.load(); }
    uint32_t originalWidth() const { return originalWidth_; }
    uint32_t originalHeight() const { return originalHeight_; }
    uint32_t currentWidth() const { return currentWidth_.load(); }
    uint32_t currentHeight() const { return currentHeight_.load(); }

    // Enumerate supported display modes for the adapter
    static std::vector<DisplayModeEntry> enumerateDisplayModes(const std::wstring& deviceName = L"");

    // Select the optimal display mode that matches target dimensions and aspect ratio
    static DisplayModeEntry findBestResolutionMatch(
        uint32_t targetWidth,
        uint32_t targetHeight,
        const std::vector<DisplayModeEntry>& modes);

private:
    uint32_t                originalWidth_ = 1920;
    uint32_t                originalHeight_ = 1080;
    std::atomic<uint32_t>   currentWidth_{1920};
    std::atomic<uint32_t>   currentHeight_{1080};
    std::atomic<bool>       resolutionChanged_{false};
    std::wstring            activeDeviceName_;
};

} // namespace cppdesk
