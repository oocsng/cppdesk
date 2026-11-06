# Design: Native X11 Linux Support for CppDesk

Date: 2026-11-07
Topic: Linux Support (Host + Viewer GUI)
Status: Proposed

## 1. Intent and Why
CppDesk currently supports Windows (Direct2D/DXGI/GDI) and macOS (Cocoa/Metal/CoreGraphics).
Adding native Linux support enables users to run CppDesk on Linux workstations and servers
both as a remote desktop host (to be viewed and controlled remotely) and as a desktop viewer
client (to connect to remote Windows, macOS, or Linux endpoints).

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- Native X11 host screen capture using MIT-SHM (`XShmGetImage`) with fallback to `XGetImage`.
- Multi-monitor enumeration on Linux using Xrandr (`XRRGetScreenResourcesCurrent`).
- Native X11 input injection for mouse move, click, scroll wheel, and keyboard keys via XTest (`libXtst`).
- Native X11 viewer window implementation (`src/ui/ui_window_linux.cpp`) implementing `CppDeskWindow` with software rasterizer blitting.
- Seamless build support in `CMakeLists.txt` for Linux targets (`CppDesk` binary and `CppDeskTests` test runner).
- Complete interoperability with existing Windows and macOS peers over existing CppDesk network protocol, encryption, and tile codec pipelines.

### Non-Goals
- Pure Wayland-native compositing without XWayland (Wayland requires PipeWire portals and separate security permissions; deferred to a future extension).
- PulseAudio/ALSA real-time bidirectional voice intercom on Linux (stubbed safely for now, keeping voice protocol intact).
- Windows-specific services (e.g. Windows Service Manager / UAC elevation) on Linux; Linux daemon runs as a standard systemd service using command line flags.

### Anti-Goals
- Introducing heavy UI frameworks such as Qt, GTK, or Electron. The codebase stays light and self-contained with direct Xlib/XCB calls.
- Breaking or altering cross-platform network protocol structures or serialization formats.

## 3. Constraints
- C++20 standard compliance with standard GCC/Clang on modern Linux distributions (Ubuntu 22.04+, Debian 12+, Fedora, Arch).
- Shared memory MIT-SHM must verify server support and fall back cleanly to `XGetImage` if running over remote X11 or restricted container setups.
- Must not use or include any forbidden terms in code, comments, or documentation.

## 4. Approaches Considered

### Approach 1: Full Native X11 (Host + Viewer GUI) [Selected]
- Screen Capture: X11 MIT-SHM + Xrandr display querying.
- Input Injection: XTest extension (`libXtst`).
- Windowing / GUI: Native X11 window using `XPutImage` / `XShmPutImage` with existing CppDesk software canvas blit.
- Rationale: High performance, direct parity with Windows and macOS implementations, zero third-party GUI framework dependencies.

### Approach 2: Headless Host Daemon (Server / VM Only) [Rejected]
- Excluded because user requires full client capabilities to control remote machines from Linux, not just server mode.

### Approach 3: Wayland-Native Portal + PipeWire [Rejected]
- Excluded because of complex runtime D-Bus dependencies, user-facing permission popups on each connection, and lack of universal compatibility across legacy or lightweight X11 environments.

## 5. Detailed Component Specifications

### 5.1. Build System (`CMakeLists.txt`)
- Add Linux branch (`elseif(UNIX AND NOT APPLE)`) to detect and link:
  - `X11` (`find_package(X11 REQUIRED)`)
  - `Xext` (MIT-SHM shared memory)
  - `Xtst` (XTest input injection)
  - `Xrandr` (multi-monitor querying)
  - `JPEG` (`find_package(JPEG REQUIRED)` for `libjpeg-turbo` tile encode/decode)
  - `zstd`
  - `pthread`
  - `dl`
- Include `src/ui/ui_window_linux.cpp` for `CppDesk` executable on Linux.
- Define `CppDeskTests` executable target for Linux so `ctest` and automated test runners work.

