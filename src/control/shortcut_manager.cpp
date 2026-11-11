#include "shortcut_manager.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <shlwapi.h>
#endif

#include <vector>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace cppdesk {

std::string ShortcutManager::sanitizeDeskId(const std::string& input) {
    std::string clean;
    for (char c : input) {
        if (c >= '0' && c <= '9') {
            clean += c;
        }
    }
    if (clean.size() > 9) {
        clean = clean.substr(0, 9);
    }
    return clean;
}

bool ShortcutManager::createDesktopShortcut(const std::string& deskId, const std::string& customAlias) {
#if !defined(_WIN32)
    (void)deskId; (void)customAlias;
    return false;
#else
    std::string cleanId = sanitizeDeskId(deskId);
    if (cleanId.size() != 9) {
        return false;
    }

    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return false;
    }

    PWSTR pDesktopPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, NULL, &pDesktopPath);
    if (FAILED(hr) || !pDesktopPath) {
        if (pDesktopPath) {
            CoTaskMemFree(pDesktopPath);
            pDesktopPath = nullptr;
        }
        return false;
    }

    std::wstring desktopDir = pDesktopPath;
    if (pDesktopPath) {
        CoTaskMemFree(pDesktopPath);
        pDesktopPath = nullptr;
    }

    std::wstring safeName;
    if (!customAlias.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, customAlias.c_str(), -1, NULL, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wbuf(wlen);
            MultiByteToWideChar(CP_UTF8, 0, customAlias.c_str(), -1, wbuf.data(), wlen);
            safeName = wbuf.data();
        }
    }
    if (safeName.empty()) {
        std::wstring wId(cleanId.begin(), cleanId.end());
        safeName = L"CppDesk - " + wId;
    } else {
        safeName = L"CppDesk - " + safeName;
    }

    // Sanitize filename for Windows
    for (auto& ch : safeName) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            ch = L'_';
        }
    }

    std::wstring fullPath = desktopDir + L"\\" + safeName + L".lnk";

    HRESULT coInitHr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    IShellLinkW* psl = nullptr;
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (LPVOID*)&psl);
    bool success = false;
    if (SUCCEEDED(hr) && psl) {
        psl->SetPath(exePath);
        std::wstring args = L"--connect " + std::wstring(cleanId.begin(), cleanId.end());
        psl->SetArguments(args.c_str());
        psl->SetDescription(L"Connect to CppDesk Remote");

        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
        if (SUCCEEDED(hr) && ppf) {
            hr = ppf->Save(fullPath.c_str(), TRUE);
            if (SUCCEEDED(hr)) {
                success = true;
            }
            ppf->Release();
        }
        psl->Release();
    }

    if (SUCCEEDED(coInitHr)) {
        CoUninitialize();
    }
    return success;
#endif
}

bool ShortcutManager::isUriProtocolRegistered() {
#if !defined(_WIN32)
    return false;
#else
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\cppdesk", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool ShortcutManager::setUriProtocolRegistered(bool enable) {
    if (!enable) {
        // Delete protocol key tree
        LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\cppdesk");
        return (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }

    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return false;
    }

    HKEY hKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\cppdesk", 0, NULL, 0,
                                KEY_WRITE, NULL, &hKey, NULL);
    if (st != ERROR_SUCCESS) {
        return false;
    }

    const wchar_t* desc = L"URL:CppDesk Remote Connection";
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)desc, (DWORD)((wcslen(desc) + 1) * sizeof(wchar_t)));
    const wchar_t* emptyVal = L"";
    RegSetValueExW(hKey, L"URL Protocol", 0, REG_SZ, (const BYTE*)emptyVal, (DWORD)sizeof(wchar_t));
    RegCloseKey(hKey);

    HKEY hCmdKey = nullptr;
    st = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\cppdesk\\shell\\open\\command", 0, NULL, 0,
                         KEY_WRITE, NULL, &hCmdKey, NULL);
    if (st != ERROR_SUCCESS) {
        return false;
    }

    std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" \"%1\"";
    RegSetValueExW(hCmdKey, NULL, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hCmdKey);

    return true;
#endif
}

#if !defined(_WIN32)
// There is no URL-scheme registry to write to here.
bool ShortcutManager::setUriProtocolRegistered(bool) {
    return false;
}
#endif

std::string ShortcutManager::parseStartupConnectTarget(const std::wstring& cmdLine) {
    if (cmdLine.empty()) {
        return "";
    }
    std::vector<std::wstring> args;
#if defined(_WIN32)
    int winArgc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &winArgc);
    if (!argv || winArgc <= 0) {
        if (argv) {
            LocalFree(argv);
        }
        return "";
    }
    for (int i = 0; i < winArgc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
#else
    // Split on spaces, keeping a double-quoted argument whole.
    {
        std::wstring cur;
        bool inQuotes = false, have = false;
        for (wchar_t c : cmdLine) {
            if (c == L'"') {
                inQuotes = !inQuotes;
                have = true;
            } else if (c == L' ' && !inQuotes) {
                if (have) args.push_back(cur);
                cur.clear();
                have = false;
            } else {
                cur += c;
                have = true;
            }
        }
        if (have) args.push_back(cur);
    }
#endif
    const int argc = static_cast<int>(args.size());

    std::string targetId;

    for (int i = 0; i < argc; ++i) {
        std::wstring arg = args[i];

        // Check for --connect <id> or -c <id>
        if (arg == L"--connect" || arg == L"-c") {
            if (i + 1 < argc) {
                std::wstring val = args[i + 1];
                std::string s(val.begin(), val.end());
                std::string clean = sanitizeDeskId(s);
                if (clean.size() == 9) {
                    targetId = clean;
                    break;
                }
            }
        } else if (arg.rfind(L"--connect=", 0) == 0) {
            std::wstring val = arg.substr(10);
            std::string s(val.begin(), val.end());
            std::string clean = sanitizeDeskId(s);
            if (clean.size() == 9) {
                targetId = clean;
                break;
            }
        } else if (arg.rfind(L"cppdesk://", 0) == 0) {
            std::wstring val = arg.substr(10);
            // remove trailing slashes
            while (!val.empty() && (val.back() == L'/' || val.back() == L'\\')) {
                val.pop_back();
            }
            std::string s(val.begin(), val.end());
            std::string clean = sanitizeDeskId(s);
            if (clean.size() == 9) {
                targetId = clean;
                break;
            }
        }
    }

    return targetId;
}

} // namespace cppdesk
