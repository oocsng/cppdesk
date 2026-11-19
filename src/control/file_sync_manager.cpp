#include "file_sync_manager.hpp"
#include "../core/win_compat.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef DeleteFile
#undef DeleteFile
#endif
#endif

#include <cmath>
#include <cstdio>
#include <cwctype>
#include <chrono>
#include <algorithm>

namespace cppdesk {

namespace {

constexpr uint64_t MAX_SYNC_FILE_BYTES = 2ULL * 1024 * 1024 * 1024;
constexpr uint32_t MAX_BLOCKS_PER_FILE =
    static_cast<uint32_t>(MAX_SYNC_FILE_BYTES / FileSyncManager::SYNC_BLOCK_SIZE);

uint32_t generateSyncId() {
    static std::atomic<uint32_t> s_counter{1};
    uint32_t id = static_cast<uint32_t>(GetTickCount64()) ^ (s_counter.fetch_add(1) * 2654435761u);
    return (id == 0) ? 1u : id;
}

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.c_str(), -1, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 1) {
        cp = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(cp, flags, str.c_str(), -1, nullptr, 0);
        if (len <= 1) return std::wstring();
    }
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(cp, flags, str.c_str(), -1, w.data(), len);
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

std::filesystem::path toFsPath(const std::string& utf8) {
#if defined(_WIN32)
    return std::filesystem::path(utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string fromFsPath(const std::filesystem::path& p) {
#if defined(_WIN32)
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

bool isReparseOrSymlink(const std::filesystem::path& p) {
    std::error_code ec;
    auto st = std::filesystem::symlink_status(p, ec);
    if (!ec && std::filesystem::is_symlink(st)) return true;
#if defined(_WIN32)
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
#endif
    return false;
}

bool isPathInside(const std::filesystem::path& childPath,
                  const std::filesystem::path& parentPath,
                  bool allowEqual = false) {
    if (parentPath.empty() || childPath.empty()) return false;
    std::error_code ec;
    std::filesystem::path parentCanon = std::filesystem::weakly_canonical(parentPath, ec);
    if (ec || parentCanon.empty()) return false;
    std::filesystem::path childCanon = std::filesystem::weakly_canonical(childPath, ec);
    if (ec || childCanon.empty()) return false;

    // Reject symlinks/reparse points along the relative path inside parentPath.
    std::filesystem::path relLex = childPath.lexically_normal().lexically_relative(parentPath.lexically_normal());
    if (!relLex.empty() && relLex != ".") {
        std::filesystem::path cur = parentCanon;
        for (const auto& part : relLex) {
            if (part == "..") return false;
            if (part == "." || part.empty()) continue;
            cur /= part;
            if (isReparseOrSymlink(cur)) return false;
        }
    }

#if defined(_WIN32)
    std::wstring childNorm = childCanon.generic_wstring();
    std::wstring parentNorm = parentCanon.generic_wstring();
    for (wchar_t& ch : childNorm) ch = static_cast<wchar_t>(std::towlower(ch));
    for (wchar_t& ch : parentNorm) ch = static_cast<wchar_t>(std::towlower(ch));
    while (parentNorm.size() > 1 && parentNorm.back() == L'/' &&
           !(parentNorm.size() == 3 && parentNorm[1] == L':')) {
        parentNorm.pop_back();
    }
    while (childNorm.size() > 1 && childNorm.back() == L'/' &&
           !(childNorm.size() == 3 && childNorm[1] == L':')) {
        childNorm.pop_back();
    }
    if (childNorm == parentNorm) return allowEqual;
    if (childNorm.size() <= parentNorm.size()) return false;
    if (childNorm.compare(0, parentNorm.size(), parentNorm) != 0) return false;
    return parentNorm.back() == L'/' || childNorm[parentNorm.size()] == L'/';
#else
    std::string childNorm = childCanon.generic_string();
    std::string parentNorm = parentCanon.generic_string();
    while (parentNorm.size() > 1 && parentNorm.back() == '/') parentNorm.pop_back();
    while (childNorm.size() > 1 && childNorm.back() == '/') childNorm.pop_back();
    if (childNorm == parentNorm) return allowEqual;
    if (childNorm.size() <= parentNorm.size()) return false;
    if (childNorm.compare(0, parentNorm.size(), parentNorm) != 0) return false;
    return parentNorm.back() == '/' || childNorm[parentNorm.size()] == '/';
#endif
}

} // namespace

FileSyncManager::FileSyncManager() = default;

FileSyncManager::~FileSyncManager() {
    reset();
}

std::string FileSyncManager::normalizePath(const std::string& p) {
    std::string s = p;
    for (char& c : s) {
        if (c == '\\') c = '/';
    }

    size_t start = 0;
    while (start < s.size()) {
        if (s[start] == ' ' || s[start] == '\t' || s[start] == '/') {
            start++;
            continue;
        }
        if (s[start] == '.' && (start + 1 < s.size()) && s[start + 1] == '/') {
            start += 2;
            continue;
        }
        break;
    }

    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '/')) {
        end--;
    }

    std::string result = s.substr(start, end - start);
    if (result.empty() || result == "." || result == ".." ||
        result.find(':') != std::string::npos || result.find("..") != std::string::npos) {
        return "";
    }
    // Reject any "." or empty segments inside the normalized path
    size_t segStart = 0;
    while (segStart <= result.size()) {
        size_t slash = result.find('/', segStart);
        size_t segEnd = (slash == std::string::npos) ? result.size() : slash;
        std::string seg = result.substr(segStart, segEnd - segStart);
        if (seg == "." || seg == "..") return "";
        if (slash == std::string::npos) break;
        segStart = slash + 1;
    }
    if (toFsPath(result).is_absolute()) {
        return "";
    }
    return result;
}

bool FileSyncManager::scanLocalDirectory(const std::string& rootPath, std::vector<SyncFileEntry>& outEntries) {
    outEntries.clear();
    std::error_code ec;
    std::filesystem::path root = toFsPath(rootPath);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return false;
    }

    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec
    );
    std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        if (isReparseOrSymlink(entry.path())) {
            it.disable_recursion_pending();
            it.increment(ec);
            continue;
        }
        auto rel = std::filesystem::relative(entry.path(), root, entryEc);
        if (!entryEc) {
            std::string relStr = normalizePath(fromFsPath(rel));
            if (!relStr.empty()) {
                SyncFileEntry e;
                e.relativePath = relStr;
                e.isDirectory = entry.is_directory(entryEc) ? 1 : 0;
                if (!e.isDirectory) {
                    e.sizeBytes = entry.file_size(entryEc);
                    if (entryEc) e.sizeBytes = 0;
                } else {
                    e.sizeBytes = 0;
                }

                auto ftime = entry.last_write_time(entryEc);
                if (!entryEc) {
                    auto sTime = std::chrono::file_clock::to_sys(ftime);
                    auto sec = std::chrono::duration_cast<std::chrono::seconds>(sTime.time_since_epoch()).count();
                    e.mtimeSec = (sec > 0) ? static_cast<uint64_t>(sec) : 0ULL;
                } else {
                    e.mtimeSec = 0;
                }
                outEntries.push_back(std::move(e));
            }
        }
        it.increment(ec);
    }

    std::sort(outEntries.begin(), outEntries.end(), [](const SyncFileEntry& a, const SyncFileEntry& b) {
        return a.relativePath < b.relativePath;
    });
    return true;
}

