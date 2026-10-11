#include "updater.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <sstream>
#include <vector>
#include <thread>
#include <cctype>

namespace cppdesk {

namespace {

std::wstring toWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring out(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &out[0], len);
    return out;
}

} // namespace

bool AutoUpdater::parseVersionTriad(const std::string& verStr, int& major, int& minor, int& patch) {
    major = 0;
    minor = 0;
    patch = 0;
    std::string s = verStr;
    size_t start = 0;
    while (start < s.size() && (std::isspace(static_cast<unsigned char>(s[start])) || s[start] == 'v' || s[start] == 'V')) {
        start++;
    }
    s = s.substr(start);
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0]))) {
        return false;
    }

    std::vector<int> parts;
    std::stringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, '.')) {
        try {
            size_t endDigit = 0;
            while (endDigit < seg.size() && std::isdigit(static_cast<unsigned char>(seg[endDigit]))) {
                endDigit++;
            }
            if (endDigit > 0) {
                parts.push_back(std::stoi(seg.substr(0, endDigit)));
            } else {
                parts.push_back(0);
            }
        } catch (...) {
            parts.push_back(0);
        }
    }
    if (parts.empty()) return false;
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    major = parts[0];
    minor = parts[1];
    patch = parts[2];
    return true;
}

bool AutoUpdater::isNewerVersion(const std::string& candidateVer, const std::string& baselineVer) {
    if (candidateVer.empty()) return false;
    int candMaj = 0, candMin = 0, candPat = 0;
    int baseMaj = 0, baseMin = 0, basePat = 0;
    if (!parseVersionTriad(candidateVer, candMaj, candMin, candPat)) return false;
    if (!parseVersionTriad(baselineVer, baseMaj, baseMin, basePat)) return false;

    if (candMaj > baseMaj) return true;
    if (candMaj < baseMaj) return false;
    if (candMin > baseMin) return true;
    if (candMin < baseMin) return false;
    return candPat > basePat;
}

std::string AutoUpdater::extractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + needle.size());
    if (colonPos == std::string::npos) return "";

    size_t quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string::npos) return "";

    std::string result;
    bool escape = false;
    for (size_t i = quoteStart + 1; i < json.size(); ++i) {
        char c = json[i];
        if (escape) {
            if (c == 'n') result += '\n';
            else if (c == 'r') result += '\r';
            else if (c == 't') result += '\t';
            else if (c == '\\') result += '\\';
            else if (c == '"') result += '"';
            else if (c == '/') result += '/';
            else result += c;
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            break;
        } else {
            result += c;
        }
    }
    return result;
}

UpdateInfo AutoUpdater::queryLatestReleaseSync(
    const std::string& repoOwner,
    const std::string& repoName,
    const std::string& currentVersion)
{
    UpdateInfo info;
    info.currentVersion = currentVersion;
    info.downloadUrl = "https://github.com/" + repoOwner + "/" + repoName + "/releases/latest";

    HINTERNET hSession = WinHttpOpen(
        L"CppDesk-Updater/3.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession) {
        info.errorMessage = "Failed to initialize WinHTTP session";
        return info;
    }

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        info.errorMessage = "Failed to connect to GitHub API";
        return info;
    }

    std::wstring path = L"/repos/" + toWide(repoOwner) + L"/" + toWide(repoName) + L"/releases/latest";

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        info.errorMessage = "Failed to open WinHTTP request";
        return info;
    }

    // Set standard GitHub API request headers
    LPCWSTR headers = L"User-Agent: CppDesk-Updater\r\nAccept: application/vnd.github.v3+json\r\n";
    BOOL bSend = WinHttpSendRequest(
        hRequest,
        headers,
        -1L,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    );

    if (!bSend || !WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        info.errorMessage = "Failed to query GitHub release response";
        return info;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX
    );

    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        info.errorMessage = "GitHub API returned HTTP " + std::to_string(statusCode);
        return info;
    }

    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buffer(bytesAvailable + 1, 0);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
            responseBody.append(buffer.data(), bytesRead);
        } else {
            break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    info.latestVersion = extractJsonString(responseBody, "tag_name");
    if (!info.latestVersion.empty() && (info.latestVersion[0] == 'v' || info.latestVersion[0] == 'V')) {
        info.latestVersion = info.latestVersion.substr(1);
    }
    std::string releaseUrl = extractJsonString(responseBody, "html_url");
    if (!releaseUrl.empty()) {
        info.downloadUrl = releaseUrl;
        info.releaseUrl = releaseUrl;
    }
    info.releaseNotes = extractJsonString(responseBody, "body");
    info.directDownloadUrl = extractAssetDownloadUrl(responseBody, "CppDesk.exe", &info.assetSizeBytes);

    if (!info.latestVersion.empty()) {
        info.checkSucceeded = true;
        info.success = true;
        info.updateRequired = isNewerVersion(info.latestVersion, currentVersion);
    } else {
        info.errorMessage = "No release tag found in GitHub API response";
    }

    return info;
}

