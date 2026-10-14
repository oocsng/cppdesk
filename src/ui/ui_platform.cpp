#include "ui_platform.hpp"

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

struct MacVkPair {
    uint16_t macKey;
    uint32_t winVk;
};

static const MacVkPair s_macToVkMap[] = {
    // Alphanumerics A-Z
    { 0x00, 0x41 }, // 'A'
    { 0x0B, 0x42 }, // 'B'
    { 0x08, 0x43 }, // 'C'
    { 0x02, 0x44 }, // 'D'
    { 0x0E, 0x45 }, // 'E'
    { 0x03, 0x46 }, // 'F'
    { 0x05, 0x47 }, // 'G'
    { 0x04, 0x48 }, // 'H'
    { 0x22, 0x49 }, // 'I'
    { 0x26, 0x4A }, // 'J'
    { 0x28, 0x4B }, // 'K'
    { 0x25, 0x4C }, // 'L'
    { 0x2E, 0x4D }, // 'M'
    { 0x2D, 0x4E }, // 'N'
    { 0x1F, 0x4F }, // 'O'
    { 0x23, 0x50 }, // 'P'
    { 0x0C, 0x51 }, // 'Q'
    { 0x0F, 0x52 }, // 'R'
    { 0x01, 0x53 }, // 'S'
    { 0x11, 0x54 }, // 'T'
    { 0x20, 0x55 }, // 'U'
    { 0x09, 0x56 }, // 'V'
    { 0x0D, 0x57 }, // 'W'
    { 0x07, 0x58 }, // 'X'
    { 0x10, 0x59 }, // 'Y'
    { 0x06, 0x5A }, // 'Z'

    // Digits 0-9
    { 0x1D, 0x30 }, // '0'
    { 0x12, 0x31 }, // '1'
    { 0x13, 0x32 }, // '2'
    { 0x14, 0x33 }, // '3'
    { 0x15, 0x34 }, // '4'
    { 0x17, 0x35 }, // '5'
    { 0x16, 0x36 }, // '6'
    { 0x1A, 0x37 }, // '7'
    { 0x1C, 0x38 }, // '8'
    { 0x19, 0x39 }, // '9'

    // Navigation & Editing
    { 0x24, 0x0D }, // Return / Enter
    { 0x30, 0x09 }, // Tab
    { 0x31, 0x20 }, // Space
    { 0x33, 0x08 }, // Backspace / Delete
    { 0x35, 0x1B }, // Escape
    { 0x75, 0x2E }, // Forward Delete
    { 0x73, 0x24 }, // Home
    { 0x77, 0x23 }, // End
    { 0x74, 0x21 }, // Page Up
    { 0x79, 0x22 }, // Page Down
    { 0x7B, 0x25 }, // Left Arrow
    { 0x7C, 0x27 }, // Right Arrow
    { 0x7E, 0x26 }, // Up Arrow
    { 0x7D, 0x28 }, // Down Arrow

    // Function keys F1-F12
    { 0x7A, 0x70 }, // F1
    { 0x78, 0x71 }, // F2
    { 0x63, 0x72 }, // F3
    { 0x76, 0x73 }, // F4
    { 0x60, 0x74 }, // F5
    { 0x61, 0x75 }, // F6
    { 0x62, 0x76 }, // F7
    { 0x64, 0x77 }, // F8
    { 0x65, 0x78 }, // F9
    { 0x6D, 0x79 }, // F10
    { 0x67, 0x7A }, // F11
    { 0x6F, 0x7B }, // F12

    // Modifiers
    { 0x37, 0x5B }, // Command (Left Win)
    { 0x36, 0x5C }, // Right Command (Right Win)
    { 0x38, 0x10 }, // Shift
    { 0x39, 0x14 }, // CapsLock
    { 0x3A, 0x12 }, // Option (Alt)
    { 0x3B, 0x11 }, // Control

    // Punctuation / OEM
    { 0x18, 0xBB }, // Equal / Plus
    { 0x1B, 0xBD }, // Minus / Underscore
    { 0x1E, 0xDD }, // Right Bracket
    { 0x21, 0xDB }, // Left Bracket
    { 0x27, 0xDE }, // Quote
    { 0x29, 0xBA }, // Semicolon
    { 0x2A, 0xDC }, // Backslash
    { 0x2B, 0xBC }, // Comma
    { 0x2C, 0xBF }, // Slash
    { 0x2F, 0xBE }, // Period
    { 0x32, 0xC0 }  // Backquote / Tilde
};

