#include "canvas_d2d.hpp"

#include "theme.hpp"

#include <string>

namespace cppdesk::ui {

namespace {

D2D1_COLOR_F d2dColor(Color c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

D2D1_RECT_F d2dRect(Rect r) {
    return D2D1::RectF(r.left, r.top, r.right, r.bottom);
}

D2D1_MATRIX_3X2_F d2dMatrix(const Transform2D& t) {
    D2D1_MATRIX_3X2_F m;
    m._11 = t.m11;
    m._12 = t.m12;
    m._21 = t.m21;
    m._22 = t.m22;
    m._31 = t.dx;
    m._32 = t.dy;
    return m;
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(len, 0)), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), len);
    return w;
}

template <typename T>
void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

D2DCanvas::D2DCanvas(ID2D1Factory* d2d, IDWriteFactory* dwrite) : d2d_(d2d), dwrite_(dwrite) {
    // The same families, sizes and weights the Windows UI has always used.
    struct Spec { const wchar_t* family; theme::type::Style style; bool wrap; };
    const Spec specs[8] = {
        { L"Segoe UI", theme::type::hero, false },        // HeroId
        { L"Segoe UI", theme::type::heading, false },     // Heading
        { L"Segoe UI", theme::type::subheading, false },  // Subheading
        { L"Segoe UI", theme::type::body, false },        // Body
        { L"Segoe UI", theme::type::bodyBold, false },    // BodyBold
        { L"Segoe UI", theme::type::caption, false },     // Small
        { L"Consolas", theme::type::mono, false },        // Mono
        { L"Segoe UI", theme::type::tooltip, true },      // Tooltip
    };
    if (!dwrite_) return;
    for (size_t i = 0; i < formats_.size(); ++i) {
        dwrite_->CreateTextFormat(specs[i].family, nullptr, static_cast<DWRITE_FONT_WEIGHT>(specs[i].style.weight),
                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, specs[i].style.size, L"en-us",
                                  &formats_[i]);
        if (formats_[i]) {
            formats_[i]->SetWordWrapping(specs[i].wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
}

D2DCanvas::~D2DCanvas() {
    detach();
    for (auto*& f : formats_) release(f);
}

void D2DCanvas::attach(ID2D1RenderTarget* rt, float /*dpiScale*/) {
    detach();
    rt_ = rt;
    if (rt_) rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
}

void D2DCanvas::detach() {
    if (inFrame_ && rt_) {
        unwindState();
        rt_->EndDraw();
        inFrame_ = false;
    }
    for (auto& kv : images_) release(kv.second.bitmap);
    images_.clear();
    release(brush_);
    rt_ = nullptr;
    transform_ = Transform2D::identity();
    clipDepth_ = 0;
    layerDepth_ = 0;
}

bool D2DCanvas::consumeRecreateNeeded() {
    bool r = recreateNeeded_;
    recreateNeeded_ = false;
    return r;
}

ID2D1SolidColorBrush* D2DCanvas::brush(Color c) {
    if (brush_) brush_->SetColor(d2dColor(c));
    return brush_;
}

void D2DCanvas::unwindState() {
    while (layerDepth_ > 0) {
        rt_->PopLayer();
        --layerDepth_;
    }
    while (clipDepth_ > 0) {
        rt_->PopAxisAlignedClip();
        --clipDepth_;
    }
}

void D2DCanvas::beginFrame(float, float, Color clear) {
    if (!rt_) return;
    transform_ = Transform2D::identity();
    clipDepth_ = 0;
    layerDepth_ = 0;
    rt_->BeginDraw();
    inFrame_ = true;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(d2dColor(clear));
}

void D2DCanvas::endFrame() {
    if (!rt_ || !inFrame_) return;
    // A draw function that returned early may have left state pushed; Direct2D requires it popped.
    unwindState();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = rt_->EndDraw();
    inFrame_ = false;
    if (hr == D2DERR_RECREATE_TARGET) recreateNeeded_ = true;
}

void D2DCanvas::fillRect(Rect r, Color c) {
    if (rt_ && brush_) rt_->FillRectangle(d2dRect(r), brush(c));
}

void D2DCanvas::fillRoundRect(Rect r, float radius, Color c) {
    if (rt_ && brush_) rt_->FillRoundedRectangle(D2D1::RoundedRect(d2dRect(r), radius, radius), brush(c));
}

void D2DCanvas::strokeRoundRect(Rect r, float radius, Color c, float width) {
    if (rt_ && brush_) rt_->DrawRoundedRectangle(D2D1::RoundedRect(d2dRect(r), radius, radius), brush(c), width);
}

void D2DCanvas::fillEllipse(float cx, float cy, float rx, float ry, Color c) {
    if (rt_ && brush_) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(c));
}

void D2DCanvas::strokeEllipse(float cx, float cy, float rx, float ry, Color c, float width) {
    if (rt_ && brush_) rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(c), width);
}

void D2DCanvas::drawLine(float x0, float y0, float x1, float y1, Color c, float width) {
    if (rt_ && brush_) rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), width);
}

