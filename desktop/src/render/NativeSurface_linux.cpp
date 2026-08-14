// NativeSurface_linux.cpp — Linux / Vulkan surface shim.
//
// STATUS: UNVERIFIED, and per S3 §8 this is the HARD one — harder than Windows.
// The spike's own words: "Linux is the hard case — Filament's
// SwapChain::CONFIG_ENABLE_XCB exists precisely because X11 vs XCB vs Wayland
// handles differ, and Qt on Wayland gives a wl_surface, which needs
// VK_KHR_wayland_surface. Expect a Linux-specific investigation."
//
// The three cases this file has to grow into:
//
//   1. X11 (QPA platform "xcb", X11 session). QWindow::winId() is an X11
//      `Window` (an XID, i.e. an unsigned long). Filament's Vulkan backend
//      accepts it as the native window when the swapchain is created WITHOUT
//      SwapChain::CONFIG_ENABLE_XCB — the Xlib path. This is what the code
//      below does, and it is the case most likely to work first.
//
//   2. XCB. Same XID, but the swapchain must be created with
//      SwapChain::CONFIG_ENABLE_XCB so Filament uses
//      VK_KHR_xcb_surface instead of VK_KHR_xlib_surface. Which one is correct
//      depends on how Filament was built and on the driver's extension list;
//      the flag is plumbed through swapChainConfigFlags() below so the caller
//      can pass it without ViewportWindow needing platform #ifdefs.
//
//   3. Wayland (QPA platform "wayland"). winId() is NOT a usable handle.
//      Filament needs a `struct wl_surface*` (and the display), obtainable via
//      QGuiApplication::platformNativeInterface()->nativeResourceForWindow(
//          "surface", window) and ..."display"). Filament's SwapChain also
//      wants a size for Wayland because a wl_surface carries none. That is why
//      this file REFUSES Wayland rather than handing over a wrong pointer: an
//      honest error beats a PreconditionPanic on a driver thread, which is
//      exactly the failure mode S3 hit on macOS (REPORT.md §3).
//      Workaround for users today: run with QT_QPA_PLATFORM=xcb (XWayland).
//
// Also inherited from S3 §8 and applying to every Vulkan target: resize is
// riskier than on Metal (VK_ERROR_OUT_OF_DATE_KHR + in-flight fences),
// gl_PointSize needs `largePoints` and is weak on some Intel/AMD Linux drivers
// (fallback: instanced quads — see materials/points.mat), and 10M points at
// 60 fps is NOT expected on integrated GPUs without A14's LOD.
//
// Owner: C1 (seam) / a dedicated Linux spike + C8 (make it real).
#include "render/NativeSurface.h"

#include <QGuiApplication>
#include <QWindow>

namespace lidarscan {
namespace {

class LinuxX11Surface final : public NativeSurface {
 public:
  LinuxX11Surface(void* handle, bool xcb)
      : NativeSurface(RenderBackend::kVulkan), handle_(handle), xcb_(xcb) {}

  void* swapChainHandle() const override { return handle_; }
  void configure(const SurfaceGeometry&, bool) override {}
  bool needsSwapChainRecreateOnResize() const override { return false; }  // ASSUMPTION — see header
  bool isVerifiedPlatform() const override { return false; }

  // ViewportWindow ORs this into the createSwapChain() flags.
  bool wantsXcbConfigFlag() const { return xcb_; }

  QString describe() const override {
    return QString("Vulkan · X11 window 0x%1 (%2) · UNVERIFIED (S3 proved Metal/macOS only)")
        .arg(reinterpret_cast<qulonglong>(handle_), 0, 16)
        .arg(xcb_ ? "xcb" : "xlib");
  }

 private:
  void* handle_ = nullptr;
  bool xcb_ = false;
};

}  // namespace

RenderBackend NativeSurface::backendForThisPlatform() { return RenderBackend::kVulkan; }

std::unique_ptr<NativeSurface> NativeSurface::create(QWindow* window, QString* error) {
  if (!window) {
    if (error) *error = "NativeSurface::create: null QWindow";
    return nullptr;
  }
  const QString platform = QGuiApplication::platformName();

  if (platform.startsWith("wayland")) {
    if (error) {
      *error =
          "Wayland is not supported yet: Filament needs a wl_surface (via "
          "QPlatformNativeInterface) plus an explicit size, not QWindow::winId(). "
          "Run with QT_QPA_PLATFORM=xcb (XWayland) until the Wayland path lands. "
          "See src/render/NativeSurface_linux.cpp for the full analysis.";
    }
    return nullptr;
  }
  if (!platform.startsWith("xcb")) {
    if (error) {
      *error = QString("unsupported Qt platform plugin '%1' — expected xcb").arg(platform);
    }
    return nullptr;
  }

  void* handle = reinterpret_cast<void*>(static_cast<uintptr_t>(window->winId()));
  if (!handle) {
    if (error) *error = "QWindow::winId() returned no X11 window id";
    return nullptr;
  }
  // Default to the Xlib path; LIDARSCAN_FILAMENT_XCB=1 selects the XCB one
  // without a rebuild, because which is correct depends on the Filament build
  // and the driver's extension list and will need experimentation.
  const bool xcb = qEnvironmentVariableIntValue("LIDARSCAN_FILAMENT_XCB") != 0;
  return std::make_unique<LinuxX11Surface>(handle, xcb);
}

}  // namespace lidarscan
