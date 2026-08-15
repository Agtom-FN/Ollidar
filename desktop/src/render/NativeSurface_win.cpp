// NativeSurface_win.cpp — Windows / Vulkan surface shim.
//
// STATUS: UNVERIFIED. Spike S3 proved Metal on macOS only; its §8 risk
// assessment for the Vulkan leg is reproduced below verbatim in the places it
// applies, because the next engineer to touch this file needs the analysis, not
// a pointer to a spike report. The exit criterion for S3 was "macOS + Windows
// or Linux"; only half of it is met, and closing the Windows half is scheduled
// with S7's first CI push (Tech Spec §5, risk row "Filament-embedded-in-Qt").
//
// What is believed to be true, and why:
//   * QWindow::winId() on the "windows" QPA platform IS an HWND. Unlike the
//     macOS case (S3 §3, where the documented NSView is rejected in favour of an
//     undocumented CAMetalLayer), Filament's Vulkan backend documents and
//     accepts an HWND for the native window, so this file is expected to be
//     nearly correct as written — but "expected" is not "measured".
//   * The QWindow must be QSurface::VulkanSurface. Qt will then not install its
//     own presentation surface, which is what we want: Filament owns the
//     VkSurfaceKHR.
//
// What is known to be RISKY (S3 §8, in priority order):
//   1. Resize. macOS needed 1,105 swapchain destroy/recreate cycles under S3's
//      resize storm and survived them. On Vulkan the same storm exercises
//      VK_ERROR_OUT_OF_DATE_KHR handling, in-flight-frame fencing and swapchain
//      recreation — the classic crash / validation-error surface. Budget real
//      time for it. needsSwapChainRecreateOnResize() returns false here on the
//      theory that Filament's Vulkan backend handles out-of-date swapchains
//      internally; THAT IS AN ASSUMPTION and is the first thing to test.
//   2. gl_PointSize. Verified on Metal. On Vulkan it requires the shader to
//      write PointSize AND the device to expose `largePoints`; some drivers
//      clamp the point-size range to 1.0. Mitigation if it fails: instanced
//      quad / billboard expansion in the vertex shader (4-6x vertex work).
//      See materials/points.mat.
//   3. Performance off Apple silicon is unknown. 10M @ 138 fps on an M4 with
//      the GPU ~7 ms busy says nothing about an integrated Intel/AMD laptop
//      GPU, which very likely will NOT hit 10M @ 60 fps and will need the LOD
//      path (A14) from day one.
//   4. libzstd / link-list drift applies on every platform — the Filament
//      version is pinned (v1.75.0) in tools/fetch_filament.sh for this reason.
//
// Owner: C1 (seam) / S7 + C8 (make it real).
#include "render/NativeSurface.h"

#include <QWindow>

#include <cstdint>  // uintptr_t — QWindow::winId() returns quintptr on every OS

namespace lidarscan {
namespace {

class WindowsVulkanSurface final : public NativeSurface {
 public:
  explicit WindowsVulkanSurface(void* hwnd)
      : NativeSurface(RenderBackend::kVulkan), hwnd_(hwnd) {}

  void* swapChainHandle() const override { return hwnd_; }

  void configure(const SurfaceGeometry&, bool) override {
    // Nothing to do: an HWND carries no drawable-size or vsync state of its own
    // the way a CAMetalLayer does. Vsync on Vulkan is the present mode chosen
    // when the swapchain is created (FIFO vs MAILBOX/IMMEDIATE), which Filament
    // selects internally — exposing it is part of finishing this file.
  }

  bool needsSwapChainRecreateOnResize() const override { return false; }  // ASSUMPTION — see header
  bool isVerifiedPlatform() const override { return false; }

  QString describe() const override {
    return QString("Vulkan · HWND %1 · UNVERIFIED (S3 proved Metal/macOS only)")
        .arg(reinterpret_cast<qulonglong>(hwnd_), 0, 16);
  }

 private:
  void* hwnd_ = nullptr;
};

}  // namespace

RenderBackend NativeSurface::backendForThisPlatform() { return RenderBackend::kVulkan; }

std::unique_ptr<NativeSurface> NativeSurface::create(QWindow* window, QString* error) {
  if (!window) {
    if (error) *error = "NativeSurface::create: null QWindow";
    return nullptr;
  }
  void* hwnd = reinterpret_cast<void*>(static_cast<uintptr_t>(window->winId()));
  if (!hwnd) {
    if (error) *error = "QWindow::winId() returned no HWND";
    return nullptr;
  }
  return std::make_unique<WindowsVulkanSurface>(hwnd);
}

}  // namespace lidarscan
