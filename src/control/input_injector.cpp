#include "input_injector.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#endif
#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>

namespace cppdesk {

uint16_t InputInjector::mapVkToMacKeyCode(uint16_t vk) {
    switch (vk) {
        // Navigation and control
        case 0x0D: return 0x24; // VK_RETURN -> kVK_Return
        case 0x09: return 0x30; // VK_TAB -> kVK_Tab
        case 0x20: return 0x31; // VK_SPACE -> kVK_Space
        case 0x08: return 0x33; // VK_BACK -> kVK_Delete
        case 0x2E: return 0x75; // VK_DELETE -> kVK_ForwardDelete
        case 0x1B: return 0x35; // VK_ESCAPE -> kVK_Escape
        case 0x25: return 0x7B; // VK_LEFT -> kVK_LeftArrow
        case 0x27: return 0x7C; // VK_RIGHT -> kVK_RightArrow
        case 0x26: return 0x7E; // VK_UP -> kVK_UpArrow
        case 0x28: return 0x7D; // VK_DOWN -> kVK_DownArrow
        case 0x24: return 0x73; // VK_HOME -> kVK_Home
        case 0x23: return 0x77; // VK_END -> kVK_End
        case 0x21: return 0x74; // VK_PRIOR -> kVK_PageUp
        case 0x22: return 0x79; // VK_NEXT -> kVK_PageDown

        // Modifiers
        case 0x10: return 0x38; // VK_SHIFT -> kVK_Shift
        case 0xA0: return 0x38; // VK_LSHIFT -> kVK_Shift
        case 0xA1: return 0x3C; // VK_RSHIFT -> kVK_RightShift
        case 0x11: return 0x3B; // VK_CONTROL -> kVK_Control
        case 0xA2: return 0x3B; // VK_LCONTROL -> kVK_Control
        case 0xA3: return 0x3E; // VK_RCONTROL -> kVK_RightControl
        case 0x12: return 0x3A; // VK_MENU -> kVK_Option
        case 0xA4: return 0x3A; // VK_LMENU -> kVK_Option
        case 0xA5: return 0x3D; // VK_RMENU -> kVK_RightOption
        case 0x5B: return 0x37; // VK_LWIN -> kVK_Command
        case 0x5C: return 0x36; // VK_RWIN -> kVK_RightCommand
        case 0x14: return 0x39; // VK_CAPITAL -> kVK_CapsLock

        // Numbers 0-9
        case 0x30: return 0x1D; // '0'
        case 0x31: return 0x12; // '1'
        case 0x32: return 0x13; // '2'
        case 0x33: return 0x14; // '3'
        case 0x34: return 0x15; // '4'
        case 0x35: return 0x17; // '5'
        case 0x36: return 0x16; // '6'
        case 0x37: return 0x1A; // '7'
        case 0x38: return 0x1C; // '8'
        case 0x39: return 0x19; // '9'

        // Letters A-Z
        case 0x41: return 0x00; // 'A'
        case 0x42: return 0x0B; // 'B'
        case 0x43: return 0x08; // 'C'
        case 0x44: return 0x02; // 'D'
        case 0x45: return 0x0E; // 'E'
        case 0x46: return 0x03; // 'F'
        case 0x47: return 0x05; // 'G'
        case 0x48: return 0x04; // 'H'
        case 0x49: return 0x22; // 'I'
        case 0x4A: return 0x26; // 'J'
        case 0x4B: return 0x28; // 'K'
        case 0x4C: return 0x25; // 'L'
        case 0x4D: return 0x2E; // 'M'
        case 0x4E: return 0x2D; // 'N'
        case 0x4F: return 0x1F; // 'O'
        case 0x50: return 0x23; // 'P'
        case 0x51: return 0x0C; // 'Q'
        case 0x52: return 0x0F; // 'R'
        case 0x53: return 0x01; // 'S'
        case 0x54: return 0x11; // 'T'
        case 0x55: return 0x20; // 'U'
        case 0x56: return 0x09; // 'V'
        case 0x57: return 0x0D; // 'W'
        case 0x58: return 0x07; // 'X'
        case 0x59: return 0x10; // 'Y'
        case 0x5A: return 0x06; // 'Z'

        // Function keys F1-F12
        case 0x70: return 0x7A; // F1
        case 0x71: return 0x78; // F2
        case 0x72: return 0x63; // F3
        case 0x73: return 0x76; // F4
        case 0x74: return 0x60; // F5
        case 0x75: return 0x61; // F6
        case 0x76: return 0x62; // F7
        case 0x77: return 0x64; // F8
        case 0x78: return 0x65; // F9
        case 0x79: return 0x6D; // F10
        case 0x7A: return 0x67; // F11
        case 0x7B: return 0x6F; // F12

        default: return 0xFFFF;
    }
}

uint32_t InputInjector::mapVkToX11KeySym(uint16_t vk) {
    switch (vk) {
        // Navigation and control
        case 0x0D: return 0xFF0D; // VK_RETURN -> XK_Return
        case 0x09: return 0xFF09; // VK_TAB -> XK_Tab
        case 0x20: return 0x0020; // VK_SPACE -> XK_space
        case 0x08: return 0xFF08; // VK_BACK -> XK_BackSpace
        case 0x2E: return 0xFFFF; // VK_DELETE -> XK_Delete
        case 0x1B: return 0xFF1B; // VK_ESCAPE -> XK_Escape
        case 0x25: return 0xFF51; // VK_LEFT -> XK_Left
        case 0x27: return 0xFF53; // VK_RIGHT -> XK_Right
        case 0x26: return 0xFF52; // VK_UP -> XK_Up
        case 0x28: return 0xFF54; // VK_DOWN -> XK_Down
        case 0x24: return 0xFF50; // VK_HOME -> XK_Home
        case 0x23: return 0xFF57; // VK_END -> XK_End
        case 0x21: return 0xFF55; // VK_PRIOR -> XK_Page_Up
        case 0x22: return 0xFF56; // VK_NEXT -> XK_Page_Down

        // Modifiers
        case 0x10: return 0xFFE1; // VK_SHIFT -> XK_Shift_L
        case 0xA0: return 0xFFE1; // VK_LSHIFT -> XK_Shift_L
        case 0xA1: return 0xFFE2; // VK_RSHIFT -> XK_Shift_R
        case 0x11: return 0xFFE3; // VK_CONTROL -> XK_Control_L
        case 0xA2: return 0xFFE3; // VK_LCONTROL -> XK_Control_L
        case 0xA3: return 0xFFE4; // VK_RCONTROL -> XK_Control_R
        case 0x12: return 0xFFE9; // VK_MENU -> XK_Alt_L
        case 0xA4: return 0xFFE9; // VK_LMENU -> XK_Alt_L
        case 0xA5: return 0xFFEA; // VK_RMENU -> XK_Alt_R
        case 0x5B: return 0xFFEB; // VK_LWIN -> XK_Super_L
        case 0x5C: return 0xFFEC; // VK_RWIN -> XK_Super_R
        case 0x14: return 0xFFE5; // VK_CAPITAL -> XK_Caps_Lock

        // Numbers 0-9
        case 0x30: return 0x0030; // '0'
        case 0x31: return 0x0031; // '1'
        case 0x32: return 0x0032; // '2'
        case 0x33: return 0x0033; // '3'
        case 0x34: return 0x0034; // '4'
        case 0x35: return 0x0035; // '5'
        case 0x36: return 0x0036; // '6'
        case 0x37: return 0x0037; // '7'
        case 0x38: return 0x0038; // '8'
        case 0x39: return 0x0039; // '9'

        // Letters A-Z -> XK_a - XK_z
        case 0x41: return 0x0061; // 'A' -> 'a'
        case 0x42: return 0x0062; // 'B' -> 'b'
        case 0x43: return 0x0063; // 'C' -> 'c'
        case 0x44: return 0x0064; // 'D' -> 'd'
        case 0x45: return 0x0065; // 'E' -> 'e'
        case 0x46: return 0x0066; // 'F' -> 'f'
        case 0x47: return 0x0067; // 'G' -> 'g'
        case 0x48: return 0x0068; // 'H' -> 'h'
        case 0x49: return 0x0069; // 'I' -> 'i'
        case 0x4A: return 0x006A; // 'J' -> 'j'
        case 0x4B: return 0x006B; // 'K' -> 'k'
        case 0x4C: return 0x006C; // 'L' -> 'l'
        case 0x4D: return 0x006D; // 'M' -> 'm'
        case 0x4E: return 0x006E; // 'N' -> 'n'
        case 0x4F: return 0x006F; // 'O' -> 'o'
        case 0x50: return 0x0070; // 'P' -> 'p'
        case 0x51: return 0x0071; // 'Q' -> 'q'
        case 0x52: return 0x0072; // 'R' -> 'r'
        case 0x53: return 0x0073; // 'S' -> 's'
        case 0x54: return 0x0074; // 'T' -> 't'
        case 0x55: return 0x0075; // 'U' -> 'u'
        case 0x56: return 0x0076; // 'V' -> 'v'
        case 0x57: return 0x0077; // 'W' -> 'w'
        case 0x58: return 0x0078; // 'X' -> 'x'
        case 0x59: return 0x0079; // 'Y' -> 'y'
        case 0x5A: return 0x007A; // 'Z' -> 'z'

        // Function keys F1-F12
        case 0x70: return 0xFFBE; // F1
        case 0x71: return 0xFFBF; // F2
        case 0x72: return 0xFFC0; // F3
        case 0x73: return 0xFFC1; // F4
        case 0x74: return 0xFFC2; // F5
        case 0x75: return 0xFFC3; // F6
        case 0x76: return 0xFFC4; // F7
        case 0x77: return 0xFFC5; // F8
        case 0x78: return 0xFFC6; // F9
        case 0x79: return 0xFFC7; // F10
        case 0x7A: return 0xFFC8; // F11
        case 0x7B: return 0xFFC9; // F12

        // Punctuation & symbols
        case 0xBA: return 0x003B; // VK_OEM_1 -> XK_semicolon
        case 0xBB: return 0x003D; // VK_OEM_PLUS -> XK_equal
        case 0xBC: return 0x002C; // VK_OEM_COMMA -> XK_comma
        case 0xBD: return 0x002D; // VK_OEM_MINUS -> XK_minus
        case 0xBE: return 0x002E; // VK_OEM_PERIOD -> XK_period
        case 0xBF: return 0x002F; // VK_OEM_2 -> XK_slash
        case 0xC0: return 0x0060; // VK_OEM_3 -> XK_grave
        case 0xDB: return 0x005B; // VK_OEM_4 -> XK_bracketleft
        case 0xDC: return 0x005C; // VK_OEM_5 -> XK_backslash
        case 0xDD: return 0x005D; // VK_OEM_6 -> XK_bracketright
        case 0xDE: return 0x0027; // VK_OEM_7 -> XK_apostrophe

        // Numpad 0-9
        case 0x60: return 0xFFB0; // VK_NUMPAD0 -> XK_KP_0
        case 0x61: return 0xFFB1; // VK_NUMPAD1 -> XK_KP_1
        case 0x62: return 0xFFB2; // VK_NUMPAD2 -> XK_KP_2
        case 0x63: return 0xFFB3; // VK_NUMPAD3 -> XK_KP_3
        case 0x64: return 0xFFB4; // VK_NUMPAD4 -> XK_KP_4
        case 0x65: return 0xFFB5; // VK_NUMPAD5 -> XK_KP_5
        case 0x66: return 0xFFB6; // VK_NUMPAD6 -> XK_KP_6
        case 0x67: return 0xFFB7; // VK_NUMPAD7 -> XK_KP_7
        case 0x68: return 0xFFB8; // VK_NUMPAD8 -> XK_KP_8
        case 0x69: return 0xFFB9; // VK_NUMPAD9 -> XK_KP_9
        case 0x6A: return 0xFFAA; // VK_MULTIPLY -> XK_KP_Multiply
        case 0x6B: return 0xFFAB; // VK_ADD -> XK_KP_Add
        case 0x6D: return 0xFFAD; // VK_SUBTRACT -> XK_KP_Subtract
        case 0x6E: return 0xFFAE; // VK_DECIMAL -> XK_KP_Decimal
        case 0x6F: return 0xFFAF; // VK_DIVIDE -> XK_KP_Divide

        default: return 0;
    }
}

uint16_t InputInjector::mapMacKeyCodeToVk(uint16_t macKeyCode) {
    if (macKeyCode == 0xFFFF) return 0;   // the "unmapped" marker of mapVkToMacKeyCode
    switch (macKeyCode) {   // the right-hand modifiers are the generic ones too, as WM_KEYDOWN reports them
        case 0x3C: return 0x10;   // right Shift
        case 0x3E: return 0x11;   // right Control
        case 0x3D: return 0x12;   // right Option
        case 0x37: case 0x36: return 0x11;   // Command acts as Ctrl
        default: break;
    }
    for (uint16_t vk = 1; vk < 255; ++vk) {   // lowest vk wins: generic modifiers before left/right ones
        if (mapVkToMacKeyCode(vk) == macKeyCode) return vk;
    }
    return 0;
}

uint16_t InputInjector::mapX11KeySymToVk(uint32_t keysym) {
    if (keysym >= 'A' && keysym <= 'Z') keysym += 'a' - 'A';   // the table maps letters to lowercase keysyms
    if (keysym == 0) return 0;
    for (uint16_t vk = 1; vk < 255; ++vk) {   // lowest vk wins: generic modifiers before left/right ones
        if (mapVkToX11KeySym(vk) == keysym) return vk;
    }
    return 0;
}

bool InputInjector::isExtendedVk(uint16_t vk) {
    return (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E || vk == 0xA3 || vk == 0xA5;
}

namespace {

#if defined(_WIN32)
void ensureDesktopSynced() {
    static uint64_t lastSyncTick = 0;
    uint64_t now = GetTickCount64();
    if (now - lastSyncTick > 1000) {
        lastSyncTick = now;
        InputInjector::syncToInputDesktop();
    }
}

static std::atomic<uint64_t> s_lastMetricsTick{0};
static std::atomic<int> s_vLeft{0};
static std::atomic<int> s_vTop{0};
static std::atomic<int> s_vWidth{1};
static std::atomic<int> s_vHeight{1};

inline void getCachedVirtualDesktopMetrics(int& outLeft, int& outTop, int& outWidth, int& outHeight) {
    uint64_t now = GetTickCount64();
    uint64_t last = s_lastMetricsTick.load(std::memory_order_relaxed);
    if (last == 0 || (now - last > 1000)) {
        int l = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int t = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int w = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
        int h = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
        s_vLeft.store(l, std::memory_order_relaxed);
        s_vTop.store(t, std::memory_order_relaxed);
        s_vWidth.store(w, std::memory_order_relaxed);
        s_vHeight.store(h, std::memory_order_relaxed);
        s_lastMetricsTick.store(now, std::memory_order_release);
        outLeft = l;
        outTop = t;
        outWidth = w;
        outHeight = h;
    } else {
        outLeft = s_vLeft.load(std::memory_order_relaxed);
        outTop = s_vTop.load(std::memory_order_relaxed);
        outWidth = s_vWidth.load(std::memory_order_relaxed);
        outHeight = s_vHeight.load(std::memory_order_relaxed);
    }
}

void mapNormalizedToVirtualDesktop(float normX, float normY, const MonitorDesc& mon, LONG& outDx, LONG& outDy) {
    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);

    double targetX = mon.x + normX * std::max(1, mon.width - 1);
    double targetY = mon.y + normY * std::max(1, mon.height - 1);

    int vLeft = 0, vTop = 0, vWidth = 1, vHeight = 1;
    getCachedVirtualDesktopMetrics(vLeft, vTop, vWidth, vHeight);

    outDx = static_cast<LONG>(((targetX - vLeft) * 65535.0) / std::max(1, vWidth - 1));
    outDy = static_cast<LONG>(((targetY - vTop) * 65535.0) / std::max(1, vHeight - 1));
}
#endif

} // namespace

#if defined(_WIN32)
void InputInjector::injectMouseMove(float normX, float normY, const MonitorDesc& monitor) {
    ensureDesktopSynced();
    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);

