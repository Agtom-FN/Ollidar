// event_bus.h — typed pub/sub between engine threads and the app.
//
// SEMANTICS (contract for A2–A15 and for the app layers)
//
// Ordering
//   publish() assigns a globally increasing `sequence` under one bus mutex,
//   so events have a TOTAL order across all producer threads, and every
//   subscriber observes that order (FIFO per subscriber, no reordering).
//
// Delivery modes — chosen per subscription, never both:
//   • Queued (default). The event is copied into that subscriber's bounded
//     ring. The consumer drains it on ITS OWN thread with poll()/drain()/
//     wait(). This is what JNI and Qt use: the app pumps events on its UI or
//     capture thread and no engine thread ever touches app state.
//   • Callback. The callback runs INLINE ON THE PUBLISHING THREAD (a driver
//     thread) while the bus lock is held. It must be non-blocking, must not
//     publish, must not subscribe/unsubscribe, and must not call back into
//     the engine — doing so deadlocks. It exists for latency-critical
//     in-engine consumers (record/, live SLAM), not for UI code.
//
// Backpressure — a bounded ring per subscriber (default 1024 events):
//   kDropOldest (default) evicts the oldest queued event; kDropNewest drops
//   the arriving one; kBlock stalls the publisher until the consumer drains
//   (offline replay only — a stalled driver thread loses live data).
//   Drops are never silent: the NEXT event handed to that subscriber is a
//   synthesized EventType::kEventsDropped carrying how many were lost since
//   the last delivery and in total. Consumers that see it know their view of
//   the stream has a hole (a renderer, for instance, must fall back to a
//   full page re-read rather than trusting incremental ranges).
//
// Lifetime
//   unsubscribe() blocks until any in-flight callback for that subscription
//   has returned, so the caller's user_data is safe to free afterwards.
//   close() wakes every waiter and makes further publishes no-ops; the
//   Engine calls it in its destructor before joining anything.
//
// Owner: A1.
#ifndef SCANENGINE_CORE_EVENT_BUS_H
#define SCANENGINE_CORE_EVENT_BUS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/core/event.h"

namespace scanengine {

using SubscriptionId = std::uint32_t;
inline constexpr SubscriptionId kInvalidSubscription = 0;

// Invoked inline on the publishing thread. See the header comment.
using EventCallback = void (*)(const Event& ev, void* user_data);

enum class OverflowPolicy : std::uint8_t {
  kDropOldest = 0,
  kDropNewest = 1,
  kBlock = 2,
};

struct SubscriptionOptions {
  std::uint32_t category_mask = mask_of(EventCategory::kAll);
  std::uint32_t capacity = 1024;      // events; ignored in callback mode
  OverflowPolicy policy = OverflowPolicy::kDropOldest;
  EventCallback callback = nullptr;   // non-null selects callback mode
  void* user_data = nullptr;
};

class EventBus {
 public:
  EventBus();
  ~EventBus();

  EventBus(const EventBus&) = delete;
  EventBus& operator=(const EventBus&) = delete;

  Result<SubscriptionId> subscribe(const SubscriptionOptions& opts);
  Status unsubscribe(SubscriptionId id);

  // Producers. `ev.sequence` and `ev.t_mono_ns` are filled in by the bus
  // (t_mono_ns only when the caller left it 0 — a driver that already
  // arrival-stamped its data should pass that stamp through).
  void publish(Event ev);

  // Convenience for the common "type + payload" shape.
  template <typename PayloadT>
  void publish(EventType type, const PayloadT& payload, std::int64_t t_mono_ns = 0) {
    Event ev;
    ev.type = type;
    ev.t_mono_ns = t_mono_ns;
    static_assert(sizeof(PayloadT) <= sizeof(Event::Payload), "payload too large");
    copy_payload(ev, payload);
    publish(ev);
  }

  // Consumers (queued mode). All return/copy by value; none allocate.
  bool poll(SubscriptionId id, Event* out);
  bool wait(SubscriptionId id, Event* out, std::int32_t timeout_ms);
  std::size_t drain(SubscriptionId id, Event* out, std::size_t max_events);

  std::size_t queued(SubscriptionId id) const;
  std::uint64_t dropped_total(SubscriptionId id) const;
  std::size_t subscriber_count() const;
  std::uint32_t last_sequence() const;

  void close();
  bool closed() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Helper used by the template above; keeps the union copy in one place.
  template <typename PayloadT>
  static void copy_payload(Event& ev, const PayloadT& p) {
    static_assert(sizeof(PayloadT) <= sizeof(Event::Payload), "payload too large");
    const auto* src = reinterpret_cast<const std::uint8_t*>(&p);
    for (std::size_t i = 0; i < sizeof(PayloadT); ++i) ev.payload.raw[i] = src[i];
  }
};

}  // namespace scanengine

#endif  // SCANENGINE_CORE_EVENT_BUS_H
