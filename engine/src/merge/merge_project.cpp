// merge_project.cpp — the multi-session merge project (A13, Tech Spec §3.10).
//
// The order of business is fixed by what each stage can and cannot recover
// from: coarse alignment picks the basin (georeferenced > manual picks > the
// yaw search), pairwise ICP refines inside it, the pose graph reconciles the
// pairs when there are more than two sessions, and only then are points
// copied — deduped by voxel with an explicit priority rule that leaves every
// surviving point owned by exactly one session, which is what makes the
// per-session run table in MergeResult lossless.
#include "scanengine/merge/merge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/poses/se3.h"
#include "scanengine/slam/post/cloud_filter.h"
#include "scanengine/slam/post/pose_graph.h"

#include "voxel_grid.h"

namespace scanengine {
namespace merge {

const char* to_string(AlignSource s) noexcept {
  switch (s) {
    case AlignSource::kNone: return "unaligned";
    case AlignSource::kAnchor: return "anchor";
    case AlignSource::kGeoreferenced: return "georeferenced";
    case AlignSource::kManual: return "manual";
    case AlignSource::kYawSearch: return "yaw-search";
    case AlignSource::kIcp: return "icp";
    case AlignSource::kRelaxed: return "relaxed";
  }
  return "unknown";
}

Status collect_pages(const PageStore& store, StreamId stream, SessionCloud* out) {
  if (out == nullptr) return ScanError::kInvalidArgument;
  out->clear();
  const std::vector<PageId> ids = store.page_ids();
  for (PageId id : ids) {
    const PageView v = store.page_view(id);
    if (!v.valid() || v.count == 0) continue;
    if (stream != StreamId::kUnknown && v.stream != stream) continue;
    out->add(Span<const PointVertex>(v.data, v.count));
  }
  if (out->chunks.empty()) {
    return set_last_error(ScanError::kNotFound, "merge: no pages on stream %d in this store",
                          static_cast<int>(stream));
  }
  return kOkStatus;
}

const MergePair* MergeReport::pair(std::uint32_t a, std::uint32_t b) const {
  for (const MergePair& p : pairs) {
    if ((p.session_a == a && p.session_b == b) || (p.session_a == b && p.session_b == a)) {
      return &p;
    }
  }
  return nullptr;
}

std::uint32_t MergeResult::session_at(std::uint64_t point_index) const {
  std::size_t lo = 0, hi = ranges.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (point_index < ranges[mid].first) {
      hi = mid;
    } else if (point_index >= ranges[mid].first + ranges[mid].count) {
      lo = mid + 1;
    } else {
      return ranges[mid].session;
    }
  }
  return 0xFFFFFFFFu;
}

namespace {

// Project a possibly-slightly-scaled rigid matrix back onto SE(3) by routing
// its rotation through a quaternion. A10's `global_from_local` is a
// similarity whose scale is normally locked to 1 but is stored as a general
// 3x3, so it can arrive a few ULPs (or a few 1e-4, if the estimator was run
// unlocked) off orthonormal.
void orthonormalize(const double in[16], double out[16]) {
  double R[9], t[3], q[4], Rn[9];
  se3::mat4_get_rt(in, R, t);
  se3::matrix_to_quat(R, q);
  se3::quat_to_matrix(q, Rn);
  se3::mat4_from_rt(Rn, t, out);
}

double scale_of(const double m[16]) {
  double R[9], t[3];
  se3::mat4_get_rt(m, R, t);
  // Column norms; for a similarity all three are the scale.
  double s = 0.0;
  for (int c = 0; c < 3; ++c) {
    double n = 0.0;
    for (int r = 0; r < 3; ++r) n += R[r * 3 + c] * R[r * 3 + c];
    s += std::sqrt(n);
  }
  return s / 3.0;
}

void quat_pos_of(const double m[16], double q[4], double p[3]) {
  double R[9];
  se3::mat4_get_rt(m, R, p);
  se3::matrix_to_quat(R, q);
}

}  // namespace

// --- the project ------------------------------------------------------------

struct MergeProject::Impl {
  std::vector<MergeSession> sessions;
  std::uint32_t anchor_id = 0;
  crs::EnuFrame project_enu{};
  bool has_project_enu = false;
  MergeReport report;

  bool valid(std::uint32_t id) const { return id < sessions.size(); }

  // world_from_session for a georeferenced session, composed through the CRS.
  Status georef_world_from_session(const MergeSession& s, const crs::EnuFrame& world_enu,
                                   const double world_from_world_enu[16], double out[16]) const {
    double enu_from_enu_m[16];
    SCAN_TRY(enu_from_enu(world_enu, s.georef.enu, enu_from_enu_m));
    double gfl[16];
    orthonormalize(s.georef.solution.global_from_local, gfl);
    double tmp[16];
    se3::mat4_mul(enu_from_enu_m, gfl, tmp);
    se3::mat4_mul(world_from_world_enu, tmp, out);
    return kOkStatus;
  }

