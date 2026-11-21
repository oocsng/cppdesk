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

class UiPlatform {
public:
    static ViewportGeometry calculateViewportGeometry(float viewWidth, float viewHeight, uint32_t frameWidth, uint32_t frameHeight, ScaleMode scaleMode);
    static bool mapPointToNormalized(float viewX, float viewY, const ViewportGeometry& geo, float& outNormX, float& outNormY);
    static bool mapPointToNormalized(float x, float y, const UiPlatformRect& canvasRect, float& outNormX, float& outNormY);
    static void mapNormalizedToPoint(float normX, float normY, const ViewportGeometry& geo, float& outViewX, float& outViewY);
    static void mapNormalizedToPoint(float normX, float normY, const UiPlatformRect& canvasRect, float& outViewX, float& outViewY);

    // Second-order damped harmonic oscillator (macOS CoreAnimation CASpringAnimation physics)
    static bool stepSpring(float& pos, float& vel, float target, float omega, float zeta, float dt, float eps = 0.0012f);

    // stepSpring under the app's motion rules: never overshoots (damping raised to critical), a spring heading to 0
    // (an exit) runs 25% faster, and with reduced motion the value jumps straight to the target.
    static bool stepMotion(float& pos, float& vel, float target, float omega, float zeta, float dt, float eps, bool reduced);

    // A pressed control shrinks to 97% around its centre; hover changes colour only, never size.
    static UiPlatformRect computeSpringRect(const UiPlatformRect& base, float pressT);
    // A popover at progress t (0..1) scaled from 95% around the point it opens from (its trigger).
    static UiPlatformRect popoverRect(const UiPlatformRect& base, float t, float originX, float originY);

    // The X resource Xft.dpi as a UI scale ("192" -> 2.0); 1.0 when missing or invalid.
    static float dpiScaleFromXftDpi(const char* value);
    // Local paths from a text/uri-list (file drops): file:// entries only, percent-decoded.
    static std::vector<std::string> parseFileUriList(const std::string& uriList);
    // True only for http:// or https:// URLs with a non-empty remainder and no whitespace or control characters.
    static bool isSafeExternalUrl(const std::string& url);
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

} // namespace cppdesk
