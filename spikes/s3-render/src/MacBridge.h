#pragma once
#include <cstdint>

// Thin Objective-C++ shim between Qt's native window handle and Filament's
// Metal swapchain. Everything here is macOS-specific; the Windows/Linux port
// would replace it with HWND / xcb-or-wayland handle plumbing for Vulkan.
namespace macbridge {

// QWindow::winId() on macOS is an NSView*. Filament's SwapChain docs state an
// NSView can be handed to createSwapChain() directly.
void* viewFromWinId(uint64_t winId);

// Ensure the view is layer-backed before Filament touches it.
void prepareView(void* nsView);

// Plan-B / control path: create our own CAMetalLayer as a sublayer of the view
// and return it (also accepted by Engine::createSwapChain).
void* attachOwnMetalLayer(void* nsView, double scale, int wpx, int hpx);

// Locate the CAMetalLayer actually in use (whether Filament or we created it).
void* findMetalLayer(void* nsView);

// Keep the drawable in sync with the window: geometry in points, backing scale,
// explicit drawableSize in pixels. `vsync=false` sets displaySyncEnabled=NO so
// the benchmark can measure uncapped throughput.
void configureLayer(void* layer, double scale, int wpx, int hpx,
                    double frameW, double frameH, bool vsync);

// Class name of the view's own backing layer (Qt installs QMetalLayer for
// QSurface::MetalSurface windows).
const char* viewLayerClassName(void* nsView);

// Descriptive info for the report.
const char* metalDeviceName();
double displayRefreshHz();
uint64_t currentDrawableW(void* layer);
uint64_t currentDrawableH(void* layer);
bool layerIsMetal(void* layer);
const char* layerClassName(void* layer);
bool viewOwnsLayerDirectly(void* nsView, void* layer);

} // namespace macbridge
