#include "recording_canvas.hpp"

#include <cstdio>

namespace cppdesk::ui {

namespace {

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    return buf;
}

std::string rect(Rect r) {
    return num(r.left) + " " + num(r.top) + " " + num(r.right) + " " + num(r.bottom);
}

std::string color(Color c) {
    return "c" + num(c.r) + "," + num(c.g) + "," + num(c.b) + "," + num(c.a);
}

const char* fontName(FontRole f) {
    switch (f) {
        case FontRole::HeroId: return "HeroId";
        case FontRole::Heading: return "Heading";
        case FontRole::Subheading: return "Subheading";
        case FontRole::Body: return "Body";
        case FontRole::BodyBold: return "BodyBold";
        case FontRole::Small: return "Small";
        case FontRole::Mono: return "Mono";
        case FontRole::Tooltip: return "Tooltip";
    }
    return "?";
}

const char* hName(HAlign h) {
    return h == HAlign::Leading ? "L" : (h == HAlign::Center ? "C" : "T");
}

const char* vName(VAlign v) {
    return v == VAlign::Top ? "T" : (v == VAlign::Center ? "C" : "B");
}

std::string path(const Path& p) {
    std::string out;
    for (const auto& s : p.segments()) {
        switch (s.type) {
            case Path::SegmentType::Move: out += " M" + num(s.x) + "," + num(s.y); break;
            case Path::SegmentType::Line: out += " L" + num(s.x) + "," + num(s.y); break;
            case Path::SegmentType::Arc:
                out += " A" + num(s.x) + "," + num(s.y) + "," + num(s.radius) + (s.clockwise ? ",cw" : ",ccw") +
                       (s.largeArc ? ",large" : ",small");
                break;
            case Path::SegmentType::Close: out += " Z"; break;
        }
    }
    return out;
}

} // namespace

void RecordingCanvas::beginFrame(float widthDip, float heightDip, Color clear) {
    ops_.clear();
    transform_ = Transform2D::identity();
    clipDepth_ = 0;
    opacityDepth_ = 0;
    record("beginFrame " + num(widthDip) + " " + num(heightDip) + " " + color(clear));
}

void RecordingCanvas::endFrame() {
    record("endFrame");
}

void RecordingCanvas::fillRect(Rect r, Color c) {
    record("fillRect " + rect(r) + " " + color(c));
}

void RecordingCanvas::fillRoundRect(Rect r, float radius, Color c) {
    record("fillRoundRect " + rect(r) + " r" + num(radius) + " " + color(c));
}

void RecordingCanvas::strokeRoundRect(Rect r, float radius, Color c, float width) {
    record("strokeRoundRect " + rect(r) + " r" + num(radius) + " " + color(c) + " w" + num(width));
}

void RecordingCanvas::fillEllipse(float cx, float cy, float rx, float ry, Color c) {
    record("fillEllipse " + num(cx) + " " + num(cy) + " " + num(rx) + " " + num(ry) + " " + color(c));
}

void RecordingCanvas::strokeEllipse(float cx, float cy, float rx, float ry, Color c, float width) {
    record("strokeEllipse " + num(cx) + " " + num(cy) + " " + num(rx) + " " + num(ry) + " " + color(c) + " w" + num(width));
}

void RecordingCanvas::drawLine(float x0, float y0, float x1, float y1, Color c, float width) {
    record("drawLine " + num(x0) + " " + num(y0) + " " + num(x1) + " " + num(y1) + " " + color(c) + " w" + num(width));
}

void RecordingCanvas::fillPath(const Path& p, Color c) {
    record("fillPath" + path(p) + " " + color(c));
}

void RecordingCanvas::strokePath(const Path& p, Color c, float width) {
    record("strokePath" + path(p) + " " + color(c) + " w" + num(width));
}

void RecordingCanvas::drawText(std::string_view utf8, Rect r, FontRole font, Color c, HAlign h, VAlign v) {
    record("drawText " + rect(r) + " " + fontName(font) + " " + hName(h) + vName(v) + " " + color(c) + " |" +
           std::string(utf8));
}

float RecordingCanvas::measureTextWidth(std::string_view utf8, FontRole font) {
    float advance = 7.0f;
    switch (font) {
        case FontRole::HeroId: advance = 18.0f; break;
        case FontRole::Heading: advance = 10.0f; break;
        case FontRole::Subheading: advance = 8.5f; break;
        case FontRole::Small: case FontRole::Tooltip: advance = 6.0f; break;
        default: break;
    }
    size_t codePoints = 0;
    for (unsigned char ch : utf8) {
        if ((ch & 0xC0) != 0x80) ++codePoints;
    }
    return advance * static_cast<float>(codePoints);
}

Point RecordingCanvas::measureTextBlock(std::string_view utf8, FontRole font, float maxWidth) {
    float w = measureTextWidth(utf8, font);
    if (maxWidth <= 0.0f || w <= maxWidth) return Point{ w, utf8.empty() ? 0.0f : 16.0f };
    float lines = std::ceil(w / maxWidth);
    return Point{ maxWidth, lines * 16.0f };
}

ImageId RecordingCanvas::createImage(int width, int height, bool opaque) {
    ImageId id{ nextImage_++ };
    record("createImage " + std::to_string(id.value) + " " + std::to_string(width) + "x" + std::to_string(height) +
           (opaque ? " opaque" : ""));
    return id;
}

void RecordingCanvas::updateImage(ImageId id, const uint8_t*, int, Rect dirty) {
    record("updateImage " + std::to_string(id.value) + " " + rect(dirty));
}

void RecordingCanvas::drawImage(ImageId id, Rect dst, float opacity) {
    record("drawImage " + std::to_string(id.value) + " " + rect(dst) + " o" + num(opacity));
}

void RecordingCanvas::destroyImage(ImageId id) {
    record("destroyImage " + std::to_string(id.value));
}

void RecordingCanvas::pushClip(Rect r) {
    ++clipDepth_;
    record("pushClip " + rect(r));
}

void RecordingCanvas::popClip() {
    if (clipDepth_ > 0) --clipDepth_;
    record("popClip");
}

void RecordingCanvas::pushOpacity(float opacity) {
    ++opacityDepth_;
    record("pushOpacity " + num(opacity));
}

void RecordingCanvas::popOpacity() {
    if (opacityDepth_ > 0) --opacityDepth_;
    record("popOpacity");
}

void RecordingCanvas::setTransform(const Transform2D& t) {
    transform_ = t;
    record("setTransform " + num(t.m11) + " " + num(t.m12) + " " + num(t.m21) + " " + num(t.m22) + " " + num(t.dx) +
           " " + num(t.dy));
}

std::string RecordingCanvas::serialize() const {
    std::string out;
    for (const auto& op : ops_) {
        out += op;
        out += '\n';
    }
    return out;
}

} // namespace cppdesk::ui
