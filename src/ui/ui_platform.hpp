#pragma once

#include "../core/protocol.hpp"
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace cppdesk {

struct UiPlatformRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    float centerX() const { return (left + right) * 0.5f; }
    float centerY() const { return (top + bottom) * 0.5f; }

    bool contains(float x, float y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }

    UiPlatformRect offset(float dx, float dy) const {
        return { left + dx, top + dy, right + dx, bottom + dy };
    }

    UiPlatformRect inflate(float dx, float dy) const {
        return { left - dx, top - dy, right + dx, bottom + dy };
    }
};

using UiRect = UiPlatformRect;

struct ViewportGeometry {
    UiPlatformRect contentRect{};
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    bool hasLetterbox = false;
    bool hasPillarbox = false;

    operator const UiPlatformRect&() const { return contentRect; }
    operator UiPlatformRect&() { return contentRect; }
};

struct ModifierTransition {
    uint32_t vkCode = 0;
    bool isDown = false;
};

class UiPlatform {
public:
    static ViewportGeometry calculateViewportGeometry(float viewWidth, float viewHeight, uint32_t frameWidth, uint32_t frameHeight, ScaleMode scaleMode);
    static bool mapPointToNormalized(float viewX, float viewY, const ViewportGeometry& geo, float& outNormX, float& outNormY);
    static bool mapPointToNormalized(float x, float y, const UiPlatformRect& canvasRect, float& outNormX, float& outNormY);
    static void mapNormalizedToPoint(float normX, float normY, const ViewportGeometry& geo, float& outViewX, float& outViewY);
    static void mapNormalizedToPoint(float normX, float normY, const UiPlatformRect& canvasRect, float& outViewX, float& outViewY);

    static uint32_t macKeyCodeToWindowsVk(uint16_t macKeyCode);
    static uint16_t windowsVkToMacKeyCode(uint32_t vkCode);

    static std::vector<ModifierTransition> decodeModifierTransitions(uint64_t oldMask, uint64_t newMask);
    static std::vector<ModifierTransition> decodeModifierTransitions(uint32_t oldMask, uint32_t newMask);

    static bool blitTileToCanvas(uint8_t* dstCanvas, int canvasWidth, int canvasHeight, int canvasPitch,
                                const uint8_t* srcTile, int tileX, int tileY, int tileWidth, int tileHeight, int tilePitch);

    // Second-order damped harmonic oscillator (macOS CoreAnimation CASpringAnimation physics)
    static bool stepSpring(float& pos, float& vel, float target, float omega, float zeta, float dt, float eps = 0.0012f);

    // Compute MacBook-style tactile spring transformed rect (hover expands 2.8%, lifts -1.8px; press squishes 4.8%, depresses +1.6px)
    static UiPlatformRect computeSpringRect(const UiPlatformRect& base, float hoverT, float pressT);
};

inline ViewportGeometry calculateViewportGeometry(float viewWidth, float viewHeight, uint32_t frameWidth, uint32_t frameHeight, ScaleMode scaleMode) {
    return UiPlatform::calculateViewportGeometry(viewWidth, viewHeight, frameWidth, frameHeight, scaleMode);
}

inline bool mapPointToNormalized(float viewX, float viewY, const ViewportGeometry& geo, float& outNormX, float& outNormY) {
    return UiPlatform::mapPointToNormalized(viewX, viewY, geo, outNormX, outNormY);
}

inline bool mapPointToNormalized(float x, float y, const UiPlatformRect& canvasRect, float& outNormX, float& outNormY) {
    return UiPlatform::mapPointToNormalized(x, y, canvasRect, outNormX, outNormY);
}

inline void mapNormalizedToPoint(float normX, float normY, const ViewportGeometry& geo, float& outViewX, float& outViewY) {
    UiPlatform::mapNormalizedToPoint(normX, normY, geo, outViewX, outViewY);
}

inline void mapNormalizedToPoint(float normX, float normY, const UiPlatformRect& canvasRect, float& outViewX, float& outViewY) {
    UiPlatform::mapNormalizedToPoint(normX, normY, canvasRect, outViewX, outViewY);
}

inline uint32_t macKeyCodeToWindowsVk(uint16_t macKeyCode) {
    return UiPlatform::macKeyCodeToWindowsVk(macKeyCode);
}

inline uint16_t windowsVkToMacKeyCode(uint32_t vkCode) {
    return UiPlatform::windowsVkToMacKeyCode(vkCode);
}

inline std::vector<ModifierTransition> decodeModifierTransitions(uint64_t oldMask, uint64_t newMask) {
    return UiPlatform::decodeModifierTransitions(oldMask, newMask);
}

inline std::vector<ModifierTransition> decodeModifierTransitions(uint32_t oldMask, uint32_t newMask) {
    return UiPlatform::decodeModifierTransitions(oldMask, newMask);
}

inline bool blitTileToCanvas(uint8_t* dstCanvas, int canvasWidth, int canvasHeight, int canvasPitch,
                            const uint8_t* srcTile, int tileX, int tileY, int tileWidth, int tileHeight, int tilePitch) {
    return UiPlatform::blitTileToCanvas(dstCanvas, canvasWidth, canvasHeight, canvasPitch, srcTile, tileX, tileY, tileWidth, tileHeight, tilePitch);
}

} // namespace cppdesk