ID2D1PathGeometry* D2DCanvas::buildGeometry(const Path& p, bool filled) const {
    if (!d2d_ || p.segments().empty()) return nullptr;
    ID2D1PathGeometry* geo = nullptr;
    if (FAILED(d2d_->CreatePathGeometry(&geo)) || !geo) return nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geo->Open(&sink)) || !sink) {
        geo->Release();
        return nullptr;
    }
    const D2D1_FIGURE_BEGIN begin = filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW;
    bool open = false;
    for (const auto& s : p.segments()) {
        switch (s.type) {
            case Path::SegmentType::Move:
                if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(D2D1::Point2F(s.x, s.y), begin);
                open = true;
                break;
            case Path::SegmentType::Line:
                if (open) sink->AddLine(D2D1::Point2F(s.x, s.y));
                break;
            case Path::SegmentType::Arc:
                if (open) {
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(s.x, s.y), D2D1::SizeF(s.radius, s.radius), 0.0f,
                                                  s.clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                                              : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                                  s.largeArc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                }
                break;
            case Path::SegmentType::Close:
                if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
                break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return geo;
}

void D2DCanvas::fillPath(const Path& p, Color c) {
    if (!rt_ || !brush_) return;
    if (ID2D1PathGeometry* geo = buildGeometry(p, true)) {
        rt_->FillGeometry(geo, brush(c));
        geo->Release();
    }
}

void D2DCanvas::strokePath(const Path& p, Color c, float width) {
    if (!rt_ || !brush_) return;
    if (ID2D1PathGeometry* geo = buildGeometry(p, false)) {
        rt_->DrawGeometry(geo, brush(c), width);
        geo->Release();
    }
}

void D2DCanvas::drawText(std::string_view utf8, Rect r, FontRole font, Color c, HAlign h, VAlign v) {
    IDWriteTextFormat* fmt = format(font);
    if (!rt_ || !brush_ || !fmt || utf8.empty()) return;
    fmt->SetTextAlignment(h == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                          : h == HAlign::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                          : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(v == VAlign::Top ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                               : v == VAlign::Bottom ? DWRITE_PARAGRAPH_ALIGNMENT_FAR
                               : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Short strings convert on the stack, as the window's drawText always has.
    if (utf8.size() < 512) {
        wchar_t stackWide[512];
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), stackWide, 512);
        if (wlen > 0) {
            rt_->DrawText(stackWide, static_cast<UINT32>(wlen), fmt, d2dRect(r), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }
    std::wstring w = widen(utf8);
    rt_->DrawText(w.c_str(), static_cast<UINT32>(w.size()), fmt, d2dRect(r), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float D2DCanvas::measureTextWidth(std::string_view utf8, FontRole font) {
    IDWriteTextFormat* fmt = format(font);
    if (!dwrite_ || !fmt || utf8.empty()) return 0.0f;
    std::wstring w = widen(utf8);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt, 100000.0f, 1000.0f, &layout)) ||
        !layout) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

Point D2DCanvas::measureTextBlock(std::string_view utf8, FontRole font, float maxWidth) {
    IDWriteTextFormat* fmt = format(font);
    if (!dwrite_ || !fmt || utf8.empty()) return Point{};
    std::wstring w = widen(utf8);
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt, maxWidth, 1000.0f, &layout)) ||
        !layout) {
        return Point{};
    }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    return Point{ m.width, m.height };
}

