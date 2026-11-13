#include "wayland_pipewire.hpp"

#include "wayland_helpers.hpp"

#include <atomic>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unistd.h>

#if defined(CPPDESK_WAYLAND)
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>
#endif

namespace cppdesk::wayland {

#if defined(CPPDESK_WAYLAND)

namespace {

// The few libpipewire functions used, looked up by name when the stream starts (nothing is linked).
struct Api {
    decltype(&pw_init) init = nullptr;
    decltype(&pw_properties_new) propertiesNew = nullptr;
    decltype(&pw_thread_loop_new) loopNew = nullptr;
    decltype(&pw_thread_loop_destroy) loopDestroy = nullptr;
    decltype(&pw_thread_loop_start) loopStart = nullptr;
    decltype(&pw_thread_loop_stop) loopStop = nullptr;
    decltype(&pw_thread_loop_lock) loopLock = nullptr;
    decltype(&pw_thread_loop_unlock) loopUnlock = nullptr;
    decltype(&pw_thread_loop_get_loop) loopGet = nullptr;
    decltype(&pw_context_new) contextNew = nullptr;
    decltype(&pw_context_destroy) contextDestroy = nullptr;
    decltype(&pw_context_connect_fd) connectFd = nullptr;
    decltype(&pw_core_disconnect) coreDisconnect = nullptr;
    decltype(&pw_stream_new) streamNew = nullptr;
    decltype(&pw_stream_destroy) streamDestroy = nullptr;
    decltype(&pw_stream_add_listener) addListener = nullptr;
    decltype(&pw_stream_connect) streamConnect = nullptr;
    decltype(&pw_stream_disconnect) streamDisconnect = nullptr;
    decltype(&pw_stream_update_params) updateParams = nullptr;
    decltype(&pw_stream_dequeue_buffer) dequeue = nullptr;
    decltype(&pw_stream_queue_buffer) queue = nullptr;
};

template <class F>
bool lookup(void* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(dlsym(lib, name));
    return out != nullptr;
}

// Looks the functions up in the library (re-read when the requested file changes, which only tests do).
// The library stays loaded until the process ends.
bool loadApi(Api& out) {
    static std::mutex mu;
    static std::string loadedFrom;
    static Api api;
    static bool ok = false, tried = false;
    std::lock_guard<std::mutex> lk(mu);
    const char* env = std::getenv("CPPDESK_PIPEWIRE_LIB");
    const std::string want = env && *env ? env : "libpipewire-0.3.so.0";
    if (!tried || want != loadedFrom) {
        tried = true;
        loadedFrom = want;
        ok = false;
        api = Api();
        if (void* lib = dlopen(want.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            ok = lookup(lib, "pw_init", api.init) && lookup(lib, "pw_properties_new", api.propertiesNew) &&
                 lookup(lib, "pw_thread_loop_new", api.loopNew) && lookup(lib, "pw_thread_loop_destroy", api.loopDestroy) &&
                 lookup(lib, "pw_thread_loop_start", api.loopStart) && lookup(lib, "pw_thread_loop_stop", api.loopStop) &&
                 lookup(lib, "pw_thread_loop_lock", api.loopLock) && lookup(lib, "pw_thread_loop_unlock", api.loopUnlock) &&
                 lookup(lib, "pw_thread_loop_get_loop", api.loopGet) && lookup(lib, "pw_context_new", api.contextNew) &&
                 lookup(lib, "pw_context_destroy", api.contextDestroy) && lookup(lib, "pw_context_connect_fd", api.connectFd) &&
                 lookup(lib, "pw_core_disconnect", api.coreDisconnect) && lookup(lib, "pw_stream_new", api.streamNew) &&
                 lookup(lib, "pw_stream_destroy", api.streamDestroy) && lookup(lib, "pw_stream_add_listener", api.addListener) &&
                 lookup(lib, "pw_stream_connect", api.streamConnect) && lookup(lib, "pw_stream_disconnect", api.streamDisconnect) &&
                 lookup(lib, "pw_stream_update_params", api.updateParams) && lookup(lib, "pw_stream_dequeue_buffer", api.dequeue) &&
                 lookup(lib, "pw_stream_queue_buffer", api.queue);
            if (ok) api.init(nullptr, nullptr);
        }
    }
    if (ok) out = api;
    return ok;
}

PixelFormat fromSpa(uint32_t f) {
    switch (f) {
        case SPA_VIDEO_FORMAT_BGRx: return PixelFormat::BGRx;
        case SPA_VIDEO_FORMAT_BGRA: return PixelFormat::BGRA;
        case SPA_VIDEO_FORMAT_RGBx: return PixelFormat::RGBx;
        case SPA_VIDEO_FORMAT_RGBA: return PixelFormat::RGBA;
        default: return PixelFormat::Unsupported;
    }
}

}   // namespace

struct PipeWireStream::Impl {
    std::shared_ptr<LatestFrame> sink;
    std::atomic<PwState> state{PwState::Stopped};
    Api api;
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    // what the stream agreed on; read and written on the PipeWire thread only
    PixelFormat format = PixelFormat::Unsupported;
    int width = 0, height = 0;

    static void onState(void* data, pw_stream_state, pw_stream_state state, const char*) {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR) self->state = PwState::Failed;
        else if (state == PW_STREAM_STATE_STREAMING) self->state = PwState::Streaming;
        else if (state == PW_STREAM_STATE_UNCONNECTED && self->state.load() == PwState::Streaming) self->state = PwState::Failed;
    }

