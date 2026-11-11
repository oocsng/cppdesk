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
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace cppdesk {

namespace {

#if defined(_WIN32)
constexpr char kSep = '\\';

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
#else
constexpr char kSep = '/';
#endif

// Paths cross the wire as UTF-8. On Windows a narrow std::filesystem::path is read as the ANSI code page,
// so convert explicitly in both directions.
std::filesystem::path toPath(const std::string& utf8) {
#if defined(_WIN32)
    return std::filesystem::path(utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string fromPath(const std::filesystem::path& p) {
#if defined(_WIN32)
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

bool isSep(char c) { return c == '\\' || c == '/'; }

std::string trimTrailingSeps(std::string p) {
    while (!p.empty() && isSep(p.back())) p.pop_back();
    return p;
}

// Resolves a client-supplied path to an absolute path that is not a drive or share root.
// "C:\" trims to "C:" (drive-relative), which is not absolute, so drive roots are refused as well.
bool resolveNonRootPath(const std::string& utf8, std::filesystem::path& out, std::string& outError) {
    std::string trimmed = trimTrailingSeps(utf8);
    if (trimmed.empty()) {
        outError = "Refusing to operate on an empty or root path";
        return false;
    }
    std::filesystem::path p = toPath(trimmed);
    if (!p.is_absolute() || !p.has_relative_path()) {
        outError = "Refusing to operate on a relative path or a drive root: " + utf8;
        return false;
    }
    out = std::move(p);
    return true;
}

bool isValidEntryName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
#if defined(_WIN32)
    const char* bad = "\\/:*?\"<>|";
#else
    const char* bad = "/";
#endif
    for (char c : name) {
        if (c == '\0' || std::strchr(bad, c) != nullptr) return false;
    }
    return true;
}

bool nameLess(const FileBrowserItemEntry& a, const FileBrowserItemEntry& b) {
    size_t n = std::min(a.name.size(), b.name.size());
    for (size_t i = 0; i < n; ++i) {
        int ca = std::tolower(static_cast<unsigned char>(a.name[i]));
        int cb = std::tolower(static_cast<unsigned char>(b.name[i]));
        if (ca != cb) return ca < cb;
    }
    if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
    return a.name < b.name;
}

} // namespace

std::vector<std::string> RemoteFileBrowserManager::getLocalDrives() {
    std::vector<std::string> drives;
#if defined(_WIN32)
    DWORD mask = GetLogicalDrives();
    for (char d = 'A'; d <= 'Z'; ++d) {
        if (mask & (1u << (d - 'A'))) {
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
#if defined(_WIN32)
    for (char& c : p) {
        if (c == '/') c = '\\';
    }
#endif
    if (!isSep(p.back())) p.push_back(kSep);
    return p;
}

// Pure string logic so a Windows viewer can compute the parent of a POSIX host path (and the reverse).
// The result keeps its trailing separator: "C:\Users\bob" -> "C:\Users\", "C:\Users" -> "C:\", "C:\" -> drive list.
std::string RemoteFileBrowserManager::getParentDirectory(const std::string& path) {
    if (path.empty() || path == "$DRIVES$") return "$DRIVES$";
    std::string p = trimTrailingSeps(path);
    if (p.empty()) return "$DRIVES$";
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return "$DRIVES$";
    return p.substr(0, pos + 1);
}

std::string RemoteFileBrowserManager::joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == "$DRIVES$") return name;
    if (isSep(dir.back())) return dir + name;
    char sep = (dir.find('\\') != std::string::npos) ? '\\' : (dir.find('/') != std::string::npos ? '/' : kSep);
    return dir + sep + name;
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
    std::filesystem::path dirPath = toPath(path);
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
    FileBrowserItemEntry parentEntry;
    parentEntry.name = "..";
    parentEntry.isDirectory = 1;
    parentEntry.sizeBytes = 0;
    parentEntry.mtimeSec = 0;
    parentEntry.attributes = 0x10;
    outEntries.push_back(std::move(parentEntry));

    std::vector<FileBrowserItemEntry> dirs;
    std::vector<FileBrowserItemEntry> files;

    // No skip_permission_denied: an unreadable directory must report an error, not look empty.
    std::filesystem::directory_iterator it(dirPath, ec);
    std::filesystem::directory_iterator end;
    if (ec) {
        outError = "Cannot read directory: " + ec.message();
        outEntries.clear();
        return false;
    }

    while (it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        std::string filename = fromPath(entry.path().filename());
        if (filename.empty()) {
            it.increment(ec);
            if (ec) break;
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
        if (ec) break;
    }

    if (ec) {
        outError = "Cannot read directory: " + ec.message();
        outEntries.clear();
        return false;
    }

    std::sort(dirs.begin(), dirs.end(), nameLess);
    std::sort(files.begin(), files.end(), nameLess);

    for (auto& d : dirs) outEntries.push_back(std::move(d));
    for (auto& f : files) outEntries.push_back(std::move(f));

    return true;
}

bool RemoteFileBrowserManager::createLocalDirectory(const std::string& path, std::string& outError) {
    outError.clear();
    std::error_code ec;
    std::filesystem::path p = toPath(trimTrailingSeps(path));
    if (path.empty() || !p.is_absolute()) {
        outError = "Refusing to create a directory from a relative path: " + path;
        return false;
    }
    std::filesystem::create_directories(p, ec);
    if (ec) {
        outError = "Failed to create directory: " + ec.message();
        return false;
    }
    return true;
}

bool RemoteFileBrowserManager::deleteLocalItem(const std::string& path, std::string& outError) {
    outError.clear();
    std::filesystem::path target;
    if (!resolveNonRootPath(path, target, outError)) return false;

    std::error_code ec;
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
    // newName is a bare name inside the same directory. A separator, "..", or a drive would move the item elsewhere.
    if (!isValidEntryName(newName)) {
        outError = "Invalid name: " + newName;
        return false;
    }
    std::filesystem::path oldP;
    if (!resolveNonRootPath(oldPath, oldP, outError)) return false;

    std::error_code ec;
    if (!std::filesystem::exists(oldP, ec)) {
        outError = "Original item does not exist: " + oldPath;
        return false;
    }

    std::filesystem::path newP = oldP.parent_path() / toPath(newName);
    if (std::filesystem::exists(newP, ec) && !std::filesystem::equivalent(oldP, newP, ec)) {
        outError = "An item named '" + newName + "' already exists";
        return false;
    }
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
