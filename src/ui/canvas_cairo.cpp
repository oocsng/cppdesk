#include "canvas_cairo.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cppdesk::ui {

namespace {

constexpr float kPi = 3.14159265358979f;

cairo_matrix_t toCairo(const Transform2D& t) {
    cairo_matrix_t m;
    cairo_matrix_init(&m, t.m11, t.m12, t.m21, t.m22, t.dx, t.dy);
    return m;
}

// Angle swept going clockwise on screen (increasing angle, y down) from a0 to a1, in (0, 2pi].
float clockwiseSweep(float a0, float a1) {
    float s = a1 - a0;
    while (s <= 0.0f) s += 2.0f * kPi;
    while (s > 2.0f * kPi) s -= 2.0f * kPi;
    return s;
}

} // namespace

CairoCanvas::CairoCanvas() {
    // Segoe UI and Consolas first, so a Linux box that has them looks like Windows; then close relatives.
    const char* uiFamilies = "Segoe UI,Inter,Noto Sans,Ubuntu,Cantarell,DejaVu Sans,Sans";
    const char* monoFamilies = "Consolas,DejaVu Sans Mono,Noto Sans Mono,Monospace";
    const theme::type::Style styles[8] = { theme::type::hero, theme::type::heading, theme::type::subheading,
                                           theme::type::body, theme::type::bodyBold, theme::type::caption,
                                           theme::type::mono, theme::type::tooltip };
    for (size_t i = 0; i < fonts_.size(); ++i) {
        fonts_[i] = pango_font_description_new();
        pango_font_description_set_family(fonts_[i], i == static_cast<size_t>(FontRole::Mono) ? monoFamilies : uiFamilies);
        pango_font_description_set_weight(fonts_[i], static_cast<PangoWeight>(styles[i].weight));
        pango_font_description_set_absolute_size(fonts_[i], styles[i].size * PANGO_SCALE);
    }
    measureSurface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    measureCr_ = cairo_create(measureSurface_);
}

CairoCanvas::~CairoCanvas() {
    detach();
    for (auto* f : fonts_) pango_font_description_free(f);
    cairo_destroy(measureCr_);
    cairo_surface_destroy(measureSurface_);
}

void CairoCanvas::attach(cairo_surface_t* target, float dpiScale) {
    detach();
    target_ = target;
    scale_ = dpiScale > 0.0f ? dpiScale : 1.0f;
    // Measure at the scale text is drawn at, so hinting matches.
    cairo_identity_matrix(measureCr_);
    cairo_scale(measureCr_, scale_, scale_);
}

void CairoCanvas::detach() {
    if (cr_) endFrame();
    for (auto& kv : images_) cairo_surface_destroy(kv.second.surface);
    images_.clear();
    target_ = nullptr;
    transform_ = Transform2D::identity();
    stack_.clear();
}

void CairoCanvas::beginFrame(float, float, Color clear) {
    if (!target_) return;
    if (cr_) endFrame();
    cr_ = cairo_create(target_);
    transform_ = Transform2D::identity();
    stack_.clear();
    setTransform(transform_);
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE);
    setColor(clear);
    cairo_paint(cr_);
    cairo_restore(cr_);
}

void CairoCanvas::endFrame() {
    if (!cr_) return;
    while (!stack_.empty()) pop(stack_.back().first, stack_.back().second);
    cairo_destroy(cr_);
    cr_ = nullptr;
    cairo_surface_flush(target_);
}

void CairoCanvas::setColor(Color c) {
    cairo_set_source_rgba(cr_, c.r, c.g, c.b, c.a);
}

void CairoCanvas::fillRect(Rect r, Color c) {
    if (!cr_) return;
    cairo_rectangle(cr_, r.left, r.top, r.right - r.left, r.bottom - r.top);
    setColor(c);
    cairo_fill(cr_);
}