bool FileSyncManager::computeBlockHashes(
    const std::string& fullPath,
    uint64_t fileSize,
    std::vector<std::array<uint8_t, 32>>& outHashes)
{
    outHashes.clear();
    if (fileSize == 0) return true;
    if (fileSize > MAX_SYNC_FILE_BYTES) return false;

    std::ifstream is(toFsPath(fullPath), std::ios::binary);
    if (!is.is_open()) return false;

    size_t totalBlocks = static_cast<size_t>((fileSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE);
    outHashes.reserve(totalBlocks);

    std::vector<uint8_t> buffer(SYNC_BLOCK_SIZE);
    uint64_t remaining = fileSize;

    while (remaining > 0 && is) {
        size_t toRead = static_cast<size_t>(std::min<uint64_t>(SYNC_BLOCK_SIZE, remaining));
        is.read(reinterpret_cast<char*>(buffer.data()), toRead);
        std::streamsize bytesRead = is.gcount();
        if (bytesRead <= 0) break;
        outHashes.push_back(CryptoUtils::sha256(buffer.data(), static_cast<size_t>(bytesRead)));
        remaining -= static_cast<uint64_t>(bytesRead);
    }

    return (outHashes.size() == totalBlocks);
}

std::string FileSyncManager::formatSpeed(double bps) {
    if (bps <= 0.0) return "0 KB/s";
    char buf[64];
    if (bps >= 1024.0 * 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f GB/s", bps / (1024.0 * 1024.0 * 1024.0));
    } else if (bps >= 1024.0 * 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f MB/s", bps / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f KB/s", bps / 1024.0);
    }
    return buf;
}

std::string FileSyncManager::formatEta(double etaSec) {
    if (etaSec < 0.0 || !std::isfinite(etaSec) || etaSec > 86400.0) return "ETA --";
    int s = static_cast<int>(etaSec);
    char buf[64];
    if (s < 60) {
        std::snprintf(buf, sizeof(buf), "ETA %ds", s);
    } else if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "ETA %dm %02ds", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "ETA %dh %02dm", s / 3600, (s % 3600) / 60);
    }
    return buf;
}

void FileSyncManager::setLocalRoot(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    localRoot_ = path;
}

std::string FileSyncManager::localRoot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return localRoot_;
}

void FileSyncManager::setRemoteRoot(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteRoot_ = path;
}

std::string FileSyncManager::remoteRoot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remoteRoot_;
}

void FileSyncManager::setSyncMode(SyncMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
}

SyncMode FileSyncManager::syncMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

void FileSyncManager::setMirrorPurge(bool purge) {
    std::lock_guard<std::mutex> lock(mutex_);
    mirrorPurge_ = purge;
}

bool FileSyncManager::mirrorPurge() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mirrorPurge_;
}

SyncState FileSyncManager::syncState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void FileSyncManager::setSyncState(SyncState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

SyncMetrics FileSyncManager::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

SyncTelemetry FileSyncManager::telemetry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    SyncTelemetry t;
    t.state = state_;
    t.transferredBytes = transferredBytes_;
    t.totalBytes = totalBytes_;
    t.speedBps = smoothedBps_;
    if (smoothedBps_ > 1024.0 && transferredBytes_ < totalBytes_) {
        t.etaSeconds = static_cast<double>(totalBytes_ - transferredBytes_) / smoothedBps_;
    } else {
        t.etaSeconds = 0.0;
    }
    t.currentFileIndex = currentFileIndex_;
    t.totalFiles = totalFiles_;
    t.currentFileName = currentFileName_;
    t.errorMessage = errorMessage_;
    return t;
}

std::vector<SyncDiffItem> FileSyncManager::diffItems() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return diffItems_;
}

bool FileSyncManager::startLocalScan() {
    std::string root;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = localRoot_;
        state_ = SyncState::Scanning;
    }
    std::vector<SyncFileEntry> entries;
    bool ok = scanLocalDirectory(root, entries);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ok) {
            localEntries_ = std::move(entries);
        } else {
            state_ = SyncState::Failed;
            errorMessage_ = "Failed to scan local root: " + root;
        }
    }
    return ok;
}

void FileSyncManager::setLocalEntries(std::vector<SyncFileEntry> entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    localEntries_ = std::move(entries);
}

std::vector<SyncFileEntry> FileSyncManager::localEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return localEntries_;
}

void FileSyncManager::handleScanResp(const SyncScanRespPayload& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (resp.statusCode == 0) {
        remoteEntries_.clear();
        remoteEntries_.reserve(resp.entries.size());
        for (const auto& entry : resp.entries) {
            std::string norm = normalizePath(entry.relativePath);
            if (norm.empty()) continue;
            SyncFileEntry clean = entry;
            clean.relativePath = std::move(norm);
            remoteEntries_.push_back(std::move(clean));
        }
    } else {
        errorMessage_ = "Remote scan failed with status: " + std::to_string(resp.statusCode);
        state_ = SyncState::Failed;
    }
}

void FileSyncManager::setRemoteEntries(std::vector<SyncFileEntry> entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteEntries_ = std::move(entries);
}

std::vector<SyncFileEntry> FileSyncManager::remoteEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remoteEntries_;
}

