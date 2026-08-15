// PageStore: paging, pointer stability, bounds, provenance, backpressure.
#include <cstddef>
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
