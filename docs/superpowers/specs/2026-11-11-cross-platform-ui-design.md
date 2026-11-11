# Cross-Platform UI and macOS Host

Date: 2026-11-11
Status: Draft for review

## Intent

CppDesk should match RustDesk in feel, core functions and features on the three desktop platforms the
user cares about: Windows, macOS and Linux. Mobile and web are out.

Today only Windows has the real app. `src/ui/ui_window.cpp` is a 10,400-line Direct2D UI with every
screen. The Linux UI (`ui_window_linux.cpp`, about 420 lines) only opens a window that shows the remote
screen, and the macOS UI (`ui_window_mac.mm`, about 400 lines) has never been built. Every later
feature (video codec, direct connections, UI polish) would otherwise have to be built three times or
reach Windows users only. This project gives all three platforms the same UI and makes a Mac a full
peer, able both to control other machines and to be controlled, the way RustDesk does.

This is the first of several projects agreed with the user, in this order: this one, then the video
codec, direct connections (NAT hole punching), trust and security, a UI feel pass, and Wayland.

## Goals

1. **One UI on all three platforms.** Every screen, modal, drawer and setting that Windows has today
   (dashboard, session view and tab bar, settings, file transfer and file explorer drawers, sync,
   chat, approval and TOTP modals, run task, recording player, omnibar, toasts) appears on macOS and
   Linux, drawn by the same screen code.
2. **Windows looks and behaves exactly as it does today** at every step.
3. **macOS works as viewer and host**: it builds, passes the test suite, connects to Windows and Linux
   hosts, and can itself be controlled (screen capture and input injection).
4. **Platform features follow these rules** (decided by the user):
   - **Hidden on macOS and Linux:**
     - Windows service install, uninstall, start and stop;
     - the "Elevate Admin" button;
     - Windows Action Center wording;
     - desktop shortcut creation.
   - **Mapped to the platform equivalent:**
     - the tray icon becomes a menu-bar item on macOS and a tray icon on Linux;
     - notifications use the native notification system on each platform.
   - **Attended host only on macOS:** the app is open and a user is logged in. Incoming requests use the
     existing approval modal. The Screen Recording and Accessibility prompts are explained in the UI,
     including when a permission is revoked or only takes effect after a relaunch.
5. **macOS joins CI.** GitHub Actions builds and tests macOS on every push next to Windows and Linux.

## Non-goals

- Wayland (separate later project). Linux stays X11, including XWayland sessions.
- The video codec, NAT hole punching and security changes (separate projects).
- Native menus, a native look per platform, and screen-reader accessibility.
- Mobile, web, and signed or notarized macOS downloads.
- Unattended access on macOS and Linux (launch agent, systemd service, login-screen capture, start at
  login). This is a follow-up project, together with the platform service equivalents.

## Anti-goals

These would make the project a failure even if everything technically works:

- Windows looks or feels different, gets slower, or loses a feature.
- The screen code forks per platform, so a change has to be made two or three times.
- A large UI toolkit (Qt, GTK, Flutter, Dear ImGui) is added or the UI is rewritten.

## Constraints

- C++20. Windows keeps building with `build.ps1` (MinGW g++). Linux and macOS build with CMake.
- Allowed new dependencies:
  - **Linux:** system Cairo and Pango (`libcairo2-dev`, `libpango1.0-dev`).
  - **macOS:** Apple frameworks only (AppKit, Core Graphics, Core Text, ScreenCaptureKit).
- The macOS minimum moves from 12 to 12.3 (ScreenCaptureKit). The README and `Info.plist` must say so.
- The wire protocol does not change. A macOS peer must interoperate with Windows and Linux peers.
- No Mac is available locally. macOS is verified by CI. The app on a real Mac screen is checked by the
  user.

## Approaches considered

1. **Shared drawing layer (chosen).** Keep the immediate-mode screen code and make it platform-neutral.
   It draws through a small canvas interface with three backends: Direct2D, Core Graphics + Core Text,
   and Cairo + Pango. This keeps the look, needs no rewrite, and can be done in steps that keep Windows
   working throughout. The costs are two new backends and per-platform text input.
2. **Rewrite in Qt 6.** Native integration, accessibility, Wayland and HiDPI come for free. Rejected:
   it means a full rewrite of 10,400 lines and a large LGPL dependency, and it loses the current look.
