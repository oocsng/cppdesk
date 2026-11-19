#include "updater.hpp"
#include "../core/crypto_identity.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <sddl.h>
#endif

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>
#include <thread>
#include <cctype>

namespace cppdesk {

namespace {

constexpr size_t kMaxReleaseApiBytes = 2 * 1024 * 1024;       // 2 MB GitHub API JSON cap
constexpr uint64_t kMaxUpdateAssetBytes = 256ULL * 1024 * 1024; // 256 MB executable cap

std::wstring toWide(const std::string& str) {
    if (str.empty()) return L"";
#if defined(_WIN32)
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring out(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &out[0], len);
    return out;
#else
    return std::wstring(str.begin(), str.end());
#endif
}

bool isValidSha256Hex(const std::string& hex) {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

// "sha256:<64 hex>" -> lower-case hex; anything else -> "".
std::string parseSha256Digest(const std::string& digest) {
    static const std::string prefix = "sha256:";
    if (digest.size() != prefix.size() + 64 || digest.compare(0, prefix.size(), prefix) != 0) return "";
    std::string hex = digest.substr(prefix.size());
    for (char& c : hex) {
        unsigned char u = static_cast<unsigned char>(c);
        if (!std::isxdigit(u)) return "";
        c = static_cast<char>(std::tolower(u));
    }
    return hex;
}

void skipJsonWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

// Parses a JSON string starting at s[i] == '"', advancing i past the closing '"'.
bool parseJsonStringAt(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    bool escape = false;
    while (i < s.size()) {
        char c = s[i++];
        if (escape) {
            if (c == 'n') out += '\n';
            else if (c == 'r') out += '\r';
            else if (c == 't') out += '\t';
            else if (c == '\\') out += '\\';
            else if (c == '"') out += '"';
            else if (c == '/') out += '/';
            else out += c;
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

// Advances i past a JSON value (string, object, array, or primitive).
void skipJsonValue(const std::string& s, size_t& i) {
    skipJsonWs(s, i);
    if (i >= s.size()) return;
    if (s[i] == '"') {
        std::string dummy;
        parseJsonStringAt(s, i, dummy);
        return;
    }
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        bool inStr = false;
        bool esc = false;
        while (i < s.size()) {
            char c = s[i++];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
            } else {
                if (c == '"') inStr = true;
                else if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') {
                    if (--depth == 0) return;
                }
            }
        }
        return;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
        ++i;
    }
}

#if defined(_WIN32)
struct TrackedWinHttpSession {
    std::mutex mutex;
    HINTERNET handle = nullptr;
    bool closed = false;

    void close() {
        std::lock_guard<std::mutex> lk(mutex);
        if (!closed && handle) {
            closed = true;
            WinHttpCloseHandle(handle);
            handle = nullptr;
        }
    }
};

std::mutex g_activeSessionsMutex;
std::vector<std::weak_ptr<TrackedWinHttpSession>> g_activeSessions;

std::shared_ptr<TrackedWinHttpSession> registerWinHttpSession(HINTERNET hSession) {
    auto s = std::make_shared<TrackedWinHttpSession>();
    s->handle = hSession;
    std::lock_guard<std::mutex> lk(g_activeSessionsMutex);
    g_activeSessions.erase(
        std::remove_if(g_activeSessions.begin(), g_activeSessions.end(),
                       [](const std::weak_ptr<TrackedWinHttpSession>& w) { return w.expired(); }),
        g_activeSessions.end());
    g_activeSessions.push_back(s);
    return s;
}

bool validatePeFromHandle(HANDLE hFile) {
    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < static_cast<LONGLONG>(sizeof(IMAGE_DOS_HEADER) + sizeof(DWORD))) {
        return false;
    }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) return false;

    IMAGE_DOS_HEADER dos{};
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &dos, sizeof(dos), &bytesRead, nullptr) || bytesRead != sizeof(dos)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos.e_lfanew <= 0 || static_cast<uint64_t>(dos.e_lfanew) + sizeof(DWORD) > static_cast<uint64_t>(liSize.QuadPart)) {
        return false;
    }

    LARGE_INTEGER liSeek{};
    liSeek.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(hFile, liSeek, nullptr, FILE_BEGIN)) return false;

    DWORD ntSig = 0;
    if (!ReadFile(hFile, &ntSig, sizeof(ntSig), &bytesRead, nullptr) || bytesRead != sizeof(ntSig)) return false;
    return (ntSig == IMAGE_NT_SIGNATURE);
}

