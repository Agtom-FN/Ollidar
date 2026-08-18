// reprocess.h — ROUND 13. "Process this scan": take a SEALED container whose
// capture broke into sections, put it back into one frame, and leave the
// result on disk where the viewer will find it.
//
// --- WHY THE CORRECTED MAP IS A NEW FILE ----------------------------------
//
// Tech Spec §3 key rule 2 is "replay == capture": the raw streams a container
// holds must always re-resolve to the cloud the live pass produced. Section
// stitching deliberately produces DIFFERENT points, so writing them over a raw
// stream would break that rule permanently and silently.
//
// `streams/map.bin` is not a raw stream — it is the resolved cache ROUND 8
// added, and engine.cpp already documents that deleting it costs only speed.
// So overwriting it would be *defensible*. It is still not done, for one
// reason: after the overwrite nothing on disk can say whether the cloud in
// front of you is the live pass or a correction, and the next field report
// would be written from a cloud whose provenance nobody could recover.
//
// So the corrected cloud goes to `processed/map_stitched.bin` — the directory
// literally named for derived output — in the same chunk framing, beside a
// `processed/stitch.json` recording what was done and what it moved.
// `load_recorded_cloud()` prefers it when present, so Review draws the
// corrected map with no change to any caller, and deleting those two files
// returns the container to exactly what the phone sealed.
//
// Owner: ROUND 13.
#ifndef SCANENGINE_SLAM_POST_REPROCESS_H
#define SCANENGINE_SLAM_POST_REPROCESS_H

#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/cloud/point_page.h"
#include "scanengine/core/error.h"
#include "scanengine/slam/post/map_consistency.h"
#include "scanengine/slam/post/loop_end.h"
#include "scanengine/slam/post/mount_watch.h"
#include "scanengine/slam/post/progress.h"
#include "scanengine/slam/post/section_stitch.h"

namespace scanengine {
namespace post {

struct ReprocessOptions {
  bool stitch_sections = true;
  // Loop closure runs AFTER stitching, on a trajectory that is finally one
  // walk. Off by default: ROUND 12 showed the closer refuses these captures
  // for reasons it can state, and this round did not change that.
  bool close_loops = false;
  // ROUND 16 item 60. ON by default, and the default is the decision: this is
  // the "Process this scan" pipeline, the one place in the product that is
  // ALLOWED to move points the live pass could not have moved, and the loop
  // gap is the largest error in every capture the owner has taken. It refuses
  // itself on six independent gates — including ROUND 12's ruler, which vetoes
  // any correction that would make the number on the summary card worse — so
  // "on by default" costs a refusal and a log line when there is no loop, and
  // that is what it should cost. See slam/post/loop_end.h.
  bool close_loop_end = true;
  LoopEndConfig loop_end{};
  bool densify_with_phone_imu = true;
  SectionStitchConfig sections{};
  // Written even when nothing was stitched, so the sidecar always says what
  // the last processing run concluded.
  bool always_write_sidecar = true;

  // ROUND 15 item 57. Run the ROUND 12 ruler (map_consistency.h) over the
  // cloud this pass just produced. It is nearly free — the points and their
  // times are already in hand, and the measurement is a voxel pass plus some
  // 3x3 Jacobi — and it is the only number in this report that is an
  // ACCURACY-shaped statement rather than a description of what moved. On by
  // default, because a summary card that cannot say how well the map agrees
  // with itself is a card that has to fall back on adjectives.
  bool measure_self_consistency = true;
  MapConsistencyConfig consistency{};
};

struct ReprocessReport {
  bool ran = false;             // the pipeline resolved
  bool map_written = false;     // processed/map_stitched.bin exists and is new
  std::uint64_t points = 0;
  std::uint64_t poses = 0;
  SectionStitchReport stitch{};
  MountWatchReport mount{};
  // ROUND 15: `measurable` is false — with `blocker` saying why — on a scan
  // that never painted the same surface twice. That is a legitimate answer
  // for a single pass down a corridor and the card says so in those words.
  MapConsistencyReport consistency{};
  // ROUND 16 item 60. Always filled — including every refusal, each naming its
  // gate — so a field log can say why a scan's loop was or was not closed.
  LoopEndReport loop_end{};
  bool loop_end_applied = false;
  // ROUND 16 item 59: `processed/trajectory.bin` was written — the corrected
  // walk, as float32 metre triples, for the path the app draws inside the
  // cloud. See write_trajectory() in the .cpp for the format and for why it is
  // a file rather than an ABI call.
  bool trajectory_written = false;
  std::string map_path;
  std::string sidecar_path;
};

// Resolve `lscan_dir` offline with section stitching on, write the corrected
// cloud and its provenance into `processed/`, and report what moved.
//
// Idempotent: running it twice replaces the two derived files and reads the
// same raw streams, so the second answer is bit-identical to the first.
Status reprocess_d6_container(const std::string& lscan_dir, const ReprocessOptions& opts,
                              ReprocessReport* out, PostProgressFn progress = nullptr,
                              CancelToken* cancel = nullptr);

// True when `lscan_dir` already carries a stitched cloud.
bool has_stitched_cloud(const std::string& lscan_dir);

// The derived-product writer, exposed because the FORMAT is the risk: this
// file is read back by a hand-rolled single-file chunk reader in
// d6_resolve.cpp rather than by FileRecordReader (whose stream list is
// hard-coded), so writer and reader have to be tested against each other and
// not merely against themselves.
Status write_point_chunk_file(const std::string& path, const std::vector<PointVertex>& pts,
                              std::int64_t t_first_ns);

}  // namespace post
}  // namespace scanengine

#endif  // SCANENGINE_SLAM_POST_REPROCESS_H
