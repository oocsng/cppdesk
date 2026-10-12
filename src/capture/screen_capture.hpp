#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "../core/protocol.hpp"
#include "virtual_display_manager.hpp"
#include "privacy_cloak_manager.hpp"

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace cppdesk {

struct CursorState {
    float normX = 0.0f;
    float normY = 0.0f;
    bool  visible = true;
};

class TileCodec {
public:
    static void initGdiPlus();
    static void shutdownGdiPlus();

    // Encode a contiguous BGRA rectangle (width * height * 4 bytes) into an EncodedTile
    static EncodedTile encodeRect(
        uint16_t x,
        uint16_t y,
        uint16_t width,
        uint16_t height,
        const uint8_t* bgraRect,
        QualityPreset preset);

    // Decode an EncodedTile and blit it into a full-frame BGRA canvas (canvasW * canvasH * 4)
    static bool decodeTileIntoCanvas(
        const EncodedTile& tile,
        uint8_t* canvasBgra,
        int canvasW,
        int canvasH);

    // Raw-pointer zero-copy overload decoding directly from network stream without intermediate EncodedTile allocations
    static bool decodeTileIntoCanvas(
        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
        TileEncoding encoding,
        const uint8_t* tileData, size_t dataSize,
        uint8_t* canvasBgra,
        int canvasW,
        int canvasH);

    // Compress/decompress helpers exposed for testing
    static std::vector<uint8_t> compressZstd(const void* src, size_t srcLen, int level = 1);
    static bool decompressZstd(const void* src, size_t srcLen, void* dst, size_t dstCapacity);
    static std::vector<uint8_t> encodeJpeg(const uint8_t* bgra, int width, int height, int quality);
    static bool decodeJpeg(const uint8_t* jpegData, size_t jpegLen, std::vector<uint8_t>& outBgra, int& outW, int& outH);
    static bool decodeJpegIntoCanvas(
        const uint8_t* jpegData, size_t jpegLen,
        uint8_t* canvasBgra, int canvasW, int canvasH,
        uint16_t x, uint16_t y, uint16_t width, uint16_t height);
};

// ---------------- TileThreadPool (Option 2A) ----------------
// High-performance CPU worker thread pool for parallel tile compression
class TileThreadPool {
public:
    static TileThreadPool& instance();

    TileThreadPool();
    ~TileThreadPool();

    TileThreadPool(const TileThreadPool&) = delete;
    TileThreadPool& operator=(const TileThreadPool&) = delete;

    struct RectTask {
        uint16_t rx = 0;
        uint16_t ry = 0;
        uint16_t rw = 0;
        uint16_t rh = 0;
        std::vector<uint8_t> bgraPixels;
        const uint8_t* frameBase = nullptr;
        int frameStride = 0;
        QualityPreset preset = QualityPreset::Balanced;
        EncodedTile result;
    };

    struct DecodeTask {
        uint16_t rx = 0;
        uint16_t ry = 0;
        uint16_t rw = 0;
        uint16_t rh = 0;
        TileEncoding encoding = TileEncoding::RawBGRA;
        const uint8_t* data = nullptr;
        size_t dataSize = 0;
        uint8_t* canvasBgra = nullptr;
        int canvasW = 0;
        int canvasH = 0;
    };

    void parallelEncode(std::vector<RectTask>& tasks);
    void parallelDecode(std::vector<DecodeTask>& tasks);

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::mutex               mutex_;
    std::mutex               dispatchMutex_;
    std::condition_variable  cvTask_;
    std::condition_variable  cvDone_;
    std::atomic<bool>        stop_{false};

    std::vector<RectTask*>   activeBatch_;
    std::vector<DecodeTask*> activeDecodeBatch_;
    std::atomic<size_t>      nextTaskIdx_{0};
    std::atomic<size_t>      remainingTasks_{0};
};

class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    // Enumerate all connected displays
    std::vector<MonitorDesc> enumerateMonitors();

    // Select active monitor index (0-based)
    bool selectMonitor(int monitorIndex);
    int currentMonitorIndex() const { return activeMonitorIdx_; }
    MonitorDesc currentMonitor() const { return activeMonitor_; }

    // Capture current screen and produce dirty tiles since previous frame
    bool captureDirtyTiles(
        bool forceKeyframe,
        QualityPreset preset,
        std::vector<EncodedTile>& outTiles,
        bool& outIsKeyframe,
        CursorState& outCursor);

    enum class DxgiRecoveryState { Active, FallbackGdi, Disabled };
    DxgiRecoveryState dxgiRecoveryState() const { return dxgiRecoveryState_; }
    void triggerDxgiAccessLostForTest();

    // Raw frame access for tests
    int frameWidth() const { return frameW_; }
    int frameHeight() const { return frameH_; }
    const std::vector<uint8_t>& currentFrameBgra() const { return currentFrame_; }
    bool usingDxgi() const { return dxgiInitialized_ && dxgiRecoveryState_ == DxgiRecoveryState::Active; }

    // Telemetry latency metrics
    float lastCaptureLatencyMs() const { return lastCaptureLatencyMs_; }
    float lastEncodeLatencyMs() const { return lastEncodeLatencyMs_; }
    bool hasGdiCachedResources() const { return gdiMemDC_ != nullptr && gdiSection_ != nullptr; }

    // Headless auto-provisioning toggle
    void setAutoHeadless(bool enable) { autoHeadless_ = enable; }
    bool autoHeadless() const { return autoHeadless_; }

    // Privacy Cloak manager
    PrivacyCloakManager& privacyCloak() { return privacyCloak_; }
    const PrivacyCloakManager& privacyCloak() const { return privacyCloak_; }

private:
    bool initDxgiForMonitor(int monitorIndex);
    void releaseDxgi();
    void releaseGdiResources();
    bool captureViaDxgi(bool& outFrameUpdated, std::vector<RECT>& outDirtyRects, bool& outHasExplicitDirtyRects);
    bool captureViaGdi();
    bool captureViaCoreGraphics();
    CursorState captureCursorState() const;

    struct DxgiImpl;
    std::unique_ptr<DxgiImpl> dxgi_;
    bool                      dxgiInitialized_ = false;
    DxgiRecoveryState         dxgiRecoveryState_ = DxgiRecoveryState::Active;
    uint64_t                  lastDxgiAttemptTick_ = 0;
    bool                      hasValidFrame_ = false;

    // GDI handle recycling cache
#if defined(_WIN32)
    HDC                       gdiMemDC_ = nullptr;
    HBITMAP                   gdiSection_ = nullptr;
#else
    void*                     gdiMemDC_ = nullptr;
    void*                     gdiSection_ = nullptr;
#endif
    void*                     gdiBits_ = nullptr;
    int                       gdiWidth_ = 0;
    int                       gdiHeight_ = 0;

    std::vector<MonitorDesc>  monitors_;
    int                       activeMonitorIdx_ = 0;
    MonitorDesc               activeMonitor_;

    int                       frameW_ = 0;
    int                       frameH_ = 0;
    std::vector<uint8_t>      currentFrame_;
    std::vector<uint64_t>     prevTileHashes_;
    float                     lastCaptureLatencyMs_ = 0.0f;
    float                     lastEncodeLatencyMs_ = 0.0f;

    // Reusable working vectors avoiding per-frame heap churn
    std::vector<uint8_t>                  reusableDirtyMask_;
    std::vector<uint8_t>                  reusableCandidateTiles_;
    std::vector<TileThreadPool::RectTask> reusableTasks_;
    bool                                  autoHeadless_ = true;
    PrivacyCloakManager                   privacyCloak_;
};

} // namespace cppdesk
