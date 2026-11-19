#include "windows_service_manager.hpp"
#include "../core/crypto_identity.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include <accctrl.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cppdesk {

namespace {

static constexpr uint32_t REBOOT_TOKEN_MAGIC = 0x43505052; // "CPPR"

static std::mutex   g_rebootTokenMutex;
static std::wstring g_testTokenDir;

bool constantTimeEquals32(const std::array<uint8_t, 32>& a, const std::array<uint8_t, 32>& b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0;
}

bool getCurrentUserSid(std::vector<uint8_t>& outSidBytes, std::wstring& outSidString) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(hToken);
        return false;
    }
    std::vector<uint8_t> buf(needed);
    bool ok = false;
    if (GetTokenInformation(hToken, TokenUser, buf.data(), needed, &needed)) {
        TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
        DWORD sidLen = GetLengthSid(tu->User.Sid);
        outSidBytes.assign(reinterpret_cast<uint8_t*>(tu->User.Sid),
                           reinterpret_cast<uint8_t*>(tu->User.Sid) + sidLen);
        LPWSTR sidStr = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &sidStr) && sidStr) {
            outSidString = sidStr;
            LocalFree(sidStr);
            ok = true;
        }
    }
    CloseHandle(hToken);
    return ok;
}

bool buildStrictSecurityDescriptor(bool testOverride, PSECURITY_DESCRIPTOR* outSD) {
    *outSD = nullptr;
    std::wstring sddl;
    if (testOverride) {
        std::vector<uint8_t> sidBytes;
        std::wstring sidStr;
        if (!getCurrentUserSid(sidBytes, sidStr) || sidStr.empty()) {
            return false;
        }
        // Protected DACL: SYSTEM and current user SID only.
        sddl = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;" + sidStr + L")";
    } else {
        // Production protected DACL: SYSTEM and Built-in Administrators only.
        sddl = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)";
    }
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl.c_str(), SDDL_REVISION_1, outSD, nullptr) != FALSE;
}

bool isAclStrictlyRestricted(const std::wstring& path, bool testOverride) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;

    PSID pOwner = nullptr;
    PACL pDacl = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    DWORD err = GetNamedSecurityInfoW(
        path.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
        &pOwner,
        nullptr,
        &pDacl,
        nullptr,
        &pSD
    );
    if (err != ERROR_SUCCESS || !pSD) {
        if (pSD) LocalFree(pSD);
        return false;
    }

    auto cleanup = [&]() { LocalFree(pSD); };

    if (!pDacl || !pOwner) {
        cleanup();
        return false;
    }

    // Build allowed SIDs: SYSTEM + (current user in testOverride, Built-in Administrators in production)
    BYTE sysSidBuf[SECURITY_MAX_SID_SIZE] = {};
    DWORD sysSidSize = sizeof(sysSidBuf);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sysSidBuf, &sysSidSize)) {
        cleanup();
        return false;
    }

    BYTE adminSidBuf[SECURITY_MAX_SID_SIZE] = {};
    DWORD adminSidSize = sizeof(adminSidBuf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSidBuf, &adminSidSize)) {
        cleanup();
        return false;
    }

    std::vector<uint8_t> curUserSid;
    std::wstring curUserSidStr;
    if (testOverride && !getCurrentUserSid(curUserSid, curUserSidStr)) {
        cleanup();
        return false;
    }

    auto isAllowedSid = [&](PSID sid) -> bool {
        if (!sid || !IsValidSid(sid)) return false;
        if (EqualSid(sid, sysSidBuf)) return true;
        if (testOverride) {
            return !curUserSid.empty() && EqualSid(sid, curUserSid.data());
        }
        return EqualSid(sid, adminSidBuf) != FALSE;
    };

    // An elevated admin's new files are owned by Administrators rather than the user; that owner is trusted too.
    if (!isAllowedSid(pOwner) && !EqualSid(pOwner, adminSidBuf)) {
        cleanup();
        return false;
    }

    ACL_SIZE_INFORMATION aclInfo{};
    if (!GetAclInformation(pDacl, &aclInfo, sizeof(aclInfo), AclSizeInformation) || aclInfo.AceCount == 0) {
        cleanup();
        return false;
    }

    for (DWORD i = 0; i < aclInfo.AceCount; ++i) {
        LPVOID pAce = nullptr;
        if (!GetAce(pDacl, i, &pAce) || !pAce) {
            cleanup();
            return false;
        }
        auto* hdr = static_cast<PACE_HEADER>(pAce);
        if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            PSID aceSid = &static_cast<ACCESS_ALLOWED_ACE*>(pAce)->SidStart;
            if (!isAllowedSid(aceSid)) {
                cleanup();
                return false;
            }
        } else if (hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
            // Deny ACEs only narrow access, safe to ignore.
        } else {
            // Unexpected allow-object or unknown ACE type: fail closed.
            cleanup();
            return false;
        }
    }

    cleanup();
    return true;
}