void FileSyncManager::generateDiffPlan() {
    std::lock_guard<std::mutex> lock(mutex_);
    diffItems_.clear();
    metrics_ = SyncMetrics{};

    std::unordered_map<std::string, const SyncFileEntry*> localMap;
    std::unordered_map<std::string, const SyncFileEntry*> remoteMap;

    for (const auto& e : localEntries_) {
        localMap[e.relativePath] = &e;
    }
    for (const auto& e : remoteEntries_) {
        remoteMap[e.relativePath] = &e;
    }

    std::unordered_map<std::string, bool> allPaths;
    for (const auto& kv : localMap) allPaths[kv.first] = true;
    for (const auto& kv : remoteMap) allPaths[kv.first] = true;

    auto timeMatch = [](uint64_t t1, uint64_t t2) {
        return (t1 >= t2 ? (t1 - t2) : (t2 - t1)) <= 2ULL;
    };

    for (const auto& kv : allPaths) {
        const std::string& path = kv.first;
        auto lit = localMap.find(path);
        auto rit = remoteMap.find(path);

        bool hasLocal = (lit != localMap.end());
        bool hasRemote = (rit != remoteMap.end());

        SyncDiffItem item;
        item.relativePath = path;
        item.localSize = hasLocal ? lit->second->sizeBytes : 0;
        item.remoteSize = hasRemote ? rit->second->sizeBytes : 0;
        item.localMtime = hasLocal ? lit->second->mtimeSec : 0;
        item.remoteMtime = hasRemote ? rit->second->mtimeSec : 0;
        item.isDirectory = hasLocal ? (lit->second->isDirectory != 0) : (rit->second->isDirectory != 0);

        if (mode_ == SyncMode::PushMirror) {
            if (hasLocal && !hasRemote) {
                item.status = SyncDiffStatus::NewLocal;
            } else if (hasLocal && hasRemote) {
                if (item.isDirectory) {
                    item.status = SyncDiffStatus::Match;
                } else if (item.localSize == item.remoteSize && timeMatch(item.localMtime, item.remoteMtime)) {
                    item.status = SyncDiffStatus::Match;
                } else {
                    item.status = SyncDiffStatus::ModifiedLocal;
                }
            } else if (!hasLocal && hasRemote) {
                if (mirrorPurge_) {
                    item.status = SyncDiffStatus::DeleteTarget;
                } else {
                    item.status = SyncDiffStatus::Match;
                }
            }
        } else if (mode_ == SyncMode::PullMirror) {
            if (!hasLocal && hasRemote) {
                item.status = SyncDiffStatus::NewRemote;
            } else if (hasLocal && hasRemote) {
                if (item.isDirectory) {
                    item.status = SyncDiffStatus::Match;
                } else if (item.localSize == item.remoteSize && timeMatch(item.localMtime, item.remoteMtime)) {
                    item.status = SyncDiffStatus::Match;
                } else {
                    item.status = SyncDiffStatus::ModifiedRemote;
                }
            } else if (hasLocal && !hasRemote) {
                if (mirrorPurge_) {
                    item.status = SyncDiffStatus::DeleteTarget;
                } else {
                    item.status = SyncDiffStatus::Match;
                }
            }
        } else { // TwoWay
            if (hasLocal && !hasRemote) {
                item.status = SyncDiffStatus::NewLocal;
            } else if (!hasLocal && hasRemote) {
                item.status = SyncDiffStatus::NewRemote;
            } else if (hasLocal && hasRemote) {
                if (item.isDirectory) {
                    item.status = SyncDiffStatus::Match;
                } else if (item.localSize == item.remoteSize && timeMatch(item.localMtime, item.remoteMtime)) {
                    item.status = SyncDiffStatus::Match;
                } else if (item.localMtime >= item.remoteMtime) {
                    item.status = SyncDiffStatus::ModifiedLocal;
                } else {
                    item.status = SyncDiffStatus::ModifiedRemote;
                }
            }
        }

        if (!item.isDirectory) {
            metrics_.totalFiles++;
            if (item.status == SyncDiffStatus::NewLocal) {
                metrics_.newCount++;
                metrics_.bytesToTransfer += item.localSize;
            } else if (item.status == SyncDiffStatus::NewRemote) {
                metrics_.newCount++;
                metrics_.bytesToTransfer += item.remoteSize;
            } else if (item.status == SyncDiffStatus::ModifiedLocal) {
                metrics_.modCount++;
                metrics_.bytesToTransfer += item.localSize;
            } else if (item.status == SyncDiffStatus::ModifiedRemote) {
                metrics_.modCount++;
                metrics_.bytesToTransfer += item.remoteSize;
            } else if (item.status == SyncDiffStatus::DeleteTarget) {
                metrics_.delCount++;
            } else {
                metrics_.matchCount++;
            }
        } else {
            if (item.status == SyncDiffStatus::NewLocal || item.status == SyncDiffStatus::NewRemote) {
                metrics_.newCount++;
            } else if (item.status == SyncDiffStatus::DeleteTarget) {
                metrics_.delCount++;
            } else {
                metrics_.matchCount++;
            }
        }

        item.sizeBytes = (item.localSize > 0) ? item.localSize : item.remoteSize;
        if (item.isDirectory) {
            if (item.status == SyncDiffStatus::DeleteTarget) {
                item.action = SyncActionType::DeleteDir;
            } else {
                item.action = SyncActionType::CreateDir;
            }
        } else {
            if (item.status == SyncDiffStatus::DeleteTarget) {
                item.action = SyncActionType::DeleteFile;
            } else if (item.status == SyncDiffStatus::NewRemote || item.status == SyncDiffStatus::ModifiedRemote) {
                item.action = SyncActionType::RequestFile;
            } else {
                item.action = SyncActionType::CreateDir;
            }
        }

        diffItems_.push_back(std::move(item));
    }

    std::sort(diffItems_.begin(), diffItems_.end(), [](const SyncDiffItem& a, const SyncDiffItem& b) {
        return a.relativePath < b.relativePath;
    });

    state_ = SyncState::DiffReady;
}

std::vector<SyncDiffItem> FileSyncManager::generateDiffPlan(
    const std::vector<SyncFileEntry>& localEntries,
    const std::vector<SyncFileEntry>& remoteEntries,
    SyncMode mode,
    bool purge)
{
    FileSyncManager mgr;
    mgr.setLocalEntries(localEntries);
    mgr.setRemoteEntries(remoteEntries);
    mgr.setSyncMode(mode);
    mgr.setMirrorPurge(purge);
    mgr.generateDiffPlan();
    return mgr.diffItems();
}

std::string FileSyncManager::resolveTargetRoot() const {
    if (!stagingRootOverride_.empty()) return stagingRootOverride_;
    if (mode_ == SyncMode::PullMirror) {
        return localRoot_.empty() ? remoteRoot_ : localRoot_;
    }
    return remoteRoot_.empty() ? localRoot_ : remoteRoot_;
}

void FileSyncManager::setStagingRootOverride(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    stagingRootOverride_ = path;
}

std::string FileSyncManager::stagingRootOverride() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stagingRootOverride_;
}

