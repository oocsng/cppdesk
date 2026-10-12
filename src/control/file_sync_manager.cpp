#include "file_sync_manager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef DeleteFile
#undef DeleteFile
#endif

#include <cmath>
#include <cstdio>
#include <chrono>
#include <algorithm>

namespace cppdesk {

namespace {

std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return std::wstring();
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, w.data(), len);
    return w;
}

} // namespace

FileSyncManager::FileSyncManager() {
    activeSyncId_ = static_cast<uint32_t>(GetTickCount64());
}

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
    if (result.empty() || result.find(':') != std::string::npos || result.find("..") != std::string::npos) {
        return "";
    }
    if (std::filesystem::path(result).is_absolute()) {
        return "";
    }
    return result;
}

bool FileSyncManager::scanLocalDirectory(const std::string& rootPath, std::vector<SyncFileEntry>& outEntries) {
    outEntries.clear();
    std::error_code ec;
    std::filesystem::path root(rootPath);
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
        auto rel = std::filesystem::relative(entry.path(), root, entryEc);
        if (!entryEc) {
            std::string relStr = normalizePath(rel.generic_string());
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
                    e.mtimeSec = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::seconds>(sTime.time_since_epoch()).count()
                    );
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

    std::ifstream is(fullPath, std::ios::binary);
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

const std::vector<SyncFileEntry>& FileSyncManager::localEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return localEntries_;
}

void FileSyncManager::handleScanResp(const SyncScanRespPayload& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (resp.statusCode == 0) {
        remoteEntries_ = resp.entries;
    } else {
        errorMessage_ = "Remote scan failed with status: " + std::to_string(resp.statusCode);
        state_ = SyncState::Failed;
    }
}

void FileSyncManager::setRemoteEntries(std::vector<SyncFileEntry> entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    remoteEntries_ = std::move(entries);
}

