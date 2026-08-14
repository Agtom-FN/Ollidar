// Event bus: ordering, filtering, overflow policy, threaded producers,
// callback mode, unsubscribe lifetime.
#include <atomic>
#include <thread>
#include <vector>

#include "doctest.h"
#include "scanengine/core/event_bus.h"

using namespace scanengine;

namespace {

Event make(EventType t, std::uint32_t device = 0) {
  Event e;
  e.type = t;
  e.payload.device.device = device;
  return e;
}

}  // namespace

TEST_CASE("bus/queued_delivery_preserves_order_and_assigns_sequences") {
  EventBus bus;
  auto sub = bus.subscribe({});
  REQUIRE(sub.ok());

  for (std::uint32_t i = 0; i < 5; ++i) bus.publish(make(EventType::kDeviceState, i));
  CHECK(bus.queued(sub.value()) == 5);
  CHECK(bus.last_sequence() == 5);

  Event ev;
  for (std::uint32_t i = 0; i < 5; ++i) {
    REQUIRE(bus.poll(sub.value(), &ev));
    CHECK(ev.sequence == i + 1);
    CHECK(ev.payload.device.device == i);
    CHECK(ev.t_mono_ns > 0);  // the bus stamps events the producer left at 0
  }
  CHECK_FALSE(bus.poll(sub.value(), &ev));
}

TEST_CASE("bus/publisher_supplied_timestamps_are_preserved") {
  EventBus bus;
  auto sub = bus.subscribe({});
  Event e = make(EventType::kPointsAvailable);
  e.t_mono_ns = 123456;
  bus.publish(e);
  Event out;
  REQUIRE(bus.poll(sub.value(), &out));
  CHECK(out.t_mono_ns == 123456);
}

TEST_CASE("bus/category_mask_filters") {
  EventBus bus;
  SubscriptionOptions opts;
  opts.category_mask = mask_of(EventCategory::kPoints);
  auto sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  bus.publish(make(EventType::kDeviceState));
  bus.publish(make(EventType::kPointsAvailable));
  bus.publish(make(EventType::kEngineState));

  Event ev;
  REQUIRE(bus.poll(sub.value(), &ev));
  CHECK(ev.type == EventType::kPointsAvailable);
  CHECK_FALSE(bus.poll(sub.value(), &ev));
}

TEST_CASE("bus/drain_returns_a_batch") {
  EventBus bus;
  auto sub = bus.subscribe({});
  for (int i = 0; i < 10; ++i) bus.publish(make(EventType::kRotation));
  Event batch[4];
  CHECK(bus.drain(sub.value(), batch, 4) == 4);
  CHECK(bus.queued(sub.value()) == 6);
}

TEST_CASE("bus/overflow_drop_oldest_reports_the_hole") {
  EventBus bus;
  SubscriptionOptions opts;
  opts.capacity = 4;
  opts.policy = OverflowPolicy::kDropOldest;
  auto sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  for (std::uint32_t i = 0; i < 10; ++i) bus.publish(make(EventType::kDeviceState, i));
  CHECK(bus.dropped_total(sub.value()) == 6);

  // The drop notice comes FIRST, then the surviving newest events.
  Event ev;
  REQUIRE(bus.poll(sub.value(), &ev));
  CHECK(ev.type == EventType::kEventsDropped);
  CHECK(ev.payload.dropped.count == 6);
  CHECK(ev.payload.dropped.total == 6);

  REQUIRE(bus.poll(sub.value(), &ev));
  CHECK(ev.payload.device.device == 6);  // oldest survivor
  CHECK(bus.queued(sub.value()) == 3);
}

TEST_CASE("bus/overflow_drop_newest_keeps_the_oldest") {
  EventBus bus;
  SubscriptionOptions opts;
  opts.capacity = 3;
  opts.policy = OverflowPolicy::kDropNewest;
  auto sub = bus.subscribe(opts);

  for (std::uint32_t i = 0; i < 6; ++i) bus.publish(make(EventType::kDeviceState, i));
  CHECK(bus.dropped_total(sub.value()) == 3);

  Event ev;
  REQUIRE(bus.poll(sub.value(), &ev));
  CHECK(ev.type == EventType::kEventsDropped);
  REQUIRE(bus.poll(sub.value(), &ev));
  CHECK(ev.payload.device.device == 0);  // oldest kept
}