void AutoUpdater::checkForUpdatesAsync(
    std::function<void(const UpdateInfo&)> onComplete,
    const std::string& repoOwner,
    const std::string& repoName,
    const std::string& currentVersion)
{
    std::thread([onComplete, repoOwner, repoName, currentVersion]() {
        UpdateInfo info = queryLatestReleaseSync(repoOwner, repoName, currentVersion);
        if (onComplete) {
            onComplete(info);
        }
    }).detach();
}

std::string AutoUpdater::extractAssetDownloadUrl(const std::string& json, const std::string& assetName, uint64_t* outSize) {
    if (outSize) *outSize = 0;
    if (json.empty() || assetName.empty()) return "";

    size_t pos = 0;
    while ((pos = json.find(assetName, pos)) != std::string::npos) {
        size_t objStart = json.rfind('{', pos);
        size_t objEnd = json.find('}', pos);
        if (objStart != std::string::npos && objEnd != std::string::npos && objEnd > objStart) {
            std::string assetObj = json.substr(objStart, objEnd - objStart + 1);
            std::string dlUrl = extractJsonString(assetObj, "browser_download_url");
            if (!dlUrl.empty()) {
                if (outSize) {
                    size_t sizeKey = assetObj.find("\"size\"");
                    if (sizeKey != std::string::npos) {
                        size_t col = assetObj.find(':', sizeKey);
                        if (col != std::string::npos) {
                            size_t dStart = col + 1;
                            while (dStart < assetObj.size() && (assetObj[dStart] == ' ' || assetObj[dStart] == '\t')) dStart++;
                            size_t dEnd = dStart;
                            while (dEnd < assetObj.size() && std::isdigit(static_cast<unsigned char>(assetObj[dEnd]))) dEnd++;
                            if (dEnd > dStart) {
                                try {
                                    *outSize = std::stoull(assetObj.substr(dStart, dEnd - dStart));
                                } catch (...) {
                                    *outSize = 0;
                                }
                            }
                        }
                    }
                }
                return dlUrl;
            }
        }
        pos += assetName.size();
    }
    return "";
}

void AutoUpdater::downloadUpdateAssetAsync(
    const std::string& downloadUrl,
    const std::wstring& targetPath,
    UpdateProgressCallback onProgress,
    std::function<void(bool success, const std::string& error)> onComplete)
{
    std::thread([downloadUrl, targetPath, onProgress, onComplete]() {
        if (downloadUrl.empty() || targetPath.empty()) {
            if (onComplete) onComplete(false, "Invalid URL or target path");
            return;
        }

        std::wstring wUrl = toWide(downloadUrl);
        URL_COMPONENTSW urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
        urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &urlComp)) {
            if (onComplete) onComplete(false, "Failed to parse download URL");
            return;
        }

        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength + urlComp.dwExtraInfoLength);

        HINTERNET hSession = WinHttpOpen(
            L"CppDesk-Updater/3.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (!hSession) {
            if (onComplete) onComplete(false, "Failed to initialize WinHTTP session");
            return;
        }

        HINTERNET hConnect = WinHttpConnect(
            hSession,
            host.c_str(),
            urlComp.nPort,
            0
        );

        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, "Failed to connect to host: " + downloadUrl);
            return;
        }

        DWORD reqFlags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            reqFlags
        );

        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, "Failed to create WinHTTP request");
            return;
        }

        // Configure WinHTTP to automatically follow HTTP 302 redirects
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        LPCWSTR reqHeaders = L"User-Agent: CppDesk-Updater\r\nAccept: application/octet-stream\r\n";
        BOOL bSend = WinHttpSendRequest(
            hRequest,
            reqHeaders,
            -1L,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0
        );

        if (!bSend || !WinHttpReceiveResponse(hRequest, nullptr)) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, "Failed to receive HTTP response header");
            return;
        }

        DWORD statusCode = 0;
        DWORD scSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode != 200) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, "Server returned HTTP " + std::to_string(statusCode));
            return;
        }

        uint64_t totalBytes = 0;
        wchar_t clBuf[64] = {};
        DWORD clBufSize = sizeof(clBuf);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clBuf, &clBufSize, WINHTTP_NO_HEADER_INDEX)) {
            try {
                totalBytes = std::stoull(clBuf);
            } catch (...) {
                totalBytes = 0;
            }
        }

        HANDLE hFile = CreateFileW(
            targetPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, "Failed to create target file on disk");
            return;
        }

        uint64_t downloadedBytes = 0;
        auto startTime = std::chrono::steady_clock::now();
        auto lastSpeedTime = startTime;
        uint64_t lastSpeedBytes = 0;
        double currentSpeedBps = 0.0;

        constexpr DWORD BUF_SIZE = 64 * 1024;
        std::vector<uint8_t> buffer(BUF_SIZE);
        bool readSuccess = true;

        while (true) {
            DWORD bytesAvailable = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
                readSuccess = false;
                break;
            }
            if (bytesAvailable == 0) {
                break; // EOF
            }

            DWORD toRead = std::min<DWORD>(bytesAvailable, BUF_SIZE);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, buffer.data(), toRead, &bytesRead) || bytesRead == 0) {
                readSuccess = (bytesRead == 0);
                break;
            }

            DWORD written = 0;
            if (!WriteFile(hFile, buffer.data(), bytesRead, &written, nullptr) || written != bytesRead) {
                readSuccess = false;
                break;
            }

            downloadedBytes += bytesRead;

            auto now = std::chrono::steady_clock::now();
            double elapsedSinceSpeed = std::chrono::duration<double>(now - lastSpeedTime).count();
            if (elapsedSinceSpeed >= 0.25) {
                currentSpeedBps = static_cast<double>(downloadedBytes - lastSpeedBytes) / elapsedSinceSpeed;
                lastSpeedTime = now;
                lastSpeedBytes = downloadedBytes;
            }

            if (onProgress) {
                onProgress(downloadedBytes, totalBytes, currentSpeedBps);
            }
        }

        CloseHandle(hFile);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (!readSuccess || (totalBytes > 0 && downloadedBytes < totalBytes)) {
            DeleteFileW(targetPath.c_str());
            if (onComplete) onComplete(false, "Download stream truncated or failed");
            return;
        }

        if (onProgress) {
            onProgress(downloadedBytes, totalBytes, currentSpeedBps);
        }

        if (onComplete) {
            onComplete(true, "");
        }
    }).detach();
}

