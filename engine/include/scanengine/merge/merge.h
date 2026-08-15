// merge.h — multi-session merge (§3.10).
//
// Owner: A13 (depends on A7 and A10).
//
//   "Coarse: automatic when sessions are georeferenced (shared CRS);
//    otherwise manual 3-point / drag alignment.
//    Refine: voxel-downsampled point-to-plane ICP per pair; optional global
//    relaxation for >2 sessions.
//    Merge: unified cloud with per-session provenance, voxel dedup, combined
//    export."
//
// The pieces live next door and are included from here, which is the header
// an app should include:
//
//   merge/session.h   what a session is: a NON-OWNING cloud (spans over A7's
//                     final cloud or over PageStore pages), its keyframes,
//                     its A10 GeorefSolution *and the ENU frame that solution
//                     is expressed in*, and its provenance id.
//   merge/align.h     the three coarse paths and their evidence.
//   merge/icp.h       pairwise point-to-plane refinement (A7's kernel with the
//                     outer loop unrolled so the residual trace survives) and
//                     the overlap gate.
//
// ### Per-point provenance, given that PointVertex has no free channel
//
// §3.10 requires the merged cloud to keep per-session provenance and
// cloud/point_page.h has no channel to put it in — `PointVertex` is 16 bytes
// of position + RGBA8 and DESIGN.md §5 forbids changing it (the S3-proven GPU
// layout). A13 therefore does not store a per-point id at all. It stores a
// per-point-RANGE one:
//
//   * The merged cloud is emitted as one CONTIGUOUS RUN PER SESSION, in
//     session-priority order, and `MergeResult::ranges` is the run table.
//     `MergeResult::session_at(i)` is a binary search over at most N entries.
//     That is per-point provenance, exactly, in O(N) memory instead of
//     O(points) — and voxel dedup is defined so that a merged point belongs to
//     exactly one session (see MergeOutputConfig::priority), which is what
//     makes the run table lossless rather than a summary.
//   * Published into a `PageStore`, each run becomes a run of pages.
//     DESIGN.md §5 says pages are single-STREAM so that "provenance survives
//     into merged exports (A13)"; the natural extension is single-SESSION
//     pages, and `MergeResult::pages` reports the page->session mapping. It is
//     one entry per page for every page except the ones a session boundary
//     falls inside: `PageStore` has no page-break API, so at most (N-1) pages
//     are shared between two sessions and carry two entries.
//     `MergeReport::pages_shared` counts them, and docs/A13-merge.md §7 names
//     the one-line PageStore seam that would take it to zero.
//
// ### What is NOT here
//
// `SessionMerger::add_session(lscan_dir)` — the A1 seam below — cannot be
// implemented today: nothing writes a processed cloud into a `.lscan`
// (`stream_of(ChunkType::kPointsXyzRgba)` returns `kUnknown`, docs/A7-post.md
// §8 item 2), so there is no cloud in `processed/` to read. The in-memory
// path is the supported one and is what the merge workbench (C6) drives.
#ifndef SCANENGINE_MERGE_MERGE_H
#define SCANENGINE_MERGE_MERGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/types.h"
#include "scanengine/merge/align.h"
#include "scanengine/merge/icp.h"
#include "scanengine/merge/session.h"
#include "scanengine/slam/post/pose_graph.h"
#include "scanengine/slam/post/progress.h"

namespace scanengine {
namespace merge {

// --- the residual report (this is C6's surface) -----------------------------

// One pair of sessions. The first six fields are the A1 seam, unchanged in
// name, order and meaning; everything after them is additive.
struct MergePair {
  std::uint32_t session_a = 0;
  std::uint32_t session_b = 0;
  double b_from_a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  float rms_residual_m = 0.f;   // after refinement
  float overlap_fraction = 0.f; // symmetric (the conservative one)
  bool converged = false;