bool FileSyncManager::startSync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SyncState::DiffReady && state_ != SyncState::Idle) {
        return false;
    }

    state_ = SyncState::Syncing;
    activeSyncId_ = generateSyncId();
    transferredBytes_ = 0;
    totalBytes_ = metrics_.bytesToTransfer;
    currentFileIndex_ = 0;
    totalFiles_ = metrics_.newCount + metrics_.modCount + metrics_.delCount;
    syncStartTickMs_ = GetTickCount64();
    lastTelemetryTickMs_ = syncStartTickMs_;
    smoothedBps_ = 0.0;
    lastSpeedBps_ = 0.0;
    errorMessage_.clear();

    pendingActions_.clear();
    outgoingFiles_.clear();
    removePartFilesLocked();
    incomingFiles_.clear();

    uint32_t actionIdGen = 100;
    std::filesystem::path localRootP = localRoot_.empty() ? std::filesystem::path() : toFsPath(localRoot_);

    for (const auto& item : diffItems_) {
        std::string norm = normalizePath(item.relativePath);
        if (norm.empty()) continue;

        if (mode_ == SyncMode::PushMirror) {
            if (item.isDirectory) {
                if (item.status == SyncDiffStatus::NewLocal) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::CreateDir);
                    act.targetPath = norm;
                    pendingActions_.push_back(std::move(act));
                } else if (item.status == SyncDiffStatus::DeleteTarget) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::DeleteDir);
                    act.targetPath = norm;
                    pendingActions_.push_back(std::move(act));
                }
            } else {
                if (item.status == SyncDiffStatus::DeleteTarget) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::DeleteFile);
                    act.targetPath = norm;
                    pendingActions_.push_back(std::move(act));
                } else if (item.status == SyncDiffStatus::NewLocal || item.status == SyncDiffStatus::ModifiedLocal) {
                    OutgoingFileTask task;
                    task.fileId = nextFileId_++;
                    task.relativePath = norm;
                    task.fullSourcePath = localRootP / toFsPath(norm);
                    task.totalSize = item.localSize;
                    task.mtimeSec = item.localMtime;
                    task.totalBlocks = (item.localSize > 0) ? static_cast<uint32_t>((item.localSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
                    pathToFileIdMap_[task.relativePath] = task.fileId;
                    outgoingFiles_.push_back(std::move(task));
                }
            }
        } else if (mode_ == SyncMode::PullMirror) {
            std::filesystem::path fullLocal = localRootP / toFsPath(norm);
            if (item.isDirectory) {
                if (item.status == SyncDiffStatus::NewRemote) {
                    if (!localRootP.empty() && isPathInside(fullLocal, localRootP, false)) {
                        std::error_code ec;
                        std::filesystem::create_directories(fullLocal, ec);
                    }
                } else if (item.status == SyncDiffStatus::DeleteTarget) {
                    if (!localRootP.empty() && isPathInside(fullLocal, localRootP, false)) {
                        std::error_code ec;
                        std::filesystem::remove_all(fullLocal, ec);
                    }
                }
            } else {
                if (item.status == SyncDiffStatus::DeleteTarget) {
                    if (!localRootP.empty() && isPathInside(fullLocal, localRootP, false)) {
                        std::error_code ec;
                        std::filesystem::remove(fullLocal, ec);
                    }
                } else if (item.status == SyncDiffStatus::NewRemote || item.status == SyncDiffStatus::ModifiedRemote) {
                    uint32_t fileId = nextFileId_++;
                    registerIncomingFileLocked(fileId, norm, item.remoteSize, item.remoteMtime, activeSyncId_);
                    SyncActionReqPayload act;
                    act.actionId = fileId;
                    act.actionType = static_cast<uint8_t>(SyncActionType::RequestFile);
                    act.targetPath = norm;
                    pendingActions_.push_back(std::move(act));
                }
            }
        } else { // TwoWay
            std::filesystem::path fullLocal = localRootP / toFsPath(norm);
            if (item.isDirectory) {
                if (item.status == SyncDiffStatus::NewLocal) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::CreateDir);
                    act.targetPath = norm;
                    pendingActions_.push_back(std::move(act));
                } else if (item.status == SyncDiffStatus::NewRemote) {
                    if (!localRootP.empty() && isPathInside(fullLocal, localRootP, false)) {
                        std::error_code ec;
                        std::filesystem::create_directories(fullLocal, ec);
                    }
                }
            } else {
                if (item.status == SyncDiffStatus::NewLocal || item.status == SyncDiffStatus::ModifiedLocal) {
                    OutgoingFileTask task;
                    task.fileId = nextFileId_++;
                    task.relativePath = norm;
                    task.fullSourcePath = localRootP / toFsPath(norm);
                    task.totalSize = item.localSize;
                    task.mtimeSec = item.localMtime;
                    task.totalBlocks = (item.localSize > 0) ? static_cast<uint32_t>((item.localSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
                    pathToFileIdMap_[task.relativePath] = task.fileId;
                    outgoingFiles_.push_back(std::move(task));
                } else if (item.status == SyncDiffStatus::NewRemote || item.status == SyncDiffStatus::ModifiedRemote) {
                    uint32_t fileId = nextFileId_++;
                    registerIncomingFileLocked(fileId, norm, item.remoteSize, item.remoteMtime, activeSyncId_);
                    SyncActionReqPayload act;
                    act.actionId = fileId;
                    act.actionType = static_cast<uint8_t>(SyncActionType::RequestFile);
                    act.targetPath = norm;
                    pendingActions_.push_back(std::move(act));
                }
            }
        }
    }

    if (outgoingFiles_.empty() && pendingActions_.empty()) {
        state_ = SyncState::Completed;
    }
    return true;
}

void FileSyncManager::removePartFilesLocked() {
    for (const auto& kv : incomingFiles_) {
        if (!kv.second.partFullPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(kv.second.partFullPath, ec);
        }
    }
}

void FileSyncManager::cancelSyncLocked() {
    state_ = SyncState::Cancelled;
    activeSyncId_ = 0;
    outgoingFiles_.clear();
    pendingActions_.clear();
    removePartFilesLocked();
    incomingFiles_.clear();
}

void FileSyncManager::cancelSync() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelSyncLocked();
}

void FileSyncManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SyncState::Idle;
    activeSyncId_ = 0;
    localEntries_.clear();
    remoteEntries_.clear();
    diffItems_.clear();
    metrics_ = SyncMetrics{};
    pendingActions_.clear();
    outgoingFiles_.clear();
    removePartFilesLocked();
    incomingFiles_.clear();
    pathToFileIdMap_.clear();
    transferredBytes_ = 0;
    totalBytes_ = 0;
    currentFileIndex_ = 0;
    totalFiles_ = 0;
    currentFileName_.clear();
    errorMessage_.clear();
}

bool FileSyncManager::computeDirtyBlockIndices(
    const std::filesystem::path& fullSourcePath,
    uint64_t totalSize,
    uint64_t targetFileSize,
    const std::vector<std::array<uint8_t, 32>>& targetHashes,
    std::vector<uint32_t>& outDirtyIndices)
{
    outDirtyIndices.clear();
    if (totalSize == 0) {
        outDirtyIndices.push_back(0);
        return true;
    }
    if (totalSize > MAX_SYNC_FILE_BYTES) return false;

    std::ifstream is(fullSourcePath, std::ios::binary);
    if (!is.is_open()) return false;

    std::vector<uint8_t> buffer(SYNC_BLOCK_SIZE);
    uint64_t remaining = totalSize;
    uint32_t blockIdx = 0;

    while (remaining > 0 && is) {
        size_t toRead = static_cast<size_t>(std::min<uint64_t>(SYNC_BLOCK_SIZE, remaining));
        is.read(reinterpret_cast<char*>(buffer.data()), toRead);
        std::streamsize bytesRead = is.gcount();
        if (bytesRead <= 0) return false;

        auto hash = CryptoUtils::sha256(buffer.data(), static_cast<size_t>(bytesRead));
        bool identical = (blockIdx < targetHashes.size() && hash == targetHashes[blockIdx]);
        if (!identical) {
            outDirtyIndices.push_back(blockIdx);
        }

        remaining -= static_cast<uint64_t>(bytesRead);
        blockIdx++;
    }
    if (remaining > 0) return false;

    // If the file shrank but all remaining blocks matched the prefix of targetHashes,
    // force one block so the receiver truncates the target file to totalSize.
    if (outDirtyIndices.empty() &&
        ((targetFileSize > 0 && targetFileSize != totalSize) || targetHashes.size() > blockIdx)) {
        outDirtyIndices.push_back(blockIdx > 0 ? blockIdx - 1 : 0);
    }
    return true;
}