  void rebase_to_anchor() {
    if (!valid(anchor_id)) return;
    double inv[16];
    se3::mat4_inverse_rigid(sessions[anchor_id].world_from_session, inv);
    for (MergeSession& s : sessions) {
      double m[16];
      se3::mat4_mul(inv, s.world_from_session, m);
      for (int i = 0; i < 16; ++i) s.world_from_session[i] = m[i];
    }
    se3::mat4_identity(sessions[anchor_id].world_from_session);
  }

  void refresh_session_summaries() {
    report.sessions.clear();
    report.input_points = 0;
    for (const MergeSession& s : sessions) {
      SessionSummary sum;
      sum.id = s.id;
      sum.provenance_id = s.provenance_id;
      sum.align = s.align;
      sum.anchor = s.anchor;
      sum.georeferenced = s.georef.valid && s.georef.solution.converged;
      sum.georef_horizontal_sigma_m = s.georef.solution.horizontal_sigma_m;
      for (int i = 0; i < 16; ++i) sum.world_from_session[i] = s.world_from_session[i];
      sum.input_points = s.point_count();
      sum.keyframes = static_cast<std::uint32_t>(s.keyframes.size());
      report.input_points += sum.input_points;
      report.sessions.push_back(sum);
    }
  }
};

MergeProject::MergeProject() : impl_(new Impl) {}
MergeProject::~MergeProject() = default;

Result<std::uint32_t> MergeProject::add_session(const SessionInput& in) {
  if (in.cloud.empty()) {
    return set_last_error(ScanError::kInvalidArgument,
                          "merge: session '%s' has no points (the cloud is a list of spans the "
                          "caller must keep alive)",
                          in.provenance_id.c_str());
  }
  MergeSession s;
  s.id = static_cast<std::uint32_t>(impl_->sessions.size());
  s.provenance_id = in.provenance_id.empty() ? ("session-" + std::to_string(s.id)) : in.provenance_id;
  for (const MergeSession& other : impl_->sessions) {
    if (other.provenance_id == s.provenance_id) {
      return set_last_error(ScanError::kAlreadyExists, "merge: duplicate session id '%s'",
                            s.provenance_id.c_str());
    }
  }
  s.lscan_dir = in.lscan_dir;
  s.cloud = in.cloud;
  s.keyframes = in.keyframes;
  s.georef = in.georef;
  s.priority = in.priority < 0 ? static_cast<std::int32_t>(s.id) : in.priority;
  se3::mat4_identity(s.world_from_session);
  s.align = AlignSource::kNone;

  const bool first = impl_->sessions.empty();
  impl_->sessions.push_back(s);
  if (first || in.anchor) {
    const Status st = set_anchor(s.id);
    if (!st.ok()) return st;
  }
  impl_->refresh_session_summaries();
  return s.id;
}

std::size_t MergeProject::session_count() const { return impl_->sessions.size(); }

const MergeSession& MergeProject::session(std::uint32_t id) const { return impl_->sessions[id]; }

const std::vector<MergeSession>& MergeProject::sessions() const { return impl_->sessions; }

int MergeProject::find(const std::string& provenance_id) const {
  for (const MergeSession& s : impl_->sessions) {
    if (s.provenance_id == provenance_id) return static_cast<int>(s.id);
  }
  return -1;
}

Status MergeProject::remove_session(std::uint32_t id) {
  if (!impl_->valid(id)) {
    return set_last_error(ScanError::kNotFound, "merge: no session %u to remove", id);
  }
  const bool was_anchor = impl_->sessions[id].anchor;

  impl_->sessions.erase(impl_->sessions.begin() + static_cast<std::ptrdiff_t>(id));
  // Ids ARE indices (merge.h states this), so everything after the hole
  // renumbers. Default priorities were handed out as the id at add time; a
  // caller-supplied priority is left alone, because it is a caller's ordering
  // and not an index.
  for (std::size_t i = 0; i < impl_->sessions.size(); ++i) {
    impl_->sessions[i].id = static_cast<std::uint32_t>(i);
  }

  // Every pair names sessions by id, so all of them are now either stale or
  // wrong. Clearing is the honest answer; refine()/survey_overlap() rebuild.
  impl_->report.pairs.clear();
  impl_->report.worst_rms_m = 0.f;
  impl_->report.worst_overlap = 0.f;
  impl_->report.pairs_refined = 0;
  impl_->report.pairs_converged = 0;
  impl_->report.pairs_low_overlap = 0;
  impl_->report.relaxed = false;
  impl_->report.graph = post::PoseGraphSummary{};
  impl_->report.merged_points = 0;
  impl_->report.dedup_dropped_points = 0;
  impl_->report.priority_dropped_points = 0;
  impl_->report.pages_appended = 0;
  impl_->report.pages_shared = 0;

  if (impl_->sessions.empty()) {
    impl_->anchor_id = 0;
    impl_->refresh_session_summaries();
    return kOkStatus;
  }

  if (was_anchor) {
    // set_anchor() rebases everything onto the new anchor's frame, so the
    // relative geometry survives and only the origin moves.
    impl_->anchor_id = 0;
    for (MergeSession& s : impl_->sessions) s.anchor = false;
    const Status st = set_anchor(0);
    if (!st.ok()) return st;
  } else {
    // The anchor is still there; its index shifted iff it sat after the hole.
    if (impl_->anchor_id > id) --impl_->anchor_id;
  }
  impl_->refresh_session_summaries();
  return kOkStatus;
}

Status MergeProject::set_anchor(std::uint32_t id) {
  if (!impl_->valid(id)) {
    return set_last_error(ScanError::kNotFound, "merge: no session %u", id);
  }
  for (MergeSession& s : impl_->sessions) {
    if (s.anchor && s.id != id && s.align == AlignSource::kAnchor) s.align = AlignSource::kNone;
    s.anchor = (s.id == id);
  }
  impl_->anchor_id = id;
  MergeSession& a = impl_->sessions[id];
  if (!impl_->has_project_enu) {
    // The merged frame IS the anchor's local frame, so every existing
    // alignment is re-expressed rather than invalidated.
    impl_->rebase_to_anchor();
  }
  if (a.align == AlignSource::kNone) a.align = AlignSource::kAnchor;
  impl_->refresh_session_summaries();
  return kOkStatus;
}

std::uint32_t MergeProject::anchor() const { return impl_->anchor_id; }

Status MergeProject::set_project_enu_frame(const crs::EnuFrame& frame) {
  if (!frame.valid) {
    return set_last_error(ScanError::kInvalidArgument, "merge: project ENU frame is not valid");
  }
  impl_->project_enu = frame;
  impl_->has_project_enu = true;
  return kOkStatus;
}

bool MergeProject::has_project_enu_frame() const { return impl_->has_project_enu; }

const crs::EnuFrame& MergeProject::project_enu_frame() const { return impl_->project_enu; }

Status MergeProject::align_georeferenced(GeorefAlignReport* out) {
  GeorefAlignReport rep;
  if (impl_->sessions.empty()) {
    rep.blocker = "no sessions";
    if (out != nullptr) *out = rep;
    return set_last_error(ScanError::kNotFound, "merge: no sessions to align");
  }

  auto usable = [](const MergeSession& s) {
    if (!s.georef.valid || !s.georef.solution.converged || !s.georef.enu.valid) return false;
    const double sc = scale_of(s.georef.solution.global_from_local);
    return std::fabs(sc - 1.0) <= 1e-3;
  };

  crs::EnuFrame world_enu{};
  double world_from_world_enu[16];
  se3::mat4_identity(world_from_world_enu);

  if (impl_->has_project_enu) {
    world_enu = impl_->project_enu;
    rep.reference_is_project = true;
    rep.reference = impl_->anchor_id;
  } else {
    const MergeSession& a = impl_->sessions[impl_->anchor_id];
    if (!usable(a)) {
      rep.blocker =
          "the anchor session is not georeferenced; set a project ENU frame or anchor a "
          "georeferenced session";
      if (out != nullptr) *out = rep;
      return set_last_error(ScanError::kInvalidState,
                            "merge: anchor session '%s' is not georeferenced, and the merged "
                            "frame is the anchor's local frame",
                            a.provenance_id.c_str());
    }
    world_enu = a.georef.enu;
    rep.reference = a.id;
    // world (= anchor local) from the anchor's own ENU frame.
    double gfl[16];
    orthonormalize(a.georef.solution.global_from_local, gfl);
    se3::mat4_inverse_rigid(gfl, world_from_world_enu);
  }

  int epsg_seen = 0;
  for (MergeSession& s : impl_->sessions) {
    if (!usable(s)) {
      ++rep.skipped;
      continue;
    }
    if (s.georef.epsg != 0) {
      if (epsg_seen == 0) {
        epsg_seen = s.georef.epsg;
      } else if (epsg_seen != s.georef.epsg) {
        rep.epsg_mismatch = true;
      }
    }
    double m[16];
    const Status st = impl_->georef_world_from_session(s, world_enu, world_from_world_enu, m);
    if (!st.ok()) {
      ++rep.skipped;
      continue;
    }
    for (int i = 0; i < 16; ++i) s.world_from_session[i] = m[i];
    s.align = AlignSource::kGeoreferenced;
    ++rep.aligned;

    const double d[3] = {s.georef.enu.origin_ecef.x - world_enu.origin_ecef.x,
                         s.georef.enu.origin_ecef.y - world_enu.origin_ecef.y,
                         s.georef.enu.origin_ecef.z - world_enu.origin_ecef.z};
    const double sep = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (sep > rep.max_origin_separation_m) rep.max_origin_separation_m = sep;
  }

  if (!impl_->has_project_enu) {
    // Force the anchor to be exactly the identity: the composition above is
    // algebraically identity for it, and "exactly" beats "to 1e-16" for a
    // frame every other transform is expressed against.
    se3::mat4_identity(impl_->sessions[impl_->anchor_id].world_from_session);
    impl_->sessions[impl_->anchor_id].align = AlignSource::kGeoreferenced;
  }

  impl_->refresh_session_summaries();
  if (out != nullptr) *out = rep;
  if (rep.aligned == 0) {
    return set_last_error(ScanError::kNotFound, "merge: no session carries a converged georef");
  }
  return kOkStatus;
}

Status MergeProject::align_from_correspondences(std::uint32_t id,
                                                Span<const PointCorrespondence> picks,
                                                const CorrespondenceOptions& opts,
                                                CorrespondenceSolution* out) {
  if (!impl_->valid(id)) return set_last_error(ScanError::kNotFound, "merge: no session %u", id);
  const CorrespondenceSolution sol = solve_correspondences(picks, opts);
  if (out != nullptr) *out = sol;
  if (!sol.ok) {
    return set_last_error(ScanError::kInvalidArgument, "merge: 3-point alignment failed: %s",
                          sol.blocker);
  }
  if (std::fabs(sol.scale - 1.0) > 1e-6) {
    // The solve reports the scale (it is a genuinely useful diagnostic — it
    // says the picks disagree with a rigid model) but a merge transform is
    // rigid: everything downstream, including mat4_inverse_rigid and the pose
    // graph, assumes it.
    return set_last_error(ScanError::kNotSupported,
                          "merge: correspondence solve returned scale %.6f; merge transforms are "
                          "rigid",
                          sol.scale);
  }
  return set_alignment(id, sol.b_from_a, AlignSource::kManual);
}

Status MergeProject::set_alignment(std::uint32_t id, const double world_from_session[16],
                                   AlignSource source) {
  if (!impl_->valid(id)) return set_last_error(ScanError::kNotFound, "merge: no session %u", id);
  if (world_from_session == nullptr || !se3::mat4_is_rigid(world_from_session, 1e-5)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "merge: world_from_session for '%s' is not a rigid transform (a "
                          "column-major matrix from a UI is the usual cause)",
                          impl_->sessions[id].provenance_id.c_str());
  }
  MergeSession& s = impl_->sessions[id];
  for (int i = 0; i < 16; ++i) s.world_from_session[i] = world_from_session[i];
  s.align = source;
  impl_->refresh_session_summaries();
  return kOkStatus;
}

