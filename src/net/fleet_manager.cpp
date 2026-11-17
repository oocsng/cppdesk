#include "fleet_manager.hpp"

#include <algorithm>

namespace cppdesk {

struct FleetManager::Member {
    FleetMember info;
    std::unique_ptr<NetworkEngine> engine;   // null for a UI-snapshot placeholder
    ViewerConnectionState placeholderState = ViewerConnectionState::Disconnected;
};

FleetManager::FleetManager(IdentityManager& identity) : identity_(identity) {}

FleetManager::~FleetManager() { closeAll(); }

std::vector<FleetMember> FleetManager::eligibleMembers(const IdentityManager& id, const std::vector<DiscoveredPeer>& peers) {
    std::vector<FleetMember> out;
    for (const auto& p : peers)
        if (!p.viaRelay && id.findPairedHost(p.deskId)) out.push_back({p.deskId, p.hostname, p.ip + ":" + std::to_string(p.port)});
    std::stable_sort(out.begin(), out.end(), [](const FleetMember& a, const FleetMember& b) { return a.name < b.name; });
    return out;
}

size_t FleetManager::open(const std::vector<FleetMember>& members) {
    size_t opened = 0;
    for (const auto& fm : members) {
        auto has = [&] {
            return std::any_of(members_.begin(), members_.end(), [&](const auto& x) { return x->info.deskId == fm.deskId; });
        };
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (members_.size() >= kMaxMembers) break;
            if (has()) continue;   // one engine per PC: a host takes a single session
        }
        // Built and dialled outside the lock; connectToRemote only starts the session thread.
        auto m = std::make_shared<Member>();
        m->info = fm;
        m->engine = std::make_unique<NetworkEngine>(identity_);
        m->engine->startViewerOnly();
        m->engine->connectToRemote(fm.address, "", fm.deskId);   // discovery's address is spoofable: only that paired PC may answer
        std::lock_guard<std::mutex> lk(mutex_);
        if (members_.size() >= kMaxMembers || has()) continue;   // lost a race with another open(); m stops after lk unlocks
        members_.push_back(std::move(m));
        ++opened;
    }
    return opened;
}

void FleetManager::close(uint64_t deskId) {
    std::shared_ptr<Member> m;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(members_.begin(), members_.end(), [&](const auto& x) { return x->info.deskId == deskId; });
        if (it == members_.end()) return;
        m = *it;
        members_.erase(it);
    }
    if (!m->engine) return;
    if (auto* e = connected(*m)) e->sendReleaseAllModifiers();   // before the disconnect, so no key stays held
    m->engine->stop();   // frees the host now, even while another thread's snapshot still holds the engine
}

void FleetManager::closeAll() {
    for (const auto& m : snapshot()) close(m->info.deskId);
}

size_t FleetManager::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return members_.size();
}

std::vector<FleetTileStatus> FleetManager::status() const {
    std::vector<FleetTileStatus> out;
    for (const auto& m : snapshot()) {
        FleetTileStatus t;
        t.deskId = m->info.deskId;
        t.name = m->info.name;
        if (!m->engine) {
            t.state = m->placeholderState;
            out.push_back(std::move(t));
            continue;
        }
        const auto st = m->engine->viewerStats();
        t.state = st.state;
        t.statusMessage = st.statusMessage;
        t.qualityPreset = st.qualityPreset;
        t.targetFps = st.targetFps;
        // The UI polls this every frame. Checking the settings in effect (not a once-flag) also repairs the engine's own
        // default request, which can land just after Connected; requestVideoSettings records them, so steady state sends nothing.
        if (st.state == ViewerConnectionState::Connected && (st.qualityPreset != QualityPreset::LowBandwidth || st.targetFps != 15))
            m->engine->requestVideoSettings(QualityPreset::LowBandwidth, 0, true, 15);
        for (const auto& item : m->engine->fileTransferManager().snapshotTransfers()) {
            if (item.isOutgoing) { t.lastOutgoing = item; break; }   // newest first
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool FleetManager::copyFrame(uint64_t deskId, uint64_t& seq, std::vector<uint8_t>& bgra, int& w, int& h, CursorState& cursor) {
    auto m = find(deskId);
    return m && m->engine && m->engine->copyLatestViewerFrame(seq, bgra, w, h, cursor);
}

void FleetManager::sendKeyToAll(uint16_t vk, uint16_t scan, bool isDown, bool isExtended) {
    for (const auto& m : snapshot())
        if (auto* e = connected(*m)) e->sendKeyEvent(vk, scan, isDown, isExtended);
}

void FleetManager::sendKeyTo(uint64_t deskId, uint16_t vk, uint16_t scan, bool isDown, bool isExtended) {
    auto m = find(deskId);
    if (auto* e = m ? connected(*m) : nullptr) e->sendKeyEvent(vk, scan, isDown, isExtended);
}

void FleetManager::releaseModifiers(uint64_t deskId) {
    for (const auto& m : snapshot())
        if (deskId == 0 || m->info.deskId == deskId)
            if (auto* e = connected(*m)) e->sendReleaseAllModifiers();
}

void FleetManager::sendMouseMoveTo(uint64_t deskId, float nx, float ny) {
    auto m = find(deskId);
    if (auto* e = m ? connected(*m) : nullptr) e->sendMouseMove(nx, ny);
}

void FleetManager::sendMouseButtonTo(uint64_t deskId, MouseButtonId b, bool isDown, float nx, float ny) {
    auto m = find(deskId);
    if (auto* e = m ? connected(*m) : nullptr) e->sendMouseButton(b, isDown, nx, ny);
}

void FleetManager::pushClipboardToAll() {
    for (const auto& m : snapshot())
        if (auto* e = connected(*m)) e->pushLocalClipboardNow();
}

size_t FleetManager::sendFileToAll(const std::string& path) {
    size_t queued = 0;
    for (const auto& m : snapshot())
        if (auto* e = connected(*m); e && e->sendFile(path) != 0) ++queued;
    return queued;
}

bool FleetManager::sendFileTo(uint64_t deskId, const std::string& path) {
    auto m = find(deskId);
    auto* e = m ? connected(*m) : nullptr;
    return e && e->sendFile(path) != 0;
}

void FleetManager::addPlaceholderForSnapshot(uint64_t deskId, const std::string& name, ViewerConnectionState state) {
    auto m = std::make_shared<Member>();
    m->info = {deskId, name, ""};
    m->placeholderState = state;
    std::lock_guard<std::mutex> lk(mutex_);
    members_.push_back(std::move(m));
}

std::vector<std::shared_ptr<FleetManager::Member>> FleetManager::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return members_;
}

std::shared_ptr<FleetManager::Member> FleetManager::find(uint64_t deskId) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& m : members_)
        if (m->info.deskId == deskId) return m;
    return nullptr;
}

NetworkEngine* FleetManager::connected(const Member& m) {
    if (!m.engine) return nullptr;
    const auto st = m.engine->viewerStats();
    return st.state == ViewerConnectionState::Connected && st.pairedSession && st.remoteDeskId == m.info.deskId ? m.engine.get() : nullptr;
}

} // namespace cppdesk