bool handleMatchesSha256(HANDLE hFile, const std::string& expectedHex) {
    if (!isValidSha256Hex(expectedHex)) return false;
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) return false;

    std::vector<uint8_t> data;
    constexpr DWORD kChunk = 1 << 20;
    std::vector<uint8_t> chunk(kChunk);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(hFile, chunk.data(), kChunk, &got, nullptr)) return false;
        if (got == 0) break;
        if (data.size() + got > kMaxUpdateAssetBytes) return false;
        data.insert(data.end(), chunk.begin(), chunk.begin() + got);
    }

    std::string actual = CryptoUtils::toHex(CryptoUtils::sha256(data.data(), data.size()).data(), 32);
    std::string expected = expectedHex;
    for (char& c : expected) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return actual == expected;
}

bool createOwnerOnlyDirectory(const std::wstring& dirPath) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR sd = nullptr;
    // Owner-only full access, inherited by files created inside the staging directory.
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;OW)", SDDL_REVISION_1, &sd, nullptr)) {
        sa.lpSecurityDescriptor = sd;
    }
    BOOL ok = CreateDirectoryW(dirPath.c_str(), sd ? &sa : nullptr);
    DWORD err = GetLastError();
    if (sd) LocalFree(sd);
    return ok || err == ERROR_ALREADY_EXISTS;
}
#endif

template <typename Fn>
void invokeWithGate(const std::shared_ptr<AutoUpdater::CallbackGate>& gate, Fn&& fn) {
    if (gate) {
        gate->runIfAlive(std::forward<Fn>(fn));
    } else {
        fn();
    }
}

} // namespace

void AutoUpdater::cancelPendingOperations() {
#if defined(_WIN32)
    std::vector<std::shared_ptr<TrackedWinHttpSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(g_activeSessionsMutex);
        for (const auto& w : g_activeSessions) {
            if (auto s = w.lock()) sessions.push_back(std::move(s));
        }
        g_activeSessions.clear();
    }
    for (const auto& s : sessions) {
        s->close();
    }
#endif
}

std::wstring AutoUpdater::createStagingExePath() {
#if defined(_WIN32)
    std::wstring baseDir;
    DWORD need = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (need > 1) {
        std::wstring localAppData(need, L'\0');
        DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), need);
        if (got > 0 && got < need) {
            localAppData.resize(got);
            std::wstring appDir = localAppData + L"\\CppDesk";
            CreateDirectoryW(appDir.c_str(), nullptr);
            std::wstring updatesDir = appDir + L"\\updates";
            if (createOwnerOnlyDirectory(updatesDir)) {
                baseDir = updatesDir;
            }
        }
    }
    if (baseDir.empty()) {
        wchar_t tempDir[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, tempDir);
        if (len == 0 || len >= MAX_PATH) return L"";
        std::wstring fallbackDir = std::wstring(tempDir) + L"CppDesk_updates";
        if (!createOwnerOnlyDirectory(fallbackDir)) return L"";
        baseDir = fallbackDir;
    }

    uint8_t rnd[8]{};
    CryptoUtils::randomBytes(rnd, sizeof(rnd));
    std::string hex = CryptoUtils::toHex(rnd, sizeof(rnd));
    return baseDir + L"\\CppDesk_update_" + toWide(hex) + L".exe";
#else
    return L"";
#endif
}

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
    if (json.empty() || key.empty()) return "";

    size_t i = 0;
    while (i < json.size()) {
        if (json[i] == '"') {
            std::string str;
            if (!parseJsonStringAt(json, i, str)) return "";
            size_t afterKey = i;
            skipJsonWs(json, afterKey);
            if (afterKey < json.size() && json[afterKey] == ':') {
                size_t valPos = afterKey + 1;
                skipJsonWs(json, valPos);
                if (str == key) {
                    if (valPos >= json.size() || json[valPos] != '"') return "";
                    std::string val;
                    return parseJsonStringAt(json, valPos, val) ? val : "";
                }
                i = valPos;
                continue;
            }
        } else {
            ++i;
        }
    }
    return "";
}

