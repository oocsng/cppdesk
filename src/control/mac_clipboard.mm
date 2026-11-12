#if defined(__APPLE__)

#include "mac_clipboard.hpp"

#import <AppKit/AppKit.h>

namespace cppdesk {

bool macClipboardGetText(std::string& utf8) {
    @autoreleasepool {
        NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        if (!text) return false;
        utf8 = std::string([text UTF8String]);
        return true;
    }
}

bool macClipboardSetText(const std::string& utf8) {
    @autoreleasepool {
        NSString* text = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
        if (!text) return false;   // not valid UTF-8
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        return [pb setString:text forType:NSPasteboardTypeString];
    }
}

} // namespace cppdesk

#endif
