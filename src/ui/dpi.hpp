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

} // namespace cppdesk
