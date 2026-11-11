#pragma once

// Platform-neutral drawing surface for the app's screens. The screens draw through Canvas; each platform
// supplies a backend (Direct2D on Windows), and RecordingCanvas records calls for tests. No platform headers.

#include "ui_platform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cppdesk::ui {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

inline Color rgba(uint8_t r, uint8_t g, uint8_t b, float a = 1.0f) {
    return Color{ r / 255.0f, g / 255.0f, b / 255.0f, a };
}

inline Color withAlpha(Color c, float aMultiplier) {
    return Color{ c.r, c.g, c.b, std::clamp(c.a * aMultiplier, 0.0f, 1.0f) };
}

inline Color lerpColor(const Color& a, const Color& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

// One per text style the screens use today.
enum class FontRole { HeroId, Heading, Subheading, Body, BodyBold, Small, Mono, Tooltip };
enum class HAlign { Leading, Center, Trailing };
enum class VAlign { Top, Center, Bottom };

using Rect = UiRect;

// 2D affine transform in the Direct2D Matrix3x2F convention: row vectors, x' = x*m11 + y*m21 + dx,
// y' = x*m12 + y*m22 + dy, and a*b applies a first.
struct Transform2D {
    float m11 = 1.0f, m12 = 0.0f;
    float m21 = 0.0f, m22 = 1.0f;
    float dx = 0.0f, dy = 0.0f;

    static Transform2D identity() { return {}; }

    static Transform2D translation(float x, float y) {
        Transform2D t;
        t.dx = x;
        t.dy = y;
        return t;
    }

    // Scale about (cx, cy).
    static Transform2D scale(float sx, float sy, float cx, float cy) {
        Transform2D t;
        t.m11 = sx;
        t.m22 = sy;
        t.dx = cx - sx * cx;
        t.dy = cy - sy * cy;
        return t;
    }

    // Rotate clockwise on screen (y down) about (cx, cy).
    static Transform2D rotation(float degrees, float cx, float cy) {
        const float rad = degrees * 3.14159265358979f / 180.0f;
        const float c = std::cos(rad), s = std::sin(rad);
        Transform2D t;
        t.m11 = c;
        t.m12 = s;
        t.m21 = -s;
        t.m22 = c;
        t.dx = cx - cx * c + cy * s;
        t.dy = cy - cx * s - cy * c;
        return t;
    }

    Transform2D operator*(const Transform2D& b) const {
        Transform2D r;
        r.m11 = m11 * b.m11 + m12 * b.m21;
        r.m12 = m11 * b.m12 + m12 * b.m22;
        r.m21 = m21 * b.m11 + m22 * b.m21;
        r.m22 = m21 * b.m12 + m22 * b.m22;
        r.dx = dx * b.m11 + dy * b.m21 + b.dx;
        r.dy = dx * b.m12 + dy * b.m22 + b.dy;
        return r;
    }
};

class Path {
public:
    enum class SegmentType { Move, Line, Arc, Close };
    struct Segment {
        SegmentType type = SegmentType::Move;
        float x = 0.0f;
        float y = 0.0f;
        float radius = 0.0f;
        bool clockwise = true;
        bool largeArc = false;
    };

    void moveTo(float x, float y) { segments_.push_back({ SegmentType::Move, x, y }); }
    void lineTo(float x, float y) { segments_.push_back({ SegmentType::Line, x, y }); }
    // Circular arc from the current point to (x, y).
    void arcTo(float x, float y, float radius, bool clockwise, bool largeArc) {
        segments_.push_back({ SegmentType::Arc, x, y, radius, clockwise, largeArc });
    }
    void close() { segments_.push_back({ SegmentType::Close }); }

    const std::vector<Segment>& segments() const { return segments_; }

private:
    std::vector<Segment> segments_;
};

// Handle to a bitmap owned by the canvas that created it. 0 is never a valid image.
struct ImageId {
    uint32_t value = 0;
    bool valid() const { return value != 0; }
};

class Canvas {
public:
    virtual ~Canvas() = default;

    // Frame. beginFrame resets the transform, clip and opacity stacks.
    virtual void beginFrame(float widthDip, float heightDip, Color clear) = 0;
    virtual void endFrame() = 0;

    // Shapes
    virtual void fillRect(Rect r, Color c) = 0;
    virtual void fillRoundRect(Rect r, float radius, Color c) = 0;
    virtual void strokeRoundRect(Rect r, float radius, Color c, float width) = 0;
    virtual void fillEllipse(float cx, float cy, float rx, float ry, Color c) = 0;
    virtual void strokeEllipse(float cx, float cy, float rx, float ry, Color c, float width) = 0;
    virtual void drawLine(float x0, float y0, float x1, float y1, Color c, float width) = 0;
    virtual void fillPath(const Path& p, Color c) = 0;
    virtual void strokePath(const Path& p, Color c, float width) = 0;

    // Text, clipped to the rect.
    virtual void drawText(std::string_view utf8, Rect r, FontRole font, Color c, HAlign h, VAlign v) = 0;
    virtual float measureTextWidth(std::string_view utf8, FontRole font) = 0;

    // Images (BGRA, premultiplied like the decoded remote frames)
    virtual ImageId createImage(int width, int height) = 0;
    virtual void updateImage(ImageId id, const uint8_t* bgra, int pitch, Rect dirty) = 0;
    virtual void drawImage(ImageId id, Rect dst, float opacity) = 0;
    virtual void destroyImage(ImageId id) = 0;

    // State
    virtual void pushClip(Rect r) = 0;
    virtual void popClip() = 0;
    virtual void pushOpacity(float opacity) = 0;
    virtual void popOpacity() = 0;
    virtual void setTransform(const Transform2D& t) = 0;
    virtual Transform2D transform() const = 0;

    // True when every clip and opacity pushed this frame has been popped.
    virtual bool frameBalanced() const = 0;
};

} // namespace cppdesk::ui
