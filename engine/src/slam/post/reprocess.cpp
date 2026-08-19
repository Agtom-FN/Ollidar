// reprocess.cpp — ROUND 13. See reprocess.h for why the corrected cloud is a
// new file rather than an overwrite.

#include "scanengine/slam/post/reprocess.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

#include "scanengine/cloud/page_store.h"
#include "scanengine/core/log.h"
#include "scanengine/record/lscan.h"
#include "scanengine/slam/post/d6_resolve.h"

namespace scanengine {
namespace post {
namespace {

constexpr const char* kMod = "reprocess";

std::string join(const std::string& dir, const char* leaf) {
  if (dir.empty()) return leaf;
  const char last = dir[dir.size() - 1];
  return (last == '/' || last == '\\') ? dir + leaf : dir + "/" + leaf;
}

}  // namespace

// The corrected cloud, in the container's own chunk framing, so anything that
// can read `streams/map.bin` can read this with the same code.
Status write_point_chunk_file(const std::string& path, const std::vector<PointVertex>& pts,
                              std::int64_t t_first_ns) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    return set_last_error(ScanError::kFileError, "reprocess: cannot create '%s'", path.c_str());
  }
  lscan::StreamFileHeader sh;
  sh.stream = StreamId::kSlamMap;
  sh.t_start_mono_ns = t_first_ns;
  std::uint8_t hdr[lscan::kStreamHeaderBytes];
  lscan::encode_stream_header(sh, hdr);
  if (std::fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
    std::fclose(f);
    return set_last_error(ScanError::kFileError, "reprocess: short write on '%s'", path.c_str());
  }

  // Same batching the recorder uses, so a reader's per-chunk costs are the
  // ones it was written for.
  constexpr std::size_t kPerChunk = 4096;
  for (std::size_t i = 0; i < pts.size(); i += kPerChunk) {
    const std::size_t n = std::min(kPerChunk, pts.size() - i);
    lscan::ChunkHeader ch{};
    ch.payload_len = static_cast<std::uint32_t>(n * lscan::kPointVertexBytes);
    ch.type = lscan::ChunkType::kPointsXyzRgba;
    ch.flags = lscan::kFlagNone;
    ch.t_mono_ns = t_first_ns;
    std::uint8_t chdr[lscan::kChunkHeaderBytes];
    lscan::encode_chunk_header(ch, chdr);
    const ByteSpan payload(reinterpret_cast<const std::uint8_t*>(pts.data() + i), ch.payload_len);
    const std::uint32_t crc = lscan::chunk_crc(ch, payload);
    std::uint8_t trailer[lscan::kChunkTrailerBytes];
    trailer[0] = static_cast<std::uint8_t>(crc & 0xFFu);
    trailer[1] = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);
    trailer[2] = static_cast<std::uint8_t>((crc >> 16) & 0xFFu);
    trailer[3] = static_cast<std::uint8_t>((crc >> 24) & 0xFFu);
    const bool ok = std::fwrite(chdr, 1, sizeof(chdr), f) == sizeof(chdr) &&
                    std::fwrite(payload.data(), 1, ch.payload_len, f) == ch.payload_len &&
                    std::fwrite(trailer, 1, sizeof(trailer), f) == sizeof(trailer);
    if (!ok) {
      std::fclose(f);
      return set_last_error(ScanError::kFileError, "reprocess: short write on '%s'", path.c_str());
    }
  }
  if (std::fclose(f) != 0) {
    return set_last_error(ScanError::kFileError, "reprocess: could not close '%s'", path.c_str());
  }
  return kOkStatus;
}

