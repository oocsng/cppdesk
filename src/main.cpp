#include "core/protocol.hpp"
#include "core/crypto_identity.hpp"
#include "net/network_engine.hpp"
#include "ui/ui_window.hpp"
#include "control/shortcut_manager.hpp"
#include "net/updater.hpp"

#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <timeapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif
#include <iostream>
#endif

#include <string>
#include <vector>
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

// A fixed identity so every --ui-snapshot run draws the same screens: a known desk ID and session code.
static void prepareSnapshotIdentity(cppdesk::IdentityManager& id) {
    std::error_code ec;
    std::filesystem::remove(id.configFilePath(), ec);
    {
        std::ofstream cfg(id.configFilePath(), std::ios::trunc);
        cfg << "desk_id=482910375\n" << "totp_secret=JBSWY3DPEHPK3PXP\n";
    }
    id.loadOrCreate();
    id.setSessionCode("k7m2qp");
}

#if defined(_WIN32)

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int nCmdShow) {
    // Request 1ms scheduler timer resolution for precise 60 FPS frame pacing and jitter-free sleep
    struct MultimediaTimerScope {
        MultimediaTimerScope() { timeBeginPeriod(1); }
        ~MultimediaTimerScope() { timeEndPeriod(1); }
    } mmTimerScope;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);

    int instanceId = 1;
    std::wstring uiSnapshotDir;
    bool uiSnapshotRecord = false;
    bool headlessRelayOnly = false;
    uint16_t relayPort = cppdesk::DEFAULT_RELAY_PORT;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--service-install" || arg == L"--service-uninstall" ||
                arg == L"--service-start" || arg == L"--service-stop" || arg == L"--service-run") {
                // Windows background service mode is not supported (ServiceMain stub disabled).
                LocalFree(argv);
                OleUninitialize();
                return 1;
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
            } else if (arg == L"--ui-snapshot" && i + 1 < argc) {
                uiSnapshotDir = argv[++i];
            } else if (arg == L"--record") {
                uiSnapshotRecord = true;
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

    if (!uiSnapshotDir.empty()) {
        // Fixed identity so every run draws the same pixels: its own instance, a known desk ID and session code.
        constexpr int kSnapshotInstance = 4243;
        cppdesk::IdentityManager snapIdentity(kSnapshotInstance);
        prepareSnapshotIdentity(snapIdentity);
        cppdesk::NetworkEngine snapNetwork(snapIdentity);
        int written = cppdesk::CppDeskWindow::runUiSnapshots(snapIdentity, snapNetwork, uiSnapshotDir, uiSnapshotRecord);
        std::error_code ec;
        std::filesystem::remove(snapIdentity.configFilePath(), ec);
        OleUninitialize();
        return written > 0 ? 0 : 1;
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
        OleUninitialize();
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
    const bool shouldRelaunch = window.relaunchRequested();
    network.stop();
    if (hMutex) CloseHandle(hMutex);
    OleUninitialize();

    // Restart after releasing the single-instance mutex so the new copy does not exit on ERROR_ALREADY_EXISTS.
    if (shouldRelaunch) {
        std::wstring exePath(1024, L'\0');
        DWORD len = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
        if (len > 0 && len < exePath.size()) {
            exePath.resize(len);
            std::wstring cmdLine = GetCommandLineW();
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(exePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
    }

    return exitCode;
}

#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)

static std::atomic<bool> g_shutdownRequested{false};

static void handlePosixSignal(int sig) {
    (void)sig;
    g_shutdownRequested.store(true);
}

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE to avoid unhandled socket closures killing process
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, handlePosixSignal);
    std::signal(SIGTERM, handlePosixSignal);



    int instanceId = 1;
    bool headlessRelayOnly = false;
    bool headlessHostOnly = false;
    uint16_t relayPort = cppdesk::DEFAULT_RELAY_PORT;
    std::string connectTarget;
    std::string uiSnapshotDir;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ui-snapshot" && i + 1 < argc) {
            uiSnapshotDir = argv[++i];
        } else if (arg == "--instance" && i + 1 < argc) {
            try {
                instanceId = std::stoi(argv[++i]);
            } catch (...) {}
        } else if (arg == "--connect" && i + 1 < argc) {
            connectTarget = argv[++i];
        } else if (arg == "--daemon" || arg == "--service-run" || arg == "--host-only") {
            headlessHostOnly = true;
        } else if (arg == "--relay-server") {
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

    // Parse startup connect target directly from argv (avoiding UTF-8 sign-extension and quote-splitting issues).
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string candidate;
        if ((arg == "--connect" || arg == "-c") && i + 1 < argc) {
            candidate = argv[++i];
        } else if (arg.rfind("--connect=", 0) == 0) {
            candidate = arg.substr(10);
        } else if (arg.rfind("cppdesk://", 0) == 0) {
            candidate = arg.substr(10);
            while (!candidate.empty() && (candidate.back() == '/' || candidate.back() == '\\')) {
                candidate.pop_back();
            }
        }
        if (!candidate.empty()) {
            const std::string clean = cppdesk::ShortcutManager::sanitizeDeskId(candidate);
            if (clean.size() == 9) {
                connectTarget = clean;
                break;
            }
        }
    }

#if defined(__linux__) || defined(__APPLE__)
    if (!uiSnapshotDir.empty()) {
        constexpr int kSnapshotInstance = 4243;
        cppdesk::IdentityManager snapIdentity(kSnapshotInstance);
        prepareSnapshotIdentity(snapIdentity);
        cppdesk::NetworkEngine snapNetwork(snapIdentity);
        int written = cppdesk::CppDeskWindow::runUiSnapshots(snapIdentity, snapNetwork, uiSnapshotDir);
        std::error_code ec;
        std::filesystem::remove(snapIdentity.configFilePath(), ec);
        return written > 0 ? 0 : 1;
    }
#endif

    if (headlessRelayOnly) {
        cppdesk::RelayServer server;
        if (!server.start(relayPort)) {
            std::cerr << "Failed to start relay server on port " << relayPort << "\n";
            return 1;
        }
        std::cout << "CppDesk Relay Server running on port " << relayPort << "...\n";
        while (server.isRunning() && !g_shutdownRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        server.stop();
        return 0;
    }

    // Single-instance protection via POSIX advisory file locking in a user-private directory (never unlinked).
    std::string lockDir;
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && xdg[0] == '/') {
        lockDir = std::string(xdg) + "/cppdesk";
    } else if (const char* home = std::getenv("HOME"); home && home[0] == '/') {
        lockDir = std::string(home) + "/.cppdesk";
    }
    int lockFd = -1;
    if (!lockDir.empty()) {
        ::mkdir(lockDir.c_str(), 0700);
        ::chmod(lockDir.c_str(), 0700);
        std::string lockPath = lockDir + "/single_instance_" + std::to_string(instanceId) + ".lock";
        lockFd = open(lockPath.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st{};
        if (lockFd < 0 || fstat(lockFd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            std::cout << "Another instance of CppDesk (instance " << instanceId << ") is already running.\n";
            if (lockFd >= 0) close(lockFd);
            return 0;
        }
    }

    cppdesk::IdentityManager identity(instanceId);
    identity.loadOrCreate();

    cppdesk::NetworkEngine network(identity);
    network.start();

#if defined(__linux__) || defined(__unix__)
    if (!std::getenv("DISPLAY")) {
        setenv("DISPLAY", ":0", 0);
    }
#endif

    if (headlessHostOnly) {
        std::cout << "CppDesk Host Daemon running (ID: " << identity.deskId() << ")...\n";
        while (network.isRunning() && !g_shutdownRequested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        network.stop();
        if (lockFd >= 0) {
            flock(lockFd, LOCK_UN);
            close(lockFd);
        }
        return 0;
    }

    cppdesk::CppDeskWindow window(identity, network, connectTarget);
    if (!window.create()) {
        std::cerr << "Failed to create CppDesk window.\n";
        network.stop();
        if (lockFd >= 0) {
            flock(lockFd, LOCK_UN);
            close(lockFd);
        }
        return 1;
    }

#if defined(__linux__) || defined(__APPLE__)
    // The signal handler only sets a flag; close the window from here so Ctrl+C and SIGTERM end the app.
    std::atomic<bool> loopDone{false};
    std::thread signalWatcher([&] {
        while (!loopDone.load()) {
            if (g_shutdownRequested.exchange(false)) window.requestQuit();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    int exitCode = window.messageLoop();
    loopDone = true;
    signalWatcher.join();
#else
    int exitCode = window.messageLoop();
#endif
    network.stop();

    if (lockFd >= 0) {
        flock(lockFd, LOCK_UN);
        close(lockFd);
    }

#if defined(__linux__)
    // Restart in place once the single-instance lock is released (a copy started earlier would see it held).
    if (window.relaunchRequested()) execv("/proc/self/exe", argv);
#elif defined(__APPLE__)
    if (window.relaunchRequested()) {
        char self[PATH_MAX] = {};
        uint32_t size = sizeof(self);
        if (_NSGetExecutablePath(self, &size) == 0) execv(self, argv);
    }
#endif
    return exitCode;
}

#endif
