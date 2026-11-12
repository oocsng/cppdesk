#if defined(__APPLE__)

#include "mac_test_driver.hpp"
#include <cstring>

#import <Cocoa/Cocoa.h>

void macClick(void* nsWindow, float xDip, float yDip) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    const NSPoint inWindow = NSMakePoint(xDip, window.contentView.bounds.size.height - yDip);   // y-up in the window
    for (NSEventType type : { NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp }) {
        NSEvent* ev = [NSEvent mouseEventWithType:type location:inWindow modifierFlags:0 timestamp:0
                                     windowNumber:window.windowNumber context:nil eventNumber:0 clickCount:1
                                         pressure:type == NSEventTypeLeftMouseDown ? 1.0f : 0.0f];
        [window sendEvent:ev];
    }
}

void macType(void* nsWindow, const char* characters, uint16_t macKeyCode) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    NSString* chars = [NSString stringWithUTF8String:characters];
    for (NSEventType type : { NSEventTypeKeyDown, NSEventTypeKeyUp }) {
        NSEvent* ev = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0
                                   windowNumber:window.windowNumber context:nil characters:chars
                    charactersIgnoringModifiers:chars isARepeat:NO keyCode:macKeyCode];
        [window sendEvent:ev];
    }
}

bool macWindowDrewVariedPixels(void* nsWindow) {
    NSWindow* window = (__bridge NSWindow*)nsWindow;
    NSView* view = window.contentView;
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if (!rep) return false;
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    const unsigned char* data = rep.bitmapData;
    const NSInteger w = rep.pixelsWide, h = rep.pixelsHigh, bpp = rep.bitsPerPixel / 8, stride = rep.bytesPerRow;
    if (!data || w < 20 || h < 20 || bpp < 3) return false;
    const unsigned char* corner = data + 5 * stride + 5 * bpp;
    bool varied = false;
    for (NSInteger y = 0; y < h && !varied; y += 7)
        for (NSInteger x = 0; x < w && !varied; x += 7)
            varied = std::memcmp(data + y * stride + x * bpp, corner, 3) != 0;
    return varied && (corner[0] | corner[1] | corner[2]) != 0;   // not the all-black "nothing drawn yet" window
}

#endif