namespace {

// ROUND 16 item 59 — the corrected trajectory, as a derived product the phone
// can read without a JNI call and without a second resolve.
//
// > *"i want to see the path of mine showing in the pointcloud too for me to
// >  check if the scan is right"* — owner, on 0.9.0.
//
// The trajectory this pass produces is ALREADY the corrected one: `d6_resolve`
// applies the section-stitch correction and the loop-end correction to
// `*out_trajectory` as well as to the cloud, precisely so that "anything
// downstream sees one frame". Until this round it was then thrown away at the
// end of `reprocess_d6_container`, and the app had no way to obtain a
// trajectory at all — `ProjectProbe.hasPoses` is one bit, and
// `scan_reprocess_result` carries only counts.
//
// Writing it beside `map_stitched.bin` rather than adding a C ABI entry point
// is the smaller change AND the more honest one: the corrected cloud is
// already a file in `processed/`, this is the trajectory that goes with THAT
// cloud, and the two are only ever consistent because they were written by the
// same pass. A getter would have had to re-derive it and could disagree.
//
// The format is deliberately the dullest thing that works: an 8-byte magic, a
// little-endian u32 count, a u32 of zero reserved, then `count` records. No
// CRC — this is a derived file whose only reader checks the magic and the
// count against the file length, and deleting it costs nothing but the line
// on screen.
//
// ROUND 18 item 70 — "LSTRAJ02": each record is now 16 bytes, three
// little-endian float32 metres plus a u32 of flags. The owner's headline
// ("the path record seems not so accurate") is in large part poses this file
// carried WITHOUT their own verdicts: during his 6-7 s tracking losses ARCore
// freezes (round 17 measured 181 consecutive poses of 0.000 m / 0.00 deg in
// scan-040) and those frozen poses, plus the teleport at re-acquisition, drew
// as an ordinary walked line in Review and on the floor plan. The pose stream
// knows which poses the tracker disowned; this file just never said.
//
//   bit 0  UNTRACKED — the tracker disowned this pose (tracking_lost, or
//          quality 0). Its position is a held guess, not a measurement.
//   bit 1  JUMP_IN — the SEGMENT from the previous record to this one is not
//          a walked path: the stream was blind across it (> 150 ms between
//          poses, i.e. more than four dropped ARCore frames), or it implies
//          more than PoseSectionTracker's 6 m/s, or it is the re-acquisition
//          step out of an untracked run. Renderers draw it as a gap or a
//          flagged bridge, never as a line the operator supposedly walked.
//
// Readers that only knew "LSTRAJ01" refuse the new magic and show no path
// (the round-16 behaviour for a missing file) rather than misreading 16-byte
// records as 12-byte ones; both in-tree readers (`TrajectoryFile.kt`,
// `lscan_plan.cpp`) accept both versions.
inline constexpr std::int64_t kTrajBlindGapNs = 150'000'000;
inline constexpr double kTrajMaxWalkSpeedMps = 6.0;  // PoseSectionTracker.MAX_SPEED_MPS

bool traj_pose_untracked(const TrajPose& p) { return p.tracking_lost != 0 || p.quality == 0; }

std::uint32_t traj_flags(const std::vector<TrajPose>& traj, std::size_t i) {
  std::uint32_t flags = 0;
  if (traj_pose_untracked(traj[i])) flags |= 1u;
  if (i > 0) {
    const TrajPose& a = traj[i - 1];
    const TrajPose& b = traj[i];
    const std::int64_t dt_ns = b.t_ns - a.t_ns;
    const double dx = b.p[0] - a.p[0], dy = b.p[1] - a.p[1], dz = b.p[2] - a.p[2];
    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const bool blind = dt_ns > kTrajBlindGapNs;
    const bool impossible =
        dt_ns > 0 && d / (static_cast<double>(dt_ns) * 1e-9) > kTrajMaxWalkSpeedMps;
    const bool regain = traj_pose_untracked(a) && !traj_pose_untracked(b);
    if (blind || impossible || regain) flags |= 2u;
  }
  return flags;
}

void write_trajectory(const std::string& path, const std::vector<TrajPose>& traj) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  const char magic[8] = {'L', 'S', 'T', 'R', 'A', 'J', '0', '2'};
  std::fwrite(magic, 1, sizeof(magic), f);
  const std::uint32_t n = static_cast<std::uint32_t>(traj.size());
  std::uint8_t head[8];
  head[0] = static_cast<std::uint8_t>(n & 0xFFu);
  head[1] = static_cast<std::uint8_t>((n >> 8) & 0xFFu);
  head[2] = static_cast<std::uint8_t>((n >> 16) & 0xFFu);
  head[3] = static_cast<std::uint8_t>((n >> 24) & 0xFFu);
  head[4] = head[5] = head[6] = head[7] = 0;
  std::fwrite(head, 1, sizeof(head), f);
  for (std::size_t i = 0; i < traj.size(); ++i) {
    const TrajPose& p = traj[i];
    const float xyz[3] = {static_cast<float>(p.p[0]), static_cast<float>(p.p[1]),
                          static_cast<float>(p.p[2])};
    std::uint8_t buf[16];
    std::memcpy(buf, xyz, 12);
    const std::uint32_t flags = traj_flags(traj, i);
    buf[12] = static_cast<std::uint8_t>(flags & 0xFFu);
    buf[13] = static_cast<std::uint8_t>((flags >> 8) & 0xFFu);
    buf[14] = static_cast<std::uint8_t>((flags >> 16) & 0xFFu);
    buf[15] = static_cast<std::uint8_t>((flags >> 24) & 0xFFu);
    std::fwrite(buf, 1, sizeof(buf), f);
  }
  std::fclose(f);
}

