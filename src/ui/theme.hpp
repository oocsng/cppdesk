#pragma once

// Shared UI palette for the Windows Direct2D client (warm cream / espresso with terracotta accent).
// Every token is defined once as a light/dark pair in theme.cpp; updateActivePalette() blends the live
// COL_* globals between the two.

#include <d2d1.h>
#include <algorithm>
#include <cstdint>
#include <span>

namespace cppdesk {

inline D2D1_COLOR_F rgba(uint8_t r, uint8_t g, uint8_t b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

inline D2D1_COLOR_F withAlpha(D2D1_COLOR_F c, float aMultiplier) {
    return D2D1::ColorF(c.r, c.g, c.b, std::clamp(c.a * aMultiplier, 0.0f, 1.0f));
}

inline D2D1_COLOR_F lerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    );
}

extern D2D1_COLOR_F COL_BG_MAIN;
extern D2D1_COLOR_F COL_BG_NAV;
extern D2D1_COLOR_F COL_BG_CARD;
extern D2D1_COLOR_F COL_BG_SUBTLE;
extern D2D1_COLOR_F COL_BG_CARD_ALT;
extern D2D1_COLOR_F COL_BG_INPUT;
extern D2D1_COLOR_F COL_BG_INPUT_FOCUS;
extern D2D1_COLOR_F COL_BORDER;
extern D2D1_COLOR_F COL_BORDER_ALT;
extern D2D1_COLOR_F COL_BORDER_FOCUS;
extern D2D1_COLOR_F COL_PRIMARY_ACCENT;
extern D2D1_COLOR_F COL_PRIMARY_ACCENT_HV;
extern D2D1_COLOR_F COL_SEC_BTN_BG;
extern D2D1_COLOR_F COL_SEC_BTN_HV;
extern D2D1_COLOR_F COL_SUCCESS;
extern D2D1_COLOR_F COL_WARNING;
extern D2D1_COLOR_F COL_DANGER;
extern D2D1_COLOR_F COL_DANGER_HV;
extern D2D1_COLOR_F COL_TEXT_PRIMARY;
extern D2D1_COLOR_F COL_TEXT_SECONDARY;
extern D2D1_COLOR_F COL_TEXT_MUTED;
extern D2D1_COLOR_F COL_TEXT_ACCENT;
extern D2D1_COLOR_F COL_TEXT_ON_ACCENT;
extern D2D1_COLOR_F COL_STAGE_BG;
extern D2D1_COLOR_F COL_HUD_BG;   // floating overlay pill; dark in both modes

// Sets every live COL_* token to lerp(light, dark, darkT). darkT is clamped to 0..1.
void updateActivePalette(float darkT);

struct PaletteToken {
    const char*          name;
    D2D1_COLOR_F*        live;
    D2D1_COLOR_F         light;
    D2D1_COLOR_F         dark;
};

std::span<const PaletteToken> paletteTokens();

// WCAG 2.x contrast ratio (1..21) between two opaque colors; alpha is ignored.
double contrastRatio(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b);

} // namespace cppdesk
