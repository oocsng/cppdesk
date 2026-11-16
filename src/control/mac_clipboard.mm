#if defined(__APPLE__)

#include "mac_clipboard.hpp"

#import <AppKit/AppKit.h>

#include <atomic>
#include <mutex>

// Supplies one promised file's URL when an app pastes.
@interface CppDeskFilePromise : NSObject <NSPasteboardItemDataProvider> {
@public
    std::function<std::optional<std::vector<std::string>>()> fetch;
    size_t index;
}
@end

@implementation CppDeskFilePromise
- (void)pasteboard:(NSPasteboard*)pasteboard item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type {
    if (!fetch) return;
    const auto paths = fetch();   // every item asks; the files are fetched once
    if (!paths || index >= paths->size()) return;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:(*paths)[index].c_str()]];
    [item setString:url.absoluteString forType:type];
}
- (void)pasteboardFinishedWithDataProvider:(NSPasteboard*)pasteboard {
}
@end

namespace cppdesk {

namespace {
std::atomic<long> g_offeredChangeCount{-1};   // the pasteboard generation that holds our promised files
std::mutex g_offerMutex;                       // offers come from network threads
NSArray* g_providers = nil;                    // kept here: a pasteboard item may not keep its provider alive
}

bool macClipboardGetText(std::string& utf8) {
    @autoreleasepool {
        NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char* bytes = text ? [text UTF8String] : nullptr;   // NULL for text UTF-8 cannot hold (a lone surrogate)
        if (!bytes) return false;
        utf8 = bytes;
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

bool macClipboardGetFiles(std::vector<std::string>& paths) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (pb.changeCount == g_offeredChangeCount.load()) return false;   // reading them would fetch them
        NSArray* urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                          options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
        for (NSURL* url in urls) {
            const char* path = url.fileSystemRepresentation;
            if (path) paths.push_back(path);
        }
        return !paths.empty();
    }
}

bool macClipboardOfferFiles(size_t count, std::function<std::optional<std::vector<std::string>>()> fetch) {
    std::lock_guard<std::mutex> lock(g_offerMutex);
    @autoreleasepool {
        NSMutableArray* items = [NSMutableArray array];
        NSMutableArray* providers = [NSMutableArray array];
        for (size_t i = 0; i < count; ++i) {
            CppDeskFilePromise* provider = [CppDeskFilePromise new];
            provider->fetch = fetch;
            provider->index = i;
            [providers addObject:provider];
            NSPasteboardItem* item = [NSPasteboardItem new];
            if (![item setDataProvider:provider forTypes:@[ NSPasteboardTypeFileURL ]]) return false;
            [items addObject:item];
        }
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        if (![pb writeObjects:items]) return false;
        g_providers = providers;
        g_offeredChangeCount = pb.changeCount;
        return true;
    }
}

void macClipboardClearFiles() {
    std::lock_guard<std::mutex> lock(g_offerMutex);
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (pb.changeCount == g_offeredChangeCount.load()) [pb clearContents];
        g_offeredChangeCount = -1;
        g_providers = nil;
    }
}

} // namespace cppdesk

#endif
