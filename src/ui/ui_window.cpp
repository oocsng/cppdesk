#include "ui_window.hpp"
#include "theme.hpp"
#include "recording_canvas.hpp"
#include "../control/windows_service_manager.hpp"
#include "../capture/screen_capture.hpp"
#include <gdiplus.h>

#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#ifdef DeleteFile
#undef DeleteFile
#endif

#include <cstdio>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <algorithm>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

namespace cppdesk {

namespace {

// Second-order damped harmonic oscillator (macOS CoreAnimation CASpringAnimation physics)
bool stepSpring(float& pos, float& vel, float target, float omega, float zeta, float dt, float eps = 0.0012f) {
    return cppdesk::UiPlatform::stepSpring(pos, vel, target, omega, zeta, dt, eps);
}

bool stepExp(float& current, float target, float speed, float dt) {
    float diff = target - current;
    if (std::fabs(diff) <= 0.003f) {
        if (current != target) {
            current = target;
            return true;
        }
        return false;
    }
    float factor = 1.0f - std::exp(-speed * dt);
    current += diff * factor;
    return true;
}

float smoothStepEase(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), w.data(), len);
    return w;
}

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

// ---------------- Windows Platform Shell ----------------

class WinShell : public ui::PlatformShell {
public:
    explicit WinShell(CppDeskWindow& w) : w_(w) {}

    void invalidate() override {
        if (w_.hwnd_) InvalidateRect(w_.hwnd_, nullptr, FALSE);
    }

    void setCursor(ui::CursorShape shape) override {
        LPCTSTR id = IDC_ARROW;
        switch (shape) {
            case ui::CursorShape::Hand: id = IDC_HAND; break;
            case ui::CursorShape::IBeam: id = IDC_IBEAM; break;
            case ui::CursorShape::ResizeH: id = IDC_SIZEWE; break;
            case ui::CursorShape::ResizeV: id = IDC_SIZENS; break;
            default: break;
        }
        SetCursor(LoadCursor(nullptr, id));
    }

    float dpiScale() const override { return static_cast<float>(w_.dpi_) / 96.0f; }

    // Snapshots draw at a fixed instant so clock-driven pulses and timeouts are repeatable.
    uint64_t nowMs() const override { return w_.snapshotMode_ ? 10000 : GetTickCount64(); }

    void setTextInputActive(bool) override {}

    std::optional<std::string> chooseOpenFile(const std::string& title, const std::string& filterName,
                                              const std::string& filterPattern) override {
        std::wstring wTitle = utf8ToWide(title);
        std::wstring filter = utf8ToWide(filterName + " (" + filterPattern + ")");
        filter.push_back(L'\0');
        filter += utf8ToWide(filterPattern);
        filter.push_back(L'\0');
        filter += L"All Files (*.*)";
        filter.push_back(L'\0');
        filter += L"*.*";
        filter.push_back(L'\0');
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = w_.hwnd_;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = filter.c_str();
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle = wTitle.c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return std::nullopt;
        return wideToUtf8(file);
    }

