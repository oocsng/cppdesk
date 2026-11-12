#include "canvas_cg.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cppdesk::ui {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr CGFloat kNoWrap = 1.0e7;

// Angle swept going clockwise on screen (increasing angle, y down) from a0 to a1, in (0, 2pi].
float clockwiseSweep(float a0, float a1) {
    float s = a1 - a0;
    while (s <= 0.0f) s += 2.0f * kPi;
    while (s > 2.0f * kPi) s -= 2.0f * kPi;
    return s;
}

// DirectWrite weight (400 normal, 600 semi-bold, 700 bold) to the Core Text weight trait.
CGFloat ctWeight(int dwWeight) {
    if (dwWeight >= 700) return 0.4;
    if (dwWeight >= 600) return 0.3;
    return 0.0;
}

// The weight trait Core Text reports for a font, from -1 (thin) to 1 (black); 0 is regular.
CGFloat reportedWeight(CTFontRef f) {
    CGFloat w = 0.0;
    CFDictionaryRef traits = CTFontCopyTraits(f);
    if (traits) {
        CFNumberRef n = static_cast<CFNumberRef>(CFDictionaryGetValue(traits, kCTFontWeightTrait));
        if (n) CFNumberGetValue(n, kCFNumberCGFloatType, &w);
        CFRelease(traits);
    }
    return w;
}

CFDictionaryRef traitsAttribute(CGFloat weight) {
    CFNumberRef wn = CFNumberCreate(kCFAllocatorDefault, kCFNumberCGFloatType, &weight);
    const void* tk[] = { kCTFontWeightTrait };
    const void* tv[] = { wn };
    CFDictionaryRef traits = CFDictionaryCreate(kCFAllocatorDefault, tk, tv, 1, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    const void* ak[] = { kCTFontTraitsAttribute };
    const void* av[] = { traits };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, ak, av, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFRelease(traits);
    CFRelease(wn);
    return attrs;
}

// The system UI font at a weight. How to ask for a weight differs between macOS versions, so try the ways in turn and
// keep the first whose reported weight is close to the one wanted.
CTFontRef systemFont(float size, int dwWeight) {
    const CGFloat want = ctWeight(dwWeight);
    CTFontRef base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr);
    if (want == 0.0 || !base) return base;
    auto close = [&](CTFontRef f) { return f && std::fabs(reportedWeight(f) - want) < 0.08; };

    CTFontRef f = nullptr;
    {   // A copy of the system font with the weight trait.
        CFDictionaryRef attrs = traitsAttribute(want);
        CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
        f = CTFontCreateCopyWithAttributes(base, size, nullptr, desc);
        CFRelease(desc);
        CFRelease(attrs);
    }
    if (close(f)) { CFRelease(base); return f; }
    if (f) CFRelease(f);

    {   // The system font by name plus the weight trait.
        CFDictionaryRef traitsOnly = traitsAttribute(want);
        const void* ak[] = { kCTFontNameAttribute, kCTFontTraitsAttribute };
        const void* av[] = { CFSTR(".AppleSystemUIFont"), CFDictionaryGetValue(traitsOnly, kCTFontTraitsAttribute) };
        CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, ak, av, 2, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
        f = CTFontCreateWithFontDescriptor(desc, size, nullptr);
        CFRelease(desc);
        CFRelease(attrs);
        CFRelease(traitsOnly);
    }
    if (close(f)) { CFRelease(base); return f; }
    if (f) CFRelease(f);

    {   // The weight axis of the variable system font (tag 'wght').
        const int32_t tag = 0x77676874;
        const double value = dwWeight;
        CFNumberRef key = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &tag);
        CFNumberRef val = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
        CFDictionaryRef axes = CFDictionaryCreate(kCFAllocatorDefault, reinterpret_cast<const void**>(&key),
                                                  reinterpret_cast<const void**>(&val), 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
        const void* ak[] = { kCTFontVariationAttribute };
        const void* av[] = { axes };
        CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, ak, av, 1, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
        f = CTFontCreateCopyWithAttributes(base, size, nullptr, desc);
        CFRelease(desc);
        CFRelease(attrs);
        CFRelease(axes);
        CFRelease(val);
        CFRelease(key);
    }
    if (close(f)) { CFRelease(base); return f; }
    if (f) CFRelease(f);

    // Last resort: the bold system font for anything from semi-bold up.
    CFRelease(base);
    return CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, size, nullptr);
}