ImageId D2DCanvas::createImage(int width, int height, bool opaque) {
    if (!rt_ || width <= 0 || height <= 0) return {};
    ImageEntry e;
    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                 opaque ? D2D1_ALPHA_MODE_IGNORE : D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt_->CreateBitmap(D2D1::SizeU(width, height), props, &e.bitmap)) || !e.bitmap) return {};
    e.width = width;
    e.height = height;
    ImageId id{ nextImage_++ };
    images_[id.value] = e;
    return id;
}

void D2DCanvas::updateImage(ImageId id, const uint8_t* bgra, int pitch, Rect dirty) {
    auto it = images_.find(id.value);
    if (it == images_.end() || !bgra || pitch <= 0) return;
    const ImageEntry& e = it->second;
    // Clamp the dirty rectangle to the bitmap so a stale rectangle can never read or write out of bounds.
    UINT32 l = static_cast<UINT32>(std::clamp(static_cast<int>(dirty.left), 0, e.width));
    UINT32 t = static_cast<UINT32>(std::clamp(static_cast<int>(dirty.top), 0, e.height));
    UINT32 r = static_cast<UINT32>(std::clamp(static_cast<int>(std::ceil(dirty.right)), 0, e.width));
    UINT32 b = static_cast<UINT32>(std::clamp(static_cast<int>(std::ceil(dirty.bottom)), 0, e.height));
    if (r <= l || b <= t) return;
    D2D1_RECT_U dst = D2D1::RectU(l, t, r, b);
    const uint8_t* src = bgra + static_cast<size_t>(t) * pitch + static_cast<size_t>(l) * 4;
    e.bitmap->CopyFromMemory(&dst, src, static_cast<UINT32>(pitch));
}

void D2DCanvas::drawImage(ImageId id, Rect dst, float opacity) {
    auto it = images_.find(id.value);
    if (!rt_ || it == images_.end()) return;
    rt_->DrawBitmap(it->second.bitmap, d2dRect(dst), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void D2DCanvas::destroyImage(ImageId id) {
    auto it = images_.find(id.value);
    if (it == images_.end()) return;
    release(it->second.bitmap);
    images_.erase(it);
}

void D2DCanvas::pushClip(Rect r) {
    if (!rt_) return;
    rt_->PushAxisAlignedClip(d2dRect(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ++clipDepth_;
}

void D2DCanvas::popClip() {
    if (!rt_ || clipDepth_ == 0) return;
    rt_->PopAxisAlignedClip();
    --clipDepth_;
}

void D2DCanvas::pushOpacity(float opacity) {
    if (!rt_) return;
    ID2D1Layer* layer = nullptr;
    if (FAILED(rt_->CreateLayer(nullptr, &layer)) || !layer) return;
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), std::clamp(opacity, 0.0f, 1.0f)),
                   layer);
    layer->Release();   // the render target holds its own reference while the layer is pushed
    ++layerDepth_;
}

void D2DCanvas::popOpacity() {
    if (!rt_ || layerDepth_ == 0) return;
    rt_->PopLayer();
    --layerDepth_;
}

void D2DCanvas::setTransform(const Transform2D& t) {
    transform_ = t;
    if (rt_) rt_->SetTransform(d2dMatrix(t));
}

} // namespace cppdesk::ui
