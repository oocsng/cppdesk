#pragma once

#if defined(_WIN32)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

#include "../core/protocol.hpp"

#include <cstdint>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace cppdesk {

// Remote Screen Blanking & Hardware DPMS Display Power Manager
class ScreenBlankManager {
public:
    static ScreenBlankManager& instance();

    ~ScreenBlankManager();

    // Win32 DPMS Power Management APIs
    static void powerDownDisplays();
    static void wakeDisplays();

#if defined(_WIN32)
#if defined(_WIN32)
    // Input Filtering Helpers for Low-Level Windows Hooks
    static bool isPhysicalEvent(DWORD flags, bool isMouse);
    static bool isPhysicalMouseInput(const MSLLHOOKSTRUCT& hookStruct);
    static bool isPhysicalKeyboardInput(const KBDLLHOOKSTRUCT& hookStruct);
#endif
#endif

    // Lifecycle
    bool engageBlanking(
        ScreenBlankMode mode = ScreenBlankMode::Unified,
        const std::string& notice = "",
        const std::string& brand = "",
        bool showDeskId = true,
        uint64_t deskId = 0);

    void disengageBlanking();

    bool isBlankActive() const;
    ScreenBlankMode currentBlankMode() const;

    // Emergency physical interrupt callback
    void setEmergencyWakeCallback(std::function<void()> cb);
    void triggerEmergencyWake();

    // Internal thread message loop
    void blankingThreadFunc();

private:
    ScreenBlankManager();
    ScreenBlankManager(const ScreenBlankManager&) = delete;
    ScreenBlankManager& operator=(const ScreenBlankManager&) = delete;

    void createCurtainWindow();
    void destroyCurtainWindow();

    std::atomic<bool> blankActive_{false};
    std::atomic<ScreenBlankMode> currentMode_{ScreenBlankMode::CurtainOnly};
#if defined(_WIN32)
    std::atomic<DWORD> blankThreadId_{0};
#else
    std::atomic<uint32_t> blankThreadId_{0};
#endif

    std::mutex stateMutex_;
    std::thread blankThread_;
    std::function<void()> emergencyWakeCallback_;

    std::mutex initMutex_;
    std::condition_variable initCv_;
    bool initReady_{false};

    std::string customNotice_;
    std::string brandName_;
    bool showDeskId_{true};
    uint64_t deskId_{0};

#if defined(_WIN32)
    HWND hwndCurtain_{nullptr};
    HHOOK hMouseHook_{nullptr};
    HHOOK hKbdHook_{nullptr};
#else
    void* hwndCurtain_{nullptr};
    void* hMouseHook_{nullptr};
    void* hKbdHook_{nullptr};
#endif
};

} // namespace cppdesk
