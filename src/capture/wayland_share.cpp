#include "wayland_share.hpp"

#include "wayland_pipewire.hpp"

#include "../control/input_injector.hpp"

namespace cppdesk::wayland {

namespace {

std::mutex g_registryMutex;
std::weak_ptr<WaylandShare> g_active;
std::function<std::shared_ptr<WaylandShare>(const std::string&)> g_factory;

// The real thing: video from PipeWire.
class PipeWireFrameSource : public WaylandShare::FrameSource {
public:
    PipeWireFrameSource() : slot_(std::make_shared<LatestFrame>()), stream_(slot_) {}
    bool available() const override { return PipeWireStream::available(); }
    bool start(int fd, uint32_t nodeId) override { return stream_.start(fd, nodeId); }
    void stop() override { stream_.stop(); }
    std::shared_ptr<LatestFrame> sink() override { return slot_; }
    bool broken() const override { return stream_.state() == PwState::Failed; }

private:
    std::shared_ptr<LatestFrame> slot_;
    PipeWireStream stream_;
};

}   // namespace

WaylandShare::WaylandShare(PortalOptions opts, std::unique_ptr<FrameSource> source)
    : opts_(std::move(opts)), source_(std::move(source)) {}

WaylandShare::~WaylandShare() { close(); }

void WaylandShare::begin() {
    std::lock_guard<std::mutex> lk(mu_);
    if (begun_ || closed_) return;
    begun_ = true;
    if (!source_->available()) {   // known before anyone is asked: no question is shown for a picture that cannot arrive
        override_ = ShareState::Unavailable;
        overrideReason_ = "the video part of this desktop's screen sharing is not installed";
        return;
    }
    portal_.startAsync(opts_);
}

ShareState WaylandShare::state() const {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (override_ != ShareState::Idle) return override_;
    }
    return portal_.state();
}

ShareNotice WaylandShare::notice() const {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (override_ != ShareState::Idle) return {override_, overrideReason_};
    }
    return portal_.notice();
}

void WaylandShare::fail(ShareState state, const std::string& reason) {
    override_ = state;
    overrideReason_ = reason;
    portal_.close();   // the session is useless now; this also takes any control away
}

bool WaylandShare::resolveStream() {
    if (haveStream_) return true;
    PortalResult r;
    if (!portal_.result(r) || r.streams.empty()) return false;
    stream_ = r.streams.front();
    haveStream_ = true;
    return true;
}

bool WaylandShare::latestFrame(std::vector<uint8_t>& bgra, int& width, int& height) {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_ || override_ != ShareState::Idle) return false;
    const ShareState s = portal_.state();
    if (s != ShareState::Running && s != ShareState::ViewOnly) return false;
    if (!sourceStarted_) {
        PortalResult r;
        if (!portal_.result(r) || !resolveStream()) return false;
        sourceStarted_ = true;
        tokenChange_ = true;
        newToken_ = r.newRestoreToken;
        const int fd = portal_.pipeWireFd();
        sourceStartedAt_ = std::chrono::steady_clock::now();
        // The person has answered by now, so this is a failure, not "unavailable": falling back to X11 would put the
        // peer's input on the desktop whatever they allowed.
        if (!source_->start(fd, stream_.nodeId)) {
            fail(ShareState::Failed, "The desktop allowed sharing, but its picture could not be received.");
            return false;
        }
    }
    if (source_->broken()) {
        fail(ShareState::Failed, "The picture of your screen stopped arriving.");
        return false;
    }
    bool isNew = false;
    if (!source_->sink()->copy(bgra, width, height, isNew)) {
        const auto waited = std::chrono::steady_clock::now() - sourceStartedAt_;
        if (!gotFrame_ && waited > std::chrono::milliseconds(opts_.firstFrameTimeoutMs)) {
            fail(ShareState::Failed, "The picture of your screen never arrived.");
        }
        return false;
    }
    gotFrame_ = true;
    frameW_ = width;
    frameH_ = height;
    return true;
}

bool WaylandShare::takeTokenChange(std::string& token) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!tokenChange_) return false;
    tokenChange_ = false;
    token = newToken_;
    return true;
}

bool WaylandShare::controlLive() {
    return !closed_ && override_ == ShareState::Idle && portal_.state() == ShareState::Running;
}

bool WaylandShare::pointerMove(float normX, float normY) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!controlLive() || !resolveStream()) return false;
    const StreamPoint p = pointForNormalized(normX, normY, stream_, frameW_, frameH_);
    return portal_.pointerMotionAbsolute(p.nodeId, p.x, p.y);
}

bool WaylandShare::pointerButton(MouseButtonId button, bool down, float normX, float normY) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!controlLive() || !resolveStream()) return false;
    const StreamPoint p = pointForNormalized(normX, normY, stream_, frameW_, frameH_);
    portal_.pointerMotionAbsolute(p.nodeId, p.x, p.y);
    const uint32_t code = evdevButton(button);
    if (!portal_.pointerButton(code, down)) return false;
    if (down) buttonsDown_.insert(code); else buttonsDown_.erase(code);
    return true;
}

bool WaylandShare::wheel(int32_t vertical, int32_t horizontal) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!controlLive()) return false;
    bool sent = false;
    // The portal counts steps down and to the right as positive; the wire's wheel delta is positive upwards.
    if (vertical != 0) sent = portal_.pointerAxisDiscrete(0, -wheelSteps(vertical)) || sent;
    if (horizontal != 0) sent = portal_.pointerAxisDiscrete(1, wheelSteps(horizontal)) || sent;
    return sent;
}

bool WaylandShare::key(uint16_t vk, bool down) {
    const uint32_t sym = InputInjector::mapVkToX11KeySym(vk);
    if (sym == 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!controlLive()) return false;
    if (!portal_.keyboardKeysym(sym, down)) return false;
    if (down) keysDown_.insert(sym); else keysDown_.erase(sym);
    return true;
}

void WaylandShare::releaseAll() {
    std::lock_guard<std::mutex> lk(mu_);
    if (controlLive()) {
        for (uint32_t sym : keysDown_) portal_.keyboardKeysym(sym, false);
        for (uint32_t code : buttonsDown_) portal_.pointerButton(code, false);
    }
    keysDown_.clear();
    buttonsDown_.clear();
}

void WaylandShare::close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        closed_ = true;
    }
    portal_.close();
    if (source_) source_->stop();
}

std::shared_ptr<WaylandShare> WaylandShare::active() {
    std::lock_guard<std::mutex> lk(g_registryMutex);
    return g_active.lock();
}

void WaylandShare::setActive(const std::shared_ptr<WaylandShare>& share) {
    std::lock_guard<std::mutex> lk(g_registryMutex);
    g_active = share;
}

std::shared_ptr<WaylandShare> WaylandShare::make(const std::string& restoreToken) {
    std::function<std::shared_ptr<WaylandShare>(const std::string&)> factory;
    {
        std::lock_guard<std::mutex> lk(g_registryMutex);
        factory = g_factory;
    }
    if (factory) return factory(restoreToken);
    PortalOptions opts;
    opts.restoreToken = restoreToken;
    return std::make_shared<WaylandShare>(opts, std::make_unique<PipeWireFrameSource>());
}

void WaylandShare::setFactoryForTest(std::function<std::shared_ptr<WaylandShare>(const std::string&)> factory) {
    std::lock_guard<std::mutex> lk(g_registryMutex);
    g_factory = std::move(factory);
}

} // namespace cppdesk::wayland
