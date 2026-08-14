// PageStore: paging, pointer stability, bounds, provenance, backpressure.
#include <cstddef>
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
