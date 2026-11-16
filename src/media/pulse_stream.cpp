#include "pulse_stream.hpp"

#include "audio_convert.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

#if defined(CPPDESK_PULSE)
#include <pulse/simple.h>
#endif

namespace cppdesk {

#if defined(CPPDESK_PULSE)

namespace {

// The few libpulse-simple functions used, looked up by name when a stream opens (nothing is linked).
struct Api {
    decltype(&pa_simple_new) create = nullptr;
    decltype(&pa_simple_free) destroy = nullptr;
    decltype(&pa_simple_read) read = nullptr;
    decltype(&pa_simple_write) write = nullptr;
};

// Re-read when CPPDESK_PULSE_LIB changes (only tests do that); the library stays loaded until the process ends.
bool loadApi(Api& out) {
    static std::mutex mu;
    static std::string loadedFrom;
    static Api api;
    static bool ok = false;
    const char* env = std::getenv("CPPDESK_PULSE_LIB");
    const std::string want = (env && *env) ? env : "libpulse-simple.so.0";
    std::lock_guard<std::mutex> lk(mu);
    if (want != loadedFrom) {
        loadedFrom = want;
        api = Api{};
        ok = false;
        if (void* lib = dlopen(want.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            api.create = reinterpret_cast<decltype(api.create)>(dlsym(lib, "pa_simple_new"));
            api.destroy = reinterpret_cast<decltype(api.destroy)>(dlsym(lib, "pa_simple_free"));
            api.read = reinterpret_cast<decltype(api.read)>(dlsym(lib, "pa_simple_read"));
            api.write = reinterpret_cast<decltype(api.write)>(dlsym(lib, "pa_simple_write"));
            ok = api.create && api.destroy && api.read && api.write;
        }
    }
    if (ok) out = api;
    return ok;
}

} // namespace

// What the stream's own thread shares with the PulseStream; the thread keeps it alive, so a thread that is still
// inside the library when close() gives up on it finishes harmlessly later.
struct PulseStream::State {
    Api api;
    pa_simple* h = nullptr;
    Dir dir = Dir::Record;
    std::atomic<bool> stop{false}, error{false}, done{false};
    std::mutex mu;
    std::condition_variable cv;
    audio::BoundedPcmQueue q;
    size_t chunk = 0;
    explicit State(size_t maxBytes) : q(maxBytes) {}
};

namespace {

void runStream(const std::shared_ptr<PulseStream::State>& st) {
    std::vector<uint8_t> tmp(st->chunk);
    int err = 0;
    if (st->dir == PulseStream::Dir::Record) {
        while (!st->stop) {
            if (st->api.read(st->h, tmp.data(), tmp.size(), &err) < 0) { st->error = true; break; }
            if (st->stop) break;
            st->q.push(tmp.data(), tmp.size());
            { std::lock_guard<std::mutex> lk(st->mu); }
            st->cv.notify_all();
        }
    } else {
        while (!st->stop) {
            const size_t n = st->q.pop(tmp.data(), tmp.size());
            if (n == 0) {
                std::unique_lock<std::mutex> lk(st->mu);
                st->cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return st->stop.load() || st->q.size() > 0; });
                continue;
            }
            if (st->api.write(st->h, tmp.data(), n, &err) < 0) { st->error = true; break; }
        }
    }
    st->api.destroy(st->h);
    st->h = nullptr;
    { std::lock_guard<std::mutex> lk(st->mu); st->done = true; }
    st->cv.notify_all();
}

} // namespace

#else

struct PulseStream::State {};

#endif

PulseStream::PulseStream() = default;

PulseStream::~PulseStream() { close(); }

bool PulseStream::available() {
#if defined(CPPDESK_PULSE)
    Api api;
    return loadApi(api);
#else
    return false;
#endif
}

