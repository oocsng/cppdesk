#pragma once

// Pixel <-> DIP conversion for the Windows UI. Layout and drawing use DIPs (1 DIP = 1 px at 96 DPI);
// Win32 sizes and mouse coordinates are physical pixels. No platform headers, so it is testable anywhere.

#include <cmath>

namespace cppdesk {

// A DPI of 0 (failed query, window not created yet) is treated as 96.
inline float dpiScale(unsigned dpi) {
    return dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
}

inline float pxToDip(float px, unsigned dpi) {
    return px / dpiScale(dpi);
}

inline int dipToPx(float dip, unsigned dpi) {
    return static_cast<int>(std::lround(dip * dpiScale(dpi)));
}

// Rounds a DIP coordinate to the nearest whole physical pixel, so bitmaps drawn there are not resampled.
inline float snapToPx(float dip, unsigned dpi) {
    return pxToDip(static_cast<float>(dipToPx(dip, dpi)), dpi);
}

struct SpanFit { int pos; int size; };

// Shrinks and moves a window span (one axis, physical pixels) so it lies inside the work area [workStart, workEnd].
inline SpanFit fitSpanToWork(int pos, int size, int workStart, int workEnd) {
    if (size > workEnd - workStart) size = workEnd - workStart;
    if (pos + size > workEnd) pos = workEnd - size;
    if (pos < workStart) pos = workStart;
    return { pos, size };
}

} // namespace cppdesk
