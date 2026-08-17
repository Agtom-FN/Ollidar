#include "scanengine/cloud/page_store.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "scanengine/core/log.h"

namespace scanengine {
namespace {
constexpr const char* kMod = "cloud";
}

struct Page {
  PageId id = kInvalidPageId;
  StreamId stream = StreamId::kUnknown;
  std::unique_ptr<PointVertex[]> data;  // allocated once, never reallocated
  std::uint32_t capacity = 0;
  std::atomic<std::uint32_t> count{0};
  std::int64_t t_first_ns = 0;
  std::int64_t t_last_ns = 0;
  float bmin[3] = {0.f, 0.f, 0.f};
  float bmax[3] = {0.f, 0.f, 0.f};
};

struct PageStore::Impl {
  PageStoreConfig cfg;
  mutable std::mutex m;
  std::vector<std::unique_ptr<Page>> pages;  // oldest first: pages[0] is what eviction takes
  // Retired page buffers, oldest retirement first. NEVER freed while the store
  // lives (page_store.h "Memory, and why an evicted page's buffer is not
  // freed"): a reader may hold PageView::data with no lock.
  std::vector<std::unique_ptr<Page>> pool;
  PageId next_page_id = 1;
  std::uint64_t total_points = 0;
  std::uint64_t dropped_points = 0;
  std::uint64_t resident_points = 0;
  std::uint64_t evicted_pages = 0;
  std::uint64_t evicted_points = 0;
  bool evicting = false;       // has evicted at least once in this epoch
  bool logged_evicting = false;  // ONE info line per epoch, never a warn per revolution

  struct Sub {
    PageSubscriptionId id;
    PageCallback cb;
    void* user;
  };
  std::vector<Sub> subs;
  PageSubscriptionId next_sub_id = 1;

  Page* find(PageId id) const {
    for (const auto& p : pages) {
      if (p->id == id) return p.get();
    }
    return nullptr;
  }

  // The page `stream` is currently filling, or nullptr if it needs a new one.
  //
  // This used to be "the LAST page, if its stream matches" — which meant that
  // two INTERLEAVED producers (the Mid-360's raw cloud and live SLAM's
  // registered map, ~10 stream switches a second in a live capture) closed a
  // page at every switch. Measured on the S2 simulator: 4430 points per page
  // against a 1 048 576-point capacity — the store's real capacity was 0.4% of
  // its nominal one, which is why the field session's 64-page store filled
  // during a PREVIEW. Keeping one open page PER STREAM preserves the
  // single-stream provenance rule exactly (a page still carries one stream, for
  // A13 merge + A9 export) and gives back the other 99.6%.
  //
  // Only the newest page of a stream can have room: appends always fill it
  // before a new one is created, so a linear scan from the back finds it and
  // stops at the first match.
  Page* open_page_for(StreamId stream) const {
    for (auto it = pages.rbegin(); it != pages.rend(); ++it) {
      Page* p = it->get();
      if (p->stream != stream) continue;
      return p->count.load(std::memory_order_relaxed) < p->capacity ? p : nullptr;
    }
    return nullptr;
  }

  // Retire pages[0]. Called with `m` held; appends the kEvicted notification to
  // `updates` so it is published in order with the appends around it, outside
  // the lock.
  void retire_oldest(std::vector<PageUpdate>* updates) {
    if (pages.empty()) return;
    std::unique_ptr<Page> p = std::move(pages.front());
    pages.erase(pages.begin());

    const std::uint32_t n = p->count.load(std::memory_order_relaxed);
    PageUpdate u;
    u.page = p->id;
    u.stream = p->stream;
    u.first = 0;
    u.count = n;
    u.page_created = false;
    u.kind = PageUpdateKind::kEvicted;
    if (updates != nullptr) updates->push_back(u);

    ++evicted_pages;
    evicted_points += n;
    resident_points -= std::min<std::uint64_t>(resident_points, n);
    evicting = true;

    // The id is retired with the page. acquire() hands out a NEW one, so a
    // stale PageView or a queued event can never resolve to a different page.
    p->id = kInvalidPageId;
    p->stream = StreamId::kUnknown;
    p->count.store(0, std::memory_order_relaxed);
    pool.push_back(std::move(p));
  }

