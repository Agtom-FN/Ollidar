#include "app/ReplayController.h"

#include "app/EngineHost.h"
#include "app/FieldLog.h"

#include <cstdio>

#include "scanengine/record/lscan.h"

namespace lidarscan {
namespace {

// What the container actually holds. Asking the file is the only honest way to
// choose a replay device: a caller cannot know, and guessing from a filename
// would hand the D6 parser Livox datagrams.
struct StreamCensus {
  bool mid70 = false;
  bool imu_serial = false;
};

StreamCensus censusOf(const QString& lscan_dir) {
  StreamCensus c;
  scanengine::lscan::FileRecordReader reader;
  if (!reader.open(lscan_dir.toStdString()).ok()) return c;  // start() reports the real error
  for (const auto& s : reader.stream_summaries()) {
    if (s.chunk_count == 0) continue;
    if (s.stream == scanengine::StreamId::kLidarMid70) c.mid70 = true;
    if (s.stream == scanengine::StreamId::kImuSerial) c.imu_serial = true;
  }
  (void)reader.close();
  return c;
}

}  // namespace

ReplayController::ReplayController(EngineHost* host, QObject* parent)
    : QObject(parent), host_(host) {
  poll_timer_.setInterval(150);
  connect(&poll_timer_, &QTimer::timeout, this, &ReplayController::poll);
}

ReplayController::~ReplayController() {
  stop();
  joinLegs();
}

void ReplayController::startLeg(Leg& leg) {
  auto* eng = host_->engine();
  leg.source = std::make_unique<scanengine::lscan::ReplaySource>(*eng);

  scanengine::lscan::ReplayConfig cfg;
  cfg.lscan_dir = dir_.toStdString();
  cfg.target_device = leg.device;
  cfg.chunk_type = leg.chunk_type;
  cfg.speed = speed_;
  // ROUND 8/9: poses and phone IMU ride alongside whatever `chunk_type` names.
  // Left at their defaults, and deliberately so on BOTH legs: a container that
  // has neither (every Mid-70 recording, today) replays them zero times, and a
  // container that has them is a D6 project with exactly one leg.

  leg.done.store(false);
  leg.result = scanengine::ScanError::kOk;
  Leg* raw = &leg;
  leg.thread = std::thread([raw, cfg] {
    const auto st = raw->source->run(cfg);
    raw->result = st.error();
    raw->done.store(true);
  });
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

  // Read the container BEFORE starting a session: if it cannot be opened at all
  // there is nothing to replay, and starting an engine session first would
  // leave one running behind a failed call.
  const StreamCensus census = censusOf(lscan_dir);

  // Live preview: no lscan_dir, so nothing is recorded (see the header).
  if (host_->engine()->session_active()) {
    if (!host_->stopSession(err)) return false;
  }
  if (!host_->startSession(QString(), "quickscan", false, err)) return false;

  dir_ = lscan_dir;
  speed_ = speed;
  legs_.clear();
  FieldLog::info("replay", QString("event=replay_start dir=%1 speed=%2 mid70=%3 d6=%4 "
                                   "imu_serial=%5")
                               .arg(lscan_dir)
                               .arg(speed, 0, 'f', 3)
                               .arg(census.mid70 ? 1 : 0)
                               .arg(census.mid70 ? 0 : 1)
                               .arg(census.imu_serial ? 1 : 0));

  // --- the lidar leg, always present -------------------------------------
  {
    auto leg = std::make_unique<Leg>();
    if (census.mid70) {
      // A17. kInject is the backend with NO transport: the caller pushes whole
      // datagrams through push_serial_bytes(), which is exactly one recorded
      // kMid70Points chunk each. Its reconnect state machine is the same one
      // the live path runs, driven here by the recorded arrival stamps.
      scanengine::Mid70Config cfg;
      cfg.backend = scanengine::Mid70Backend::kInject;
      // No decimation on replay: the live budget exists to keep a walking
      // operator's viewport fluid, and a replay is being watched, not walked.
      cfg.live_points_per_sec = 0;
      // Nothing may reconnect: there is no link to lose, and the watchdog would
      // otherwise trip on the gaps a paused or slow replay leaves.
      cfg.reconnect.enabled = false;
      cfg.internal_supervisor_thread = false;
      leg->device = host_->addMid70(cfg, err);
      leg->chunk_type = scanengine::lscan::ChunkType::kMid70Points;
      leg->label = "Mid-70 points";
    } else {
      scanengine::D6Config d6;
      d6.send_start_stop_commands = false;  // no transport to write to
      d6.require_start_ack = false;
      d6.serial.port_name = "replay";
      leg->device = host_->addD6(d6, err);
      leg->chunk_type = scanengine::lscan::ChunkType::kD6Raw;
      leg->label = "D6 raw";
    }
    if (leg->device == scanengine::kInvalidDeviceId) {
      QString stop_err;
      (void)host_->stopSession(&stop_err);
      return false;
    }
    legs_.push_back(std::move(leg));
  }

  // --- A18: the serial IMU leg, only when the container has one -----------
  //
  // Added even when the lidar leg is a D6, because "kImuSerial chunks exist" is
  // a property of the container, not of which lidar wrote it. No pre-A18
  // recording contains any, so this branch is dead for every `.lscan` on disk
  // before this change {the record/lscan.h argument for a new stream file}.
  if (census.imu_serial) {
    auto leg = std::make_unique<Leg>();
    scanengine::ImuSerialConfig cfg;
    cfg.serial.port_name = "replay";
    // NO write_fn and NO rate command: there is no port, and asking a replay to
    // configure a module that is not there would be a write into nothing that
    // the driver would then report as a failure.
    cfg.send_rate_command = false;
    cfg.internal_supervisor_thread = false;
    // EngineHost::addImuSerial() insists on an open SerialReader, which a replay
    // has no business owning, so this goes through the generic engine path
    // instead: a plain kImuSerial device with a write-less UsbSerialConfig.
    scanengine::DeviceConfig dc;
    dc.kind = scanengine::DeviceKind::kImuSerial;
    dc.imu_serial = cfg;
    auto r = host_->engine()->add_device(dc);
    if (r.ok()) {
      leg->device = r.value();
      leg->chunk_type = scanengine::lscan::ChunkType::kImuSerialRaw;
      leg->label = "serial IMU raw";
      legs_.push_back(std::move(leg));
    } else {
      // NOT fatal, and NOT written into `err`: the lidar leg is about to start
      // successfully and a caller that reads `err` after a `true` return would
      // be told the whole replay failed. A lidar replay without its IMU is a
      // smaller answer, not a wrong one. It is said out loud on stderr, where
      // every other honest-but-not-fatal line in this app goes.
      std::fprintf(stderr,
                   "[lidarscan][replay] this project has a serial-IMU stream but the IMU "
                   "device could not be added (%s) — replaying the lidar alone; live "
                   "odometry will have no gyro\n",
                   scanengine::error_str(r.error()));
    }
  }

  running_.store(true);
  for (auto& leg : legs_) startLeg(*leg);

  poll_timer_.start();
  Q_EMIT started(lscan_dir, speed);
  return true;
}

bool ReplayController::allLegsDone() const {
  for (const auto& leg : legs_) {
    if (!leg->done.load()) return false;
  }
  return true;
}

void ReplayController::joinLegs() {
  for (auto& leg : legs_) {
    if (leg->thread.joinable()) leg->thread.join();
  }
}

void ReplayController::stop() {
  if (!running_.load()) return;
  FieldLog::info("replay", QString("event=replay_stop dir=%1 (asked)").arg(dir_));
  for (auto& leg : legs_) {
    if (leg->source) leg->source->stop();
  }
  joinLegs();
  teardown();
}

void ReplayController::poll() {
  if (!running_.load()) {
    poll_timer_.stop();
    return;
  }
  if (!allLegsDone()) return;
  joinLegs();
  teardown();
}

void ReplayController::teardown() {
  poll_timer_.stop();
  running_.store(false);

  QString summary;
  for (const auto& leg : legs_) {
    if (!leg->source) continue;
    // Safe now: every writer thread has been joined.
    const auto& s = leg->source->stats();
    if (!summary.isEmpty()) summary += " · ";
    summary += QString("%1: %2 chunks / %3 bytes at %4x%5%6")
                   .arg(QString::fromUtf8(leg->label))
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
    if (leg->result != scanengine::ScanError::kOk) {
      summary += QString(" · ended with %1").arg(scanengine::error_str(leg->result));
    }
  }
  legs_.clear();
  FieldLog::info("replay",
                 QString("event=replay_done dir=%1 summary=\"%2\"").arg(dir_, summary));
  Q_EMIT finished(summary);
}

}  // namespace lidarscan
