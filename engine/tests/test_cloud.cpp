// PageStore: paging, pointer stability, bounds, provenance, backpressure.
#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"

using namespace scanengine;

namespace {

std::vector<PointVertex> ramp(std::size_t n, float base = 0.f) {
  std::vector<PointVertex> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i].x = base + static_cast<float>(i);
    v[i].y = -static_cast<float>(i);
    v[i].z = 1.f;
    v[i].r = v[i].g = v[i].b = 128;
    v[i].a = 255;
  }
  return v;
}

Span<const PointVertex> span_of(const std::vector<PointVertex>& v) {
  return Span<const PointVertex>(v.data(), v.size());
}

}  // namespace

TEST_CASE("cloud/point_vertex_is_the_S3_layout") {
  CHECK(sizeof(PointVertex) == 16);
  CHECK(offsetof(PointVertex, x) == 0);
  CHECK(offsetof(PointVertex, r) == 12);
  CHECK(kDefaultPageCapacity == (1u << 20));
}

TEST_CASE("cloud/append_fills_pages_and_splits_across_them") {
  PageStoreConfig cfg;
  cfg.page_capacity = 100;
  cfg.max_pages = 4;
  PageStore store(cfg);

  const auto pts = ramp(250);
  std::uint32_t appended = 0;
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 1000, &appended).ok());
  CHECK(appended == 250);
  CHECK(store.page_count() == 3);
  CHECK(store.total_points() == 250);

  const auto ids = store.page_ids();
  REQUIRE(ids.size() == 3);
  CHECK(store.page_view(ids[0]).count == 100);
  CHECK(store.page_view(ids[2]).count == 50);
  CHECK(store.page_view(ids[0]).capacity == 100);
  CHECK(store.page_view(ids[0]).stream == StreamId::kLidarD6);
}

TEST_CASE("cloud/page_data_pointer_is_stable_across_appends") {
  PageStoreConfig cfg;
  cfg.page_capacity = 1000;
  PageStore store(cfg);

  const auto a = ramp(10);
  CHECK(store.append(StreamId::kLidarD6, span_of(a), 1).ok());
  const PageId id = store.page_ids().front();
  const PointVertex* first_ptr = store.page_view(id).data;

  for (int i = 0; i < 50; ++i) {
    const auto more = ramp(10, static_cast<float>(i));
    CHECK(store.append(StreamId::kLidarD6, span_of(more), 2).ok());
  }
  const PageView v = store.page_view(id);
  CHECK(v.data == first_ptr);  // never reallocated — the renderer keeps it
  CHECK(v.count == 510);
  CHECK(v.data[0].x == 0.f);
}

TEST_CASE("cloud/pages_are_single_stream_for_provenance") {
  PageStoreConfig cfg;
  cfg.page_capacity = 1000;
  PageStore store(cfg);
  const auto a = ramp(5);
  CHECK(store.append(StreamId::kLidarD6, span_of(a), 1).ok());
  CHECK(store.append(StreamId::kLidarMid360, span_of(a), 2).ok());
  CHECK(store.page_count() == 2);
  const auto ids = store.page_ids();
  CHECK(store.page_view(ids[0]).stream == StreamId::kLidarD6);
  CHECK(store.page_view(ids[1]).stream == StreamId::kLidarMid360);
}

TEST_CASE("cloud/bounds_track_the_written_points") {
  PageStore store(PageStoreConfig{100, 4});
  std::vector<PointVertex> v(3);
  v[0] = {1.f, 2.f, 3.f, 0, 0, 0, 255};
  v[1] = {-4.f, 5.f, 0.f, 0, 0, 0, 255};
  v[2] = {0.f, -6.f, 9.f, 0, 0, 0, 255};
  CHECK(store.append(StreamId::kLidarD6, span_of(v), 1).ok());
  const PageView pv = store.page_view(store.page_ids().front());
  CHECK(pv.bounds_min[0] == -4.f);
  CHECK(pv.bounds_min[1] == -6.f);
  CHECK(pv.bounds_min[2] == 0.f);
  CHECK(pv.bounds_max[0] == 1.f);
  CHECK(pv.bounds_max[1] == 5.f);
  CHECK(pv.bounds_max[2] == 9.f);
}