    static void onParam(void* data, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<Impl*>(data);
        if (!param || id != SPA_PARAM_Format) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        self->format = fromSpa(info.format);
        self->width = static_cast<int>(info.size.width);
        self->height = static_cast<int>(info.size.height);
        // Ask for plain memory buffers only (no GPU buffers), which is all convertToBgra can read.
        uint8_t buffer[256];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[1];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType, SPA_POD_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        self->api.updateParams(self->stream, params, 1);
    }

    static void onProcess(void* data) {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* pb = self->api.dequeue(self->stream);
        if (!pb) return;
        const spa_buffer* sb = pb->buffer;
        if (sb && sb->n_datas > 0 && self->width > 0 && self->height > 0) {
            const spa_data& d = sb->datas[0];
            const bool broken = d.chunk->size == 0;   // an empty buffer: nothing new to show
            const size_t stride = d.chunk->stride > 0 ? static_cast<size_t>(d.chunk->stride) : static_cast<size_t>(self->width) * 4;
            const size_t offset = d.chunk->offset;
            const size_t need = stride * (self->height - 1) + static_cast<size_t>(self->width) * 4;
            if (d.data && !broken && offset + need <= d.maxsize) {
                const uint8_t* src = static_cast<const uint8_t*>(d.data) + offset;
                const PixelFormat fmt = self->format;
                const int w = self->width, h = self->height;
                self->sink->write(w, h, [&](uint8_t* dst) { convertToBgra(fmt, src, stride, w, h, dst); });
            }
        }
        self->api.queue(self->stream, pb);
    }

    void startStream(int fd, uint32_t nodeId) {
        loop = api.loopNew("cppdesk-screen", nullptr);
        if (!loop) return;
        api.loopStart(loop);
        api.loopLock(loop);
        context = api.contextNew(api.loopGet(loop), nullptr, 0);
        if (context) core = api.connectFd(context, fd, nullptr, 0);   // the fd now belongs to PipeWire
        if (core) {
            stream = api.streamNew(core, "CppDesk screen",
                                    api.propertiesNew("media.type", "Video", "media.category", "Capture", "media.role", "Screen", nullptr));
        }
        if (stream) {
            static const pw_stream_events events = [] {
                pw_stream_events e{};
                e.version = PW_VERSION_STREAM_EVENTS;
                e.state_changed = &Impl::onState;
                e.param_changed = &Impl::onParam;
                e.process = &Impl::onProcess;
                return e;
            }();
            api.addListener(stream, &listener, &events, this);
            uint8_t buffer[1024];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            spa_rectangle defSize{1280, 720}, minSize{1, 1}, maxSize{16384, 16384};
            spa_fraction defRate{30, 1}, minRate{0, 1}, maxRate{120, 1};
            const spa_pod* params[1];
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,
                SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                       SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate)));
            const int rc = api.streamConnect(stream, PW_DIRECTION_INPUT, nodeId,
                                              static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                              params, 1);
            if (rc < 0) { api.streamDestroy(stream); stream = nullptr; }
        }
        api.loopUnlock(loop);
    }

    void teardown() {
        if (!loop) return;
        sink->accept(false);
        api.loopLock(loop);
        if (stream) { api.streamDestroy(stream); stream = nullptr; }
        if (core) { api.coreDisconnect(core); core = nullptr; }
        if (context) { api.contextDestroy(context); context = nullptr; }
        api.loopUnlock(loop);
        api.loopStop(loop);
        api.loopDestroy(loop);
        loop = nullptr;
    }
};

PipeWireStream::PipeWireStream(std::shared_ptr<LatestFrame> sink) : impl_(new Impl) { impl_->sink = std::move(sink); }

PipeWireStream::~PipeWireStream() { stop(); }

bool PipeWireStream::available() {
    Api api;
    return loadApi(api);
}

bool PipeWireStream::start(int fd, uint32_t nodeId) {
    stop();
    if (!loadApi(impl_->api)) {
        if (fd >= 0) ::close(fd);
        impl_->state = PwState::Unavailable;
        return false;
    }
    impl_->state = PwState::Starting;
    impl_->width = impl_->height = 0;
    impl_->format = PixelFormat::Unsupported;
    impl_->sink->accept(true);
    impl_->startStream(fd, nodeId);
    if (!impl_->stream) {
        // The fd is ours to close only if it never reached PipeWire (a failed connect may or may not have closed it;
        // leaking one descriptor beats closing a stranger's).
        if (!impl_->context && fd >= 0) ::close(fd);
        impl_->teardown();
        impl_->state = PwState::Failed;
        return false;
    }
    return true;
}

void PipeWireStream::stop() {
    impl_->teardown();
    if (impl_->state != PwState::Unavailable) impl_->state = PwState::Stopped;
}

PwState PipeWireStream::state() const { return impl_->state; }

#else   // built without the PipeWire headers: the stream is simply unavailable

struct PipeWireStream::Impl {
    std::shared_ptr<LatestFrame> sink;
    std::atomic<PwState> state{PwState::Stopped};
};

PipeWireStream::PipeWireStream(std::shared_ptr<LatestFrame> sink) : impl_(new Impl) { impl_->sink = std::move(sink); }

PipeWireStream::~PipeWireStream() = default;

bool PipeWireStream::available() { return false; }

bool PipeWireStream::start(int fd, uint32_t) {
    if (fd >= 0) ::close(fd);
    impl_->state = PwState::Unavailable;
    return false;
}

void PipeWireStream::stop() {}

PwState PipeWireStream::state() const { return impl_->state; }

#endif

} // namespace cppdesk::wayland
