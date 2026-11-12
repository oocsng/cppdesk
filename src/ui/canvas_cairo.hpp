#pragma once

// Cairo / Pango backend for the platform-neutral Canvas (Linux).

#include "canvas.hpp"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cppdesk::ui {

class CairoCanvas : public Canvas {
public:
    CairoCanvas();
    ~CairoCanvas() override;

    CairoCanvas(const CairoCanvas&) = delete;
    CairoCanvas& operator=(const CairoCanvas&) = delete;

    // Draw into a borrowed ARGB32 or RGB24 image surface; layout units are DIPs, scaled by dpiScale.
    void attach(cairo_surface_t* target, float dpiScale);
    // Frees every image; their ImageIds become invalid.
    void detach();

    void beginFrame(float widthDip, float heightDip, Color clear) override;
    void endFrame() override;

    void fillRect(Rect r, Color c) override;
    void fillRoundRect(Rect r, float radius, Color c) override;
    void strokeRoundRect(Rect r, float radius, Color c, float width) override;
    void fillEllipse(float cx, float cy, float rx, float ry, Color c) override;
    void strokeEllipse(float cx, float cy, float rx, float ry, Color c, float width) override;
    void drawLine(float x0, float y0, float x1, float y1, Color c, float width) override;
    void fillPath(const Path& p, Color c) override;
    void strokePath(const Path& p, Color c, float width) override;

    void drawText(std::string_view utf8, Rect r, FontRole font, Color c, HAlign h, VAlign v) override;
    float measureTextWidth(std::string_view utf8, FontRole font) override;
    Point measureTextBlock(std::string_view utf8, FontRole font, float maxWidth) override;

    ImageId createImage(int width, int height, bool opaque = false) override;
    void updateImage(ImageId id, const uint8_t* bgra, int pitch, Rect dirty) override;
    void drawImage(ImageId id, Rect dst, float opacity) override;
    void destroyImage(ImageId id) override;

    void pushClip(Rect r) override;
    void popClip() override;
    void pushOpacity(float opacity) override;
    void popOpacity() override;
    void setTransform(const Transform2D& t) override;
    Transform2D transform() const override { return transform_; }

    bool frameBalanced() const override { return stack_.empty(); }

private:
    struct ImageEntry {
        cairo_surface_t* surface = nullptr;
        int width = 0;
        int height = 0;
    };
    enum class Pushed { Clip, Opacity };

    void setColor(Color c);
    void roundRectPath(Rect r, float radius);
    void buildPath(const Path& p);
    void pop(Pushed kind, float opacity);
    PangoLayout* layout(cairo_t* cr, std::string_view utf8, FontRole font) const;

    cairo_surface_t* target_ = nullptr;
    cairo_t* cr_ = nullptr;
    cairo_surface_t* measureSurface_ = nullptr;   // 1x1, so text can be measured outside a frame
    cairo_t* measureCr_ = nullptr;
    float scale_ = 1.0f;
    std::array<PangoFontDescription*, 8> fonts_{};
    std::unordered_map<uint32_t, ImageEntry> images_;
    uint32_t nextImage_ = 1;
    Transform2D transform_{};
    std::vector<std::pair<Pushed, float>> stack_;   // clips and opacity layers in push order
};

} // namespace cppdesk::ui