void CairoCanvas::roundRectPath(Rect r, float radius) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    radius = std::clamp(radius, 0.0f, std::min(w, h) / 2.0f);
    cairo_new_sub_path(cr_);
    cairo_arc(cr_, r.right - radius, r.top + radius, radius, -kPi / 2, 0);
    cairo_arc(cr_, r.right - radius, r.bottom - radius, radius, 0, kPi / 2);
    cairo_arc(cr_, r.left + radius, r.bottom - radius, radius, kPi / 2, kPi);
    cairo_arc(cr_, r.left + radius, r.top + radius, radius, kPi, 3 * kPi / 2);
    cairo_close_path(cr_);
}

void CairoCanvas::fillRoundRect(Rect r, float radius, Color c) {
    if (!cr_) return;
    roundRectPath(r, radius);
    setColor(c);
    cairo_fill(cr_);
}

void CairoCanvas::strokeRoundRect(Rect r, float radius, Color c, float width) {
    if (!cr_) return;
    roundRectPath(r, radius);
    setColor(c);
    cairo_set_line_width(cr_, width);
    cairo_stroke(cr_);
}

void CairoCanvas::fillEllipse(float cx, float cy, float rx, float ry, Color c) {
    if (!cr_ || rx <= 0.0f || ry <= 0.0f) return;
    cairo_save(cr_);
    cairo_translate(cr_, cx, cy);
    cairo_scale(cr_, rx, ry);
    cairo_arc(cr_, 0, 0, 1, 0, 2 * kPi);
    cairo_restore(cr_);
    setColor(c);
    cairo_fill(cr_);
}

void CairoCanvas::strokeEllipse(float cx, float cy, float rx, float ry, Color c, float width) {
    if (!cr_ || rx <= 0.0f || ry <= 0.0f) return;
    cairo_save(cr_);
    cairo_translate(cr_, cx, cy);
    cairo_scale(cr_, rx, ry);
    cairo_arc(cr_, 0, 0, 1, 0, 2 * kPi);
    cairo_restore(cr_);   // the path stays in device space; stroke with an unscaled pen
    setColor(c);
    cairo_set_line_width(cr_, width);
    cairo_stroke(cr_);
}

void CairoCanvas::drawLine(float x0, float y0, float x1, float y1, Color c, float width) {
    if (!cr_) return;
    cairo_move_to(cr_, x0, y0);
    cairo_line_to(cr_, x1, y1);
    setColor(c);
    cairo_set_line_width(cr_, width);
    cairo_stroke(cr_);
}

void CairoCanvas::buildPath(const Path& p) {
    float curX = 0.0f, curY = 0.0f;
    bool open = false;
    for (const auto& s : p.segments()) {
        switch (s.type) {
            case Path::SegmentType::Move:
                cairo_move_to(cr_, s.x, s.y);
                open = true;
                break;
            case Path::SegmentType::Line:
                if (open) cairo_line_to(cr_, s.x, s.y);
                break;
            case Path::SegmentType::Arc: {
                if (!open) break;
                // Direct2D/SVG endpoint arc (rx = ry, no rotation) -> centre and angles for cairo_arc.
                const float dx = s.x - curX, dy = s.y - curY;
                const float chord = std::sqrt(dx * dx + dy * dy);
                if (chord < 1e-6f) break;
                const float r = std::max(s.radius, chord / 2.0f);
                const float h = std::sqrt(std::max(0.0f, r * r - chord * chord / 4.0f));
                const float mx = (curX + s.x) / 2.0f, my = (curY + s.y) / 2.0f;
                const float nx = -dy / chord, ny = dx / chord;
                float cx = mx + nx * h, cy = my + ny * h;
                auto sweepFrom = [&](float ccx, float ccy) {
                    const float a0 = std::atan2(curY - ccy, curX - ccx), a1 = std::atan2(s.y - ccy, s.x - ccx);
                    return s.clockwise ? clockwiseSweep(a0, a1) : clockwiseSweep(a1, a0);
                };
                if ((sweepFrom(cx, cy) > kPi + 1e-4f) != s.largeArc) {
                    cx = mx - nx * h;
                    cy = my - ny * h;
                }
                const float a0 = std::atan2(curY - cy, curX - cx), a1 = std::atan2(s.y - cy, s.x - cx);
                if (s.clockwise) cairo_arc(cr_, cx, cy, r, a0, a1);
                else cairo_arc_negative(cr_, cx, cy, r, a0, a1);
                break;
            }
            case Path::SegmentType::Close:
                if (open) cairo_close_path(cr_);
                open = false;
                break;
        }
        if (s.type != Path::SegmentType::Close) {
            curX = s.x;
            curY = s.y;
        }
    }
}

