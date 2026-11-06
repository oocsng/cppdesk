# Linux Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add native X11 Linux remote desktop support to CppDesk with host screen capture, input injection, session viewer window, and CMake build targets.

**Architecture:** Native X11 integration using MIT-SHM (`XShmGetImage`) and Xrandr for multi-monitor frame capture, XTest for synthetic mouse and keyboard event injection, `libjpeg-turbo` for cross-platform tile codec interoperability, and an X11 session viewer canvas window matching the lightweight macOS architecture.

**Tech Stack:** C++20, CMake 3.20+, X11 (libX11, libXext, libXtst, libXrandr), libjpeg-turbo, zstd, POSIX pthreads.

**Spec:** docs/superpowers/specs/2026-11-07-linux-support-design.md

## Global Constraints
- Target platform: Modern Linux distributions (Ubuntu 22.04+, Debian 12+, Fedora, Arch) with X11 / XWayland.
- Standard: C++20 standard compliance with GCC 11+ or Clang 14+.
- External libraries: Only standard X11 libraries (`X11`, `Xext`, `Xtst`, `Xrandr`), `JPEG`, `zstd`, and `pthread`. No heavyweight GUI toolkits (no Qt, GTK, or Electron).
- Compatibility: Wire protocol and tile encoding formats must remain 100% interoperable with Windows and macOS peers.
- Naming rules: Strictly avoid forbidden keywords across all code, tests, and documentation.

## Review Focus
- X11 display connection missing: `XOpenDisplay(nullptr)` returns null when `$DISPLAY` is unset; code must handle null display gracefully without segfaulting.
- MIT-SHM extension failure: `XShmAttach` or remote X11 forwarding fails; code must fall back cleanly to standard `XGetImage`.
- Display server disconnection during runtime: I/O errors or X server restarts must not crash background worker threads.
- Multi-monitor coordinate boundaries: normalized coordinates `(normX, normY)` must map accurately across negative or offset multi-monitor Xrandr geometries.
- JPEG buffer allocation: libjpeg decompression must validate scanline pitch and dimensions before writing into BGRA canvas buffers.

---

### Task 1: CMake Linux Build Configuration and Target Setup

**Files:**
- Modify: `CMakeLists.txt:105-155`
- Test: `tests/test_suite.cpp`

**Interfaces:**
- Consumes: Existing `CORE_SOURCES` list in `CMakeLists.txt`.
- Produces: `CppDesk` executable target on Linux and `CppDeskTests` executable target on Linux.

- [ ] **Step 1: Write test validating Linux build definitions**

Add preprocessor and configuration test in `tests/test_suite.cpp`:
```cpp
void testLinuxPlatformDefinitions() {
#if defined(__linux__)
    assert(true);
#endif
}
```

- [ ] **Step 2: Run test build to inspect missing Linux configuration**

Run: `cmake -B build -S .`
Expected: CMake succeeds and includes Linux platform libraries.

- [ ] **Step 3: Update `CMakeLists.txt` for Linux dependencies and executable targets**

In `CMakeLists.txt`:
```cmake
elseif(UNIX AND NOT APPLE)
    list(APPEND CORE_SOURCES
        src/simd/simd_fallback.cpp
    )

    find_package(X11 REQUIRED)
    find_package(JPEG REQUIRED)
    find_library(LIB_ZSTD zstd REQUIRED)
    find_library(LIB_XTST Xtst REQUIRED)
    find_library(LIB_XRANDR Xrandr REQUIRED)
    find_library(LIB_XEXT Xext REQUIRED)

    set(PLATFORM_LIBS
        ${X11_LIBRARIES}
        ${LIB_XEXT}
        ${LIB_XTST}
        ${LIB_XRANDR}
        ${JPEG_LIBRARIES}
        ${LIB_ZSTD}
        pthread
        dl
    )

    add_executable(CppDeskTests
        ${CORE_SOURCES}
        tests/test_suite.cpp
    )
    target_link_libraries(CppDeskTests PRIVATE ${PLATFORM_LIBS})

    add_executable(CppDesk
        ${CORE_SOURCES}
        src/ui/ui_window_linux.cpp
        src/main.cpp
    )
    target_link_libraries(CppDesk PRIVATE ${PLATFORM_LIBS})
```

- [ ] **Step 4: Verify CMake target generation**

Run: `cmake -B build -S .`
Expected: `CppDesk` and `CppDeskTests` targets generated for Unix/Linux.