Status MergeProject::align_yaw_search(std::uint32_t id, std::uint32_t reference,
                                      const YawSearchConfig& cfg, YawSearchResult* out) {
  if (!impl_->valid(id) || !impl_->valid(reference)) {
    return set_last_error(ScanError::kNotFound, "merge: no session %u/%u", id, reference);
  }
  if (id == reference) return set_last_error(ScanError::kInvalidArgument, "merge: id == reference");
  const MergeSession& ref = impl_->sessions[reference];
  if (ref.align == AlignSource::kNone) {
    return set_last_error(ScanError::kInvalidState,
                          "merge: reference session '%s' is not aligned yet",
                          ref.provenance_id.c_str());
  }
  std::vector<PointVertex> src, tgt;
  downsample(impl_->sessions[id].cloud, cfg.work_voxel_m, 0, &src);
  downsample(ref.cloud, cfg.work_voxel_m, 0, &tgt);

  YawSearchConfig c = cfg;
  c.work_voxel_m = 0.0;  // already done, at the same pitch
  const YawSearchResult r = yaw_translation_search(Span<const PointVertex>(src.data(), src.size()),
                                                   Span<const PointVertex>(tgt.data(), tgt.size()),
                                                   c);
  if (out != nullptr) *out = r;
  if (!r.ok) {
    return set_last_error(ScanError::kNotFound,
                          "merge: yaw search for '%s' did not produce a trustworthy alignment: %s "
                          "(overlap %.2f, margin %.2f)",
                          impl_->sessions[id].provenance_id.c_str(), r.blocker, r.overlap,
                          r.margin);
  }
  double m[16];
  se3::mat4_mul(ref.world_from_session, r.b_from_a, m);
  return set_alignment(id, m, AlignSource::kYawSearch);
}

