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
//   page_data_mutable()/notify_recoloured() (INT-34) are the colorization
//   seam: ONE writer at a time may rewrite an existing page's colours, and it
//   must not overlap another writer on the same page. Appending to other
//   pages concurrently is fine — pages never reallocate.
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

  // --- the one producer that rewrites existing points (INT-34) ------------
  //
  // docs/A11-color.md §8.1: `Colorizer::colorize(PageStore*)` must rewrite
  // points that already exist, and until now `src/color/colorizer.cpp`
  // const_cast'd `PageView::data` to do it. That cast was *defined* (the
  // buffer is non-const inside the store and never reallocates) and *narrow*
  // (only r/g/b/a change), but it was implicit where it should be explicit.
  // This accessor is the explicit version.
  //
  // CONTRACT. The returned pointer addresses [0, count) of the page's buffer
  // and is stable for the page's lifetime, exactly as PageView::data is. A
  // caller may write the r/g/b/a bytes of any live point. It must NOT write
  // x/y/z: the page's bounding box is maintained incrementally at append()
  // time and is not recomputed here, so moving a point silently invalidates
  // the frustum-cull box every renderer and exporter reads. It must not write
  // past `count` either — use append() for new points.
  //
  // Returns nullptr if `id` is not a live page. Take `count` from
  // page_view(id) (an acquire load); this call deliberately does not return
  // one, so that the count a writer works against is the one it read.
  PointVertex* page_data_mutable(PageId id);

  // Tell subscribers that [first, first+count) of `page` had their COLOURS
  // rewritten in place — the notification half of the seam above, so a live
  // viewer re-uploads the GPU page instead of holding the pre-colorization
  // buffer until something else touches it (A14's territory; A11 §8.1 asked
  // for exactly this).
  //
  // Publishes one PageUpdate with kind == kRecoloured. Subscribers run on the
  // CALLING thread with no store lock held, the same rule append() follows.
  // `count == 0` is a no-op returning kOkStatus; an unknown page id is
  // kNotFound; a range past the page's live count is kInvalidArgument.
  Status notify_recoloured(PageId page, std::uint32_t first, std::uint32_t count);

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