    std::optional<std::string> chooseFolder(const std::string& title) override {
        std::optional<std::string> result;
        IFileDialog* pfd = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))) || !pfd) {
            return result;
        }
        DWORD options = 0;
        if (SUCCEEDED(pfd->GetOptions(&options))) pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        std::wstring wTitle = utf8ToWide(title);
        pfd->SetTitle(wTitle.c_str());
        if (SUCCEEDED(pfd->Show(w_.hwnd_))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(pfd->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = wideToUtf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        pfd->Release();
        return result;
    }

    void openUrl(const std::string& url) override {
        ShellExecuteW(nullptr, L"open", utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void revealPath(const std::string& path) override {
        std::wstring args = L"/select,\"" + utf8ToWide(path) + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    void relaunchApp() override {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (w_.hwnd_) DestroyWindow(w_.hwnd_);
    }

    bool setFullscreen(bool on) override { return w_.hwnd_ ? w_.setFullscreenWindow(on) : false; }
    void setDarkTitleBar(bool) override { w_.applyWindowThemeAttribute(); }
    void setFrameIntervalMs(unsigned ms) override {
        if (w_.hwnd_) SetTimer(w_.hwnd_, 1, ms, nullptr);
    }
    void quitApp() override {
        if (w_.hwnd_) DestroyWindow(w_.hwnd_);
    }
    bool relaunchElevated() override { return InputInjector::relaunchAsAdmin(w_.hwnd_); }
    void playAlertSound() override { MessageBeep(MB_OK); }
    void setMouseCapture(bool capture) override {
        if (capture) {
            if (w_.hwnd_) SetCapture(w_.hwnd_);
        } else {
            ReleaseCapture();
        }
    }
    bool isKeyDown(uint16_t vk) const override {
        return vk == VK_LBUTTON ? (GetAsyncKeyState(vk) & 0x8000) != 0 : (GetKeyState(vk) & 0x8000) != 0;
    }
    void trackMouseLeave() override {
        if (!w_.hwnd_) return;
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = w_.hwnd_;
        TrackMouseEvent(&tme);
    }
    bool loadAppIcon(std::vector<uint8_t>& bgra, int& width, int& height) override {
        return w_.loadAppIconPixels(bgra, width, height);
    }
    void checkForUpdates(bool manual) override {
        HWND target = w_.hwnd_;
        AutoUpdater::checkForUpdatesAsync("oocsng", "cppdesk", CPP_DESK_VERSION, [target, manual](const UpdateInfo& info) {
            if (target && IsWindow(target)) {
                auto* pInfo = new UpdateInfo(info);
                if (!PostMessageW(target, WM_DESK_UPDATE_CHECK_DONE, reinterpret_cast<WPARAM>(pInfo), manual ? 1 : 0)) {
                    delete pInfo;
                }
            }
        });
    }
    void startUpdateDownload(const std::string& url, uint64_t, const std::string& sha256,
                             std::function<void(bool, const std::string&)> done) override {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring targetPath = std::wstring(tempDir) + L"CppDesk_update.exe";
        wchar_t currentExe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, currentExe, MAX_PATH);
        std::wstring currExePath(currentExe);
        HWND h = w_.hwnd_;
        AutoUpdater::downloadUpdateAssetAsync(
            url, targetPath,
            [h](uint64_t downloaded, uint64_t total, double speed) {
                if (!h) return;
                auto* p = new UpdateProgressData{ downloaded, total, speed };
                if (!PostMessageW(h, WM_APP_UPDATE_PROGRESS, reinterpret_cast<WPARAM>(p), 0)) {
                    delete p;
                }
            },
            [h, targetPath, currExePath, done](bool success, const std::string& err) {
                if (!success) {
                    done(false, "Download failed: " + err);
                    return;
                }
                if (!AutoUpdater::validatePeBinary(targetPath)) {
                    done(false, "Validation error: Corrupt PE binary");
                    return;
                }
                done(true, "Restarting CppDesk...");
                if (AutoUpdater::launchSelfUpdate(targetPath, currExePath)) {
                    if (h) PostMessageW(h, WM_CLOSE, 0, 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    ExitProcess(0);
                } else {
                    done(false, "Failed to launch updater helper");
                }
            },
            sha256);
    }
    ui::ServiceState serviceState() const override {
        switch (WindowsServiceManager::getServiceState()) {
            case ServiceStatusState::Running: return ui::ServiceState::Running;
            case ServiceStatusState::Stopped: return ui::ServiceState::Stopped;
            case ServiceStatusState::Pending: return ui::ServiceState::Pending;
            default: return ui::ServiceState::NotInstalled;
        }
    }
    bool runServiceAction(ui::ServiceAction action) override {
        switch (action) {
            case ui::ServiceAction::Install: return WindowsServiceManager::installService();
            case ui::ServiceAction::Uninstall: return WindowsServiceManager::uninstallService();
            case ui::ServiceAction::Start: return WindowsServiceManager::startService();
            case ui::ServiceAction::Stop: return WindowsServiceManager::stopService();
        }
        return false;
    }
    void showNotification(const std::string&, const std::string&) override {}
    void setTrayStatus(const std::string&) override {}

    bool has(ui::Capability) const override { return true; }

private:
    CppDeskWindow& w_;
};

CppDeskWindow::CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget)
    : identity_(identity)
    , network_(network)
{
    shell_ = std::make_unique<WinShell>(*this);
    app_ = std::make_unique<AppUi>(identity_, network_, *shell_, autoConnectTarget);

    // Network callbacks arrive on worker threads; hand them to the UI thread as window messages.
    network_.setOnScratchpadUpdateCallback([this](const ScratchpadUpdatePayload& p) {
        if (!hwnd_) return;
        auto* copy = new ScratchpadUpdatePayload(p);
        if (!PostMessageW(hwnd_, WM_APP_SCRATCHPAD_UPDATE, reinterpret_cast<WPARAM>(copy), 0)) {
            delete copy;
        }
    });

    network_.setOnCommandPaletteOutputCallback([this](const CommandPaletteOutputPayload& p) {
        if (!hwnd_) return;
        auto* copy = new CommandPaletteOutputPayload(p);
        if (!PostMessageW(hwnd_, WM_APP_COMMAND_PALETTE, reinterpret_cast<WPARAM>(copy), 0)) {
            delete copy;
        }
    });

    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    qpcFreq_ = freq.QuadPart;
    lastQpcCounter_ = now.QuadPart;
}

CppDeskWindow::~CppDeskWindow() {
    app_.reset();
    releaseGraphics();
}

void CppDeskWindow::applyWindowThemeAttribute() {
    if (!hwnd_) return;
    BOOL dark = identity_.settings().darkTheme ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD cornerPref = 2; // DWMWCP_ROUND (native rounded window corners)
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
}



bool CppDeskWindow::create(HINSTANCE hInstance, int nCmdShow) {
    if (!initGraphics()) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &CppDeskWindow::WndProcStatic;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    if (!appIcon) appIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIcon = appIcon;
    wc.hIconSm = appIcon;
    wc.lpszClassName = L"CppDeskMainWindowClass";
    RegisterClassExW(&wc);

    std::string title = "CppDesk";
    if (identity_.instanceId() > 1) {
        title += " #" + std::to_string(identity_.instanceId());
    }
    std::wstring wTitle = utf8ToWide(title);

    int offset = (identity_.instanceId() - 1) * 44;
    constexpr float kInitialWidthDips = 1220.0f;
    constexpr float kInitialHeightDips = 760.0f;
    const unsigned systemDpi = GetDpiForSystem();
    hwnd_ = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        wc.lpszClassName,
        wTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        100 + offset,
        70 + offset,
        dipToPx(kInitialWidthDips, systemDpi),
        dipToPx(kInitialHeightDips, systemDpi),
        nullptr,
        nullptr,
        hInstance,
        this
    );

    if (!hwnd_) {
        return false;
    }

    // The window may have opened on a monitor whose DPI differs from the system DPI. Size it for that monitor
    // and keep it inside the work area: 1220x760 DIPs at 150% is taller than a 1080p screen.
    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) dpi_ = 96;
    {
        const int widthPx = dipToPx(kInitialWidthDips, dpi_);
        const int heightPx = dipToPx(kInitialHeightDips, dpi_);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        RECT placed{};
        GetWindowRect(hwnd_, &placed);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
            const RECT& work = monitorInfo.rcWork;
            const SpanFit x = fitSpanToWork(placed.left, widthPx, work.left, work.right);
            const SpanFit y = fitSpanToWork(placed.top, heightPx, work.top, work.bottom);
            SetWindowPos(hwnd_, nullptr, x.pos, y.pos, x.size, y.size, SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (dpi_ != systemDpi) {
            SetWindowPos(hwnd_, nullptr, 0, 0, widthPx, heightPx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    applyWindowThemeAttribute();
    DragAcceptFiles(hwnd_, TRUE);

    app_->notificationMgr_ = std::make_unique<NotificationManager>();
    app_->notificationMgr_->init(hwnd_, hInstance, title);

    network_.setOnFrameDecodedCallback([this]() {
        if (!app_->frameRedrawPending_.exchange(true)) {
            if (hwnd_) {
                PostMessageW(hwnd_, WM_APP_FRAME_READY, 0, 0);
            }
        }
    });

    network_.setOnVersionMismatchCallback([this](const std::string& hostVer, const std::string& viewerVer) {
        app_->peerMismatchHostVer_ = hostVer;
        app_->peerMismatchViewerVer_ = viewerVer;
        app_->showPeerMismatchModal_ = true;
        app_->showUpdateRequiredModal_ = true;
        std::string cleanVer = hostVer;
        if (!cleanVer.empty() && (cleanVer[0] == 'v' || cleanVer[0] == 'V')) {
            cleanVer = cleanVer.substr(1);
        }
        app_->latestUpdateInfo_.latestVersion = cleanVer;
        app_->triggerUpdateCheck(false);
        if (hwnd_) {
            shell_->invalidate();
        }
    });

    network_.setOnVersionUpgradeNoticeCallback([this](const VersionUpgradeNoticePayload& notice) {
        app_->showToast("Notice from Viewer: Upgrade to CppDesk v" + notice.senderVersion + " recommended.");
    });

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    SetTimer(hwnd_, 1, 32, nullptr);

    if (!app_->autoConnectTarget_.empty()) {
        app_->remoteIdInput_ = app_->autoConnectTarget_;
        app_->initiateConnection();
    }
    return true;
}

int CppDeskWindow::messageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

bool CppDeskWindow::initGraphics() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    if (FAILED(hr) || !d2dFactory_) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&dwriteFactory_)
    );
    if (FAILED(hr) || !dwriteFactory_) return false;

    d2dCanvas_ = std::make_unique<ui::D2DCanvas>(d2dFactory_, dwriteFactory_);
    return true;
}

void CppDeskWindow::discardDeviceResources() {
    // Detaching drops every canvas image, so the handles are simply cleared.
    if (d2dCanvas_) d2dCanvas_->detach();
    if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
    hwndTarget_ = nullptr;
    if (!app_) return;   // teardown: the screens are already gone
    app_->appIconBitmap_ = {};
    app_->remoteBitmap_ = {};
    app_->recordingPlayerBitmap_ = {};
    app_->bitmapW_ = 0;
    app_->bitmapH_ = 0;
    app_->recordingPlayerBitmapW_ = 0;
    app_->recordingPlayerBitmapH_ = 0;
}

void CppDeskWindow::releaseGraphics() {
    discardDeviceResources();
    d2dCanvas_.reset();
    if (dwriteFactory_) { dwriteFactory_->Release(); dwriteFactory_ = nullptr; }
    if (d2dFactory_) { d2dFactory_->Release(); d2dFactory_ = nullptr; }
}

bool CppDeskWindow::loadAppIconPixels(std::vector<uint8_t>& bgra, int& outW, int& outH) {

    TileCodec::initGdiPlus();

    Gdiplus::Bitmap* gdiBmp = nullptr;
    IStream* pStream = nullptr;

    // 1. Embedded RCDATA resource 102
    HRSRC hRes = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), RT_RCDATA);
    if (hRes) {
        HGLOBAL hData = LoadResource(GetModuleHandleW(nullptr), hRes);
        if (hData) {
            void* pBytes = LockResource(hData);
            DWORD dwSize = SizeofResource(GetModuleHandleW(nullptr), hRes);
            if (pBytes && dwSize > 0) {
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dwSize);
                if (hMem) {
                    void* pDest = GlobalLock(hMem);
                    if (pDest) {
                        std::memcpy(pDest, pBytes, dwSize);
                        GlobalUnlock(hMem);
                        if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) == S_OK) {
                            gdiBmp = Gdiplus::Bitmap::FromStream(pStream, FALSE);
                        } else {
                            GlobalFree(hMem);
                            pStream = nullptr;
                        }
                    } else {
                        GlobalFree(hMem);
                    }
                }
            }
        }
    }

    // 2. Fallback to assets/icon.png on disk
    if (!gdiBmp || gdiBmp->GetLastStatus() != Gdiplus::Ok) {
        if (gdiBmp) { delete gdiBmp; gdiBmp = nullptr; }
        if (pStream) { pStream->Release(); pStream = nullptr; }
        if (std::filesystem::exists("assets/icon.png")) {
            gdiBmp = Gdiplus::Bitmap::FromFile(L"assets/icon.png");
        }
    }

    // 3. Fallback to Win32 icon 101
    if (!gdiBmp || gdiBmp->GetLastStatus() != Gdiplus::Ok) {
        if (gdiBmp) { delete gdiBmp; gdiBmp = nullptr; }
        if (pStream) { pStream->Release(); pStream = nullptr; }
        HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, 64, 64, LR_DEFAULTCOLOR);
        if (hIcon) {
            gdiBmp = new Gdiplus::Bitmap(hIcon);
            DestroyIcon(hIcon);
        }
    }

    if (!gdiBmp || gdiBmp->GetLastStatus() != Gdiplus::Ok) {
        if (gdiBmp) delete gdiBmp;
        if (pStream) pStream->Release();
        return false;
    }

    bool loaded = false;
    UINT w = gdiBmp->GetWidth();
    UINT h = gdiBmp->GetHeight();
    if (w > 0 && h > 0) {
        Gdiplus::Rect lockRect(0, 0, w, h);
        Gdiplus::BitmapData bdata;
        if (gdiBmp->LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bdata) == Gdiplus::Ok) {
            bgra.resize(static_cast<size_t>(w) * h * 4);
            for (UINT y = 0; y < h; ++y) {
                std::memcpy(bgra.data() + static_cast<size_t>(y) * w * 4,
                            static_cast<const uint8_t*>(bdata.Scan0) + static_cast<ptrdiff_t>(y) * bdata.Stride,
                            static_cast<size_t>(w) * 4);
            }
            outW = static_cast<int>(w);
            outH = static_cast<int>(h);
            loaded = true;
            gdiBmp->UnlockBits(&bdata);
        }
    }

    delete gdiBmp;
    if (pStream) {
        pStream->Release();
        pStream = nullptr;
    }
    return loaded;
}


