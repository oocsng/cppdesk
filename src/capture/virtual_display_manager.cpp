#include "virtual_display_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace cppdesk {

namespace {

#if defined(_WIN32)
void renderSyntheticDesktopInternal(VirtualDisplayInfo& info, HDC hdc, void* pBits, const std::string& hostStatusText) {
#else
void renderSyntheticDesktopInternal(VirtualDisplayInfo& info, void* /*hdc*/, void* pBits, const std::string& /*hostStatusText*/) {
#endif
    if (!pBits) return;
    uint32_t w = info.width;
    uint32_t h = info.height;
    uint8_t* pBytes = static_cast<uint8_t*>(pBits);

    // Modern dark slate gradient (#181B22 to #101216)
    for (uint32_t y = 0; y < h; ++y) {
        float t = static_cast<float>(y) / static_cast<float>(h > 1 ? (h - 1) : 1);
        uint8_t b = static_cast<uint8_t>(34.0f * (1.0f - t) + 22.0f * t);
        uint8_t g = static_cast<uint8_t>(27.0f * (1.0f - t) + 18.0f * t);
        uint8_t r = static_cast<uint8_t>(24.0f * (1.0f - t) + 16.0f * t);
        uint8_t* row = pBytes + static_cast<size_t>(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            bool isGrid = ((x % 64 == 0) || (y % 64 == 0));
            row[x * 4 + 0] = isGrid ? static_cast<uint8_t>(b + 12) : b;
            row[x * 4 + 1] = isGrid ? static_cast<uint8_t>(g + 12) : g;
            row[x * 4 + 2] = isGrid ? static_cast<uint8_t>(r + 12) : r;
            row[x * 4 + 3] = 0xFF;
        }
    }

#if defined(_WIN32)
    if (hdc) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 225, 235));

        std::string title = "CppDesk Virtual Display #" + std::to_string(info.id);
        std::string geom = std::to_string(w) + "x" + std::to_string(h) + " @ " + std::to_string(info.refreshRate) + "Hz";
        std::string status = hostStatusText.empty()
            ? (info.isHeadlessFallback ? "Headless Desktop Emulation Active" : "Secondary Virtual Monitor Active")
            : hostStatusText;

        RECT rcTitle{ static_cast<LONG>(w / 2 - 250), static_cast<LONG>(h / 2 - 40), static_cast<LONG>(w / 2 + 250), static_cast<LONG>(h / 2 - 10) };
        DrawTextA(hdc, title.c_str(), -1, &rcTitle, DT_CENTER | DT_SINGLELINE);

        RECT rcGeom{ static_cast<LONG>(w / 2 - 250), static_cast<LONG>(h / 2 - 10), static_cast<LONG>(w / 2 + 250), static_cast<LONG>(h / 2 + 15) };
        DrawTextA(hdc, geom.c_str(), -1, &rcGeom, DT_CENTER | DT_SINGLELINE);

        RECT rcStat{ static_cast<LONG>(w / 2 - 300), static_cast<LONG>(h / 2 + 20), static_cast<LONG>(w / 2 + 300), static_cast<LONG>(h / 2 + 50) };
        DrawTextA(hdc, status.c_str(), -1, &rcStat, DT_CENTER | DT_SINGLELINE);

        GdiFlush();

        // Enforce opaque alpha for text pixels blitted by GDI
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = pBytes + static_cast<size_t>(y) * w * 4;
            for (uint32_t x = 0; x < w; ++x) {
                row[x * 4 + 3] = 0xFF;
            }
        }
    }
#endif
}

} // namespace

VirtualDisplayManager& VirtualDisplayManager::instance() {
    static VirtualDisplayManager s_instance;
    return s_instance;
}

VirtualDisplayManager::VirtualDisplayManager() = default;

VirtualDisplayManager::~VirtualDisplayManager() {
    destroyAllSessionDisplays();
}

bool VirtualDisplayManager::isHostHeadless() {
#if !defined(_WIN32)
    return false;
#else
    int metricsMonitors = GetSystemMetrics(SM_CMONITORS);
    uint32_t enumMonitors = getPhysicalMonitorCount();
    return (metricsMonitors == 0 || enumMonitors == 0);
#endif
}

uint32_t VirtualDisplayManager::getPhysicalMonitorCount() {
#if !defined(_WIN32)
    return 1;
#else
    struct CountCtx {
        uint32_t count = 0;
    } ctx;
    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<CountCtx*>(lParam);
            c->count++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx)
    );
    return ctx.count;
#endif
}

