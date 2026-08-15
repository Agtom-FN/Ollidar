// session.h — what a "session" is to the merge module (Tech Spec §3.10).
//
// A13 merges N finished captures. Its input is therefore NOT a live engine:
// it is what A7's post pipeline produced (`PostSlamPipeline::final_cloud()`,
// its keyframes and its trajectory), optionally what A10 solved for the same
// session (`GeorefSolution` + the ENU frame that solution is expressed in),
// and a stable provenance id the app can put in front of a human.
//
// THE CLOUD IS NON-OWNING, and that is the whole reason this file exists.
// A merged project holds several full-density clouds at once — three
// 30-minute Mid-360 sessions are ~1 GB of PointVertex — and copying them into
// the merger would double that for no gain. So a session's cloud is a list of
// SPANS:
//
//   * A7's `final_cloud()` is one span over one vector.
//   * A `PageStore` is one span per page, and cloud/point_page.h guarantees
//     `PageView::data` is stable for the store's lifetime (a page allocates
//     its capacity once and never reallocates), which is exactly what makes
//     `collect_pages()` safe rather than a lifetime bug waiting to happen.
//
// The caller must keep that memory alive for as long as the MergeProject is
// used. This is the same contract PageStore already gives its renderer.
//
// Owner: A13.
#ifndef SCANENGINE_MERGE_SESSION_H
#define SCANENGINE_MERGE_SESSION_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/core/types.h"
#include "scanengine/gnss/crs.h"
#include "scanengine/gnss/georef.h"

namespace scanengine {
namespace merge {

// A cloud as a list of contiguous runs. Empty runs are ignored; the order of
// the runs is the order of the points in every derived product, so it is part
// of the determinism contract.
struct SessionCloud {
  std::vector<Span<const PointVertex>> chunks;

  void add(Span<const PointVertex> s) {
    if (s.size() != 0) chunks.push_back(s);
  }
  void add(const std::vector<PointVertex>& v) {
    if (!v.empty()) chunks.push_back(Span<const PointVertex>(v.data(), v.size()));
  }
  std::uint64_t point_count() const {
    std::uint64_t n = 0;
    for (const auto& c : chunks) n += static_cast<std::uint64_t>(c.size());
    return n;
  }
  bool empty() const { return point_count() == 0; }
  void clear() { chunks.clear(); }
};

// Every page of `store` on `stream` becomes one chunk, in `page_ids()` order.
// `stream == StreamId::kUnknown` takes every page regardless of stream.
// The spans point INTO the store; it must outlive the SessionCloud.
Status collect_pages(const PageStore& store, StreamId stream, SessionCloud* out);

// A keyframe pose of the session, in the session's own local frame. A13 does
// not need them to align anything — the clouds carry all the geometry — but
// it carries them so a merge workbench can draw every session's trajectory in
// the merged frame (`MergeProject::keyframes_in_world()`), and so a future
// intra-session relaxation has somewhere to attach.
struct SessionKeyframe {
  std::int64_t t_ns = 0;
  double q[4] = {0.0, 0.0, 0.0, 1.0};  // (x, y, z, w), local_from_body
  double p[3] = {0.0, 0.0, 0.0};
};

// A10's answer for this session, plus the frame that answer lives in.
//
// THE ENU FRAME IS NOT OPTIONAL AND IS NOT SHARED. `GeorefSolution::
// global_from_local` maps the session's local frame into the ENU tangent
// frame the session's own GnssSource anchored — normally at that session's
// first fix. Two sessions captured on two days have two different ENU
// origins, so "they share a CRS" does NOT mean "their global coordinates are
// in the same frame": composing them means going ENU_a -> ECEF -> ENU_b.
// That composition is exact (a rotation and a translation, crs.h §"local ENU
// tangent frame"), it is what `MergeProject::align_georeferenced()` does, and
// it is why this struct carries the frame rather than assuming one.
struct SessionGeoref {
  bool valid = false;
  GeorefSolution solution{};
  crs::EnuFrame enu{};
  int epsg = 0;  // 0 = unknown; sessions with different non-zero codes are
                 // still composable (the ENU/ECEF path does not use it), but
                 // a mismatch is reported because it usually means the
                 // operator picked two different project CRSs.
};

// What the caller hands `MergeProject::add_session()`.
struct SessionInput {
  // Stable, human-facing, and the thing a report is keyed by. Empty gets
  // "session-<n>". Uniqueness is checked (kAlreadyExists).
  std::string provenance_id;
  // Where it came from, for the report only. A13 does not read it — see
  // docs/A13-merge.md §2 for why `.lscan` cannot be a cloud source yet.
  std::string lscan_dir;

  SessionCloud cloud;
  std::vector<SessionKeyframe> keyframes;
  SessionGeoref georef;

  // Voxel-dedup priority when two sessions claim the same voxel: LOWER WINS.
  // Ties break on session id, so the default (priority = insertion order)
  // means "the first session added owns the overlap".
  std::int32_t priority = -1;  // -1 = use the session's own index

  // Pin this session's local frame as the merged frame. Exactly one session
  // is the anchor; the first one added is it unless this says otherwise.
  bool anchor = false;
};

enum class AlignSource : std::uint8_t {
  kNone = 0,           // no alignment yet — identity, and not trusted
  kAnchor = 1,         // this session IS the merged frame
  kGeoreferenced = 2,  // composed through the shared CRS
  kManual = 3,         // 3-point correspondences (or a caller-supplied matrix)
  kYawSearch = 4,      // the Manhattan fallback
  kIcp = 5,            // refined pairwise
  kRelaxed = 6,        // refined and then moved by the global relaxation
};

const char* to_string(AlignSource s) noexcept;

// One session inside a project. `world_from_session` is the live estimate;
// everything else is what the caller supplied.
struct MergeSession {
  std::uint32_t id = 0;
  std::string provenance_id;
  std::string lscan_dir;
  SessionCloud cloud;
  std::vector<SessionKeyframe> keyframes;
  SessionGeoref georef;
  std::int32_t priority = 0;

  double world_from_session[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  AlignSource align = AlignSource::kNone;
  bool anchor = false;

  std::uint64_t point_count() const { return cloud.point_count(); }
};

}  // namespace merge
}  // namespace scanengine

#endif  // SCANENGINE_MERGE_SESSION_H