  // --- A13 additions ---
  float rms_before_m = 0.f;       // before refinement, same correspondences
  float overlap_a_in_b = 0.f;
  float overlap_b_in_a = 0.f;
  std::uint32_t iterations = 0;
  std::uint32_t rejected_steps = 0;
  std::uint64_t inliers = 0;
  float inlier_ratio = 0.f;
  bool refined = false;      // ICP ran
  bool low_overlap = false;  // the gate fired: reported, NOT merged
  bool in_graph = false;     // became an edge of the global relaxation
  const char* blocker = "";  // stable string; empty when nothing went wrong
};

struct SessionSummary {
  std::uint32_t id = 0;
  std::string provenance_id;
  AlignSource align = AlignSource::kNone;
  bool anchor = false;
  bool georeferenced = false;
  double georef_horizontal_sigma_m = 0.0;
  double world_from_session[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  std::uint64_t input_points = 0;
  std::uint64_t kept_points = 0;             // survived dedup + priority
  std::uint64_t dropped_duplicate_points = 0;  // lost a voxel to a higher-priority session
  std::uint32_t keyframes = 0;
  // Where this session's run sits in the merged cloud.
  std::uint64_t first_point = 0;
  std::uint64_t point_count = 0;
};

struct MergeReport {
  std::vector<MergePair> pairs;
  float worst_rms_m = 0.f;

  // --- A13 additions ---
  std::vector<SessionSummary> sessions;
  float worst_overlap = 0.f;
  std::uint32_t pairs_refined = 0;
  std::uint32_t pairs_converged = 0;
  std::uint32_t pairs_low_overlap = 0;
  bool relaxed = false;
  post::PoseGraphSummary graph{};

  std::uint64_t input_points = 0;
  std::uint64_t merged_points = 0;
  std::uint64_t dedup_dropped_points = 0;    // within a session
  std::uint64_t priority_dropped_points = 0; // across sessions
  std::uint32_t pages_appended = 0;
  std::uint32_t pages_shared = 0;  // pages a session boundary falls inside

  const MergePair* pair(std::uint32_t a, std::uint32_t b) const;
};

// --- the merged cloud -------------------------------------------------------

struct SessionRange {
  std::uint32_t session = 0;
  std::uint64_t first = 0;
  std::uint64_t count = 0;
};

struct PageProvenance {
  PageId page = kInvalidPageId;
  std::uint32_t session = 0;
  std::uint32_t first = 0;  // index within the page
  std::uint32_t count = 0;
};

struct MergeResult {
  std::vector<PointVertex> cloud;    // in the merged (anchor) frame
  std::vector<SessionRange> ranges;  // contiguous, ascending, gap-free
  std::vector<PageProvenance> pages; // filled by publish()
  double bounds_min[3] = {0.0, 0.0, 0.0};
  double bounds_max[3] = {0.0, 0.0, 0.0};

  // Per-point provenance. Returns 0xFFFFFFFF for an out-of-range index.
  std::uint32_t session_at(std::uint64_t point_index) const;
};

// --- configuration ----------------------------------------------------------

enum class MergePriority : std::uint8_t {
  // Lower `SessionInput::priority` wins a contested voxel, ties on session id.
  kSessionPriority = 0,
  // Same, but any georeferenced session outranks any non-georeferenced one
  // first. The default: a session tied to the CRS is the one whose
  // coordinates a survey deliverable should quote.
  kGeoreferencedFirst = 1,
};

struct MergeOutputConfig {
  // §3.10's "voxel dedup". Applied WITHIN each session first (A7's
  // `voxel_downsample`, which averages and is insertion-ordered), then ACROSS
  // sessions by the priority rule below.
  double dedup_voxel_m = 0.03;
  // Average a voxel's members within a session (A7's default) or keep the
  // first point, which leaves every output coordinate a value the sensor
  // actually measured. A survey deliverable usually wants the second.
  bool average_within_session = true;
  MergePriority priority = MergePriority::kGeoreferencedFirst;
  // Cross-session voxel pitch. 0 = same as dedup_voxel_m. Coarser here erodes
  // the seam between two sessions less visibly but throws away more of the
  // lower-priority session.
  double cross_session_voxel_m = 0.0;
  // Skip sessions whose alignment is still AlignSource::kNone rather than
  // merging them at the identity. Default true: an unaligned session dumped
  // at the origin is the worst possible failure mode — it looks like data.
  bool require_alignment = true;
};

struct RefineConfig {
  MergeIcpConfig icp{};
  // Refine every pair whose coarse overlap clears MergeIcpConfig::min_overlap.
  bool refine_all_pairs = true;
  // With more than two sessions, run A7's pose graph over the session nodes
  // once every pair has been refined. Below three sessions it is a no-op (a
  // two-node graph with one edge has a closed-form answer, which is the ICP
  // result itself).
  bool global_relaxation = true;
  // Edge weights for that graph. The translation sigma is
  // max(icp_sigma_trans_m, pair RMS), the same rule A7 uses for loop edges.
  double icp_sigma_rot_deg = 0.5;
  double icp_sigma_trans_m = 0.02;
  double icp_huber_sigmas = 2.0;
  // Add A10's answer as a unary position prior on each georeferenced session
  // node, weighted by that session's own reported horizontal sigma — the seam
  // pose_graph.h calls "THE A10 SEAM". Off makes the relaxation purely
  // geometric, which is what you want when one session's RTK is suspect.
  bool georef_position_priors = true;
  double georef_prior_huber_sigmas = 2.0;
  post::PoseGraphOptions graph{};
};

// --- the project ------------------------------------------------------------

// N sessions, their pairwise transforms and their global ones.
//
// Threading: not thread-safe; one project, one thread. Long operations poll a
// `post::CancelToken` (A15 owns the token, A7's convention).
//
// LIFETIME: a session's cloud is a list of spans the caller owns. They must
// stay valid until the project is destroyed. `build()` is the only method
// that copies points, and it copies only the survivors.
class MergeProject {
 public:
  MergeProject();
  ~MergeProject();
  MergeProject(const MergeProject&) = delete;
  MergeProject& operator=(const MergeProject&) = delete;

