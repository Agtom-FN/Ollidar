// DisplayLink_mac.mm — display-synchronised render clock on macOS.
//
// S3's render loop was a spinning QTimer(0) that burned ~83% of a core retrying
// Renderer::beginFrame(); REPORT.md §7 flags replacing it as C1 work. Two
// implementations, preferred in this order:
//
//   1. CADisplayLink obtained from the NSView (macOS 14+). Fires on the main
//      run loop — already the GUI thread — and follows the display the view is
//      on, including ProMotion's variable refresh. This is Apple's current API;
//      CVDisplayLink is deprecated as of macOS 15.
//   2. CVDisplayLink (all macOS versions). Its callback runs on a dedicated
//      high-priority CV thread, so the tick is forwarded to the GUI thread with
//      a queued invocation behind a pending flag that COALESCES: if the GUI
//      thread has not yet run the previous tick, the new one is counted and
//      dropped rather than queued. Without that, a frame that overran the
//      display interval would build a backlog of stale ticks.
//   3. Neither available → the shared QTimer fallback.
//
// Either way the rule from DisplayLink.h holds: every tick — and therefore
// every Filament call — happens on the GUI thread.
//
// Owner: C1.
#include "render/DisplayLink.h"
#include "render/DisplayLinkFallback.h"

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

#include <QObject>
#include <QRect>
#include <QScreen>
#include <QWindow>

#include <atomic>

namespace lidarscan {
namespace {

// Common base: owns the callback and the "one frame at a time" rule.
class MacDisplayLinkBase : public DisplayLink {
 public:
  MacDisplayLinkBase(QWindow* w, Tick tick) : window_(w), tick_(std::move(tick)) {}

  double nominalRefreshHz() const override { return hz_; }
  unsigned long long coalescedTicks() const override { return coalesced_; }

  // Runs on the GUI thread.
  void fire() {
    if (in_tick_) {
      ++coalesced_;
      return;
    }
    in_tick_ = true;
    if (tick_) tick_();
    in_tick_ = false;
  }

 protected:
  void refreshHzFromScreen() {
    QScreen* s = window_ ? window_->screen() : nullptr;
    hz_ = (s && s->refreshRate() > 1.0) ? s->refreshRate() : 60.0;
  }

  QWindow* window_ = nullptr;
  Tick tick_;
  double hz_ = 60.0;
  bool in_tick_ = false;
  unsigned long long coalesced_ = 0;
};

}  // namespace
}  // namespace lidarscan

// CADisplayLink needs an Objective-C selector target.
@interface LSDisplayLinkTarget : NSObject
@property(nonatomic, assign) void* holder;  // lidarscan::MacDisplayLinkBase*
- (void)onDisplayLink:(id)sender;
@end

@implementation LSDisplayLinkTarget
- (void)onDisplayLink:(id)sender {
  (void)sender;
  if (self.holder) static_cast<lidarscan::MacDisplayLinkBase*>(self.holder)->fire();
}
@end

namespace lidarscan {
namespace {

// ---------------------------------------------------------------------------
// 1. CADisplayLink — main-thread, macOS 14+
// ---------------------------------------------------------------------------
class CADisplayLinkImpl final : public MacDisplayLinkBase {
 public:
  CADisplayLinkImpl(QWindow* w, Tick tick, NSView* view)
      : MacDisplayLinkBase(w, std::move(tick)), view_(view) {
    refreshHzFromScreen();
  }

  ~CADisplayLinkImpl() override { stop(); }

  bool attach() {
    if (@available(macOS 14.0, *)) {
      target_ = [[LSDisplayLinkTarget alloc] init];
      target_.holder = static_cast<MacDisplayLinkBase*>(this);
      link_ = [view_ displayLinkWithTarget:target_ selector:@selector(onDisplayLink:)];
      return link_ != nil;
    }
    return false;
  }

