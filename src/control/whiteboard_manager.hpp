#pragma once

#include "../core/protocol.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#endif

#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <string>
#include <chrono>

namespace cppdesk {

struct AnnotationStroke {
    uint32_t                strokeId = 0;
    WhiteboardTool          tool = WhiteboardTool::Pen;
    uint32_t                argbColor = 0xFFFF3B30; // Default vivid red
    float                   thickness = 3.5f;
    std::vector<WhiteboardPoint> points;
    uint64_t                timestampMs = 0;
};

class WhiteboardManager {
public:
    WhiteboardManager();
    ~WhiteboardManager();

    WhiteboardManager(const WhiteboardManager&) = delete;
    WhiteboardManager& operator=(const WhiteboardManager&) = delete;

    // Local Drawing State
    void setActiveTool(WhiteboardTool tool) { activeTool_ = tool; }
    WhiteboardTool activeTool() const { return activeTool_; }

    void setActiveColor(uint32_t argb) { activeColor_ = argb; }
    uint32_t activeColor() const { return activeColor_; }

    void setActiveThickness(float t) { activeThickness_ = t; }
    float activeThickness() const { return activeThickness_; }

    void startStroke(float normX, float normY);
    void addStrokePoint(float normX, float normY);
    AnnotationStroke finishStroke();
    void clearAllStrokes();

    // Ingesting Strokes from Remote Peer
    void applyRemoteStroke(const AnnotationStroke& stroke);
    void applyRemoteClear();

    // Laser pointer coordinate (temporary decaying dot)
    void setLaserPointer(float normX, float normY);
    bool getLaserPointer(float& outX, float& outY, float& outAlpha) const;

    // Snapshot of active strokes for rendering
    std::vector<AnnotationStroke> snapshotStrokes() const;

    // Packet Serialization & Deserialization
    static std::vector<uint8_t> serializeStroke(const AnnotationStroke& stroke);
    static bool deserializeStroke(const uint8_t* data, size_t len, AnnotationStroke& outStroke);
    static std::vector<uint8_t> serializeClearPacket();

    // Host-Side Transparent Click-Through Overlay Window (Option 3 & 4)
    bool isHostOverlayActive() const { return hwndOverlay_ != nullptr; }
    void showHostOverlay();
    void hideHostOverlay();
    void updateHostOverlayCanvas();

private:
#if defined(_WIN32)
    static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

    WhiteboardTool          activeTool_ = WhiteboardTool::Pen;
    uint32_t                activeColor_ = 0xFFFF3B30;
    float                   activeThickness_ = 3.5f;

    mutable std::mutex      strokesMutex_;
    std::vector<AnnotationStroke> strokes_;
    AnnotationStroke        currentStroke_;
    bool                    isDrawing_ = false;
    uint32_t                nextStrokeId_ = 1;

    // Laser pointer decay tracking
    mutable std::mutex      laserMutex_;
    float                   laserNormX_ = -1.0f;
    float                   laserNormY_ = -1.0f;
    uint64_t                laserTimestampMs_ = 0;

    // Host click-through transparent layered window
#if defined(_WIN32)
    HWND                    hwndOverlay_ = nullptr;
#else
    void*                   hwndOverlay_ = nullptr;
#endif
};

} // namespace cppdesk
