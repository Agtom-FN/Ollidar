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
// Backpressure — two policies, chosen by the owner of the store
//   kReject (the default, and what every offline/post-processing store keeps):
//   the store is bounded by max_pages, and when full it appends nothing and
//   returns kCapacityExceeded (bumping dropped_points) rather than growing
//   without limit. A post-processing pass that overruns its store has a BUG,
//   and it must say so, not quietly throw the oldest half of a cloud away.
//
//   kEvictOldest (opt-in, for a LIVE capture store only): when full, the
//   OLDEST page is retired to make room, so the view is a moving window over
//   the newest data and a live preview never dead-ends. This is the fix for
//   the field bug of 2026-08-17: with kReject, a full store dropped EVERY
//   subsequent point for the rest of the run — the live view froze at the fill
//   instant and the log took one warn per revolution (1400 of them in one
//   session) — while the recording, a completely separate path, stayed perfect.
//   That separation is exactly what makes eviction safe here: A5 already has
//   the raw data on disk (Tech Spec §3 rule 2), so the only thing eviction
//   costs is the OLDEST end of a live PREVIEW.
//   A Mid-360 at ~200 k pts/s reaches a 64 × 1 M ceiling in about five and a
//   half minutes — and reached it in SECONDS before Impl::open_page_for landed
//   (see page_store.cpp: interleaved producers used to close a page at every
//   stream switch, which is what actually filled the field session's store
//   during a preview).
//
// Memory, and why an evicted page's buffer is not freed
//   An evicted page's POINTS are gone from the store immediately (page_view()
//   stops resolving its id, and one PageUpdate with kind == kEvicted is
//   published), but its BUFFER is retired into an internal pool and handed to a
//   later page instead of being freed. A reader is allowed to hold
//   PageView::data with no lock — that is the whole point of the contract
//   above — so freeing under it would be a use-after-free that no amount of
//   care on the reader's side could avoid. Recycling instead makes the worst
//   case a reader copying a few of the NEXT page's points, and even that needs
//   the reader to stall for `spare_pages` full page fills (seconds) between
//   taking a PageView and reading it.
//   The cost is exactly `spare_pages` extra resident pages: the store's
//   ceiling is (max_pages + spare_pages) × page_capacity × 16 bytes, and it
//   allocates that once and never again.
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

// What a full store does with the next point. See the header comment.
enum class PageFullPolicy : std::uint8_t {
  // Refuse: nothing is appended, kCapacityExceeded comes back, dropped_points
  // counts what did not land. The pre-2026-08-17 behaviour, and still the
  // default, so no offline consumer changes.
  kReject = 0,
  // Retire the oldest page and keep going. LIVE CAPTURE ONLY — opt in by
  // setting this on the store's config (or Engine::set_live_page_eviction()).
  kEvictOldest = 1,
};

const char* to_string(PageFullPolicy p) noexcept;

struct PageStoreConfig {
  std::uint32_t page_capacity = kDefaultPageCapacity;  // points per page
  std::uint32_t max_pages = 64;                        // 64 M points ≈ 1 GB
  PageFullPolicy when_full = PageFullPolicy::kReject;
  // How many retired page buffers stay in the recycle pool before one is
  // handed out again — the grace a lock-free reader gets between a page being
  // evicted and its memory being written by a different page. One page is a
  // whole page fill (≈ 5 s of a Mid-360 at 1 M points/page); the cost is one
  // extra resident page. Ignored when when_full == kReject.
  std::uint32_t spare_pages = 1;
};

// Everything an app needs to say "the live map is a window now", in one
// lock-free-to-ask snapshot (PageStore::stats()).
struct PageStoreStats {
  std::uint32_t pages = 0;             // pages live right now
  std::uint32_t max_pages = 0;         // the ceiling they are counted against
  std::uint64_t resident_points = 0;   // points a consumer can read right now
  std::uint64_t total_points = 0;      // points ever appended
  std::uint64_t dropped_points = 0;    // kReject only: points refused
  std::uint64_t evicted_pages = 0;     // kEvictOldest only: pages retired
  std::uint64_t evicted_points = 0;    // kEvictOldest only: points retired
  PageFullPolicy when_full = PageFullPolicy::kReject;
  // True once the store has evicted at least once in this epoch, i.e. the
  // consumer is looking at the NEWEST resident_points, not at everything.
  bool evicting = false;
};

class PageStore {
 public:
  explicit PageStore(const PageStoreConfig& cfg = {});
  ~PageStore();

  PageStore(const PageStore&) = delete;
  PageStore& operator=(const PageStore&) = delete;

  // Append points for `stream`. Splits across pages as needed, allocating
  // new ones on demand, and notifies subscribers once per touched page.
  //
  // With when_full == kReject: returns kCapacityExceeded if the store is full
  // (nothing partial is silently dropped: `appended` reports what did land).
  // With when_full == kEvictOldest: never returns kCapacityExceeded for want
  // of room — the oldest page is retired instead, one kEvicted PageUpdate is
  // published per retirement, and stats().evicting goes true.
  Status append(StreamId stream, Span<const PointVertex> points,
                std::int64_t t_mono_ns, std::uint32_t* appended = nullptr);

  // Turn eviction on (or off) after construction — the seam a C-ABI app uses,
  // since scan_engine_config predates the policy. Switching to kEvictOldest
  // does NOT evict anything by itself; the next full append does.
  // kInvalidArgument for an unknown policy value; otherwise always ok.
  Status set_full_policy(PageFullPolicy policy);

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
  PageStoreStats stats() const;

  // Drop all points. Pages and their buffers are freed; every PageView taken
  // before this call becomes dangling, so consumers must stop reading first
  // (the Engine only calls this between sessions).
  void clear();

  // Drop all points WITHOUT freeing anything — every live page is retired
  // through the same path an eviction takes (one kEvicted PageUpdate each, ids
  // never reused, buffers into the recycle pool). This is the safe version of
  // clear() for a store a renderer is reading right now, and it is what a live
  // capture wants between sessions: LIO restarts at the origin on every
  // start_session(), so points from the previous session are in a STALE FRAME
  // and would sit in the live map misaligned with everything after them.
  //
  // Counters: resident/evicting reset (a new epoch), the lifetime totals
  // (total_points, evicted_*, dropped_points) do not.
  void recycle_all();

  PageSubscriptionId subscribe(PageCallback cb, void* user_data);
  Status unsubscribe(PageSubscriptionId id);

  const PageStoreConfig& config() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scanengine

#endif  // SCANENGINE_CLOUD_PAGE_STORE_H