namespace {

// b_from_a from two world placements.
void relative_transform(const double world_from_a[16], const double world_from_b[16],
                        double b_from_a[16]) {
  double inv_b[16];
  se3::mat4_inverse_rigid(world_from_b, inv_b);
  se3::mat4_mul(inv_b, world_from_a, b_from_a);
}

}  // namespace

Status MergeProject::survey_overlap(const MergeIcpConfig& cfg, post::CancelToken* cancel) {
  impl_->refresh_session_summaries();
  impl_->report.pairs.clear();
  impl_->report.worst_overlap = 0.f;
  impl_->report.pairs_low_overlap = 0;
  impl_->report.pairs_refined = 0;
  impl_->report.pairs_converged = 0;
  const std::size_t n = impl_->sessions.size();
  if (n < 2) return kOkStatus;

  std::vector<std::vector<PointVertex>> ds(n);
  for (std::size_t i = 0; i < n; ++i) {
    downsample(impl_->sessions[i].cloud, cfg.source_voxel_m, cfg.max_source_points, &ds[i]);
  }
  float worst = 1.f;
  for (std::size_t a = 0; a < n; ++a) {
    for (std::size_t b = a + 1; b < n; ++b) {
      if (post::cancelled(cancel)) return ScanError::kCancelled;
      MergePair p;
      p.session_a = static_cast<std::uint32_t>(a);
      p.session_b = static_cast<std::uint32_t>(b);
      relative_transform(impl_->sessions[a].world_from_session,
                         impl_->sessions[b].world_from_session, p.b_from_a);
      const OverlapEstimate ov = estimate_overlap(
          Span<const PointVertex>(ds[a].data(), ds[a].size()),
          Span<const PointVertex>(ds[b].data(), ds[b].size()), p.b_from_a, cfg.overlap_voxel_m,
          cfg.overlap_samples);
      p.overlap_fraction = static_cast<float>(ov.symmetric);
      p.overlap_a_in_b = static_cast<float>(ov.a_in_b);
      p.overlap_b_in_a = static_cast<float>(ov.b_in_a);
      p.low_overlap = ov.symmetric < cfg.min_overlap;
      if (p.low_overlap) {
        p.blocker = "overlap below threshold";
        ++impl_->report.pairs_low_overlap;
      }
      if (p.overlap_fraction < worst) worst = p.overlap_fraction;
      impl_->report.pairs.push_back(p);
    }
  }
  impl_->report.worst_overlap = impl_->report.pairs.empty() ? 0.f : worst;
  return kOkStatus;
}

