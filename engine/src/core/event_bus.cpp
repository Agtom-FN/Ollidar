#include "scanengine/core/event_bus.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "scanengine/core/log.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {
namespace {
constexpr const char* kMod = "eventbus";
}

const char* to_string(EventType t) noexcept {
  switch (t) {
    case EventType::kNone: return "none";
    case EventType::kEventsDropped: return "events-dropped";
    case EventType::kEngineState: return "engine-state";
    case EventType::kSessionState: return "session-state";
    case EventType::kDeviceState: return "device-state";
    case EventType::kDeviceHealth: return "device-health";
    case EventType::kPointsAvailable: return "points-available";
    case EventType::kRotation: return "rotation";
    case EventType::kPoseUpdate: return "pose-update";
    case EventType::kGnssFix: return "gnss-fix";
    case EventType::kNtripState: return "ntrip-state";
    case EventType::kGeorefConverged: return "georef-converged";
    case EventType::kJobProgress: return "job-progress";
    case EventType::kError: return "error";
  }
  return "?";
}

EventCategory category_of(EventType t) noexcept {
  switch (t) {
    case EventType::kEventsDropped: return EventCategory::kMeta;
    case EventType::kEngineState:
    case EventType::kSessionState: return EventCategory::kEngine;
    case EventType::kDeviceState:
    case EventType::kDeviceHealth: return EventCategory::kDevice;
    case EventType::kPointsAvailable:
    case EventType::kRotation: return EventCategory::kPoints;
    case EventType::kPoseUpdate:
    case EventType::kGnssFix:
    case EventType::kNtripState:
    case EventType::kGeorefConverged: return EventCategory::kPose;
    case EventType::kJobProgress: return EventCategory::kJobs;
    case EventType::kError: return EventCategory::kErrors;
    case EventType::kNone: return EventCategory::kNone;
  }
  return EventCategory::kNone;
}

// --- subscriber -------------------------------------------------------------

struct Subscriber {
  SubscriptionId id = kInvalidSubscription;
  std::uint32_t mask = 0;
  std::uint32_t capacity = 0;
  OverflowPolicy policy = OverflowPolicy::kDropOldest;
  EventCallback callback = nullptr;
  void* user = nullptr;

  mutable std::mutex m;
  std::condition_variable data_cv;   // consumer waits for events
  std::condition_variable space_cv;  // kBlock publisher waits for room
  std::deque<Event> q;
  std::uint64_t dropped_pending = 0;
  std::uint64_t dropped_total = 0;
  bool closed = false;

  bool accepts(EventType t) const {
    // Meta events (kEventsDropped) are always delivered: a subscriber must
    // learn about holes in its own stream regardless of its filter.
    if (t == EventType::kEventsDropped) return true;
    return (mask & mask_of(category_of(t))) != 0;
  }

  // Called with `m` held. Returns the event to hand out, synthesizing the
  // drop notice first when one is pending.
  bool pop_locked(Event* out) {
    if (dropped_pending > 0) {
      Event ev;
      ev.type = EventType::kEventsDropped;
      ev.t_mono_ns = SteadyClock::now().nanos;
      ev.payload.dropped.count = dropped_pending;
      ev.payload.dropped.total = dropped_total;
      dropped_pending = 0;
      *out = ev;
      return true;
    }
    if (q.empty()) return false;
    *out = q.front();
    q.pop_front();
    space_cv.notify_one();
    return true;
  }

  bool has_deliverable_locked() const { return dropped_pending > 0 || !q.empty(); }
};

struct EventBus::Impl {
  mutable std::mutex bus_m;
  std::vector<std::shared_ptr<Subscriber>> subs;
  SubscriptionId next_id = 1;
  std::uint32_t sequence = 0;
  bool closed = false;

  std::shared_ptr<Subscriber> find(SubscriptionId id) const {
    std::lock_guard<std::mutex> lock(bus_m);
    for (const auto& s : subs) {
      if (s->id == id) return s;
    }
    return nullptr;
  }
};

EventBus::EventBus() : impl_(new Impl) {}

EventBus::~EventBus() { close(); }

Result<SubscriptionId> EventBus::subscribe(const SubscriptionOptions& opts) {
  if (opts.callback == nullptr && opts.capacity == 0) {
    return set_last_error(ScanError::kInvalidArgument,
                          "event bus: queued subscription needs capacity > 0");
  }
  auto s = std::make_shared<Subscriber>();
  s->mask = opts.category_mask;
  s->capacity = opts.capacity;
  s->policy = opts.policy;
  s->callback = opts.callback;
  s->user = opts.user_data;

  std::lock_guard<std::mutex> lock(impl_->bus_m);
  if (impl_->closed) {
    return set_last_error(ScanError::kInvalidState, "event bus is closed");
  }
  s->id = impl_->next_id++;
  impl_->subs.push_back(s);
  return s->id;
}

