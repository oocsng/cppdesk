#pragma once

#if defined(_WIN32)
#if defined(_WIN32)
#if defined(_WIN32)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif
#endif
#endif

#include "../core/protocol.hpp"

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <atomic>

namespace cppdesk {

enum class VirtualBackend : uint8_t {
    Software = 0,
    IddCx    = 1
};

struct VirtualDisplayInfo {
    uint32_t       id = 0;
    int32_t        x = 0;
    int32_t        y = 0;
    uint32_t       width = 1920;
    uint32_t       height = 1080;
    uint32_t       refreshRate = 60;
    bool           isHeadlessFallback = false;
    VirtualBackend backend = VirtualBackend::Software;
    std::string    name;
};

class VirtualDisplayManager {
public:
    static VirtualDisplayManager& instance();

    // Query physical host monitor presence
    static bool isHostHeadless();
    static uint32_t getPhysicalMonitorCount();
    static bool hasIddCxDriver();

    // Headless host auto-provisioning
    bool ensureHeadlessDisplay(uint32_t width = 1920, uint32_t height = 1080, uint32_t refreshRate = 60);
    void releaseHeadlessDisplay();
    bool hasActiveHeadlessDisplay() const;
    uint32_t headlessDisplayId() const;

    // Virtual display lifecycle
    bool createVirtualDisplay(
        uint32_t width = 1920,
        uint32_t height = 1080,
        uint32_t refreshRate = 60,
        bool preferIddCx = true,
        uint32_t* outDisplayId = nullptr);

    bool destroyVirtualDisplay(uint32_t displayId);
    bool setVirtualDisplayMode(uint32_t displayId, uint32_t width, uint32_t height, uint32_t refreshRate);
    void destroyAllSessionDisplays();

    size_t virtualDisplayCount() const;
    std::vector<VirtualDisplayInfo> activeDisplays() const;
    bool getDisplayInfo(uint32_t displayId, VirtualDisplayInfo& outInfo) const;

    // Software surface capture and synthetic wallpaper rendering
    bool captureSoftwareSurface(uint32_t displayId, std::vector<uint8_t>& outBgra, int& outW, int& outH);
    void renderSyntheticDesktop(uint32_t displayId, const std::string& hostStatusText = "");

private:
    VirtualDisplayManager();
    ~VirtualDisplayManager();
    VirtualDisplayManager(const VirtualDisplayManager&) = delete;
    VirtualDisplayManager& operator=(const VirtualDisplayManager&) = delete;

    struct SurfaceState {
        VirtualDisplayInfo info;
#if defined(_WIN32)
        HDC                hdc = nullptr;
        HBITMAP            hbm = nullptr;
        void*              pBits = nullptr;
        HGDIOBJ            hOldBm = nullptr;
#else
        void*              hdc = nullptr;
        void*              hbm = nullptr;
        void*              pBits = nullptr;
        void*              hOldBm = nullptr;
#endif
    };

    bool allocateSoftwareSurface(SurfaceState& surface, uint32_t width, uint32_t height);
    void releaseSoftwareSurface(SurfaceState& surface);
    int32_t computeNextVirtualX_Locked(uint32_t width) const;

    mutable std::mutex                        mutex_;
    std::unordered_map<uint32_t, SurfaceState> displays_;
    std::atomic<uint32_t>                     nextId_{1};
    uint32_t                                  headlessId_{0};
};

} // namespace cppdesk
