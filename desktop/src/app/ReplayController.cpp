#include "app/ReplayController.h"

#include "app/EngineHost.h"

namespace lidarscan {

ReplayController::ReplayController(EngineHost* host, QObject* parent)
    : QObject(parent), host_(host) {
  poll_timer_.setInterval(150);
  connect(&poll_timer_, &QTimer::timeout, this, &ReplayController::poll);
}

ReplayController::~ReplayController() {
  stop();
  if (thread_.joinable()) thread_.join();
}

bool ReplayController::start(const QString& lscan_dir, double speed, QString* err) {
  if (running_.load()) {
    if (err) *err = "a replay is already running";
    return false;
  }
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  auto* eng = host_->engine();

  // Live preview: no lscan_dir, so nothing is recorded (see the header).
  if (eng->session_active()) {
    if (!host_->stopSession(err)) return false;
  }
  if (!host_->startSession(QString(), "quickscan", false, err)) return false;

  scanengine::D6Config d6;
  d6.send_start_stop_commands = false;  // no transport to write to
  d6.require_start_ack = false;
  d6.serial.port_name = "replay";
  device_ = host_->addD6(d6, err);
  if (device_ == scanengine::kInvalidDeviceId) return false;

  dir_ = lscan_dir;
  speed_ = speed;
  source_ = std::make_unique<scanengine::lscan::ReplaySource>(*eng);
  done_.store(false);
  running_.store(true);
  result_ = scanengine::ScanError::kOk;

  scanengine::lscan::ReplayConfig cfg;
  cfg.lscan_dir = lscan_dir.toStdString();
  cfg.target_device = device_;
  cfg.chunk_type = scanengine::lscan::ChunkType::kD6Raw;
  cfg.speed = speed;

  thread_ = std::thread([this, cfg] {
    const auto st = source_->run(cfg);
    result_ = st.error();
    done_.store(true);
  });

  poll_timer_.start();
  Q_EMIT started(lscan_dir, speed);
  return true;
}

void ReplayController::stop() {
  if (!running_.load()) return;
  if (source_) source_->stop();
  if (thread_.joinable()) thread_.join();
  teardown();
}

void ReplayController::poll() {
  if (!running_.load()) {
    poll_timer_.stop();
    return;
  }
  if (!done_.load()) return;
  if (thread_.joinable()) thread_.join();
  teardown();
}

void ReplayController::teardown() {
  poll_timer_.stop();
  running_.store(false);

  QString summary;
  if (source_) {
    // Safe now: the writer thread has been joined.
    const auto& s = source_->stats();
    summary = QString("replayed %1 chunks / %2 bytes at %3x%4%5")
                  .arg(s.chunks_replayed)
                  .arg(s.bytes_replayed)
                  .arg(speed_ <= 0 ? QString("max") : QString::number(speed_, 'g', 3))
                  .arg(s.truncated_tail_chunks
                           ? QString(" · %1 truncated-tail chunks skipped")
                                 .arg(s.truncated_tail_chunks)
                           : QString())
                  .arg(s.crc_mismatch_chunks
                           ? QString(" · %1 CRC mismatches skipped").arg(s.crc_mismatch_chunks)
                           : QString());
    if (result_ != scanengine::ScanError::kOk) {
      summary += QString(" · ended with %1").arg(scanengine::error_str(result_));
    }
  }
  source_.reset();
  Q_EMIT finished(summary);
}

}  // namespace lidarscan
