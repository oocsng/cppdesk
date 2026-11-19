#if defined(__APPLE__)

#include "mac_screen_capture.hpp"
#include "mac_frame.hpp"
#include "../media/audio_convert.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

namespace cppdesk {

namespace {

constexpr NSInteger kUserDeclined = -3801;   // SCStreamErrorUserDeclined: no Screen Recording permission
constexpr int64_t kStepTimeoutSeconds = 5;

// What the stream's callbacks (on their own queue) and the capture thread share.
struct Shared {
    std::atomic<int> state{static_cast<int>(MacCaptureState::Stopped)};
    std::mutex mutex;
    std::vector<uint8_t> frame;   // newest complete frame, tightly packed BGRA
    int width = 0;
    int height = 0;
    uint64_t seq = 0;
    uint64_t copiedSeq = 0;
    bool accepting = false;   // callbacks only touch the frame while this is true (guarded by `mutex`)
    MacCaptureStream::AudioSink audioSink;   // guarded by `mutex`
    audio::ConvertState convert;             // the audio queue only

    void setState(MacCaptureState s) { state.store(static_cast<int>(s)); }
};

MacCaptureState stateForError(NSError* error) {
    return error && error.code == kUserDeclined ? MacCaptureState::NoPermission : MacCaptureState::Failed;
}

} // namespace

} // namespace cppdesk

// Keeps the shared state alive for as long as macOS may still call it (it can deliver a frame or an error after
// the stream was stopped and the owner is gone).
@interface CppDeskStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<cppdesk::Shared> shared_;
}
- (void)deliverAudio:(CMSampleBufferRef)sampleBuffer;
@end

@implementation CppDeskStreamOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) {
        if (@available(macOS 13.0, *)) {
            if (type == SCStreamOutputTypeAudio) [self deliverAudio:sampleBuffer];
        }
        return;
    }
    if (!CMSampleBufferIsValid(sampleBuffer)) return;
    // Only complete frames: macOS also sends "idle" (nothing changed) and "blank" notifications without pixels.
    NSArray* attachments = (__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    NSDictionary* info = attachments.firstObject;
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (status && status.integerValue != SCFrameStatusComplete) return;
    CVImageBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixels) return;
    CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
    const int w = static_cast<int>(CVPixelBufferGetWidth(pixels));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pixels));
    const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
    if (base && w > 0 && h > 0) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!shared_->accepting) {
            CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
            return;
        }
        shared_->frame.resize(static_cast<size_t>(w) * h * 4);
        cppdesk::copyBgraRows(base, stride, w, h, shared_->frame.data());
        shared_->width = w;
        shared_->height = h;
        ++shared_->seq;
    }
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
}

// The Mac's sound: float or 16-bit, planar or interleaved, any rate: converted to what the wire carries.
- (void)deliverAudio:(CMSampleBufferRef)sampleBuffer {
    if (!CMSampleBufferIsValid(sampleBuffer) || !CMSampleBufferDataIsReady(sampleBuffer)) return;
    cppdesk::MacCaptureStream::AudioSink sink;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        if (!shared_->accepting) return;
        sink = shared_->audioSink;
    }
    if (!sink) return;
    CMAudioFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
    const AudioStreamBasicDescription* asbd = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : nullptr;
    const CMItemCount frameCount = CMSampleBufferGetNumSamples(sampleBuffer);
    if (!asbd || frameCount <= 0 || asbd->mChannelsPerFrame == 0) return;
    const bool isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool planar = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if (!((isFloat && asbd->mBitsPerChannel == 32) || (!isFloat && asbd->mBitsPerChannel == 16))) return;

    size_t listSize = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, &listSize, nullptr, 0, nullptr, nullptr, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, nullptr);
    if (listSize == 0) return;
    std::vector<uint8_t> storage(listSize);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    CMBlockBufferRef block = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, nullptr, list, listSize, nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) != noErr) return;

    // One block of memory in the layout PcmBlock says: planes one after another, or interleaved.
    const size_t channels = asbd->mChannelsPerFrame;
    const size_t frames = static_cast<size_t>(frameCount);
    const size_t bytesPerSample = isFloat ? sizeof(float) : sizeof(int16_t);
    std::vector<uint8_t> flat(frames * channels * bytesPerSample, 0);
    if (planar) {
        for (UInt32 c = 0; c < list->mNumberBuffers && c < channels; ++c) {
            if (!list->mBuffers[c].mData) continue;
            std::memcpy(flat.data() + c * frames * bytesPerSample, list->mBuffers[c].mData,
                        std::min<size_t>(list->mBuffers[c].mDataByteSize, frames * bytesPerSample));
        }
    } else if (list->mNumberBuffers >= 1 && list->mBuffers[0].mData) {
        std::memcpy(flat.data(), list->mBuffers[0].mData, std::min<size_t>(list->mBuffers[0].mDataByteSize, flat.size()));
    }
    if (block) CFRelease(block);

    std::vector<int16_t> wire;
    cppdesk::audio::toWire({ flat.data(), frames, static_cast<int>(channels), static_cast<int>(asbd->mSampleRate),
                             isFloat ? cppdesk::audio::SampleFormat::F32 : cppdesk::audio::SampleFormat::S16, planar },
                           shared_->convert, wire);
    if (!wire.empty()) sink(wire.data(), wire.size() / 2);
}