    double targetX = monitor.x + normX * std::max(1, monitor.width - 1);
    double targetY = monitor.y + normY * std::max(1, monitor.height - 1);

    // Hardware cursor positioning (always succeeds even over elevated windows/Task Manager)
    SetCursorPos(static_cast<int>(std::round(targetX)), static_cast<int>(std::round(targetY)));

    LONG dx = 0, dy = 0;
    mapNormalizedToVirtualDesktop(normX, normY, monitor, dx, dy);

    INPUT inp{};
    inp.type = INPUT_MOUSE;
    inp.mi.dx = dx;
    inp.mi.dy = dy;
    inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
        syncToInputDesktop();
        if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
            mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, dx, dy, 0, 0);
        }
    }
}

void InputInjector::injectMouseButton(MouseButtonId button, bool isDown, float normX, float normY, const MonitorDesc& monitor) {
    ensureDesktopSynced();
    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);

    double targetX = monitor.x + normX * std::max(1, monitor.width - 1);
    double targetY = monitor.y + normY * std::max(1, monitor.height - 1);
    SetCursorPos(static_cast<int>(std::round(targetX)), static_cast<int>(std::round(targetY)));

    LONG dx = 0, dy = 0;
    mapNormalizedToVirtualDesktop(normX, normY, monitor, dx, dy);

    DWORD btnFlag = 0;
    switch (button) {
        case MouseButtonId::Left:
            btnFlag = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case MouseButtonId::Right:
            btnFlag = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButtonId::Middle:
            btnFlag = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
    }
    if (btnFlag == 0) return;

    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dx = dx;
    inputs[0].mi.dy = dy;
    inputs[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dx = dx;
    inputs[1].mi.dy = dy;
    inputs[1].mi.dwFlags = btnFlag | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    if (SendInput(2, inputs, sizeof(INPUT)) == 0) {
        syncToInputDesktop();
        if (SendInput(2, inputs, sizeof(INPUT)) == 0) {
            mouse_event(inputs[0].mi.dwFlags, inputs[0].mi.dx, inputs[0].mi.dy, 0, 0);
            mouse_event(inputs[1].mi.dwFlags, inputs[1].mi.dx, inputs[1].mi.dy, 0, 0);
        }
    }
}

