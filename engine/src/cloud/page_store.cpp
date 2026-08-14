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
  std::vector<std::unique_ptr<Page>> pages;
  PageId next_page_id = 1;
  std::uint64_t total_points = 0;
  std::uint64_t dropped_points = 0;

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
};

PageStore::PageStore(const PageStoreConfig& cfg) : impl_(new Impl) {
  impl_->cfg = cfg;
  if (impl_->cfg.page_capacity == 0) impl_->cfg.page_capacity = kDefaultPageCapacity;
  if (impl_->cfg.max_pages == 0) impl_->cfg.max_pages = 1;
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

  {
    std::lock_guard<std::mutex> lock(impl_->m);
    while (written < points.size()) {
      Page* tail = impl_->pages.empty() ? nullptr : impl_->pages.back().get();
      const bool need_page =
          tail == nullptr || tail->count.load(std::memory_order_relaxed) >= tail->capacity ||
          tail->stream != stream;  // pages are single-stream: keeps provenance
                                   // per page for A13 merge + A9 export.
      bool created = false;
      if (need_page) {
        if (impl_->pages.size() >= impl_->cfg.max_pages) {
          full = true;
          break;
        }
        auto p = std::make_unique<Page>();
        p->id = impl_->next_page_id++;
        p->stream = stream;
        p->capacity = impl_->cfg.page_capacity;
        p->data.reset(new PointVertex[p->capacity]);
        p->t_first_ns = t_mono_ns;
        tail = p.get();
        impl_->pages.push_back(std::move(p));
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

      PageUpdate u;
      u.page = tail->id;
      u.stream = stream;
      u.first = live;
      u.count = n;
      u.page_created = created;
      updates.push_back(u);

      written += n;
    }

    if (full) impl_->dropped_points += points.size() - written;
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
  if (full) {
    SCAN_LOG_WARN(kMod, "page store full (%u pages): dropped %zu points",
                  impl_->cfg.max_pages, points.size() - written);
    return set_last_error(ScanError::kCapacityExceeded,
                          "page store full at %u pages; dropped %zu points",
                          impl_->cfg.max_pages, points.size() - written);
  }
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
  return impl_->pages.size() * static_cast<std::size_t>(impl_->cfg.page_capacity) *
         sizeof(PointVertex);
}

void PageStore::clear() {
  std::lock_guard<std::mutex> lock(impl_->m);
  impl_->pages.clear();
  impl_->total_points = 0;
  impl_->dropped_points = 0;
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