bool AutoUpdater::validatePeBinary(const std::wstring& filePath) {
    if (filePath.empty()) return false;

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER) + sizeof(DWORD))) {
        CloseHandle(hFile);
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &dos, sizeof(dos), &bytesRead, nullptr) || bytesRead != sizeof(dos)) {
        CloseHandle(hFile);
        return false;
    }

    if (dos.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    if (dos.e_lfanew <= 0 || static_cast<uint64_t>(dos.e_lfanew) + sizeof(DWORD) > static_cast<uint64_t>(liSize.QuadPart)) {
        CloseHandle(hFile);
        return false;
    }

    LARGE_INTEGER liSeek{};
    liSeek.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(hFile, liSeek, nullptr, FILE_BEGIN)) {
        CloseHandle(hFile);
        return false;
    }

    DWORD ntSig = 0;
    if (!ReadFile(hFile, &ntSig, sizeof(ntSig), &bytesRead, nullptr) || bytesRead != sizeof(ntSig)) {
        CloseHandle(hFile);
        return false;
    }

    CloseHandle(hFile);
    return (ntSig == IMAGE_NT_SIGNATURE);
}

int AutoUpdater::applySelfUpdateAndRestart(
    const std::wstring& sourceExePath,
    const std::wstring& targetExePath,
    uint32_t waitPid,
    bool launchAfter)
{
    if (waitPid != 0) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, waitPid);
        if (hProcess) {
            WaitForSingleObject(hProcess, 15000);
            CloseHandle(hProcess);
        }
    }
    Sleep(200);

    bool copied = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (CopyFileW(sourceExePath.c_str(), targetExePath.c_str(), FALSE)) {
            copied = true;
            break;
        }
        Sleep(250);
    }

    if (copied && launchAfter) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(targetExePath.begin(), targetExePath.end());
        cmdBuf.push_back(L'\0');
        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return copied ? 0 : 1;
}

int AutoUpdater::applySelfUpdateAndRestart(const std::wstring& targetExePath, uint32_t waitPid) {
    wchar_t myPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, myPath, MAX_PATH);
    return applySelfUpdateAndRestart(myPath, targetExePath, waitPid, true);
}

std::wstring AutoUpdater::buildSelfUpdateCommand(const std::wstring& newExePath, const std::wstring& currentExePath) {
    std::wstring cmd = L"cmd.exe /c \"timeout /t 1 /nobreak >nul & move /y \"" + newExePath + L"\" \"" + currentExePath + L"\" & start \"\" \"" + currentExePath + L"\"\"";
    return cmd;
}

bool AutoUpdater::launchSelfUpdate(const std::wstring& newExePath, const std::wstring& currentExePath) {
    DWORD currentPid = GetCurrentProcessId();
    std::wstring cmd = L"\"" + newExePath + L"\" --update-target \"" + currentExePath + L"\" --wait-pid " + std::to_wstring(currentPid);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    // Launch target new executable directly as GUI process (zero console window popup)
    BOOL res = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    if (res) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    // Fallback: spawn command line helper if direct launch fails
    std::wstring fbCmd = buildSelfUpdateCommand(newExePath, currentExePath);
    std::vector<wchar_t> fbBuf(fbCmd.begin(), fbCmd.end());
    fbBuf.push_back(L'\0');
    res = CreateProcessW(
        nullptr,
        fbBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    );
    if (res) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    return false;
}

} // namespace cppdesk
