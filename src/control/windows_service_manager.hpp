#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cppdesk {

enum class ServiceStatusState : uint8_t {
    NotInstalled = 0,
    Stopped      = 1,
    Running      = 2,
    Pending      = 3
};

class WindowsServiceManager {
public:
    static constexpr const wchar_t* SERVICE_NAME = L"CppDeskService";
    static constexpr const wchar_t* DISPLAY_NAME = L"CppDesk Remote Desktop Service";

    // Service Control Manager lifecycle
    static bool isServiceInstalled();
    static bool isServiceRunning();
    static ServiceStatusState getServiceState();

    static bool installService(const std::wstring& customExePath = L"");
    static bool uninstallService();
    static bool startService();
    static bool stopService();
    static int  runServiceDispatcher();

    // Remote reboot token persistence & validation
    static std::string generateRebootTokenHex();
    static bool saveRebootToken(const std::string& tokenHex, uint64_t callerDeskId, uint32_t validSeconds = 600);
    static bool validateAndConsumeRebootToken(const std::string& tokenHex, uint64_t callerDeskId);
    static bool hasPendingRebootToken(uint64_t* outCallerDeskId = nullptr);
    static void clearRebootToken();
    // Test-only override: points reboot token storage at a temp dir with current-user+SYSTEM DACL.
    // Pass empty string to restore production SYSTEM+Administrators storage.
    static void setRebootTokenDirForTest(const std::wstring& dirPath);

    // System reboot execution
    static bool initiateHostReboot(uint32_t countdownSeconds = 5, bool safeMode = false);
    static bool clearSafeBootIfActive();

private:
    static std::wstring getRebootTokenDirPath(bool& outTestOverride);
};

} // namespace cppdesk