### 5.2. Screen Capture and Codecs (`src/capture/screen_capture.cpp`)
- Implement `#elif defined(__linux__)`:
  - `enumerateMonitors()`: Open default `XOpenDisplay(nullptr)`. Use `XRRGetScreenResourcesCurrent` and `XRRGetOutputInfo` to discover connected outputs, primary status, position (`x`, `y`), and size (`width`, `height`). If Xrandr fails, fall back to default screen dimensions (`DefaultScreenOfDisplay`).
  - `captureViaX11()`:
    - Attempt `XShmCreateImage` and attach shared memory segment (`shmget`, `shmat`, `XShmAttach`).
    - Capture frame via `XShmGetImage` for low-overhead readback. If SHM fails, use `XGetImage`.
    - Extract pixels and map 32-bit BGRA to CppDesk frame buffer.
  - `captureCursorState()`: Use `XQueryPointer` against root window to retrieve relative pointer position and calculate normalized `normX`, `normY`.
  - Tile Codec JPEG Support: Use `libjpeg-turbo` (`jpeg_create_compress`, `jpeg_create_decompress`) on Linux to implement `TileCodec::encodeJpeg` and `TileCodec::decodeJpegIntoCanvas`, maintaining full wire compatibility with Windows and macOS peers.

### 5.3. Input Injection (`src/control/input_injector.cpp`)
- Implement `#elif defined(__linux__)`:
  - `injectMouseMove(normX, normY, monitor)`: Compute absolute root coordinates and call `XTestFakeMotionEvent(dpy, -1, rootX, rootY, CurrentTime)`.
  - `injectMouseButton(button, isDown, normX, normY, monitor)`:
    - Map `MouseButtonId::Left` -> 1, `Middle` -> 2, `Right` -> 3.
    - Move pointer to position, then call `XTestFakeButtonEvent(dpy, xBtn, isDown ? True : False, CurrentTime)`.
  - `injectMouseWheel(verticalDelta, horizontalDelta)`:
    - Map vertical delta positive to Button 4 (wheel up) and negative to Button 5 (wheel down).
    - Map horizontal delta to Button 6 / 7.
    - Fire down and up button events in sequence.
  - `injectKeyEvent(vkCode, scanCode, isDown, isExtended)`:
    - Translate common Windows VK codes to X11 KeySym (e.g. VK_RETURN -> XK_Return, VK_BACK -> XK_BackSpace, A-Z -> XK_a - XK_z).
    - Convert KeySym to KeyCode via `XKeysymToKeycode(dpy, keysym)`.
    - Call `XTestFakeKeyEvent(dpy, keycode, isDown ? True : False, CurrentTime)`.
  - `releaseAllModifiers()`: Send key up for Shift, Control, Alt, and Super keycodes.

### 5.4. Application Entry and Headless Host (`src/main.cpp`)
- Add `--daemon` / `--service-run` CLI argument handling for headless host mode:
  - Runs `NetworkEngine` and host services without attempting to open a viewer window.
  - Defaults `$DISPLAY` to `:0` if unset, enabling systemd service operation.

### 5.5. User Interface Window (`src/ui/ui_window_linux.cpp`)
- Create Linux implementation of `CppDeskWindow` declared in `src/ui/ui_window.hpp`:
  - `create(void* displayContext, int showCommand)`:
    - Connect to X server (`XOpenDisplay`).
    - Create top-level window with 32-bit Visual or standard TrueColor visual.
    - Set window properties, title ("CppDesk"), and WM_DELETE_WINDOW atom.
    - Allocate software backbuffer matching window dimensions.
  - `messageLoop()`:
    - Poll X11 events via `XNextEvent` / `XPending` or `poll()` on X connection fd.
    - Dispatch exposure events (`Expose`), window resizing (`ConfigureNotify`), mouse moves (`MotionNotify`), button presses (`ButtonPress`, `ButtonRelease`), and key events (`KeyPress`, `KeyRelease`).
    - Render remote desktop frame via software rasterizer and blit using `XPutImage` / `XShmPutImage`.
  - `close()`: Terminate message loop and release X resources.

### 5.6. Clipboard Subsystem (`src/control/shell_clipboard.cpp`)
- Maintain existing POSIX stubs for virtual file COM interfaces.
- Add X11 text selection integration for clipboard text synchronization (`CLIPBOARD` atom) using an off-screen helper window to process `SelectionRequest` and `SelectionNotify` events.

## 6. Left to Builder
- Key mapping table specifics between Windows VK codes and X11 KeySyms for international keyboard layouts.
- Specific polling cadence and frame timer resolution (using `nanosleep` or `timerfd`) on Linux.
- Auto-reconnection logic when X server connection drops or display sleep occurs.