  // A fresh page at the tail, from the recycle pool when it has more than the
  // configured grace spares, otherwise newly allocated. Called with `m` held.
  // Returns nullptr only if the allocation failed.
  Page* acquire(StreamId stream, std::int64_t t_mono_ns) {
    std::unique_ptr<Page> p;
    if (pool.size() > cfg.spare_pages) {
      p = std::move(pool.front());
      pool.erase(pool.begin());
    }
    if (p == nullptr) {
      p = std::make_unique<Page>();
      p->capacity = cfg.page_capacity;
      p->data.reset(new (std::nothrow) PointVertex[p->capacity]);
      if (p->data == nullptr) return nullptr;
    }
    p->id = next_page_id++;
    p->stream = stream;
    p->count.store(0, std::memory_order_relaxed);
    p->t_first_ns = t_mono_ns;
    p->t_last_ns = t_mono_ns;
    for (int k = 0; k < 3; ++k) p->bmin[k] = p->bmax[k] = 0.f;
    pages.push_back(std::move(p));
    return pages.back().get();
  }
};

PageStore::PageStore(const PageStoreConfig& cfg) : impl_(new Impl) {
  impl_->cfg = cfg;
  if (impl_->cfg.page_capacity == 0) impl_->cfg.page_capacity = kDefaultPageCapacity;
  if (impl_->cfg.max_pages == 0) impl_->cfg.max_pages = 1;
  // The recycle grace cannot exceed the store itself: it is one extra page's
  // worth of memory per spare, and a store that keeps more spares than pages
  // is paying for a grace it can never use.
  if (impl_->cfg.spare_pages > impl_->cfg.max_pages) impl_->cfg.spare_pages = impl_->cfg.max_pages;
}

PageStore::~PageStore() = default;

const PageStoreConfig& PageStore::config() const { return impl_->cfg; }

Status PageStore::append(StreamId stream, Span<const PointVertex> points,
                         std::int64_t t_mono_ns, std::uint32_t* appended) {
  if (appended != nullptr) *appended = 0;
  if (points.empty()) return kOkStatus;
  if (points.data() == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "page store: null point buffer");
  }

  std::size_t written = 0;
  std::vector<PageUpdate> updates;
  bool full = false;
  bool oom = false;
  bool log_evicting = false;
  std::uint32_t log_pages = 0;
  std::uint64_t log_points = 0;

  {
    std::lock_guard<std::mutex> lock(impl_->m);
    while (written < points.size()) {
      // Pages are single-stream (provenance per page for A13 merge + A9
      // export), so each stream fills its OWN open page.
      Page* tail = impl_->open_page_for(stream);
      const bool need_page = tail == nullptr;
      bool created = false;
      if (need_page) {
        if (impl_->pages.size() >= impl_->cfg.max_pages) {
          if (impl_->cfg.when_full != PageFullPolicy::kEvictOldest) {
            full = true;
            break;
          }
          // LIVE capture: make room instead of dead-ending. The oldest page
          // leaves the window; the recording on disk still has every point.
          impl_->retire_oldest(&updates);
          if (!impl_->logged_evicting) {
            impl_->logged_evicting = true;
            log_evicting = true;
            log_pages = impl_->cfg.max_pages;
            log_points = static_cast<std::uint64_t>(impl_->cfg.max_pages) *
                         impl_->cfg.page_capacity;
          }
        }
        tail = impl_->acquire(stream, t_mono_ns);
        if (tail == nullptr) {  // allocation failed: refuse, do not half-write
          oom = true;
          break;
        }
        created = true;
      }

      const std::uint32_t live = tail->count.load(std::memory_order_relaxed);
      const std::uint32_t room = tail->capacity - live;
      const std::uint32_t n =
          static_cast<std::uint32_t>(std::min<std::size_t>(room, points.size() - written));

      std::memcpy(tail->data.get() + live, points.data() + written,
                  static_cast<std::size_t>(n) * sizeof(PointVertex));

      // Bounds, maintained incrementally so exporters/renderers get a cheap
      // frustum-cull box per page.
      for (std::uint32_t i = 0; i < n; ++i) {
        const PointVertex& v = tail->data[live + i];
        const float xyz[3] = {v.x, v.y, v.z};
        if (live == 0 && i == 0) {
          for (int k = 0; k < 3; ++k) tail->bmin[k] = tail->bmax[k] = xyz[k];
        } else {
          for (int k = 0; k < 3; ++k) {
            tail->bmin[k] = std::min(tail->bmin[k], xyz[k]);
            tail->bmax[k] = std::max(tail->bmax[k], xyz[k]);
          }
        }
      }

      // Release-store the new count: everything written above is visible to
      // a reader that observes this count (or the kPointsAvailable event).
      tail->count.store(live + n, std::memory_order_release);
      tail->t_last_ns = t_mono_ns;
      impl_->total_points += n;
      impl_->resident_points += n;

      PageUpdate u;
      u.page = tail->id;
      u.stream = stream;
      u.first = live;
      u.count = n;
      u.page_created = created;
      updates.push_back(u);

      written += n;
    }

    if (full || oom) impl_->dropped_points += points.size() - written;
  }

