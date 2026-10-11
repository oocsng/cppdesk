#include "scratchpad_manager.hpp"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace cppdesk {

ScratchpadUpdatePayload ScratchpadManager::updateLocalText(const std::string& newText, uint32_t localDeskId, uint32_t cursorPos) {
    std::lock_guard<std::mutex> lock(mutex_);
    revision_++;
    text_ = newText;
    lastEditorDeskId_ = localDeskId;
    cursorPos_ = cursorPos;

    ScratchpadUpdatePayload payload;
    payload.revision = revision_;
    payload.authorDeskId = lastEditorDeskId_;
    payload.cursorPosition = cursorPos_;
    payload.text = text_;
    return payload;
}

bool ScratchpadManager::handleRemoteUpdate(const ScratchpadUpdatePayload& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Reject older revisions
    if (payload.revision < revision_) {
        return false;
    }
    // Accept equal or newer revision
    revision_ = payload.revision;
    text_ = payload.text;
    lastEditorDeskId_ = static_cast<uint32_t>(payload.authorDeskId);
    cursorPos_ = payload.cursorPosition;
    return true;
}

std::string ScratchpadManager::getText() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return text_;
}

uint64_t ScratchpadManager::getRevision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

uint32_t ScratchpadManager::getLastEditor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastEditorDeskId_;
}

uint32_t ScratchpadManager::getCursorPos() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cursorPos_;
}

void ScratchpadManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    revision_++;
    text_.clear();
    cursorPos_ = 0;
}

std::string ScratchpadManager::generateSnippet(const std::string& snippetType, uint32_t hostDeskId, const std::string& hostname) {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &tt);
#else
    localtime_r(&tt, &tmBuf);
#endif
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmBuf);

    if (snippetType == "host" || snippetType == "host_info") {
        std::ostringstream ss;
        ss << "\n--- Host Diagnostics ---\n";
        ss << "Host: " << (hostname.empty() ? "Remote Desktop" : hostname) << "\n";
        ss << "Desk ID: " << hostDeskId << "\n";
        ss << "Logged: " << timeStr << "\n";
        ss << "Status: Active Remote Session\n";
        ss << "-------------------------\n";
        return ss.str();
    } else if (snippetType == "security" || snippetType == "connection_stats") {
        std::ostringstream ss;
        ss << "\n--- Connection Security ---\n";
        ss << "Security: AES-256-GCM + ECDH P-256 AEAD\n";
        ss << "Auth: 9-Digit Identity Handshake\n";
        ss << "Verified at: " << timeStr << "\n";
        ss << "---------------------------\n";
        return ss.str();
    } else if (snippetType == "timestamp") {
        std::ostringstream ss;
        ss << "[Timestamp: " << timeStr << "] ";
        return ss.str();
    }

    return "[" + std::string(timeStr) + "] Logged\n";
}

} // namespace cppdesk
