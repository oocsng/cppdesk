#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include "ui_window.hpp"
#include "ui_platform.hpp"
#include "../net/network_engine.hpp"
#include "../core/crypto_identity.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>

@interface CppDeskMacWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) BOOL isClosed;
@end

@implementation CppDeskMacWindowDelegate
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;
    self.isClosed = YES;
}
@end

@interface CppDeskMacCanvasView : NSView {
    cppdesk::NetworkEngine* network_;
    cppdesk::IdentityManager* identity_;
    uint64_t lastModifiers_;
    uint64_t lastFrameSeq_;
    std::vector<uint8_t> frameBgra_;
    int frameW_;
    int frameH_;
    cppdesk::CursorState cursorState_;
    cppdesk::ScaleMode scaleMode_;
    NSTrackingArea* trackingArea_;
}
- (instancetype)initWithFrame:(NSRect)frameRect network:(cppdesk::NetworkEngine*)network identity:(cppdesk::IdentityManager*)identity;
- (void)setScaleMode:(cppdesk::ScaleMode)mode;
@end

@implementation CppDeskMacCanvasView

- (instancetype)initWithFrame:(NSRect)frameRect network:(cppdesk::NetworkEngine*)network identity:(cppdesk::IdentityManager*)identity {
    self = [super initWithFrame:frameRect];
    if (self) {
        network_ = network;
        identity_ = identity;
        lastModifiers_ = 0;
        lastFrameSeq_ = 0;
        frameW_ = 0;
        frameH_ = 0;
        scaleMode_ = cppdesk::ScaleMode::FitAspect;
        trackingArea_ = nil;
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)becomeFirstResponder {
    return YES;
}

- (BOOL)canBecomeKeyView {
    return YES;
}

- (void)setScaleMode:(cppdesk::ScaleMode)mode {
    scaleMode_ = mode;
    [self setNeedsDisplay:YES];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (trackingArea_ != nil) {
        [self removeTrackingArea:trackingArea_];
    }
    NSTrackingAreaOptions options = (NSTrackingMouseMoved |
                                     NSTrackingActiveAlways |
                                     NSTrackingInVisibleRect |
                                     NSTrackingCursorUpdate);
    trackingArea_ = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                 options:options
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:trackingArea_];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    if (!ctx) return;

    // Clear stage with dark background
    CGContextSetRGBFillColor(ctx, 0.07, 0.07, 0.08, 1.0);
    CGContextFillRect(ctx, NSRectToCGRect(self.bounds));

    if (!network_) return;

    // Retrieve decoded 32-bit BGRA buffer
    bool hasNewFrame = network_->copyLatestViewerFrame(lastFrameSeq_, frameBgra_, frameW_, frameH_, cursorState_);
    (void)hasNewFrame;

    if (frameW_ > 0 && frameH_ > 0 && !frameBgra_.empty()) {
        NSRect b = self.bounds;
        auto geo = cppdesk::UiPlatform::calculateViewportGeometry(
            static_cast<float>(b.size.width),
            static_cast<float>(b.size.height),
            static_cast<uint32_t>(frameW_),
            static_cast<uint32_t>(frameH_),
            scaleMode_);

        CGRect dstRect = CGRectMake(geo.contentRect.left, geo.contentRect.top, geo.contentRect.width(), geo.contentRect.height());

        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr,
            frameBgra_.data(),
            frameBgra_.size(),
            nullptr);

        if (provider) {
            CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
            if (colorSpace) {
                CGImageRef image = CGImageCreate(
                    frameW_,
                    frameH_,
                    8,
                    32,
                    frameW_ * 4,
                    colorSpace,
                    kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                    provider,
                    nullptr,
                    false,
                    kCGRenderingIntentDefault);

                if (image) {
                    CGContextDrawImage(ctx, dstRect, image);
                    CGImageRelease(image);
                }
                CGColorSpaceRelease(colorSpace);
            }
            CGDataProviderRelease(provider);
        }

        // Render remote hardware cursor overlay if active
        if (cursorState_.visible && frameW_ > 0 && frameH_ > 0) {
            float normCurX = static_cast<float>(cursorState_.x) / static_cast<float>(frameW_);
            float normCurY = static_cast<float>(cursorState_.y) / static_cast<float>(frameH_);
            float viewCurX = 0.0f, viewCurY = 0.0f;
            cppdesk::UiPlatform::mapNormalizedToPoint(normCurX, normCurY, geo, viewCurX, viewCurY);

            CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 0.85);
            CGContextFillEllipseInRect(ctx, CGRectMake(viewCurX - 4.0, viewCurY - 4.0, 8.0, 8.0));
            CGContextSetRGBStrokeColor(ctx, 0.0, 0.0, 0.0, 0.85);
            CGContextSetLineWidth(ctx, 1.5);
            CGContextStrokeEllipseInRect(ctx, CGRectMake(viewCurX - 4.0, viewCurY - 4.0, 8.0, 8.0));
        }
    }
}

- (BOOL)normalizeEventLocation:(NSEvent*)event normX:(float&)outNormX normY:(float&)outNormY {
    NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
    NSRect b = self.bounds;
    auto geo = cppdesk::UiPlatform::calculateViewportGeometry(
        static_cast<float>(b.size.width),
        static_cast<float>(b.size.height),
        static_cast<uint32_t>(frameW_ > 0 ? frameW_ : 1920),
        static_cast<uint32_t>(frameH_ > 0 ? frameH_ : 1080),
        scaleMode_);
    return cppdesk::UiPlatform::mapPointToNormalized(static_cast<float>(loc.x), static_cast<float>(loc.y), geo, outNormX, outNormY);
}

- (void)mouseMoved:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseMove(nx, ny);
    }
}

