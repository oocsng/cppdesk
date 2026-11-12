#pragma once

#include "ui_platform.hpp"
#include "dpi.hpp"
#include "text_input.hpp"
#include "../core/protocol.hpp"
#include "../core/crypto_identity.hpp"
#include "../net/network_engine.hpp"
#include "../media/session_recorder.hpp"
#include "../media/session_recording_player.hpp"
#include "../control/session_tab_manager.hpp"
#include "../control/shortcut_manager.hpp"
#include "../core/qr_matrix.hpp"
#include "../capture/session_rewind_buffer.hpp"
#include "../capture/privacy_cloak_manager.hpp"
#include "notification_manager.hpp"
#include "../net/updater.hpp"

#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <functional>
#include <memory>

#include "canvas.hpp"
#include "app_ui.hpp"
#include "platform_shell.hpp"
#if defined(_WIN32)
#include "canvas_d2d.hpp"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#endif

namespace cppdesk {

#if defined(_WIN32)
static constexpr UINT WM_DESK_UPDATE_CHECK_DONE = WM_USER + 102;
static constexpr UINT WM_APP_FRAME_READY        = WM_APP + 101;
static constexpr UINT WM_APP_SCRATCHPAD_UPDATE  = WM_APP + 102;
static constexpr UINT WM_APP_COMMAND_PALETTE    = WM_APP + 103;
static constexpr UINT WM_APP_UPDATE_PROGRESS    = WM_APP + 104;

inline D2D1_COLOR_F toD2D(const ui::Color& c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}
#endif

#if defined(_WIN32)

class CppDeskWindow {
public:
    CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget = "");
    ~CppDeskWindow();

    bool create(HINSTANCE hInstance = nullptr, int nCmdShow = 1);
    // Renders every fixed UI state offscreen to <outDir>/<state>.bmp and returns how many were written.
    // Used to check that UI refactors leave the pixels unchanged.
    // With record set, each state is drawn through a RecordingCanvas and saved as <state>.ops.txt instead.
    static int runUiSnapshots(IdentityManager& identity, NetworkEngine& network, const std::wstring& outDir,
                              bool record = false);
    int messageLoop();

private:
    friend class WinShell;
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Direct2D / DirectWrite lifecycle
    bool initGraphics();
    void discardDeviceResources();
    void releaseGraphics();
    void applyWindowThemeAttribute();
    void onPaint();
    bool loadAppIconPixels(std::vector<uint8_t>& bgra, int& width, int& height);
    void onDropFiles(HDROP hDrop);
    bool setFullscreenWindow(bool on);
    void restoreFromTray(NotificationType contextType = NotificationType::GeneralInfo);

    // Layout and drawing are in DIPs; Win32 sizes and mouse coordinates are physical pixels.
    float toDip(int px) const { return pxToDip(static_cast<float>(px), dpi_); }
    D2D1_SIZE_F clientSizeDips() const;
    D2D1_SIZE_U clientSizePx() const;

    IdentityManager&        identity_;
    NetworkEngine&          network_;
    HWND                    hwnd_ = nullptr;
    unsigned                dpi_ = 96;
    WINDOWPLACEMENT         savedWindowPlacement_{};
    int64_t                 qpcFreq_ = 0;
    int64_t                 lastQpcCounter_ = 0;
    textinput::Utf16Assembler charAssembler_;           // joins WM_CHAR surrogate pairs

    ID2D1Factory*           d2dFactory_ = nullptr;
    IDWriteFactory*         dwriteFactory_ = nullptr;
    ID2D1RenderTarget*      renderTarget_ = nullptr;   // the window target, or an offscreen one for snapshots
    ID2D1HwndRenderTarget*  hwndTarget_ = nullptr;     // same object as renderTarget_ when drawing to the window
    bool                    snapshotMode_ = false;     // drawing offscreen at a fixed size, no window
    static constexpr float  kSnapshotWidth = 1280.0f;
    static constexpr float  kSnapshotHeight = 800.0f;
    std::unique_ptr<ui::D2DCanvas> d2dCanvas_;         // the window's Direct2D canvas
    std::unique_ptr<ui::PlatformShell> shell_;         // WinShell for this window
    std::unique_ptr<AppUi>  app_;                      // every screen
};

#elif defined(__APPLE__)

struct CppDeskWindowMacImpl;

class CppDeskWindow {
public:
    CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget = "");
    ~CppDeskWindow();

    bool create(void* displayContext = nullptr, int showCommand = 1);
    int messageLoop();
    void close();
    // Renders every fixed UI state offscreen to <outDir>/<state>.png and returns how many were written.
    // Needs no window server.
    static int runUiSnapshots(IdentityManager& identity, NetworkEngine& network, const std::string& outDir);

    // One loop iteration: waits up to timeoutMs for events, posted work, the tick or a due frame, and handles
    // them. messageLoop() calls it until it returns false (the window closed).
    bool processEvents(int timeoutMs);
    // Thread-safe: runs fn on the UI thread, waking the loop.
    void post(std::function<void()> fn);
    // Thread-safe: closes the window as if the user had (Ctrl+C or SIGTERM in a terminal, Quit in the menu).
    void requestQuit();
    // The screens this window hosts.
    AppUi& app();
    // The NSWindow (nullptr before create()).
    void* nativeWindow() const;
    // The user asked to restart; main re-executes the app after the loop.
    bool relaunchRequested() const;

private:
    IdentityManager& identity_;
    NetworkEngine& network_;
    std::string autoConnectTarget_;
    std::unique_ptr<CppDeskWindowMacImpl> pImpl_;
};

#elif defined(__linux__) || defined(__unix__)

struct CppDeskWindowLinuxImpl;

class CppDeskWindow {
public:
    CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget = "");
    ~CppDeskWindow();

    bool create(void* displayContext = nullptr, int showCommand = 1);
    int messageLoop();
    void close();
    // Renders every fixed UI state offscreen to <outDir>/<state>.png and returns how many were written.
    // Needs no X display.
    static int runUiSnapshots(IdentityManager& identity, NetworkEngine& network, const std::string& outDir);

    // One loop iteration: waits up to timeoutMs for X events, posted work, the tick or a due frame, and handles
    // them. messageLoop() calls it until it returns false (the window closed).
    bool processEvents(int timeoutMs);
    // Thread-safe: runs fn on the UI thread, waking the loop.
    void post(std::function<void()> fn);
    // Thread-safe: closes the window as if the user had (Ctrl+C or SIGTERM in a terminal).
    void requestQuit();
    // The screens this window hosts.
    AppUi& app();
    // The X window id (0 before create()).
    unsigned long nativeWindow() const;
    // The user asked to restart; main re-executes the app after the loop.
    bool relaunchRequested() const;

private:
    IdentityManager& identity_;
    NetworkEngine& network_;
    std::string autoConnectTarget_;
    std::unique_ptr<CppDeskWindowLinuxImpl> pImpl_;
};

#endif

} // namespace cppdesk
