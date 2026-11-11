#pragma once

// Direct2D / DirectWrite backend for the platform-neutral Canvas (Windows only).

#include "canvas.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace cppdesk::ui {

class D2DCanvas : public Canvas {
public:
    // The factories are borrowed and must outlive the canvas. Text formats are created here; they do not
    // depend on a device, so they survive detach().
    D2DCanvas(ID2D1Factory* d2d, IDWriteFactory* dwrite);
    ~D2DCanvas() override;

    D2DCanvas(const D2DCanvas&) = delete;
    D2DCanvas& operator=(const D2DCanvas&) = delete;

    // Binds a render target (borrowed). Device resources are created for it.
    void attach(ID2D1RenderTarget* rt, float dpiScale);
    // Releases every device resource, including all images; their ImageIds become invalid.
    void detach();
    // True once after endFrame() saw D2DERR_RECREATE_TARGET; the owner must recreate the target and re-attach.
    bool consumeRecreateNeeded();

    // The DirectWrite format for a role, for the few callers that still build their own text layouts.
    IDWriteTextFormat* format(FontRole font) const { return formats_[static_cast<size_t>(font)]; }
    ID2D1RenderTarget* renderTarget() const { return rt_; }

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

    ImageId createImage(int width, int height) override;
    void updateImage(ImageId id, const uint8_t* bgra, int pitch, Rect dirty) override;
    void drawImage(ImageId id, Rect dst, float opacity) override;
    void destroyImage(ImageId id) override;

    void pushClip(Rect r) override;
    void popClip() override;
    void pushOpacity(float opacity) override;
    void popOpacity() override;
    void setTransform(const Transform2D& t) override;
    Transform2D transform() const override { return transform_; }

    bool frameBalanced() const override { return clipDepth_ == 0 && layerDepth_ == 0; }

private:
    struct ImageEntry {
        ID2D1Bitmap* bitmap = nullptr;
        int width = 0;
        int height = 0;
    };

    ID2D1SolidColorBrush* brush(Color c);
    ID2D1PathGeometry* buildGeometry(const Path& p, bool filled) const;
    void unwindState();

    ID2D1Factory* d2d_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    std::array<IDWriteTextFormat*, 8> formats_{};
    std::unordered_map<uint32_t, ImageEntry> images_;
    uint32_t nextImage_ = 1;
    Transform2D transform_{};
    int clipDepth_ = 0;
    int layerDepth_ = 0;
    bool inFrame_ = false;
    bool recreateNeeded_ = false;
};

} // namespace cppdesk::ui
