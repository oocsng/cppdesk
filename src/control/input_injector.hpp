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

    // Option 5A: Windows UAC & Desktop Elevation helpers
    static bool syncToInputDesktop();
    static bool isElevated();
    static bool relaunchAsAdmin(void* hwndParent = nullptr);

    // Extended Remote System Actions
    static bool executeSystemAction(SystemActionType action);
};

} // namespace cppdesk