bool readRebootTokenFile(const std::wstring& filePath, std::string& outToken, uint64_t& outDeskId, uint64_t& outExpireMs) {
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        0, // Exclusive access
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < 24 || fileSize.QuadPart > 1024) {
        CloseHandle(hFile);
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr);
    CloseHandle(hFile);
    if (!ok || bytesRead != buf.size()) {
        CryptoUtils::secureZero(buf.data(), buf.size());
        return false;
    }

    uint32_t magic = 0;
    uint32_t tokenLen = 0;
    std::memcpy(&magic, buf.data(), sizeof(magic));
    std::memcpy(&tokenLen, buf.data() + 4, sizeof(tokenLen));
    size_t expectedSize = sizeof(magic) + sizeof(tokenLen) + tokenLen + sizeof(outDeskId) + sizeof(outExpireMs);
    if (magic != REBOOT_TOKEN_MAGIC || tokenLen == 0 || tokenLen > 256 || buf.size() != expectedSize) {
        CryptoUtils::secureZero(buf.data(), buf.size());
        return false;
    }

    outToken.assign(reinterpret_cast<const char*>(buf.data() + 8), tokenLen);
    std::memcpy(&outDeskId, buf.data() + 8 + tokenLen, sizeof(outDeskId));
    std::memcpy(&outExpireMs, buf.data() + 8 + tokenLen + sizeof(outDeskId), sizeof(outExpireMs));
    CryptoUtils::secureZero(buf.data(), buf.size());
    return true;
}

bool runBcdedit(const std::wstring& args) {
    wchar_t sysDir[MAX_PATH] = {};
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    std::wstring exePath = std::wstring(sysDir, len) + L"\\bcdedit.exe";
    std::wstring cmdLine = L"\"" + exePath + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            exePath.c_str(),
            cmdBuf.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        return false;
    }

    DWORD waitRes = WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1;
    bool ok = (waitRes == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

} // namespace

// ---------------- Service Management ----------------

bool WindowsServiceManager::isServiceInstalled() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_STATUS);
    bool installed = (hSvc != nullptr);
    if (hSvc) CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return installed;
}

bool WindowsServiceManager::isServiceRunning() {
    return getServiceState() == ServiceStatusState::Running;
}

ServiceStatusState WindowsServiceManager::getServiceState() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return ServiceStatusState::NotInstalled;

    SC_HANDLE hSvc = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!hSvc) {
        CloseServiceHandle(hSCM);
        return ServiceStatusState::NotInstalled;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD bytesNeeded = 0;
    ServiceStatusState state = ServiceStatusState::Stopped;

    if (QueryServiceStatusEx(hSvc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytesNeeded)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            state = ServiceStatusState::Running;
        } else if (ssp.dwCurrentState == SERVICE_START_PENDING || ssp.dwCurrentState == SERVICE_STOP_PENDING) {
            state = ServiceStatusState::Pending;
        } else {
            state = ServiceStatusState::Stopped;
        }
    }

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return state;
}

bool WindowsServiceManager::installService(const std::wstring& customExePath) {
    (void)customExePath;
    std::fprintf(stderr, "[CppDesk] Windows Service install is not supported in this build.\n");
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

bool WindowsServiceManager::uninstallService() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSCM) return false;

    SC_HANDLE hSvc = OpenServiceW(hSCM, SERVICE_NAME, DELETE | SERVICE_STOP);
    if (!hSvc) {
        CloseServiceHandle(hSCM);
        return false;
    }

    SERVICE_STATUS ss{};
    ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
    bool ok = (DeleteService(hSvc) != 0);

    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return ok;
}

