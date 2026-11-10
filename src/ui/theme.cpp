#include "theme.hpp"

namespace cppdesk {

D2D1_COLOR_F COL_BG_MAIN{};
D2D1_COLOR_F COL_BG_NAV{};
D2D1_COLOR_F COL_BG_CARD{};
D2D1_COLOR_F COL_BG_SUBTLE{};
D2D1_COLOR_F COL_BG_CARD_ALT{};
D2D1_COLOR_F COL_BG_INPUT{};
D2D1_COLOR_F COL_BG_INPUT_FOCUS{};
D2D1_COLOR_F COL_BORDER{};
D2D1_COLOR_F COL_BORDER_ALT{};
D2D1_COLOR_F COL_BORDER_FOCUS{};
D2D1_COLOR_F COL_PRIMARY_ACCENT{};
D2D1_COLOR_F COL_PRIMARY_ACCENT_HV{};
D2D1_COLOR_F COL_SEC_BTN_BG{};
D2D1_COLOR_F COL_SEC_BTN_HV{};
D2D1_COLOR_F COL_SUCCESS{};
D2D1_COLOR_F COL_WARNING{};
D2D1_COLOR_F COL_DANGER{};
D2D1_COLOR_F COL_DANGER_HV{};
D2D1_COLOR_F COL_TEXT_PRIMARY{};
D2D1_COLOR_F COL_TEXT_SECONDARY{};
D2D1_COLOR_F COL_TEXT_MUTED{};
D2D1_COLOR_F COL_TEXT_ACCENT{};
D2D1_COLOR_F COL_TEXT_ON_ACCENT{};
D2D1_COLOR_F COL_STAGE_BG{};

namespace {

// Surfaces: cream linen <-> roasted espresso. Accent stays terracotta in both modes.
// Status colors and text-on-accent are mode-independent (dark == light).
const PaletteToken kTokens[] = {
    { "COL_BG_MAIN",           &COL_BG_MAIN,           rgba(250, 248, 245), rgba(28, 27, 25) },
    { "COL_BG_NAV",            &COL_BG_NAV,            rgba(245, 240, 234), rgba(34, 33, 30) },
    { "COL_BG_CARD",           &COL_BG_CARD,           rgba(255, 255, 255), rgba(38, 37, 34) },
    { "COL_BG_SUBTLE",         &COL_BG_SUBTLE,         rgba(243, 238, 230), rgba(46, 44, 40) },
    { "COL_BG_CARD_ALT",       &COL_BG_CARD_ALT,       rgba(249, 243, 236), rgba(51, 44, 38) },
    { "COL_BG_INPUT",          &COL_BG_INPUT,          rgba(243, 238, 230), rgba(34, 32, 29) },
    { "COL_BG_INPUT_FOCUS",    &COL_BG_INPUT_FOCUS,    rgba(255, 255, 255), rgba(42, 40, 36) },
    { "COL_BORDER",            &COL_BORDER,            rgba(228, 222, 212), rgba(59, 56, 51) },
    { "COL_BORDER_ALT",        &COL_BORDER_ALT,        rgba(224, 198, 185), rgba(87, 68, 57) },
    { "COL_BORDER_FOCUS",      &COL_BORDER_FOCUS,      rgba(217, 119, 87),  rgba(217, 119, 87) },
    { "COL_PRIMARY_ACCENT",    &COL_PRIMARY_ACCENT,    rgba(217, 119, 87),  rgba(217, 119, 87) },
    { "COL_PRIMARY_ACCENT_HV", &COL_PRIMARY_ACCENT_HV, rgba(193, 95, 60),   rgba(228, 138, 108) },
    { "COL_SEC_BTN_BG",        &COL_SEC_BTN_BG,        rgba(240, 234, 225), rgba(44, 42, 38) },
    { "COL_SEC_BTN_HV",        &COL_SEC_BTN_HV,        rgba(230, 222, 210), rgba(58, 55, 49) },
    { "COL_SUCCESS",           &COL_SUCCESS,           rgba(46, 125, 96),   rgba(46, 125, 96) },
    { "COL_WARNING",           &COL_WARNING,           rgba(217, 130, 43),  rgba(217, 130, 43) },
    { "COL_DANGER",            &COL_DANGER,            rgba(199, 62, 58),   rgba(199, 62, 58) },
    { "COL_DANGER_HV",         &COL_DANGER_HV,         rgba(173, 50, 47),   rgba(173, 50, 47) },
    { "COL_TEXT_PRIMARY",      &COL_TEXT_PRIMARY,      rgba(38, 35, 32),    rgba(250, 247, 242) },
    { "COL_TEXT_SECONDARY",    &COL_TEXT_SECONDARY,    rgba(105, 98, 89),   rgba(184, 177, 166) },
    { "COL_TEXT_MUTED",        &COL_TEXT_MUTED,        rgba(148, 140, 130), rgba(128, 121, 111) },
    { "COL_TEXT_ACCENT",       &COL_TEXT_ACCENT,       rgba(193, 95, 60),   rgba(228, 138, 108) },
    { "COL_TEXT_ON_ACCENT",    &COL_TEXT_ON_ACCENT,    rgba(255, 255, 255), rgba(255, 255, 255) },
    { "COL_STAGE_BG",          &COL_STAGE_BG,          rgba(235, 229, 219), rgba(20, 19, 18) },
};

// Live tokens hold light values from startup, before any window calls updateActivePalette.
// Must stay below kTokens: initialization order within a translation unit follows definition order.
const bool kPaletteInitialized = (updateActivePalette(0.0f), true);

} // namespace

void updateActivePalette(float darkT) {
    darkT = std::clamp(darkT, 0.0f, 1.0f);
    for (const auto& tok : kTokens) {
        *tok.live = lerpColor(tok.light, tok.dark, darkT);
    }
}

std::span<const PaletteToken> paletteTokens() {
    return kTokens;
}

} // namespace cppdesk