TEST_CASE("cloud/backpressure_is_reported_not_silent") {
  PageStoreConfig cfg;
  cfg.page_capacity = 10;
  cfg.max_pages = 2;
  PageStore store(cfg);

  const auto pts = ramp(50);
  std::uint32_t appended = 0;
  const Status s = store.append(StreamId::kLidarD6, span_of(pts), 1, &appended);
  CHECK(s.error() == ScanError::kCapacityExceeded);
  CHECK(appended == 20);
  CHECK(store.dropped_points() == 30);
  CHECK(store.total_points() == 20);
  CHECK(store.resident_bytes() == 2 * 10 * sizeof(PointVertex));
}

TEST_CASE("cloud/subscribers_see_every_range_exactly_once") {
  PageStoreConfig cfg;
  cfg.page_capacity = 4;
  cfg.max_pages = 8;
  PageStore store(cfg);

  std::vector<PageUpdate> seen;
  const auto id = store.subscribe(
      [](const PageUpdate& u, void* user) {
        static_cast<std::vector<PageUpdate>*>(user)->push_back(u);
      },
      &seen);
  CHECK(id != 0);

  const auto pts = ramp(10);
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  REQUIRE(seen.size() == 3);
  CHECK(seen[0].page_created);
  CHECK(seen[0].first == 0);
  CHECK(seen[0].count == 4);
  CHECK(seen[1].count == 4);
  CHECK(seen[2].count == 2);
  std::uint32_t total = 0;
  for (const auto& u : seen) total += u.count;
  CHECK(total == 10);

  CHECK(store.unsubscribe(id).ok());
  CHECK(store.unsubscribe(id).error() == ScanError::kNotFound);
}

// --- INT-34: the colorization seam (docs/A11-color.md §8.1) ---------------

TEST_CASE("cloud/page_data_mutable_lets_the_one_rewriting_producer_rewrite") {
  PageStoreConfig cfg;
  cfg.page_capacity = 4;
  cfg.max_pages = 8;
  PageStore store(cfg);

  const auto pts = ramp(10);
  REQUIRE(store.append(StreamId::kSlamMap, span_of(pts), 1).ok());
  const auto ids = store.page_ids();
  REQUIRE(ids.size() == 3);

  // The pointer is the SAME buffer the const view addresses — that identity
  // is what makes the accessor a replacement for the const_cast that used to
  // live in src/color/colorizer.cpp, rather than a copy.
  for (const PageId id : ids) {
    CHECK(store.page_data_mutable(id) == store.page_view(id).data);
  }
  CHECK(store.page_data_mutable(9999) == nullptr);

  // Rewrite only the colours, exactly as the colorizer does.
  for (const PageId id : ids) {
    const PageView v = store.page_view(id);
    PointVertex* w = store.page_data_mutable(id);
    REQUIRE(w != nullptr);
    for (std::uint32_t i = 0; i < v.count; ++i) {
      w[i].r = 10;
      w[i].g = 20;
      w[i].b = 30;
    }
  }

  // Positions, counts, bounds and page ids all survive: that is the property
  // that makes writing through this pointer safe without recomputing the
  // incrementally-maintained bounding box.
  std::uint32_t seen = 0;
  for (const PageId id : ids) {
    const PageView v = store.page_view(id);
    for (std::uint32_t i = 0; i < v.count; ++i) {
      CHECK(v.data[i].x == pts[seen].x);
      CHECK(v.data[i].y == pts[seen].y);
      CHECK(v.data[i].r == 10);
      CHECK(v.data[i].b == 30);
      ++seen;
    }
  }
  CHECK(seen == 10);
  CHECK(store.total_points() == 10);
  CHECK(store.page_view(ids[0]).bounds_max[0] == 3.f);  // untouched by the rewrite
}

