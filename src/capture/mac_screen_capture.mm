#if defined(__APPLE__)

#include "mac_screen_capture.hpp"
#include "mac_frame.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

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
@end

@implementation CppDeskStreamOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer)) return;
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
        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];

        impl_->output = [[CppDeskStreamOutput alloc] init];
        impl_->output->shared_ = impl_->shared;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.accepting = true;
        }
        impl_->queue = dispatch_queue_create("cppdesk.capture", DISPATCH_QUEUE_SERIAL);
        impl_->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:impl_->output];
        NSError* addError = nil;
        if (![impl_->stream addStreamOutput:impl_->output type:SCStreamOutputTypeScreen sampleHandlerQueue:impl_->queue error:&addError]) {
            shared.setState(stateForError(addError));
            impl_->stream = nil;
            impl_->output = nil;
            return false;
        }

        // 3. Start it.
        __block NSError* startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [impl_->stream startCaptureWithCompletionHandler:^(NSError* e) {
            startError = e;
            dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, kStepTimeoutSeconds * NSEC_PER_SEC)) != 0) {
            shared.setState(MacCaptureState::Failed);
            return false;
        }
        if (startError) {
            shared.setState(stateForError(startError));
            return false;
        }
        shared.setState(MacCaptureState::Running);
        return true;
    }
}

void MacCaptureStream::stop() {
    @autoreleasepool {
        if (impl_->stream) {
            SCStream* stream = impl_->stream;
            dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
            [stream stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(stopped); }];
            dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            [stream removeStreamOutput:impl_->output type:SCStreamOutputTypeScreen error:nil];
            impl_->stream = nil;
        }
        // Let a callback that is already running finish, then refuse any later one.
        if (impl_->queue) dispatch_sync(impl_->queue, ^{});
        impl_->output = nil;
        impl_->queue = nil;
    }
    Shared& shared = *impl_->shared;
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.accepting = false;
    shared.frame.clear();
    shared.width = shared.height = 0;
    shared.seq = shared.copiedSeq = 0;
    shared.setState(MacCaptureState::Stopped);
}

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