  // --- sessions ---
  Result<std::uint32_t> add_session(const SessionInput& in);
  std::size_t session_count() const;
  const MergeSession& session(std::uint32_t id) const;
  const std::vector<MergeSession>& sessions() const;
  // -1 when no session carries that provenance id.
  int find(const std::string& provenance_id) const;

  // Removes a session from the project (desktop NOTES §11.8: "'Remove
  // selected' is a message box, not a real removal … a future desktop pass
  // that wants this needs an engine-side seam"). kNotFound for an unknown id.
  //
  // IDS ARE INDICES, AND THEY RENUMBER. `session(id)` is a direct index into
  // the session vector and `MergeSession::id` is that index, so removing
  // session k decrements the id of every session after it. This is stated
  // rather than avoided: the alternative — leaving a hole — would make
  // `session(id)` return a tombstone every caller would have to test for, and
  // `sessions()` a vector with gaps, which is a worse contract than "re-read
  // the list after a removal". Provenance ids do NOT change, so a UI that keys
  // its rows on `provenance_id` (and re-reads `find()`) needs no remapping at
  // all — which is the recommended way to hold a reference across a removal.
  //
  // THE PAIR REPORT IS INVALIDATED. `report().pairs` names sessions by id and
  // every pair involving the removed session is meaningless, so the pairs, the
  // graph summary and the aggregate counters are CLEARED; the per-session
  // summaries are rebuilt. Call refine()/survey_overlap() again for a fresh
  // report — the same thing those methods do anyway ("the report is rebuilt
  // from scratch").
  //
  // THE ANCHOR SURVIVES. Removing a non-anchor session leaves every alignment
  // untouched (the merged frame is unchanged). Removing the ANCHOR moves it to
  // what is then session 0 and rebases every remaining alignment onto it, so
  // the relative geometry the operator already established is preserved and
  // only the origin moves. Removing the last session leaves an empty project.
  Status remove_session(std::uint32_t id);

  Status set_anchor(std::uint32_t id);
  std::uint32_t anchor() const;

  // The frame merged coordinates are expressed in. Default: the anchor
  // session's own local frame, which keeps coordinates small — float32
  // positions hold ~1 mm at 8 km from the origin, so a merged frame pinned to
  // a UTM easting would quantize the deliverable (the same reason
  // gnss/georef.h hands out doubles).
  Status set_project_enu_frame(const crs::EnuFrame& frame);
  bool has_project_enu_frame() const;
  const crs::EnuFrame& project_enu_frame() const;

