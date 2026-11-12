#pragma once

// Test-only helpers that feed AppKit events to the CppDesk window (implemented in mac_test_driver.mm). The window
// is an opaque NSWindow* so the C++ tests need no Objective-C.

#include <cstdint>

// A left click at (xDip, yDip) from the window's top-left.
void macClick(void* nsWindow, float xDip, float yDip);
// A key press and release. `characters` is what the key types ("" for keys that type nothing, like Delete).
void macType(void* nsWindow, const char* characters, uint16_t macKeyCode);
// True when the window's content is drawn: not blank and not one flat colour.
bool macWindowDrewVariedPixels(void* nsWindow);
