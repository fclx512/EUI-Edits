#include "core/platform/native_bridge.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>

#include <string.h>

void eui_set_application_icon_rgba(int width, int height, const unsigned char* pixels) {
    if (pixels == NULL || width <= 0 || height <= 0) {
        return;
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSBitmapImageRep* representation = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                         pixelsWide:width
                         pixelsHigh:height
                      bitsPerSample:8
                    samplesPerPixel:4
                           hasAlpha:YES
                           isPlanar:NO
                     colorSpaceName:NSDeviceRGBColorSpace
                        bytesPerRow:width * 4
                       bitsPerPixel:32];
        if (representation == nil) {
            return;
        }

        memcpy([representation bitmapData], pixels, (size_t)width * (size_t)height * 4u);
        NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
        [image addRepresentation:representation];
        [NSApp setApplicationIconImage:image];
        [image release];
        [representation release];
    }
}

void eui_set_native_child_window(void* parentWindow, void* childWindow, int enabled) {
    if (parentWindow == NULL || childWindow == NULL) {
        return;
    }

    @autoreleasepool {
        NSWindow* parent = (NSWindow*)parentWindow;
        NSWindow* child = (NSWindow*)childWindow;
        if (enabled) {
            if ([child parentWindow] != parent) {
                [[child parentWindow] removeChildWindow:child];
                [parent addChildWindow:child ordered:NSWindowAbove];
            }
            [child orderWindow:NSWindowAbove relativeTo:[parent windowNumber]];
            [child makeKeyAndOrderFront:nil];
        } else if ([child parentWindow] == parent) {
            [parent removeChildWindow:child];
        }
    }
}

#else

void eui_set_application_icon_rgba(int width, int height, const unsigned char* pixels) {
    (void)width;
    (void)height;
    (void)pixels;
}

void eui_set_native_child_window(void* parentWindow, void* childWindow, int enabled) {
    (void)parentWindow;
    (void)childWindow;
    (void)enabled;
}

#endif
