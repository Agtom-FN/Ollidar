// replay.h — feed a recorded .lscan capture back through a live Engine.
//
// Tech Spec §3 Key Rule 2: "Replay == capture — which is what makes
// local/cloud/transfer processing the same pipeline in three places." This
// class is what makes that literally true for the D6 raw stream: it does
// not re-implement any decode logic. It reads kD6Raw chunks back out of a
// FileRecordReader and calls the exact same Engine::push_serial_bytes()
// entry point a live USB-serial reader thread calls, with the payload byte
// for byte and (by default) the ORIGINAL recorded t_mono_ns as the arrival
// stamp — so a replayed capture produces a bit-identical decoded point
// stream to the live pass that made it (see engine/tests/test_lscan_io.cpp
// for the round-trip proof).
//
// Only ChunkType::kD6Raw replays through push_serial_bytes(), because that is
// the only "feed a device bytes" entry point Engine exposes; Mid-360 (A3) and
// GNSS (A10) raw streams will need an analogous push entry point on Engine
// before this class's merge/pacing logic (already generic across chunk
// types) needs anything beyond a wider `chunk_type`/dispatch table.
//
// ROUND 8 added ONE second dispatch, and it is a different shape on purpose:
// ChunkType::kPoseAr chunks go to Engine::push_pose(). android/NOTES.md
// ROUND 7 §9 item 1 called the missing pose writer "the single blocker for
// offline D6 re-assembly, ... and for 'replay == capture' being true of a D6
// cloud rather than only of its bytes". Bytes alone reproduce a 2D fan; bytes
// plus trajectory reproduce the room. See ReplayConfig::replay_poses.
//
// Threading: run() is a blocking call, intended to be driven from a
// dedicated "replay" thread by the caller (engine_cli, a test, or a future
// cloud-worker driver) exactly like a live app drives its own serial-reader
// thread — record/ itself still owns no thread, per DESIGN.md §2.
//
// Owner: A5.
#ifndef SCANENGINE_RECORD_REPLAY_H
#define SCANENGINE_RECORD_REPLAY_H

#include <atomic>
#include <cstdint>
#include <string>

#include "scanengine/core/engine.h"
#include "scanengine/core/error.h"
#include "scanengine/record/lscan.h"

namespace scanengine {
namespace lscan {

struct ReplayConfig {
  std::string lscan_dir;
  DeviceId target_device = kInvalidDeviceId;   // device on `engine` to receive push_serial_bytes()
  ChunkType chunk_type = ChunkType::kD6Raw;    // which recorded stream to replay
  // Speed multiplier applied to the gaps between recorded t_mono_ns values:
  //   1.0  = real time (the capture's own pacing, reproduced with
  //          std::this_thread::sleep_until against a wall clock anchored at
  //          the first replayed chunk)
  //   >1.0 = faster than real time (e.g. 2.0 replays twice as fast)
  //   <1.0 = slower than real time (excluding <= 0, see below)
  //   <= 0  = unpaced — every chunk is pushed as fast as the engine can
  //           decode it. This is what batch reprocessing (local/cloud/
  //           transfer, Tech Spec §3.8) and this task's round-trip tests
  //           use; it is NOT "instant" in the sense of a different code
  //           path, only in the sense of no artificial sleeps.
  double speed = 1.0;

  // --- ROUND 8: the trajectory replays too -------------------------------
  //
  // `chunk_type` selects ONE stream to feed through push_serial_bytes().
  // ChunkType::kPoseAr is not a byte stream and never could be: its entry
  // point is Engine::push_pose(), not push_serial_bytes(). So poses ride
  // alongside whatever `chunk_type` names rather than instead of it — which
  // is what a D6 replay needs, because a D6 without its trajectory replays
  // into a flat fan and with it replays into the room.
  //
  // DEFAULT true, and that is provably not a behaviour change: nothing wrote
  // a kPoseAr chunk before ROUND 8, so no `.lscan` in existence contains one.
  // Every recording made from ROUND 8 on does, and for those "replay ==
  // capture" (Tech Spec §3 key rule 2) is only true with this on. A caller
  // that deliberately wants the bytes without the trajectory — e.g. a test
  // isolating the decode path — sets it false.
  bool replay_poses = true;
};

struct ReplayStats {
  std::uint64_t chunks_replayed = 0;
  std::uint64_t bytes_replayed = 0;
  // ROUND 8: counted separately from `chunks_replayed` (which stays the count
  // of `chunk_type` chunks, so an existing caller's number does not change
  // meaning). Zero on a pre-0.5.0 recording, which is how a caller tells
  // "this project predates trajectory storage" from "this project has poses".
  std::uint64_t poses_replayed = 0;
  // Copied from FileRecordReader::warnings() after the run — a replayed
  // capture that hit a recorded crash's truncated tail is not an error,
  // just a fact the caller may want to report (e.g. in a job's log).
  std::uint32_t truncated_tail_chunks = 0;
  std::uint32_t crc_mismatch_chunks = 0;
};

class ReplaySource {
 public:
  explicit ReplaySource(Engine& engine) : engine_(engine) {}

  // Blocking. Returns once every chunk of `cfg.chunk_type` has been
  // replayed, or immediately after the in-flight push once stop() has been
  // called from another thread.
  Status run(const ReplayConfig& cfg);

  // Safe to call from a different thread than the one inside run().
  void stop() { stop_requested_.store(true, std::memory_order_relaxed); }

  const ReplayStats& stats() const { return stats_; }

 private:
  Engine& engine_;
  std::atomic<bool> stop_requested_{false};
  ReplayStats stats_{};
};

}  // namespace lscan
}  // namespace scanengine

#endif  // SCANENGINE_RECORD_REPLAY_H