bool VirtualDisplayManager::hasIddCxDriver() {
#if !defined(_WIN32)
    return false;
#else
    HANDLE h1 = CreateFileA("\\\\.\\ParsecVirtualDisplay",
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h1 != INVALID_HANDLE_VALUE) {
        CloseHandle(h1);
        return true;
    }
    HANDLE h2 = CreateFileA("\\\\.\\RustDeskIddDisplay",
                            GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h2 != INVALID_HANDLE_VALUE) {
        CloseHandle(h2);
        return true;
    }
    return false;
#endif
}

bool VirtualDisplayManager::allocateSoftwareSurface(SurfaceState& surface, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 8192 || height > 8192) {
        return false;
    }
    releaseSoftwareSurface(surface);
#if !defined(_WIN32)
    // A plain pixel buffer; there is no GDI surface to draw text on here.
    surface.pBits = std::calloc(static_cast<size_t>(width) * height, 4);
    return surface.pBits != nullptr;
#else

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    surface.hdc = CreateCompatibleDC(nullptr);
    if (!surface.hdc) return false;

    surface.hbm = CreateDIBSection(surface.hdc, &bmi, DIB_RGB_COLORS, &surface.pBits, nullptr, 0);
    if (!surface.hbm || !surface.pBits) {
        releaseSoftwareSurface(surface);
        return false;
    }

    surface.hOldBm = SelectObject(surface.hdc, surface.hbm);
    return true;
#endif
}

void VirtualDisplayManager::releaseSoftwareSurface(SurfaceState& surface) {
#if !defined(_WIN32)
    std::free(surface.pBits);
    surface.pBits = nullptr;
#else
    if (surface.hdc) {
        if (surface.hOldBm) {
            SelectObject(surface.hdc, surface.hOldBm);
            surface.hOldBm = nullptr;
        }
        DeleteDC(surface.hdc);
        surface.hdc = nullptr;
    }
    if (surface.hbm) {
        DeleteObject(surface.hbm);
        surface.hbm = nullptr;
    }
    surface.pBits = nullptr;
#endif
}

int32_t VirtualDisplayManager::computeNextVirtualX_Locked(uint32_t /*width*/) const {
    int32_t maxX = 0;
#if !defined(_WIN32)
    maxX = 1920;
#else
    struct BoundsCtx {
        int32_t maxX = 0;
    } bctx;

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<BoundsCtx*>(lParam);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoA(hMon, &mi)) {
                if (mi.rcMonitor.right > c->maxX) {
                    c->maxX = mi.rcMonitor.right;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&bctx)
    );
    maxX = bctx.maxX;
#endif

    for (const auto& kv : displays_) {
        int32_t right = kv.second.info.x + static_cast<int32_t>(kv.second.info.width);
        if (right > maxX) {
            maxX = right;
        }
    }
    return maxX;
}

bool VirtualDisplayManager::ensureHeadlessDisplay(uint32_t width, uint32_t height, uint32_t refreshRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headlessId_ != 0) {
        auto it = displays_.find(headlessId_);
        if (it != displays_.end()) {
            return true; // Idempotent
        }
    }

    uint32_t id = nextId_++;
    SurfaceState st;
    st.info.id = id;
    st.info.width = (width > 0) ? width : 1920;
    st.info.height = (height > 0) ? height : 1080;
    st.info.refreshRate = (refreshRate > 0) ? refreshRate : 60;
    st.info.x = 0;
    st.info.y = 0;
    st.info.isHeadlessFallback = true;
    st.info.backend = VirtualBackend::Software;
    st.info.name = "Virtual Display 1 (Headless Fallback) [Virtual]";

    if (!allocateSoftwareSurface(st, st.info.width, st.info.height)) {
        return false;
    }

    renderSyntheticDesktopInternal(st.info, st.hdc, st.pBits, "Headless Display Active");
    displays_[id] = st;
    headlessId_ = id;
    return true;
}

void VirtualDisplayManager::releaseHeadlessDisplay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headlessId_ != 0) {
        auto it = displays_.find(headlessId_);
        if (it != displays_.end()) {
            releaseSoftwareSurface(it->second);
            displays_.erase(it);
        }
        headlessId_ = 0;
    }
}

bool VirtualDisplayManager::hasActiveHeadlessDisplay() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headlessId_ == 0) return false;
    return displays_.find(headlessId_) != displays_.end();
}

uint32_t VirtualDisplayManager::headlessDisplayId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return headlessId_;
}