bool WindowsServiceManager::startService() {
    std::fprintf(stderr, "[CppDesk] Windows Service start is not supported in this build.\n");
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

bool WindowsServiceManager::stopService() {
    std::fprintf(stderr, "[CppDesk] Windows Service stop is not supported in this build.\n");
    SetLastError(ERROR_NOT_SUPPORTED);
    return false;
}

int WindowsServiceManager::runServiceDispatcher() {
    std::fprintf(stderr, "[CppDesk] Windows Service mode is not supported in this build.\n");
    SetLastError(ERROR_NOT_SUPPORTED);
    return 1;
}

// ---------------- Reboot Token Management ----------------

void WindowsServiceManager::setRebootTokenDirForTest(const std::wstring& dirPath) {
    std::lock_guard<std::mutex> lk(g_rebootTokenMutex);
    g_testTokenDir = dirPath;
}

std::wstring WindowsServiceManager::getRebootTokenDirPath(bool& outTestOverride) {
    if (!g_testTokenDir.empty()) {
        outTestOverride = true;
        return g_testTokenDir;
    }
    outTestOverride = false;
    wchar_t commonAppDir[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, commonAppDir))) {
        return (std::filesystem::path(commonAppDir) / L"CppDesk").wstring();
    }
    return L"";
}

std::string WindowsServiceManager::generateRebootTokenHex() {
    std::array<uint8_t, 32> randBytes{};
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        randBytes.data(),
        static_cast<ULONG>(randBytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG
    );
    if (status < 0) {
        CryptoUtils::secureZero(randBytes.data(), randBytes.size());
        return "";
    }
    std::string hex = CryptoUtils::toHex(randBytes.data(), randBytes.size());
    CryptoUtils::secureZero(randBytes.data(), randBytes.size());
    return hex;
}

bool WindowsServiceManager::saveRebootToken(const std::string& tokenHex, uint64_t callerDeskId, uint32_t validSeconds) {
    if (tokenHex.empty() || tokenHex.size() > 256) return false;

    std::lock_guard<std::mutex> lk(g_rebootTokenMutex);
    bool testOverride = false;
    std::wstring dirPath = getRebootTokenDirPath(testOverride);
    if (dirPath.empty()) return false;

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!buildStrictSecurityDescriptor(testOverride, &pSD) || !pSD) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = pSD;
    sa.bInheritHandle = FALSE;

    DWORD dirAttrs = GetFileAttributesW(dirPath.c_str());
    if (dirAttrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(dirPath.c_str(), &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
            LocalFree(pSD);
            return false;
        }
    }

    // Refuse pre-existing directories that do not have a strictly restricted DACL.
    if (!isAclStrictlyRestricted(dirPath, testOverride)) {
        LocalFree(pSD);
        return false;
    }

    std::wstring filePath = (std::filesystem::path(dirPath) / L"reboot_token.bin").wstring();
    if (GetFileAttributesW(filePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!isAclStrictlyRestricted(filePath, testOverride)) {
            LocalFree(pSD);
            return false;
        }
        if (!DeleteFileW(filePath.c_str())) {
            LocalFree(pSD);
            return false;
        }
    }

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_WRITE,
        0,
        &sa,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    LocalFree(pSD);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    uint32_t magic = REBOOT_TOKEN_MAGIC;
    uint32_t tokenLen = static_cast<uint32_t>(tokenHex.size());
    uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    uint64_t expireMs = nowMs + static_cast<uint64_t>(validSeconds) * 1000ULL;

    std::vector<uint8_t> payload;
    payload.reserve(sizeof(magic) + sizeof(tokenLen) + tokenLen + sizeof(callerDeskId) + sizeof(expireMs));
    auto append = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        payload.insert(payload.end(), b, b + n);
    };
    append(&magic, sizeof(magic));
    append(&tokenLen, sizeof(tokenLen));
    append(tokenHex.data(), tokenLen);
    append(&callerDeskId, sizeof(callerDeskId));
    append(&expireMs, sizeof(expireMs));

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    if (ok) FlushFileBuffers(hFile);
    CloseHandle(hFile);
    CryptoUtils::secureZero(payload.data(), payload.size());

    if (!ok || written != payload.size() || !isAclStrictlyRestricted(filePath, testOverride)) {
        DeleteFileW(filePath.c_str());
        return false;
    }
    return true;
}