const std::vector<SyncFileEntry>& FileSyncManager::remoteEntries() const {
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
        int64_t diff = static_cast<int64_t>(t1) - static_cast<int64_t>(t2);
        return std::abs(diff) <= 2;
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
    activeSyncId_ = static_cast<uint32_t>(GetTickCount64());
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
    pendingBlocks_.clear();
    incomingFiles_.clear();

    uint32_t actionIdGen = 100;

    for (const auto& item : diffItems_) {
        if (mode_ == SyncMode::PushMirror) {
            if (item.isDirectory) {
                if (item.status == SyncDiffStatus::NewLocal) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::CreateDir);
                    act.targetPath = item.relativePath;
                    pendingActions_.push_back(std::move(act));
                } else if (item.status == SyncDiffStatus::DeleteTarget) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::DeleteDir);
                    act.targetPath = item.relativePath;
                    pendingActions_.push_back(std::move(act));
                }
            } else {
                if (item.status == SyncDiffStatus::DeleteTarget) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::DeleteFile);
                    act.targetPath = item.relativePath;
                    pendingActions_.push_back(std::move(act));
                } else if (item.status == SyncDiffStatus::NewLocal || item.status == SyncDiffStatus::ModifiedLocal) {
                    OutgoingFileTask task;
                    task.fileId = nextFileId_++;
                    task.relativePath = item.relativePath;
                    task.fullSourcePath = (std::filesystem::path(localRoot_) / item.relativePath).string();
                    task.totalSize = item.localSize;
                    task.mtimeSec = item.localMtime;
                    task.totalBlocks = (item.localSize > 0) ? static_cast<uint32_t>((item.localSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
                    if (item.status == SyncDiffStatus::NewLocal) {
                        task.hashesRequested = true;
                        task.hashesReceived = true;
                    }
                    pathToFileIdMap_[task.relativePath] = task.fileId;
                    outgoingFiles_.push_back(std::move(task));
                }
            }
        } else if (mode_ == SyncMode::PullMirror) {
            if (item.isDirectory) {
                if (item.status == SyncDiffStatus::NewRemote) {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(localRoot_) / item.relativePath, ec);
                } else if (item.status == SyncDiffStatus::DeleteTarget) {
                    std::error_code ec;
                    std::filesystem::remove_all(std::filesystem::path(localRoot_) / item.relativePath, ec);
                }
            } else {
                if (item.status == SyncDiffStatus::DeleteTarget) {
                    std::error_code ec;
                    std::filesystem::remove(std::filesystem::path(localRoot_) / item.relativePath, ec);
                } else if (item.status == SyncDiffStatus::NewRemote || item.status == SyncDiffStatus::ModifiedRemote) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::RequestFile);
                    act.targetPath = item.relativePath;
                    pendingActions_.push_back(std::move(act));
                }
            }
        } else { // TwoWay
            if (item.isDirectory) {
                if (item.status == SyncDiffStatus::NewLocal) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::CreateDir);
                    act.targetPath = item.relativePath;
                    pendingActions_.push_back(std::move(act));
                } else if (item.status == SyncDiffStatus::NewRemote) {
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(localRoot_) / item.relativePath, ec);
                }
            } else {
                if (item.status == SyncDiffStatus::NewLocal || item.status == SyncDiffStatus::ModifiedLocal) {
                    OutgoingFileTask task;
                    task.fileId = nextFileId_++;
                    task.relativePath = item.relativePath;
                    task.fullSourcePath = (std::filesystem::path(localRoot_) / item.relativePath).string();
                    task.totalSize = item.localSize;
                    task.mtimeSec = item.localMtime;
                    task.totalBlocks = (item.localSize > 0) ? static_cast<uint32_t>((item.localSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
                    if (item.status == SyncDiffStatus::NewLocal) {
                        task.hashesRequested = true;
                        task.hashesReceived = true;
                    }
                    pathToFileIdMap_[task.relativePath] = task.fileId;
                    outgoingFiles_.push_back(std::move(task));
                } else if (item.status == SyncDiffStatus::NewRemote || item.status == SyncDiffStatus::ModifiedRemote) {
                    SyncActionReqPayload act;
                    act.actionId = actionIdGen++;
                    act.actionType = static_cast<uint8_t>(SyncActionType::RequestFile);
                    act.targetPath = item.relativePath;
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

void FileSyncManager::cancelSync() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SyncState::Cancelled;
    pendingBlocks_.clear();
    outgoingFiles_.clear();
    pendingActions_.clear();

    for (const auto& kv : incomingFiles_) {
        if (!kv.second.partFullPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(kv.second.partFullPath, ec);
        }
    }
    incomingFiles_.clear();
}

void FileSyncManager::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = SyncState::Idle;
    localEntries_.clear();
    remoteEntries_.clear();
    diffItems_.clear();
    metrics_ = SyncMetrics{};
    pendingActions_.clear();
    outgoingFiles_.clear();
    pendingBlocks_.clear();
    incomingFiles_.clear();
    pathToFileIdMap_.clear();
    transferredBytes_ = 0;
    totalBytes_ = 0;
    currentFileIndex_ = 0;
    totalFiles_ = 0;
    currentFileName_.clear();
    errorMessage_.clear();
}

void FileSyncManager::queueBlocksForTask(OutgoingFileTask& task) {
    if (task.totalSize == 0) {
        OutgoingBlock b;
        b.syncId = activeSyncId_;
        b.fileId = task.fileId;
        b.blockIndex = 0;
        b.blockOffset = 0;
        b.totalFileSize = 0;
        b.totalBlocks = 1;
        b.blockHash = CryptoUtils::sha256("", 0);
        pendingBlocks_.push_back(std::move(b));
        return;
    }

    std::ifstream is(task.fullSourcePath, std::ios::binary);
    if (!is.is_open()) {
        errorMessage_ = "Failed to open source file: " + task.fullSourcePath;
        return;
    }

    std::vector<uint8_t> buffer(SYNC_BLOCK_SIZE);
    uint64_t remaining = task.totalSize;
    uint32_t blockIdx = 0;

    std::vector<OutgoingBlock> fileBlocks;
    while (remaining > 0 && is) {
        size_t toRead = static_cast<size_t>(std::min<uint64_t>(SYNC_BLOCK_SIZE, remaining));
        is.read(reinterpret_cast<char*>(buffer.data()), toRead);
        std::streamsize bytesRead = is.gcount();
        if (bytesRead <= 0) break;

        auto hash = CryptoUtils::sha256(buffer.data(), static_cast<size_t>(bytesRead));

        bool identical = false;
        if (blockIdx < task.targetHashes.size()) {
            if (hash == task.targetHashes[blockIdx]) {
                identical = true;
            }
        }

        if (!identical) {
            OutgoingBlock b;
            b.syncId = activeSyncId_;
            b.fileId = task.fileId;
            b.blockIndex = blockIdx;
            b.blockOffset = static_cast<uint64_t>(blockIdx) * SYNC_BLOCK_SIZE;
            b.totalFileSize = task.totalSize;
            b.blockHash = hash;
            b.blockData.assign(buffer.begin(), buffer.begin() + bytesRead);
            fileBlocks.push_back(std::move(b));
        }

        remaining -= static_cast<uint64_t>(bytesRead);
        blockIdx++;
    }

    uint32_t dirtyBlocksCount = static_cast<uint32_t>(fileBlocks.size());
    for (auto& b : fileBlocks) {
        b.totalBlocks = dirtyBlocksCount;
        pendingBlocks_.push_back(std::move(b));
    }
}

bool FileSyncManager::pumpOutgoingSync(const SendPacketFn& sendPacket, int maxBlocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != SyncState::Syncing) return false;

    // 1. Dispatch pending action requests
    while (!pendingActions_.empty()) {
        auto act = std::move(pendingActions_.front());
        pendingActions_.pop_front();
        std::vector<uint8_t> payload;
        serializeSyncActionReq(act, payload);
        sendPacket(PacketType::SYNC_ACTION_REQ, payload);
    }

    // 2. Dispatch outgoing file blocks
    int blocksSent = 0;
    while (blocksSent < maxBlocks) {
        if (!pendingBlocks_.empty()) {
            auto blk = std::move(pendingBlocks_.front());
            pendingBlocks_.pop_front();
            SyncDeltaBlockPayload blockPayload;
            blockPayload.syncId = blk.syncId;
            blockPayload.fileId = blk.fileId;
            blockPayload.blockIndex = blk.blockIndex;
            blockPayload.blockOffset = blk.blockOffset;
            blockPayload.totalFileSize = blk.totalFileSize;
            blockPayload.totalBlocks = blk.totalBlocks;
            blockPayload.blockHash = blk.blockHash;
            blockPayload.blockData = std::move(blk.blockData);
            std::vector<uint8_t> payload;
            serializeSyncDeltaBlock(blockPayload, payload);
            sendPacket(PacketType::SYNC_DELTA_BLOCK, payload);
            transferredBytes_ += blk.blockData.size();
            blocksSent++;
            continue;
        }

        if (outgoingFiles_.empty()) break;

        auto& fileTask = outgoingFiles_.front();

        if (!fileTask.hashesRequested && fileTask.totalSize > 0) {
            SyncHashReqPayload req;
            req.syncId = activeSyncId_;
            req.fileId = fileTask.fileId;
            req.relativePath = fileTask.relativePath;
            fileTask.hashesRequested = true;
            std::vector<uint8_t> payload;
            serializeSyncHashReq(req, payload);
            sendPacket(PacketType::SYNC_HASH_REQ, payload);
            break;
        }

        if (!fileTask.queuedBlocks && fileTask.hashesReceived) {
            queueBlocksForTask(fileTask);
            fileTask.queuedBlocks = true;
            currentFileName_ = fileTask.relativePath;
            currentFileIndex_++;
        }

        if (pendingBlocks_.empty() && fileTask.queuedBlocks) {
            outgoingFiles_.pop_front();
        }
    }

    // 3. Periodic telemetry update
    uint64_t now = GetTickCount64();
    if (now - lastTelemetryTickMs_ >= 250 || (outgoingFiles_.empty() && pendingBlocks_.empty() && pendingActions_.empty())) {
        lastTelemetryTickMs_ = now;
        double elapsed = (now - syncStartTickMs_) / 1000.0;
        if (elapsed > 0.05) {
            lastSpeedBps_ = static_cast<double>(transferredBytes_) / elapsed;
            smoothedBps_ = (smoothedBps_ <= 0.0) ? lastSpeedBps_ : (smoothedBps_ * 0.7 + lastSpeedBps_ * 0.3);
        }

        SyncStatusUpdatePayload status;
        status.syncId = activeSyncId_;
        status.transferredBytes = transferredBytes_;
        status.totalBytes = totalBytes_;
        status.currentFileIndex = currentFileIndex_;
        status.totalFiles = totalFiles_;
        status.currentFileName = currentFileName_;

        if (outgoingFiles_.empty() && pendingBlocks_.empty() && pendingActions_.empty()) {
            state_ = SyncState::Completed;
            status.syncState = static_cast<uint8_t>(SyncState::Completed);
        } else {
            status.syncState = static_cast<uint8_t>(SyncState::Syncing);
        }

        std::vector<uint8_t> payload;
        serializeSyncStatusUpdate(status, payload);
        sendPacket(PacketType::SYNC_STATUS_UPDATE, payload);
    }

    return (blocksSent > 0);
}

bool FileSyncManager::handleHashResp(const SyncHashRespPayload& resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& task : outgoingFiles_) {
        if (task.fileId == resp.fileId) {
            task.targetHashes = resp.blockHashes;
            task.hashesReceived = true;
            if (!task.queuedBlocks) {
                queueBlocksForTask(task);
                task.queuedBlocks = true;
                currentFileName_ = task.relativePath;
                currentFileIndex_++;
            }
            return true;
        }
    }
    return false;
}