TEST_CASE("bus/callback_mode_runs_inline_on_the_publishing_thread") {
  EventBus bus;
  struct Ctx {
    std::vector<std::uint32_t> seqs;
    std::thread::id thread;
  } ctx;
  ctx.thread = std::this_thread::get_id();

  SubscriptionOptions opts;
  opts.callback = [](const Event& ev, void* user) {
    auto* c = static_cast<Ctx*>(user);
    CHECK(std::this_thread::get_id() == c->thread);
    c->seqs.push_back(ev.sequence);
  };
  opts.user_data = &ctx;
  auto sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  bus.publish(make(EventType::kDeviceState));
  bus.publish(make(EventType::kRotation));
  CHECK(ctx.seqs.size() == 2);
  CHECK(ctx.seqs[0] == 1);
  CHECK(ctx.seqs[1] == 2);
  // Callback subscriptions never queue.
  CHECK(bus.queued(sub.value()) == 0);
}

TEST_CASE("bus/unsubscribe_stops_delivery_and_is_idempotent_by_error") {
  EventBus bus;
  auto sub = bus.subscribe({});
  REQUIRE(sub.ok());
  CHECK(bus.subscriber_count() == 1);
  CHECK(bus.unsubscribe(sub.value()).ok());
  CHECK(bus.subscriber_count() == 0);
  CHECK(bus.unsubscribe(sub.value()).error() == ScanError::kNotFound);

  bus.publish(make(EventType::kDeviceState));
  Event ev;
  CHECK_FALSE(bus.poll(sub.value(), &ev));
}

TEST_CASE("bus/threaded_producers_are_totally_ordered_and_lose_nothing") {
  EventBus bus;
  SubscriptionOptions opts;
  opts.capacity = 100000;
  auto sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  constexpr int kThreads = 4;
  constexpr int kPerThread = 2000;
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&bus, t] {
      for (int i = 0; i < kPerThread; ++i) {
        bus.publish(make(EventType::kPointsAvailable, static_cast<std::uint32_t>(t)));
      }
    });
  }
  for (auto& th : producers) th.join();

  CHECK(bus.queued(sub.value()) == kThreads * kPerThread);
  CHECK(bus.dropped_total(sub.value()) == 0);

  Event ev;
  std::uint32_t prev = 0;
  int seen = 0;
  std::vector<int> per_thread(kThreads, 0);
  while (bus.poll(sub.value(), &ev)) {
    CHECK(ev.sequence > prev);  // strictly increasing: a total order
    prev = ev.sequence;
    ++per_thread[ev.payload.device.device];
    ++seen;
  }
  CHECK(seen == kThreads * kPerThread);
  for (int t = 0; t < kThreads; ++t) CHECK(per_thread[t] == kPerThread);
}

TEST_CASE("bus/wait_blocks_until_a_producer_publishes") {
  EventBus bus;
  auto sub = bus.subscribe({});
  REQUIRE(sub.ok());

  std::thread producer([&bus] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bus.publish(make(EventType::kSessionState));
  });

  Event ev;
  CHECK(bus.wait(sub.value(), &ev, 5000));
  CHECK(ev.type == EventType::kSessionState);
  producer.join();

  // ...and times out when nothing arrives.
  CHECK_FALSE(bus.wait(sub.value(), &ev, 10));
}

TEST_CASE("bus/close_wakes_waiters_and_stops_publishing") {
  EventBus bus;
  auto sub = bus.subscribe({});
  REQUIRE(sub.ok());
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    Event ev;
    (void)bus.wait(sub.value(), &ev, -1);
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  bus.close();
  waiter.join();
  CHECK(returned.load());
  CHECK(bus.closed());

  bus.publish(make(EventType::kDeviceState));
  CHECK(bus.last_sequence() == 0);
  CHECK(bus.subscribe({}).error() == ScanError::kInvalidState);
}
