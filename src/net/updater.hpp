#pragma once

#include "../core/protocol.hpp"

#include <string>
#include <functional>
#include <vector>

namespace cppdesk {

struct UpdateInfo {
    bool checkSucceeded = false;
    bool success = false; // Compatibility alias
    bool updateRequired = false;
    std::string currentVersion = CPP_DESK_VERSION;
    std::string latestVersion;
    std::string downloadUrl = "https://github.com/oocsng/cppdesk/releases/latest";
    std::string releaseUrl = "https://github.com/oocsng/cppdesk/releases/latest"; // Compatibility alias
    std::string directDownloadUrl; // Direct URL to CppDesk.exe asset
    std::string releaseNotes;
    std::string errorMessage;
    uint64_t assetSizeBytes = 0;
    std::string assetSha256; // lower-case hex from the GitHub asset digest; empty when the release has none
};

using UpdateProgressCallback = std::function<void(uint64_t downloadedBytes, uint64_t totalBytes, double speedBps)>;

class AutoUpdater {
public:
    // Parses a semantic version string (e.g. "3.0.0", "v3.1.2") into major, minor, patch
    static bool parseVersionTriad(const std::string& verStr, int& major, int& minor, int& patch);

    // Returns true if candidateVer > baselineVer according to semantic versioning
    static bool isNewerVersion(const std::string& candidateVer, const std::string& baselineVer);

    // Synchronous query using WinHTTP hitting GitHub Releases API
    static UpdateInfo queryLatestReleaseSync(
        const std::string& repoOwner = "oocsng",
        const std::string& repoName = "cppdesk",
        const std::string& currentVersion = CPP_DESK_VERSION);

    // Asynchronous background query (calls onComplete on background thread)
    static void checkForUpdatesAsync(
        std::function<void(const UpdateInfo&)> onComplete,
        const std::string& repoOwner = "oocsng",
        const std::string& repoName = "cppdesk",
        const std::string& currentVersion = CPP_DESK_VERSION);

    // Overload accepting (repoOwner, repoName, currentVersion, onComplete)
    static void checkForUpdatesAsync(
        const std::string& repoOwner,
        const std::string& repoName,
        const std::string& currentVersion,
        std::function<void(const UpdateInfo&)> onComplete) {
        checkForUpdatesAsync(std::move(onComplete), repoOwner, repoName, currentVersion);
    }

    // Simple JSON value extractor helper
    static std::string extractJsonString(const std::string& json, const std::string& key);

    // Extract direct asset download URL from release JSON
    // outSha256 receives the asset's "sha256:<hex>" digest as lower-case hex, or "" when absent or malformed
    static std::string extractAssetDownloadUrl(const std::string& json, const std::string& assetName = "CppDesk.exe", uint64_t* outSize = nullptr, std::string* outSha256 = nullptr);

    // Asynchronous asset downloader with progress reporting.
    // A non-empty expectedSha256 must match the downloaded file or the file is deleted and the download fails.
    static void downloadUpdateAssetAsync(
        const std::string& downloadUrl,
        const std::wstring& targetPath,
        UpdateProgressCallback onProgress,
        std::function<void(bool success, const std::string& error)> onComplete,
        const std::string& expectedSha256 = "");

    // True when the file's SHA-256 equals expectedHex (case-insensitive). False for an empty expectation or unreadable file.
    static bool fileMatchesSha256(const std::wstring& filePath, const std::string& expectedHex);

    // Validates PE binary integrity (DOS and NT headers)
    static bool validatePeBinary(const std::wstring& filePath);

    // Builds helper command for detached replacement
    static std::wstring buildSelfUpdateCommand(const std::wstring& newExePath, const std::wstring& currentExePath);

    // Applies self-update: waits for previous instance PID, replaces target binary, and optionally launches updated app
    static int applySelfUpdateAndRestart(
        const std::wstring& sourceExePath,
        const std::wstring& targetExePath,
        uint32_t waitPid,
        bool launchAfter = true);

    // Convenience overload using current module executable as source
    static int applySelfUpdateAndRestart(const std::wstring& targetExePath, uint32_t waitPid);

    // Spawns detached helper and triggers process replacement
    static bool launchSelfUpdate(const std::wstring& newExePath, const std::wstring& currentExePath);
};

} // namespace cppdesk