CTFontRef makeFont(float size, int weight, bool mono) {
    if (mono) {
        CTFontRef f = CTFontCreateWithName(weight >= 600 ? CFSTR("Menlo-Bold") : CFSTR("Menlo-Regular"), size, nullptr);
        if (f) return f;
    }
    return systemFont(size, weight);
}

} // namespace

struct CGCanvas::Laid {
    std::vector<CTLineRef> lines;
    std::vector<float> widths;
    float ascent = 0.0f;
    float lineHeight = 0.0f;
    float maxWidth = 0.0f;

    Laid() = default;
    Laid(const Laid&) = delete;
    Laid(Laid&& o) noexcept
        : lines(std::move(o.lines)), widths(std::move(o.widths)), ascent(o.ascent), lineHeight(o.lineHeight),
          maxWidth(o.maxWidth) { o.lines.clear(); }
    ~Laid() { for (CTLineRef l : lines) CFRelease(l); }
    float height() const { return lineHeight * static_cast<float>(lines.size()); }
};

CGCanvas::CGCanvas() {
    const theme::type::Style styles[8] = { theme::type::hero, theme::type::heading, theme::type::subheading,
                                           theme::type::body, theme::type::bodyBold, theme::type::caption,
                                           theme::type::mono, theme::type::tooltip };
    for (size_t i = 0; i < fonts_.size(); ++i) {
        fonts_[i] = makeFont(styles[i].size, styles[i].weight, i == static_cast<size_t>(FontRole::Mono));
    }
}

CGCanvas::~CGCanvas() {
    detach();
    for (CTFontRef f : fonts_) if (f) CFRelease(f);
}

void CGCanvas::attach(CGContextRef bitmap, float dpiScale) {
    detach();
    ctx_ = bitmap;
    scale_ = dpiScale > 0.0f ? dpiScale : 1.0f;
    // Text drawn at fractional positions, without per-glyph rounding, so a width measured is the width drawn.
    CGContextSetAllowsFontSubpixelPositioning(ctx_, true);
    CGContextSetShouldSubpixelPositionFonts(ctx_, true);
    CGContextSetShouldSubpixelQuantizeFonts(ctx_, false);
}

void CGCanvas::detach() {
    if (inFrame_) endFrame();
    images_.clear();
    ctx_ = nullptr;
    transform_ = Transform2D::identity();
    stack_.clear();
}

void CGCanvas::applyTransform() {
    // CoreGraphics has no public "set CTM": undo the current one, then build base (DIP to y-down device pixels)
    // and the caller's transform. concat(X) means X first, then the CTM, like Transform2D's "a*b applies a first".
    CGContextConcatCTM(ctx_, CGAffineTransformInvert(CGContextGetCTM(ctx_)));
    const float pixelH = static_cast<float>(CGBitmapContextGetHeight(ctx_));
    CGContextConcatCTM(ctx_, CGAffineTransformMake(scale_, 0, 0, -scale_, 0, pixelH));
    const Transform2D& t = transform_;
    CGContextConcatCTM(ctx_, CGAffineTransformMake(t.m11, t.m12, t.m21, t.m22, t.dx, t.dy));
}

void CGCanvas::beginFrame(float, float, Color clear) {
    if (!ctx_) return;
    if (inFrame_) endFrame();
    inFrame_ = true;
    CGContextSaveGState(ctx_);   // the frame's own state level; endFrame restores it
    transform_ = Transform2D::identity();
    stack_.clear();
    CGContextSaveGState(ctx_);
    CGContextConcatCTM(ctx_, CGAffineTransformInvert(CGContextGetCTM(ctx_)));   // device space for the clear
    CGContextSetBlendMode(ctx_, kCGBlendModeCopy);
    setFill(clear);
    CGContextFillRect(ctx_, CGRectMake(0, 0, CGBitmapContextGetWidth(ctx_), CGBitmapContextGetHeight(ctx_)));
    CGContextRestoreGState(ctx_);
    applyTransform();
}