void InputInjector::injectMouseWheel(int32_t verticalDelta, int32_t horizontalDelta) {
    ensureDesktopSynced();
    if (verticalDelta != 0) {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.mouseData = static_cast<DWORD>(verticalDelta);
        inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
        if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(verticalDelta), 0);
        }
    }
    if (horizontalDelta != 0) {
        INPUT inp{};
        inp.type = INPUT_MOUSE;
        inp.mi.mouseData = static_cast<DWORD>(horizontalDelta);
        inp.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
            mouse_event(MOUSEEVENTF_HWHEEL, 0, 0, static_cast<DWORD>(horizontalDelta), 0);
        }
    }
}

void InputInjector::injectKeyEvent(uint16_t vkCode, uint16_t scanCode, bool isDown, bool isExtended) {
    ensureDesktopSynced();
    if (scanCode == 0 && vkCode != 0) {
        scanCode = static_cast<uint16_t>(MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC));
    }

    INPUT inp{};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vkCode;
    inp.ki.wScan = scanCode;
    inp.ki.dwFlags = 0;

    if (scanCode != 0) {
        inp.ki.dwFlags |= KEYEVENTF_SCANCODE;
    }
    if (isExtended) {
        inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (!isDown) {
        inp.ki.dwFlags |= KEYEVENTF_KEYUP;
    }

    if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
        syncToInputDesktop();
        if (SendInput(1, &inp, sizeof(INPUT)) == 0) {
            keybd_event(static_cast<BYTE>(vkCode), static_cast<BYTE>(scanCode),
                        (isDown ? 0 : KEYEVENTF_KEYUP) | (isExtended ? KEYEVENTF_EXTENDEDKEY : 0), 0);
        }
    }
}