void CairoCanvas::fillPath(const Path& p, Color c) {
    if (!cr_ || p.segments().empty()) return;
    buildPath(p);
    setColor(c);
    cairo_fill(cr_);
}

void CairoCanvas::strokePath(const Path& p, Color c, float width) {
    if (!cr_ || p.segments().empty()) return;
    buildPath(p);
    setColor(c);
    cairo_set_line_width(cr_, width);
    cairo_stroke(cr_);
}

PangoLayout* CairoCanvas::layout(cairo_t* cr, std::string_view utf8, FontRole font) const {
    PangoLayout* l = pango_cairo_create_layout(cr);
    // Metric hinting rounds advances per device pixel, so widths differ between scales; turn it off so a width
    // measured for layout is the width drawn.
    cairo_font_options_t* opts = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options(pango_layout_get_context(l), opts);
    cairo_font_options_destroy(opts);
    pango_layout_context_changed(l);
    pango_layout_set_font_description(l, fonts_[static_cast<size_t>(font)]);
    pango_layout_set_text(l, utf8.data(), static_cast<int>(utf8.size()));
    return l;
}

void CairoCanvas::drawText(std::string_view utf8, Rect r, FontRole font, Color c, HAlign h, VAlign v) {
    if (!cr_ || utf8.empty()) return;
    const float w = r.right - r.left, hgt = r.bottom - r.top;
    PangoLayout* l = layout(cr_, utf8, font);
    const bool wraps = font == FontRole::Tooltip;   // only the tooltip format wraps, as on Windows
    if (wraps) {
        pango_layout_set_width(l, static_cast<int>(w * PANGO_SCALE));
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_alignment(l, h == HAlign::Center ? PANGO_ALIGN_CENTER
                                      : h == HAlign::Trailing ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    }
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    const float tw = static_cast<float>(logical.width) / PANGO_SCALE;
    const float th = static_cast<float>(logical.height) / PANGO_SCALE;
    float x = r.left;
    if (!wraps) x += h == HAlign::Center ? (w - tw) / 2.0f : h == HAlign::Trailing ? w - tw : 0.0f;
    const float y = r.top + (v == VAlign::Center ? (hgt - th) / 2.0f : v == VAlign::Bottom ? hgt - th : 0.0f);

    cairo_save(cr_);
    cairo_rectangle(cr_, r.left, r.top, w, hgt);
    cairo_clip(cr_);
    setColor(c);
    cairo_move_to(cr_, x, y);
    pango_cairo_show_layout(cr_, l);
    cairo_restore(cr_);
    g_object_unref(l);
}

float CairoCanvas::measureTextWidth(std::string_view utf8, FontRole font) {
    if (utf8.empty()) return 0.0f;
    PangoLayout* l = layout(measureCr_, utf8, font);
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    g_object_unref(l);
    return static_cast<float>(logical.width) / PANGO_SCALE;
}

Point CairoCanvas::measureTextBlock(std::string_view utf8, FontRole font, float maxWidth) {
    if (utf8.empty()) return Point{};
    PangoLayout* l = layout(measureCr_, utf8, font);
    if (font == FontRole::Tooltip) {
        pango_layout_set_width(l, static_cast<int>(maxWidth * PANGO_SCALE));
        pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    }
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    g_object_unref(l);
    return Point{ static_cast<float>(logical.width) / PANGO_SCALE, static_cast<float>(logical.height) / PANGO_SCALE };
}

ImageId CairoCanvas::createImage(int width, int height, bool opaque) {
    if (!target_ || width <= 0 || height <= 0) return {};
    cairo_surface_t* s = cairo_image_surface_create(opaque ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return {};
    }
    ImageId id{ nextImage_++ };
    images_[id.value] = ImageEntry{ s, width, height };
    return id;
}

void CairoCanvas::updateImage(ImageId id, const uint8_t* bgra, int pitch, Rect dirty) {
    auto it = images_.find(id.value);
    if (it == images_.end() || !bgra || pitch <= 0) return;
    const ImageEntry& e = it->second;
    // Clamp the dirty rectangle to the image so a stale rectangle can never read or write out of bounds.
    const int l = std::clamp(static_cast<int>(dirty.left), 0, e.width);
    const int t = std::clamp(static_cast<int>(dirty.top), 0, e.height);
    const int r = std::clamp(static_cast<int>(std::ceil(dirty.right)), 0, e.width);
    const int b = std::clamp(static_cast<int>(std::ceil(dirty.bottom)), 0, e.height);
    if (r <= l || b <= t) return;
    cairo_surface_flush(e.surface);
    uint8_t* dst = cairo_image_surface_get_data(e.surface);
    const int stride = cairo_image_surface_get_stride(e.surface);
    for (int y = t; y < b; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * stride + static_cast<size_t>(l) * 4,
                    bgra + static_cast<size_t>(y) * pitch + static_cast<size_t>(l) * 4, static_cast<size_t>(r - l) * 4);
    }
    cairo_surface_mark_dirty_rectangle(e.surface, l, t, r - l, b - t);
}

