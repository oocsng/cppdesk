#pragma once

#include "network_engine.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cppdesk {

struct FleetMember { uint64_t deskId = 0; std::string name; std::string address; };   // address "ip:port"

struct FleetTileStatus {
    uint64_t deskId = 0;
    std::string name;
    ViewerConnectionState state = ViewerConnectionState::Disconnected;
    std::string statusMessage;
    std::optional<FileTransferItem> lastOutgoing;   // newest outgoing transfer, for done/failed/progress
};

// Drives one viewer-only NetworkEngine per paired PC, all on one shared IdentityManager (which must outlive this).
// Thread-safe: the member list is only touched under mutex_; engine calls happen on a snapshot, outside the lock,
// and only on members that are Connected, so a dropped PC never holds up the others.
class FleetManager {
public:
    static constexpr size_t kMaxMembers = 16;
    explicit FleetManager(IdentityManager& identity);
    ~FleetManager();
    FleetManager(const FleetManager&) = delete;
    FleetManager& operator=(const FleetManager&) = delete;

    // Paired hosts that discovery currently sees directly (not viaRelay), sorted by name.
    static std::vector<FleetMember> eligibleMembers(const IdentityManager& id, const std::vector<DiscoveredPeer>& peers);
    size_t open(const std::vector<FleetMember>& members);   // opens at most kMaxMembers in all; returns how many
    void close(uint64_t deskId);                            // release-all, disconnect, drop the engine
    void closeAll();
    size_t size() const;
    // In open() order. Also asks each newly Connected member for LowBandwidth at 15 FPS, once per connection.
    std::vector<FleetTileStatus> status() const;
    bool copyFrame(uint64_t deskId, uint64_t& seq, std::vector<uint8_t>& bgra, int& w, int& h, CursorState& cursor);
    void sendKeyToAll(uint16_t vk, uint16_t scan, bool isDown, bool isExtended);
    void sendKeyTo(uint64_t deskId, uint16_t vk, uint16_t scan, bool isDown, bool isExtended);
    void releaseModifiers(uint64_t deskId);                 // deskId 0 = all
    void sendMouseMoveTo(uint64_t deskId, float nx, float ny);
    void sendMouseButtonTo(uint64_t deskId, MouseButtonId b, bool isDown, float nx, float ny);
    void pushClipboardToAll();
    size_t sendFileToAll(const std::string& path);          // returns how many PCs it was queued to
    bool sendFileTo(uint64_t deskId, const std::string& path);
    void addPlaceholderForSnapshot(uint64_t deskId, const std::string& name, ViewerConnectionState state); // UI snapshots only

private:
    struct Member;
    std::vector<std::shared_ptr<Member>> snapshot() const;
    std::shared_ptr<Member> find(uint64_t deskId) const;
    static NetworkEngine* connected(const Member& m);       // the engine if it is Connected, else null

    IdentityManager& identity_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Member>> members_;
};

} // namespace cppdesk