bool FileSyncManager::pumpOutgoingSync(const SendPacketFn& sendPacket, int maxBlocks) {
    // 1. Drain pending action requests without holding mutex_ across sendPacket
    std::vector<SyncActionReqPayload> actionsToSend;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != SyncState::Syncing) return false;
        while (!pendingActions_.empty()) {
            actionsToSend.push_back(std::move(pendingActions_.front()));
            pendingActions_.pop_front();
        }
    }
    for (const auto& act : actionsToSend) {
        std::vector<uint8_t> payload;
        serializeSyncActionReq(act, payload);
        if (!sendPacket(PacketType::SYNC_ACTION_REQ, payload)) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = SyncState::Failed;
            errorMessage_ = "Failed to send sync action request";
            return false;
        }
    }

    // 2. Lazily stream outgoing file blocks without holding mutex_ across disk I/O or sendPacket
    int blocksSent = 0;
    while (blocksSent < maxBlocks) {
        enum class Step { Done, SendHashReq, ScanDirty, SendBlock };
        Step step = Step::Done;
        SyncHashReqPayload hashReq;
        std::filesystem::path sourcePath;
        uint64_t totalSize = 0;
        uint64_t targetFileSize = 0;
        uint32_t fileId = 0;
        uint32_t syncId = 0;
        uint32_t blockIdx = 0;
        uint32_t dirtyTotal = 0;
        std::vector<std::array<uint8_t, 32>> targetHashes;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != SyncState::Syncing || outgoingFiles_.empty()) {
                break;
            }
            auto& fileTask = outgoingFiles_.front();

            if (!fileTask.hashesRequested) {
                hashReq.syncId = activeSyncId_;
                hashReq.fileId = fileTask.fileId;
                hashReq.relativePath = fileTask.relativePath;
                fileTask.hashesRequested = true;
                step = Step::SendHashReq;
            } else if (!fileTask.hashesReceived) {
                // Waiting for peer SYNC_HASH_RESP; exit without spinning.
                break;
            } else if (!fileTask.scanDone) {
                fileId = fileTask.fileId;
                sourcePath = fileTask.fullSourcePath;
                totalSize = fileTask.totalSize;
                targetFileSize = fileTask.targetFileSize;
                targetHashes = fileTask.targetHashes;
                step = Step::ScanDirty;
            } else if (fileTask.dirtyCursor < fileTask.dirtyIndices.size()) {
                syncId = activeSyncId_;
                fileId = fileTask.fileId;
                sourcePath = fileTask.fullSourcePath;
                totalSize = fileTask.totalSize;
                blockIdx = fileTask.dirtyIndices[fileTask.dirtyCursor++];
                dirtyTotal = static_cast<uint32_t>(fileTask.dirtyIndices.size());
                if (fileTask.dirtyCursor >= fileTask.dirtyIndices.size()) {
                    outgoingFiles_.pop_front();
                }
                step = Step::SendBlock;
            } else {
                outgoingFiles_.pop_front();
                continue;
            }
        }

        if (step == Step::SendHashReq) {
            std::vector<uint8_t> payload;
            serializeSyncHashReq(hashReq, payload);
            if (!sendPacket(PacketType::SYNC_HASH_REQ, payload)) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = SyncState::Failed;
                errorMessage_ = "Failed to send sync hash request";
                return false;
            }
            break;
        }

        if (step == Step::ScanDirty) {
            std::vector<uint32_t> dirty;
            bool ok = computeDirtyBlockIndices(sourcePath, totalSize, targetFileSize, targetHashes, dirty);
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ != SyncState::Syncing || outgoingFiles_.empty() || outgoingFiles_.front().fileId != fileId) {
                break;
            }
            if (!ok) {
                state_ = SyncState::Failed;
                errorMessage_ = "Failed to read source file: " + fromFsPath(sourcePath);
                return false;
            }
            auto& fileTask = outgoingFiles_.front();
            fileTask.dirtyIndices = std::move(dirty);
            fileTask.dirtyCursor = 0;
            fileTask.scanDone = true;
            currentFileName_ = fileTask.relativePath;
            currentFileIndex_++;
            if (fileTask.dirtyIndices.empty()) {
                outgoingFiles_.pop_front();
            }
            continue;
        }

        if (step == Step::SendBlock) {
            SyncDeltaBlockPayload blockPayload;
            blockPayload.syncId = syncId;
            blockPayload.fileId = fileId;
            blockPayload.blockIndex = blockIdx;
            blockPayload.blockOffset = static_cast<uint64_t>(blockIdx) * SYNC_BLOCK_SIZE;
            blockPayload.totalFileSize = totalSize;
            blockPayload.totalBlocks = dirtyTotal;

            if (totalSize == 0) {
                blockPayload.blockHash = CryptoUtils::sha256("", 0);
            } else {
                std::ifstream is(sourcePath, std::ios::binary);
                if (!is.is_open()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_ = SyncState::Failed;
                    errorMessage_ = "Failed to open source file: " + fromFsPath(sourcePath);
                    return false;
                }
                is.seekg(static_cast<std::streamoff>(blockPayload.blockOffset), std::ios::beg);
                size_t toRead = static_cast<size_t>(std::min<uint64_t>(SYNC_BLOCK_SIZE, totalSize - blockPayload.blockOffset));
                blockPayload.blockData.resize(toRead);
                is.read(reinterpret_cast<char*>(blockPayload.blockData.data()), toRead);
                std::streamsize bytesRead = is.gcount();
                if (bytesRead <= 0 || static_cast<size_t>(bytesRead) != toRead) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_ = SyncState::Failed;
                    errorMessage_ = "Failed to read source file block: " + fromFsPath(sourcePath);
                    return false;
                }
                blockPayload.blockHash = CryptoUtils::sha256(blockPayload.blockData.data(), blockPayload.blockData.size());
            }

            const size_t blockBytes = blockPayload.blockData.size();
            std::vector<uint8_t> payload;
            serializeSyncDeltaBlock(blockPayload, payload);
            if (!sendPacket(PacketType::SYNC_DELTA_BLOCK, payload)) {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = SyncState::Failed;
                errorMessage_ = "Failed to send sync delta block";
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                transferredBytes_ += blockBytes;
            }
            blocksSent++;
        }
    }

    // 3. Periodic telemetry update (send outside mutex_)
    bool sendStatus = false;
    SyncStatusUpdatePayload status;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == SyncState::Syncing) {
            uint64_t now = GetTickCount64();
            bool allDone = outgoingFiles_.empty() && pendingActions_.empty() && incomingFiles_.empty();
            if (now - lastTelemetryTickMs_ >= 250 || allDone) {
                lastTelemetryTickMs_ = now;
                double elapsed = (now - syncStartTickMs_) / 1000.0;
                if (elapsed > 0.05) {
                    lastSpeedBps_ = static_cast<double>(transferredBytes_) / elapsed;
                    smoothedBps_ = (smoothedBps_ <= 0.0) ? lastSpeedBps_ : (smoothedBps_ * 0.7 + lastSpeedBps_ * 0.3);
                }

                status.syncId = activeSyncId_;
                status.transferredBytes = transferredBytes_;
                status.totalBytes = totalBytes_;
                status.currentFileIndex = currentFileIndex_;
                status.totalFiles = totalFiles_;
                status.currentFileName = currentFileName_;

                if (allDone) {
                    state_ = SyncState::Completed;
                    status.syncState = static_cast<uint8_t>(SyncState::Completed);
                } else {
                    status.syncState = static_cast<uint8_t>(SyncState::Syncing);
                }
                sendStatus = true;
            }
        }
    }

    if (sendStatus) {
        std::vector<uint8_t> payload;
        serializeSyncStatusUpdate(status, payload);
        if (!sendPacket(PacketType::SYNC_STATUS_UPDATE, payload)) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = SyncState::Failed;
            errorMessage_ = "Failed to send sync status update";
            return false;
        }
    }

    return (blocksSent > 0);
}

