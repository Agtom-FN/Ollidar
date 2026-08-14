#include "render/PagedCloudRenderer.h"

#include <filament/Box.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/VertexBuffer.h>

#include <utils/EntityManager.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace filament;

namespace lidarscan {

void PagedCloudRenderer::init(Engine* engine, Scene* scene, MaterialInstance* mi) {
  engine_ = engine;
  scene_ = scene;
  material_ = mi;
}

void PagedCloudRenderer::shutdown() {
  if (!engine_) return;
  reset();
  if (shared_ib_) {
    engine_->destroy(shared_ib_);
    shared_ib_ = nullptr;
    shared_ib_capacity_ = 0;
  }
  engine_ = nullptr;
  scene_ = nullptr;
  material_ = nullptr;
}

void PagedCloudRenderer::destroyPage(GpuPage& p) {
  auto& em = utils::EntityManager::get();
  if (p.in_scene && scene_) scene_->remove(p.entity);
  if (!p.entity.isNull()) {
    engine_->destroy(p.entity);
    em.destroy(p.entity);
  }
  if (p.vb) engine_->destroy(p.vb);
  if (p.owns_ib && p.ib) engine_->destroy(p.ib);
  p = GpuPage{};
}

void PagedCloudRenderer::reset() {
  if (!engine_) return;
  // Nothing may still be referencing these buffers when they go away.
  engine_->flushAndWait();
  for (auto& kv : pages_) destroyPage(kv.second);
  pages_.clear();
  stats_ = CloudStats{};
  for (auto& b : lum_hist_) b = 0;
  lum_count_ = 0;
  intensity_abs_min_ = 0.0f;
  intensity_abs_max_ = 1.0f;
}

IndexBuffer* PagedCloudRenderer::sharedIndexBuffer(std::uint32_t capacity) {
  if (shared_ib_ && shared_ib_capacity_ == capacity) return shared_ib_;
  if (shared_ib_) return nullptr;  // a page with a different capacity: it gets its own

  auto* idx = static_cast<std::uint32_t*>(std::malloc(sizeof(std::uint32_t) * capacity));
  if (!idx) return nullptr;
  for (std::uint32_t i = 0; i < capacity; ++i) idx[i] = i;
  IndexBuffer* ib = IndexBuffer::Builder()
                        .indexCount(capacity)
                        .bufferType(IndexBuffer::IndexType::UINT)
                        .build(*engine_);
  ib->setBuffer(*engine_,
                IndexBuffer::BufferDescriptor(idx, sizeof(std::uint32_t) * capacity,
                                              [](void* b, size_t, void*) { std::free(b); }));
  shared_ib_ = ib;
  shared_ib_capacity_ = capacity;
  return ib;
}

PagedCloudRenderer::GpuPage* PagedCloudRenderer::ensurePage(scanengine::PageId id,
                                                            const scanengine::PageView& view) {
  auto it = pages_.find(id);
  if (it != pages_.end()) return &it->second;

  GpuPage p;
  p.capacity = view.capacity ? view.capacity : scanengine::kDefaultPageCapacity;

  // 16-byte interleaved PointVertex: float3 position + normalized UBYTE4 colour.
  // This layout is asserted in cloud/point_page.h and is what S3 measured.
  p.vb = VertexBuffer::Builder()
             .vertexCount(p.capacity)
             .bufferCount(1)
             .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0,
                        sizeof(scanengine::PointVertex))
             .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::UBYTE4,
                        offsetof(scanengine::PointVertex, r), sizeof(scanengine::PointVertex))
             .normalized(VertexAttribute::COLOR)
             .build(*engine_);
  if (!p.vb) return nullptr;

  p.ib = sharedIndexBuffer(p.capacity);
  if (p.ib) {
    p.owns_ib = false;
  } else {
    auto* idx = static_cast<std::uint32_t*>(std::malloc(sizeof(std::uint32_t) * p.capacity));
    for (std::uint32_t i = 0; i < p.capacity; ++i) idx[i] = i;
    p.ib = IndexBuffer::Builder()
               .indexCount(p.capacity)
               .bufferType(IndexBuffer::IndexType::UINT)
               .build(*engine_);
    p.ib->setBuffer(*engine_,
                    IndexBuffer::BufferDescriptor(idx, sizeof(std::uint32_t) * p.capacity,
                                                  [](void* b, size_t, void*) { std::free(b); }));
    p.owns_ib = true;
  }

  p.entity = utils::EntityManager::get().create();
  RenderableManager::Builder(1)
      .boundingBox(Box{{0, 0, 0}, {0.5f, 0.5f, 0.5f}})  // replaced from PageView bounds on upload
      .material(0, material_)
      .geometry(0, RenderableManager::PrimitiveType::POINTS, p.vb, p.ib, 0, 1)
      .culling(true)
      .castShadows(false)
      .receiveShadows(false)
      .build(*engine_, p.entity);

  auto res = pages_.emplace(id, p);
  return &res.first->second;
}