uint32_t UiPlatform::macKeyCodeToWindowsVk(uint16_t macKeyCode) {
    for (const auto& pair : s_macToVkMap) {
        if (pair.macKey == macKeyCode) {
            return pair.winVk;
        }
    }
    return 0;
}

uint16_t UiPlatform::windowsVkToMacKeyCode(uint32_t vkCode) {
    for (const auto& pair : s_macToVkMap) {
        if (pair.winVk == vkCode) {
            return pair.macKey;
        }
    }
    return 0xFFFF;
}

std::vector<ModifierTransition> UiPlatform::decodeModifierTransitions(uint64_t oldMask, uint64_t newMask) {
    std::vector<ModifierTransition> result;
    struct ModDef {
        uint64_t flag;
        uint32_t vk;
    };
    static const ModDef mods[] = {
        { 1ULL << 16, 0x14 }, // CapsLock
        { 1ULL << 17, 0x10 }, // Shift
        { 1ULL << 18, 0x11 }, // Control
        { 1ULL << 19, 0x12 }, // Option (Alt)
        { 1ULL << 20, 0x5B }  // Command (Win)
    };
    for (const auto& m : mods) {
        bool wasDown = (oldMask & m.flag) != 0;
        bool isDown  = (newMask & m.flag) != 0;
        if (wasDown != isDown) {
            result.push_back({ m.vk, isDown });
        }
    }
    return result;
}

std::vector<ModifierTransition> UiPlatform::decodeModifierTransitions(uint32_t oldMask, uint32_t newMask) {
    return decodeModifierTransitions(static_cast<uint64_t>(oldMask), static_cast<uint64_t>(newMask));
}

bool UiPlatform::blitTileToCanvas(uint8_t* dstCanvas, int canvasWidth, int canvasHeight, int canvasPitch,
                                 const uint8_t* srcTile, int tileX, int tileY, int tileWidth, int tileHeight, int tilePitch) {
    if (!dstCanvas || !srcTile || canvasWidth <= 0 || canvasHeight <= 0 || tileWidth <= 0 || tileHeight <= 0) {
        return false;
    }
    if (tileX >= canvasWidth || tileY >= canvasHeight || (tileX + tileWidth) <= 0 || (tileY + tileHeight) <= 0) {
        return false;
    }

    int srcStartX = 0;
    int srcStartY = 0;
    int dstStartX = tileX;
    int dstStartY = tileY;

    if (dstStartX < 0) {
        srcStartX = -dstStartX;
        dstStartX = 0;
    }
    if (dstStartY < 0) {
        srcStartY = -dstStartY;
        dstStartY = 0;
    }

    int dstEndX = std::min(canvasWidth, tileX + tileWidth);
    int dstEndY = std::min(canvasHeight, tileY + tileHeight);

    int copyWidth = dstEndX - dstStartX;
    int copyHeight = dstEndY - dstStartY;

    if (copyWidth <= 0 || copyHeight <= 0) {
        return false;
    }

    size_t copyBytes = static_cast<size_t>(copyWidth) * 4;
    for (int row = 0; row < copyHeight; ++row) {
        const uint8_t* srcRow = srcTile + (srcStartY + row) * tilePitch + (srcStartX * 4);
        uint8_t* dstRow = dstCanvas + (dstStartY + row) * canvasPitch + (dstStartX * 4);
        std::memcpy(dstRow, srcRow, copyBytes);
    }
    return true;
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

UiPlatformRect UiPlatform::computeSpringRect(const UiPlatformRect& base, float hoverT, float pressT) {
    float hT = std::clamp(hoverT, 0.0f, 1.25f);
    float pT = std::clamp(pressT, -0.25f, 1.25f);
    float scaleDelta = (0.028f * hT) - (0.048f * pT);
    float dx = std::max(base.width() * scaleDelta * 0.5f, 1.2f * hT - 1.8f * pT);
    float dy = std::max(base.height() * scaleDelta * 0.5f, 0.9f * hT - 1.5f * pT);
    float liftY = (-1.8f * hT) + (1.6f * pT);
    return base.inflate(dx, dy).offset(0.0f, liftY);
}

} // namespace cppdesk

