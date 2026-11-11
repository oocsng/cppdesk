#pragma once

// Shared UI palette for the client (warm cream / espresso with terracotta accent).
// Every token is defined once as a light/dark pair in theme.cpp; updateActivePalette() blends the live
// COL_* globals between the two.

#include "canvas.hpp"
#include <algorithm>
#include <cstdint>
#include <span>

namespace cppdesk {

// The palette is in platform-neutral colors; the helpers come from the canvas types.
using ui::rgba;
using ui::withAlpha;
using ui::lerpColor;

extern ui::Color COL_BG_MAIN;
extern ui::Color COL_BG_NAV;
extern ui::Color COL_BG_CARD;
extern ui::Color COL_BG_SUBTLE;
extern ui::Color COL_BG_CARD_ALT;
extern ui::Color COL_BG_INPUT;
extern ui::Color COL_BG_INPUT_FOCUS;
extern ui::Color COL_BORDER;
extern ui::Color COL_BORDER_ALT;
extern ui::Color COL_BORDER_FOCUS;
extern ui::Color COL_PRIMARY_ACCENT;
extern ui::Color COL_PRIMARY_ACCENT_HV;
extern ui::Color COL_SEC_BTN_BG;
extern ui::Color COL_SEC_BTN_HV;
extern ui::Color COL_SUCCESS;
extern ui::Color COL_WARNING;
extern ui::Color COL_DANGER;
extern ui::Color COL_DANGER_HV;
extern ui::Color COL_TEXT_PRIMARY;
extern ui::Color COL_TEXT_SECONDARY;
extern ui::Color COL_TEXT_MUTED;
extern ui::Color COL_TEXT_ACCENT;
extern ui::Color COL_TEXT_ON_ACCENT;
extern ui::Color COL_STAGE_BG;
extern ui::Color COL_HUD_BG;   // floating overlay pill; dark in both modes
extern ui::Color COL_HUD_TEXT;            // text on always-dark overlays: fixed light colors in both modes
extern ui::Color COL_HUD_TEXT_SECONDARY;
extern ui::Color COL_HUD_TEXT_MUTED;
extern ui::Color COL_HUD_TEXT_ACCENT;

// Sets every live COL_* token to lerp(light, dark, darkT). darkT is clamped to 0..1.
void updateActivePalette(float darkT);

struct PaletteToken {
    const char*          name;
    ui::Color*        live;
    ui::Color         light;
    ui::Color         dark;
};

std::span<const PaletteToken> paletteTokens();

// WCAG 2.x contrast ratio (1..21) between two opaque colors; alpha is ignored.
double contrastRatio(const ui::Color& a, const ui::Color& b);

// Layout, type and motion scales. Values are in DIPs. New screen work should use these instead of literals.
namespace theme {

namespace space { inline constexpr float xs = 4.0f, sm = 8.0f, md = 12.0f, lg = 16.0f, xl = 24.0f, xxl = 32.0f; }

namespace radius { inline constexpr float sm = 6.0f, md = 12.0f, lg = 16.0f; }

namespace type {
struct Style { float size; int weight; };  // weight: DirectWrite numeric weight (400 normal, 600 semi-bold, 700 bold)
inline constexpr Style hero{33.0f, 700}, heading{18.5f, 600}, subheading{14.5f, 600}, body{13.0f, 400},
                       bodyBold{13.0f, 600}, caption{11.5f, 600}, mono{14.0f, 700}, tooltip{11.5f, 400};
}

namespace motion {
struct Spring { float omega; float zeta; };  // damped spring: stiffness, damping ratio
inline constexpr Spring themeBlend{20.0f, 0.92f}, hover{32.0f, 0.72f}, press{38.0f, 0.68f},
                        standard{28.0f, 0.74f}, menu{32.0f, 0.76f}, panel{26.0f, 0.78f},
                        drawer{26.0f, 0.80f}, scroll{32.0f, 0.88f};
}

} // namespace theme

} // namespace cppdesk