3. **Dear ImGui.** The quickest port. Rejected: the result feels like a developer tool, and its text
   input and accessibility are weak, which works against a RustDesk-level feel.

The user picked approach 1.

## Current state (verified in code)

- The screen code already redraws from state every frame and draws almost entirely through its own
  helpers in `CppDeskWindow`:
  - **Shapes:** `fillRoundRect`, `strokeRoundRect`, `drawCardShadow`, `drawCardSurface`.
  - **Text and controls:** `drawText`, `drawButton`, `drawTextField`, `drawToggleSwitch`.
  - **Icons and indicators:** `drawIcon*`, `drawPulseDot`, `drawSparkline`, `renderQrCodeDirect2D`.
  - About 40 raw Direct2D calls remain (ellipses, lines, geometries, bitmaps).
- Windows types leak into the screen code: `D2D1_COLOR_F` (66 uses) and the eight DirectWrite formats
  `fmtHeroId_`, `fmtHeading_`, `fmtSubheading_`, `fmtBody_`, `fmtBodyBold_`, `fmtSmall_`, `fmtMono_`,
  `fmtTooltip_`.
- The Windows platform services used are window messages (40 `WM_` cases), timers, cursors, mouse
  tracking, `GetOpenFileName` and `IFileDialog`, file drop (`WM_DROPFILES`), `ShellExecute`, per-monitor
  DPI (`WM_DPICHANGED`) and the clipboard (through `ClipboardManager`).
- `ui_window.hpp` declares a different `CppDeskWindow` per platform. The Linux and macOS ones only have
  `create`, `messageLoop` and `close`.
- The theme tokens (`src/ui/theme.*`) and DPI helpers (`src/ui/dpi.hpp`) are already split out.
  `theme.cpp` is Windows-only in CMake today.
- **macOS engine:**
  - **Crypto:** real, using Security framework ECDH and CommonCrypto GCM, but never verified against
    Windows. Linux needed the CNG raw secret reversed to match, and macOS very likely needs the same.
  - **Capture:** uses `CGDisplayCreateImage`, which Apple removed from the macOS 15 SDK. The code
    declares it by hand.
  - **Input:** injection uses `CGEvent`.

## Design

### Structure

- **`src/ui/canvas.hpp`:** platform-neutral types and the canvas interface.
  - **Types:**
    - `Color` (float RGBA, convertible from the theme tokens).
    - `FontRole`, one per current format: HeroId, Heading, Subheading, Body, BodyBold, Small, Mono and
      Tooltip.
    - `HAlign` and `VAlign`.
    - `Rect`, reusing `UiRect`.
    - `Image`, an opaque handle to an uploaded BGRA bitmap.
  - **Operations:**
    - Fill and stroke rounded rects, ellipses, lines and polygons.
    - Draw and measure UTF-8 text in a `FontRole` with alignment and clipping.
    - Create and update an `Image` from BGRA, including a dirty sub-rectangle, and draw it scaled.
    - Push and pop a clip, an opacity and a 2D transform (translate, scale, rotate). The screen code
      uses about 32 `SetTransform` calls for its spring animations and icons, so this is required.
    - Fill and stroke arbitrary paths (move, line and arc segments) for the icons and geometries that
      use Direct2D path geometry today.
- **`src/ui/app_ui.cpp` / `app_ui.hpp`:** all screen and widget code moves here from `ui_window.cpp`.
  It takes input events and draws a frame through a `Canvas&`. No platform headers.
- **`src/ui/platform_shell.hpp`:** what the screen code needs from the platform:
  - invalidate and redraw, timers, cursor shape and the DPI scale;
  - clipboard text, open-file and choose-folder dialogs, and opening a URL or folder;
  - dropped files, text input (committed UTF-8 text), and showing a notification;
  - a tray or menu-bar item with a small menu (show the window, quit);
  - capability queries, so the screen code can hide a Windows-only control instead of branching on the
    platform.
- **Per-platform files:**
  - **Windows:** `ui_window.cpp` keeps only the Win32 window, the message loop, the Direct2D canvas
    backend and the Windows shell.
  - **macOS:** `ui_window_mac.mm` holds the NSWindow and NSView shell and the Core Graphics + Core Text
    backend.
  - **Linux:** `ui_window_linux.cpp` holds the Xlib shell and the Cairo + Pango backend on an Xlib
    surface. Each platform keeps its `create`/`messageLoop`/`close` entry points, so `main.cpp` does not
    change.
