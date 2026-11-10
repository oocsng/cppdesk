# UI Foundation: Theme Module and DPI Scaling

Date: 2026-11-11
Status: Draft for review

## Background

CppDesk is a native Direct2D client on Windows, with a separate macOS UI in `src/ui/ui_window_mac.mm`. The UI is mostly one 9,000-line file, `src/ui/ui_window.cpp`.

The wider goal is to improve the UX across four areas: the Connect screen and address book, the in-session toolbar and side drawer, the file browser and transfers, and the overall look and feel. Each area is its own sub-project with its own spec. This spec covers only the first, the look-and-feel foundation, because the other three depend on it.

## Agreed brief

- **Goal:** make CppDesk nicer to use. This spec lays the shared base for the screen-level work that follows.
- **Visual direction:** palette A. Keep the warm espresso and terracotta look and tighten it. No new color direction.
- **Foundation covers:** a shared theme module, spacing/radius/type/motion scales, and DPI scaling on Windows.
- **Assumptions:** the app stays a native Direct2D client. The wire protocol and existing features do not change. The macOS port is not touched.

## Current state (verified in code)

- The palette is a set of mutable globals `COL_*` in an anonymous namespace at `ui_window.cpp:100-159`. Each color is written twice: once as a light initializer and once inside `updateActivePalette(darkT)`, where it is blended with a dark value. The two lists can drift apart.
- No other file can use the palette because of the anonymous namespace.
- The comment at line 152 claims at least 4.5:1 contrast in both modes. By calculation, `COL_TEXT_MUTED` is about 3.1:1 on the light background and about 4.0:1 on the dark one.
- Some colors are hardcoded outside the palette, for example the transfer HUD pill at `ui_window.cpp:3003`.
- The process is already per-monitor DPI aware: `src/cppdesk.manifest` declares PerMonitorV2 and `src/main.cpp:41` calls `SetProcessDpiAwarenessContext`.
- The Direct2D render target is created with `dpiX = dpiY = 96` (`ui_window.cpp:1486`) and all layout is in raw pixels. On a display scaled above 100% the UI is crisp but proportionally too small, and the window opens at 1220x760 physical pixels (`ui_window.cpp:304-305`) on every monitor.
- There is no `WM_DPICHANGED` handling.

## Design

### 1. Theme module

New files `src/ui/theme.hpp` and `src/ui/theme.cpp`.

- Every color token is defined once as a light/dark pair in one table. `updateActivePalette(darkT)` moves into this module and computes the current value of each token from that table. The existing blend animation is unchanged.
- The existing `COL_*` names stay. They become `extern` globals declared in `theme.hpp` and defined in `theme.cpp`, so call sites in `ui_window.cpp` do not change.
- Palette A is refined, not replaced. Hues stay. Any text token that carries information and falls under 4.5:1 is adjusted, `COL_TEXT_MUTED` first. One-off colors, including the transfer HUD pill, are replaced with tokens.
- New `constexpr` scales in the theme header:
  - Spacing: 4, 8, 12, 16, 24, 32.
  - Radius: small, medium, large.
  - Type: size and weight for title, body, label and caption.
  - Motion: speeds for hover, press, panel slide and the theme blend, replacing numbers scattered through the draw code.
- Existing layout literals are not rewritten as part of this work. The three screen-level sub-projects adopt the scales as they rework each screen.
- The module uses `D2D1_COLOR_F`, so it is Windows-only.

### 2. DPI scaling (Windows)

- `CppDeskWindow` keeps a `dpi_` member from `GetDpiForWindow`. The render target is created with `dpiX = dpiY = dpi_`. Direct2D then treats layout numbers as DIPs and scales to physical pixels, so existing layout literals are unchanged.
- The six `GetClientRect` sites (`ui_window.cpp` lines 583, 1477, 7517, 7647, 7878, 7982) convert the pixel size to a DIP width and height by dividing by `dpi_ / 96`.
- Mouse messages (`WM_MOUSEMOVE`, `WM_LBUTTONDOWN/UP`, `WM_RBUTTONDOWN/UP`, `WM_MBUTTONDOWN/UP`, lines 1026-1052) divide coordinates by the same factor before calling `onMouseMove` and `onMouseButton`. Conversion happens only there.
- A new `WM_DPICHANGED` handler updates `dpi_`, resizes the window to the rectangle Windows suggests, updates the render target DPI and invalidates the window. Text formats are in DIPs and need no change.
- The initial window size is expressed as 1220x760 DIPs and converted to pixels for the monitor it opens on.
- The conversion between pixels and DIPs is one small pure function so it can be unit tested.

Items to verify during implementation, because they may use raw pixels:
- The remote-screen canvas in 1:1 `ScaleMode`: 1:1 must map remote pixels to physical pixels, not DIPs. Fit and fill modes should be unaffected.
- Mapping a canvas click to remote coordinates must stay ratio-based.
- Text-input caret and IME positions.
- Borderless fullscreen.

### 3. Order of work

1. Extract tokens into the theme module with no value changes. The build passes and the app looks identical.
2. Refine palette A (muted text, one-off colors). The contrast test passes.
3. Add the spacing, radius, type and motion scales.
4. DPI: render target, client size, mouse conversion, `WM_DPICHANGED`, initial window size.

Each step is verified on its own so a regression can be traced to one step.

## Testing

Added to `tests/test_suite.cpp`, run with `build.ps1 -Test`:

- **Contrast test.** Every text token against its backgrounds meets 4.5:1 in light and dark. Large text, decorative elements and borders are exempt, and the exemptions are listed explicitly in the test.
- **Theme table test.** With `darkT = 0` every token equals its light value, and with `darkT = 1` its dark value.
- **DPI helper test.** The pixel-to-DIP conversion at 100%, 125%, 150% and 200%, including rounding.
- **Manual check** at 100%, 150% and 200% scaling, and while dragging the window between monitors of different scale.

## Constraints and risks

- `build.ps1` and `CMakeLists.txt` must list `theme.cpp`. `build.ps1` already has uncommitted changes.
- The working tree has uncommitted file-browser work that touches `ui_window.cpp`. Committing or stashing it before implementation keeps the two changes reviewable separately. Nothing is committed without an explicit request.
- DPI changes touch input handling. A mistake would misplace clicks on scaled displays, which the manual check is meant to catch.
- Project rules apply at implementation time: `/ui-ux-pro-max`, `/impeccable` and `/taste-skill` for UI design work, `/agent-reach` before research or planning, and the review skills followed by `gsd-code-fixer` afterward.

## Out of scope

- Redesigning the Connect screen, the in-session toolbar and side drawer, or the file browser. Those are separate sub-projects.
- macOS UI changes, and a platform-neutral token table.
- Splitting `ui_window.cpp` into panel files.
- Any change to the network protocol or relay.
