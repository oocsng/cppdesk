#include "core/protocol.hpp"
#include "core/crypto_identity.hpp"
#include "net/network_engine.hpp"
#include "ui/ui_window.hpp"

#include "control/windows_service_manager.hpp"
#include "control/shortcut_manager.hpp"
#include "net/updater.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <timeapi.h>

#include <string>
#include <vector>
#include <cstdio>
#include <chrono>
#include <thread>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int nCmdShow) {
    // Request 1ms scheduler timer resolution for precise 60 FPS frame pacing and jitter-free sleep
    struct MultimediaTimerScope {
        MultimediaTimerScope() { timeBeginPeriod(1); }
        ~MultimediaTimerScope() { timeEndPeriod(1); }
    } mmTimerScope;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);

    int instanceId = 1;
    bool headlessRelayOnly = false;
    uint16_t relayPort = cppdesk::DEFAULT_RELAY_PORT;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--service-install") {
                bool ok = cppdesk::WindowsServiceManager::installService();
                LocalFree(argv);
                OleUninitialize();
                return ok ? 0 : 1;
            } else if (arg == L"--service-uninstall") {
                bool ok = cppdesk::WindowsServiceManager::uninstallService();
                LocalFree(argv);
                OleUninitialize();
                return ok ? 0 : 1;
            } else if (arg == L"--service-start") {
                bool ok = cppdesk::WindowsServiceManager::startService();
                LocalFree(argv);
                OleUninitialize();
                return ok ? 0 : 1;
            } else if (arg == L"--service-stop") {
                bool ok = cppdesk::WindowsServiceManager::stopService();
                LocalFree(argv);
                OleUninitialize();
                return ok ? 0 : 1;
            } else if (arg == L"--service-run") {
                int res = cppdesk::WindowsServiceManager::runServiceDispatcher();
                LocalFree(argv);
                OleUninitialize();
                return res;
            } else if (arg == L"--update-target" && i + 1 < argc) {
                std::wstring targetExe = argv[++i];
                DWORD waitPid = 0;
                for (int j = 1; j < argc; ++j) {
                    if (std::wstring(argv[j]) == L"--wait-pid" && j + 1 < argc) {
                        try {
                            waitPid = static_cast<DWORD>(std::stoul(argv[j + 1]));
                        } catch (...) {}
                        break;
                    }
                }
                LocalFree(argv);
                OleUninitialize();
                return cppdesk::AutoUpdater::applySelfUpdateAndRestart(targetExe, waitPid);
            } else if (arg == L"--instance" && i + 1 < argc) {
                try {
                    instanceId = std::stoi(argv[++i]);
                } catch (...) {}
            } else if (arg == L"--relay-server") {
                headlessRelayOnly = true;
                if (i + 1 < argc) {
                    try {
                        int p = std::stoi(argv[i + 1]);
                        if (p > 0 && p <= 65535) {
                            relayPort = static_cast<uint16_t>(p);
                            ++i;
                        }
                    } catch (...) {}
                }
            }
        }
        LocalFree(argv);
    }

    if (headlessRelayOnly) {
        cppdesk::RelayServer server;
        if (!server.start(relayPort)) {
            return 1;
        }
        while (server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }

    // Clean up temporary update binary if left behind from previous self-update
    wchar_t tempDir[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempDir) > 0) {
        std::wstring updateTemp = std::wstring(tempDir) + L"CppDesk_update.exe";
        DeleteFileW(updateTemp.c_str());
    }

    // Enforce single-instance per instanceId to prevent zombie instances and duplicate tray icons
    std::wstring mutexName = L"Local\\CppDesk_SingleInstance_Mutex_" + std::to_wstring(instanceId);
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existingHwnd = FindWindowW(L"CppDeskMainWindowClass", nullptr);
        if (existingHwnd) {
            if (IsIconic(existingHwnd)) {
                ShowWindow(existingHwnd, SW_RESTORE);
            } else {
                ShowWindow(existingHwnd, SW_SHOW);
            }
            SetForegroundWindow(existingHwnd);
        }
        CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }

    cppdesk::IdentityManager identity(instanceId);
    identity.loadOrCreate();

    cppdesk::NetworkEngine network(identity);
    network.start();

    std::string autoConnectTarget = cppdesk::ShortcutManager::parseStartupConnectTarget(GetCommandLineW());
    cppdesk::CppDeskWindow window(identity, network, autoConnectTarget);
    if (!window.create(hInstance, nCmdShow)) {
        network.stop();
        if (hMutex) CloseHandle(hMutex);
        OleUninitialize();
        return 1;
    }

    int exitCode = window.messageLoop();
    network.stop();
    if (hMutex) CloseHandle(hMutex);
    OleUninitialize();

    // Absolute zero background residue: terminate all process threads immediately
    ExitProcess(static_cast<UINT>(exitCode));
    return exitCode;
}