void FileSyncManager::registerIncomingFile(
    uint32_t fileId,
    const std::string& relativePath,
    uint64_t totalSize,
    uint64_t mtimeSec)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& in = incomingFiles_[fileId];
    in.fileId = fileId;
    in.relativePath = normalizePath(relativePath);
    in.totalSize = totalSize;
    in.mtimeSec = mtimeSec;
    in.totalBlocks = (totalSize > 0) ? static_cast<uint32_t>((totalSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
    in.receivedBlocks = 0;
    in.blockReceived.assign(in.totalBlocks, false);
    in.initialized = false;
    pathToFileIdMap_[in.relativePath] = fileId;
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
    OutgoingFileTask t;
    t.fileId = (fileId != 0) ? fileId : nextFileId_++;
    t.relativePath = normalizePath(relativePath);
    t.fullSourcePath = (std::filesystem::path(sourceRoot) / t.relativePath).string();
    t.totalSize = totalSize;
    t.mtimeSec = mtimeSec;
    t.totalBlocks = (totalSize > 0) ? static_cast<uint32_t>((totalSize + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
    t.targetHashes = targetHashes;
    t.hashesRequested = true;
    t.hashesReceived = true;
    pathToFileIdMap_[t.relativePath] = t.fileId;
    queueBlocksForTask(t);
    t.queuedBlocks = true;
    outgoingFiles_.push_back(std::move(t));
}

bool FileSyncManager::handleDeltaBlock(const SyncDeltaBlockPayload& block) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. Verify CNG SHA-256 integrity of the payload
    auto calculated = CryptoUtils::sha256(block.blockData.data(), block.blockData.size());
    if (calculated != block.blockHash) {
        errorMessage_ = "Block SHA-256 digest validation failed for fileId " + std::to_string(block.fileId);
        return false;
    }

    auto& in = incomingFiles_[block.fileId];

    if (!in.initialized) {
        in.syncId = block.syncId;
        in.fileId = block.fileId;
        in.totalSize = block.totalFileSize;
        in.totalBlocks = block.totalBlocks;
        if (in.blockReceived.size() < in.totalBlocks) {
            in.blockReceived.assign(in.totalBlocks, false);
        }

        if (in.relativePath.empty()) {
            for (const auto& kv : pathToFileIdMap_) {
                if (kv.second == block.fileId) {
                    in.relativePath = kv.first;
                    break;
                }
            }
        }

        if (in.relativePath.empty()) {
            for (const auto& item : diffItems_) {
                if (item.localSize == block.totalFileSize || item.remoteSize == block.totalFileSize) {
                    in.relativePath = item.relativePath;
                    in.mtimeSec = (mode_ == SyncMode::PullMirror) ? item.remoteMtime : item.localMtime;
                    break;
                }
            }
        }

        if (in.relativePath.empty()) {
            for (const auto& kv : incomingFiles_) {
                if (!kv.second.relativePath.empty() && kv.second.totalSize == block.totalFileSize) {
                    in.relativePath = kv.second.relativePath;
                    in.mtimeSec = kv.second.mtimeSec;
                    break;
                }
            }
        }

        in.relativePath = normalizePath(in.relativePath);
        if (in.relativePath.empty()) {
            in.relativePath = "sync_file_" + std::to_string(block.fileId) + ".dat";
        }

        std::string root = resolveTargetRoot();
        std::filesystem::path rootP = std::filesystem::path(root).lexically_normal();
        std::filesystem::path targetP = (rootP / in.relativePath).lexically_normal();

        // Security check: ensure target path is strictly contained within root
        std::string normalTarget = targetP.generic_string();
        std::string normalRoot = rootP.generic_string();
        if (normalTarget.rfind(normalRoot, 0) != 0) {
            errorMessage_ = "Path traversal rejected: " + in.relativePath;
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(targetP.parent_path(), ec);

        in.targetFullPath = targetP.string();
        in.partFullPath = targetP.string() + ".part";

        if (std::filesystem::exists(targetP, ec) && !std::filesystem::is_directory(targetP, ec)) {
            std::filesystem::copy_file(targetP, in.partFullPath, std::filesystem::copy_options::overwrite_existing, ec);
        } else {
            std::ofstream ofs(in.partFullPath, std::ios::binary | std::ios::trunc);
            ofs.close();
        }

        in.initialized = true;
    }

    // 2. Seek and write delta block payload into staging .part file
    {
#if defined(_WIN32)
        std::wstring partW = utf8ToWide(in.partFullPath);
        FILE* fp = _wfopen(partW.c_str(), L"r+b");
        if (!fp) {
            fp = _wfopen(partW.c_str(), L"w+b");
        }
#else
        FILE* fp = fopen(in.partFullPath.c_str(), "r+b");
        if (!fp) {
            fp = fopen(in.partFullPath.c_str(), "w+b");
        }
#endif
        if (!fp) {
            errorMessage_ = "Failed to open staging file: " + in.partFullPath;
            return false;
        }

#if defined(_WIN32)
        if (_fseeki64(fp, static_cast<long long>(block.blockOffset), SEEK_SET) != 0) {
#else
        if (fseeko(fp, static_cast<off_t>(block.blockOffset), SEEK_SET) != 0) {
#endif
            fclose(fp);
            errorMessage_ = "Failed to seek staging file: " + in.partFullPath;
            return false;
        }

        if (!block.blockData.empty()) {
            size_t written = fwrite(block.blockData.data(), 1, block.blockData.size(), fp);
            if (written != block.blockData.size()) {
                fclose(fp);
                errorMessage_ = "Failed to write staging file block: " + in.partFullPath;
                return false;
            }
        }
        fflush(fp);
        fclose(fp);
    }

    if (block.blockIndex >= in.blockReceived.size()) {
        in.blockReceived.resize(block.blockIndex + 1, false);
    }
    if (!in.blockReceived[block.blockIndex]) {
        in.blockReceived[block.blockIndex] = true;
        in.receivedBlocks++;
    }

    // 3. Check for completion and perform atomic replacement
    bool isComplete = (in.receivedBlocks >= in.totalBlocks) ||
                      (block.blockOffset + block.blockData.size() >= in.totalSize);

    if (isComplete) {
        std::wstring partW = utf8ToWide(in.partFullPath);
        std::wstring targetW = utf8ToWide(in.targetFullPath);

        if (!MoveFileExW(partW.c_str(), targetW.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::error_code ec;
            std::filesystem::rename(in.partFullPath, in.targetFullPath, ec);
        }

        if (in.mtimeSec > 0) {
            HANDLE hFile = CreateFileW(
                targetW.c_str(),
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
        incomingFiles_.erase(block.fileId);
    }

    return true;
}

bool FileSyncManager::handleStatusUpdate(const SyncStatusUpdatePayload& status) {
    std::lock_guard<std::mutex> lock(mutex_);
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

    std::string norm = normalizePath(req.targetPath);
    if (norm.empty() && req.actionType != static_cast<uint8_t>(SyncActionType::AbortSync)) {
        resp.statusCode = 1;
        resp.message = "Invalid path";
        return false;
    }

    std::string root = resolveTargetRoot();
    std::filesystem::path full = std::filesystem::path(root) / norm;
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
        case SyncActionType::AbortSync:
            cancelSync();
            break;
        case SyncActionType::RequestFile: {
            if (std::filesystem::exists(full, ec) && !std::filesystem::is_directory(full, ec)) {
                uint64_t sz = std::filesystem::file_size(full, ec);
                auto ftime = std::filesystem::last_write_time(full, ec);
                auto sTime = std::chrono::file_clock::to_sys(ftime);
                uint64_t mt = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(sTime.time_since_epoch()).count()
                );
                OutgoingFileTask t;
                t.fileId = nextFileId_++;
                t.relativePath = norm;
                t.fullSourcePath = full.string();
                t.totalSize = sz;
                t.mtimeSec = mt;
                t.totalBlocks = (sz > 0) ? static_cast<uint32_t>((sz + SYNC_BLOCK_SIZE - 1) / SYNC_BLOCK_SIZE) : 1;
                t.hashesRequested = true;
                t.hashesReceived = true;
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
    std::string scanRoot = req.rootPath.empty() ? localRoot() : req.rootPath;
    if (scanRoot.empty()) {
        resp.statusCode = 1; // DirectoryNotFound
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(scanRoot, ec) || !std::filesystem::is_directory(scanRoot, ec)) {
        resp.statusCode = 1; // DirectoryNotFound
        return false;
    }

    setLocalRoot(scanRoot);
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
    std::string norm = normalizePath(req.relativePath);
    std::string root = localRoot().empty() ? remoteRoot() : localRoot();
    std::filesystem::path full = std::filesystem::path(root) / norm;
    std::error_code ec;

    // Register incoming file mapping for reverse or subsequent writes
    registerIncomingFile(req.fileId, norm, 0, 0);

    if (!std::filesystem::exists(full, ec) || std::filesystem::is_directory(full, ec)) {
        resp.targetFileSize = 0;
        resp.blockHashes.clear();
        return true;
    }

    uint64_t sz = std::filesystem::file_size(full, ec);
    resp.targetFileSize = sz;
    computeBlockHashes(full.string(), sz, resp.blockHashes);
    return true;
}

} // namespace cppdesk