void InputInjector::releaseAllModifiers() {
    const uint16_t modVks[] = {
        VK_LSHIFT, VK_RSHIFT, VK_SHIFT,
        VK_LCONTROL, VK_RCONTROL, VK_CONTROL,
        VK_LMENU, VK_RMENU, VK_MENU,
        VK_LWIN, VK_RWIN
    };
    for (uint16_t vk : modVks) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            injectKeyEvent(vk, 0, false, (vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN));
        }
    }
}

bool InputInjector::syncToInputDesktop() {
    HDESK hInputDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!hInputDesk) {
        hInputDesk = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    }
    if (!hInputDesk) return false;

    char inputName[256] = {};
    DWORD needed1 = 0;
    GetUserObjectInformationA(hInputDesk, UOI_NAME, inputName, sizeof(inputName), &needed1);

    HDESK hCurDesk = GetThreadDesktop(GetCurrentThreadId());
    char curName[256] = {};
    DWORD needed2 = 0;
    if (hCurDesk) {
        GetUserObjectInformationA(hCurDesk, UOI_NAME, curName, sizeof(curName), &needed2);
    }

    if (hCurDesk && std::strcmp(inputName, curName) == 0) {
        // Desktop has not changed; close newly opened handle immediately to avoid handle leakage
        CloseDesktop(hInputDesk);
        return true;
    }

    static std::mutex s_deskMtx;
    std::lock_guard<std::mutex> lk(s_deskMtx);
    static HDESK s_prevSwitchedDesk = nullptr;
    if (SetThreadDesktop(hInputDesk)) {
        if (s_prevSwitchedDesk) {
            CloseDesktop(s_prevSwitchedDesk);
        }
        s_prevSwitchedDesk = hInputDesk;
        return true;
    }

    CloseDesktop(hInputDesk);
    return false;
}

