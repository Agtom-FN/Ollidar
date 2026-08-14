#include "render/DisplayLinkFallback.h"

#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <atomic>

namespace lidarscan {
namespace detail {
namespace {

class TimerDisplayLink final : public DisplayLink {
 public:
  TimerDisplayLink(QWindow* w, Tick tick) : window_(w), tick_(std::move(tick)) {
    timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer_, &QTimer::timeout, &timer_, [this] {
      if (in_tick_) {  // never re-enter a frame
        ++coalesced_;
        return;
      }
      in_tick_ = true;
      if (tick_) tick_();
      in_tick_ = false;
    });
    retargetScreen();
  }

  void start() override {
    if (running_) return;
    running_ = true;
    timer_.start(interval_ms_);
  }

  void stop() override {
    running_ = false;
    timer_.stop();
  }

  bool running() const override { return running_; }
  double nominalRefreshHz() const override { return hz_; }
  unsigned long long coalescedTicks() const override { return coalesced_; }
  QString name() const override {
    return QString("QTimer fallback @ %1 ms").arg(interval_ms_);
  }

  void retargetScreen() override {
    QScreen* s = window_ ? window_->screen() : nullptr;
    hz_ = (s && s->refreshRate() > 1.0) ? s->refreshRate() : 60.0;
    interval_ms_ = std::max(1, static_cast<int>(1000.0 / hz_));
    if (running_) timer_.start(interval_ms_);
  }

 private:
  QWindow* window_ = nullptr;
  Tick tick_;
  QTimer timer_;
  bool running_ = false;
  bool in_tick_ = false;
  int interval_ms_ = 16;
  double hz_ = 60.0;
  unsigned long long coalesced_ = 0;
};

}  // namespace

std::unique_ptr<DisplayLink> makeTimerDisplayLink(QWindow* window, DisplayLink::Tick tick) {
  return std::make_unique<TimerDisplayLink>(window, std::move(tick));
}

}  // namespace detail
}  // namespace lidarscan
