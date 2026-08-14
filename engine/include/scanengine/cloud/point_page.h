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

// What changed in the store. Handed to PageStore subscribers (the renderer,
// and internally the Engine, which turns it into an event).
struct PageUpdate {
  PageId page = kInvalidPageId;
  StreamId stream = StreamId::kUnknown;
  std::uint32_t first = 0;   // index of the first newly written point
  std::uint32_t count = 0;   // number of newly written points
  bool page_created = false; // renderer must allocate GPU buffers for `page`
};

}  // namespace scanengine

#endif  // SCANENGINE_CLOUD_POINT_PAGE_H
