// PagedCloudRenderer.h — GPU mirror of the engine's PageStore.
//
// Productionized from spikes/s3-render/src/PagedCloud.{h,cpp}. The S3 version
// owned its own points and was fed by a synthetic producer; this one owns
// nothing but GPU buffers and mirrors scanengine::PageStore, which is "the
// engine↔renderer contract" (cloud/page_store.h).
//
// HOW IT READS THE STORE, AND WHY THAT IS SAFE WITHOUT A LOCK
//   page_store.h guarantees: a page allocates its full capacity once and NEVER
//   reallocates, so PageView::data is stable; `count` only ever grows and is
//   published with a release store. So sync() can, every frame:
//     for each page id -> page_view(id) -> upload [uploaded, count) -> remember count
//   and never take a lock, never miss data, and never read a torn point.
//
// WHY IT POLLS INSTEAD OF FOLLOWING kPointsAvailable EVENTS
//   DESIGN.md §5: an event subscriber that sees kEventsDropped "must stop
//   trusting incremental ranges and re-read the pages". Re-reading the pages is
//   the only path that is correct under backpressure — so this renderer just
//   always does that, and the event stream is used exclusively for UI (device
//   state, health, session state). One code path, no drop-recovery branch that
//   only executes under load and therefore only breaks in the field.
//
// DIFFERENCES FROM S3 WORTH KNOWING
//   * One shared identity IndexBuffer for every page instead of one per page.
//     Filament requires an IndexBuffer even for POINTS; at 1M points that is
//     4 MB each, so sharing saves 4 MB per page (36 MB at S3's 10M-point run).
//   * Per-page bounding boxes come from PageView::bounds_*, so frustum culling
//     actually culls. S3 deliberately gave every page the whole room to measure
//     the worst case.
//   * A soft LOD budget (A14's DisplayParams::lod_point_budget): pages past the
//     budget stay resident on the GPU but are removed from the scene. That is a
//     throttle, not the coarse-to-fine LOD of §3.12 — see NOTES.md.
//
// Owner: C1.
#pragma once

#include <cstdint>
#include <map>

#include <utils/Entity.h>

#include "scanengine/cloud/page_store.h"

namespace filament {
class Engine;
class IndexBuffer;
class MaterialInstance;
class Scene;
class VertexBuffer;
}  // namespace filament

namespace lidarscan {

struct CloudStats {
  std::uint64_t resident_points = 0;   // points uploaded to the GPU
  std::uint64_t drawn_points = 0;      // points actually in the scene (after the LOD budget)
  std::size_t pages = 0;
  std::size_t pages_drawn = 0;
  std::size_t gpu_bytes = 0;
  std::uint64_t uploads = 0;           // setBufferAt calls since init
  bool bounds_valid = false;
  float bounds_min[3] = {0, 0, 0};
  float bounds_max[3] = {0, 0, 0};
  // Luminance range of the uploaded points, 0..1, using the same
  // 0.299/0.587/0.114 weights evaluate_point_color() uses for its
  // RGB-derived-intensity bridge. This is what ColorMode::kIntensity's
  // auto_range needs and what PageView (which carries positional bounds only)
  // cannot provide. Accumulated while the points are already in cache for the
  // upload memcpy, so it costs one extra pass over new data.
  //
  // These are the 1st and 99th PERCENTILE, not the min and max, and that is a
  // deliberate C1 decision: a lidar scan routinely contains a handful of
  // saturated returns off retroreflective tape or a road sign (the engine's own
  // synthetic room has a high-reflectivity band at intensity 255 among points
  // at ~25), and stretching the colour ramp over min..max lets those few points
  // push every real return into the bottom 10% of the ramp — i.e. renders the
  // whole cloud black. Percentiles of the actual data are still "the actual
  // data range" in A14's sense; robustness is the renderer's business.
  bool intensity_valid = false;
  float intensity_min = 0.0f;   // 1st percentile
  float intensity_max = 1.0f;   // 99th percentile
  float intensity_abs_min = 0.0f;
  float intensity_abs_max = 1.0f;
};

class PagedCloudRenderer {
 public:
  void init(filament::Engine* engine, filament::Scene* scene, filament::MaterialInstance* mi);
  void shutdown();

  // Upload everything the store has that the GPU does not. Returns the number
  // of points uploaded by this call.
  std::uint32_t sync(const scanengine::PageStore& store, std::uint32_t lod_point_budget);

  // Drop every GPU page (after a PageStore::clear(), or when closing a project).
  void reset();

  const CloudStats& stats() const { return stats_; }

 private:
  struct GpuPage {
    filament::VertexBuffer* vb = nullptr;
    filament::IndexBuffer* ib = nullptr;  // shared unless the capacity differs
    bool owns_ib = false;
    utils::Entity entity;
    std::uint32_t capacity = 0;
    std::uint32_t uploaded = 0;
    bool in_scene = false;
  };

  GpuPage* ensurePage(scanengine::PageId id, const scanengine::PageView& view);
  filament::IndexBuffer* sharedIndexBuffer(std::uint32_t capacity);
  void destroyPage(GpuPage& p);

  filament::Engine* engine_ = nullptr;
  filament::Scene* scene_ = nullptr;
  filament::MaterialInstance* material_ = nullptr;
  filament::IndexBuffer* shared_ib_ = nullptr;
  std::uint32_t shared_ib_capacity_ = 0;

  // Ordered by PageId so "the first pages" is a stable, creation-ordered set.
  std::map<scanengine::PageId, GpuPage> pages_;
  CloudStats stats_{};
  // 256-bin luminance histogram over every uploaded point, for the percentile
  // range above. 1 KB, one increment per point.
  static constexpr int kLumBins = 256;
  std::uint64_t lum_hist_[kLumBins] = {};
  std::uint64_t lum_count_ = 0;
  float intensity_abs_min_ = 0.0f;
  float intensity_abs_max_ = 1.0f;
};

}  // namespace lidarscan
