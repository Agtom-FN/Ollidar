// point_source.h — the one iteration path every A9 writer streams points
// through: page selection, bounds-filter + decimate selection, the
// two-pass (count/bounds, then write) traversal, and the RGB-luminance
// intensity bridge documented in scanengine/export/exporter.h.
//
// Centralizing this is what guarantees the count/bounds pass and the write
// pass agree on exactly which points are selected — a divergence there
// would corrupt LAS headers (wrong point count / bounds) or PLY/PCD
// `element vertex` / `POINTS` counts silently.
//
// Private to export/ — not part of the public API surface.
#ifndef SCANENGINE_SRC_EXPORT_POINT_SOURCE_H
#define SCANENGINE_SRC_EXPORT_POINT_SOURCE_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/export/exporter.h"

namespace scanengine::exportimpl {

// --- intensity bridge (see exporter.h) --------------------------------------
inline std::uint8_t luminance8(const PointVertex& p) {
  const unsigned l = (299u * p.r + 587u * p.g + 114u * p.b + 500u) / 1000u;
  return static_cast<std::uint8_t>(l > 255u ? 255u : l);
}

// Widened losslessly (byte replicated into both halves) rather than
// left-shifted-and-zero-filled, so a 255 (max reflectivity) round-trips to
// 65535 (max), not 65280 — the LAS convention for expanding 8-bit sensor
// ranges into the format's 16-bit intensity field.
inline std::uint16_t luminance16(const PointVertex& p) {
  const std::uint8_t l = luminance8(p);
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(l) << 8) | l);
}

// LAS RGB channels are 16-bit; the same byte-replication convention applies
// to color, which only ever comes from PointVertex's 8-bit channels here.
inline std::uint16_t widen8to16(std::uint8_t v) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(v) << 8) | v);
}

// --- page selection ----------------------------------------------------------
inline std::vector<PageView> select_pages(const PageStore& store, Span<const StreamId> streams) {
  std::vector<PageView> out;
  for (PageId id : store.page_ids()) {
    const PageView pv = store.page_view(id);
    if (!pv.valid()) continue;  // defensive; page_ids() shouldn't produce these
    if (streams.empty()) {
      out.push_back(pv);
      continue;
    }
    for (const StreamId s : streams) {
      if (pv.stream == s) {
        out.push_back(pv);
        break;
      }
    }
  }
  return out;
}

// --- selection predicate -----------------------------------------------------
// `global_idx` runs over every point in every selected page, in page order,
// 0-based — the same sequence for every pass, which is what makes decimate
// deterministic across the count pass and the write pass.
inline bool point_selected(const PointVertex& p, std::uint64_t global_idx,
                            const ExportOptions& opts) {
  const std::uint32_t stride = (opts.decimate == 0) ? 1u : opts.decimate;
  if (stride > 1 && (global_idx % stride) != 0) return false;
  if (opts.bounds_filter.enabled) {
    if (p.x < opts.bounds_filter.min[0] || p.x > opts.bounds_filter.max[0]) return false;
    if (p.y < opts.bounds_filter.min[1] || p.y > opts.bounds_filter.max[1]) return false;
    if (p.z < opts.bounds_filter.min[2] || p.z > opts.bounds_filter.max[2]) return false;
  }
  return true;
}

// --- pass 1: count + bounds ---------------------------------------------------
struct SelectionStats {
  std::uint64_t count = 0;
  float min[3] = {0.f, 0.f, 0.f};
  float max[3] = {0.f, 0.f, 0.f};
};

inline SelectionStats scan_selected(const std::vector<PageView>& pages,
                                     const ExportOptions& opts) {
  SelectionStats st;
  bool first = true;
  std::uint64_t global_idx = 0;
  for (const PageView& pv : pages) {
    for (std::uint32_t i = 0; i < pv.count; ++i, ++global_idx) {
      const PointVertex& p = pv.data[i];
      if (!point_selected(p, global_idx, opts)) continue;
      ++st.count;
      if (first) {
        st.min[0] = st.max[0] = p.x;
        st.min[1] = st.max[1] = p.y;
        st.min[2] = st.max[2] = p.z;
        first = false;
      } else {
        st.min[0] = std::min(st.min[0], p.x);
        st.min[1] = std::min(st.min[1], p.y);
        st.min[2] = std::min(st.min[2], p.z);
        st.max[0] = std::max(st.max[0], p.x);
        st.max[1] = std::max(st.max[1], p.y);
        st.max[2] = std::max(st.max[2], p.z);
      }
    }
  }
  return st;
}

inline std::uint64_t count_selected(const std::vector<PageView>& pages,
                                     const ExportOptions& opts) {
  std::uint64_t n = 0;
  std::uint64_t global_idx = 0;
  for (const PageView& pv : pages) {
    for (std::uint32_t i = 0; i < pv.count; ++i, ++global_idx) {
      if (point_selected(pv.data[i], global_idx, opts)) ++n;
    }
  }
  return n;
}

// --- pass 2: the write traversal ---------------------------------------------
//
// Checked every kCancelCheckStride RAW points (not selected points), so a
// heavily-filtered export (e.g. a tiny bounds box over a huge cloud) still
// cancels promptly instead of scanning to completion first.
inline constexpr std::uint32_t kCancelCheckStride = 4096;
inline constexpr std::uint64_t kProgressStride = 8192;

// Emit signature: void(const PointVertex& p, const PageView& page,
//                       std::uint32_t idx_in_page, std::uint64_t out_seq)
// out_seq is 0..total-1, the point's position in the OUTPUT file.
template <typename Emit>
Status for_each_selected(const std::vector<PageView>& pages, const ExportOptions& opts,
                          std::uint64_t total, ExportProgressCallback progress_cb,
                          void* progress_user_data, ExportCancelToken* cancel_token,
                          Emit&& emit) {
  std::uint64_t global_idx = 0;
  std::uint64_t out_seq = 0;
  std::uint32_t since_check = 0;
  for (const PageView& pv : pages) {
    for (std::uint32_t i = 0; i < pv.count; ++i, ++global_idx) {
      const PointVertex& p = pv.data[i];
      if (point_selected(p, global_idx, opts)) {
        emit(p, pv, i, out_seq);
        ++out_seq;
        if (progress_cb != nullptr && total > 0 &&
            (out_seq % kProgressStride == 0 || out_seq == total)) {
          progress_cb(
              static_cast<float>(static_cast<double>(out_seq) / static_cast<double>(total)),
              progress_user_data);
        }
      }
      if (++since_check >= kCancelCheckStride) {
        since_check = 0;
        if (cancel_token != nullptr && cancel_token->cancelled()) {
          return Status(ScanError::kCancelled);
        }
      }
    }
  }
  if (progress_cb != nullptr && total == 0) {
    progress_cb(1.0f, progress_user_data);
  }
  return Status(ScanError::kOk);
}

}  // namespace scanengine::exportimpl

#endif  // SCANENGINE_SRC_EXPORT_POINT_SOURCE_H
