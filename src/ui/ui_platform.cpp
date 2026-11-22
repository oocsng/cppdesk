#include "ui_platform.hpp"

#include <cctype>
#include <cstdlib>
#include <string>

namespace cppdesk {

ViewportGeometry UiPlatform::calculateViewportGeometry(float viewWidth, float viewHeight, uint32_t frameWidth, uint32_t frameHeight, ScaleMode scaleMode) {
    ViewportGeometry geo{};
    if (viewWidth <= 0.0f || viewHeight <= 0.0f || frameWidth == 0 || frameHeight == 0) {
        return geo;
    }

    float fw = static_cast<float>(frameWidth);
    float fh = static_cast<float>(frameHeight);

    switch (scaleMode) {
        case ScaleMode::Stretch: {
            geo.contentRect = { 0.0f, 0.0f, viewWidth, viewHeight };
            geo.scaleX = viewWidth / fw;
            geo.scaleY = viewHeight / fh;
            geo.offsetX = 0.0f;
            geo.offsetY = 0.0f;
            geo.hasLetterbox = false;
            geo.hasPillarbox = false;
            break;
        }
        case ScaleMode::FillAspect: {
            float s = std::max(viewWidth / fw, viewHeight / fh);
            float targetW = fw * s;
            float targetH = fh * s;
            float offX = (viewWidth - targetW) * 0.5f;
            float offY = (viewHeight - targetH) * 0.5f;
            geo.contentRect = { offX, offY, offX + targetW, offY + targetH };
            geo.scaleX = s;
            geo.scaleY = s;
            geo.offsetX = offX;
            geo.offsetY = offY;
            geo.hasLetterbox = false;
            geo.hasPillarbox = false;
            break;
        }
        case ScaleMode::Original: {
            float offX = (viewWidth - fw) * 0.5f;
            float offY = (viewHeight - fh) * 0.5f;
            geo.contentRect = { offX, offY, offX + fw, offY + fh };
            geo.scaleX = 1.0f;
            geo.scaleY = 1.0f;
            geo.offsetX = offX;
            geo.offsetY = offY;
            geo.hasLetterbox = (offY > 0.001f);
            geo.hasPillarbox = (offX > 0.001f);
            break;
        }
        case ScaleMode::FitAspect:
        default: {
            float sX = viewWidth / fw;
            float sY = viewHeight / fh;
            float s = std::min(sX, sY);
            float targetW = fw * s;
            float targetH = fh * s;
            float offX = (viewWidth - targetW) * 0.5f;
            float offY = (viewHeight - targetH) * 0.5f;
            geo.contentRect = { offX, offY, offX + targetW, offY + targetH };
            geo.scaleX = s;
            geo.scaleY = s;
            geo.offsetX = offX;
            geo.offsetY = offY;
            geo.hasLetterbox = (offY > 0.001f);
            geo.hasPillarbox = (offX > 0.001f);
            break;
        }
    }
    return geo;
}

bool UiPlatform::mapPointToNormalized(float viewX, float viewY, const ViewportGeometry& geo, float& outNormX, float& outNormY) {
    return mapPointToNormalized(viewX, viewY, geo.contentRect, outNormX, outNormY);
}

bool UiPlatform::mapPointToNormalized(float x, float y, const UiPlatformRect& canvasRect, float& outNormX, float& outNormY) {
    if (!canvasRect.contains(x, y)) {
        return false;
    }
    float w = canvasRect.width();
    float h = canvasRect.height();
    if (w <= 0.0f || h <= 0.0f) {
        return false;
    }
    outNormX = std::clamp((x - canvasRect.left) / w, 0.0f, 1.0f);
    outNormY = std::clamp((y - canvasRect.top) / h, 0.0f, 1.0f);
    return true;
}

void UiPlatform::mapNormalizedToPoint(float normX, float normY, const ViewportGeometry& geo, float& outViewX, float& outViewY) {
    mapNormalizedToPoint(normX, normY, geo.contentRect, outViewX, outViewY);
}

void UiPlatform::mapNormalizedToPoint(float normX, float normY, const UiPlatformRect& canvasRect, float& outViewX, float& outViewY) {
    outViewX = canvasRect.left + normX * canvasRect.width();
    outViewY = canvasRect.top + normY * canvasRect.height();
}

bool UiPlatform::stepSpring(float& pos, float& vel, float target, float omega, float zeta, float dt, float eps) {
    float diff = pos - target;
    if (std::fabs(diff) <= eps && std::fabs(vel) <= eps * 8.0f) {
        if (pos != target || vel != 0.0f) {
            pos = target;
            vel = 0.0f;
            return true;
        }
        return false;
    }

    if (!std::isfinite(dt) || dt <= 0.0f) return false;
    dt = std::clamp(dt, 0.0f, 0.048f);

    int steps = (dt > 0.008f) ? static_cast<int>(std::ceil(dt / 0.008f)) : 1;
    steps = std::clamp(steps, 1, 6);
    float subDt = dt / static_cast<float>(steps);

    for (int i = 0; i < steps; ++i) {
        float disp = pos - target;
        float accel = (-omega * omega * disp) - (2.0f * zeta * omega * vel);
        vel += accel * subDt;
        pos += vel * subDt;
    }

    if (std::fabs(pos - target) <= eps && std::fabs(vel) <= eps * 8.0f) {
        pos = target;
        vel = 0.0f;
    }
    return true;
}

bool UiPlatform::stepMotion(float& pos, float& vel, float target, float omega, float zeta, float dt, float eps, bool reduced) {
    if (reduced) {
        if (pos == target && vel == 0.0f) return false;
        pos = target;
        vel = 0.0f;
        return true;
    }
    if (target == 0.0f) omega *= 1.25f;
    return stepSpring(pos, vel, target, omega, std::max(zeta, 1.0f), dt, eps);
}

UiPlatformRect UiPlatform::computeSpringRect(const UiPlatformRect& base, float pressT) {
    float shrink = 0.03f * std::clamp(pressT, 0.0f, 1.0f) * 0.5f;
    return base.inflate(-base.width() * shrink, -base.height() * shrink);
}

UiPlatformRect UiPlatform::popoverRect(const UiPlatformRect& base, float t, float originX, float originY) {
    float s = 0.95f + 0.05f * std::clamp(t, 0.0f, 1.0f);
    return { originX + (base.left - originX) * s, originY + (base.top - originY) * s,
             originX + (base.right - originX) * s, originY + (base.bottom - originY) * s };
}

float UiPlatform::dpiScaleFromXftDpi(const char* value) {
    if (!value) return 1.0f;
    char* end = nullptr;
    const double dpi = std::strtod(value, &end);
    if (end == value || !std::isfinite(dpi) || dpi <= 0.0) return 1.0f;
    return static_cast<float>(std::clamp(dpi / 96.0, 1.0, 4.0));
}

bool UiPlatform::isSafeExternalUrl(const std::string& url) {
    size_t schemeLen = 0;
    auto startsWithIgnoreCase = [&](const char* prefix, size_t len) {
        if (url.size() <= len) return false;
        for (size_t i = 0; i < len; ++i) {
            if (std::tolower(static_cast<unsigned char>(url[i])) != static_cast<unsigned char>(prefix[i])) return false;
        }
        return true;
    };
    if (startsWithIgnoreCase("https://", 8)) {
        schemeLen = 8;
    } else if (startsWithIgnoreCase("http://", 7)) {
        schemeLen = 7;
    } else {
        return false;
    }
    if (url.size() <= schemeLen) return false;
    for (unsigned char c : url) {
        if (c <= 0x1F || c == 0x7F || std::isspace(c)) return false;
    }
    return true;
}

std::vector<std::string> UiPlatform::parseFileUriList(const std::string& uriList) {
    std::vector<std::string> paths;
    size_t start = 0;
    while (start < uriList.size()) {
        size_t end = uriList.find('\n', start);
        if (end == std::string::npos) end = uriList.size();
        std::string line = uriList.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("file://", 0) != 0) continue;   // also skips '#' comments and other schemes
        const size_t pathStart = line.find('/', 7);       // after file://host
        if (pathStart == std::string::npos) continue;
        std::string host = line.substr(7, pathStart - 7);
        for (char& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!host.empty() && host != "localhost") continue;   // a remote host's path is not a local file
        std::string path;
        bool hasNul = false;
        for (size_t i = pathStart; i < line.size(); ++i) {
            if (line[i] == '\0') {
                hasNul = true;
                break;
            }
            if (line[i] == '%' && i + 2 < line.size() && std::isxdigit(static_cast<unsigned char>(line[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(line[i + 2]))) {
                const int decoded = std::stoi(line.substr(i + 1, 2), nullptr, 16);
                if (decoded == 0) {
                    hasNul = true;
                    break;
                }
                path.push_back(static_cast<char>(decoded));
                i += 2;
            } else {
                path.push_back(line[i]);
            }
        }
        if (!hasNul) paths.push_back(std::move(path));
    }
    return paths;
}

} // namespace cppdesk