void CGCanvas::endFrame() {
    if (!inFrame_) return;
    while (!stack_.empty()) pop(stack_.back());
    CGContextRestoreGState(ctx_);
    inFrame_ = false;
}

void CGCanvas::setFill(Color c) { CGContextSetRGBFillColor(ctx_, c.r, c.g, c.b, c.a); }

void CGCanvas::setStroke(Color c, float width) {
    CGContextSetRGBStrokeColor(ctx_, c.r, c.g, c.b, c.a);
    CGContextSetLineWidth(ctx_, width);
}

void CGCanvas::fillRect(Rect r, Color c) {
    if (!inFrame_) return;
    setFill(c);
    CGContextFillRect(ctx_, CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top));
}

void CGCanvas::roundRectPath(Rect r, float radius) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0.0f || h <= 0.0f) return;
    radius = std::clamp(radius, 0.0f, std::min(w, h) / 2.0f);
    CGPathRef p = CGPathCreateWithRoundedRect(CGRectMake(r.left, r.top, w, h), radius, radius, nullptr);
    CGContextAddPath(ctx_, p);
    CGPathRelease(p);
}

void CGCanvas::fillRoundRect(Rect r, float radius, Color c) {
    if (!inFrame_) return;
    roundRectPath(r, radius);
    setFill(c);
    CGContextFillPath(ctx_);
}

void CGCanvas::strokeRoundRect(Rect r, float radius, Color c, float width) {
    if (!inFrame_) return;
    roundRectPath(r, radius);
    setStroke(c, width);
    CGContextStrokePath(ctx_);
}

void CGCanvas::fillEllipse(float cx, float cy, float rx, float ry, Color c) {
    if (!inFrame_ || rx <= 0.0f || ry <= 0.0f) return;
    CGContextAddEllipseInRect(ctx_, CGRectMake(cx - rx, cy - ry, 2 * rx, 2 * ry));
    setFill(c);
    CGContextFillPath(ctx_);
}

void CGCanvas::strokeEllipse(float cx, float cy, float rx, float ry, Color c, float width) {
    if (!inFrame_ || rx <= 0.0f || ry <= 0.0f) return;
    CGContextAddEllipseInRect(ctx_, CGRectMake(cx - rx, cy - ry, 2 * rx, 2 * ry));
    setStroke(c, width);
    CGContextStrokePath(ctx_);
}

void CGCanvas::drawLine(float x0, float y0, float x1, float y1, Color c, float width) {
    if (!inFrame_) return;
    CGContextMoveToPoint(ctx_, x0, y0);
    CGContextAddLineToPoint(ctx_, x1, y1);
    setStroke(c, width);
    CGContextStrokePath(ctx_);
}

void CGCanvas::buildPath(const Path& p) {
    float curX = 0.0f, curY = 0.0f;
    bool open = false;
    for (const auto& s : p.segments()) {
        switch (s.type) {
            case Path::SegmentType::Move:
                CGContextMoveToPoint(ctx_, s.x, s.y);
                open = true;
                break;
            case Path::SegmentType::Line:
                if (open) CGContextAddLineToPoint(ctx_, s.x, s.y);
                break;
            case Path::SegmentType::Arc: {
                if (!open) break;
                // Direct2D/SVG endpoint arc (rx = ry, no rotation) -> centre and angles, as in CairoCanvas.
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
                // CG's "clockwise" flag counts decreasing angle, so increasing angle (screen-clockwise, y down) is 0.
                CGContextAddArc(ctx_, cx, cy, r, a0, a1, s.clockwise ? 0 : 1);
                break;
            }
            case Path::SegmentType::Close:
                if (open) CGContextClosePath(ctx_);
                open = false;
                break;
        }
        if (s.type != Path::SegmentType::Close) {
            curX = s.x;
            curY = s.y;
        }
    }
}

void CGCanvas::fillPath(const Path& p, Color c) {
    if (!inFrame_ || p.segments().empty()) return;
    buildPath(p);
    setFill(c);
    CGContextFillPath(ctx_);
}

void CGCanvas::strokePath(const Path& p, Color c, float width) {
    if (!inFrame_ || p.segments().empty()) return;
    buildPath(p);
    setStroke(c, width);
    CGContextStrokePath(ctx_);
}