bool PulseStream::open(Dir dir, const char* device, uint32_t rate, uint8_t channels, const char* appName, int latencyMs) {
    close();
#if defined(CPPDESK_PULSE)
    if (rate == 0 || channels == 0) return false;
    Api api;
    if (!loadApi(api)) return false;

    const uint32_t bytesPerMs = rate * channels * 2 / 1000;
    const uint32_t lat = static_cast<uint32_t>(std::max(1, latencyMs)) * std::max<uint32_t>(1, bytesPerMs);
    pa_sample_spec ss{};
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = rate;
    ss.channels = channels;
    pa_buffer_attr attr;
    std::memset(&attr, 0xff, sizeof(attr));   // every field "server's choice"
    attr.tlength = lat;
    attr.fragsize = lat;

    int err = 0;
    pa_simple* h = api.create(nullptr, appName ? appName : "CppDesk", dir == Dir::Record ? PA_STREAM_RECORD : PA_STREAM_PLAYBACK,
                              device, dir == Dir::Record ? "record" : "playback", &ss, nullptr, &attr, &err);
    if (!h) return false;

    const size_t maxBytes = static_cast<size_t>(bytesPerMs) * (dir == Dir::Record ? 1000 : 250);
    auto st = std::make_shared<State>(std::max<size_t>(maxBytes, 4096));
    st->api = api;
    st->h = h;
    st->dir = dir;
    st->chunk = std::max<size_t>(static_cast<size_t>(bytesPerMs) * 20, 64);
    {
        std::lock_guard<std::mutex> lk(stMu_);
        st_ = st;
    }
    worker_ = std::thread([st] { runStream(st); });
    return true;
#else
    (void)dir; (void)device; (void)rate; (void)channels; (void)appName; (void)latencyMs;
    return false;
#endif
}

int PulseStream::read(uint8_t* buf, size_t bytes) {
#if defined(CPPDESK_PULSE)
    std::shared_ptr<State> st;
    { std::lock_guard<std::mutex> lk(stMu_); st = st_; }
    if (!st || st->dir != Dir::Record || !buf) return -1;
    size_t n = st->q.pop(buf, bytes);
    if (n > 0) return static_cast<int>(n);
    if (st->error) return -1;
    {
        std::unique_lock<std::mutex> lk(st->mu);
        st->cv.wait_for(lk, std::chrono::milliseconds(100),
                        [&] { return st->q.size() > 0 || st->error.load() || st->stop.load(); });
    }
    n = st->q.pop(buf, bytes);
    if (n > 0) return static_cast<int>(n);
    return st->error ? -1 : 0;
#else
    (void)buf; (void)bytes;
    return -1;
#endif
}

bool PulseStream::write(const uint8_t* buf, size_t bytes) {
#if defined(CPPDESK_PULSE)
    std::shared_ptr<State> st;
    { std::lock_guard<std::mutex> lk(stMu_); st = st_; }
    if (!st || st->dir != Dir::Playback || st->error || st->stop || !buf) return false;
    st->q.push(buf, bytes);
    { std::lock_guard<std::mutex> lk(st->mu); }
    st->cv.notify_all();
    return true;
#else
    (void)buf; (void)bytes;
    return false;
#endif
}

void PulseStream::flush() {
#if defined(CPPDESK_PULSE)
    std::shared_ptr<State> st;
    { std::lock_guard<std::mutex> lk(stMu_); st = st_; }
    if (st) st->q.clear();
#endif
}

void PulseStream::close() {
#if defined(CPPDESK_PULSE)
    std::shared_ptr<State> st;
    {
        std::lock_guard<std::mutex> lk(stMu_);
        st = std::move(st_);
        st_.reset();
    }
    if (!st) return;
    st->stop = true;
    st->cv.notify_all();
    if (worker_.joinable()) {
        bool finished;
        {
            std::unique_lock<std::mutex> lk(st->mu);
            finished = st->cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return st->done.load(); });
        }
        if (finished) worker_.join();
        else worker_.detach();   // still inside the library (an idle or dead server); it frees the stream when it returns
    }
#endif
}

bool PulseStream::isOpen() const {
#if defined(CPPDESK_PULSE)
    std::lock_guard<std::mutex> lk(stMu_);
    return st_ && !st_->error && !st_->stop;
#else
    return false;
#endif
}

} // namespace cppdesk
