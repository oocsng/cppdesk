#include "screen_blank_manager.hpp"

#include <algorithm>
#include <vector>
#include <chrono>

#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif
#ifndef LLKHF_INJECTED
#define LLKHF_INJECTED 0x00000010
#endif

namespace cppdesk {

struct ScreenBlankCurtainConfig {
    std::wstring brandName;
    std::wstring noticeText;
    std::wstring deskIdText;
    bool showDeskId = true;
};

static ScreenBlankCurtainConfig s_curtainConfig;

static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (req <= 1) return L"";
    std::wstring out(req - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), req);
    return out;
}

static LRESULT CALLBACK BlankCurtainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = std::max<int>(1, rc.right - rc.left);
            int h = std::max<int>(1, rc.bottom - rc.top);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // 1. OLED Deep Dark Background (#0B0E14)
            HBRUSH bgBrush = CreateSolidBrush(RGB(11, 14, 20));
            FillRect(memDC, &rc, bgBrush);
            DeleteObject(bgBrush);

            // 2. Central Frosted Security Card Container
            int cardW = std::clamp(w - 60, 360, 680);
            int cardH = std::clamp(h - 60, 300, 420);
            int cardX = (w - cardW) / 2;
            int cardY = (h - cardH) / 2;
            RECT cardRc = { cardX, cardY, cardX + cardW, cardY + cardH };

            HBRUSH cardBrush = CreateSolidBrush(RGB(20, 24, 34));
            HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(36, 45, 61));
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, cardBrush);
            HPEN oldPen = (HPEN)SelectObject(memDC, cardPen);
            RoundRect(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom, 22, 22);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(cardBrush);
            DeleteObject(cardPen);

            // 3. Central Shield Polygon Icon (Emerald Accent #10B981)
            int shieldCX = cardX + cardW / 2;
            int shieldCY = cardY + 58;
            POINT shieldPts[5] = {
                { shieldCX, shieldCY - 26 },
                { shieldCX + 24, shieldCY - 14 },
                { shieldCX + 16, shieldCY + 18 },
                { shieldCX, shieldCY + 28 },
                { shieldCX - 16, shieldCY + 18 }
            };
            HBRUSH shieldBrush = CreateSolidBrush(RGB(16, 185, 129));
            HPEN shieldPen = CreatePen(PS_SOLID, 2, RGB(5, 150, 105));
            oldBrush = (HBRUSH)SelectObject(memDC, shieldBrush);
            oldPen = (HPEN)SelectObject(memDC, shieldPen);
            Polygon(memDC, shieldPts, 5);

            // Inner lock core
            HBRUSH innerBrush = CreateSolidBrush(RGB(20, 24, 34));
            SelectObject(memDC, innerBrush);
            Ellipse(memDC, shieldCX - 5, shieldCY - 4, shieldCX + 5, shieldCY + 6);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(shieldBrush);
            DeleteObject(shieldPen);
            DeleteObject(innerBrush);

            // 4. Branded Typography & Badges
            SetBkMode(memDC, TRANSPARENT);

            // Brand Title (Segoe UI Bold 24pt, #F8FAFC)
            HFONT hFontBrand = CreateFontW(25, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(memDC, hFontBrand);
            SetTextColor(memDC, RGB(248, 250, 252));
            RECT brandRc = { cardX + 24, shieldCY + 36, cardX + cardW - 24, shieldCY + 70 };
            DrawTextW(memDC, s_curtainConfig.brandName.c_str(), -1, &brandRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Status Pill: [ SECURED PRIVACY CURTAIN ACTIVE ]
            HFONT hFontPill = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(memDC, hFontPill);
            SetTextColor(memDC, RGB(16, 185, 129));
            RECT pillRc = { cardX + 24, brandRc.bottom + 2, cardX + cardW - 24, brandRc.bottom + 20 };
            DrawTextW(memDC, L"[ SECURED PRIVACY CURTAIN ACTIVE ]", -1, &pillRc, DT_CENTER | DT_SINGLELINE);

            // Custom Security Notice (#CBD5E1, Segoe UI 15pt)
            HFONT hFontNotice = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(memDC, hFontNotice);
            SetTextColor(memDC, RGB(203, 213, 225));
            RECT noticeRc = { cardX + 32, pillRc.bottom + 12, cardX + cardW - 32, pillRc.bottom + 76 };
            DrawTextW(memDC, s_curtainConfig.noticeText.c_str(), -1, &noticeRc, DT_CENTER | DT_WORDBREAK);

            // Desk ID Pill Badge
            HFONT hFontId = nullptr;
            if (s_curtainConfig.showDeskId && !s_curtainConfig.deskIdText.empty()) {
                RECT idBox = { cardX + cardW / 2 - 130, noticeRc.bottom + 8, cardX + cardW / 2 + 130, noticeRc.bottom + 34 };
                HBRUSH idBrush = CreateSolidBrush(RGB(28, 33, 46));
                HPEN idPen = CreatePen(PS_SOLID, 1, RGB(45, 55, 75));
                HBRUSH prevBrush = (HBRUSH)SelectObject(memDC, idBrush);
                HPEN prevPen = (HPEN)SelectObject(memDC, idPen);
                RoundRect(memDC, idBox.left, idBox.top, idBox.right, idBox.bottom, 8, 8);
                SelectObject(memDC, prevBrush);
                SelectObject(memDC, prevPen);
                DeleteObject(idBrush);
                DeleteObject(idPen);

                hFontId = CreateFontW(14, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
                SelectObject(memDC, hFontId);
                SetTextColor(memDC, RGB(148, 163, 184));
                DrawTextW(memDC, s_curtainConfig.deskIdText.c_str(), -1, &idBox, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // Host Physical Abort Failsafe
            HFONT hFontFailsafe = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(memDC, hFontFailsafe);
            SetTextColor(memDC, RGB(100, 116, 139));
            RECT hintRc = { cardX + 24, cardRc.bottom - 36, cardX + cardW - 24, cardRc.bottom - 12 };
            DrawTextW(memDC, L"Local emergency failsafe: Press any host physical key or move mouse to unlock workstation.", -1, &hintRc, DT_CENTER | DT_SINGLELINE);

            // Transfer backbuffer to screen
            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

            // Cleanup GDI objects
            SelectObject(memDC, hOldFont);
            DeleteObject(hFontBrand);
            DeleteObject(hFontPill);
            DeleteObject(hFontNotice);
            if (hFontId) DeleteObject(hFontId);
            DeleteObject(hFontFailsafe);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(nullptr);
            return TRUE;
        case WM_CLOSE:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static LRESULT CALLBACK BlankLowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam != 0) {
        const MSLLHOOKSTRUCT* hs = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (ScreenBlankManager::isPhysicalMouseInput(*hs)) {
            ScreenBlankManager::instance().triggerEmergencyWake();
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK BlankLowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && lParam != 0) {
        const KBDLLHOOKSTRUCT* hs = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (ScreenBlankManager::isPhysicalKeyboardInput(*hs)) {
            ScreenBlankManager::instance().triggerEmergencyWake();
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

ScreenBlankManager::ScreenBlankManager() = default;

ScreenBlankManager::~ScreenBlankManager() {
    disengageBlanking();
}

ScreenBlankManager& ScreenBlankManager::instance() {
    static ScreenBlankManager s_instance;
    return s_instance;
}

void ScreenBlankManager::powerDownDisplays() {
    // Hardware DPMS power off eliminated to prevent DWM composition disruption and cursor freezes
}

void ScreenBlankManager::wakeDisplays() {
    // DPMS wake safe no-op
}

bool ScreenBlankManager::isPhysicalEvent(DWORD flags, bool isMouse) {
    if (isMouse) {
        return (flags & LLMHF_INJECTED) == 0;
    } else {
        return (flags & LLKHF_INJECTED) == 0;
    }
}

bool ScreenBlankManager::isPhysicalMouseInput(const MSLLHOOKSTRUCT& hookStruct) {
    return isPhysicalEvent(hookStruct.flags, true);
}

bool ScreenBlankManager::isPhysicalKeyboardInput(const KBDLLHOOKSTRUCT& hookStruct) {
    return isPhysicalEvent(hookStruct.flags, false);
}

bool ScreenBlankManager::engageBlanking(
    ScreenBlankMode mode,
    const std::string& notice,
    const std::string& brand,
    bool showDeskId,
    uint64_t deskId)
{
    disengageBlanking();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentMode_.store(mode);
        customNotice_ = notice;
        brandName_ = brand;
        showDeskId_ = showDeskId;
        deskId_ = deskId;
        initReady_ = false;
    }

    blankActive_.store(true);

    blankThread_ = std::thread(&ScreenBlankManager::blankingThreadFunc, this);

    {
        std::unique_lock<std::mutex> lk(initMutex_);
        initCv_.wait_for(lk, std::chrono::milliseconds(2000), [this] {
            return initReady_;
        });
    }

    return true;
}

void ScreenBlankManager::disengageBlanking() {
    blankActive_.store(false);

    DWORD tid = blankThreadId_.exchange(0);
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }

    if (blankThread_.joinable()) {
        if (blankThread_.get_id() != std::this_thread::get_id()) {
            try {
                blankThread_.join();
            } catch (...) {}
        } else {
            blankThread_.detach();
        }
    }

    wakeDisplays();
}

bool ScreenBlankManager::isBlankActive() const {
    return blankActive_.load();
}

ScreenBlankMode ScreenBlankManager::currentBlankMode() const {
    return currentMode_.load();
}

void ScreenBlankManager::setEmergencyWakeCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    emergencyWakeCallback_ = std::move(cb);
}

void ScreenBlankManager::triggerEmergencyWake() {
    bool expected = true;
    if (!blankActive_.compare_exchange_strong(expected, false)) {
        return;
    }

    wakeDisplays();

    DWORD tid = blankThreadId_.load();
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        cb = emergencyWakeCallback_;
    }
    if (cb) {
        try {
            cb();
        } catch (...) {}
    }
}

void ScreenBlankManager::blankingThreadFunc() {
    DWORD tid = GetCurrentThreadId();
    blankThreadId_.store(tid);

    MSG dummyMsg;
    PeekMessageW(&dummyMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    ScreenBlankMode mode = currentMode_.load();

    // 1. Install low-level input hooks for emergency physical wake
    hMouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, BlankLowLevelMouseProc, GetModuleHandleW(nullptr), 0);
    hKbdHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, BlankLowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);

    // 2. Create curtain window if CurtainOnly or Unified
    if (mode == ScreenBlankMode::CurtainOnly || mode == ScreenBlankMode::Unified) {
        createCurtainWindow();
    }

    // 3. Command displays to DPMS standby if DpmsOnly or Unified
    if (mode == ScreenBlankMode::DpmsOnly || mode == ScreenBlankMode::Unified) {
        powerDownDisplays();
    }

    // Signal initialization ready
    {
        std::lock_guard<std::mutex> lk(initMutex_);
        initReady_ = true;
        initCv_.notify_all();
    }

    // Message loop
    MSG msg;
    while (blankActive_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Clean up
    destroyCurtainWindow();

    if (hMouseHook_) {
        UnhookWindowsHookEx(hMouseHook_);
        hMouseHook_ = nullptr;
    }
    if (hKbdHook_) {
        UnhookWindowsHookEx(hKbdHook_);
        hKbdHook_ = nullptr;
    }

    blankThreadId_.store(0);
}

void ScreenBlankManager::createCurtainWindow() {
    if (hwndCurtain_) return;

    std::string activeBrand = brandName_.empty() ? "CppDesk Enterprise Security" : brandName_;
    std::string activeNotice = customNotice_.empty() ? "Screen output hidden and local physical inputs secured for authorized administration." : customNotice_;

    s_curtainConfig.brandName = utf8ToWide(activeBrand);
    s_curtainConfig.noticeText = utf8ToWide(activeNotice);
    s_curtainConfig.showDeskId = showDeskId_;

    if (deskId_ > 0) {
        std::string rawId = std::to_string(deskId_);
        std::string fmtId;
        for (size_t i = 0; i < rawId.size(); ++i) {
            if (i > 0 && (rawId.size() - i) % 3 == 0) fmtId += "-";
            fmtId += rawId[i];
        }
        std::string fullText = "Workstation Desk ID: " + fmtId;
        s_curtainConfig.deskIdText = utf8ToWide(fullText);
    } else {
        s_curtainConfig.deskIdText = L"";
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BlankCurtainWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CppDeskScreenBlankCurtainClass";
    wc.hCursor = nullptr;
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max<int>(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int vh = std::max<int>(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    hwndCurtain_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"CppDeskScreenBlankCurtainClass",
        L"CppDesk Privacy Curtain",
        WS_POPUP,
        vx, vy, vw, vh,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );

    if (hwndCurtain_) {
        SetWindowDisplayAffinity(hwndCurtain_, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(hwndCurtain_, SW_SHOWMAXIMIZED);
        SetWindowPos(hwndCurtain_, HWND_TOPMOST, vx, vy, vw, vh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        UpdateWindow(hwndCurtain_);
    }
}

void ScreenBlankManager::destroyCurtainWindow() {
    if (hwndCurtain_) {
        DestroyWindow(hwndCurtain_);
        hwndCurtain_ = nullptr;
    }
}

} // namespace cppdesk