TEST_CASE("cloud/a_recolour_notifies_with_a_kind_an_append_never_carries") {
  PageStoreConfig cfg;
  cfg.page_capacity = 8;
  cfg.max_pages = 4;
  PageStore store(cfg);

  std::vector<PageUpdate> seen;
  const auto sub = store.subscribe(
      [](const PageUpdate& u, void* user) {
        static_cast<std::vector<PageUpdate>*>(user)->push_back(u);
      },
      &seen);
  REQUIRE(sub != 0);

  const auto pts = ramp(6);
  REQUIRE(store.append(StreamId::kSlamMap, span_of(pts), 1).ok());
  REQUIRE(seen.size() == 1);
  // Every pre-INT-34 producer is unchanged in meaning: an append is kAppended
  // without anyone having said so.
  CHECK(seen[0].kind == PageUpdateKind::kAppended);
  const PageId id = store.page_ids().front();

  seen.clear();
  CHECK(store.notify_recoloured(id, 0, 6).ok());
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].kind == PageUpdateKind::kRecoloured);
  CHECK(seen[0].page == id);
  CHECK(seen[0].first == 0);
  CHECK(seen[0].count == 6);
  CHECK(seen[0].stream == StreamId::kSlamMap);
  CHECK_FALSE(seen[0].page_created);  // a recolour never allocates a GPU buffer
  CHECK(std::string(to_string(PageUpdateKind::kRecoloured)) == "recoloured");

  // A partial range is legal — a caller that colours a sub-range says so.
  seen.clear();
  CHECK(store.notify_recoloured(id, 2, 3).ok());
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].first == 2);
  CHECK(seen[0].count == 3);

  // Refusals: an unknown page, and a range past the live count. The second is
  // the one that matters — publishing it would tell a renderer to upload
  // memory that has never been written.
  seen.clear();
  CHECK(store.notify_recoloured(9999, 0, 1).error() == ScanError::kNotFound);
  CHECK(store.notify_recoloured(id, 0, 7).error() == ScanError::kInvalidArgument);
  CHECK(store.notify_recoloured(id, 6, 1).error() == ScanError::kInvalidArgument);
  CHECK(store.notify_recoloured(id, 0, 0).ok());  // a no-op, not an error
  CHECK(seen.empty());

  CHECK(store.unsubscribe(sub).ok());
}

TEST_CASE("cloud/clear_never_reuses_page_ids") {
  PageStore store(PageStoreConfig{10, 4});
  const auto pts = ramp(5);
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());
  const PageId first = store.page_ids().front();
  store.clear();
  CHECK(store.page_count() == 0);
  CHECK(store.total_points() == 0);
  CHECK_FALSE(store.page_view(first).valid());

  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 2).ok());
  CHECK(store.page_ids().front() != first);
}

// --- the live point window (2026-08-17 field bug) --------------------------
//
// The owner's report was "live view not moving even i move the lidar", and the
// engine log said why 1400 times in one session: `page store full (64 pages):
// dropped 8195 points`. A bounded store that REFUSES for the rest of the run
// freezes the display at the instant it filled. These cases pin both halves of
// the fix: a live store recycles, and an offline store still hard-caps.

namespace {

// Where the newest resident point sits. ramp() writes x = base + i, so this is
// "is the live view still advancing?" as a number.
float newest_x(const PageStore& store) {
  float best = -1.f;
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid() || v.count == 0) continue;
    best = std::max(best, v.bounds_max[0]);
  }
  return best;
}

}  // namespace

TEST_CASE("cloud/a_live_store_recycles_instead_of_dead_ending") {
  PageStoreConfig cfg;
  cfg.page_capacity = 10;
  cfg.max_pages = 4;
  cfg.when_full = PageFullPolicy::kEvictOldest;
  PageStore store(cfg);

  // 200 batches of 25 points = 5000 points through a 40-point window: the
  // store passes its ceiling on batch 2 and then runs for another 198.
  float last_newest = -1.f;
  std::set<const PointVertex*> buffers;
  for (int batch = 0; batch < 200; ++batch) {
    const auto pts = ramp(25, static_cast<float>(batch) * 25.f);
    std::uint32_t appended = 0;
    const Status s = store.append(StreamId::kLidarMid360, span_of(pts), batch + 1, &appended);
    // Never dead-ends: every point of every batch lands, forever.
    CHECK(s.ok());
    CHECK(appended == 25);
    // The window never grows past its ceiling...
    CHECK(store.page_count() <= cfg.max_pages);
    // ...and what it holds keeps moving forward, which is the actual bug.
    const float now = newest_x(store);
    CHECK(now > last_newest);
    last_newest = now;
    for (const PageId id : store.page_ids()) buffers.insert(store.page_view(id).data);
  }

  const PageStoreStats st = store.stats();
  CHECK(st.when_full == PageFullPolicy::kEvictOldest);
  CHECK(st.evicting);
  CHECK(st.pages == cfg.max_pages);
  CHECK(st.total_points == 5000);
  CHECK(st.dropped_points == 0);          // nothing was refused
  CHECK(st.evicted_points == 5000 - st.resident_points);
  CHECK(st.evicted_pages == 500 - cfg.max_pages);
  CHECK(st.resident_points == cfg.max_pages * cfg.page_capacity);

  // Bounded memory, and bounded ALLOCATION: the buffers handed out over the
  // whole soak are the same max_pages + spare_pages buffers, recycled.
  CHECK(buffers.size() <= cfg.max_pages + cfg.spare_pages);
  CHECK(store.resident_bytes() <=
        (cfg.max_pages + cfg.spare_pages) * cfg.page_capacity * sizeof(PointVertex));

  // The newest point in the store is the last one appended.
  CHECK(newest_x(store) == doctest::Approx(199.f * 25.f + 24.f));
}