bool InputInjector::isElevated() {
    bool elevated = false;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation{};
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            elevated = (elevation.TokenIsElevated != 0);
        }
        CloseHandle(hToken);
    }
    return elevated;
}

bool InputInjector::relaunchAsAdmin(void* hwndParent) {
    wchar_t szPath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, szPath, MAX_PATH) == 0) {
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = szPath;
    sei.hwnd = reinterpret_cast<HWND>(hwndParent);
    sei.nShow = SW_NORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

bool InputInjector::executeSystemAction(SystemActionType action) {
    switch (action) {
        case SystemActionType::TaskManager:
            ShellExecuteA(nullptr, "open", "taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
            return true;

        case SystemActionType::ShowDesktop:
            injectKeyEvent(VK_LWIN, 0, true, false);
            injectKeyEvent('D', 0, true, false);
            injectKeyEvent('D', 0, false, false);
            injectKeyEvent(VK_LWIN, 0, false, false);
            return true;

        case SystemActionType::LockWorkstation:
            return LockWorkStation() != FALSE;

        case SystemActionType::SendCtrlAltDel: {
            typedef VOID(WINAPI* SendSASFunc)(BOOL AsUser);
            HMODULE hSas = LoadLibraryA("sas.dll");
            bool sasSucceeded = false;
            if (hSas) {
                auto pFunc = GetProcAddress(hSas, "SendSAS");
                if (pFunc) {
                    auto pSendSAS = reinterpret_cast<SendSASFunc>(reinterpret_cast<void(*)()>(pFunc));
                    pSendSAS(FALSE);
                    sasSucceeded = true;
                }
                FreeLibrary(hSas);
            }
            if (!sasSucceeded) {
                // Fallback: launch Task Manager
                ShellExecuteA(nullptr, "open", "taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
            }
            return true;
        }

        case SystemActionType::EmergencyReboot: {
            HANDLE hToken = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
                TOKEN_PRIVILEGES tkp{};
                LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &tkp.Privileges[0].Luid);
                tkp.PrivilegeCount = 1;
                tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
                CloseHandle(hToken);
            }
            return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED) != FALSE;
        }

        default:
            return false;
    }
}

