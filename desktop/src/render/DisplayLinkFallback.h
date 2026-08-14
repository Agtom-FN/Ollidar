// Timer-paced DisplayLink fallback, shared by every platform.
//
// This is the seam Windows and Linux use today (and macOS uses if both
// CADisplayLink and CVDisplayLink fail to start). It is NOT a spin loop: the
// timer period is the display's refresh interval, so it costs one wakeup per
// frame like a real display link. What it does not give is phase alignment with
// vblank — frames are submitted at the right RATE but not at the right MOMENT,
// so a frame can miss a vsync window and show up as a jitter spike.
//
// Replacing it per platform (C8 / the Vulkan leg):
//   Windows — DXGI's waitable swapchain object (IDXGISwapChain2::
//     GetFrameLatencyWaitableObject) or DwmGetCompositionTimingInfo; with
//     Vulkan, VK_KHR_present_wait / VK_GOOGLE_display_timing where available.
//   Linux   — the presentation feedback of the compositor: Wayland's
//     wp_presentation_feedback, or GLX/DRM vblank counters under X11;
//     VK_KHR_present_wait again where the driver exposes it.
#pragma once

#include "render/DisplayLink.h"

class QWindow;

namespace lidarscan {
namespace detail {

std::unique_ptr<DisplayLink> makeTimerDisplayLink(QWindow* window, DisplayLink::Tick tick);

}  // namespace detail
}  // namespace lidarscan
