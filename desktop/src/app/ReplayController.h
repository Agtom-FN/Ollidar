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

  EngineHost* host_ = nullptr;
  QString dir_;
  double speed_ = 1.0;
  scanengine::DeviceId device_ = scanengine::kInvalidDeviceId;
  std::unique_ptr<scanengine::lscan::ReplaySource> source_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> done_{false};
  scanengine::ScanError result_ = scanengine::ScanError::kOk;
  QTimer poll_timer_;
};

}  // namespace lidarscan