- **Remote screen:** the session view draws the decoded remote frame as an `Image`, updated by dirty
  rectangle each frame. This must stay at least as fast as today's Direct2D path on Windows.

### macOS host

- **Capture:** move screen capture to ScreenCaptureKit (macOS 12.3+). Keep the existing
  dirty-tile pipeline and feed it frames from an `SCStream`.
- **Permissions:** request and explain the Screen Recording and Accessibility permissions in the UI.
  Without them the Mac can still act as a viewer.
- **Crypto:** fix the ECDH raw secret byte order if needed, and prove macOS ↔ Windows interop the same
  way Linux was proven. CI runs a test that derives a key against known vectors, or a cross-process
  exchange.

### Order of work

Each step leaves all three platforms building and the Windows suite green:

1. **Neutral types.** Replace `D2D1_COLOR_F` and the DirectWrite formats in helper signatures with
   `Color` and `FontRole`. This is Windows-only and has no visible change.
2. **Canvas and the screen-code move.** Introduce `Canvas` with the Direct2D backend and move the
   screen code to `app_ui.cpp`. Windows must look the same: compare screenshots of each screen before
   and after.
3. **Linux.** Write the Cairo + Pango backend and the Xlib shell, then remove the stub UI. Linux gets
   every screen.
4. **macOS build.** Add the macOS CI job, get the suite to pass, and write the Core Graphics + Core
   Text backend and the AppKit shell.
5. **macOS host.** ScreenCaptureKit capture, permission flows, and the crypto interop check.

## Testing

- **Recording canvas:** a test backend records draw calls, so every screen can be rendered headless in
  the test suite on all three platforms. Tests assert that a frame renders and that key labels appear.
  This replaces today's habit of verifying the UI only by eye.
- **Shared backend scene:** a small test scene (shapes, text, image) is drawn by each real backend into
  an offscreen buffer to check it renders without errors and roughly fills the expected regions.
- **Windows regression (the step 2 gate):**
  - Before step 2 starts, the recording canvas dumps the draw-call stream of each screen in a fixed set
    of states (fixed size, theme, data and animation time) as golden files.
  - After the move, the Windows backend must produce the same streams.
  - This is automated and runs in CI. The user does one visual spot check of the main screens before
    step 2 merges. *(Builder's call, recorded here.)*
- **CI:** builds and runs the suite on Windows, Linux (Xvfb) and macOS for every push. macOS GUI and
  permission behavior are checked by the user on a real Mac.

## Calls made during review

These were settled without the user. Each can still be overridden.

- **Merging:** each step merges to `main` once CI is green on all three platforms.
  - Step 3 replaces the Linux stub UI when it merges.
  - Step 4 merges macOS UI code that CI builds and tests but nobody has run on a screen. The release
    notes label it untested until the user has tried it on a Mac.
- **Performance:**
  - **Windows:** no regression. Frame time for the session view is measured before and after step 2.
  - **Linux and macOS:** the remote view must keep up with 30 fps at 1920x1080.
  - **Fallback:** if Cairo or Core Graphics can't reach that, the remote frame uses a faster image path
    (XShm on Linux, a layer-backed view on macOS) while the rest of the UI stays on the canvas.
- **Keyboard on macOS:** UI shortcuts follow the platform (Cmd+C, Cmd+V and so on). For remote input,
  Cmd maps to Ctrl when controlling a Windows or Linux host, using the existing key mapping helpers in
  `ui_platform.hpp`.

## Risks

- **Text** is the hardest part to make identical. DirectWrite, Core Text and Pango measure and wrap
  differently, so small layout differences between platforms are expected and acceptable. Overlaps and
  clipped labels are not.
- **The step 2 move** is large: most of 10,400 lines change files. It should be mechanical, without
  behavior changes mixed in.
- **macOS changes can't be run here.** Each macOS step depends on CI and on the user testing on a Mac.

## Left to the builder

- Exact canvas method names, how `FontRole` maps to sizes and weights per platform, and the font
  families.
- How `ui_window.cpp` is split beyond the boundaries above, and how animations and widget state move.
- The Linux text input method (XIM or plain `XLookupString` UTF-8), as long as non-ASCII typing works.
- The macOS CI runner version and how dependencies are installed (Homebrew or vendored).
