// DisplayLink.h — the render-loop clock.
//
// S3's render loop was a spinning QTimer(0) that burned ~83% of a core retrying
// Renderer::beginFrame(). That was a deliberate benchmark artefact (it maximises
// measurement resolution) and REPORT.md §7 lists replacing it as C1 work:
// "production should drive the loop from CVDisplayLink/CADisplayLink on macOS
// and the equivalent elsewhere".
//
// Contract:
//   * tick() is always invoked ON THE GUI THREAD. Filament's Engine is used
//     from exactly one thread for the whole app lifetime, and that thread is
//     also the one that owns the QWindow, the swapchain and the resize handling
//     — so there is no cross-thread swapchain race by construction.
//   * Ticks COALESCE. If a frame overruns the display interval, pending ticks
//     collapse into one instead of queueing up; a slow frame must never turn
//     into a backlog that the app then has to work through.
//   * The link is display-accurate: it follows the refresh rate of the screen
//     the window is actually on, and re-targets when the window moves to
//     another screen (the multi-monitor case S3 could not test — §5).
//
// Owner: C1.
#pragma once

#include <QString>
#include <functional>
#include <memory>

class QWindow;

namespace lidarscan {

class DisplayLink {
 public:
  using Tick = std::function<void()>;

  // Never returns nullptr: if no platform display link is available, a
  // timer-paced fallback is returned and name() says so.
  static std::unique_ptr<DisplayLink> create(QWindow* window, Tick tick);

  virtual ~DisplayLink() = default;

  virtual void start() = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;

  // Nominal refresh of the display the window is on, in Hz (0 if unknown).
  virtual double nominalRefreshHz() const = 0;

  // How many ticks were dropped by coalescing — a direct measure of how often
  // a frame overran the display interval.
  virtual unsigned long long coalescedTicks() const = 0;

  // e.g. "CADisplayLink (macOS 14+)", "CVDisplayLink", "QTimer fallback".
  virtual QString name() const = 0;

  // Called when the window moves to a different screen: re-target the link at
  // the new display's refresh rate.
  virtual void retargetScreen() = 0;
};

}  // namespace lidarscan
