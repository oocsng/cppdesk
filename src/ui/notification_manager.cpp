#include "notification_manager.hpp"

#include <vector>

namespace cppdesk {

#if defined(_WIN32)

namespace {

std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &out[0], needed);
    return out;
}

} // namespace

NotificationManager::NotificationManager() = default;

NotificationManager::~NotificationManager() {
    shutdown();
}

bool NotificationManager::init(HWND hwnd, HINSTANCE hInstance, const std::string& appTitle) {
    hwnd_ = hwnd;
    hInstance_ = hInstance;
    appTitle_ = appTitle.empty() ? "CppDesk" : appTitle;

    hTrayIcon_ = LoadIconW(hInstance_, MAKEINTRESOURCEW(101));
    if (!hTrayIcon_) {
        hTrayIcon_ = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = hTrayIcon_;

    std::wstring wTip = utf8ToWide(appTitle_);
    wcsncpy_s(nid.szTip, wTip.c_str(), _TRUNCATE);

    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        trayAdded_ = true;
        return true;
    }
    return false;
}

void NotificationManager::shutdown() {
    stopFlash();
    if (trayAdded_ && hwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
    }
}

void NotificationManager::notify(
    NotificationType type,
    const std::string& title,
    const std::string& message,
    const AppSettings& settings)
{
    lastType_.store(type);

    if (muted_.load()) {
        return;
    }

    // 1. Taskbar Flash (Continuous pulsing until window receives focus)
    if (settings.enableTaskbarFlash) {
        flashTaskbar(true);
    }

    // 2. Notification Sound
    if (settings.enableNotificationSounds) {
        MessageBeep(MB_ICONASTERISK);
    }

    // 3. Windows Action Center Push Toast / Tray Balloon
    if (settings.enablePushNotifications && trayAdded_ && hwnd_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        nid.uFlags = NIF_INFO | NIF_SHOWTIP;

        std::wstring wTitle = utf8ToWide(title.empty() ? appTitle_ : title);
        std::wstring wMsg = utf8ToWide(message);

        wcsncpy_s(nid.szInfoTitle, wTitle.c_str(), _TRUNCATE);
        wcsncpy_s(nid.szInfo, wMsg.c_str(), _TRUNCATE);

        // Windows 10/11 automatically promotes NIF_INFO to native Action Center Toast
        nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
        nid.hBalloonIcon = hTrayIcon_;
        nid.uTimeout = 6000;

        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void NotificationManager::flashTaskbar(bool flash) {
    if (!hwnd_) return;
    FLASHWINFO fi{};
    fi.cbSize = sizeof(fi);
    fi.hwnd = hwnd_;
    fi.dwFlags = flash ? (FLASHW_ALL | FLASHW_TIMERNOFG) : FLASHW_STOP;
    fi.uCount = 0;
    fi.dwTimeout = 0;
    FlashWindowEx(&fi);
    isFlashing_.store(flash);
}

void NotificationManager::stopFlash() {
    if (isFlashing_.load()) {
        flashTaskbar(false);
    }
}

void NotificationManager::showContextMenu(int x, int y, const std::string& statusText) {
    if (!hwnd_) return;
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, L"Open CppDesk");
    SetMenuDefaultItem(hMenu, IDM_TRAY_RESTORE, FALSE);

    std::wstring wStatus = L"Status: " + utf8ToWide(statusText.empty() ? "Online" : statusText);
    AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_TRAY_STATUS, wStatus.c_str());

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING | (muted_.load() ? MF_CHECKED : MF_UNCHECKED), IDM_TRAY_MUTE, L"Mute Notifications");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit CppDesk");

    SetForegroundWindow(hwnd_);
    TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON, x, y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

#else

NotificationManager::NotificationManager() = default;
NotificationManager::~NotificationManager() = default;

bool NotificationManager::init(void* /*hwnd*/, void* /*hInstance*/, const std::string& appTitle) {
    appTitle_ = appTitle.empty() ? "CppDesk" : appTitle;
    return true;
}

void NotificationManager::shutdown() {}

void NotificationManager::notify(
    NotificationType type,
    const std::string& /*title*/,
    const std::string& /*message*/,
    const AppSettings& /*settings*/)
{
    lastType_.store(type);
}

void NotificationManager::flashTaskbar(bool /*flash*/) {}
void NotificationManager::stopFlash() {}
void NotificationManager::showContextMenu(int /*x*/, int /*y*/, const std::string& /*statusText*/) {}

#endif

} // namespace cppdesk
