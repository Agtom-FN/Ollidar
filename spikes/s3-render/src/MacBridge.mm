#include "MacBridge.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <QuartzCore/CAMetalLayer.h>

#include <string>

namespace {
std::string gDeviceName;
std::string gLayerClass;
} // namespace

namespace macbridge {

void* viewFromWinId(uint64_t winId) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(winId));
}

void prepareView(void* nsViewPtr) {
    NSView* view = (__bridge NSView*) nsViewPtr;
    if (!view) return;
    [view setWantsLayer:YES];
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
}

void* attachOwnMetalLayer(void* nsViewPtr, double scale, int wpx, int hpx) {
    NSView* view = (__bridge NSView*) nsViewPtr;
    if (!view) return nullptr;
    [view setWantsLayer:YES];
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(wpx, hpx);
    layer.frame = view.bounds;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    layer.needsDisplayOnBoundsChange = YES;
    [view.layer addSublayer:layer];
    return (__bridge void*) layer;
}

static CAMetalLayer* findMetalLayerImpl(NSView* view) {
    if (!view) return nil;
    if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
        return (CAMetalLayer*) view.layer;
    }
    for (CALayer* sub in view.layer.sublayers) {
        if ([sub isKindOfClass:[CAMetalLayer class]]) {
            return (CAMetalLayer*) sub;
        }
    }
    return nil;
}

void* findMetalLayer(void* nsViewPtr) {
    NSView* view = (__bridge NSView*) nsViewPtr;
    return (__bridge void*) findMetalLayerImpl(view);
}

void configureLayer(void* layerPtr, double scale, int wpx, int hpx,
                    double frameW, double frameH, bool vsync) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) layerPtr;
    if (!layer) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES]; // no implicit animation on resize
    layer.contentsScale = scale;
    if (frameW > 0.0 && frameH > 0.0) {
        layer.frame = CGRectMake(0, 0, frameW, frameH);
    }
    layer.drawableSize = CGSizeMake(wpx, hpx);
    if (@available(macOS 10.13, *)) {
        layer.displaySyncEnabled = vsync ? YES : NO;
        layer.allowsNextDrawableTimeout = NO;
    }
    layer.maximumDrawableCount = 3;
    [CATransaction commit];
}

const char* viewLayerClassName(void* nsViewPtr) {
    NSView* view = (__bridge NSView*) nsViewPtr;
    static std::string name;
    name = (view && view.layer) ? std::string(NSStringFromClass([view.layer class]).UTF8String)
                                : "<nil>";
    return name.c_str();
}

const char* metalDeviceName() {
    if (gDeviceName.empty()) {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        gDeviceName = dev ? std::string([[dev name] UTF8String]) : "<none>";
    }
    return gDeviceName.c_str();
}

double displayRefreshHz() {
    NSScreen* s = [NSScreen mainScreen];
    if (@available(macOS 12.0, *)) {
        double hz = [s maximumFramesPerSecond];
        if (hz > 0) return hz;
    }
    return 60.0;
}

uint64_t currentDrawableW(void* layerPtr) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) layerPtr;
    return layer ? (uint64_t) layer.drawableSize.width : 0;
}

uint64_t currentDrawableH(void* layerPtr) {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) layerPtr;
    return layer ? (uint64_t) layer.drawableSize.height : 0;
}

bool layerIsMetal(void* layerPtr) {
    id obj = (__bridge id) layerPtr;
    return obj && [obj isKindOfClass:[CAMetalLayer class]];
}

const char* layerClassName(void* layerPtr) {
    id obj = (__bridge id) layerPtr;
    gLayerClass = obj ? std::string(NSStringFromClass([obj class]).UTF8String) : "<nil>";
    return gLayerClass.c_str();
}

bool viewOwnsLayerDirectly(void* nsViewPtr, void* layerPtr) {
    NSView* view = (__bridge NSView*) nsViewPtr;
    id layer = (__bridge id) layerPtr;
    return view && layer && (view.layer == layer);
}

} // namespace macbridge