Status MergeProject::refine(const RefineConfig& cfg, post::CancelToken* cancel) {
  const std::size_t n = impl_->sessions.size();
  impl_->refresh_session_summaries();
  impl_->report.pairs.clear();
  impl_->report.pairs_refined = 0;
  impl_->report.pairs_converged = 0;
  impl_->report.pairs_low_overlap = 0;
  impl_->report.relaxed = false;
  impl_->report.graph = post::PoseGraphSummary{};
  impl_->report.worst_rms_m = 0.f;
  impl_->report.worst_overlap = 0.f;
  if (n < 2) {
    return set_last_error(ScanError::kInvalidState, "merge: refine needs at least two sessions");
  }

  // One downsample per session, reused by every pair it appears in.
  std::vector<std::vector<PointVertex>> ds(n);
  for (std::size_t i = 0; i < n; ++i) {
    downsample(impl_->sessions[i].cloud, cfg.icp.source_voxel_m, cfg.icp.max_source_points, &ds[i]);
  }
  MergeIcpConfig pair_cfg = cfg.icp;
  pair_cfg.source_voxel_m = 0.0;  // already downsampled
  pair_cfg.target_voxel_m = 0.0;
  pair_cfg.max_source_points = 0;

  float worst_rms = 0.f;
  float worst_overlap = 1.f;
  bool any_pair = false;

  for (std::size_t a = 0; a < n; ++a) {
    for (std::size_t b = a + 1; b < n; ++b) {
      if (post::cancelled(cancel)) return ScanError::kCancelled;
      MergePair p;
      p.session_a = static_cast<std::uint32_t>(a);
      p.session_b = static_cast<std::uint32_t>(b);
      const MergeSession& sa = impl_->sessions[a];
      const MergeSession& sb = impl_->sessions[b];
      if (sa.align == AlignSource::kNone || sb.align == AlignSource::kNone) {
        relative_transform(sa.world_from_session, sb.world_from_session, p.b_from_a);
        p.blocker = "a session in this pair has no coarse alignment";
        impl_->report.pairs.push_back(p);
        continue;
      }
      double init[16];
      relative_transform(sa.world_from_session, sb.world_from_session, init);

      const PairIcpResult r =
          refine_pair(Span<const PointVertex>(ds[a].data(), ds[a].size()),
                      Span<const PointVertex>(ds[b].data(), ds[b].size()), init, pair_cfg, cancel);
      for (int i = 0; i < 16; ++i) p.b_from_a[i] = r.b_from_a[i];
      p.rms_residual_m = static_cast<float>(r.rms_after_m);
      p.rms_before_m = static_cast<float>(r.rms_before_m);
      p.overlap_fraction = static_cast<float>(r.overlap.symmetric);
      p.overlap_a_in_b = static_cast<float>(r.overlap.a_in_b);
      p.overlap_b_in_a = static_cast<float>(r.overlap.b_in_a);
      p.converged = r.converged;
      p.iterations = r.iterations;
      p.rejected_steps = r.rejected_steps;
      p.inliers = r.inliers;
      p.inlier_ratio = static_cast<float>(r.inlier_ratio);
      p.refined = r.refined;
      p.low_overlap = r.low_overlap;
      p.blocker = r.blocker;
      if (r.low_overlap) ++impl_->report.pairs_low_overlap;
      if (r.refined) {
        ++impl_->report.pairs_refined;
        any_pair = true;
        if (p.rms_residual_m > worst_rms) worst_rms = p.rms_residual_m;
        if (p.overlap_fraction < worst_overlap) worst_overlap = p.overlap_fraction;
      }
      if (r.converged) ++impl_->report.pairs_converged;
      impl_->report.pairs.push_back(p);
    }
  }
  impl_->report.worst_rms_m = worst_rms;
  impl_->report.worst_overlap = any_pair ? worst_overlap : 0.f;

  if (!any_pair) {
    return set_last_error(ScanError::kNotFound,
                          "merge: no pair had enough overlap to refine (worst gate: %.2f)",
                          static_cast<double>(cfg.icp.min_overlap));
  }

  // --- apply ---------------------------------------------------------------
  const bool relax = cfg.global_relaxation && n > 2;
  if (!relax) {
    // Spanning tree out of the anchor, pairs visited in index order. For two
    // sessions this is simply "put b where the ICP says b is".
    std::vector<bool> placed(n, false);
    placed[impl_->anchor_id] = true;
    bool progress = true;
    while (progress) {
      progress = false;
      for (const MergePair& p : impl_->report.pairs) {
        if (!p.refined || !p.converged) continue;
        const std::uint32_t a = p.session_a, b = p.session_b;
        if (placed[a] == placed[b]) continue;
        double m[16];
        if (placed[a]) {
          // world_from_b = world_from_a * a_from_b
          double a_from_b[16];
          se3::mat4_inverse_rigid(p.b_from_a, a_from_b);
          se3::mat4_mul(impl_->sessions[a].world_from_session, a_from_b, m);
          for (int i = 0; i < 16; ++i) impl_->sessions[b].world_from_session[i] = m[i];
          impl_->sessions[b].align = AlignSource::kIcp;
          placed[b] = true;
        } else {
          se3::mat4_mul(impl_->sessions[b].world_from_session, p.b_from_a, m);
          for (int i = 0; i < 16; ++i) impl_->sessions[a].world_from_session[i] = m[i];
          impl_->sessions[a].align = AlignSource::kIcp;
          placed[a] = true;
        }
        progress = true;
      }
    }
    impl_->refresh_session_summaries();
    return kOkStatus;
  }

  // --- global relaxation over the session nodes ----------------------------
  post::PoseGraph graph;
  for (std::size_t i = 0; i < n; ++i) {
    double q[4], p[3];
    quat_pos_of(impl_->sessions[i].world_from_session, q, p);
    graph.add_node(q, p);
  }
  SCAN_TRY(graph.set_fixed(impl_->anchor_id, true));

  for (MergePair& p : impl_->report.pairs) {
    if (!p.refined || !p.converged) continue;
    // pose_graph.h: z = T_i^-1 T_j, so i = b and j = a gives exactly b_from_a.
    double q[4], pos[3];
    quat_pos_of(p.b_from_a, q, pos);
    const double sigma_t = std::max<double>(cfg.icp_sigma_trans_m, p.rms_residual_m);
    SCAN_TRY(graph.add_between(p.session_b, p.session_a, q, pos,
                               cfg.icp_sigma_rot_deg * se3::kDegToRad, sigma_t,
                               cfg.icp_huber_sigmas, true));
    p.in_graph = true;
  }

  if (cfg.georef_position_priors) {
    // The A10 seam (pose_graph.h): a unary factor on POSITION only, weighted
    // by that session's own reported horizontal sigma. It is what stops a
    // chain of pairwise ICPs from sliding a long merge off its survey control
    // — and it says nothing about heading, which is correct: a GNSS fix does
    // not observe one.
    crs::EnuFrame world_enu{};
    double world_from_world_enu[16];
    se3::mat4_identity(world_from_world_enu);
    bool have_world = false;
    if (impl_->has_project_enu) {
      world_enu = impl_->project_enu;
      have_world = true;
    } else {
      const MergeSession& a = impl_->sessions[impl_->anchor_id];
      if (a.georef.valid && a.georef.solution.converged && a.georef.enu.valid) {
        world_enu = a.georef.enu;
        double gfl[16];
        orthonormalize(a.georef.solution.global_from_local, gfl);
        se3::mat4_inverse_rigid(gfl, world_from_world_enu);
        have_world = true;
      }
    }
    if (have_world) {
      for (const MergeSession& s : impl_->sessions) {
        if (!s.georef.valid || !s.georef.solution.converged || !s.georef.enu.valid) continue;
        if (s.id == impl_->anchor_id) continue;  // fixed node: a prior on it is a no-op
        double m[16];
        if (!impl_->georef_world_from_session(s, world_enu, world_from_world_enu, m).ok()) continue;
        const double xyz[3] = {m[3], m[7], m[11]};
        const double sigma =
            std::max(0.005, s.georef.solution.horizontal_sigma_m > 0.0
                                ? s.georef.solution.horizontal_sigma_m
                                : 0.05);
        SCAN_TRY(graph.add_position_prior(s.id, xyz, sigma, cfg.georef_prior_huber_sigmas));
      }
    }
  }

  const Result<post::PoseGraphSummary> sum = graph.optimize(cfg.graph, cancel);
  if (!sum.ok()) return sum.status();
  impl_->report.graph = sum.value();
  impl_->report.relaxed = true;

  for (std::size_t i = 0; i < n; ++i) {
    const post::PoseNode& nd = graph.node(static_cast<std::uint32_t>(i));
    double m[16];
    se3::mat4_from_quat_pos(nd.q, nd.p, m);
    for (int k = 0; k < 16; ++k) impl_->sessions[i].world_from_session[k] = m[k];
    if (impl_->sessions[i].id != impl_->anchor_id) impl_->sessions[i].align = AlignSource::kRelaxed;
  }
  impl_->refresh_session_summaries();
  return kOkStatus;
}

