#include "replay_engine.h"

#include "scanengine/drivers/d6/d6_driver.h"

namespace lidarscan_jni {

using scanengine::DeviceConfig;
using scanengine::DeviceKind;
using scanengine::EngineConfig;
using scanengine::ScanError;

ReplayEngine::~ReplayEngine() { stop(); }

bool ReplayEngine::create() {
  if (engine_) return true;  // idempotent

  EngineConfig cfg;
  cfg.app_name = "lidarscan-android-replay";
  auto created = scanengine::Engine::create(cfg);
  if (!created.ok()) {
    last_error_ = scanengine::error_str(created.error());
    return false;
  }
  engine_ = std::move(created.value());

  // Receive-only D6 device: no serial_write (there is no transport to write
  // command bytes to), no start-ACK to wait for (see D6Config's own comment:
  // "the first decoded packet promotes the state" when require_start_ack is
  // false) — exactly desktop/src/app/ReplayController.cpp's `d6` config.
  DeviceConfig dev_cfg;
  dev_cfg.kind = DeviceKind::kD6;
  dev_cfg.d6.send_start_stop_commands = false;
  dev_cfg.d6.require_start_ack = false;
  dev_cfg.d6.serial.port_name = "replay";

  auto added = engine_->add_device(dev_cfg);
  if (!added.ok()) {
    last_error_ = scanengine::error_str(added.error());
    engine_.reset();
    return false;
  }
  device_ = added.value();
  return true;
}

bool ReplayEngine::start(const std::string& lscan_dir, double speed) {
  if (!engine_) {
    last_error_ = "ReplayEngine::create() was not called or failed";
    return false;
  }
  if (running_.load(std::memory_order_relaxed)) {
    last_error_ = "a replay is already running";
    return false;
  }

  // Record-always doesn't apply here: there is nothing new to record, this
  // IS a recording being replayed. `lscan_dir` empty + record=false means
  // "do not write a second copy to disk" (see SessionConfig's own comment).
  scanengine::SessionConfig session_cfg;  // lscan_dir left empty, record = false
  session_cfg.record = false;
  session_cfg.profile = "quickscan";
  auto started = engine_->start_session(session_cfg);
  if (!started.ok()) {
    last_error_ = started.message();
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    last_stats_ = ReplayStatsSnapshot{};
    last_stats_.running = true;
  }
  running_.store(true, std::memory_order_relaxed);

  if (thread_.joinable()) thread_.join();  // previous run already stopped
  thread_ = std::thread(&ReplayEngine::run_replay, this, lscan_dir, speed);
  return true;
}

void ReplayEngine::run_replay(std::string lscan_dir, double speed) {
  scanengine::lscan::ReplayConfig cfg;
  cfg.lscan_dir = std::move(lscan_dir);
  cfg.target_device = device_;
  cfg.chunk_type = scanengine::lscan::ChunkType::kD6Raw;
  cfg.speed = speed;

  source_ = std::make_unique<scanengine::lscan::ReplaySource>(*engine_);
  const scanengine::Status result = source_->run(cfg);
  const scanengine::lscan::ReplayStats& s = source_->stats();

  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    last_stats_.chunks_replayed = s.chunks_replayed;
    last_stats_.bytes_replayed = s.bytes_replayed;
    last_stats_.truncated_tail_chunks = s.truncated_tail_chunks;
    last_stats_.crc_mismatch_chunks = s.crc_mismatch_chunks;
    last_stats_.running = false;
    last_stats_.done = true;
    last_stats_.result_error = static_cast<std::int32_t>(result.error());
  }
  running_.store(false, std::memory_order_relaxed);

  // Flush whatever the (currently disabled/no-op for D6, but harmless) tail
  // logic needs and close out the session so engine_->points() reflects a
  // finished capture — same call stop_session() always makes.
  (void)engine_->stop_session();
}

void ReplayEngine::stop() {
  if (source_) source_->stop();
  if (thread_.joinable()) thread_.join();
  running_.store(false, std::memory_order_relaxed);
}

ReplayStatsSnapshot ReplayEngine::stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  ReplayStatsSnapshot snap = last_stats_;
  snap.running = running_.load(std::memory_order_relaxed);
  return snap;
}

}  // namespace lidarscan_jni