// macOS ended the stream: permission revoked, display unplugged or changed, sleep.
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->accepting) shared_->setState(cppdesk::stateForError(error));
}

@end

namespace cppdesk {

struct MacCaptureStream::Impl {
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    SCStream* stream = nil;
    CppDeskStreamOutput* output = nil;
    dispatch_queue_t queue = nil;
    dispatch_queue_t audioQueue = nil;
    AudioSink sink;          // kept across stop() and start(): the capturer restarts the stream by itself
    bool audioOn = false;    // this run also delivers sound
};

MacCaptureStream::MacCaptureStream() : impl_(std::make_unique<Impl>()) {}

MacCaptureStream::~MacCaptureStream() { stop(); }

bool MacCaptureStream::start(uint32_t displayId, int fps, int maxLongEdge) {
    stop();
    Shared& shared = *impl_->shared;
    @autoreleasepool {
        shared.setState(MacCaptureState::Starting);

        // 1. The displays macOS lets us capture. Without permission this fails (or lists nothing).
        __block SCShareableContent* content = nil;
        __block NSError* contentError = nil;
        dispatch_semaphore_t listed = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* c, NSError* e) {
            content = c;
            contentError = e;
            dispatch_semaphore_signal(listed);
        }];
        if (dispatch_semaphore_wait(listed, dispatch_time(DISPATCH_TIME_NOW, kStepTimeoutSeconds * NSEC_PER_SEC)) != 0) {
            shared.setState(MacCaptureState::Failed);
            return false;
        }
        SCDisplay* display = nil;
        for (SCDisplay* d in content.displays) {
            if (d.displayID == displayId) display = d;
        }
        if (!display) {
            shared.setState(contentError ? stateForError(contentError)
                                         : (CGPreflightScreenCaptureAccess() ? MacCaptureState::Failed : MacCaptureState::NoPermission));
            return false;
        }