std::uint32_t PagedCloudRenderer::sync(const scanengine::PageStore& store,
                                       std::uint32_t lod_point_budget) {
  if (!engine_ || !material_) return 0;

  const std::vector<scanengine::PageId> ids = store.page_ids();

  // Pages that vanished (PageStore::clear(), or a future A14 eviction policy).
  if (pages_.size() > ids.size()) {
    for (auto it = pages_.begin(); it != pages_.end();) {
      if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
        engine_->flushAndWait();
        destroyPage(it->second);
        it = pages_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::uint32_t uploaded_now = 0;
  CloudStats s{};
  auto& rm = engine_->getRenderableManager();

  // Ordered walk: PageIds are assigned in creation order and never reused
  // (page_store.h), so this is chronological and the LOD budget therefore keeps
  // the OLDEST points — the ones that make up the already-scanned room —
  // rather than flickering between arbitrary pages frame to frame.
  std::uint64_t drawn = 0;
  for (scanengine::PageId id : ids) {
    const scanengine::PageView view = store.page_view(id);
    if (!view.valid()) continue;

    GpuPage* p = ensurePage(id, view);
    if (!p) continue;

    const std::uint32_t count = std::min(view.count, p->capacity);
    if (count > p->uploaded) {
      const std::uint32_t n = count - p->uploaded;
      const std::size_t bytes = std::size_t(n) * sizeof(scanengine::PointVertex);
      void* mem = std::malloc(bytes);
      if (mem) {
        // Legal without a lock: [uploaded, count) is published-before by the
        // release store on `count` (page_store.h's memory contract), and
        // `data` never moves.
        std::memcpy(mem, view.data + p->uploaded, bytes);
        for (std::uint32_t k = 0; k < n; ++k) {
          const auto& v = view.data[p->uploaded + k];
          const float lum = (0.299f * v.r + 0.587f * v.g + 0.114f * v.b) / 255.0f;
          if (lum_count_ == 0) {
            intensity_abs_min_ = intensity_abs_max_ = lum;
          } else {
            intensity_abs_min_ = std::min(intensity_abs_min_, lum);
            intensity_abs_max_ = std::max(intensity_abs_max_, lum);
          }
          int bin = int(lum * float(kLumBins - 1) + 0.5f);
          bin = std::clamp(bin, 0, kLumBins - 1);
          ++lum_hist_[bin];
          ++lum_count_;
        }
        p->vb->setBufferAt(
            *engine_, 0,
            VertexBuffer::BufferDescriptor(mem, bytes,
                                           [](void* b, size_t, void*) { std::free(b); }),
            std::uint32_t(std::size_t(p->uploaded) * sizeof(scanengine::PointVertex)));
        p->uploaded = count;
        uploaded_now += n;
        ++stats_.uploads;
      }
    }

    auto inst = rm.getInstance(p->entity);
    if (inst.isValid()) {
      rm.setGeometryAt(inst, 0, RenderableManager::PrimitiveType::POINTS, p->vb, p->ib, 0,
                       std::max<std::uint32_t>(p->uploaded, 1));
      rm.setAxisAlignedBoundingBox(
          inst, Box{}.set({view.bounds_min[0], view.bounds_min[1], view.bounds_min[2]},
                          {view.bounds_max[0], view.bounds_max[1], view.bounds_max[2]}));
    }

    // Soft LOD budget: past it, keep the page resident but stop drawing it.
    const bool want_drawn =
        p->uploaded > 0 && (lod_point_budget == 0 || drawn < lod_point_budget);
    if (want_drawn && !p->in_scene) {
      scene_->addEntity(p->entity);
      p->in_scene = true;
    } else if (!want_drawn && p->in_scene) {
      scene_->remove(p->entity);
      p->in_scene = false;
    }
    if (p->in_scene) {
      drawn += p->uploaded;
      ++s.pages_drawn;
    }

    s.resident_points += p->uploaded;
    ++s.pages;
    s.gpu_bytes += std::size_t(p->capacity) * sizeof(scanengine::PointVertex);

    if (p->uploaded > 0) {
      for (int k = 0; k < 3; ++k) {
        if (!s.bounds_valid) {
          s.bounds_min[k] = view.bounds_min[k];
          s.bounds_max[k] = view.bounds_max[k];
        } else {
          s.bounds_min[k] = std::min(s.bounds_min[k], view.bounds_min[k]);
          s.bounds_max[k] = std::max(s.bounds_max[k], view.bounds_max[k]);
        }
      }
      s.bounds_valid = true;
    }
  }

  // One shared index buffer for every page (4 MB each if it were per page).
  if (shared_ib_) s.gpu_bytes += std::size_t(shared_ib_capacity_) * sizeof(std::uint32_t);
  s.drawn_points = drawn;
  s.uploads = stats_.uploads;
  if (lum_count_ > 0) {
    const std::uint64_t lo_target = lum_count_ / 100;              // 1st percentile
    const std::uint64_t hi_target = lum_count_ - lum_count_ / 100;  // 99th percentile
    std::uint64_t acc = 0;
    int lo_bin = 0, hi_bin = kLumBins - 1;
    bool lo_done = false;
    for (int b = 0; b < kLumBins; ++b) {
      acc += lum_hist_[b];
      if (!lo_done && acc > lo_target) {
        lo_bin = b;
        lo_done = true;
      }
      if (acc >= hi_target) {
        hi_bin = b;
        break;
      }
    }
    s.intensity_valid = true;
    s.intensity_min = float(lo_bin) / float(kLumBins - 1);
    s.intensity_max = float(hi_bin) / float(kLumBins - 1);
    s.intensity_abs_min = intensity_abs_min_;
    s.intensity_abs_max = intensity_abs_max_;
  }
  stats_ = s;
  return uploaded_now;
}

}  // namespace lidarscan
