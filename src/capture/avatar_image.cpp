#include "avatar_image.hpp"

#include "screen_capture.hpp"

#include <algorithm>
#include <cmath>

namespace cppdesk::avatar {

std::optional<std::vector<uint8_t>> normaliseJpeg(const std::vector<uint8_t>& fileBytes) {
    if (fileBytes.empty() || fileBytes.size() > kMaxInputBytes) return std::nullopt;
    std::vector<uint8_t> src;
    int w = 0, h = 0;
    if (!TileCodec::decodeJpeg(fileBytes.data(), fileBytes.size(), src, w, h) || w <= 0 || h <= 0) return std::nullopt;

    const int side = std::min(w, h);
    const int x0 = (w - side) / 2, y0 = (h - side) / 2;
    // Area average: output pixel i covers source span [i*side, (i+1)*side) in 1/kSize-pixel units, so it also upscales.
    std::vector<uint8_t> out(static_cast<size_t>(kSize) * kSize * 4);
    auto overlap = [&](int i, int p) {
        return std::max(0, std::min((i + 1) * side, (p + 1) * kSize) - std::max(i * side, p * kSize));
    };
    for (int oy = 0; oy < kSize; ++oy) {
        const int pyLo = oy * side / kSize, pyHi = std::min(side - 1, ((oy + 1) * side - 1) / kSize);
        for (int ox = 0; ox < kSize; ++ox) {
            const int pxLo = ox * side / kSize, pxHi = std::min(side - 1, ((ox + 1) * side - 1) / kSize);
            uint64_t acc[3] = {0, 0, 0}, total = 0;
            for (int py = pyLo; py <= pyHi; ++py) {
                const uint64_t wy = overlap(oy, py);
                for (int px = pxLo; px <= pxHi; ++px) {
                    const uint64_t wt = wy * overlap(ox, px);
                    const uint8_t* p = &src[(static_cast<size_t>(y0 + py) * w + (x0 + px)) * 4];
                    for (int c = 0; c < 3; ++c) acc[c] += wt * p[c];
                    total += wt;
                }
            }
            uint8_t* d = &out[(static_cast<size_t>(oy) * kSize + ox) * 4];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((acc[c] + total / 2) / total);
            d[3] = 255;
        }
    }
    auto jpeg = TileCodec::encodeJpeg(out.data(), kSize, kSize, 85);
    if (jpeg.empty() || jpeg.size() > kMaxWireBytes) return std::nullopt;
    return jpeg;
}

bool validateWireJpeg(const std::vector<uint8_t>& jpeg) {
    if (jpeg.empty() || jpeg.size() > kMaxWireBytes) return false;
    int w = 0, h = 0;
    // Header first: decodeJpeg would allocate whatever the peer claims (up to 64M pixels) before we could refuse it.
    if (!TileCodec::jpegDimensions(jpeg.data(), jpeg.size(), w, h) || w > 256 || h > 256) return false;
    std::vector<uint8_t> px;
    return TileCodec::decodeJpeg(jpeg.data(), jpeg.size(), px, w, h) && w >= 1 && w <= 256 && h >= 1 && h <= 256;
}

bool decodeCircleBgra(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& outBgra, int& outW, int& outH) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (jpeg.empty() || !TileCodec::decodeJpeg(jpeg.data(), jpeg.size(), px, w, h) || w <= 0 || h <= 0) return false;
    const float cx = w * 0.5f, cy = h * 0.5f, r = std::min(w, h) * 0.5f;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
            const float cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(p[c] * cov + 0.5f);
            p[3] = static_cast<uint8_t>(255.0f * cov + 0.5f);
        }
    outBgra = std::move(px);
    outW = w;
    outH = h;
    return true;
}

} // namespace cppdesk::avatar
