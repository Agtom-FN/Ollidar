// NativeSurface.h — the per-OS shim between Qt's native window handle and the
// handle Filament's Engine::createSwapChain() actually accepts.
//
// WHY THIS FILE EXISTS (spike S3, REPORT.md §3 and §9 caveat 3):
//   Filament's own SwapChain.h says "On OSX, any NSView can be used directly as
//   a nativeWindow with createSwapChain()". That is WRONG in v1.75: the Metal
//   backend's MetalSwapChain constructor takes a CAMetalLayer* and precondition-
//   checks the class of what it is handed, so passing QWindow::winId() (an
//   NSView*) aborts the driver thread with utils::PreconditionPanic. S3
//   reproduced this on demand (`--handle=nsview`, results/F-nsview-failure.log).
//   S3's recommendation was literally "write a per-OS NativeSurface shim in C1
//   rather than passing winId() around". This is it.
//
// The macOS implementation is the proven path (NativeSurface_mac.mm, 138-149 fps
// at 10M points, 1,105 swapchain recreates under a resize storm without a
// crash). The Windows and Linux implementations are UNVERIFIED — S3 §8 is
// reproduced inline in each of them so the next engineer inherits the analysis
// rather than rediscovering it.
//
// Owner: C1.
#pragma once

#include <QString>
#include <memory>

class QWindow;

namespace lidarscan {

// Which Filament backend this platform's surface is for. Kept as our own enum
// so the platform .mm/.cpp files never have to include Filament headers.
enum class RenderBackend {
  kMetal,   // macOS
  kVulkan,  // Windows, Linux
};

const char* to_string(RenderBackend b);

struct SurfaceGeometry {
  int px_w = 1;      // drawable size in PHYSICAL pixels
  int px_h = 1;
  double dpr = 1.0;  // devicePixelRatio / backing scale
  double pt_w = 1.0; // window size in logical points (macOS layer.frame)
  double pt_h = 1.0;
};

class NativeSurface {
 public:
  virtual ~NativeSurface() = default;

  // Create the surface for `window`. `window` must already have its surface
  // type set (QSurface::MetalSurface on macOS, VulkanSurface elsewhere) and
  // must have been created/exposed — the native handle does not exist before
  // that. Returns nullptr and fills `error` when the platform cannot provide a
  // usable handle (the honest answer on Wayland today).
  static std::unique_ptr<NativeSurface> create(QWindow* window, QString* error);

  // Which surface type the QWindow must be given BEFORE create() is called.
  // (Static because it is needed at QWindow construction time.)
  static RenderBackend backendForThisPlatform();

  // The pointer handed to filament::Engine::createSwapChain().
  virtual void* swapChainHandle() const = 0;

  // Push the current geometry / vsync setting into the platform surface. On
  // macOS this is contentsScale + drawableSize + displaySyncEnabled on the
  // CAMetalLayer, wrapped in a CATransaction with implicit animations off.
  virtual void configure(const SurfaceGeometry& geom, bool vsync) = 0;

  // Whether the swapchain must be destroyed and recreated when the drawable
  // size changes. True on macOS/Metal (CAMetalLayer does not renegotiate the
  // drawable size of a live swapchain — this is what Filament's own sample app
  // does, and what S3 hammered 1,105 times).
  virtual bool needsSwapChainRecreateOnResize() const = 0;

  // Human-readable provenance for the status bar / NOTES.md evidence: which
  // handle class was actually used, which GPU, which backend.
  virtual QString describe() const = 0;

  // False for the Windows/Linux stubs: the app shows a "renderer unverified on
  // this platform" banner rather than silently pretending S3 covered it.
  virtual bool isVerifiedPlatform() const = 0;

  RenderBackend backend() const { return backend_; }

 protected:
  explicit NativeSurface(RenderBackend b) : backend_(b) {}
  RenderBackend backend_;
};

}  // namespace lidarscan