  void start() override {
    if (running_ || !link_) return;
    [link_ addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
    running_ = true;
  }

  void stop() override {
    if (link_) {
      [link_ invalidate];
      link_ = nil;
    }
    running_ = false;
  }

  bool running() const override { return running_; }
  QString name() const override { return "CADisplayLink (NSView, macOS 14+)"; }

  // A view-backed CADisplayLink re-targets itself when the view moves to
  // another display; only the reported rate has to be refreshed.
  void retargetScreen() override { refreshHzFromScreen(); }

 private:
  NSView* view_ = nil;
  id link_ = nil;  // CADisplayLink*, held as id to avoid availability noise
  LSDisplayLinkTarget* target_ = nil;
  bool running_ = false;
};

// ---------------------------------------------------------------------------
// 2. CVDisplayLink — CV thread + coalescing hop to the GUI thread
// ---------------------------------------------------------------------------
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // CVDisplayLink: deprecated in macOS 15

class CVDisplayLinkImpl final : public MacDisplayLinkBase {
 public:
  CVDisplayLinkImpl(QWindow* w, Tick tick) : MacDisplayLinkBase(w, std::move(tick)) {
    refreshHzFromScreen();
    hop_ = new QObject();  // GUI-thread landing pad for the queued invocation
  }

  ~CVDisplayLinkImpl() override {
    stop();
    if (link_) {
      CVDisplayLinkRelease(link_);
      link_ = nullptr;
    }
    delete hop_;
  }

  bool attach() {
    if (CVDisplayLinkCreateWithActiveCGDisplays(&link_) != kCVReturnSuccess || !link_) {
      return false;
    }
    CVDisplayLinkSetOutputCallback(link_, &CVDisplayLinkImpl::callback, this);
    retargetScreen();
    return true;
  }

  void start() override {
    if (running_ || !link_) return;
    running_ = true;
    CVDisplayLinkStart(link_);
  }

  void stop() override {
    if (!running_ || !link_) return;
    running_ = false;
    CVDisplayLinkStop(link_);
  }

  bool running() const override { return running_; }
  QString name() const override { return "CVDisplayLink (CV thread -> GUI thread)"; }
  unsigned long long coalescedTicks() const override {
    return coalesced_ + cv_coalesced_.load(std::memory_order_relaxed);
  }

  void retargetScreen() override {
    refreshHzFromScreen();
    if (!link_ || !window_ || !window_->screen()) return;
    // Bind the link to the CGDirectDisplayID the window is actually on. Qt does
    // not expose it, so it is recovered from the NSScreen whose frame spans the
    // window's centre — the multi-monitor path S3 could not test (REPORT §5).
    const QRect g = window_->geometry();
    const double cx = g.center().x();
    for (NSScreen* s in [NSScreen screens]) {
      const NSRect f = [s frame];
      if (cx >= NSMinX(f) && cx < NSMaxX(f)) {
        NSNumber* n = [[s deviceDescription] objectForKey:@"NSScreenNumber"];
        if (n) {
          CVDisplayLinkSetCurrentCGDisplay(link_, (CGDirectDisplayID)[n unsignedIntValue]);
          return;
        }
      }
    }
  }

 private:
  static CVReturn callback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*,
                           CVOptionFlags, CVOptionFlags*, void* user) {
    auto* self = static_cast<CVDisplayLinkImpl*>(user);
    if (self->pending_.exchange(true, std::memory_order_acq_rel)) {
      self->cv_coalesced_.fetch_add(1, std::memory_order_relaxed);
      return kCVReturnSuccess;
    }
    QMetaObject::invokeMethod(
        self->hop_,
        [self] {
          self->pending_.store(false, std::memory_order_release);
          self->fire();
        },
        Qt::QueuedConnection);
    return kCVReturnSuccess;
  }

  CVDisplayLinkRef link_ = nullptr;
  QObject* hop_ = nullptr;
  std::atomic<bool> pending_{false};
  std::atomic<unsigned long long> cv_coalesced_{0};
  bool running_ = false;
};

#pragma clang diagnostic pop

}  // namespace

std::unique_ptr<DisplayLink> DisplayLink::create(QWindow* window, Tick tick) {
  if (window) {
    NSView* view =
        (__bridge NSView*)reinterpret_cast<void*>(static_cast<uintptr_t>(window->winId()));
    if (view) {
      auto ca = std::make_unique<CADisplayLinkImpl>(window, tick, view);
      if (ca->attach()) return ca;
    }
  }
  auto cv = std::make_unique<CVDisplayLinkImpl>(window, tick);
  if (cv->attach()) return cv;
  return detail::makeTimerDisplayLink(window, std::move(tick));
}

}  // namespace lidarscan