bool VirtualDisplayManager::createVirtualDisplay(
    uint32_t width,
    uint32_t height,
    uint32_t refreshRate,
    bool preferIddCx,
    uint32_t* outDisplayId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t id = nextId_++;

    SurfaceState st;
    st.info.id = id;
    st.info.width = (width > 0) ? width : 1920;
    st.info.height = (height > 0) ? height : 1080;
    st.info.refreshRate = (refreshRate > 0) ? refreshRate : 60;
    st.info.x = computeNextVirtualX_Locked(st.info.width);
    st.info.y = 0;
    st.info.isHeadlessFallback = false;
    st.info.backend = (preferIddCx && hasIddCxDriver()) ? VirtualBackend::IddCx : VirtualBackend::Software;
    st.info.name = "Virtual Display " + std::to_string(displays_.size() + 1) + " (" +
                   std::to_string(st.info.width) + "x" + std::to_string(st.info.height) + ") [Virtual]";

    if (!allocateSoftwareSurface(st, st.info.width, st.info.height)) {
        return false;
    }

    renderSyntheticDesktopInternal(st.info, st.hdc, st.pBits, "");
    displays_[id] = st;
    if (outDisplayId) *outDisplayId = id;
    return true;
}

bool VirtualDisplayManager::destroyVirtualDisplay(uint32_t displayId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = displays_.find(displayId);
    if (it == displays_.end()) return false;

    releaseSoftwareSurface(it->second);
    displays_.erase(it);

    if (headlessId_ == displayId) {
        headlessId_ = 0;
    }
    return true;
}

bool VirtualDisplayManager::setVirtualDisplayMode(uint32_t displayId, uint32_t width, uint32_t height, uint32_t refreshRate) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = displays_.find(displayId);
    if (it == displays_.end()) return false;

    auto& s = it->second;
    s.info.width = (width > 0) ? width : s.info.width;
    s.info.height = (height > 0) ? height : s.info.height;
    if (refreshRate > 0) s.info.refreshRate = refreshRate;
    s.info.name = "Virtual Display " + std::to_string(displayId) + " (" +
                  std::to_string(s.info.width) + "x" + std::to_string(s.info.height) + ") [Virtual]";

    if (!allocateSoftwareSurface(s, s.info.width, s.info.height)) {
        return false;
    }
    renderSyntheticDesktopInternal(s.info, s.hdc, s.pBits, "");
    return true;
}

void VirtualDisplayManager::destroyAllSessionDisplays() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : displays_) {
        releaseSoftwareSurface(kv.second);
    }
    displays_.clear();
    headlessId_ = 0;
}

size_t VirtualDisplayManager::virtualDisplayCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return displays_.size();
}

std::vector<VirtualDisplayInfo> VirtualDisplayManager::activeDisplays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VirtualDisplayInfo> list;
    list.reserve(displays_.size());
    for (const auto& kv : displays_) {
        list.push_back(kv.second.info);
    }
    std::sort(list.begin(), list.end(), [](const VirtualDisplayInfo& a, const VirtualDisplayInfo& b) {
        return a.id < b.id;
    });
    return list;
}

bool VirtualDisplayManager::getDisplayInfo(uint32_t displayId, VirtualDisplayInfo& outInfo) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = displays_.find(displayId);
    if (it == displays_.end()) return false;
    outInfo = it->second.info;
    return true;
}

bool VirtualDisplayManager::captureSoftwareSurface(uint32_t displayId, std::vector<uint8_t>& outBgra, int& outW, int& outH) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = displays_.find(displayId);
    if (it == displays_.end() || !it->second.pBits) return false;

    const auto& s = it->second;
    outW = static_cast<int>(s.info.width);
    outH = static_cast<int>(s.info.height);
    uint64_t totalBytes64 = static_cast<uint64_t>(outW) * static_cast<uint64_t>(outH) * 4ULL;
    if (totalBytes64 == 0 || totalBytes64 > 128ULL * 1024ULL * 1024ULL) return false;
    size_t totalBytes = static_cast<size_t>(totalBytes64);
    if (outBgra.size() != totalBytes) {
        outBgra.resize(totalBytes);
    }
    std::memcpy(outBgra.data(), s.pBits, totalBytes);
    return true;
}

void VirtualDisplayManager::renderSyntheticDesktop(uint32_t displayId, const std::string& hostStatusText) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = displays_.find(displayId);
    if (it == displays_.end()) return;
    renderSyntheticDesktopInternal(it->second.info, it->second.hdc, it->second.pBits, hostStatusText);
}

} // namespace cppdesk
