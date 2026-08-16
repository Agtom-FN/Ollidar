// CaptureWindow.h — the C2 capture window (Tech Spec §3.13 "Capture window
// (both sensors, same wizards, per-OS driver guidance)").
//
// C1 left a functional skeleton: port picker, D6 serial read/write, Mid-360
// config, one Start/Stop pair, a one-line health label. C2 turns that into
// the guided flow the spec asks for:
//
//   [choose device + project] -> [Test device] -> (self-test passes)
//     -> [Record] -> [Pause]/[Resume] -> [Stop] -> session summary
//
// SELF-TEST, NOT "START RECORDING" DIRECTLY
//   "Test device" starts a LIVE-PREVIEW session (EngineHost::startSession
//   with an empty lscan_dir, record=false — the same pattern
//   ReplayController uses) and adds the device, which the engine
//   auto-starts because add_device() starts a device immediately when a
//   session is already active (core/engine.cpp). Points stream into the
//   viewport and the health panel updates, but nothing hits disk yet. Once
//   the self-test window says the device is producing data at a reasonable
//   rate (D6: ~4k pts/s within 3 s; Mid-360: first packet before the A3
//   connect timeout), "Record" unlocks.
//
//   "Record" stops the preview session and starts a real one at the chosen
//   project directory with record=true. The device is NOT removed and
//   re-added between phases — Engine::start_session() restarts every
//   still-registered device — so the serial port / UDP configuration never
//   has to be redone. This is also how Pause/Resume work: pause = stop the
//   recording session and start a fresh live-preview one (device keeps
//   streaming into the viewport, nothing more is written to
//   streams/lidar.bin); resume = stop preview, start recording again at the
//   SAME project directory. record/lscan.h's writer appends on every
//   open() (never rewrites a previously-completed chunk), so a paused
//   capture's stream files simply grow across each resume; the manifest's
//   `sealed` flag toggles false/true each time, which is exactly A5's
//   documented "still sealed:false is a crash signal" contract working as
//   designed, not a bug in this flow.
//
// RECORD-ALWAYS
//   Tech Spec §3 key rule 2. There is no "off-the-record" capture mode:
//   every phase other than the self-test preview is either fully recording
//   or fully paused (which stops the device from adding to the .lscan at
//   all, but does not create a partially-recorded gap inside a chunk — the
//   writer's framing makes "chunk written" atomic).
//
// SESSION SUMMARY
//   Duration is wall-clock time actually spent in the Recording state
//   (paused intervals excluded). Bytes/chunks come from
//   Engine::recorder().stats(), which FileRecordWriter resets to zero on
//   every open() (lscan.cpp) — so each pause/resume segment's stats are
//   accumulated into a running total here rather than read once at the end.
//   Points are DeviceHealth::points_out at Stop time relative to the
//   baseline captured when Record was first pressed — this is "points
//   decoded by the driver since recording began", which also counts any
//   points that arrived during a pause (the device keeps streaming into the
//   viewport while paused, just not to disk); the summary label says so
//   rather than implying more precision than the generic DeviceHealth API
//   can give (there is no per-driver "points written to disk" counter on
//   the C++ API surface — see NOTES.md).
//
// D6 (serial). QSerialPortInfo enumeration, auto-refreshed on a timer while
// idle, with a CH340 hint (Vendor ID 0x1A86, the WCH chip on the vendor's
// own USB-serial adapter) attached to matching ports. 230400 8N1
// (§2.1/§3.1). The read path is `readyRead` -> Engine::push_serial_bytes(),
// and the write path is a SerialWriteFn trampoline installed in D6Config so
// the driver can send its own AA 55 F0 0F / AA 55 F5 0A frames.
//
// Mid-360. host/lidar IP + port set into Mid360Config, persisted per
// project (QSettings, keyed by the project directory) so re-opening a
// project's capture window does not require re-typing the network the
// device is on. The S2 finding (an explicit lidar IP is required on
// macOS — broadcast discovery fails with EADDRNOTAVAIL) is stated and
// enforced. "Test device" here is exactly the flow described in
// engine/docs/A3-mid360-driver.md §7: engine add_device() (auto-starts,
// session active) + start, then report first-data-or-timeout, reading the
// generic DeviceHealth's state/points_per_sec — the A3-specific
// Mid360Stats::link (kWaiting/kUp/kSilent/...) is NOT reachable from the
// public Engine C++ API (Engine only stores drivers behind the base
// `Driver*` interface; there is no accessor for the concrete
// D6Driver/Mid360Driver instance), so the health panel and self-test work
// from the same DeviceHealth every device kind publishes. See NOTES.md.
//
// UNTESTED AGAINST HARDWARE for the D6 tab — none is present. The Mid-360
// tab IS exercised end to end against the S2 simulator on loopback; see
// NOTES.md for the evidence.
//
// Owner: C1 (skeleton) / C2 (this file).
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QString>

#include <cstdint>

#include "scanengine/core/types.h"
#include "scanengine/record/lscan.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSerialPort;
class QSpinBox;
class QTabWidget;
class QTimer;

namespace lidarscan {

class EngineHost;
class RecordCluster;

class CaptureWindow : public QDialog {
  Q_OBJECT
 public:
  explicit CaptureWindow(EngineHost* host, QWidget* parent = nullptr);
  ~CaptureWindow() override;

  void setProjectDir(const QString& dir);