#elif defined(__APPLE__)

void InputInjector::injectMouseMove(float normX, float normY, const MonitorDesc& monitor) {
    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);
    CGPoint pt = CGPointMake(
        monitor.x + normX * std::max(1, monitor.width - 1),
        monitor.y + normY * std::max(1, monitor.height - 1)
    );
    CGEventRef ev = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, pt, kCGMouseButtonLeft);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void InputInjector::injectMouseButton(MouseButtonId button, bool isDown, float normX, float normY, const MonitorDesc& monitor) {
    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);
    CGPoint pt = CGPointMake(
        monitor.x + normX * std::max(1, monitor.width - 1),
        monitor.y + normY * std::max(1, monitor.height - 1)
    );
    CGEventType evType;
    CGMouseButton cgBtn = kCGMouseButtonLeft;
    if (button == MouseButtonId::Left) {
        evType = isDown ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        cgBtn = kCGMouseButtonLeft;
    } else if (button == MouseButtonId::Right) {
        evType = isDown ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        cgBtn = kCGMouseButtonRight;
    } else {
        evType = isDown ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        cgBtn = kCGMouseButtonCenter;
    }
    CGEventRef ev = CGEventCreateMouseEvent(nullptr, evType, pt, cgBtn);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void InputInjector::injectMouseWheel(int32_t verticalDelta, int32_t horizontalDelta) {
    int32_t vUnits = verticalDelta / 120;
    int32_t hUnits = horizontalDelta / 120;
    if (vUnits == 0 && verticalDelta != 0) vUnits = verticalDelta > 0 ? 1 : -1;
    if (hUnits == 0 && horizontalDelta != 0) hUnits = horizontalDelta > 0 ? 1 : -1;
    CGEventRef ev = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 2, vUnits, hUnits);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void InputInjector::injectKeyEvent(uint16_t vkCode, uint16_t scanCode, bool isDown, bool isExtended) {
    (void)scanCode;
    (void)isExtended;
    uint16_t macKey = mapVkToMacKeyCode(vkCode);
    if (macKey != 0xFFFF) {
        CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(macKey), isDown);
        if (ev) {
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }
}