CGCanvas::Laid CGCanvas::layoutText(std::string_view utf8, FontRole font, float wrapWidth) const {
    Laid out;
    CFStringRef str = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                              static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    if (!str) return out;
    const void* keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
    const void* values[] = { fonts_[static_cast<size_t>(font)], kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef astr = CFAttributedStringCreate(kCFAllocatorDefault, str, attrs);
    CTTypesetterRef ts = CTTypesetterCreateWithAttributedString(astr);
    const CFIndex length = CFStringGetLength(str);
    const CGFloat width = wrapWidth > 0.0f ? wrapWidth : kNoWrap;
    for (CFIndex start = 0; start < length;) {
        CFIndex count = CTTypesetterSuggestLineBreak(ts, start, width);
        if (count <= 0) count = 1;
        CTLineRef line = CTTypesetterCreateLine(ts, CFRangeMake(start, count));
        start += count;
        CGFloat asc = 0, desc = 0, lead = 0;
        // Trailing whitespace at a wrap point is not part of the line's visible width.
        const double w = CTLineGetTypographicBounds(line, &asc, &desc, &lead) - CTLineGetTrailingWhitespaceWidth(line);
        out.lines.push_back(line);
        out.widths.push_back(static_cast<float>(std::max(0.0, w)));
        out.ascent = static_cast<float>(asc);
        out.lineHeight = static_cast<float>(asc + desc + lead);
        out.maxWidth = std::max(out.maxWidth, out.widths.back());
    }
    CFRelease(ts);
    CFRelease(astr);
    CFRelease(attrs);
    CFRelease(str);
    return out;
}

void CGCanvas::drawText(std::string_view utf8, Rect r, FontRole font, Color c, HAlign h, VAlign v) {
    if (!inFrame_ || utf8.empty()) return;
    const float w = r.right - r.left, hgt = r.bottom - r.top;
    const bool wraps = font == FontRole::Tooltip;   // only the tooltip format wraps, as on Windows
    Laid laid = layoutText(utf8, font, wraps ? w : 0.0f);
    if (laid.lines.empty()) return;
    const float th = laid.height();
    const float y0 = r.top + (v == VAlign::Center ? (hgt - th) / 2.0f : v == VAlign::Bottom ? hgt - th : 0.0f);

    CGContextSaveGState(ctx_);
    CGContextClipToRect(ctx_, CGRectMake(r.left, r.top, w, hgt));
    setFill(c);
    CGContextSetTextMatrix(ctx_, CGAffineTransformMake(1, 0, 0, -1, 0, 0));   // glyphs upright in the y-down space
    for (size_t i = 0; i < laid.lines.size(); ++i) {
        // A wrapped tooltip aligns each line; other text aligns as one block.
        const float room = w - (wraps ? laid.widths[i] : laid.maxWidth);
        const float x = r.left + (h == HAlign::Center ? room / 2.0f : h == HAlign::Trailing ? room : 0.0f);
        CGContextSetTextPosition(ctx_, x, y0 + static_cast<float>(i) * laid.lineHeight + laid.ascent);
        CTLineDraw(laid.lines[i], ctx_);
    }
    CGContextRestoreGState(ctx_);
}

float CGCanvas::measureTextWidth(std::string_view utf8, FontRole font) {
    if (utf8.empty()) return 0.0f;
    return layoutText(utf8, font, 0.0f).maxWidth;
}

Point CGCanvas::measureTextBlock(std::string_view utf8, FontRole font, float maxWidth) {
    if (utf8.empty()) return Point{};
    const Laid laid = layoutText(utf8, font, font == FontRole::Tooltip ? maxWidth : 0.0f);
    return Point{ laid.maxWidth, laid.height() };
}

ImageId CGCanvas::createImage(int width, int height, bool opaque) {
    if (!ctx_ || width <= 0 || height <= 0) return {};
    ImageEntry e;
    e.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    e.width = width;
    e.height = height;
    e.opaque = opaque;
    ImageId id{ nextImage_++ };
    images_[id.value] = std::move(e);
    return id;
}

void CGCanvas::updateImage(ImageId id, const uint8_t* bgra, int pitch, Rect dirty) {
    auto it = images_.find(id.value);
    if (it == images_.end() || !bgra || pitch <= 0) return;
    ImageEntry& e = it->second;
    // Clamp the dirty rectangle to the image so a stale rectangle can never read or write out of bounds.
    const int l = std::clamp(static_cast<int>(dirty.left), 0, e.width);
    const int t = std::clamp(static_cast<int>(dirty.top), 0, e.height);
    const int r = std::clamp(static_cast<int>(std::ceil(dirty.right)), 0, e.width);
    const int b = std::clamp(static_cast<int>(std::ceil(dirty.bottom)), 0, e.height);
    if (r <= l || b <= t) return;
    for (int y = t; y < b; ++y) {
        std::memcpy(e.pixels.data() + (static_cast<size_t>(y) * e.width + l) * 4,
                    bgra + static_cast<size_t>(y) * pitch + static_cast<size_t>(l) * 4, static_cast<size_t>(r - l) * 4);
    }
}

void CGCanvas::drawImage(ImageId id, Rect dst, float opacity) {
    auto it = images_.find(id.value);
    if (!inFrame_ || it == images_.end()) return;
    const ImageEntry& e = it->second;
    const float w = dst.right - dst.left, h = dst.bottom - dst.top;
    if (w <= 0.0f || h <= 0.0f) return;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, e.pixels.data(), e.pixels.size(), nullptr);
    const CGBitmapInfo info = (e.opaque ? kCGImageAlphaNoneSkipFirst : kCGImageAlphaPremultipliedFirst) |
                              kCGBitmapByteOrder32Little;
    CGImageRef img = CGImageCreate(e.width, e.height, 8, 32, static_cast<size_t>(e.width) * 4, space, info, provider,
                                   nullptr, true, kCGRenderingIntentDefault);
    if (img) {
        CGContextSaveGState(ctx_);
        CGContextClipToRect(ctx_, CGRectMake(dst.left, dst.top, w, h));
        CGContextSetAlpha(ctx_, std::clamp(opacity, 0.0f, 1.0f));
        CGContextSetInterpolationQuality(ctx_, kCGInterpolationHigh);
        // Images draw with row 0 at the top of a y-up rect: flip back inside the y-down space.
        CGContextTranslateCTM(ctx_, dst.left, dst.bottom);
        CGContextScaleCTM(ctx_, 1.0, -1.0);
        CGContextDrawImage(ctx_, CGRectMake(0, 0, w, h), img);
        CGContextRestoreGState(ctx_);
        CGImageRelease(img);
    }
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
}

