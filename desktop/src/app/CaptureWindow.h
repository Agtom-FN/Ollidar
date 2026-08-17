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
#include <memory>

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
class QShowEvent;
class QSpinBox;
class QTabWidget;
class QThread;
class QTimer;
class QVBoxLayout;

namespace lidarscan {

class EngineHost;
class RecordCluster;
struct DiscoveryResult;  // app/DeviceDiscovery.h — kept out of this header on
                          // purpose so CaptureWindow.h never names an engine
                          // discovery type; see DeviceDiscovery.h's file
                          // comment.
class DiscoveryGate;      // ditto — the cancel token, app/DeviceDiscovery.h

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
  // Fires after every completed discovery pass — the manual "Auto-detect
  // devices" button AND the silent on-open run alike — once the result has
  // been applied to the UI (fields prefilled, summary text set). What
  // --auto-detect-selftest (main.cpp) waits on for a headless run.
  void autoDetectFinished(bool mid360Found, bool d6Found, bool um982Found);

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
  // Drives the SAME "Auto-detect devices" button a click would (worker
  // thread, real discovery.h calls), then autoDetectFinished() fires once
  // the result has been applied to the UI. Evidence hook for
  // --auto-detect-selftest (main.cpp).
  void triggerAutoDetectForCli();
  // main.cpp calls this when a device-ARMING CLI hook (--mid360-selftest and
  // friends) owns this run: the silent on-open discovery pass must not fire
  // at all, because the CLI hook is about to bind UDP 56201 through the Livox
  // SDK and a discovery pass holding that port faults the device. See
  // NOTES.md §16.7. Not a UI affordance — there is no way to reach it from
  // the window itself.
  void suppressSilentAutoDetectForCli();

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

  // --- Auto-detect (docs/design/REVIEW_FEEDBACK.md 2026-08-17 round 4 item
  // 5) --------------------------------------------------------------------
  //
  // buildAutoDetectSection() builds the button + the persistent summary
  // panel beneath it (SN/fw confirmation, per-sensor "not seen" causes, the
  // host-IP fix line + its copy button). startDiscovery() moves a
  // DiscoveryWorker (app/DeviceDiscovery.h) onto a throwaway QThread —
  // `silent` distinguishes the manual button (progress dialog, overwrites
  // every field with whatever this pass found) from the once-per-project
  // auto-run on open (no dialog, prefill-only: a field already holding a
  // non-default value is left alone). applyMid360Result/applyD6Result/
  // applyUm982Result are shared by both paths and are individually
  // overwrite-guarded so a silent run can safely fill in only the sensors
  // that are still at their factory defaults.
  void buildAutoDetectSection(QVBoxLayout* v);
  void onAutoDetectClicked();
  void startDiscovery(bool silent);
  void handleDiscoveryFinished(const DiscoveryResult& r, bool silent);
  // --- Discovery <-> device serialization (NOTES.md §16.7) -----------------
  //
  // Discovery binds UDP 56201 and so does the Livox SDK's push channel; on
  // macOS the second bind loses (`bind failed` -> device fault). These two
  // are the whole of the mutual exclusion, and it runs BOTH ways:
  //   * startDiscovery() refuses outright while a device session is live.
  //   * stopDiscoveryForDeviceUse() is called before every engine call that
  //     can arm a device, and BLOCKS until the worker's socket is actually
  //     closed — a canceled-but-still-bound socket faults the device exactly
  //     as a running one does. Returns false if the port did not come free,
  //     which the caller reports rather than papers over.
  bool stopDiscoveryForDeviceUse(const QString& what);
  void setAutoDetectStatus(const QString& text, const char* tone);
  void closeAutoDetectProgress();
  void applyMid360Result(const DiscoveryResult& r, bool silent);
  void applyD6Result(const DiscoveryResult& r, bool silent);
  void applyUm982Result(const DiscoveryResult& r, bool silent);
  void showEvent(QShowEvent* event) override;

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

  // --- Auto-detect --------------------------------------------------------
  QPushButton* auto_detect_btn_ = nullptr;
  QWidget* auto_detect_panel_ = nullptr;   // hidden until the first pass returns
  // Why the last pass did not run / did not finish — "a capture session is
  // running", "canceled so Test device could take UDP 56201". Empty and
  // hidden when the pass simply ran, in which case the per-sensor lines below
  // say everything.
  QLabel* auto_detect_status_line_ = nullptr;
  QLabel* auto_detect_mid360_line_ = nullptr;
  QLabel* auto_detect_d6_line_ = nullptr;
  QLabel* auto_detect_um982_line_ = nullptr;
  QLabel* auto_detect_fix_line_ = nullptr;       // the ifconfig-alias remedy, when shown
  QPushButton* auto_detect_copy_btn_ = nullptr;
  QString auto_detect_copy_payload_;
  QDialog* auto_detect_progress_ = nullptr;      // manual-run-only; null while idle
  QLabel* auto_detect_progress_label_ = nullptr;
  QThread* discovery_thread_ = nullptr;
  bool discovery_in_flight_ = false;
  // The gate for the pass in flight (app/DeviceDiscovery.h). Non-null exactly
  // while discovery_in_flight_ is true; shared with the worker so cancelling
  // is safe even as the worker tears itself down.
  std::shared_ptr<DiscoveryGate> discovery_gate_;
  // Set by stopDiscoveryForDeviceUse(): the finished() still queued behind us
  // carries a PARTIAL result that must not be applied to any field.
  bool discovery_canceled_ = false;
  // Set by suppressSilentAutoDetectForCli().
  bool suppress_silent_auto_detect_ = false;
  // Reset in setProjectDir(): the silent on-open auto-run fires at most once
  // per project per time this window is shown, not once per process.
  bool auto_detect_prompted_for_project_ = false;
  // Set by loadMid360Settings(): true once THIS project has ever had a
  // Mid-360 host/lidar IP saved. "Never-configured" for the silent auto-run
  // gate means this is false — a fresh project, still on the hard-coded
  // placeholder addresses below.
  bool had_saved_mid360_settings_ = false;

  QTabWidget* tabs_ = nullptr;

  // D6
  QComboBox* port_ = nullptr;
  QSpinBox* baud_ = nullptr;
  QCheckBox* send_commands_ = nullptr;
  QLabel* port_hint_ = nullptr;
  QLabel* ch340_hint_ = nullptr;
  QLabel* d6_auto_tag_ = nullptr;   // "auto-detected" tag, shown after a hit

  // Mid-360
  QLineEdit* host_ip_ = nullptr;
  QLineEdit* lidar_ip_ = nullptr;
  QSpinBox* point_port_ = nullptr;
  QSpinBox* imu_port_ = nullptr;
  QSpinBox* cmd_port_ = nullptr;
  QLabel* mid_hint_ = nullptr;

  // RTK (UM982) — auto-detect prefill only in this build; there is no
  // engine-side GNSS serial wiring on the desktop capture path yet (unlike
  // D6/Mid-360, GnssSource takes pushed NMEA bytes, not a device this window
  // opens/closes — see NOTES.md). The tab exists so a UM982 auto-detect hit
  // has somewhere honest to land instead of being silently dropped.
  QComboBox* um982_port_ = nullptr;
  QSpinBox* um982_baud_ = nullptr;
  QLabel* um982_heading_ = nullptr;   // "dual-antenna heading: yes/no/unknown"
  QLabel* um982_hint_ = nullptr;

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