void CairoCanvas::drawImage(ImageId id, Rect dst, float opacity) {
    auto it = images_.find(id.value);
    if (!cr_ || it == images_.end()) return;
    const ImageEntry& e = it->second;
    const float w = dst.right - dst.left, h = dst.bottom - dst.top;
    if (w <= 0.0f || h <= 0.0f) return;
    cairo_save(cr_);
    cairo_rectangle(cr_, dst.left, dst.top, w, h);
    cairo_clip(cr_);
    cairo_translate(cr_, dst.left, dst.top);
    cairo_scale(cr_, w / e.width, h / e.height);
    cairo_set_source_surface(cr_, e.surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr_), CAIRO_EXTEND_PAD);   // no faded edges when scaling
    cairo_paint_with_alpha(cr_, std::clamp(opacity, 0.0f, 1.0f));
    cairo_restore(cr_);
}

void CairoCanvas::destroyImage(ImageId id) {
    auto it = images_.find(id.value);
    if (it == images_.end()) return;
    cairo_surface_destroy(it->second.surface);
    images_.erase(it);
}

void CairoCanvas::pushClip(Rect r) {
    if (!cr_) return;
    cairo_save(cr_);
    cairo_rectangle(cr_, r.left, r.top, r.right - r.left, r.bottom - r.top);
    cairo_clip(cr_);
    stack_.push_back({ Pushed::Clip, 1.0f });
}

void CairoCanvas::popClip() {
    if (!cr_ || stack_.empty() || stack_.back().first != Pushed::Clip) return;
    pop(Pushed::Clip, 1.0f);
}

void CairoCanvas::pushOpacity(float opacity) {
    if (!cr_) return;
    cairo_push_group(cr_);
    stack_.push_back({ Pushed::Opacity, std::clamp(opacity, 0.0f, 1.0f) });
}

void CairoCanvas::popOpacity() {
    if (!cr_ || stack_.empty() || stack_.back().first != Pushed::Opacity) return;
    pop(Pushed::Opacity, stack_.back().second);
}

void CairoCanvas::pop(Pushed kind, float opacity) {
    // cairo_restore and cairo_pop_group also restore the matrix; Direct2D keeps the transform, so put it back.
    cairo_matrix_t m;
    cairo_get_matrix(cr_, &m);
    if (kind == Pushed::Clip) {
        cairo_restore(cr_);
    } else {
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, opacity);
    }
    cairo_set_matrix(cr_, &m);
    stack_.pop_back();
}

void CairoCanvas::setTransform(const Transform2D& t) {
    transform_ = t;
    if (!cr_) return;
    cairo_matrix_t m = toCairo(t), base, result;
    cairo_matrix_init_scale(&base, scale_, scale_);
    cairo_matrix_multiply(&result, &m, &base);   // t first, then the DPI scale
    cairo_set_matrix(cr_, &result);
}

} // namespace cppdesk::ui