UpdateInfo AutoUpdater::queryLatestReleaseSync(
    const std::string& repoOwner,
    const std::string& repoName,
    const std::string& currentVersion)
{
    UpdateInfo info;
    info.currentVersion = currentVersion;
    info.downloadUrl = "https://github.com/" + repoOwner + "/" + repoName + "/releases/latest";

#if defined(_WIN32)
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

    WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 30000);
    auto trackedSession = registerWinHttpSession(hSession);

    HINTERNET hConnect = WinHttpConnect(
        hSession,
        L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT,
        0
    );

    if (!hConnect) {
        trackedSession->close();
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
        trackedSession->close();
        info.errorMessage = "Failed to open WinHTTP request";
        return info;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

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
        trackedSession->close();
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
        trackedSession->close();
        info.errorMessage = "GitHub API returned HTTP " + std::to_string(statusCode);
        return info;
    }

    std::string responseBody;
    DWORD bytesAvailable = 0;
    bool responseTooLarge = false;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        DWORD toRead = std::min<DWORD>(bytesAvailable, 64 * 1024);
        if (responseBody.size() + toRead > kMaxReleaseApiBytes) {
            responseTooLarge = true;
            break;
        }
        std::vector<char> buffer(toRead);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buffer.data(), toRead, &bytesRead) && bytesRead > 0) {
            responseBody.append(buffer.data(), bytesRead);
        } else {
            break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    trackedSession->close();

    if (responseTooLarge) {
        info.errorMessage = "GitHub API response exceeded safety size limit";
        return info;
    }

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
    info.directDownloadUrl = extractAssetDownloadUrl(responseBody, "CppDesk.exe", &info.assetSizeBytes, &info.assetSha256);

    if (!info.latestVersion.empty()) {
        info.checkSucceeded = true;
        info.success = true;
        info.updateRequired = isNewerVersion(info.latestVersion, currentVersion);
    } else {
        info.errorMessage = "No release tag found in GitHub API response";
    }

    return info;
#else
    info.errorMessage = "Auto-update not supported on this platform";
    return info;
#endif
}

void AutoUpdater::checkForUpdatesAsync(
    std::function<void(const UpdateInfo&)> onComplete,
    const std::string& repoOwner,
    const std::string& repoName,
    const std::string& currentVersion,
    std::shared_ptr<CallbackGate> gate)
{
    std::thread([onComplete = std::move(onComplete), repoOwner, repoName, currentVersion, gate = std::move(gate)]() {
        if (gate && !gate->isAlive()) return;
        UpdateInfo info = queryLatestReleaseSync(repoOwner, repoName, currentVersion);
        if (onComplete) {
            invokeWithGate(gate, [&]() { onComplete(info); });
        }
    }).detach();
}

std::string AutoUpdater::extractAssetDownloadUrl(const std::string& json, const std::string& assetName, uint64_t* outSize, std::string* outSha256) {
    if (outSize) *outSize = 0;
    if (outSha256) outSha256->clear();
    if (json.empty() || assetName.empty()) return "";

    // Locate the "assets": [ ... ] array at top level (ignoring string literals such as "body").
    size_t i = 0;
    size_t assetsArrStart = std::string::npos;
    int braceDepth = 0;
    while (i < json.size()) {
        char c = json[i];
        if (c == '{') {
            ++braceDepth;
            ++i;
        } else if (c == '}') {
            if (braceDepth > 0) --braceDepth;
            ++i;
        } else if (c == '"') {
            std::string key;
            if (!parseJsonStringAt(json, i, key)) return "";
            size_t p = i;
            skipJsonWs(json, p);
            if (p < json.size() && json[p] == ':') {
                ++p;
                skipJsonWs(json, p);
                if (braceDepth <= 1 && key == "assets" && p < json.size() && json[p] == '[') {
                    assetsArrStart = p;
                    break;
                }
                i = p;
            }
        } else {
            ++i;
        }
    }

    if (assetsArrStart == std::string::npos) return "";

    // Walk each object inside the "assets" array.
    i = assetsArrStart + 1;
    while (i < json.size()) {
        skipJsonWs(json, i);
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] == ',') {
            ++i;
            continue;
        }
        if (json[i] != '{') {
            skipJsonValue(json, i);
            continue;
        }

        // Parse top-level fields of this asset object only (nested objects like "uploader" are skipped cleanly).
        ++i; // consume '{'
        std::string curName;
        std::string curUrl;
        std::string curDigest;
        uint64_t curSize = 0;

        while (i < json.size()) {
            skipJsonWs(json, i);
            if (i >= json.size()) break;
            if (json[i] == '}') {
                ++i;
                break;
            }
            if (json[i] == ',') {
                ++i;
                continue;
            }
            if (json[i] != '"') {
                skipJsonValue(json, i);
                continue;
            }
            std::string field;
            if (!parseJsonStringAt(json, i, field)) return "";
            skipJsonWs(json, i);
            if (i >= json.size() || json[i] != ':') break;
            ++i;
            skipJsonWs(json, i);

            if ((field == "name" || field == "browser_download_url" || field == "digest") &&
                i < json.size() && json[i] == '"') {
                std::string val;
                if (!parseJsonStringAt(json, i, val)) return "";
                if (field == "name") curName = std::move(val);
                else if (field == "browser_download_url") curUrl = std::move(val);
                else if (field == "digest") curDigest = std::move(val);
            } else if (field == "size" && i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) {
                size_t dStart = i;
                while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
                try {
                    curSize = std::stoull(json.substr(dStart, i - dStart));
                } catch (...) {
                    curSize = 0;
                }
            } else {
                skipJsonValue(json, i);
            }
        }

        if (curName == assetName && !curUrl.empty()) {
            if (outSize) *outSize = curSize;
            if (outSha256) *outSha256 = parseSha256Digest(curDigest);
            return curUrl;
        }
    }

    return "";
}