void CGCanvas::destroyImage(ImageId id) { images_.erase(id.value); }

void CGCanvas::pushClip(Rect r) {
    if (!inFrame_) return;
    CGContextSaveGState(ctx_);
    CGContextClipToRect(ctx_, CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top));
    stack_.push_back(Pushed::Clip);
}

void CGCanvas::popClip() {
    if (!inFrame_ || stack_.empty() || stack_.back() != Pushed::Clip) return;
    pop(Pushed::Clip);
}

void CGCanvas::pushOpacity(float opacity) {
    if (!inFrame_) return;
    CGContextSaveGState(ctx_);
    CGContextSetAlpha(ctx_, std::clamp(opacity, 0.0f, 1.0f));   // applied when the layer is composited
    CGContextBeginTransparencyLayer(ctx_, nullptr);
    CGContextSetAlpha(ctx_, 1.0f);                              // not again to what is drawn inside it
    stack_.push_back(Pushed::Opacity);
}

void CGCanvas::popOpacity() {
    if (!inFrame_ || stack_.empty() || stack_.back() != Pushed::Opacity) return;
    pop(Pushed::Opacity);
}

void CGCanvas::pop(Pushed kind) {
    if (kind == Pushed::Opacity) CGContextEndTransparencyLayer(ctx_);
    CGContextRestoreGState(ctx_);
    stack_.pop_back();
    applyTransform();   // the restore brought back the old matrix; Direct2D keeps the transform, so put it back
}

void CGCanvas::setTransform(const Transform2D& t) {
    transform_ = t;
    if (inFrame_) applyTransform();
}

} // namespace cppdesk::ui