void write_sidecar(const std::string& path, const ReprocessReport& r) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  const SectionStitchReport& s = r.stitch;
  std::fprintf(f,
               "{\n"
               "  \"schema\": 1,\n"
               "  \"producer\": \"post::reprocess_d6_container (ROUND 13)\",\n"
               "  \"source\": \"streams/lidar.bin + streams/poses_ar.bin + streams/imu_phone.bin\",\n"
               "  \"note\": \"Derived product. The sealed streams are untouched; delete this file "
               "and map_stitched.bin to return the container to what the phone wrote.\",\n"
               "  \"points\": %llu,\n"
               "  \"poses\": %llu,\n"
               "  \"posesUntracked\": %llu,\n"
               "  \"sections\": %llu,\n"
               "  \"seamsRefined\": %llu,\n"
               "  \"firstSectionMovedM\": %.6f,\n"
               "  \"firstSectionMovedDeg\": %.6f,\n"
               "  \"verticalExtentBeforeM\": %.6f,\n"
               "  \"verticalExtentAfterM\": %.6f,\n"
               "  \"endGapBeforeM\": %.6f,\n"
               "  \"endGapAfterM\": %.6f,\n"
               "  \"mountVerdict\": \"%s\",\n"
               "  \"mountImpossibleFraction\": %.6f,\n"
               "  \"selfCheckMeasurable\": %s,\n"
               "  \"selfCheckOffsetM\": %.6f,\n"
               "  \"selfCheckFloorM\": %.6f,\n"
               "  \"selfCheckWindows\": %llu,\n"
               "  \"selfCheckSeconds\": %.3f,\n"
               "  \"selfCheckBlocker\": \"%s\",\n"
               "  \"loopEndDecision\": \"%s\",\n"
               "  \"loopEndApplied\": %s,\n"
               "  \"loopEndCorrectionM\": %.6f,\n"
               "  \"loopEndCorrectionDeg\": %.6f,\n"
               "  \"loopEndGapBeforeM\": %.6f,\n"
               "  \"loopEndGapAfterM\": %.6f,\n"
               "  \"loopEndSamePlaceBeforeM\": %.6f,\n"
               "  \"loopEndSamePlaceAfterM\": %.6f,\n"
               "  \"loopEndObservability\": %.6f,\n"
               "  \"loopEndReason\": \"%s\",\n"
               "  \"seams\": [\n",
               static_cast<unsigned long long>(r.points), static_cast<unsigned long long>(r.poses),
               static_cast<unsigned long long>(s.poses_untracked),
               static_cast<unsigned long long>(s.sections),
               static_cast<unsigned long long>([&] {
                 std::size_t n = 0;
                 for (const SectionSeam& x : s.seams) {
                   if (x.decision == SeamDecision::kRefined) ++n;
                 }
                 return n;
               }()),
               s.total_translation_m, s.total_rotation_deg,
               s.trajectory_vertical_extent_before_m, s.trajectory_vertical_extent_after_m,
               s.trajectory_end_gap_before_m, s.trajectory_end_gap_after_m,
               to_string(r.mount.verdict), r.mount.impossible_fraction,
               r.consistency.measurable ? "true" : "false", r.consistency.nearest_offset_m,
               r.consistency.self_floor_m,
               static_cast<unsigned long long>(r.consistency.windows),
               static_cast<double>(r.consistency.nearest_separation) *
                   r.consistency.window_seconds,
               r.consistency.blocker, to_string(r.loop_end.decision),
               r.loop_end_applied ? "true" : "false", r.loop_end.correction_translation_m,
               r.loop_end.correction_rotation_deg, r.loop_end.end_gap_before_m,
               r.loop_end_applied ? r.loop_end.end_gap_after_m : r.loop_end.end_gap_before_m,
               r.loop_end.submap_mismatch_before_m, r.loop_end.submap_mismatch_after_m,
               r.loop_end.observability, r.loop_end.reason);
  for (std::size_t i = 0; i < s.seams.size(); ++i) {
    const SectionSeam& x = s.seams[i];
    std::fprintf(f,
                 "    {\"tMonoNs\": %lld, \"jumpM\": %.6f, \"jumpDeg\": %.6f, "
                 "\"decision\": \"%s\", \"observability\": %.6f, "
                 "\"mismatchAnalyticM\": %.6f, \"mismatchRefinedM\": %.6f}%s\n",
                 static_cast<long long>(x.t_ns), x.jump_translation_m, x.jump_rotation_deg,
                 to_string(x.decision), x.observability, x.mismatch_analytic_m,
                 x.mismatch_refined_m, (i + 1 == s.seams.size()) ? "" : ",");
  }
  // ROUND 18 item 68 — the gaps that were EXAMINED and not turned into seams.
  // Until this round the sidecar carried only the applied corrections, so a
  // capture like the owner's scan-046 — whose one 6.9 s gap was examined,
  // measured against the gyro (178.63 deg vs the tracker's 72.28) and
  // honestly refused — produced a stitch.json indistinguishable from a clean
  // walk's. The refusals are half the verdict, and the half the operator's
  // "why does my path look wrong" question actually needs. Additive: every
  // existing key is unchanged and the app's kotlinx parser ignores unknowns.
  std::fprintf(f, "  ],\n  \"gapsExamined\": [\n");
  for (std::size_t i = 0; i < s.gaps_examined.size(); ++i) {
    const SectionSeam& x = s.gaps_examined[i];
    std::fprintf(f,
                 "    {\"tMonoNs\": %lld, \"gapS\": %.3f, \"jumpM\": %.6f, \"jumpDeg\": %.6f, "
                 "\"gyroDeg\": %.6f, \"residualM\": %.6f, \"residualDeg\": %.6f, "
                 "\"decision\": \"%s\", \"reason\": \"%s\"}%s\n",
                 static_cast<long long>(x.t_ns), x.gap_s, x.jump_translation_m,
                 x.jump_rotation_deg, x.gyro_rotation_deg, x.residual_translation_m,
                 x.residual_rotation_deg, to_string(x.decision), x.reason,
                 (i + 1 == s.gaps_examined.size()) ? "" : ",");
  }
  // ROUND 19 items 73/74 + the yield audit — all additive; the app's readers
  // ignore unknown keys. Refused rescues are recorded with the same care as
  // applied ones: the refusal and its numbers ARE the product here.
  std::fprintf(f, "  ],\n  \"rescues\": [\n");
  for (std::size_t i = 0; i < r.rescues.size(); ++i) {
    const GapRescueReport& x = r.rescues[i];
    std::fprintf(f,
                 "    {\"tMonoNs\": %lld, \"gapS\": %.3f, \"gyroDeg\": %.6f, "
                 "\"rotationAppliedDeg\": %.6f, \"translationM\": %.6f, "
                 "\"coarseOverlap\": %.4f, \"observability\": %.6f, \"solvedAxes\": %d, "
                 "\"pairs\": %llu, \"mismatchIdentityM\": %.6f, \"mismatchRescuedM\": %.6f, "
                 "\"selfCheckBeforeM\": %.6f, \"selfCheckAfterM\": %.6f, "
                 "\"decision\": \"%s\", \"reason\": \"%s\"}%s\n",
                 static_cast<long long>(x.t_after_ns), x.gap_s, x.gyro_rotation_deg,
                 x.rotation_applied_deg, x.translation_m, x.coarse_overlap, x.observability,
                 x.solved_axes, static_cast<unsigned long long>(x.pairs),
                 x.mismatch_identity_m, x.mismatch_rescued_m, x.self_check_before_m,
                 x.self_check_after_m, to_string(x.decision), x.reason,
                 (i + 1 == r.rescues.size()) ? "" : ",");
  }
  std::fprintf(f, "  ],\n  \"recoveries\": [\n");
  for (std::size_t i = 0; i < r.recoveries.size(); ++i) {
    const GapRecovery& x = r.recoveries[i];
    std::fprintf(f,
                 "    {\"tBeforeNs\": %lld, \"tAfterNs\": %lld, \"candidates\": %llu, "
                 "\"noGyro\": %llu, \"admitted\": %llu, \"rulerVetoed\": %s, "
                 "\"selfCheckBeforeM\": %.6f, \"selfCheckAfterM\": %.6f, "
                 "\"reason\": \"%s\"}%s\n",
                 static_cast<long long>(x.t_before_ns), static_cast<long long>(x.t_after_ns),
                 static_cast<unsigned long long>(x.candidates),
                 static_cast<unsigned long long>(x.no_gyro),
                 static_cast<unsigned long long>(x.admitted), x.ruler_vetoed ? "true" : "false",
                 x.self_check_before_m, x.self_check_after_m, x.reason,
                 (i + 1 == r.recoveries.size()) ? "" : ",");
  }
  // ROUND 20 item 80 — additive, like everything since round 18: the app's
  // kotlinx parser ignores unknown keys, and a refusal's numbers are recorded
  // with the same care as an applied correction's.
  {
    const AutoLevelReport& al = r.auto_level;
    std::fprintf(f,
                 "  ],\n"
                 "  \"autoLevel\": {\"decision\": \"%s\", \"reason\": \"%s\", "
                 "\"floorFound\": %s, \"floorInliers\": %llu, \"floorCoverageM2\": %.3f, "
                 "\"tiltBeforeDeg\": %.4f, \"tiltAfterDeg\": %.4f, "
                 "\"correctionDeg\": %.4f, \"iterations\": %d, "
                 "\"selfCheckBeforeM\": %.6f, \"selfCheckAfterM\": %.6f, "
                 "\"applied\": %s},\n",
                 to_string(al.decision), al.reason, al.floor_found ? "true" : "false",
                 static_cast<unsigned long long>(al.floor_inliers), al.floor_coverage_m2,
                 al.tilt_before_deg, al.tilt_after_deg, al.correction_deg, al.iterations,
                 al.self_check_before_m, al.self_check_after_m,
                 r.auto_level_applied ? "true" : "false");
  }
  std::fprintf(f,
               "  \"recoveredPoints\": %llu,\n"
               "  \"yield\": {\"samples\": %llu, \"noReturns\": %llu, "
               "\"outOfWindow\": %llu, \"noPose\": %llu, \"flaggedExcluded\": %llu, "
               "\"otherDropped\": %llu, \"resolved\": %llu, \"recovered\": %llu}\n"
               "}\n",
               static_cast<unsigned long long>(r.recovered_points),
               static_cast<unsigned long long>(r.yield.samples),
               static_cast<unsigned long long>(r.yield.no_returns),
               static_cast<unsigned long long>(r.yield.out_of_window),
               static_cast<unsigned long long>(r.yield.no_pose),
               static_cast<unsigned long long>(r.yield.flagged_excluded),
               static_cast<unsigned long long>(r.yield.other_dropped),
               static_cast<unsigned long long>(r.yield.resolved),
               static_cast<unsigned long long>(r.yield.recovered));
  std::fclose(f);
}

}  // namespace

