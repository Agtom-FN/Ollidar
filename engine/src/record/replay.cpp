#include "scanengine/record/replay.h"

#include <chrono>
#include <thread>

#include "scanengine/core/log.h"

namespace scanengine {
namespace lscan {
namespace {
constexpr const char* kMod = "record";
}  // namespace

Status ReplaySource::run(const ReplayConfig& cfg) {
  stop_requested_.store(false, std::memory_order_relaxed);
  stats_ = ReplayStats{};

  FileRecordReader reader;
  SCAN_TRY(reader.open(cfg.lscan_dir));

  bool have_first = false;
  std::int64_t t0_recorded_ns = 0;
  std::chrono::steady_clock::time_point t0_wall{};

  for (;;) {
    if (stop_requested_.load(std::memory_order_relaxed)) break;

    ChunkHeader h{};
    std::vector<std::uint8_t> payload;
    const Status s = reader.next_chunk(&h, &payload);
    if (s.error() == ScanError::kAgain) break;  // clean end of every stream
    if (!s.ok()) {
      (void)reader.close();
      return s;
    }

    if (h.type != cfg.chunk_type) continue;  // not ours to replay (yet) — see class comment

    if (cfg.speed > 0.0) {
      if (!have_first) {
        t0_recorded_ns = h.t_mono_ns;
        t0_wall = std::chrono::steady_clock::now();
        have_first = true;
      }
      const std::int64_t elapsed_recorded_ns = h.t_mono_ns - t0_recorded_ns;
      const auto target_wall =
          t0_wall + std::chrono::nanoseconds(static_cast<std::int64_t>(
                        static_cast<double>(elapsed_recorded_ns) / cfg.speed));
      std::this_thread::sleep_until(target_wall);
    }

    // The exact same call a live USB-serial reader thread makes; replaying
    // the ORIGINAL t_mono_ns as the arrival stamp is what makes the decoded
    // output bit-identical to the live pass (device-state timing, rotation
    // detection, health-window packet counts all depend on arrival times).
    const Status ps = engine_.push_serial_bytes(
        cfg.target_device, ByteSpan(payload.data(), payload.size()), TimePoint{h.t_mono_ns});
    if (!ps.ok()) {
      SCAN_LOG_ERROR(kMod, "replay: push_serial_bytes failed: %s", ps.message());
      (void)reader.close();
      return ps;
    }

    ++stats_.chunks_replayed;
    stats_.bytes_replayed += payload.size();
  }

  stats_.truncated_tail_chunks = reader.warnings().truncated_tail_chunks;
  stats_.crc_mismatch_chunks = reader.warnings().crc_mismatch_chunks;
  (void)reader.close();
  return kOkStatus;
}

}  // namespace lscan
}  // namespace scanengine