  // ONE line, the first time a live store starts recycling — not the warn per
  // revolution that filled the field log (1400 of them in one session) and
  // told the operator nothing they could act on.
  if (log_evicting) {
    SCAN_LOG_INFO(kMod,
                  "live page store reached its %u-page ceiling (%llu points, %.1f MB): the "
                  "OLDEST page is now recycled for each new one, so the live view keeps "
                  "advancing over the newest data and memory stays bounded. RECORDING IS "
                  "UNAFFECTED — the .lscan on disk still has every point.",
                  log_pages, static_cast<unsigned long long>(log_points),
                  static_cast<double>(log_points) * sizeof(PointVertex) / (1024.0 * 1024.0));
  }

  // Subscriber callbacks run outside the store lock so a renderer that
  // enqueues a GPU upload cannot stall other producers.
  {
    std::vector<Impl::Sub> subs_copy;
    {
      std::lock_guard<std::mutex> lock(impl_->m);
      subs_copy = impl_->subs;
    }
    for (const auto& u : updates) {
      for (const auto& s : subs_copy) s.cb(u, s.user);
    }
  }

  if (appended != nullptr) *appended = static_cast<std::uint32_t>(written);
  if (oom) {
    SCAN_LOG_ERROR(kMod, "page store: out of memory allocating a %u-point page; dropped %zu points",
                   impl_->cfg.page_capacity, points.size() - written);
    return set_last_error(ScanError::kOutOfMemory,
                          "page store: could not allocate a page; dropped %zu points",
                          points.size() - written);
  }
  if (full) {
    SCAN_LOG_WARN(kMod, "page store full (%u pages): dropped %zu points",
                  impl_->cfg.max_pages, points.size() - written);
    return set_last_error(ScanError::kCapacityExceeded,
                          "page store full at %u pages; dropped %zu points",
                          impl_->cfg.max_pages, points.size() - written);
  }
  return kOkStatus;
}

const char* to_string(PageUpdateKind k) noexcept {
  switch (k) {
    case PageUpdateKind::kAppended: return "appended";
    case PageUpdateKind::kRecoloured: return "recoloured";
    case PageUpdateKind::kEvicted: return "evicted";
  }
  return "unknown";
}

const char* to_string(PageFullPolicy p) noexcept {
  switch (p) {
    case PageFullPolicy::kReject: return "reject";
    case PageFullPolicy::kEvictOldest: return "evict-oldest";
  }
  return "unknown";
}

Status PageStore::set_full_policy(PageFullPolicy policy) {
  if (policy != PageFullPolicy::kReject && policy != PageFullPolicy::kEvictOldest) {
    return set_last_error(ScanError::kInvalidArgument, "page store: unknown full policy %d",
                          static_cast<int>(policy));
  }
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    changed = impl_->cfg.when_full != policy;
    impl_->cfg.when_full = policy;
  }
  if (changed) {
    SCAN_LOG_INFO(kMod, "page store full-policy -> %s", to_string(policy));
  }
  return kOkStatus;
}

PointVertex* PageStore::page_data_mutable(PageId id) {
  std::lock_guard<std::mutex> lock(impl_->m);
  Page* p = impl_->find(id);
  return p == nullptr ? nullptr : p->data.get();
}

Status PageStore::notify_recoloured(PageId page, std::uint32_t first, std::uint32_t count) {
  if (count == 0) return kOkStatus;

  PageUpdate u;
  std::vector<Impl::Sub> subs_copy;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    const Page* p = impl_->find(page);
    if (p == nullptr) {
      return set_last_error(ScanError::kNotFound, "page store: no page %u to recolour", page);
    }
    const std::uint32_t live = p->count.load(std::memory_order_acquire);
    if (first > live || count > live - first) {
      return set_last_error(ScanError::kInvalidArgument,
                            "page store: recolour range [%u, %u) is past page %u's %u points",
                            first, first + count, page, live);
    }
    u.page = p->id;
    u.stream = p->stream;
    u.first = first;
    u.count = count;
    u.page_created = false;
    u.kind = PageUpdateKind::kRecoloured;
    subs_copy = impl_->subs;
  }
  // Outside the lock — same rule append() follows, so a renderer that queues
  // a GPU upload from the callback cannot stall a producer.
  for (const auto& s : subs_copy) s.cb(u, s.user);
  return kOkStatus;
}