void InputInjector::releaseAllModifiers() {
    CGKeyCode mods[] = { 0x38, 0x3B, 0x3A, 0x37 }; // Shift, Control, Option, Command
    for (CGKeyCode m : mods) {
        CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, m, false);
        if (ev) {
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
    }
}

bool InputInjector::syncToInputDesktop() {
    return true;
}

bool InputInjector::isElevated() {
    return false;
}

bool InputInjector::relaunchAsAdmin(void*) {
    return false;
}

bool InputInjector::executeSystemAction(SystemActionType) {
    return false;
}

#elif defined(__linux__) || defined(__unix__)

#if __has_include(<X11/Xlib.h>)
#include "../capture/xlib_threads.hpp"
#include <X11/Xlib.h>
#endif
#if __has_include(<X11/extensions/XTest.h>)
#include <X11/extensions/XTest.h>
#endif

namespace {
#if __has_include(<X11/Xlib.h>)
static std::mutex s_x11InputMutex;
Display* getCachedX11Display() {
    static Display* s_dpy = nullptr;
    static bool s_inited = false;
    std::lock_guard<std::mutex> lk(s_x11InputMutex);
    if (!s_inited) {
        ensureXlibThreads();
        s_inited = true;
    }
    if (!s_dpy) {
        s_dpy = XOpenDisplay(nullptr);
    }
    return s_dpy;
}
#endif
} // namespace

void InputInjector::injectMouseMove(float normX, float normY, const MonitorDesc& monitor) {
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/extensions/XTest.h>)
    Display* dpy = getCachedX11Display();
    if (!dpy) return;

    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);
    int rootX = monitor.x + static_cast<int>(normX * std::max(1, monitor.width - 1));
    int rootY = monitor.y + static_cast<int>(normY * std::max(1, monitor.height - 1));

    XTestFakeMotionEvent(dpy, -1, rootX, rootY, CurrentTime);
    XFlush(dpy);
#else
    (void)normX; (void)normY; (void)monitor;
#endif
}

