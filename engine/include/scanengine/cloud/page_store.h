// page_store.h — the engine↔renderer contract.
//
// The store owns every live point. Producers (drivers, SLAM, colorization)
// append; consumers (Filament on Android/Qt, exporters, the CLI) read pages
// by id. Nothing is copied out: consumers get a PageView pointing straight
// at the buffer the GPU upload reads from.
//
// Threading
//   append() is safe from any number of producer threads (internally
//   serialized). Readers take no lock: page_view() copies a small descriptor
//   under the store lock, and the returned `data` pointer stays valid until
//   clear()/destruction because a page never reallocates.
//   Subscriber callbacks run INLINE ON THE APPENDING THREAD — same rule as
//   EventBus callbacks: quick, no re-entry into the store.
//
// Backpressure
//   The store is bounded by max_pages. When full it appends nothing and
//   returns kCapacityExceeded (and bumps dropped_points) rather than growing
//   without limit — a 30-minute Mid-360 capture is 360 M points and must not
//   be resident. A14 replaces the cap with an LOD/eviction policy; A5's
//   .lscan recording is what makes eviction safe (raw data is already on
//   disk).
//
// Owner: A1 (this file) / A14 (LOD + display params) / A6-A7 (producers).
#ifndef SCANENGINE_CLOUD_PAGE_STORE_H
#define SCANENGINE_CLOUD_PAGE_STORE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"

namespace scanengine {

using PageSubscriptionId = std::uint32_t;
using PageCallback = void (*)(const PageUpdate& update, void* user_data);

struct PageStoreConfig {
  std::uint32_t page_capacity = kDefaultPageCapacity;  // points per page
  std::uint32_t max_pages = 64;                        // 64 M points ≈ 1 GB
};

class PageStore {
 public:
  explicit PageStore(const PageStoreConfig& cfg = {});
  ~PageStore();

  PageStore(const PageStore&) = delete;
  PageStore& operator=(const PageStore&) = delete;

  // Append points for `stream`. Splits across pages as needed, allocating
  // new ones on demand, and notifies subscribers once per touched page.
  // Returns kCapacityExceeded if the store is full (nothing partial is
  // silently dropped: `appended` reports what did land).
  Status append(StreamId stream, Span<const PointVertex> points,
                std::int64_t t_mono_ns, std::uint32_t* appended = nullptr);

  // Renderer side.
  PageView page_view(PageId id) const;
  std::vector<PageId> page_ids() const;
  std::size_t page_count() const;
  std::uint64_t total_points() const;
  std::uint64_t dropped_points() const;
  std::size_t resident_bytes() const;

  // Drop all points. Pages and their buffers are freed; every PageView taken
  // before this call becomes dangling, so consumers must stop reading first
  // (the Engine only calls this between sessions).
  void clear();

  PageSubscriptionId subscribe(PageCallback cb, void* user_data);
  Status unsubscribe(PageSubscriptionId id);

  const PageStoreConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CLOUD_PAGE_STORE_H
