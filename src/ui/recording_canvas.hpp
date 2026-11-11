#pragma once

// A Canvas that records every call as one line of text, for headless screen tests and golden comparisons.
// Floats are printed with two decimals so the output is stable across runs and compilers.

#include "canvas.hpp"

#include <string>
#include <vector>

namespace cppdesk::ui {

class RecordingCanvas : public Canvas {
public:
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
    // Deterministic stand-in: a fixed advance per code point, scaled by the font role.
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

    bool frameBalanced() const override { return clipDepth_ == 0 && opacityDepth_ == 0; }

    const std::vector<std::string>& ops() const { return ops_; }
    std::string serialize() const;

private:
    void record(std::string op) { ops_.push_back(std::move(op)); }

    std::vector<std::string> ops_;
    Transform2D transform_{};
    int clipDepth_ = 0;
    int opacityDepth_ = 0;
    uint32_t nextImage_ = 1;
};

} // namespace cppdesk::ui
