// replay_engine.h — B4's synthetic-capture replay helper.
//
// WHY THIS EXISTS (see android/NOTES.md's "B4 — replay path" section for the
// full writeup): the Capture screen's acceptance path with no hardware
// attached is to replay a bundled synthetic D6 capture
// (assets/replay/synth.lscan/, the S1 d6synth output also used by
// desktop/evidence/synth.lscan) through the *exact* same decode pipeline a
// live USB connection drives — Tech Spec §3 Key Rule 2, "replay == capture".
// `scanengine::lscan::ReplaySource` (engine/include/scanengine/record/replay.h)
// is precisely that: it reads recorded kD6Raw chunks and calls
// `Engine::push_serial_bytes()`, the same entry point
// `com.lidarscan.app.usb.D6SerialConnection`'s reader thread calls for real
// bytes.
//
// ReplaySource needs a C++ `scanengine::Engine&`, not the C ABI's opaque
// `scan_engine*` (scanengine_c.cpp's EngineHandle that wraps a real Engine
// is a file-local implementation detail, not something this shim can reach
// into without modifying engine/ — out of B4's read-only scope). So this is
// a SEPARATE, second Engine instance dedicated to replay, built by linking
// the engine's C++ API directly (allowed: scanengine_jni already links
// `scanengine` statically for the C-ABI path; this is the same static
// library, just a different header set). It never touches the live
// capture's `scan_engine*` handle B2's nativeCreateEngine created.
//
// PAUSE/RESUME GAP (documented honestly, not worked around): ReplaySource::
// run() is a single blocking call with its own internal pacing loop and no
// pause hook or seek-and-resume primitive in its public API (ReplayConfig
// has no start-offset field). Forking that loop would mean re-implementing
// pacing logic engine/ already owns, which is out of this task's read-only
// scope for engine/. So ReplayEngine supports start/stop only — the
// Kotlin-side ReplayEngineBridge (app/engine/ReplayEngineBridge.kt) reports
// pause/resume as unsupported for a replay session rather than faking a
// resume that would silently restart from t=0. This mirrors B2's own
// documented "no pause/resume in the C ABI" gap, one layer up.
#ifndef LIDARSCAN_JNI_REPLAY_ENGINE_H
#define LIDARSCAN_JNI_REPLAY_ENGINE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "scanengine/core/engine.h"
#include "scanengine/record/replay.h"

namespace lidarscan_jni {

struct ReplayStatsSnapshot {
  std::uint64_t chunks_replayed = 0;
  std::uint64_t bytes_replayed = 0;
  std::uint32_t truncated_tail_chunks = 0;
  std::uint32_t crc_mismatch_chunks = 0;
  bool running = false;
  bool done = false;
  std::int32_t result_error = 0;  // scanengine::ScanError, 0 == kOk
};

class ReplayEngine {
 public:
  ReplayEngine() = default;
  ~ReplayEngine();

  ReplayEngine(const ReplayEngine&) = delete;
  ReplayEngine& operator=(const ReplayEngine&) = delete;

  // Creates the standalone Engine and adds one receive-only D6 device (no
  // serial_write callback — replay never writes command bytes back to a
  // transport that does not exist). Returns false on failure; see
  // last_error().
  bool create();

  // Starts a detached replay thread over `lscan_dir` (a real filesystem
  // path the bundled asset has already been extracted to — see
  // ReplaySyntheticCapture.kt; ReplaySource/FileRecordReader read plain
  // files, not APK asset-manager paths). `speed`: 1.0 = real time (the
  // capture's own pacing), <= 0 = unpaced (as fast as decode allows).
  // Returns false if a replay is already running or create() was not
  // called/failed.
  bool start(const std::string& lscan_dir, double speed);

  // Requests the replay thread stop and joins it. Safe to call at any time,
  // including when nothing is running.
  void stop();

  bool is_running() const { return running_.load(std::memory_order_relaxed); }

  ReplayStatsSnapshot stats() const;

  // Non-null once create() has succeeded — used by the JNI layer's point-page
  // and device-health accessors, which read scanengine::Engine's C++ API
  // directly (PageStore/device_health), the same as PagedCloudRenderer does
  // on desktop.
  scanengine::Engine* engine() { return engine_.get(); }
  scanengine::DeviceId device_id() const { return device_; }

  const std::string& last_error() const { return last_error_; }

 private:
  void run_replay(std::string lscan_dir, double speed);

  std::unique_ptr<scanengine::Engine> engine_;
  scanengine::DeviceId device_ = scanengine::kInvalidDeviceId;
  std::unique_ptr<scanengine::lscan::ReplaySource> source_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex stats_mutex_;
  ReplayStatsSnapshot last_stats_{};

  std::string last_error_;
};

}  // namespace lidarscan_jni

#endif  // LIDARSCAN_JNI_REPLAY_ENGINE_H