bool has_stitched_cloud(const std::string& lscan_dir) {
  std::FILE* f = std::fopen(join(lscan_dir, "processed/map_stitched.bin").c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

Status reprocess_d6_container(const std::string& lscan_dir, const ReprocessOptions& opts,
                              ReprocessReport* out, PostProgressFn progress, CancelToken* cancel) {
  ReprocessReport rep;
  rep.map_path = join(lscan_dir, "processed/map_stitched.bin");
  rep.sidecar_path = join(lscan_dir, "processed/stitch.json");

  bool is_d6 = false;
  SCAN_TRY(lscan_is_d6_project(lscan_dir, &is_d6));
  if (!is_d6) {
    if (out != nullptr) *out = rep;
    return set_last_error(ScanError::kNotSupported,
                          "reprocess: '%s' is not a COIN-D6 project", lscan_dir.c_str());
  }

  // Its OWN store, never the viewer's. The viewer is reading the cloud this
  // is about to replace, and publishing into it would show a half-corrected
  // map made of two frames at once.
  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);
  std::vector<TrajPose> traj;
  std::vector<std::int64_t> ptimes;

  D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.densify_with_phone_imu = opts.densify_with_phone_imu;
  cfg.stitch_sections = opts.stitch_sections;
  cfg.sections = opts.sections;
  cfg.close_loops = opts.close_loops;
  cfg.close_loop_end = opts.close_loop_end;
  cfg.loop_end = opts.loop_end;
  cfg.rescue_gaps = opts.rescue_gaps;
  cfg.rescue = opts.rescue;
  cfg.recover_gap_points = opts.recover_gap_points;
  cfg.out_trajectory = &traj;
  cfg.out_point_times = &ptimes;

  D6ResolvePipeline pipe(cfg);
  if (progress != nullptr) pipe.set_progress_callback(progress);
  if (cancel != nullptr) pipe.set_cancel_token(cancel);
  const Status st = pipe.run(lscan_dir);
  if (!st.ok()) {
    if (out != nullptr) *out = rep;
    return st;
  }
  rep.ran = true;
  rep.stitch = pipe.stats().sections;
  rep.loop_end = pipe.stats().loop_end;
  rep.loop_end_applied = pipe.stats().loop_end_applied;
  rep.rescues = pipe.stats().rescues;
  rep.gaps_rescued = pipe.stats().gaps_rescued;
  rep.recoveries = pipe.stats().recoveries;
  rep.recovered_points = pipe.stats().recovered_points;
  {
    // ROUND 19: the yield audit, composed from the run's own counters. The
    // parser is the only place that sees range-0 samples (the driver drops
    // them before the profile sink), so `samples` is parser no-returns plus
    // everything that reached the assembler.
    const PushbroomStats& pb = pipe.stats().pushbroom;
    rep.yield.no_returns = pipe.stats().d6_no_returns;
    rep.yield.out_of_window = pb.dropped_range;
    rep.yield.no_pose = pb.dropped_no_pose;
    rep.yield.flagged_excluded = pb.flagged_total() - pb.flagged_emitted;
    rep.yield.other_dropped = pb.dropped_overflow + pb.dropped_page_full;
    rep.yield.resolved = pb.points_out;
    rep.yield.recovered = pipe.stats().recovered_points;
    rep.yield.samples = rep.yield.no_returns + pb.points_in;
  }
  rep.poses = traj.size();

  std::vector<PointVertex> pts;
  pts.reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) pts.push_back(v.data[k]);
  }
  rep.points = pts.size();

  // Free, since the cloud and the trajectory are already in hand, and it is
  // the answer to "why does this scan look wrong" that stitching cannot give.
  rep.mount = check_mount_consistency(traj, Span<const PointVertex>(pts.data(), pts.size()),
                                      Span<const std::int64_t>(ptimes.data(), ptimes.size()));

  // ROUND 20 item 80. AFTER every trajectory-shaped correction (stitch,
  // rescue, loop-end — all already inside the resolve) and BEFORE the final
  // self-check, so the number on the summary card describes the cloud that
  // was actually written. A refusal mutates nothing, so a refused container
  // reprocesses to a byte-identical map. See auto_level.h.
  if (opts.auto_level && !pts.empty()) {
    AutoLevelConfig lcfg = opts.level;
    lcfg.consistency = opts.consistency;
    rep.auto_level = auto_level_floor(pts, ptimes, traj, lcfg);
    rep.auto_level_applied = rep.auto_level.decision == AutoLevelDecision::kApplied;
  }

  // ROUND 15 item 57. Measured on the STITCHED cloud, deliberately: that is
  // the cloud the operator is about to look at, and on a multi-section
  // capture the unstitched one disagrees with itself by the seam rather than
  // by anything the sensor did. `measurable == false` is an answer, not a
  // failure — see map_consistency.h.
  if (opts.measure_self_consistency && !pts.empty()) {
    rep.consistency = measure_map_consistency(pts, ptimes, opts.consistency);
  }

  if (!pts.empty()) {
    // The container may have no `processed/` at all — a capture that produced
    // no preview never made one, and an exported/re-imported .lscan will not
    // carry an empty directory. Creating it is not a liberty: this function's
    // whole job is to put a derived product there.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(lscan_dir) / "processed", ec);
    const Status w =
        write_point_chunk_file(rep.map_path, pts, traj.empty() ? 0 : traj.front().t_ns);
    if (!w.ok()) {
      if (out != nullptr) *out = rep;
      return w;
    }
    rep.map_written = true;
  }
  if (opts.always_write_sidecar || rep.map_written) write_sidecar(rep.sidecar_path, rep);
  // ROUND 16 item 59. Written whenever there is a trajectory at all, including
  // when nothing was stitched — the path is worth drawing on a clean scan too,
  // and a file that only appears on broken captures would be a file nobody
  // trusts.
  if (!traj.empty()) {
    std::error_code tec;
    std::filesystem::create_directories(std::filesystem::path(lscan_dir) / "processed", tec);
    write_trajectory(join(lscan_dir, "processed/trajectory.bin"), traj);
    rep.trajectory_written = true;
  }

  SCAN_LOG_INFO(kMod,
                "'%s': %zu sections, %llu points -> %s (vertical extent %.3f -> %.3f m, "
                "end gap %.3f -> %.3f m, mount %s)",
                lscan_dir.c_str(), rep.stitch.sections,
                static_cast<unsigned long long>(rep.points),
                rep.map_written ? "processed/map_stitched.bin" : "(nothing written)",
                rep.stitch.trajectory_vertical_extent_before_m,
                rep.stitch.trajectory_vertical_extent_after_m,
                rep.stitch.trajectory_end_gap_before_m, rep.stitch.trajectory_end_gap_after_m,
                to_string(rep.mount.verdict));
  if (out != nullptr) *out = rep;
  return kOkStatus;
}

}  // namespace post
}  // namespace scanengine
