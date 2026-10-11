#pragma once

#include <string>
#include <mutex>
#include <cstdint>
#include "../core/protocol.hpp"

namespace cppdesk {

class ScratchpadManager {
public:
    ScratchpadManager() = default;

    // Local text edit: increments monotonic revision counter and updates state
    ScratchpadUpdatePayload updateLocalText(const std::string& newText, uint32_t localDeskId, uint32_t cursorPos = 0);

    // Remote text edit: only accepted if payload.revision >= current revision
    bool handleRemoteUpdate(const ScratchpadUpdatePayload& payload);

    // Accessors
    std::string getText() const;
    uint64_t getRevision() const;
    uint32_t getLastEditor() const;
    uint32_t getCursorPos() const;

    // Reset buffer
    void clear();

    // Quick diagnostic snippet generator for common remote desktop admin workflows
    static std::string generateSnippet(const std::string& snippetType, uint32_t hostDeskId = 0, const std::string& hostname = "");

private:
    mutable std::mutex mutex_;
    std::string        text_;
    uint64_t           revision_ = 0;
    uint32_t           lastEditorDeskId_ = 0;
    uint32_t           cursorPos_ = 0;
};

} // namespace cppdesk