 Q_SIGNALS:
  // Emitted once recording actually begins (not on self-test).
  void captureStarted(const QString& project_dir);
  void captureStopped();
  // Every Phase transition into or out of kRecording/kPaused. MainWindow
  // renders it as the RECORDING / PAUSED badge on the viewport (redesign
  // brief item 4) — the badge is a VIEW of this phase, never a second copy
  // of it.
  void recordingStateChanged(bool recording, bool paused);
  // Emitted once per self-test, pass or fail — this is what --mid360-selftest
  // (main.cpp) waits on for a headless CLI run against the S2 simulator.
  void selfTestFinished(bool passed, const QString& detail);

  // --- CLI test hook (main.cpp's --mid360-selftest) ------------------------
  //
  // Not part of the interactive UI flow: configures the Mid-360 tab and
  // clicks "Test device" programmatically, so a headless run can exercise
  // exactly the guided self-test a user would run by hand, against the S2
  // simulator on loopback. See NOTES.md for the evidence this produced.
 public:
  void runMid360SelfTestForCli(const QString& hostIp, const QString& lidarIp);
  // Only meaningful once selfTestFinished(true, ...) has fired (phase kReady).
  void triggerRecordForCli(const QString& projectDir);
  // Pause <-> Resume, whichever the current phase makes legal. Added for the
  // redesign's record-cluster evidence run, which has to photograph the
  // PAUSED state.
  void triggerPauseResumeForCli();
  void triggerStopForCli();

 private:
  enum class Phase { kIdle, kTesting, kReady, kRecording, kPaused };

  void buildUi();
  void refreshPorts();
  void applyCh340Hint();

  void onTestDevice();
  void onRecord();
  void onPauseResume();
  void onStop();
  void onSerialReadyRead();
  void updateHealth();
  void evaluateSelfTest();
  void log(const QString& s);
  void setPhase(Phase p);
  // Renders the current Phase (+ last_self_test_failed_) into the cluster.
  void updateRecordCluster();
  // Seconds spent in the Recording state so far, including the segment in
  // flight — the same accumulator + live segment the session summary sums, so
  // the ticking clock and the final summary agree by construction.
  double recordedSecondsNow() const;

  bool openDeviceForTab(QString* err);   // opens serial (D6) / configures cfg (Mid-360)
  void closeDevice();                    // tears down serial + removes the device
  bool startPreviewSession(QString* err);
  bool startRecordingSession(QString* err);
  void accumulateRecorderStats();        // called just before a recording segment closes

  void loadMid360Settings();
  void saveMid360Settings();

  // Trampoline installed in D6Config::serial.write_fn so the driver can send
  // its own start/stop command frames.
  static scanengine::ScanError serialWrite(const std::uint8_t* data, std::size_t n, void* user);

  EngineHost* host_ = nullptr;
  QSerialPort* serial_ = nullptr;
  QTimer* health_timer_ = nullptr;
  QTimer* port_refresh_timer_ = nullptr;

  QString project_dir_;
  scanengine::DeviceId device_ = scanengine::kInvalidDeviceId;
  bool device_is_d6_ = true;

  Phase phase_ = Phase::kIdle;

  // self-test bookkeeping
  QElapsedTimer self_test_clock_;
  bool self_test_passed_ = false;
  double self_test_window_s_ = 3.0;       // D6 default; Mid-360 uses a longer one
  std::uint64_t self_test_baseline_points_ = 0;

  // recording-session bookkeeping (see the file header's "SESSION SUMMARY")
  QElapsedTimer record_segment_clock_;
  double recorded_seconds_accum_ = 0.0;
  std::uint64_t record_baseline_points_ = 0;
  std::uint64_t cum_bytes_written_ = 0;
  std::uint64_t cum_chunks_written_ = 0;

  QTabWidget* tabs_ = nullptr;

  // D6
  QComboBox* port_ = nullptr;
  QSpinBox* baud_ = nullptr;
  QCheckBox* send_commands_ = nullptr;
  QLabel* port_hint_ = nullptr;
  QLabel* ch340_hint_ = nullptr;

  // Mid-360
  QLineEdit* host_ip_ = nullptr;
  QLineEdit* lidar_ip_ = nullptr;
  QSpinBox* point_port_ = nullptr;
  QSpinBox* imu_port_ = nullptr;
  QSpinBox* cmd_port_ = nullptr;
  QLabel* mid_hint_ = nullptr;

  // session
  QLineEdit* project_edit_ = nullptr;
  QComboBox* profile_ = nullptr;
  QCheckBox* live_mode_ = nullptr;

  // THE record cluster (redesign brief item 4 / REVIEW_FEEDBACK round-1 item
  // 2). It replaces the four equal-sized Test/Record/Pause/Stop buttons that
  // used to sit inside the Session form — there is now exactly one place in
  // this window where a capture is started or stopped.
  RecordCluster* record_cluster_ = nullptr;
  QTimer* elapsed_timer_ = nullptr;

  // Distinguishes "never tested" from "tested and failed" — both are
  // Phase::kIdle, and the record cluster's gate sentence differs between
  // them.
  bool last_self_test_failed_ = false;

  QLabel* self_test_label_ = nullptr;
  QProgressBar* self_test_progress_ = nullptr;

  QLabel* health_ = nullptr;
  QLabel* summary_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
};

}  // namespace lidarscan