- [ ] **Step 5: Commit changes**

```bash
git add CMakeLists.txt tests/test_suite.cpp
git commit -m "build: configure Linux X11 targets and test runner in CMake"
```

---

### Task 2: Linux Screen Capture and JPEG Tile Codec

**Files:**
- Modify: `src/capture/screen_capture.hpp:1-35`
- Modify: `src/capture/screen_capture.cpp:665-720`, `1100-1160`, `1200-1230`, `1255-1275`
- Test: `tests/test_suite.cpp`

**Interfaces:**
- Consumes: X11 display handle, Xrandr output query, `libjpeg-turbo` compress/decompress APIs.
- Produces: `ScreenCapturer::enumerateMonitors()`, `ScreenCapturer::captureViaX11()`, `TileCodec::encodeJpeg()`, `TileCodec::decodeJpegIntoCanvas()`.

- [ ] **Step 1: Write unit tests for X11 screen capture and JPEG codec**

In `tests/test_suite.cpp`:
```cpp
void testScreenCaptureLinuxMonitorEnumeration();
void testTileCodecJpegRoundtrip();
```

- [ ] **Step 2: Run test suite to verify stubs**

Run: `rtk ctest` or compile test suite.
Expected: Verification runs against current non-Windows fallback.

- [ ] **Step 3: Implement Linux X11 screen capture and libjpeg tile codec**

1. Include `<X11/Xlib.h>`, `<X11/extensions/XShm.h>`, `<X11/extensions/Xrandr.h>`, `<sys/shm.h>`, `<jpeglib.h>` under `#elif defined(__linux__)`.
2. In `enumerateMonitors()`:
   - Query `XRRGetScreenResourcesCurrent` and `XRRGetOutputInfo` to record each active display's `x`, `y`, `width`, `height`, and primary status.
   - Fall back to `DisplayWidth` and `DisplayHeight` of `DefaultScreenOfDisplay` if Xrandr is unavailable.
3. In `captureViaX11()`:
   - Initialize MIT-SHM segment with `XShmCreateImage` and `XShmAttach`.
   - Call `XShmGetImage` to read pixels into memory without server-client copy. Fall back to `XGetImage` on failure.
   - Convert 32-bit pixel data into `currentFrame_` BGRA buffer.
4. In `captureCursorState()`:
   - Call `XQueryPointer` against root window and normalize coordinates relative to the active monitor.
5. In `TileCodec::encodeJpeg()` and `TileCodec::decodeJpegIntoCanvas()`:
   - Implement `jpeg_create_compress` / `jpeg_create_decompress` pathways using `libjpeg-turbo` on Linux to match Windows GDI+ codec output.

- [ ] **Step 4: Run test suite to verify capture and codec functions**

Run: `rtk ctest --output-on-failure`
Expected: Monitor enumeration, cursor capture, and JPEG tile encode/decode tests pass.

- [ ] **Step 5: Commit changes**

```bash
git add src/capture/screen_capture.hpp src/capture/screen_capture.cpp tests/test_suite.cpp
git commit -m "feat: implement Linux X11 screen capture and libjpeg tile codec"
```

---

### Task 3: Linux XTest Input Injection

**Files:**
- Modify: `src/control/input_injector.hpp:1-32`
- Modify: `src/control/input_injector.cpp:520-541`
- Test: `tests/test_suite.cpp`

**Interfaces:**
- Consumes: `MonitorDesc`, `MouseButtonId`, `SystemActionType`, virtual key codes (`vkCode`).
- Produces: `InputInjector::injectMouseMove`, `InputInjector::injectMouseButton`, `InputInjector::injectMouseWheel`, `InputInjector::injectKeyEvent`, `InputInjector::releaseAllModifiers`.

- [ ] **Step 1: Write unit tests for Linux key translation and coordinate mapping**

In `tests/test_suite.cpp`:
```cpp
void testLinuxKeyMapping();
void testLinuxMouseCoordinateMapping();
```

- [ ] **Step 2: Run test suite to check baseline behavior**

Run: `rtk ctest`
Expected: Stub tests fail or return expected default values.

- [ ] **Step 3: Implement XTest input injector on Linux**