        // 2. The stream: BGRA at the display's pixel size (capped), no cursor (the cursor travels separately).
        size_t pixelW = static_cast<size_t>(display.width), pixelH = static_cast<size_t>(display.height);
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId)) {
            pixelW = CGDisplayModeGetPixelWidth(mode);
            pixelH = CGDisplayModeGetPixelHeight(mode);
            CGDisplayModeRelease(mode);
        }
        int outW = 0, outH = 0;
        fitCaptureSize(static_cast<int>(pixelW), static_cast<int>(pixelH), maxLongEdge, outW, outH);
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        config.width = static_cast<size_t>(outW);
        config.height = static_cast<size_t>(outH);
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.showsCursor = NO;
        config.queueDepth = 3;
        config.minimumFrameInterval = CMTimeMake(1, std::max(1, fps));
        bool wantAudio = false;
        if (@available(macOS 13.0, *)) {
            if (impl_->sink) {   // the Mac's own sound, without CppDesk's, at the wire's rate
                config.capturesAudio = YES;
                config.excludesCurrentProcessAudio = YES;
                config.sampleRate = 48000;
                config.channelCount = 2;
                wantAudio = true;
            }
        }
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];

        impl_->output = [[CppDeskStreamOutput alloc] init];
        impl_->output->shared_ = impl_->shared;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.accepting = true;
        }
        impl_->queue = dispatch_queue_create("cppdesk.capture", DISPATCH_QUEUE_SERIAL);
        impl_->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:impl_->output];
        auto failStart = [&](MacCaptureState st) -> bool {
            stop();
            shared.setState(st);
            return false;
        };

        NSError* addError = nil;
        if (![impl_->stream addStreamOutput:impl_->output type:SCStreamOutputTypeScreen sampleHandlerQueue:impl_->queue error:&addError]) {
            impl_->stream = nil;
            return failStart(stateForError(addError));
        }
        if (wantAudio) {   // sound that cannot be added leaves the screen stream as it was
            if (@available(macOS 13.0, *)) {
                impl_->audioQueue = dispatch_queue_create("cppdesk.capture.audio", DISPATCH_QUEUE_SERIAL);
                NSError* audioError = nil;
                if ([impl_->stream addStreamOutput:impl_->output type:SCStreamOutputTypeAudio sampleHandlerQueue:impl_->audioQueue error:&audioError]) {
                    std::lock_guard<std::mutex> lock(shared.mutex);
                    shared.audioSink = impl_->sink;
                    shared.convert = audio::ConvertState{};
                    impl_->audioOn = true;
                } else {
                    impl_->audioQueue = nil;
                }
            }
        }

        // 3. Start it.
        __block NSError* startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [impl_->stream startCaptureWithCompletionHandler:^(NSError* e) {
            startError = e;
            dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, kStepTimeoutSeconds * NSEC_PER_SEC)) != 0) {
            return failStart(MacCaptureState::Failed);
        }
        if (startError) {
            return failStart(stateForError(startError));
        }
        MacCaptureState afterStart = MacCaptureState::Running;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            afterStart = static_cast<MacCaptureState>(shared.state.load());
            if (afterStart == MacCaptureState::Starting) {
                shared.setState(MacCaptureState::Running);
                return true;
            }
        }
        return failStart(afterStart);
    }
}

void MacCaptureStream::stop() {
    Shared& shared = *impl_->shared;
    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.accepting = false;
        shared.audioSink = nullptr;
    }
    @autoreleasepool {
        if (impl_->stream) {
            SCStream* stream = impl_->stream;
            dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
            [stream stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(stopped); }];
            dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            [stream removeStreamOutput:impl_->output type:SCStreamOutputTypeScreen error:nil];
            if (impl_->audioOn) {
                if (@available(macOS 13.0, *)) [stream removeStreamOutput:impl_->output type:SCStreamOutputTypeAudio error:nil];
            }
            impl_->stream = nil;
        }
        // Let a callback that is already running finish, then refuse any later one.
        if (impl_->queue) dispatch_sync(impl_->queue, ^{});
        if (impl_->audioQueue) dispatch_sync(impl_->audioQueue, ^{});
        impl_->output = nil;
        impl_->queue = nil;
        impl_->audioQueue = nil;
        impl_->audioOn = false;
    }
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.frame.clear();
    shared.width = shared.height = 0;
    shared.seq = shared.copiedSeq = 0;
    shared.setState(MacCaptureState::Stopped);
}

void MacCaptureStream::setAudioSink(AudioSink sink) { impl_->sink = std::move(sink); }

bool MacCaptureStream::audioAvailable() const { return impl_->audioOn; }

MacCaptureState MacCaptureStream::state() const {
    return static_cast<MacCaptureState>(impl_->shared->state.load());
}

bool MacCaptureStream::copyLatest(std::vector<uint8_t>& bgra, int& width, int& height, bool& isNew) {
    Shared& shared = *impl_->shared;
    std::lock_guard<std::mutex> lock(shared.mutex);
    if (shared.seq == 0 || shared.frame.empty()) return false;
    bgra = shared.frame;
    width = shared.width;
    height = shared.height;
    isNew = shared.seq != shared.copiedSeq;
    shared.copiedSeq = shared.seq;
    return true;
}

} // namespace cppdesk

#endif