void AutoUpdater::downloadUpdateAssetAsync(
    const std::string& downloadUrl,
    const std::wstring& targetPath,
    UpdateProgressCallback onProgress,
    std::function<void(bool success, const std::string& error)> onComplete,
    const std::string& expectedSha256,
    std::shared_ptr<CallbackGate> gate)
{
#if !defined(_WIN32)
    if (onComplete) {
        invokeWithGate(gate, [&]() { onComplete(false, "Auto-updater not supported on this platform"); });
    }
#else
    std::thread([downloadUrl, targetPath, onProgress = std::move(onProgress), onComplete = std::move(onComplete), expectedSha256, gate = std::move(gate)]() {
        auto reportDone = [&](bool ok, const std::string& err) {
            if (onComplete) {
                invokeWithGate(gate, [&]() { onComplete(ok, err); });
            }
        };
        auto reportProgress = [&](uint64_t downloaded, uint64_t total, double speed) {
            if (onProgress) {
                invokeWithGate(gate, [&]() { onProgress(downloaded, total, speed); });
            }
        };

        if (gate && !gate->isAlive()) return;

        if (downloadUrl.empty() || targetPath.empty()) {
            reportDone(false, "Invalid URL or target path");
            return;
        }

        if (!isValidSha256Hex(expectedSha256)) {
            reportDone(false, "Refusing update without a valid SHA-256 digest");
            return;
        }

        std::wstring wUrl = toWide(downloadUrl);
        URL_COMPONENTSW urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
        urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &urlComp)) {
            reportDone(false, "Failed to parse download URL");
            return;
        }

        if (urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
            reportDone(false, "Refusing non-HTTPS update URL");
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
            reportDone(false, "Failed to initialize WinHTTP session");
            return;
        }

        WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 30000);
        auto trackedSession = registerWinHttpSession(hSession);

        HINTERNET hConnect = WinHttpConnect(
            hSession,
            host.c_str(),
            urlComp.nPort,
            0
        );

        if (!hConnect) {
            trackedSession->close();
            reportDone(false, "Failed to connect to host: " + downloadUrl);
            return;
        }

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
            trackedSession->close();
            reportDone(false, "Failed to create WinHTTP request");
            return;
        }

        // Follow HTTPS redirects only; refuse HTTPS-to-HTTP downgrades.
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
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
            trackedSession->close();
            reportDone(false, "Failed to receive HTTP response header");
            return;
        }

        DWORD statusCode = 0;
        DWORD scSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode != 200) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            trackedSession->close();
            reportDone(false, "Server returned HTTP " + std::to_string(statusCode));
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
        if (totalBytes > kMaxUpdateAssetBytes) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            trackedSession->close();
            reportDone(false, "Update asset exceeds maximum allowed size");
            return;
        }

        // Exclusive handle: verify SHA-256 and PE headers before closing so no other process can swap the file mid-check.
        HANDLE hFile = CreateFileW(
            targetPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            trackedSession->close();
            reportDone(false, "Failed to create target file on disk");
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
        bool exceededCap = false;

        while (true) {
            if (gate && !gate->isAlive()) {
                readSuccess = false;
                break;
            }
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
            if (!WinHttpReadData(hRequest, buffer.data(), toRead, &bytesRead)) {
                readSuccess = false;
                break;
            }
            if (bytesRead == 0) {
                readSuccess = true;
                break;
            }

            if (downloadedBytes + bytesRead > kMaxUpdateAssetBytes) {
                readSuccess = false;
                exceededCap = true;
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

            reportProgress(downloadedBytes, totalBytes, currentSpeedBps);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        trackedSession->close();

        if (!readSuccess || (totalBytes > 0 && downloadedBytes != totalBytes)) {
            CloseHandle(hFile);
            DeleteFileW(targetPath.c_str());
            reportDone(false, exceededCap ? "Update asset exceeds maximum allowed size" : "Download stream truncated or failed");
            return;
        }

        FlushFileBuffers(hFile);
        const bool shaOk = handleMatchesSha256(hFile, expectedSha256);
        const bool peOk = shaOk && validatePeFromHandle(hFile);
        CloseHandle(hFile);

        if (!shaOk) {
            DeleteFileW(targetPath.c_str());
            reportDone(false, "Update checksum mismatch (SHA-256)");
            return;
        }

        if (!peOk) {
            DeleteFileW(targetPath.c_str());
            reportDone(false, "Validation error: Corrupt PE binary");
            return;
        }

        reportProgress(downloadedBytes, totalBytes, currentSpeedBps);
        reportDone(true, "");
    }).detach();
#endif
}

