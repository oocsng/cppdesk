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