bool WindowsServiceManager::validateAndConsumeRebootToken(const std::string& tokenHex, uint64_t callerDeskId) {
    if (tokenHex.empty()) return false;

    std::lock_guard<std::mutex> lk(g_rebootTokenMutex);
    bool testOverride = false;
    std::wstring dirPath = getRebootTokenDirPath(testOverride);
    if (dirPath.empty()) return false;

    std::wstring filePath = (std::filesystem::path(dirPath) / L"reboot_token.bin").wstring();
    if (!isAclStrictlyRestricted(dirPath, testOverride) ||
        !isAclStrictlyRestricted(filePath, testOverride)) {
        return false;
    }

    std::string storedToken;
    uint64_t storedDeskId = 0;
    uint64_t expireMs = 0;
    bool readOk = readRebootTokenFile(filePath, storedToken, storedDeskId, expireMs);

    // Always consume/invalidate the token on any attempt (prevents brute force and replay).
    DeleteFileW(filePath.c_str());

    if (!readOk) {
        return false;
    }

    uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    auto storedHash = CryptoUtils::sha256(storedToken);
    auto givenHash = CryptoUtils::sha256(tokenHex);
    bool tokenMatches = constantTimeEquals32(storedHash, givenHash) && (storedToken.size() == tokenHex.size());
    CryptoUtils::secureZero(storedToken.data(), storedToken.size());
    CryptoUtils::secureZero(storedHash.data(), storedHash.size());
    CryptoUtils::secureZero(givenHash.data(), givenHash.size());

    if (nowMs > expireMs || storedDeskId != callerDeskId || !tokenMatches) {
        return false;
    }

    clearSafeBootIfActive();
    return true;
}

bool WindowsServiceManager::hasPendingRebootToken(uint64_t* outCallerDeskId) {
    std::lock_guard<std::mutex> lk(g_rebootTokenMutex);
    bool testOverride = false;
    std::wstring dirPath = getRebootTokenDirPath(testOverride);
    if (dirPath.empty()) return false;

    std::wstring filePath = (std::filesystem::path(dirPath) / L"reboot_token.bin").wstring();
    if (!isAclStrictlyRestricted(dirPath, testOverride) ||
        !isAclStrictlyRestricted(filePath, testOverride)) {
        return false;
    }

    std::string storedToken;
    uint64_t storedDeskId = 0;
    uint64_t expireMs = 0;
    bool readOk = readRebootTokenFile(filePath, storedToken, storedDeskId, expireMs);
    CryptoUtils::secureZero(storedToken.data(), storedToken.size());
    if (!readOk) return false;

    uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (nowMs > expireMs) {
        return false;
    }
    if (outCallerDeskId) {
        *outCallerDeskId = storedDeskId;
    }
    return true;
}

void WindowsServiceManager::clearRebootToken() {
    std::lock_guard<std::mutex> lk(g_rebootTokenMutex);
    bool testOverride = false;
    std::wstring dirPath = getRebootTokenDirPath(testOverride);
    if (dirPath.empty()) return;
    std::wstring filePath = (std::filesystem::path(dirPath) / L"reboot_token.bin").wstring();
    DeleteFileW(filePath.c_str());
}

bool WindowsServiceManager::clearSafeBootIfActive() {
    if (GetSystemMetrics(SM_CLEANBOOT) == 0) {
        return true; // Normal boot, nothing to clear.
    }
    return runBcdedit(L"/deletevalue {current} safeboot");
}

bool WindowsServiceManager::initiateHostReboot(uint32_t countdownSeconds, bool safeMode) {
    // Enable SE_SHUTDOWN_NAME privilege
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp{};
        LookupPrivilegeValueA(nullptr, "SeShutdownPrivilege", &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(hToken);
    }

    if (safeMode) {
        // Configure safeboot network using full System32\bcdedit.exe path and wait for exit code.
        if (!runBcdedit(L"/set {current} safeboot network")) {
            return false;
        }
    }

    char msg[128];
    std::snprintf(msg, sizeof(msg), "CppDesk Remote Administration Reboot (Resuming in %u seconds)", countdownSeconds);

    BOOL res = InitiateSystemShutdownExA(
        nullptr,
        msg,
        countdownSeconds,
        TRUE,  // Force apps closed
        TRUE,  // Reboot after shutdown
        SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED
    );

    if (!res) {
        // Fallback to ExitWindowsEx
        res = ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                            SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG);
    }
    return (res != FALSE);
}

} // namespace cppdesk
