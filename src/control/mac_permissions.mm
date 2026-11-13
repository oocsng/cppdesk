#if defined(__APPLE__)

#include "mac_permissions.hpp"

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>

namespace cppdesk {

ui::HostPermissionStatus macPermissionsNow() {
    ui::HostPermissionStatus s;
    s.screenRecording = CGPreflightScreenCaptureAccess();
    s.accessibility = AXIsProcessTrusted();
    return s;
}

void macRequestPermission(ui::HostPermission permission) {
    if (permission == ui::HostPermission::ScreenRecording) {
        CGRequestScreenCaptureAccess();
    } else {
        NSDictionary* options = @{ (__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES };
        AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    }
}

void macOpenPrivacyPane(ui::HostPermission permission) {
    @autoreleasepool {
        NSString* pane = permission == ui::HostPermission::ScreenRecording ? @"Privacy_ScreenCapture" : @"Privacy_Accessibility";
        NSString* url = [@"x-apple.systempreferences:com.apple.preference.security?" stringByAppendingString:pane];
        [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:url]];
    }
}

} // namespace cppdesk

#endif