void InputInjector::injectMouseButton(MouseButtonId button, bool isDown, float normX, float normY, const MonitorDesc& monitor) {
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/extensions/XTest.h>)
    Display* dpy = getCachedX11Display();
    if (!dpy) return;

    normX = std::clamp(normX, 0.0f, 1.0f);
    normY = std::clamp(normY, 0.0f, 1.0f);
    int rootX = monitor.x + static_cast<int>(normX * std::max(1, monitor.width - 1));
    int rootY = monitor.y + static_cast<int>(normY * std::max(1, monitor.height - 1));

    XTestFakeMotionEvent(dpy, -1, rootX, rootY, CurrentTime);

    unsigned int xBtn = 1;
    if (button == MouseButtonId::Right) xBtn = 3;
    else if (button == MouseButtonId::Middle) xBtn = 2;

    XTestFakeButtonEvent(dpy, xBtn, isDown ? True : False, CurrentTime);
    XFlush(dpy);
#else
    (void)button; (void)isDown; (void)normX; (void)normY; (void)monitor;
#endif
}

void InputInjector::injectMouseWheel(int32_t verticalDelta, int32_t horizontalDelta) {
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/extensions/XTest.h>)
    Display* dpy = getCachedX11Display();
    if (!dpy) return;

    if (verticalDelta > 0) {
        XTestFakeButtonEvent(dpy, 4, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 4, False, CurrentTime);
    } else if (verticalDelta < 0) {
        XTestFakeButtonEvent(dpy, 5, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 5, False, CurrentTime);
    }

    if (horizontalDelta > 0) {
        XTestFakeButtonEvent(dpy, 7, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 7, False, CurrentTime);
    } else if (horizontalDelta < 0) {
        XTestFakeButtonEvent(dpy, 6, True, CurrentTime);
        XTestFakeButtonEvent(dpy, 6, False, CurrentTime);
    }
    XFlush(dpy);
#else
    (void)verticalDelta; (void)horizontalDelta;
#endif
}

void InputInjector::injectKeyEvent(uint16_t vkCode, uint16_t scanCode, bool isDown, bool isExtended) {
    (void)scanCode;
    (void)isExtended;
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/extensions/XTest.h>)
    Display* dpy = getCachedX11Display();
    if (!dpy) return;

    uint32_t sym = mapVkToX11KeySym(vkCode);
    if (sym != 0) {
        KeyCode kc = XKeysymToKeycode(dpy, static_cast<KeySym>(sym));
        if (kc != 0) {
            XTestFakeKeyEvent(dpy, kc, isDown ? True : False, CurrentTime);
            XFlush(dpy);
        }
    }
#else
    (void)vkCode; (void)isDown;
#endif
}

void InputInjector::releaseAllModifiers() {
#if __has_include(<X11/Xlib.h>) && __has_include(<X11/extensions/XTest.h>)
    Display* dpy = getCachedX11Display();
    if (!dpy) return;

    uint32_t mods[] = { 0xFFE1, 0xFFE2, 0xFFE3, 0xFFE4, 0xFFE9, 0xFFEA, 0xFFEB, 0xFFEC };
    for (uint32_t sym : mods) {
        KeyCode kc = XKeysymToKeycode(dpy, static_cast<KeySym>(sym));
        if (kc != 0) {
            XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
        }
    }
    XFlush(dpy);
#endif
}

bool InputInjector::syncToInputDesktop() {
    return true;
}

bool InputInjector::isElevated() {
    return false;
}

bool InputInjector::relaunchAsAdmin(void*) {
    return false;
}

bool InputInjector::executeSystemAction(SystemActionType) {
    return false;
}

#else

void InputInjector::injectMouseMove(float, float, const MonitorDesc&) {}
void InputInjector::injectMouseButton(MouseButtonId, bool, float, float, const MonitorDesc&) {}
void InputInjector::injectMouseWheel(int32_t, int32_t) {}
void InputInjector::injectKeyEvent(uint16_t, uint16_t, bool, bool) {}
void InputInjector::releaseAllModifiers() {}
bool InputInjector::syncToInputDesktop() { return true; }
bool InputInjector::isElevated() { return false; }
bool InputInjector::relaunchAsAdmin(void*) { return false; }
bool InputInjector::executeSystemAction(SystemActionType) { return false; }

#endif

} // namespace cppdesk