TEST_CASE("cloud/eviction_is_opt_in_an_offline_store_still_hard_caps") {
  PageStoreConfig cfg;
  cfg.page_capacity = 10;
  cfg.max_pages = 2;
  PageStore store(cfg);  // default policy — a post-processing store
  CHECK(cfg.when_full == PageFullPolicy::kReject);

  const auto pts = ramp(50);
  std::uint32_t appended = 0;
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 1, &appended).error() ==
        ScanError::kCapacityExceeded);
  CHECK(appended == 20);

  const PageStoreStats st = store.stats();
  CHECK_FALSE(st.evicting);
  CHECK(st.evicted_pages == 0);
  CHECK(st.evicted_points == 0);
  CHECK(st.dropped_points == 30);
  CHECK(st.resident_points == 20);

  // And it can be switched at run time — the seam a C-ABI app opts in through.
  CHECK(store.set_full_policy(PageFullPolicy::kEvictOldest).ok());
  std::uint32_t appended2 = 0;
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 2, &appended2).ok());
  CHECK(appended2 == 50);
  CHECK(store.stats().evicting);
  CHECK(store.stats().dropped_points == 30);  // the old refusal is still counted
}

TEST_CASE("cloud/eviction_notifies_once_per_page_and_never_reuses_an_id") {
  PageStoreConfig cfg;
  cfg.page_capacity = 4;
  cfg.max_pages = 2;
  cfg.when_full = PageFullPolicy::kEvictOldest;
  PageStore store(cfg);

  std::vector<PageUpdate> seen;
  const auto sub = store.subscribe(
      [](const PageUpdate& u, void* user) {
        static_cast<std::vector<PageUpdate>*>(user)->push_back(u);
      },
      &seen);

  const auto pts = ramp(12);  // 3 pages through a 2-page window
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());

  int evictions = 0;
  PageId evicted_id = kInvalidPageId;
  for (const auto& u : seen) {
    if (u.kind != PageUpdateKind::kEvicted) continue;
    ++evictions;
    evicted_id = u.page;
    CHECK(u.first == 0);
    CHECK(u.count == 4);           // the whole page left the window
    CHECK_FALSE(u.page_created);
  }
  CHECK(evictions == 1);
  // The evicted page is gone for good: not enumerable, not viewable, not
  // writable, and its id is never handed out again.
  CHECK_FALSE(store.page_view(evicted_id).valid());
  CHECK(store.page_data_mutable(evicted_id) == nullptr);
  CHECK(store.notify_recoloured(evicted_id, 0, 1).error() == ScanError::kNotFound);
  for (const PageId id : store.page_ids()) CHECK(id != evicted_id);

  CHECK(std::string(to_string(PageUpdateKind::kEvicted)) == "evicted");
  CHECK(std::string(to_string(PageFullPolicy::kEvictOldest)) == "evict-oldest");
  CHECK(std::string(to_string(PageFullPolicy::kReject)) == "reject");
  CHECK(store.unsubscribe(sub).ok());
}