Status MergeProject::build(const MergeOutputConfig& cfg, MergeResult* out,
                           post::CancelToken* cancel) {
  if (out == nullptr) return ScanError::kInvalidArgument;
  out->cloud.clear();
  out->ranges.clear();
  out->pages.clear();
  if (impl_->sessions.empty()) {
    return set_last_error(ScanError::kNotFound, "merge: nothing to build");
  }

  // Session order = the priority rule. It is also the order of the runs in
  // the merged cloud, which is what makes provenance a range table.
  std::vector<std::uint32_t> order;
  order.reserve(impl_->sessions.size());
  for (const MergeSession& s : impl_->sessions) order.push_back(s.id);
  const bool georef_first = cfg.priority == MergePriority::kGeoreferencedFirst;
  std::stable_sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) {
    const MergeSession& a = impl_->sessions[x];
    const MergeSession& b = impl_->sessions[y];
    if (georef_first) {
      const int ga = (a.georef.valid && a.georef.solution.converged) ? 0 : 1;
      const int gb = (b.georef.valid && b.georef.solution.converged) ? 0 : 1;
      if (ga != gb) return ga < gb;
    }
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.id < b.id;
  });

  const double cross = cfg.cross_session_voxel_m > 0.0 ? cfg.cross_session_voxel_m
                                                       : cfg.dedup_voxel_m;
  detail::VoxelClaimMap claims(cross);
  const bool claim_enabled = claims.valid();

  impl_->refresh_session_summaries();
  impl_->report.merged_points = 0;
  impl_->report.dedup_dropped_points = 0;
  impl_->report.priority_dropped_points = 0;

  bool first_point = true;
  for (std::uint32_t sid : order) {
    if (post::cancelled(cancel)) return ScanError::kCancelled;
    MergeSession& s = impl_->sessions[sid];
    if (cfg.require_alignment && s.align == AlignSource::kNone) continue;

    // World-frame voxel dedup WITHIN the session, on the same lattice the
    // cross-session claim uses, so the two agree.
    std::uint64_t in_points = 0;
    std::vector<PointVertex> kept;
    if (cfg.dedup_voxel_m > 0.0) {
      post::VoxelAccumulator acc(cfg.dedup_voxel_m, cfg.average_within_session);
      for (const auto& chunk : s.cloud.chunks) {
        if (post::cancelled(cancel)) return ScanError::kCancelled;
        for (std::size_t i = 0; i < chunk.size(); ++i) {
          const PointVertex& v = chunk[i];
          const double p[3] = {v.x, v.y, v.z};
          double w[3];
          se3::mat4_apply(s.world_from_session, p, w);
          acc.add(w[0], w[1], w[2], v.r, v.g, v.b, v.a);
          ++in_points;
        }
      }
      acc.extract(&kept);
    } else {
      for (const auto& chunk : s.cloud.chunks) {
        if (post::cancelled(cancel)) return ScanError::kCancelled;
        for (std::size_t i = 0; i < chunk.size(); ++i) {
          const PointVertex& v = chunk[i];
          const double p[3] = {v.x, v.y, v.z};
          double w[3];
          se3::mat4_apply(s.world_from_session, p, w);
          PointVertex o = v;
          o.x = static_cast<float>(w[0]);
          o.y = static_cast<float>(w[1]);
          o.z = static_cast<float>(w[2]);
          kept.push_back(o);
          ++in_points;
        }
      }
    }
    impl_->report.dedup_dropped_points += in_points - kept.size();

    const std::uint64_t first = out->cloud.size();
    std::uint64_t dropped = 0;
    for (const PointVertex& v : kept) {
      if (claim_enabled && !claims.claim(v.x, v.y, v.z, sid)) {
        ++dropped;
        continue;
      }
      out->cloud.push_back(v);
      const double c[3] = {v.x, v.y, v.z};
      for (int k = 0; k < 3; ++k) {
        if (first_point || c[k] < out->bounds_min[k]) out->bounds_min[k] = c[k];
        if (first_point || c[k] > out->bounds_max[k]) out->bounds_max[k] = c[k];
      }
      first_point = false;
    }
    impl_->report.priority_dropped_points += dropped;

    SessionRange range;
    range.session = sid;
    range.first = first;
    range.count = out->cloud.size() - first;
    if (range.count > 0) out->ranges.push_back(range);

    for (SessionSummary& sum : impl_->report.sessions) {
      if (sum.id != sid) continue;
      sum.kept_points = range.count;
      sum.dropped_duplicate_points = dropped;
      sum.first_point = range.first;
      sum.point_count = range.count;
    }
  }
  impl_->report.merged_points = out->cloud.size();
  return kOkStatus;
}

