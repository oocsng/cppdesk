#pragma once

#include "../core/protocol.hpp"
#include <cstdint>

namespace cppdesk {

enum class MouseButtonId : uint8_t {
    Left   = 1,
    Right  = 2,
    Middle = 3
};

class InputInjector {
public:
    static void injectMouseMove(float normX, float normY, const MonitorDesc& monitor);
    static void injectMouseButton(MouseButtonId button, bool isDown, float normX, float normY, const MonitorDesc& monitor);
    static void injectMouseWheel(int32_t verticalDelta, int32_t horizontalDelta = 0);
    static void injectKeyEvent(uint16_t vkCode, uint16_t scanCode, bool isDown, bool isExtended);
    static void releaseAllModifiers();
    static uint16_t mapVkToMacKeyCode(uint16_t vk);
    // Inverse of mapVkToMacKeyCode for the Mac UI: generic VK_SHIFT/VK_MENU for modifiers, and both Command and
    // Control are VK_CONTROL, so the screens' Ctrl shortcuts work with Command. 0 if unmapped.
    static uint16_t mapMacKeyCodeToVk(uint16_t macKeyCode);
    static uint32_t mapVkToX11KeySym(uint16_t vk);
    // Inverse of mapVkToX11KeySym (generic VK_SHIFT/VK_CONTROL/VK_MENU for modifiers, as WM_KEYDOWN reports); 0 if unmapped.
    static uint16_t mapX11KeySymToVk(uint32_t keysym);
    // Keys Windows flags as extended (arrows, Home/End, PgUp/PgDn, Insert/Delete, right Ctrl/Alt).
    static bool isExtendedVk(uint16_t vk);

    // Option 5A: Windows UAC & Desktop Elevation helpers
    static bool syncToInputDesktop();
    static bool isElevated();
    static bool relaunchAsAdmin(void* hwndParent = nullptr);

    // Extended Remote System Actions
    static bool executeSystemAction(SystemActionType action);
};

} // namespace cppdesk
