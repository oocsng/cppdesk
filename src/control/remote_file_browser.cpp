#include "remote_file_browser.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if defined(_WIN32)
#include <windows.h>
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace cppdesk {

namespace {

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return std::wstring();
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, w.data(), len);
    return w;
}

std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string u(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, u.data(), len, nullptr, nullptr);
    return u;
}
#endif

} // namespace

std::vector<std::string> RemoteFileBrowserManager::getLocalDrives() {
    std::vector<std::string> drives;
#if defined(_WIN32)
    DWORD mask = GetLogicalDrives();
    for (char d = 'A'; d <= 'Z'; ++d) {
        if (mask & (1 << (d - 'A'))) {
            std::string root;
            root += d;
            root += ":\\";
            UINT dt = GetDriveTypeA(root.c_str());
            if (dt != DRIVE_NO_ROOT_DIR && dt != DRIVE_UNKNOWN) {
                drives.push_back(root);
            }
        }
    }
    if (drives.empty()) {
        drives.push_back("C:\\");
    }
#else
    drives.push_back("/");
#endif
    return drives;
}

std::string RemoteFileBrowserManager::normalizeDirectoryPath(const std::string& path) {
    if (path.empty() || path == "$DRIVES$") return "$DRIVES$";
    std::string p = path;
    for (char& c : p) {
        if (c == '/') c = '\\';
    }
    if (!p.empty() && p.back() != '\\') {
        p.push_back('\\');
    }
    return p;
}

std::string RemoteFileBrowserManager::getParentDirectory(const std::string& path) {
    if (path.empty() || path == "$DRIVES$") return "$DRIVES$";
    std::filesystem::path fp(path);
    if (!fp.has_parent_path() || fp.parent_path() == fp) {
        return "$DRIVES$";
    }
    std::string parentStr = fp.parent_path().string();
    return normalizeDirectoryPath(parentStr);
}

bool RemoteFileBrowserManager::listLocalDirectory(const std::string& path,
                                                 std::vector<FileBrowserItemEntry>& outEntries,
                                                 std::string& outCanonicalPath,
                                                 std::string& outError) {
    outEntries.clear();
    outError.clear();

    if (path.empty() || path == "$DRIVES$") {
        outCanonicalPath = "$DRIVES$";
        auto drives = getLocalDrives();
        for (const auto& d : drives) {
            FileBrowserItemEntry entry;
            entry.name = d;
            entry.isDirectory = 1;
            entry.sizeBytes = 0;
            entry.mtimeSec = 0;
            entry.attributes = 0x10; // FILE_ATTRIBUTE_DIRECTORY
            outEntries.push_back(std::move(entry));
        }
        return true;
    }

    std::error_code ec;
    std::filesystem::path dirPath(path);
    if (!std::filesystem::exists(dirPath, ec)) {
        outError = "Directory not found: " + path;
        return false;
    }
    if (!std::filesystem::is_directory(dirPath, ec)) {
        outError = "Path is not a directory: " + path;
        return false;
    }

    outCanonicalPath = normalizeDirectoryPath(path);

    // Parent directory navigation entry
    if (outCanonicalPath != "$DRIVES$") {
        FileBrowserItemEntry parentEntry;
        parentEntry.name = "..";
        parentEntry.isDirectory = 1;
        parentEntry.sizeBytes = 0;
        parentEntry.mtimeSec = 0;
        parentEntry.attributes = 0x10;
        outEntries.push_back(std::move(parentEntry));
    }

    std::vector<FileBrowserItemEntry> dirs;
    std::vector<FileBrowserItemEntry> files;

    std::filesystem::directory_iterator it(
        dirPath,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    std::filesystem::directory_iterator end;

    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        std::string filename = entry.path().filename().string();
        if (filename.empty()) {
            it.increment(ec);
            continue;
        }

        FileBrowserItemEntry item;
        item.name = filename;
        item.isDirectory = entry.is_directory(entryEc) ? 1 : 0;
        if (!item.isDirectory) {
            item.sizeBytes = entry.file_size(entryEc);
            if (entryEc) item.sizeBytes = 0;
        } else {
            item.sizeBytes = 0;
        }

        auto lwt = entry.last_write_time(entryEc);
        if (!entryEc) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                lwt - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            item.mtimeSec = static_cast<uint64_t>(std::chrono::system_clock::to_time_t(sctp));
        } else {
            item.mtimeSec = 0;
        }

        if (item.isDirectory) {
            dirs.push_back(std::move(item));
        } else {
            files.push_back(std::move(item));
        }

        it.increment(ec);
    }

    auto sortAlpha = [](const FileBrowserItemEntry& a, const FileBrowserItemEntry& b) {
        return a.name < b.name;
    };
    std::sort(dirs.begin(), dirs.end(), sortAlpha);
    std::sort(files.begin(), files.end(), sortAlpha);

    for (auto& d : dirs) outEntries.push_back(std::move(d));
    for (auto& f : files) outEntries.push_back(std::move(f));

    return true;
}

bool RemoteFileBrowserManager::createLocalDirectory(const std::string& path, std::string& outError) {
    outError.clear();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path), ec);
    if (ec) {
        outError = "Failed to create directory: " + ec.message();
        return false;
    }
    return true;
}

bool RemoteFileBrowserManager::deleteLocalItem(const std::string& path, std::string& outError) {
    outError.clear();
    std::error_code ec;
    std::filesystem::path target(path);
    if (!std::filesystem::exists(target, ec)) {
        outError = "Item does not exist: " + path;
        return false;
    }

    std::filesystem::remove_all(target, ec);
    if (ec) {
        outError = "Failed to delete item: " + ec.message();
        return false;
    }
    return true;
}

bool RemoteFileBrowserManager::renameLocalItem(const std::string& oldPath, const std::string& newName, std::string& outError) {
    outError.clear();
    std::error_code ec;
    std::filesystem::path oldP(oldPath);
    if (!std::filesystem::exists(oldP, ec)) {
        outError = "Original item does not exist: " + oldPath;
        return false;
    }

    std::filesystem::path newP = oldP.parent_path() / newName;
    std::filesystem::rename(oldP, newP, ec);
    if (ec) {
        outError = "Failed to rename: " + ec.message();
        return false;
    }
    return true;
}

std::string RemoteFileBrowserManager::formatFileSize(uint64_t bytes) {
    if (bytes == 0) return "0 B";
    if (bytes < 1024ULL) return std::to_string(bytes) + " B";
    if (bytes < 1024ULL * 1024ULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        return buf;
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string RemoteFileBrowserManager::formatFileTimestamp(uint64_t mtimeSec) {
    if (mtimeSec == 0) return "-";
    time_t raw = static_cast<time_t>(mtimeSec);
    struct tm tmInfo{};
#if defined(_WIN32)
    localtime_s(&tmInfo, &raw);
#else
    localtime_r(&raw, &tmInfo);
#endif
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday,
             tmInfo.tm_hour, tmInfo.tm_min);
    return buf;
}

} // namespace cppdesk