namespace {

struct PublishCapture {
  std::vector<PageProvenance>* out = nullptr;
  std::uint32_t session = 0;
};

void on_page_update(const PageUpdate& u, void* user_data) {
  PublishCapture* cap = static_cast<PublishCapture*>(user_data);
  if (cap == nullptr || cap->out == nullptr) return;
  PageProvenance p;
  p.page = u.page;
  p.session = cap->session;
  p.first = u.first;
  p.count = u.count;
  cap->out->push_back(p);
}

}  // namespace

Status MergeProject::publish(MergeResult* result, PageStore* store, StreamId stream) {
  if (store == nullptr || result == nullptr) return ScanError::kInvalidArgument;
  result->pages.clear();

  PublishCapture cap;
  cap.out = &result->pages;
  const PageSubscriptionId sub = store->subscribe(&on_page_update, &cap);

  Status st = kOkStatus;
  for (const SessionRange& r : result->ranges) {
    cap.session = r.session;
    const PointVertex* base = result->cloud.data() + r.first;
    std::uint32_t appended = 0;
    const Status s =
        store->append(stream, Span<const PointVertex>(base, static_cast<std::size_t>(r.count)), 0,
                      &appended);
    if (!s.ok() || appended != r.count) {
      st = s.ok() ? Status(ScanError::kCapacityExceeded) : s;
      break;
    }
  }
  (void)store->unsubscribe(sub);

  // Page accounting: one entry per (page, session). A page carrying two
  // sessions is a session boundary that fell inside it — PageStore has no
  // page-break API (docs/A13-merge.md §7).
  std::vector<PageId> seen;
  std::uint32_t shared = 0;
  for (std::size_t i = 0; i < result->pages.size(); ++i) {
    const PageId id = result->pages[i].page;
    bool first_time = true;
    for (std::size_t j = 0; j < i; ++j) {
      if (result->pages[j].page == id) {
        first_time = false;
        if (result->pages[j].session != result->pages[i].session) ++shared;
        break;
      }
    }
    if (first_time) seen.push_back(id);
  }
  impl_->report.pages_appended = static_cast<std::uint32_t>(seen.size());
  impl_->report.pages_shared = shared;
  return st;
}

Status MergeProject::keyframes_in_world(std::uint32_t id, std::vector<SessionKeyframe>* out) const {
  if (out == nullptr) return ScanError::kInvalidArgument;
  if (!impl_->valid(id)) return set_last_error(ScanError::kNotFound, "merge: no session %u", id);
  const MergeSession& s = impl_->sessions[id];
  out->clear();
  out->reserve(s.keyframes.size());
  for (const SessionKeyframe& kf : s.keyframes) {
    double local[16], world[16];
    se3::mat4_from_quat_pos(kf.q, kf.p, local);
    se3::mat4_mul(s.world_from_session, local, world);
    SessionKeyframe w;
    w.t_ns = kf.t_ns;
    quat_pos_of(world, w.q, w.p);
    out->push_back(w);
  }
  return kOkStatus;
}

const MergeReport& MergeProject::report() const { return impl_->report; }

}  // namespace merge
}  // namespace scanengine