PageView PageStore::page_view(PageId id) const {
  std::lock_guard<std::mutex> lock(impl_->m);
  const Page* p = impl_->find(id);
  if (p == nullptr) return PageView{};
  PageView v;
  v.id = p->id;
  v.stream = p->stream;
  v.data = p->data.get();
  v.count = p->count.load(std::memory_order_acquire);
  v.capacity = p->capacity;
  v.t_first_ns = p->t_first_ns;
  v.t_last_ns = p->t_last_ns;
  for (int k = 0; k < 3; ++k) {
    v.bounds_min[k] = p->bmin[k];
    v.bounds_max[k] = p->bmax[k];
  }
  return v;
}

std::vector<PageId> PageStore::page_ids() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  std::vector<PageId> ids;
  ids.reserve(impl_->pages.size());
  for (const auto& p : impl_->pages) ids.push_back(p->id);
  return ids;
}

std::size_t PageStore::page_count() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->pages.size();
}

std::uint64_t PageStore::total_points() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->total_points;
}

std::uint64_t PageStore::dropped_points() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  return impl_->dropped_points;
}

std::size_t PageStore::resident_bytes() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  // Live pages AND retired-but-not-freed buffers: this is what the process is
  // actually holding, which is the only useful answer for a memory readout.
  return (impl_->pages.size() + impl_->pool.size()) *
         static_cast<std::size_t>(impl_->cfg.page_capacity) * sizeof(PointVertex);
}

PageStoreStats PageStore::stats() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  PageStoreStats s;
  s.pages = static_cast<std::uint32_t>(impl_->pages.size());
  s.max_pages = impl_->cfg.max_pages;
  s.resident_points = impl_->resident_points;
  s.total_points = impl_->total_points;
  s.dropped_points = impl_->dropped_points;
  s.evicted_pages = impl_->evicted_pages;
  s.evicted_points = impl_->evicted_points;
  s.when_full = impl_->cfg.when_full;
  s.evicting = impl_->evicting;
  return s;
}

void PageStore::recycle_all() {
  std::vector<PageUpdate> updates;
  std::vector<Impl::Sub> subs_copy;
  std::uint64_t retired_points = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    retired_points = impl_->resident_points;
    while (!impl_->pages.empty()) impl_->retire_oldest(&updates);
    // A new epoch: the window is empty again, so "you are looking at the
    // newest N points" is not true until it fills and evicts once more. The
    // LIFETIME totals (evicted_*, total_points, dropped_points) are not reset —
    // they are what a support log adds up.
    impl_->evicting = false;
    impl_->logged_evicting = false;
    impl_->resident_points = 0;
    subs_copy = impl_->subs;
  }
  if (updates.empty()) return;
  SCAN_LOG_INFO(kMod, "live page store reset: %zu page(s) / %llu points retired (buffers kept)",
                updates.size(), static_cast<unsigned long long>(retired_points));
  // Outside the lock, same rule append() follows.
  for (const auto& u : updates) {
    for (const auto& s : subs_copy) s.cb(u, s.user);
  }
}

void PageStore::clear() {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->pages.clear();
  impl_->pool.clear();
  impl_->total_points = 0;
  impl_->dropped_points = 0;
  impl_->resident_points = 0;
  impl_->evicted_pages = 0;
  impl_->evicted_points = 0;
  impl_->evicting = false;
  impl_->logged_evicting = false;
  // Page ids are NOT reused: a stale PageView or a queued kPointsAvailable
  // event referring to an old id must resolve to "not found", never to a
  // different page.
}

PageSubscriptionId PageStore::subscribe(PageCallback cb, void* user_data) {
  if (cb == nullptr) return 0;
  std::lock_guard<std::mutex> lock(impl_->m);
  const PageSubscriptionId id = impl_->next_sub_id++;
  impl_->subs.push_back({id, cb, user_data});
  return id;
}

Status PageStore::unsubscribe(PageSubscriptionId id) {
  std::lock_guard<std::mutex> lock(impl_->m);
  auto it = std::find_if(impl_->subs.begin(), impl_->subs.end(),
                         [id](const Impl::Sub& s) { return s.id == id; });
  if (it == impl_->subs.end()) {
    return set_last_error(ScanError::kNotFound, "page store: no subscription %u", id);
  }
  impl_->subs.erase(it);
  return kOkStatus;
}

}  // namespace scanengine