bool FileSyncManager::handleHashResp(const SyncHashRespPayload& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSyncId_ != 0 && resp.syncId != 0 && resp.syncId != activeSyncId_) {
        return false;
    }
    for (auto& task : outgoingFiles_) {
        if (task.fileId == resp.fileId) {
            task.targetHashes = resp.blockHashes;
            task.targetFileSize = resp.targetFileSize;
            task.hashesReceived = true;
            return true;
        }
    }
    return false;
}

void FileSyncManager::registerIncomingFileLocked(
    uint32_t fileId,
    const std::string& relativePath,
    uint64_t totalSize,
    uint64_t mtimeSec,
    uint32_t syncId)
{
    std::string norm = normalizePath(relativePath);
    if (norm.empty()) return;

    constexpr size_t MAX_INCOMING_FILES = 64;
    if (incomingFiles_.find(fileId) == incomingFiles_.end() && incomingFiles_.size() >= MAX_INCOMING_FILES) {
        auto oldestIt = incomingFiles_.begin();
        for (auto it = incomingFiles_.begin(); it != incomingFiles_.end(); ++it) {
            if (it->second.createdOrder < oldestIt->second.createdOrder) {
                oldestIt = it;
            }
        }
        if (!oldestIt->second.partFullPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(oldestIt->second.partFullPath, ec);
        }
        incomingFiles_.erase(oldestIt);
    }
    auto& in = incomingFiles_[fileId];
    in.syncId = syncId;
    in.fileId = fileId;
    in.createdOrder = nextIncomingOrder_++;
    in.relativePath = norm;
    in.totalSize = totalSize;
    in.mtimeSec = mtimeSec;
    uint32_t tb = (totalSize > 0) ? static_cast<uint32_t>((totalSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
    if (tb > MAX_BLOCKS_PER_FILE) tb = MAX_BLOCKS_PER_FILE;
    in.totalBlocks = tb;
    in.receivedBlocks = 0;
    in.blockReceived.assign(in.totalBlocks, false);
    in.initialized = false;
    pathToFileIdMap_[in.relativePath] = fileId;
}

void FileSyncManager::registerIncomingFile(
    uint32_t fileId,
    const std::string& relativePath,
    uint64_t totalSize,
    uint64_t mtimeSec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    registerIncomingFileLocked(fileId, relativePath, totalSize, mtimeSec, activeSyncId_);
}

void FileSyncManager::queueOutgoingFile(
    const std::string& relativePath,
    const std::string& sourceRoot,
    uint64_t totalSize,
    uint64_t mtimeSec,
    const std::vector<std::array<uint8_t, 32>>& targetHashes,
    uint32_t fileId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string norm = normalizePath(relativePath);
    if (norm.empty()) return;
    if (activeSyncId_ == 0) {
        activeSyncId_ = generateSyncId();
    }
    OutgoingFileTask t;
    t.fileId = (fileId != 0) ? fileId : nextFileId_++;
    t.relativePath = norm;
    t.fullSourcePath = toFsPath(sourceRoot) / toFsPath(norm);
    t.totalSize = totalSize;
    t.mtimeSec = mtimeSec;
    t.totalBlocks = (totalSize > 0) ? static_cast<uint32_t>((totalSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
    t.targetHashes = targetHashes;
    t.hashesRequested = true;
    t.hashesReceived = true;
    t.scanDone = false;
    pathToFileIdMap_[t.relativePath] = t.fileId;
    outgoingFiles_.push_back(std::move(t));
}

bool FileSyncManager::handleDeltaBlock(const SyncDeltaBlockPayload& block) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SyncState::Syncing) return false;
    if (block.syncId == 0) return false;
    if (activeSyncId_ == 0) {
        activeSyncId_ = block.syncId;
    } else if (block.syncId != activeSyncId_) {
        return false;
    }

    if (block.totalFileSize > MAX_SYNC_FILE_BYTES) return false;
    if (block.blockData.size() > SYNC_BLOCK_SIZE) return false;
    if (block.blockIndex >= MAX_BLOCKS_PER_FILE) return false;
    if (block.totalBlocks == 0 || block.totalBlocks > MAX_BLOCKS_PER_FILE) return false;
    if (block.blockOffset != static_cast<uint64_t>(block.blockIndex) * SYNC_BLOCK_SIZE) return false;

    if (block.totalFileSize == 0) {
        if (block.blockIndex != 0 || !block.blockData.empty() || block.totalBlocks != 1) {
            return false;
        }
    } else {
        uint32_t maxFileBlocks = static_cast<uint32_t>((block.totalFileSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE);
        if (block.blockIndex >= maxFileBlocks || block.totalBlocks > maxFileBlocks) {
            return false;
        }
        if (block.blockOffset >= block.totalFileSize ||
            block.blockData.size() > block.totalFileSize - block.blockOffset) {
            return false;
        }
    }

    // 1. Verify CNG SHA-256 integrity of the payload
    auto calculated = CryptoUtils::sha256(block.blockData.data(), block.blockData.size());
    if (calculated != block.blockHash) {
        errorMessage_ = "Block SHA-256 digest validation failed for fileId " + std::to_string(block.fileId);
        return false;
    }

    // Require explicit fileId registration (never guess target path by file size)
    auto it = incomingFiles_.find(block.fileId);
    if (it == incomingFiles_.end()) {
        std::string mappedPath;
        for (const auto& kv : pathToFileIdMap_) {
            if (kv.second == block.fileId) {
                mappedPath = kv.first;
                break;
            }
        }
        if (mappedPath.empty()) {
            errorMessage_ = "Unregistered fileId in delta block: " + std::to_string(block.fileId);
            return false;
        }
        registerIncomingFileLocked(block.fileId, mappedPath, block.totalFileSize, 0, block.syncId);
        it = incomingFiles_.find(block.fileId);
        if (it == incomingFiles_.end()) return false;
    }

    auto& in = it->second;

    if (in.initialized) {
        if (block.totalFileSize != in.totalSize || block.totalBlocks != in.totalBlocks) {
            errorMessage_ = "Mismatched file metadata mid-transfer for fileId " + std::to_string(block.fileId);
            return false;
        }
    } else {
        in.syncId = block.syncId;
        in.fileId = block.fileId;
        in.totalSize = block.totalFileSize;
        in.totalBlocks = block.totalBlocks;
        in.receivedBlocks = 0;
        in.blockReceived.clear();

        in.relativePath = normalizePath(in.relativePath);
        if (in.relativePath.empty()) {
            errorMessage_ = "Invalid relative path for fileId " + std::to_string(block.fileId);
            return false;
        }

        std::string root = resolveTargetRoot();
        if (root.empty()) {
            errorMessage_ = "Target root directory not configured";
            return false;
        }
        std::filesystem::path rootP = toFsPath(root);
        std::filesystem::path targetP = rootP / toFsPath(in.relativePath);

        if (!isPathInside(targetP, rootP, false)) {
            errorMessage_ = "Path traversal rejected: " + in.relativePath;
            return false;
        }

        std::filesystem::path partP = targetP;
        partP += ".part";
        if (!isPathInside(partP, rootP, false)) {
            errorMessage_ = "Staging path rejected: " + in.relativePath;
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(targetP.parent_path(), ec);
        if (ec) {
            errorMessage_ = "Failed to create target directory: " + ec.message();
            return false;
        }
        // Re-verify containment after directory creation in case of symlink swap
        if (!isPathInside(targetP, rootP, false) || !isPathInside(partP, rootP, false)) {
            errorMessage_ = "Path traversal rejected after mkdir: " + in.relativePath;
            return false;
        }

        in.targetFullPath = targetP;
        in.partFullPath = partP;

        if (std::filesystem::exists(targetP, ec) && !std::filesystem::is_directory(targetP, ec) &&
            !isReparseOrSymlink(targetP)) {
            std::filesystem::copy_file(targetP, in.partFullPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                errorMessage_ = "Failed to prepare staging file: " + ec.message();
                return false;
            }
        } else {
            std::ofstream ofs(in.partFullPath, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                errorMessage_ = "Failed to create staging file";
                return false;
            }
            ofs.close();
        }

        in.initialized = true;
    }

    // 2. Seek and write delta block payload into staging .part file
    {
#if defined(_WIN32)
        FILE* fp = _wfopen(in.partFullPath.c_str(), L"r+b");
        if (!fp) {
            fp = _wfopen(in.partFullPath.c_str(), L"w+b");
        }
#else
        FILE* fp = fopen(in.partFullPath.c_str(), "r+b");
        if (!fp) {
            fp = fopen(in.partFullPath.c_str(), "w+b");
        }
#endif
        if (!fp) {
            errorMessage_ = "Failed to open staging file: " + fromFsPath(in.partFullPath);
            return false;
        }

#if defined(_WIN32)
        if (_fseeki64(fp, static_cast<long long>(block.blockOffset), SEEK_SET) != 0) {
#else
        if (fseeko(fp, static_cast<off_t>(block.blockOffset), SEEK_SET) != 0) {
#endif
            fclose(fp);
            errorMessage_ = "Failed to seek staging file: " + fromFsPath(in.partFullPath);
            return false;
        }

        if (!block.blockData.empty()) {
            size_t written = fwrite(block.blockData.data(), 1, block.blockData.size(), fp);
            if (written != block.blockData.size()) {
                fclose(fp);
                errorMessage_ = "Failed to write staging file block: " + fromFsPath(in.partFullPath);
                return false;
            }
        }
        fflush(fp);
        fclose(fp);
    }

    if (block.blockIndex >= in.blockReceived.size()) {
        in.blockReceived.resize(static_cast<size_t>(block.blockIndex) + 1u, false);
    }
    if (!in.blockReceived[block.blockIndex]) {
        in.blockReceived[block.blockIndex] = true;
        in.receivedBlocks++;
    }

    // 3. Finalize only once all expected dirty blocks have arrived
    if (in.receivedBlocks >= in.totalBlocks) {
        std::error_code ec;
        std::filesystem::resize_file(in.partFullPath, in.totalSize, ec);
        if (ec) {
            errorMessage_ = "Failed to truncate staging file: " + ec.message();
            return false;
        }

        bool renamed = false;
#if defined(_WIN32)
        if (MoveFileExW(in.partFullPath.c_str(), in.targetFullPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            renamed = true;
        } else {
            std::filesystem::rename(in.partFullPath, in.targetFullPath, ec);
            renamed = !ec;
        }
#else
        std::filesystem::rename(in.partFullPath, in.targetFullPath, ec);
        renamed = !ec;
#endif
        if (!renamed) {
            errorMessage_ = "Failed to replace target file: " + in.relativePath;
            return false;
        }

#if defined(_WIN32)
        if (in.mtimeSec > 0) {
            HANDLE hFile = CreateFileW(
                in.targetFullPath.c_str(),
                FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
            if (hFile != INVALID_HANDLE_VALUE) {
                FILETIME ft{};
                uint64_t fileTime = (in.mtimeSec * 10000000ULL) + 116444736000000000ULL;
                ft.dwLowDateTime = static_cast<DWORD>(fileTime & 0xFFFFFFFF);
                ft.dwHighDateTime = static_cast<DWORD>(fileTime >> 32);
                SetFileTime(hFile, nullptr, nullptr, &ft);
                CloseHandle(hFile);
            }
        }
#endif
        incomingFiles_.erase(block.fileId);
    }

    return true;
}

bool FileSyncManager::handleStatusUpdate(const SyncStatusUpdatePayload& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status.syncState > static_cast<uint8_t>(SyncState::Cancelled)) {
        return false;
    }
    if (status.syncId == 0) {
        return false;
    }
    if (activeSyncId_ == 0 || state_ != SyncState::Syncing) {
        activeSyncId_ = status.syncId;
    } else if (status.syncId != activeSyncId_) {
        return false;
    }
    state_ = static_cast<SyncState>(status.syncState);
    transferredBytes_ = status.transferredBytes;
    totalBytes_ = status.totalBytes;
    currentFileIndex_ = status.currentFileIndex;
    totalFiles_ = status.totalFiles;
    currentFileName_ = status.currentFileName;
    errorMessage_ = status.errorMessage;
    return true;
}

bool FileSyncManager::handleActionReq(const SyncActionReqPayload& req, SyncActionRespPayload& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    resp.actionId = req.actionId;
    resp.actionType = req.actionType;
    resp.statusCode = 0;
    resp.message = "OK";

    // Handle AbortSync before requiring target root or path validation
    if (static_cast<SyncActionType>(req.actionType) == SyncActionType::AbortSync) {
        cancelSyncLocked();
        return true;
    }

    std::string norm = normalizePath(req.targetPath);
    if (norm.empty()) {
        resp.statusCode = 1;
        resp.message = "Invalid path";
        return false;
    }

    std::string root = resolveTargetRoot();
    if (root.empty()) {
        resp.statusCode = 2; // AccessDenied
        resp.message = "Target root directory not configured";
        return false;
    }
    std::filesystem::path rootP = toFsPath(root);
    std::filesystem::path full = rootP / toFsPath(norm);
    if (!isPathInside(full, rootP, false)) {
        resp.statusCode = 2; // AccessDenied
        resp.message = "Path traversal rejected";
        return false;
    }
    std::error_code ec;

    switch (static_cast<SyncActionType>(req.actionType)) {
        case SyncActionType::CreateDir:
            std::filesystem::create_directories(full, ec);
            if (ec) { resp.statusCode = 1; resp.message = ec.message(); }
            break;
        case SyncActionType::DeleteFile:
            std::filesystem::remove(full, ec);
            if (ec) { resp.statusCode = 1; resp.message = ec.message(); }
            break;
        case SyncActionType::DeleteDir:
            std::filesystem::remove_all(full, ec);
            if (ec) { resp.statusCode = 1; resp.message = ec.message(); }
            break;
        case SyncActionType::RequestFile: {
            if (std::filesystem::exists(full, ec) && !ec &&
                !std::filesystem::is_directory(full, ec) && !ec &&
                !isReparseOrSymlink(full)) {
                uint64_t sz = std::filesystem::file_size(full, ec);
                if (ec || sz > MAX_SYNC_FILE_BYTES) {
                    resp.statusCode = 1;
                    resp.message = ec ? ec.message() : "File exceeds maximum sync size";
                    break;
                }
                auto ftime = std::filesystem::last_write_time(full, ec);
                uint64_t mt = 0;
                if (!ec) {
                    auto sTime = std::chrono::file_clock::to_sys(ftime);
                    auto sec = std::chrono::duration_cast<std::chrono::seconds>(sTime.time_since_epoch()).count();
                    mt = (sec > 0) ? static_cast<uint64_t>(sec) : 0ULL;
                }
                if (activeSyncId_ == 0) {
                    activeSyncId_ = generateSyncId();
                }
                OutgoingFileTask t;
                t.fileId = (req.actionId != 0) ? req.actionId : nextFileId_++;
                t.relativePath = norm;
                t.fullSourcePath = full;
                t.totalSize = sz;
                t.mtimeSec = mt;
                t.totalBlocks = (sz > 0) ? static_cast<uint32_t>((sz + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
                t.hashesRequested = true;
                t.hashesReceived = true;
                t.scanDone = false;
                outgoingFiles_.push_back(std::move(t));
                state_ = SyncState::Syncing;
            } else {
                resp.statusCode = 1;
                resp.message = "File not found";
            }
            break;
        }
        default:
            resp.statusCode = 1;
            resp.message = "Unknown action";
            break;
    }

    return (resp.statusCode == 0);
}

bool FileSyncManager::handleActionResp(const SyncActionRespPayload& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (resp.statusCode != 0) {
        errorMessage_ = "Remote action " + std::to_string(resp.actionId) + " failed: " + resp.message;
    }
    return true;
}

bool FileSyncManager::handleScanReqHost(const SyncScanReqPayload& req, SyncScanRespPayload& resp) {
    resp.scanId = req.scanId;
    resp.rootPath = req.rootPath;
    std::string cfgRoot = localRoot();
    if (cfgRoot.empty()) {
        resp.statusCode = 2; // AccessDenied
        return false;
    }
    std::string scanRoot = req.rootPath.empty() ? cfgRoot : req.rootPath;
    std::filesystem::path scanRootP = toFsPath(scanRoot);
    std::filesystem::path cfgRootP = toFsPath(cfgRoot);
    if (!isPathInside(scanRootP, cfgRootP, true)) {
        resp.statusCode = 2; // AccessDenied
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(scanRootP, ec) || ec ||
        !std::filesystem::is_directory(scanRootP, ec) || ec) {
        resp.statusCode = 1; // DirectoryNotFound
        return false;
    }

    if (!scanLocalDirectory(scanRoot, resp.entries)) {
        resp.statusCode = 3; // Error
        return false;
    }
    resp.statusCode = 0;
    return true;
}

bool FileSyncManager::handleHashReqHost(const SyncHashReqPayload& req, SyncHashRespPayload& resp) {
    resp.syncId = req.syncId;
    resp.fileId = req.fileId;
    std::string root = localRoot().empty() ? remoteRoot() : localRoot();
    if (root.empty()) {
        resp.targetFileSize = 0;
        resp.blockHashes.clear();
        return false;
    }

    std::string norm = normalizePath(req.relativePath);
    if (norm.empty()) {
        resp.targetFileSize = 0;
        resp.blockHashes.clear();
        return false;
    }

    std::filesystem::path rootP = toFsPath(root);
    std::filesystem::path full = rootP / toFsPath(norm);
    if (!isPathInside(full, rootP, false)) {
        resp.targetFileSize = 0;
        resp.blockHashes.clear();
        return false;
    }

    // Adopt peer syncId and transition into receiving state, then register fileId mapping
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (req.syncId != 0) {
            activeSyncId_ = req.syncId;
        }
        state_ = SyncState::Syncing;
        registerIncomingFileLocked(req.fileId, norm, 0, 0, req.syncId);
    }

    std::error_code ec;
    if (!std::filesystem::exists(full, ec) || ec ||
        std::filesystem::is_directory(full, ec) || ec ||
        isReparseOrSymlink(full)) {
        resp.targetFileSize = 0;
        resp.blockHashes.clear();
        return true;
    }

    uint64_t sz = std::filesystem::file_size(full, ec);
    if (ec || sz > MAX_SYNC_FILE_BYTES) {
        resp.targetFileSize = 0;
        resp.blockHashes.clear();
        return false;
    }

    resp.targetFileSize = sz;
    computeBlockHashes(fromFsPath(full), sz, resp.blockHashes);
    return true;
}

} // namespace cppdesk
