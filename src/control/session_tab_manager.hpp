#pragma once

#include "../core/protocol.hpp"
#include "../capture/screen_capture.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace cppdesk {

struct SessionTab {
    uint32_t                id = 0;
    uint64_t                deskId = 0;
    std::string             targetInput;
    std::string             title;
    std::string             remoteHostname;
    std::string             password;
    uint64_t                requirePairedDeskId = 0;   // non-zero: dial targetInput (ip:port) with pairing keys only
    ViewerConnectionState   state = ViewerConnectionState::Disconnected;
    std::string             statusMessage;

    // Per-session settings & states
    ScaleMode               scaleMode = ScaleMode::FitAspect;
    bool                    remoteInputEnabled = true;
    uint32_t                unreadChat = 0;
    uint64_t                connectedSinceTickMs = 0;
    float                   fps = 0.0f;
    uint32_t                rttMs = 0;
    int                     activeMonitorIndex = 0;
    int                     monitorCount = 1;
    QualityPreset           qualityPreset = QualityPreset::Ultra;
    bool                    privacyModeEngaged = false;

    // Per-session frame buffer cache
    std::vector<uint8_t>    cachedFrameBgra;
    int                     cachedW = 0;
    int                     cachedH = 0;
    uint64_t                lastFrameSeq = 0;
    CursorState             cursor{};
};

class SessionTabManager {
public:
    SessionTabManager();
    ~SessionTabManager();

    // Tab lifecycle
    uint32_t createTab(uint64_t deskId, const std::string& targetInput, const std::string& title = "");
    bool closeTab(uint32_t tabId);
    void closeAllTabs();

    // Tab selection & navigation
    bool selectTab(uint32_t tabId);
    uint32_t nextTab();
    uint32_t prevTab();

    // Accessors
    uint32_t activeTabId() const { return activeTabId_; }
    SessionTab* activeTab();
    const SessionTab* activeTab() const;
    SessionTab* getTab(uint32_t tabId);
    const SessionTab* getTab(uint32_t tabId) const;
    const std::vector<SessionTab>& tabs() const { return tabs_; }
    size_t tabCount() const { return tabs_.size(); }

    // Lookups
    SessionTab* findTabByDeskId(uint64_t deskId);
    SessionTab* findTabByTarget(const std::string& target);

    // Framebuffer caching
    void cacheActiveTabFrame(const uint8_t* bgra, int width, int height, uint64_t frameSeq, const CursorState& cursor);
    void cacheTabFrame(uint32_t tabId, const uint8_t* bgra, int width, int height, uint64_t frameSeq, const CursorState& cursor);

private:
    uint32_t                nextTabId_ = 1;
    uint32_t                activeTabId_ = 0;
    std::vector<SessionTab> tabs_;
};

} // namespace cppdesk