#if !defined(_WIN32)
bool AutoUpdater::validatePeBinary(const std::wstring&) { return false; }
bool AutoUpdater::fileMatchesSha256(const std::wstring&, const std::string&) { return false; }
int AutoUpdater::applySelfUpdateAndRestart(const std::wstring&, const std::wstring&, uint32_t, bool) { return 1; }
int AutoUpdater::applySelfUpdateAndRestart(const std::wstring&, uint32_t) { return 1; }
bool AutoUpdater::launchSelfUpdate(const std::wstring&, const std::wstring&) { return false; }
#else
bool AutoUpdater::fileMatchesSha256(const std::wstring& filePath, const std::string& expectedHex) {
    if (expectedHex.empty()) return false;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    // shortcut: whole file in memory (the exe is a few MB); stream it if releases ever grow large.
    std::vector<uint8_t> data;
    constexpr DWORD kChunk = 1 << 20;
    std::vector<uint8_t> chunk(kChunk);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(hFile, chunk.data(), kChunk, &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        data.insert(data.end(), chunk.begin(), chunk.begin() + got);
    }
    CloseHandle(hFile);
    if (!ok) return false;

    std::string actual = CryptoUtils::toHex(CryptoUtils::sha256(data.data(), data.size()).data(), 32);
    std::string expected = expectedHex;
    for (char& c : expected) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return actual == expected;
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
    bool usedCopyFallback = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (MoveFileExW(sourceExePath.c_str(), targetExePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            copied = true;
            break;
        }
        if (CopyFileW(sourceExePath.c_str(), targetExePath.c_str(), FALSE)) {
            copied = true;
            usedCopyFallback = true;
            break;
        }
        Sleep(250);
    }

    if (copied && usedCopyFallback) {
        if (!DeleteFileW(sourceExePath.c_str())) {
            MoveFileExW(sourceExePath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }

    bool launched = !launchAfter;
    if (copied && launchAfter) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        std::wstring quotedCmd = L"\"" + targetExePath + L"\"";
        std::vector<wchar_t> cmdBuf(quotedCmd.begin(), quotedCmd.end());
        cmdBuf.push_back(L'\0');
        if (CreateProcessW(targetExePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            launched = true;
        } else {
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWNORMAL;
            cmdBuf.assign(quotedCmd.begin(), quotedCmd.end());
            cmdBuf.push_back(L'\0');
            if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                launched = true;
            }
        }
    }
    return (copied && launched) ? 0 : 1;
}

int AutoUpdater::applySelfUpdateAndRestart(const std::wstring& targetExePath, uint32_t waitPid) {
    std::wstring myPath(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, myPath.data(), static_cast<DWORD>(myPath.size()));
        if (len == 0) return 1;
        if (len < myPath.size()) {
            myPath.resize(len);
            break;
        }
        if (myPath.size() >= 32768) return 1;
        myPath.resize(myPath.size() * 2);
    }
    return applySelfUpdateAndRestart(myPath, targetExePath, waitPid, true);
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

    // Launch target new executable directly as GUI process with explicit lpApplicationName
    BOOL res = CreateProcessW(
        newExePath.c_str(),
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

    // Fallback attempt: reinitialize si and retry with command line
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    cmdBuf.assign(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    res = CreateProcessW(
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
    return false;
}
#endif

} // namespace cppdesk
