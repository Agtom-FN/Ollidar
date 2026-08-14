// DisplayLink_generic.cpp — Windows / Linux render clock.
//
// Today this is the timer fallback (DisplayLinkFallback.cpp), which paces at the
// display's refresh RATE but not in phase with vblank. The per-platform
// replacements, and why they matter, are documented in DisplayLinkFallback.h.
// This file exists so that swapping one in is a local change with a call site
// already in place, exactly like NativeSurface_{win,linux}.cpp.
//
// Owner: C1 (seam) / C8 + the Vulkan leg (make it real).
#include "render/DisplayLink.h"
#include "render/DisplayLinkFallback.h"

namespace lidarscan {

std::unique_ptr<DisplayLink> DisplayLink::create(QWindow* window, Tick tick) {
  return detail::makeTimerDisplayLink(window, std::move(tick));
}

}  // namespace lidarscan