1. Include `<X11/Xlib.h>`, `<X11/extensions/XTest.h>`, `<X11/keysym.h>` under `#elif defined(__linux__)`.
2. Manage a persistent thread-safe or cached `Display*` connection for input injection.
3. Implement `injectMouseMove`:
   - Calculate absolute root coordinate from monitor offset and normalized coordinates.
   - Call `XTestFakeMotionEvent(dpy, -1, rootX, rootY, CurrentTime)`.
   - Flush with `XFlush(dpy)`.
4. Implement `injectMouseButton`:
   - Map `MouseButtonId::Left` -> 1, `Middle` -> 2, `Right` -> 3.
   - Position cursor and fire `XTestFakeButtonEvent(dpy, btn, isDown ? True : False, CurrentTime)`.
5. Implement `injectMouseWheel`:
   - Send sequential down and up button events using Button 4/5 for vertical and Button 6/7 for horizontal scroll.
6. Implement `injectKeyEvent`:
   - Implement `mapVkToX11KeySym(uint16_t vk)` translating Windows VK codes to X11 KeySyms.
   - Resolve `KeyCode` via `XKeysymToKeycode(dpy, keysym)`.
   - Fire `XTestFakeKeyEvent(dpy, keycode, isDown ? True : False, CurrentTime)`.
7. Implement `releaseAllModifiers`:
   - Release Shift, Control, Alt, and Super keys via `XTestFakeKeyEvent(..., False, ...)`.

- [ ] **Step 4: Verify input injector tests**

Run: `rtk ctest --output-on-failure`
Expected: Coordinate normalization and VK-to-KeySym mapping tests pass.

- [ ] **Step 5: Commit changes**

```bash
git add src/control/input_injector.hpp src/control/input_injector.cpp tests/test_suite.cpp
git commit -m "feat: implement Linux XTest synthetic input injection"
```

---

### Task 4: Headless Host Daemon and CLI Service Model

**Files:**
- Modify: `src/main.cpp:170-262`
- Test: `tests/test_suite.cpp`

**Interfaces:**
- Consumes: Command line arguments (`argc`, `argv`), POSIX signals, `NetworkEngine`, `IdentityManager`.
- Produces: CLI execution modes `--daemon`, `--service-run`, `--connect`, `--relay-server`.

- [ ] **Step 1: Write CLI argument parsing test for headless host flag**

In `tests/test_suite.cpp`:
```cpp
void testLinuxCommandLineParsing();
```

- [ ] **Step 2: Run test suite to verify CLI baseline**

Run: `rtk ctest`
Expected: Current argument parser does not recognize `--daemon` or `--service-run`.

- [ ] **Step 3: Implement headless host execution pathway in `src/main.cpp`**

In `src/main.cpp`:
1. Parse `--daemon` and `--service-run` flags to set `headlessHostOnly = true`.
2. Check `getenv("DISPLAY")`; if unset or empty, set default `setenv("DISPLAY", ":0", 0)`.
3. If running headless host mode:
   - Start `IdentityManager` and load or generate desk ID.
   - Start `NetworkEngine` listener.
   - Loop on POSIX signals (`SIGINT`, `SIGTERM`, `SIGQUIT`) without instantiating `CppDeskWindow`.
   - Clean up lockfile and stop network cleanly on termination.

- [ ] **Step 4: Verify daemon CLI parsing tests**

Run: `rtk ctest --output-on-failure`
Expected: CLI argument parsing tests pass.

- [ ] **Step 5: Commit changes**

```bash
git add src/main.cpp tests/test_suite.cpp
git commit -m "feat: add Linux headless host daemon mode and display fallback"
```

---

### Task 5: Linux X11 Session Viewer Window

**Files:**
- Create: `src/ui/ui_window_linux.cpp`
- Modify: `src/ui/ui_window.hpp:615-641`
- Test: `tests/test_suite.cpp`

**Interfaces:**
- Consumes: `IdentityManager`, `NetworkEngine`, `autoConnectTarget`, `UiPlatform::calculateViewportGeometry`.
- Produces: `CppDeskWindow` lifecycle methods (`create`, `messageLoop`, `close`).

- [ ] **Step 1: Write unit test for Linux window lifecycle interfaces**

In `tests/test_suite.cpp`:
```cpp
void testLinuxWindowInterfaceInstantiation();
```

- [ ] **Step 2: Run test to verify interface compatibility**

Run: `rtk ctest`
Expected: Header compilation checks pass.

- [ ] **Step 3: Implement `CppDeskWindow` in `src/ui/ui_window_linux.cpp`**

