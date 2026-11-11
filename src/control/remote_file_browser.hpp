#pragma once

#include "../core/protocol.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace cppdesk {

struct FileBrowserPaneState {
    std::string currentPath;
    std::vector<std::string> availableDrives;
    std::vector<FileBrowserItemEntry> entries;
    int selectedIndex = -1;
    float scrollOffset = 0.0f;
    std::string statusMessage;
    bool isLoading = false;
    int lastClickIndex = -1;       // double-click detection
    uint64_t lastClickTick = 0;
    uint64_t loadStartTick = 0;

    void beginLoading(uint64_t nowMs) {
        isLoading = true;
        loadStartTick = nowMs;
        statusMessage.clear();
    }

    // Gives up on a request that went unanswered (an older host never replies to the 3.8.0 packets).
    // Returns true once, at the moment it stops loading.
    bool expireLoading(uint64_t nowMs, uint64_t timeoutMs = 10000) {
        if (!isLoading || nowMs - loadStartTick < timeoutMs) return false;
        isLoading = false;
        statusMessage = "No response from the remote host. It may be running an older version.";
        return true;
    }

    const FileBrowserItemEntry* selectedEntry() const {
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size())) {
            return &entries[selectedIndex];
        }
        return nullptr;
    }
};

class RemoteFileBrowserManager {
public:
    static std::vector<std::string> getLocalDrives();
    static bool listLocalDirectory(const std::string& path,
                                   std::vector<FileBrowserItemEntry>& outEntries,
                                   std::string& outCanonicalPath,
                                   std::string& outError);
    static bool createLocalDirectory(const std::string& path, std::string& outError);
    static bool deleteLocalItem(const std::string& path, std::string& outError);
    static bool renameLocalItem(const std::string& oldPath, const std::string& newName, std::string& outError);

    static std::string formatFileSize(uint64_t bytes);
    static std::string formatFileTimestamp(uint64_t mtimeSec);
    static std::string normalizeDirectoryPath(const std::string& path);
    static std::string getParentDirectory(const std::string& path);
    // Joins a directory path and an entry name with the separator the directory already uses (Windows or POSIX host).
    static std::string joinPath(const std::string& dir, const std::string& name);
};

} // namespace cppdesk