- (void)mouseDragged:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseMove(nx, ny);
    }
}

- (void)rightMouseDragged:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseMove(nx, ny);
    }
}

- (void)otherMouseDragged:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseMove(nx, ny);
    }
}

- (void)mouseDown:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseButton(cppdesk::MouseButtonId::Left, true, nx, ny);
    }
}

- (void)mouseUp:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseButton(cppdesk::MouseButtonId::Left, false, nx, ny);
    }
}

- (void)rightMouseDown:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseButton(cppdesk::MouseButtonId::Right, true, nx, ny);
    }
}

- (void)rightMouseUp:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseButton(cppdesk::MouseButtonId::Right, false, nx, ny);
    }
}

- (void)otherMouseDown:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseButton(cppdesk::MouseButtonId::Middle, true, nx, ny);
    }
}

- (void)otherMouseUp:(NSEvent*)event {
    if (!network_) return;
    float nx = 0.0f, ny = 0.0f;
    if ([self normalizeEventLocation:event normX:nx normY:ny]) {
        network_->sendMouseButton(cppdesk::MouseButtonId::Middle, false, nx, ny);
    }
}

- (void)scrollWheel:(NSEvent*)event {
    if (!network_) return;
    float factor = [event hasPreciseScrollingDeltas] ? 6.0f : 120.0f;
    int16_t vDelta = static_cast<int16_t>([event scrollingDeltaY] * factor);
    int16_t hDelta = static_cast<int16_t>([event scrollingDeltaX] * factor);
    if (vDelta != 0 || hDelta != 0) {
        network_->sendMouseWheel(vDelta, hDelta);
    }
}

- (void)keyDown:(NSEvent*)event {
    if (!network_) return;
    uint16_t macKey = [event keyCode];
    uint32_t vk = cppdesk::UiPlatform::macKeyCodeToWindowsVk(macKey);
    if (vk != 0) {
        network_->sendKeyEvent(static_cast<uint16_t>(vk), 0, true, false);
    }
}

- (void)keyUp:(NSEvent*)event {
    if (!network_) return;
    uint16_t macKey = [event keyCode];
    uint32_t vk = cppdesk::UiPlatform::macKeyCodeToWindowsVk(macKey);
    if (vk != 0) {
        network_->sendKeyEvent(static_cast<uint16_t>(vk), 0, false, false);
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (!network_) return;
    uint64_t current = static_cast<uint64_t>([event modifierFlags]);
    auto transitions = cppdesk::UiPlatform::decodeModifierTransitions(lastModifiers_, current);
    lastModifiers_ = current;
    for (const auto& t : transitions) {
        network_->sendKeyEvent(static_cast<uint16_t>(t.vkCode), 0, t.isDown, false);
    }
}

@end

namespace cppdesk {

struct CppDeskWindowMacImpl {
    NSWindow* window = nil;
    CppDeskMacCanvasView* canvasView = nil;
    CppDeskMacWindowDelegate* delegate = nil;
};

CppDeskWindow::CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget)
    : identity_(identity), network_(network), autoConnectTarget_(autoConnectTarget), pImpl_(std::make_unique<CppDeskWindowMacImpl>()) {
}

CppDeskWindow::~CppDeskWindow() {
    close();
}

bool CppDeskWindow::create(void* /*displayContext*/, int /*showCommand*/) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSRect frame = NSMakeRect(100, 100, 1280, 800);
    NSWindowStyleMask styleMask = NSWindowStyleMaskTitled |
                                  NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable |
                                  NSWindowStyleMaskResizable |
                                  NSWindowStyleMaskFullSizeContentView;

    pImpl_->window = [[NSWindow alloc] initWithContentRect:frame
                                                 styleMask:styleMask
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];

    if (!pImpl_->window) {
        return false;
    }

    [pImpl_->window setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
    [pImpl_->window setTitle:@"CppDesk - Remote Desktop"];
    [pImpl_->window setBackgroundColor:[NSColor colorWithCalibratedRed:0.07f green:0.07f blue:0.08f alpha:1.0f]];
    [pImpl_->window setTitlebarAppearsTransparent:YES];
    [pImpl_->window setTitleVisibility:NSWindowTitleHidden];

    pImpl_->delegate = [[CppDeskMacWindowDelegate alloc] init];
    [pImpl_->window setDelegate:pImpl_->delegate];

    pImpl_->canvasView = [[CppDeskMacCanvasView alloc] initWithFrame:frame
                                                             network:&network_
                                                            identity:&identity_];
    [pImpl_->window setContentView:pImpl_->canvasView];
    [pImpl_->window makeFirstResponder:pImpl_->canvasView];
    [pImpl_->window makeKeyAndOrderFront:nil];

    if (!autoConnectTarget_.empty()) {
        network_.connectToRemote(autoConnectTarget_, "");
    }

    return true;
}

int CppDeskWindow::messageLoop() {
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];

    while (pImpl_ && pImpl_->delegate && !pImpl_->delegate.isClosed) {
        @autoreleasepool {
            NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:0.016];
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:deadline
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            while (event) {
                [NSApp sendEvent:event];
                event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:nil
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES];
            }

            if (pImpl_ && pImpl_->canvasView) {
                [pImpl_->canvasView setNeedsDisplay:YES];
            }

            if (pImpl_ && pImpl_->window && ![pImpl_->window isVisible]) {
                break;
            }
        }
    }
    return 0;
}

void CppDeskWindow::close() {
    if (pImpl_ && pImpl_->window) {
        [pImpl_->window close];
        pImpl_->window = nil;
        pImpl_->canvasView = nil;
        pImpl_->delegate = nil;
    }
}

} // namespace cppdesk

#endif // defined(__APPLE__)