1. Create `CppDeskWindowLinuxImpl` struct holding:
   - `Display* display = nullptr`
   - `Window window = 0`
   - `GC gc = nullptr`
   - `Atom wmDeleteWindow = 0`
   - Software frame canvas buffer and viewport geometry cache.
2. In `create()`:
   - Connect to X server (`XOpenDisplay`).
   - Create top-level window with 32-bit/TrueColor visual.
   - Configure window title, minimum size (1024x640), and WM protocols.
   - Initialize remote connection session if `autoConnectTarget` is supplied.
3. In `messageLoop()`:
   - Poll X events with `XPending` / `XNextEvent`.
   - On `Expose`: blit remote desktop canvas to window using `XPutImage` or `XShmPutImage`.
   - On `ConfigureNotify`: resize window backbuffer and update viewport scale via `UiPlatform::calculateViewportGeometry`.
   - On `MotionNotify`, `ButtonPress`, `ButtonRelease`: compute normalized frame coordinates and forward remote input to peer via `NetworkEngine::sendInputEvent`.
   - On `KeyPress`, `KeyRelease`: translate KeySym to VK code and forward to remote peer.
   - On `ClientMessage` with `wmDeleteWindow`: terminate loop.
4. In `close()`:
   - Destroy window, free GC, and close display connection.

- [ ] **Step 4: Verify window creation and mock event loop**

Run: `rtk ctest --output-on-failure`
Expected: Viewer interface and viewport geometry integration tests pass.

- [ ] **Step 5: Commit changes**

```bash
git add src/ui/ui_window_linux.cpp src/ui/ui_window.hpp tests/test_suite.cpp
git commit -m "feat: implement native X11 remote desktop viewer window"
```

---

### Task 6: Linux Clipboard Text Synchronization

**Files:**
- Modify: `src/control/shell_clipboard.hpp:135-155`
- Modify: `src/control/shell_clipboard.cpp`
- Test: `tests/test_suite.cpp`

**Interfaces:**
- Consumes: X11 selection events (`SelectionRequest`, `SelectionNotify`, `SelectionClear`).
- Produces: `ShellClipboard::getLocalClipboardText()`, `ShellClipboard::setLocalClipboardText()`.

- [ ] **Step 1: Write unit test for clipboard text getter/setter**

In `tests/test_suite.cpp`:
```cpp
void testLinuxClipboardTextSync();
```

- [ ] **Step 2: Run test suite to verify baseline**

Run: `rtk ctest`
Expected: Stub returns empty string or false.

- [ ] **Step 3: Implement X11 clipboard integration**

1. Create an off-screen 1x1 unmapped helper window on the active display.
2. Intercept and own `CLIPBOARD` atom selection.
3. On local copy: store UTF-8 string and assert ownership of `CLIPBOARD`.
4. On `SelectionRequest`: reply with `UTF8_STRING` target data to requester.
5. On remote text paste: fetch `CLIPBOARD` text from external X11 owner and forward to network peer.

- [ ] **Step 4: Verify clipboard text synchronization tests**

Run: `rtk ctest --output-on-failure`
Expected: Clipboard get/set roundtrip tests pass.

- [ ] **Step 5: Commit changes**

```bash
git add src/control/shell_clipboard.hpp src/control/shell_clipboard.cpp tests/test_suite.cpp
git commit -m "feat: implement X11 clipboard text synchronization"
```

---

### Task 7: Full System Verification and Regression Testing

**Files:**
- Modify: `tests/test_suite.cpp`
- Test: All automated test suites

**Interfaces:**
- Consumes: All updated components across CMake, screen capture, input injector, viewer window, and clipboard.
- Produces: Complete passing test suite.

- [ ] **Step 1: Add end-to-end integration test suite for Linux**

In `tests/test_suite.cpp`:
- Add full end-to-end simulation: monitor enumeration -> frame capture -> tile encoding -> network frame send -> decode -> viewer canvas blit -> input injection loop.

- [ ] **Step 2: Run test suite across all targets**

Run: `rtk ctest --verbose`
Expected: All tests pass with zero regressions.

- [ ] **Step 3: Verify clean build without warnings**

Run: `cmake --build build --config Release`
Expected: 100% build success with zero errors.

- [ ] **Step 4: Commit test suite updates**

```bash
git add tests/test_suite.cpp
git commit -m "test: add comprehensive Linux X11 integration test suite"
```