LRESULT CALLBACK CppDeskWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CppDeskWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<CppDeskWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<CppDeskWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->handleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CppDeskWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                if (app_->hoveredIsTextInput_) {
                    SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                } else if (!app_->hoveredWidgetId_.empty()) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }
                return TRUE;
            }
            break;
        }
        case WM_MOUSELEAVE: {
            app_->mouseInsideClient_ = false;
            app_->trackingMouseLeave_ = false;
            app_->hoveredWidgetId_.clear();
            app_->hoveredIsTextInput_ = false;
            shell_->invalidate();
            return 0;
        }
        case WM_KILLFOCUS: {
            app_->mouseLeftDown_ = false;
            app_->draggingSettingsScrollbar_ = false;
            app_->pressedWidgetId_.clear();
            network_.sendReleaseAllModifiers();
            shell_->invalidate();
            return 0;
        }
        case WM_ACTIVATE:
        case WM_SETFOCUS: {
            if (app_->notificationMgr_) {
                app_->notificationMgr_->stopFlash();
            }
            if (app_->activeTab_ == ActiveTab::RemoteSession) {
                network_.markViewerCanvasFullDirty();
                app_->displayedFrameSeq_ = 0;
                shell_->invalidate();
            }
            break;
        }
        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_MINIMIZE && identity_.settings().minimizeToTray) {
                ShowWindow(hwnd_, SW_HIDE);
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            if (identity_.settings().minimizeToTray) {
                ShowWindow(hwnd_, SW_HIDE);
                return 0;
            }
            network_.disconnectViewer();
            app_->hardResetViewerUiState();
            DestroyWindow(hwnd_);
            return 0;
        }
        case WM_COMMAND: {
            WORD cmdId = LOWORD(wParam);
            if (cmdId == IDM_TRAY_RESTORE) {
                restoreFromTray();
                return 0;
            } else if (cmdId == IDM_TRAY_MUTE) {
                if (app_->notificationMgr_) {
                    app_->notificationMgr_->setMuted(!app_->notificationMgr_->isMuted());
                    app_->showToast(app_->notificationMgr_->isMuted() ? "Notifications muted" : "Notifications unmuted");
                }
                return 0;
            } else if (cmdId == IDM_TRAY_EXIT) {
                DestroyWindow(hwnd_);
                return 0;
            }
            break;
        }
        case WM_TRAYICON: {
            UINT event = LOWORD(lParam);
            switch (event) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK: {
                    NotificationType nType = app_->notificationMgr_ ? app_->notificationMgr_->lastNotificationType() : NotificationType::GeneralInfo;
                    restoreFromTray(nType);
                    if (app_->notificationMgr_) app_->notificationMgr_->clearLastNotificationType();
                    return 0;
                }
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    POINT pt;
                    if (!GetCursorPos(&pt)) {
                        pt.x = static_cast<short>(LOWORD(wParam));
                        pt.y = static_cast<short>(HIWORD(wParam));
                    }
                    std::string status = "Online";
                    auto vStats = network_.viewerStats();
                    if (vStats.state == ViewerConnectionState::Connected) {
                        status = "Controlling " + vStats.remoteHostname;
                    } else if (network_.hostSessionStatus().active) {
                        status = "Host Active";
                    }
                    if (app_->notificationMgr_) {
                        app_->notificationMgr_->showContextMenu(pt.x, pt.y, status);
                    }
                    return 0;
                }
            }
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            if (dpi_ == 0) dpi_ = 96;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (renderTarget_) renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
            shell_->invalidate();
            return 0;
        }
        case WM_SIZE: {
            if (renderTarget_) {
                RECT rc{};
                GetClientRect(hwnd_, &rc);
                D2D1_SIZE_U sz = D2D1::SizeU(std::max<UINT32>(1, rc.right - rc.left),
                                             std::max<UINT32>(1, rc.bottom - rc.top));
                HRESULT hr = hwndTarget_ ? hwndTarget_->Resize(sz) : S_OK;
                if (hr == D2DERR_RECREATE_TARGET) {
                    discardDeviceResources();
                    shell_->invalidate();
                }
            }
            app_->sessionHudScrollOffset_ = std::clamp(app_->sessionHudScrollOffset_, 0.0f, app_->sessionHudMaxScroll_);
            app_->navPillInit_ = false;
            app_->activeTooltipText_.clear();
            app_->tooltipAnimT_ = 0.0f;
            app_->tooltipAnimVel_ = 0.0f;
            shell_->invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if (app_->draggingRewindScrubber_) {
                app_->draggingRewindScrubber_ = false;
            }
            app_->draggingSettingsScrollbar_ = false;
            app_->pressedWidgetId_.clear();
            shell_->invalidate();
            return 0;
        }
        case WM_TIMER: {
            uint64_t tickNow = shell_->nowMs();
            if (app_->remoteFilePane_.expireLoading(tickNow)) {
                app_->showToast(app_->remoteFilePane_.statusMessage, true);
                shell_->invalidate();
            }
            auto vStats = network_.viewerStats();
            if (vStats.state == ViewerConnectionState::Connected &&
                app_->prevViewerState_ != ViewerConnectionState::Connected) {
                app_->switchTab(ActiveTab::RemoteSession);
                app_->scaleMode_ = static_cast<ScaleMode>(std::clamp<int>(identity_.settings().defaultScaleMode, 0, 3));
                if (vStats.remoteAddress.find("127.0.0.1") != std::string::npos ||
                    vStats.remoteDeskId == identity_.deskId()) {
                    app_->remoteInputEnabled_ = false;
                    app_->showToast("Connected on same PC. View-Only enabled (F8 to toggle).");
                } else {
                    app_->remoteInputEnabled_ = true;
                    app_->showToast("Connected to " + vStats.remoteHostname);
                }

                SessionTab* activeTab = app_->sessionTabs_.activeTab();
                if (activeTab) {
                    activeTab->state = ViewerConnectionState::Connected;
                    activeTab->deskId = vStats.remoteDeskId;
                    activeTab->remoteHostname = vStats.remoteHostname;
                    if (!vStats.remoteHostname.empty()) {
                        activeTab->title = vStats.remoteHostname;
                    }
                    activeTab->connectedSinceTickMs = tickNow;
                }
            } else if (vStats.state == ViewerConnectionState::Error &&
                       app_->prevViewerState_ != ViewerConnectionState::Error) {
                SessionTab* activeTab = app_->sessionTabs_.activeTab();
                if (activeTab) {
                    activeTab->state = ViewerConnectionState::Error;
                    activeTab->statusMessage = vStats.statusMessage;
                }
                app_->showToast(vStats.statusMessage, true);
                if (app_->sessionTabs_.tabCount() <= 1) {
                    app_->switchTab(ActiveTab::Dashboard);
                }
            } else if (app_->prevViewerState_ == ViewerConnectionState::Connected &&
                       vStats.state == ViewerConnectionState::Disconnected) {
                if (app_->sessionRecorder_.isRecording()) {
                    app_->sessionRecorder_.stopRecording();
                }
                if (network_.isVoiceIntercomActive()) {
                    network_.stopVoiceIntercom();
                }
                app_->rewindBuffer_.clear();
                app_->showRewindScrubber_ = false;
                app_->rewindFrozen_ = false;
                app_->rewindOffsetMs_ = 0;
                if (app_->notificationMgr_) {
                    std::string hName = app_->prevViewerHostname_.empty() ? "Remote Desktop" : app_->prevViewerHostname_;
                    app_->notificationMgr_->notify(NotificationType::SessionDropped, "Session Disconnected",
                                             "Remote session with " + hName + " ended.", identity_.settings());
                }
                SessionTab* activeTab = app_->sessionTabs_.activeTab();
                if (activeTab) {
                    activeTab->state = ViewerConnectionState::Disconnected;
                }
                if (app_->sessionTabs_.tabCount() <= 1) {
                    app_->sessionTabs_.closeAllTabs();
                    app_->switchTab(ActiveTab::Dashboard);
                }
            }
            if (vStats.state == ViewerConnectionState::Connected) {
                app_->prevViewerHostname_ = vStats.remoteHostname;
                SessionTab* curTab = app_->sessionTabs_.activeTab();
                if (curTab) {
                    curTab->fps = vStats.fps;
                    curTab->rttMs = vStats.rttMs;
                    curTab->activeMonitorIndex = vStats.activeMonitorIndex;
                    curTab->monitorCount = vStats.monitorCount;
                    curTab->qualityPreset = vStats.qualityPreset;
                    curTab->privacyModeEngaged = vStats.privacyModeEngaged;
                    if (!vStats.remoteHostname.empty() && curTab->remoteHostname != vStats.remoteHostname) {
                        curTab->remoteHostname = vStats.remoteHostname;
                        curTab->title = vStats.remoteHostname;
                    }
                }
            }
            app_->prevViewerState_ = vStats.state;

            // Host session disconnect detection
            auto hostStatus = network_.hostSessionStatus();
            if (app_->prevHostActive_ && !hostStatus.active) {
                if (app_->notificationMgr_) {
                    std::string cName = app_->prevHostClientName_.empty() ? "Remote Client" : app_->prevHostClientName_;
                    app_->notificationMgr_->notify(NotificationType::SessionDropped, "Client Disconnected",
                                             cName + " disconnected from your desktop.", identity_.settings());
                }
            }
            if (hostStatus.active) {
                app_->prevHostClientName_ = hostStatus.viewerHostname;
            }
            app_->prevHostActive_ = hostStatus.active;

            // Chat message notifications
            uint32_t unreadChat = network_.unreadChatCount();
            if (app_->showFileDrawer_ && app_->drawerTab_ == DrawerTab::LiveChat && unreadChat > 0) {
                network_.markChatRead();
                unreadChat = 0;
            }
            if (unreadChat > app_->lastSeenUnreadChat_) {
                auto msgs = network_.chatMessages();
                if (!msgs.empty() && !msgs.back().fromLocal) {
                    app_->showToast(msgs.back().senderName + ": " + msgs.back().text);
                    bool isUnfocused = (GetForegroundWindow() != hwnd_ || IsIconic(hwnd_));
                    bool drawerClosed = (!app_->showFileDrawer_ || app_->drawerTab_ != DrawerTab::LiveChat);
                    if ((isUnfocused || drawerClosed) && app_->notificationMgr_) {
                        app_->notificationMgr_->notify(
                            NotificationType::ChatMessage,
                            msgs.back().senderName.empty() ? "New Chat Message" : msgs.back().senderName,
                            msgs.back().text,
                            identity_.settings()
                        );
                    }
                }
            }
            app_->lastSeenUnreadChat_ = unreadChat;

            // Completed file transfer notifications
            auto transfers = network_.fileTransferManager().snapshotTransfers();
            size_t completedCount = 0;
            std::string newlyCompletedName;
            for (const auto& item : transfers) {
                if (item.status == TransferStatus::Completed) {
                    completedCount++;
                    if (!item.isOutgoing && completedCount > app_->prevCompletedTransfersCount_) {
                        newlyCompletedName = item.fileName;
                    }
                }
            }
            if (completedCount > app_->prevCompletedTransfersCount_) {
                if (!newlyCompletedName.empty() && app_->notificationMgr_) {
                    app_->notificationMgr_->notify(
                        NotificationType::FileTransferDone,
                        "File Transfer Complete",
                        "File '" + newlyCompletedName + "' received successfully.",
                        identity_.settings()
                    );
                }
            }
            app_->prevCompletedTransfersCount_ = completedCount;

            // Incoming connection request notification
            auto pending = network_.pendingIncomingRequest();
            if (pending.active && !app_->prevHasPendingIncoming_) {
                app_->modalPermissions_ = identity_.settings().defaultPermissions;
                SetForegroundWindow(hwnd_);
                if (app_->notificationMgr_) {
                    std::string title = "Incoming Connection Request";
                    std::string caller = pending.callerHostname.empty() ? "Remote User" : pending.callerHostname;
                    std::string msg = caller + " (" + CryptoUtils::formatDeskId(pending.callerDeskId) + ") is requesting remote control.";
                    app_->notificationMgr_->notify(NotificationType::IncomingConnection, title, msg, identity_.settings());
                }
                shell_->invalidate();
            }
            app_->prevHasPendingIncoming_ = pending.active;
            app_->modalWasActive_ = pending.active;

            bool toastActive = (!app_->toastText_.empty() && tickNow <= app_->toastExpireTick_ + 500);
            bool inLiveSession = (vStats.state != ViewerConnectionState::Disconnected) ||
                                 network_.hostSessionStatus().active ||
                                 pending.active;

            if (app_->hasPendingMouseMove_) {
                app_->flushPendingMouseMove();
            }

            static uint32_t idleTickCounter = 0;
            ++idleTickCounter;
            bool micMonitoringActive = (app_->activeTab_ == ActiveTab::Settings && app_->settingsSection_ == SettingsSection::AudioVoice && network_.isVoiceMonitoring());
            if (inLiveSession || toastActive || micMonitoringActive || (idleTickCounter % 4 == 0)) {
                shell_->invalidate();
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            onPaint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;

        case WM_MOUSEMOVE:
            app_->onMouseMove(toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            app_->onMouseButton(MouseButtonId::Left, true, toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_LBUTTONUP:
            app_->onMouseButton(MouseButtonId::Left, false, toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_RBUTTONDOWN:
            app_->onMouseButton(MouseButtonId::Right, true, toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_RBUTTONUP:
            app_->onMouseButton(MouseButtonId::Right, false, toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MBUTTONDOWN:
            app_->onMouseButton(MouseButtonId::Middle, true, toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MBUTTONUP:
            app_->onMouseButton(MouseButtonId::Middle, false, toDip(GET_X_LPARAM(lParam)), toDip(GET_Y_LPARAM(lParam)));
            return 0;

        case WM_MOUSEWHEEL:
            app_->onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;

        case WM_CHAR:
            app_->onCharInput(static_cast<wchar_t>(wParam));
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            uint16_t vk = static_cast<uint16_t>(wParam);
            uint16_t scan = static_cast<uint16_t>((lParam >> 16) & 0xFF);
            bool isExt = (lParam & (1 << 24)) != 0;
            app_->onKeyEvent(vk, scan, true, isExt);
            return 0;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            uint16_t vk = static_cast<uint16_t>(wParam);
            uint16_t scan = static_cast<uint16_t>((lParam >> 16) & 0xFF);
            bool isExt = (lParam & (1 << 24)) != 0;
            app_->onKeyEvent(vk, scan, false, isExt);
            return 0;
        }
        case WM_DROPFILES:
            onDropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;

        case WM_DESK_UPDATE_CHECK_DONE: {
            auto* info = reinterpret_cast<UpdateInfo*>(wParam);
            bool manual = (lParam != 0);
            if (info) {
                app_->latestUpdateInfo_ = *info;
                app_->isCheckingUpdates_ = false;
                if (!info->success) {
                    app_->updateStatusText_ = "Check failed (Offline or rate-limited)";
                    if (manual) app_->showToast("Could not check for updates", true);
                } else if (info->updateRequired) {
                    std::string cleanVer = info->latestVersion;
                    if (!cleanVer.empty() && (cleanVer[0] == 'v' || cleanVer[0] == 'V')) {
                        cleanVer = cleanVer.substr(1);
                    }
                    app_->updateStatusText_ = "Mandatory Update: v" + cleanVer;
                    app_->showUpdateRequiredModal_ = true;
                    if (manual) app_->showToast("Mandatory update v" + cleanVer + " available!", true);
                } else {
                    app_->updateStatusText_ = "CppDesk is up to date (v" + std::string(CPP_DESK_VERSION) + ")";
                    if (manual) app_->showToast("You have the latest version (v" + std::string(CPP_DESK_VERSION) + ")");
                }
                delete info;
            }
            shell_->invalidate();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = dipToPx(900.0f, dpi_);
            mmi->ptMinTrackSize.y = dipToPx(600.0f, dpi_);
            return 0;
        }

        case WM_APP_SCRATCHPAD_UPDATE: {
            std::unique_ptr<ScratchpadUpdatePayload> own(reinterpret_cast<ScratchpadUpdatePayload*>(wParam));
            if (own) {
                if (app_->focusedField_ != FocusedField::ScratchpadText) {
                    app_->scratchpadTextEdit_ = own->text;
                }
                app_->lastSeenScratchpadRev_ = own->revision;
                shell_->invalidate();
            }
            return 0;
        }

        case WM_APP_COMMAND_PALETTE: {
            std::unique_ptr<CommandPaletteOutputPayload> own(reinterpret_cast<CommandPaletteOutputPayload*>(wParam));
            if (own) {
                app_->omnibarLastOutput_ = own->stdoutData.empty() ? own->errorMessage : own->stdoutData;
                app_->omnibarLastExitCode_ = own->exitCode;
                app_->omnibarHasOutput_ = true;
                shell_->invalidate();
            }
            return 0;
        }

        case WM_APP_UPDATE_PROGRESS: {
            std::unique_ptr<UpdateProgressData> own(reinterpret_cast<UpdateProgressData*>(wParam));
            if (own) {
                app_->updateDownloadedBytes_ = own->downloaded;
                app_->updateTotalBytes_ = own->total;
                app_->updateSpeedBps_ = own->speed;
                if (own->total > 0) {
                    app_->updateDownloadProgress_ = static_cast<float>(own->downloaded) / static_cast<float>(own->total);
                }
                shell_->invalidate();
            }
            return 0;
        }

        case WM_APP_FRAME_READY: {
            app_->frameRedrawPending_.store(false);
            if (app_->hasPendingMouseMove_) {
                app_->flushPendingMouseMove();
            }
            shell_->invalidate();
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd_, 1);
            if (app_->sessionRecorder_.isRecording()) {
                app_->sessionRecorder_.stopRecording();
            }
            if (app_->notificationMgr_) {
                app_->notificationMgr_->shutdown();
            }
            network_.disconnectViewer();
            app_->hardResetViewerUiState();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

// ---------------- Primitive Drawing, Vector Icons & macOS Surface Helpers ----------------

















// ---------------- Main Paint Orchestrator ----------------

void CppDeskWindow::onPaint() {
    if (!d2dFactory_ || !d2dCanvas_) return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    float dt = (qpcFreq_ > 0)
        ? static_cast<float>(now.QuadPart - lastQpcCounter_) / static_cast<float>(qpcFreq_)
        : 0.016f;
    lastQpcCounter_ = now.QuadPart;

    const D2D1_SIZE_F clientDips = clientSizeDips();

    if (!renderTarget_) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
        rtProps.type = identity_.settings().hardwareAcceleration
            ? D2D1_RENDER_TARGET_TYPE_DEFAULT
            : D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        // Target DPI = window DPI, so every layout number is a DIP and Direct2D scales it to pixels.
        rtProps.dpiX = static_cast<float>(dpi_);
        rtProps.dpiY = static_cast<float>(dpi_);
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(hwnd_, clientSizePx());
        if (FAILED(d2dFactory_->CreateHwndRenderTarget(rtProps, hwndProps, &hwndTarget_)) || !hwndTarget_) {
            return;
        }
        renderTarget_ = hwndTarget_;
        d2dCanvas_->attach(renderTarget_, 1.0f);
    }

    app_->render(*d2dCanvas_, clientDips.width, clientDips.height, dt);
    if (d2dCanvas_->consumeRecreateNeeded()) {
        discardDeviceResources();
        shell_->invalidate();
    } else if (app_->wantsAnimationFrame()) {
        shell_->invalidate();
    }
}


// ---------------- macOS Unified Top Toolbar ----------------


// ---------------- Minimalistic macOS Dashboard View ----------------


// ---------------- Session Tab Bar (Feature 6) ----------------


// ---------------- Remote Session View ----------------


// ---------------- macOS System Settings View ----------------


// ---------------- Floating macOS Side Sheet Drawer ----------------


// ---------------- Remote File Sync & Folder Mirroring Drawer ----------------


// ---------------- macOS Sheet Connection Approval Modal ----------------


// ---------------- macOS Dynamic Island Floating Top Bar ----------------


// ---------------- Dynamic Island Clipboard File Transfer Progress Pill ----------------


// ---------------- macOS Keyboard Shortcuts Sheet Modal ----------------


// ---------------- TCP Port Forwarding Manager Sheet Modal (v2.1.0) ----------------


// ---------------- Address Book Edit Sheet Modal (v2.1.0) ----------------


// ---------------- Dual-Pane File Explorer, Run Task & File Action Modals ----------------










// ---------------- Mandatory Update Modal (v3.0.0) ----------------


// ---------------- Remote Reboot & Reconnect Sheet Modal ----------------



// ---------------- In-Session Session Recording Player & Transcoder ----------------




// ---------------- Two-Factor Authentication (TOTP) UI & QR Rendering ----------------





// ---------------- Whiteboard & Screen Annotation Overlay (v2.1.0) ----------------


// ---------------- Real-Time Performance & Diagnostics HUD Overlay ----------------


// ---------------- macOS Dynamic Capsule Toast Banner ----------------


// ---------------- Master Volume Popup ----------------


// ---------------- Input & Interaction Handlers ----------------


D2D1_SIZE_U CppDeskWindow::clientSizePx() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    return D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
                       static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
}


D2D1_SIZE_F CppDeskWindow::clientSizeDips() const {
    if (snapshotMode_) return D2D1::SizeF(kSnapshotWidth, kSnapshotHeight);
    D2D1_SIZE_U px = clientSizePx();
    return D2D1::SizeF(pxToDip(static_cast<float>(px.width), dpi_), pxToDip(static_cast<float>(px.height), dpi_));
}









void CppDeskWindow::onDropFiles(HDROP hDrop) {
    POINT pt{};
    DragQueryPoint(hDrop, &pt);
    std::vector<std::string> paths;
    UINT count = DragQueryFileA(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        char filePath[MAX_PATH] = {};
        if (DragQueryFileA(hDrop, i, filePath, MAX_PATH) > 0) {
            paths.emplace_back(filePath);
        }
    }
    DragFinish(hDrop);
    // DragQueryPoint returns physical client pixels; the UI works in DIPs.
    app_->onFilesDropped(paths, toDip(pt.x), toDip(pt.y));
}

// ---------------- High-Level UI Actions ----------------










bool CppDeskWindow::setFullscreenWindow(bool on) {
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    if (on) {
        savedWindowPlacement_.length = sizeof(WINDOWPLACEMENT);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetWindowPlacement(hwnd_, &savedWindowPlacement_) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd_, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            return true;
        }
        return false;
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &savedWindowPlacement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        return true;
    }
}



void CppDeskWindow::restoreFromTray(NotificationType contextType) {
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    if (app_->notificationMgr_) {
        app_->notificationMgr_->stopFlash();
    }
    if (contextType == NotificationType::ChatMessage) {
        app_->showFileDrawer_ = true;
        app_->drawerTab_ = DrawerTab::LiveChat;
        network_.markChatRead();
        app_->focusedField_ = FocusedField::ChatInput;
    } else if (contextType == NotificationType::FileTransferDone) {
        app_->showFileDrawer_ = true;
        app_->drawerTab_ = DrawerTab::FilesAndClip;
    } else if (contextType == NotificationType::IncomingConnection) {
        auto pending = network_.pendingIncomingRequest();
        if (pending.active) {
            SetForegroundWindow(hwnd_);
        }
    }
    shell_->invalidate();
}



// ---------------- Offscreen UI Snapshots (regression checks) ----------------

namespace {

struct SnapshotState {
    const char* name;
    std::function<void(CppDeskWindow&)> setup;      // once, before the first frame
    std::function<void(CppDeskWindow&)> perFrame;   // after each animation step, before drawing
};

bool writeBmp32(const std::wstring& path, const uint8_t* bgra, UINT width, UINT height, UINT stride) {
    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    const DWORD imageBytes = width * height * 4;
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + imageBytes;
    bih.biSize = sizeof(bih);
    bih.biWidth = static_cast<LONG>(width);
    bih.biHeight = -static_cast<LONG>(height);   // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imageBytes;
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    out.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    for (UINT y = 0; y < height; ++y) {
        out.write(reinterpret_cast<const char*>(bgra + static_cast<size_t>(y) * stride), width * 4);
    }
    return static_cast<bool>(out);
}

} // namespace

int CppDeskWindow::runUiSnapshots(IdentityManager& identity, NetworkEngine& network, const std::wstring& outDir,
                                  bool record) {
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!wic) return 0;

    int written = 0;
    std::string timings;   // average frame time per state, for before/after performance checks
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    for (const auto& name : AppUi::snapshotStateNames()) {
        CppDeskWindow w(identity, network, "");
        w.snapshotMode_ = true;
        w.dpi_ = 96;
        if (!w.initGraphics()) continue;

        IWICBitmap* bitmap = nullptr;
        wic->CreateBitmap(static_cast<UINT>(kSnapshotWidth), static_cast<UINT>(kSnapshotHeight),
                          GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
        if (!bitmap) continue;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(w.d2dFactory_->CreateWicBitmapRenderTarget(bitmap, props, &w.renderTarget_)) || !w.renderTarget_) {
            bitmap->Release();
            continue;
        }
        w.d2dCanvas_->attach(w.renderTarget_, 1.0f);
        ui::RecordingCanvas recorder;
        ui::Canvas& canvas = record ? static_cast<ui::Canvas&>(recorder) : static_cast<ui::Canvas&>(*w.d2dCanvas_);

        w.app_->applySnapshotState(name);
        // Settle every spring at a fixed step; the snapshot state holds animation time still.
        constexpr double kDt = 1.0 / 60.0;
        constexpr int kFrames = 150;
        constexpr int kTimedFrom = 50;   // time only the settled frames
        double drawSeconds = 0.0;
        for (int frame = 0; frame < kFrames; ++frame) {
            LARGE_INTEGER t0{}, t1{};
            QueryPerformanceCounter(&t0);
            w.app_->render(canvas, kSnapshotWidth, kSnapshotHeight, kDt);
            QueryPerformanceCounter(&t1);
            if (frame >= kTimedFrom && qpcFreq.QuadPart > 0) {
                drawSeconds += static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
            }
        }
        char line[96];
        std::snprintf(line, sizeof(line), "%s %.3f ms\n", name.c_str(), 1000.0 * drawSeconds / (kFrames - kTimedFrom));
        timings += line;

        if (record) {
            std::ofstream ops(std::filesystem::path(outDir) / (name + ".ops.txt"), std::ios::binary | std::ios::trunc);
            ops << recorder.serialize();
            if (ops) ++written;
        } else {
            WICRect rc{ 0, 0, static_cast<INT>(kSnapshotWidth), static_cast<INT>(kSnapshotHeight) };
            IWICBitmapLock* lock = nullptr;
            if (SUCCEEDED(bitmap->Lock(&rc, WICBitmapLockRead, &lock)) && lock) {
                UINT stride = 0, size = 0;
                BYTE* data = nullptr;
                lock->GetStride(&stride);
                lock->GetDataPointer(&size, &data);
                std::wstring path = (std::filesystem::path(outDir) / (name + ".bmp")).wstring();
                if (data && writeBmp32(path, data, rc.Width, rc.Height, stride)) ++written;
                lock->Release();
            }
        }
        w.app_.reset();
        w.releaseGraphics();
        bitmap->Release();
        network.injectViewerFrameForTest({}, 0, 0);
    }
    {
        AppSettings s = identity.settings();
        s.darkTheme = false;
        identity.updateSettings(s);
    }
    wic->Release();
    std::ofstream(std::filesystem::path(outDir) / "timings.txt", std::ios::trunc) << timings;
    return written;
}

} // namespace cppdesk
