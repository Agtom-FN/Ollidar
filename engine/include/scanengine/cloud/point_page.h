// point_page.h — the render-facing point buffer layout.
//
// PROVEN IN S3 (spikes/s3-render): Filament rendered a paged 10 M-point
// cloud at 138–149 fps on an M4 using exactly this layout — interleaved
// float3 position + RGBA8 colour, 16 bytes per point, ~1 M points per page,
// one VertexBuffer + IndexBuffer + renderable per page, partial uploads with
// setBufferAt() and a setGeometryAt() count bump. Tech Spec §3.12 ("paged
// streaming buffers, ~1 M pts/page") is this file.
//
// DO NOT change PointVertex without re-running the S3 benchmark: the layout
// is what makes uploads a straight memcpy into a GPU buffer with no
// per-point shuffling, and it is what the Filament point material's vertex
// attributes are declared against.
//
// Owner: A1 (layout + store) / A14 (LOD, display parameters, colour modes).
#ifndef SCANENGINE_CLOUD_POINT_PAGE_H
#define SCANENGINE_CLOUD_POINT_PAGE_H

#include <cstddef>
#include <cstdint>

#include "scanengine/core/types.h"

namespace scanengine {

// 16 bytes. Position in the session's local metric frame (right-handed,
// metres). Colour is straight RGBA8, premultiplied by nothing; alpha is 255
// for a live point and is the channel A14 uses for LOD fade / selection.
struct PointVertex {
  float x, y, z;
  std::uint8_t r, g, b, a;
};

static_assert(sizeof(PointVertex) == 16,
              "PointVertex must stay 16 bytes — the S3-proven GPU layout");
static_assert(alignof(PointVertex) == 4, "PointVertex must stay 4-byte aligned");

inline constexpr std::uint32_t kDefaultPageCapacity = 1u << 20;  // 1,048,576 points

// A read-only view of one page, valid as long as the PageStore lives and the
// page is not cleared. `data` is STABLE: a page allocates its full capacity
// once and never reallocates, so a renderer may keep the pointer and read
// [0, count) without holding any lock (count is only ever increased, and is
// published to the renderer through EventType::kPointsAvailable, which
// establishes the happens-before edge).
struct PageView {
  PageId id = kInvalidPageId;
  StreamId stream = StreamId::kUnknown;
  const PointVertex* data = nullptr;
  std::uint32_t count = 0;     // points currently live in the page
  std::uint32_t capacity = 0;
  std::int64_t t_first_ns = 0;
  std::int64_t t_last_ns = 0;
  float bounds_min[3] = {0.f, 0.f, 0.f};
  float bounds_max[3] = {0.f, 0.f, 0.f};

  bool valid() const { return data != nullptr; }
  std::size_t bytes() const { return static_cast<std::size_t>(count) * sizeof(PointVertex); }
};

// WHY a page changed (INT-34, closing docs/A11-color.md §8.1).
//
// The store's original traffic was append-only, so a PageUpdate could only
// ever mean "these points are new". Colorization is the one producer that
// REWRITES points that already exist (§3.5's "RGB into final cloud"), and a
// renderer that cannot tell the two apart keeps the stale GPU buffer until
// something else forces a re-upload. This enum is what tells it apart, and it
// is deliberately an extra FIELD on PageUpdate rather than a second event
// type: every existing subscriber keeps compiling, keeps its one code path,
// and — because both kinds carry the same [first, first+count) range — a
// subscriber that ignores `kind` entirely still does the right thing (it
// re-uploads the range).
enum class PageUpdateKind : std::uint8_t {
  // [first, first+count) are points that did not exist before this update.
  kAppended = 0,
  // [first, first+count) already existed; only their r/g/b/a bytes changed.
  // Positions, bounds, count, page id and time range are all unchanged, so a
  // consumer that caches geometry only has to re-upload colour.
  kRecoloured = 1,
  // The page is GONE: a live store running PageFullPolicy::kEvictOldest made
  // room for newer data by retiring its oldest page. `first` is 0 and `count`
  // is how many points left the window, so a subscriber that ignores `kind`
  // does the same harmless thing it does for the other two — it asks for the
  // page, which no longer resolves (page_view() is invalid, page_data_mutable()
  // is nullptr), and skips it. A consumer that DOES read `kind` can drop its
  // GPU buffers for `page` the moment it hears, instead of noticing on its
  // next enumeration.
  //
  // The page id is never reused (page_store.h's id rule holds through
  // eviction), so this is unambiguous.
  kEvicted = 2,
};

const char* to_string(PageUpdateKind k) noexcept;

// What changed in the store. Handed to PageStore subscribers (the renderer,
// and internally the Engine, which turns it into an event).
struct PageUpdate {
  PageId page = kInvalidPageId;
  StreamId stream = StreamId::kUnknown;
  std::uint32_t first = 0;   // index of the first written point
  std::uint32_t count = 0;   // number of written points
  bool page_created = false; // renderer must allocate GPU buffers for `page`
  // Defaulted to kAppended so every pre-INT-34 producer and consumer is
  // unchanged in meaning.
  PageUpdateKind kind = PageUpdateKind::kAppended;
};

}  // namespace scanengine

#endif  // SCANENGINE_CLOUD_POINT_PAGE_H
