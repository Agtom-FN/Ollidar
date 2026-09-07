// ReplayController.h — drives lscan::ReplaySource on a worker thread.
//
// This is the desktop's first end-to-end exercise of the engine: a recorded
// `.lscan` is read back and pushed through the SAME Engine::push_serial_bytes()
// entry point a live USB serial reader calls, with the payload byte-for-byte
// and the original recorded arrival stamps — so the decoded point stream is
// bit-identical to the capture that produced it (record/replay.h, and the
// round-trip proof in engine/tests/test_lscan_io.cpp). The points land in the
// PageStore and the viewport picks them up on its next frame. Tech Spec §3 key
// rule 2, "replay == capture", made visible.
//
// THREADING
//   ReplaySource::run() blocks, so it gets its own std::thread — "intended to
//   be driven from a dedicated replay thread by the caller ... exactly like a
//   live app drives its own serial-reader thread" (replay.h). That thread is
//   the only one pushing into this device, which satisfies DESIGN.md §2's rule
//   that a single Driver must be pushed from one thread at a time.
//   ReplayStats is plain (non-atomic) memory written by that thread, so it is
//   only read after join(); live progress comes from DeviceHealth instead,
//   which the driver publishes under its own lock.
//
// WHICH STREAM IT REPLAYS (A17/A18)
//   `.lscan` containers now come in four shapes on this path, and start() picks
//   by ASKING THE FILE rather than by asking the caller: it opens the project
//   with lscan::FileRecordReader and reads the stream summaries.
//     * kLidarMid70 chunks  -> a DeviceKind::kMid70 device on Mid70Backend::
//       kInject, fed ChunkType::kMid70Points — one SDK v1 datagram per push,
//       which is exactly the shape the recorder wrote (record/lscan.h).
//     * kImuSerial chunks   -> a DeviceKind::kImuSerial device fed
//       ChunkType::kImuSerialRaw from streams/imu_serial.bin. Raw UART bytes,
//       the kD6Raw contract, reassembled into frames by the driver's parser on
//       the way through.
//     * anything else       -> the D6 path, unchanged.
//   A Mid-70 project usually has BOTH, so this runs them as two legs: two
//   devices, two ReplaySources, two threads. ReplayConfig::chunk_type names a
//   single stream and ReplaySource::run() blocks, so one leg per stream is the
//   only way to replay them together — and it is also the honest one: the two
//   were captured concurrently, and serialising them would hand LIO all the
//   points and then all the gyro.
//
// THE SESSION IT STARTS
//   An empty lscan_dir — i.e. a live preview that records nothing. Recording a
//   replay into the project being replayed would append the same bytes back
//   into the file being read; SessionConfig documents the empty-dir case as
//   exactly this "tests and live previews" use.
//
// Owner: C1.
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "scanengine/record/lscan.h"
#include "scanengine/record/replay.h"

namespace lidarscan {

class EngineHost;

class ReplayController : public QObject {
  Q_OBJECT
 public:
  explicit ReplayController(EngineHost* host, QObject* parent = nullptr);
  ~ReplayController() override;

  // speed: 1.0 = the capture's own pacing; <= 0 = as fast as the engine decodes.
  bool start(const QString& lscan_dir, double speed, QString* err);
  void stop();
  bool running() const { return running_.load(); }
  const QString& projectDir() const { return dir_; }

 Q_SIGNALS:
  void started(const QString& dir, double speed);
  void finished(const QString& summary);

 private:
  void poll();
  void teardown();

  // One stream being replayed into one device on one thread. The primary leg
  // (`legs_[0]`) is the lidar {M} D6 or Mid-70 {M} and is always present; a
  // second leg carries the serial IMU when the container has one.
  struct Leg {
    scanengine::DeviceId device = scanengine::kInvalidDeviceId;
    scanengine::lscan::ChunkType chunk_type = scanengine::lscan::ChunkType::kD6Raw;
    const char* label = "";
    std::unique_ptr<scanengine::lscan::ReplaySource> source;
    std::thread thread;
    std::atomic<bool> done{false};
    scanengine::ScanError result = scanengine::ScanError::kOk;
  };

  void startLeg(Leg& leg);
  bool allLegsDone() const;
  void joinLegs();

  EngineHost* host_ = nullptr;
  QString dir_;
  double speed_ = 1.0;
  // unique_ptr because Leg holds a std::thread and a std::atomic, neither of
  // which is movable in a way a vector reallocation would tolerate.
  std::vector<std::unique_ptr<Leg>> legs_;
  std::atomic<bool> running_{false};
  QTimer poll_timer_;
};

}  // namespace lidarscan
