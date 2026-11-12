#pragma once

// Test-only helpers that feed AppKit events to the CppDesk window (implemented in mac_test_driver.mm). The window
// is an opaque NSWindow* so the C++ tests need no Objective-C.

#include <cstdint>

// A left click at (xDip, yDip) from the window's top-left.
void macClick(void* nsWindow, float xDip, float yDip);
// A key press and release. `characters` is what the key types ("" for keys that type nothing, like Delete).
void macType(void* nsWindow, const char* characters, uint16_t macKeyCode);
// The same with the Command key held; `characters` is what the active keyboard layout types for the key
// (a Cyrillic letter on a Russian layout, say).
void macTypeWithCommand(void* nsWindow, const char* characters, uint16_t macKeyCode);
// Posts a Command+key press and release to the application's event queue, the way real keys arrive (the window's
// loop dispatches them; AppKit drops the key-up of a Command shortcut on that path).
void macPostCommandKey(const char* characters, uint16_t macKeyCode);
// Asks the application to terminate the way the Dock's Quit, logout or shutdown do; true when the app delegate
// turned it into an orderly close instead of letting AppKit call exit().
bool macRequestTerminate();
// True when the window's content is drawn: not blank and not one flat colour.
bool macWindowDrewVariedPixels(void* nsWindow);
