// NativeSurface_mac.mm — the macOS/Metal implementation of the C1 surface shim.
//
// Productionized from spikes/s3-render/src/MacBridge.mm. What changed:
//   * the three experimental handle modes (qtlayer / ownlayer / nsview) that
//     existed to FIND the working path collapse to the one that works —
//     Qt's own QMetalLayer, which S3 measured at 138.8 fps vs 139.9 for an
//     app-created layer (within noise) and which keeps Qt's compositing,
//     window-server integration and DPI handling intact;
//   * an app-created CAMetalLayer survives only as an automatic fallback for
//     the case where Qt's layer cannot be found (a Qt version bump changing the
//     layer topology is exactly the drift S3 warned about in §9 caveat 2);
//   * the reporting/diagnostic entry points S3 used for its report are folded
//     into one describe() string.
//
// Owner: C1.
#include "render/NativeSurface.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include <QWindow>

namespace lidarscan {

namespace {

CAMetalLayer* findQtMetalLayer(NSView* view) {
  if (!view) return nil;
  // Observed topology on Qt 6.11.1: winId()'s NSView.layer is a QContainerLayer
  // with Qt's QMetalLayer (a CAMetalLayer subclass) as a sublayer. Older Qt
  // versions put the CAMetalLayer directly on the view. Handle both, one level
  // deep — deeper would be guessing.
  if ([view.layer isKindOfClass:[CAMetalLayer class]]) {
    return (CAMetalLayer*)view.layer;
  }
  for (CALayer* sub in view.layer.sublayers) {
    if ([sub isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer*)sub;
  }
  return nil;
}

class MacMetalSurface final : public NativeSurface {
 public:
  MacMetalSurface(CAMetalLayer* layer, bool ownedLayer)
      : NativeSurface(RenderBackend::kMetal), layer_(layer), owned_layer_(ownedLayer) {}

  ~MacMetalSurface() override {
    if (owned_layer_ && layer_) [layer_ removeFromSuperlayer];
  }

  void* swapChainHandle() const override { return (__bridge void*)layer_; }

  void configure(const SurfaceGeometry& g, bool vsync) override {
    if (!layer_) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];  // no implicit animation on resize
    layer_.contentsScale = g.dpr;
    if (g.pt_w > 0.0 && g.pt_h > 0.0) {
      layer_.frame = CGRectMake(0, 0, g.pt_w, g.pt_h);
    }
    layer_.drawableSize = CGSizeMake(g.px_w, g.px_h);
    if (@available(macOS 10.13, *)) {
      layer_.displaySyncEnabled = vsync ? YES : NO;
      // A blocking nextDrawable is what makes Renderer::beginFrame() return
      // false instead of stalling; a timeout here would surface as a torn or
      // dropped frame rather than as backpressure.
      layer_.allowsNextDrawableTimeout = NO;
    }
    layer_.maximumDrawableCount = 3;
    [CATransaction commit];
  }

  bool needsSwapChainRecreateOnResize() const override { return true; }

  bool isVerifiedPlatform() const override { return true; }

  QString describe() const override {
    NSString* layerClass = layer_ ? NSStringFromClass([layer_ class]) : @"<nil>";
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSString* devName = dev ? [dev name] : @"<no Metal device>";
    return QString("Metal · %1 · handle=%2 (%3) · %4 Hz display")
        .arg(QString::fromUtf8([devName UTF8String]))
        .arg(QString::fromUtf8([layerClass UTF8String]))
        .arg(owned_layer_ ? "app-created sublayer" : "Qt-owned")
        .arg(refreshHz(), 0, 'f', 0);
  }

  static double refreshHz() {
    NSScreen* s = [NSScreen mainScreen];
    if (@available(macOS 12.0, *)) {
      const double hz = [s maximumFramesPerSecond];
      if (hz > 0) return hz;
    }
    return 60.0;
  }

 private:
  CAMetalLayer* layer_ = nil;
  bool owned_layer_ = false;
};

}  // namespace

RenderBackend NativeSurface::backendForThisPlatform() { return RenderBackend::kMetal; }

std::unique_ptr<NativeSurface> NativeSurface::create(QWindow* window, QString* error) {
  if (!window) {
    if (error) *error = "NativeSurface::create: null QWindow";
    return nullptr;
  }
  NSView* view = (__bridge NSView*)reinterpret_cast<void*>(
      static_cast<uintptr_t>(window->winId()));
  if (!view) {
    if (error) *error = "QWindow::winId() returned no NSView";
    return nullptr;
  }
  [view setWantsLayer:YES];
  view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;

  CAMetalLayer* layer = findQtMetalLayer(view);
  bool owned = false;
  if (!layer) {
    // Fallback: Qt did not give us a CAMetalLayer (wrong surface type, or a Qt
    // version that changed its layer topology). Make our own — S3 benchmarked
    // this path too (results/E-ownlayer.md, 139.9 fps at 10M).
    layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.frame = view.bounds;
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    layer.needsDisplayOnBoundsChange = YES;
    [view.layer addSublayer:layer];
    owned = true;
  }
  if (!layer) {
    if (error) *error = "could not obtain or create a CAMetalLayer";
    return nullptr;
  }
  return std::make_unique<MacMetalSurface>(layer, owned);
}

}  // namespace lidarscan
