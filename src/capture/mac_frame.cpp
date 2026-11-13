#include "mac_frame.hpp"

#include <algorithm>
#include <cstring>

namespace cppdesk {

void copyBgraRows(const uint8_t* src, size_t srcStride, int width, int height, uint8_t* dst) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int r = 0; r < height; ++r) {
        const uint8_t* from = src + static_cast<size_t>(r) * srcStride;
        uint8_t* to = dst + static_cast<size_t>(r) * rowBytes;
        std::memcpy(to, from, rowBytes);
        for (size_t i = 3; i < rowBytes; i += 4) to[i] = 0xFF;
    }
}

void fitCaptureSize(int width, int height, int maxLongEdge, int& outWidth, int& outHeight) {
    int64_t w = std::max(width, 1), h = std::max(height, 1);
    const int64_t longEdge = std::max(w, h);
    if (maxLongEdge > 0 && longEdge > maxLongEdge) {
        w = w * maxLongEdge / longEdge;
        h = h * maxLongEdge / longEdge;
    }
    outWidth = static_cast<int>(std::max<int64_t>(2, w & ~int64_t{1}));
    outHeight = static_cast<int>(std::max<int64_t>(2, h & ~int64_t{1}));
}

} // namespace cppdesk