TEST_CASE("cloud/recycle_all_empties_the_window_without_freeing_a_buffer") {
  PageStoreConfig cfg;
  cfg.page_capacity = 4;
  cfg.max_pages = 4;
  cfg.when_full = PageFullPolicy::kEvictOldest;
  PageStore store(cfg);

  const auto pts = ramp(12);
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 1).ok());
  const std::vector<PageId> before = store.page_ids();
  REQUIRE(before.size() == 3);
  std::set<const PointVertex*> buffers_before;
  for (const PageId id : before) buffers_before.insert(store.page_view(id).data);
  const std::size_t bytes_before = store.resident_bytes();

  std::vector<PageUpdate> seen;
  const auto sub = store.subscribe(
      [](const PageUpdate& u, void* user) {
        static_cast<std::vector<PageUpdate>*>(user)->push_back(u);
      },
      &seen);
  store.recycle_all();

  // One kEvicted per page, oldest first, and the window is empty.
  REQUIRE(seen.size() == 3);
  for (std::size_t i = 0; i < seen.size(); ++i) {
    CHECK(seen[i].kind == PageUpdateKind::kEvicted);
    CHECK(seen[i].page == before[i]);
  }
  CHECK(store.page_count() == 0);
  CHECK(store.stats().resident_points == 0);
  CHECK_FALSE(store.stats().evicting);
  // Lifetime totals survive: a support log adds these up.
  CHECK(store.stats().total_points == 12);
  CHECK(store.stats().evicted_points == 12);
  // Nothing was freed — the same buffers come back, under NEW ids.
  CHECK(store.resident_bytes() == bytes_before);
  CHECK(store.append(StreamId::kLidarD6, span_of(pts), 2).ok());
  std::size_t reused = 0;
  for (const PageId id : store.page_ids()) {
    for (const PageId old : before) CHECK(id != old);
    if (buffers_before.count(store.page_view(id).data) == 1) ++reused;
  }
  // All but `spare_pages` of them come straight back out of the pool; the
  // spare is held back deliberately, as the grace a lock-free reader gets.
  CHECK(reused == before.size() - cfg.spare_pages);
  CHECK(store.unsubscribe(sub).ok());
}

TEST_CASE("cloud/interleaved_streams_do_not_close_each_others_pages") {
  // A live capture has TWO producers into one store: the Mid-360's raw cloud
  // and live SLAM's registered map. They interleave about ten times a second,
  // and while "the tail page" was the only page anyone could append to, every
  // switch closed a page — 4430 points into a 1 048 576-point page on the S2
  // simulator, i.e. 0.4% of the store's nominal capacity. That is what made a
  // 64-page store fill during a PREVIEW in the field.
  PageStoreConfig cfg;
  cfg.page_capacity = 100;
  cfg.max_pages = 16;
  PageStore store(cfg);

  const auto pts = ramp(10);
  for (int i = 0; i < 5; ++i) {
    CHECK(store.append(StreamId::kLidarMid360, span_of(pts), 2 * i + 1).ok());
    CHECK(store.append(StreamId::kSlamMap, span_of(pts), 2 * i + 2).ok());
  }
  // One page per stream, both half full — not ten pages of ten points.
  CHECK(store.page_count() == 2);
  const auto ids = store.page_ids();
  REQUIRE(ids.size() == 2);
  CHECK(store.page_view(ids[0]).stream == StreamId::kLidarMid360);
  CHECK(store.page_view(ids[0]).count == 50);
  CHECK(store.page_view(ids[1]).stream == StreamId::kSlamMap);
  CHECK(store.page_view(ids[1]).count == 50);
  CHECK(store.total_points() == 100);

  // And each stream still gets its own new page when its own fills: five more
  // batches fill the Mid-360 page exactly, the sixth opens a new one.
  for (int i = 0; i < 5; ++i) {
    CHECK(store.append(StreamId::kLidarMid360, span_of(pts), 100 + i).ok());
  }
  CHECK(store.page_count() == 2);
  CHECK(store.page_view(store.page_ids()[0]).count == 100);
  CHECK(store.append(StreamId::kLidarMid360, span_of(pts), 200).ok());
  CHECK(store.page_count() == 3);
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    // Provenance is intact: no page ever mixes two streams.
    CHECK((v.stream == StreamId::kLidarMid360 || v.stream == StreamId::kSlamMap));
  }
}
