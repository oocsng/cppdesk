#include "theme.hpp"

#include <cmath>
#include <utility>

namespace cppdesk {

ui::Color COL_BG_MAIN{};
ui::Color COL_BG_NAV{};
ui::Color COL_BG_CARD{};
ui::Color COL_BG_SUBTLE{};
ui::Color COL_BG_CARD_ALT{};
ui::Color COL_BG_INPUT{};
ui::Color COL_BG_INPUT_FOCUS{};
ui::Color COL_BORDER{};
ui::Color COL_BORDER_ALT{};
ui::Color COL_BORDER_FOCUS{};
ui::Color COL_PRIMARY_ACCENT{};
ui::Color COL_PRIMARY_ACCENT_HV{};
ui::Color COL_SEC_BTN_BG{};
ui::Color COL_SEC_BTN_HV{};
ui::Color COL_SUCCESS{};
ui::Color COL_WARNING{};
ui::Color COL_DANGER{};
ui::Color COL_DANGER_HV{};
ui::Color COL_TEXT_PRIMARY{};
ui::Color COL_TEXT_SECONDARY{};
ui::Color COL_TEXT_MUTED{};
ui::Color COL_TEXT_ACCENT{};
ui::Color COL_TEXT_ON_ACCENT{};
ui::Color COL_STAGE_BG{};
ui::Color COL_HUD_BG{};
ui::Color COL_HUD_TEXT{};
ui::Color COL_HUD_TEXT_SECONDARY{};
ui::Color COL_HUD_TEXT_MUTED{};
ui::Color COL_HUD_TEXT_ACCENT{};

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
    { "COL_WARNING",           &COL_WARNING,           rgba(168, 92, 18),   rgba(217, 130, 43) },
    { "COL_DANGER",            &COL_DANGER,            rgba(199, 62, 58),   rgba(199, 62, 58) },
    { "COL_DANGER_HV",         &COL_DANGER_HV,         rgba(173, 50, 47),   rgba(173, 50, 47) },
    { "COL_TEXT_PRIMARY",      &COL_TEXT_PRIMARY,      rgba(38, 35, 32),    rgba(250, 247, 242) },
    { "COL_TEXT_SECONDARY",    &COL_TEXT_SECONDARY,    rgba(84, 78, 70),     rgba(184, 177, 166) },
    { "COL_TEXT_MUTED",        &COL_TEXT_MUTED,        rgba(104, 97, 88),    rgba(168, 161, 150) },
    { "COL_TEXT_ACCENT",       &COL_TEXT_ACCENT,       rgba(160, 70, 38),   rgba(228, 138, 108) },
    { "COL_TEXT_ON_ACCENT",    &COL_TEXT_ON_ACCENT,    rgba(255, 255, 255), rgba(255, 255, 255) },
    { "COL_STAGE_BG",          &COL_STAGE_BG,          rgba(235, 229, 219), rgba(20, 19, 18) },
    { "COL_HUD_BG",            &COL_HUD_BG,            rgba(28, 27, 25, 0.94f), rgba(28, 27, 25, 0.94f) },
    { "COL_HUD_TEXT",           &COL_HUD_TEXT,           rgba(250, 247, 242), rgba(250, 247, 242) },
    { "COL_HUD_TEXT_SECONDARY", &COL_HUD_TEXT_SECONDARY, rgba(184, 177, 166), rgba(184, 177, 166) },
    { "COL_HUD_TEXT_MUTED",     &COL_HUD_TEXT_MUTED,     rgba(168, 161, 150), rgba(168, 161, 150) },
    { "COL_HUD_TEXT_ACCENT",    &COL_HUD_TEXT_ACCENT,    rgba(228, 138, 108), rgba(228, 138, 108) },
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

namespace {
double relativeLuminance(const ui::Color& c) {
    auto lin = [](float v) {
        return v <= 0.03928f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}
} // namespace

double contrastRatio(const ui::Color& a, const ui::Color& b) {
    double la = relativeLuminance(a);
    double lb = relativeLuminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

} // namespace cppdesk
