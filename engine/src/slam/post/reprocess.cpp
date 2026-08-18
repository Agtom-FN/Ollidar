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
// little-endian u32 count, a u32 of zero reserved, then `count` records of
// three little-endian float32 metres. No CRC — this is a derived file whose
// only reader checks the magic and the count against the file length, and
// deleting it costs nothing but the line on screen.
void write_trajectory(const std::string& path, const std::vector<TrajPose>& traj) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  const char magic[8] = {'L', 'S', 'T', 'R', 'A', 'J', '0', '1'};
  std::fwrite(magic, 1, sizeof(magic), f);
  const std::uint32_t n = static_cast<std::uint32_t>(traj.size());
  std::uint8_t head[8];
  head[0] = static_cast<std::uint8_t>(n & 0xFFu);
  head[1] = static_cast<std::uint8_t>((n >> 8) & 0xFFu);
  head[2] = static_cast<std::uint8_t>((n >> 16) & 0xFFu);
  head[3] = static_cast<std::uint8_t>((n >> 24) & 0xFFu);
  head[4] = head[5] = head[6] = head[7] = 0;
  std::fwrite(head, 1, sizeof(head), f);
  for (const TrajPose& p : traj) {
    const float xyz[3] = {static_cast<float>(p.p[0]), static_cast<float>(p.p[1]),
                          static_cast<float>(p.p[2])};
    std::uint8_t buf[12];
    std::memcpy(buf, xyz, sizeof(buf));
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
  std::fprintf(f, "  ]\n}\n");
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