  // --- coarse alignment ---
  struct GeorefAlignReport {
    std::uint32_t aligned = 0;
    std::uint32_t skipped = 0;
    std::uint32_t reference = 0;      // the session whose ENU frame was used
    bool reference_is_project = false;
    double max_origin_separation_m = 0.0;  // between the ENU frames composed
    bool epsg_mismatch = false;
    const char* blocker = "";
  };
  // Every session with a converged `SessionGeoref` is placed by composing
  // through the shared CRS (align.h::enu_from_enu). Sessions without one are
  // left alone. kNotFound when none is georeferenced.
  Status align_georeferenced(GeorefAlignReport* out = nullptr);

  // 3-point (or more) manual alignment. `picks[i].a` is in the session's own
  // frame, `picks[i].b` in the MERGED frame — which is the anchor session's
  // frame, so a workbench that lets an operator click a point in the already
  // placed cloud is handing back exactly that.
  //
  // A solution whose scale is not 1 (only possible with
  // CorrespondenceOptions::allow_scale) is REPORTED but not applied:
  // kNotSupported, because every transform downstream of here — the pose
  // graph, mat4_inverse_rigid, the ICP init — is rigid. The scale is still
  // the diagnostic worth showing an operator, so `out` carries it either way.
  Status align_from_correspondences(std::uint32_t id, Span<const PointCorrespondence> picks,
                                    const CorrespondenceOptions& opts = {},
                                    CorrespondenceSolution* out = nullptr);
  // The "drag" half of §3.10: a matrix straight from the UI.
  Status set_alignment(std::uint32_t id, const double world_from_session[16],
                       AlignSource source = AlignSource::kManual);

  // The fallback. Places `id` against `reference` (which must already be
  // aligned) and applies the result only when it clears the search's own
  // gates; an ambiguous or low-overlap search leaves the session untouched
  // and returns kNotFound so the UI has to ask.
  Status align_yaw_search(std::uint32_t id, std::uint32_t reference,
                          const YawSearchConfig& cfg = {}, YawSearchResult* out = nullptr);

  // --- refinement ---
  // Pairwise ICP over every pair with enough overlap, then (optionally) the
  // pose graph over the session nodes. The report is rebuilt from scratch.
  Status refine(const RefineConfig& cfg = {}, post::CancelToken* cancel = nullptr);

  // Overlap only — what a workbench shows before anybody presses "align".
  // Uses the current alignments; does not modify them.
  Status survey_overlap(const MergeIcpConfig& cfg = {}, post::CancelToken* cancel = nullptr);

  // --- output ---
  Status build(const MergeOutputConfig& cfg, MergeResult* out,
               post::CancelToken* cancel = nullptr);
  // Appends `result->cloud` into `store`, one contiguous run per session, and
  // fills `result->pages` with the page->session mapping the appends actually
  // produced (captured from the store's own subscriber callback, so it is
  // observed rather than predicted). The store's pages are shared with
  // whatever else is in it, so a merged product normally gets its own store.
  Status publish(MergeResult* result, PageStore* store, StreamId stream = StreamId::kSlamMap);

  // Every session's keyframes, transformed into the merged frame — the
  // trajectory overlay of §3.9.
  Status keyframes_in_world(std::uint32_t id, std::vector<SessionKeyframe>* out) const;

  const MergeReport& report() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merge

// The A1 seam names, kept at namespace scope so existing includes and the
// header self-containment test still see them.
using merge::MergePair;
using merge::MergeReport;

// The interface A1 declared for this module. NOT implemented: `lscan_dir`
// cannot be a cloud source until a processed cloud can be written into a
// `.lscan` (docs/A7-post.md §8 item 2, docs/A13-merge.md §2). `MergeProject`
// above is the implemented API and covers every method here plus the
// evidence each step produced.
class SessionMerger {
 public:
  virtual ~SessionMerger() = default;
  virtual Status add_session(const std::string& lscan_dir, std::uint32_t* session_id) = 0;
  virtual Status coarse_align_georeferenced() = 0;
  virtual Status set_manual_alignment(std::uint32_t session, const double transform[16]) = 0;
  virtual Status refine_icp(float voxel_size_m) = 0;
  virtual Status build_merged(PageStore* out, MergeReport* report) = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_MERGE_MERGE_H