Status EventBus::unsubscribe(SubscriptionId id) {
  std::shared_ptr<Subscriber> victim;
  {
    std::lock_guard<std::mutex> lock(impl_->bus_m);
    auto it = std::find_if(impl_->subs.begin(), impl_->subs.end(),
                           [id](const std::shared_ptr<Subscriber>& s) { return s->id == id; });
    if (it == impl_->subs.end()) {
      return set_last_error(ScanError::kNotFound, "event bus: no subscription %u", id);
    }
    victim = *it;
    impl_->subs.erase(it);
  }
  // Removal happened under the bus lock, which publish() also holds while
  // dispatching, so no callback can start after this point. Taking the
  // subscriber lock waits for one already running.
  {
    std::lock_guard<std::mutex> lock(victim->m);
    victim->closed = true;
    victim->q.clear();
  }
  victim->data_cv.notify_all();
  victim->space_cv.notify_all();
  return kOkStatus;
}

void EventBus::publish(Event ev) {
  std::lock_guard<std::mutex> lock(impl_->bus_m);
  if (impl_->closed) return;

  ev.sequence = ++impl_->sequence;
  if (ev.t_mono_ns == 0) ev.t_mono_ns = SteadyClock::now().nanos;

  for (const auto& s : impl_->subs) {
    if (!s->accepts(ev.type)) continue;

    if (s->callback != nullptr) {
      // Inline dispatch on the publishing thread; see header contract.
      std::lock_guard<std::mutex> slock(s->m);
      if (s->closed) continue;
      s->callback(ev, s->user);
      continue;
    }

    std::unique_lock<std::mutex> slock(s->m);
    if (s->closed) continue;
    if (s->q.size() >= s->capacity) {
      switch (s->policy) {
        case OverflowPolicy::kDropOldest:
          s->q.pop_front();
          ++s->dropped_pending;
          ++s->dropped_total;
          break;
        case OverflowPolicy::kDropNewest:
          ++s->dropped_pending;
          ++s->dropped_total;
          continue;  // arriving event is discarded
        case OverflowPolicy::kBlock:
          // Offline/replay only: stalls this producer thread. The bus lock
          // is held, so other producers stall too — never enable this on a
          // live capture path.
          s->space_cv.wait(slock, [&] { return s->closed || s->q.size() < s->capacity; });
          if (s->closed) continue;
          break;
      }
    }
    s->q.push_back(ev);
    slock.unlock();
    s->data_cv.notify_one();
  }
}

bool EventBus::poll(SubscriptionId id, Event* out) {
  if (out == nullptr) return false;
  auto s = impl_->find(id);
  if (!s) return false;
  std::lock_guard<std::mutex> lock(s->m);
  return s->pop_locked(out);
}

bool EventBus::wait(SubscriptionId id, Event* out, std::int32_t timeout_ms) {
  if (out == nullptr) return false;
  auto s = impl_->find(id);
  if (!s) return false;
  std::unique_lock<std::mutex> lock(s->m);
  if (!s->has_deliverable_locked()) {
    if (timeout_ms < 0) {
      s->data_cv.wait(lock, [&] { return s->closed || s->has_deliverable_locked(); });
    } else {
      s->data_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [&] { return s->closed || s->has_deliverable_locked(); });
    }
  }
  return s->pop_locked(out);
}

std::size_t EventBus::drain(SubscriptionId id, Event* out, std::size_t max_events) {
  if (out == nullptr || max_events == 0) return 0;
  auto s = impl_->find(id);
  if (!s) return 0;
  std::lock_guard<std::mutex> lock(s->m);
  std::size_t n = 0;
  while (n < max_events && s->pop_locked(&out[n])) ++n;
  return n;
}

std::size_t EventBus::queued(SubscriptionId id) const {
  auto s = impl_->find(id);
  if (!s) return 0;
  std::lock_guard<std::mutex> lock(s->m);
  return s->q.size();
}

std::uint64_t EventBus::dropped_total(SubscriptionId id) const {
  auto s = impl_->find(id);
  if (!s) return 0;
  std::lock_guard<std::mutex> lock(s->m);
  return s->dropped_total;
}

std::size_t EventBus::subscriber_count() const {
  std::lock_guard<std::mutex> lock(impl_->bus_m);
  return impl_->subs.size();
}

std::uint32_t EventBus::last_sequence() const {
  std::lock_guard<std::mutex> lock(impl_->bus_m);
  return impl_->sequence;
}

void EventBus::close() {
  std::vector<std::shared_ptr<Subscriber>> victims;
  {
    std::lock_guard<std::mutex> lock(impl_->bus_m);
    if (impl_->closed) return;
    impl_->closed = true;
    victims.swap(impl_->subs);
  }
  for (const auto& s : victims) {
    {
      std::lock_guard<std::mutex> lock(s->m);
      s->closed = true;
    }
    s->data_cv.notify_all();
    s->space_cv.notify_all();
  }
  SCAN_LOG_DEBUG(kMod, "closed (%zu subscribers released)", victims.size());
}

bool EventBus::closed() const {
  std::lock_guard<std::mutex> lock(impl_->bus_m);
  return impl_->closed;
}

}  // namespace scanengine
