// CaptureWindow.h — the capture panel (Tech Spec §3.13 "Capture window"), as
// redesigned by docs/design/REVIEW_FEEDBACK.md's 2026-08-17 **round 5** (owner,
// after the first full hardware session with both sensors). NOTES.md §17 is the
// long-form account of that pass; this header is the contract.
//
// WHAT ROUND 5 CHANGED, IN ONE PLACE
//
// 1. NO POPUPS, FEWER STEPS (item 7). This is a QDockWidget pinned across the
//    bottom of the main window, not a QDialog floating over it — the live
//    viewport it is driving is directly above it, and the A14 display dock sits
//    beside it. The auto-detect progress dialog is gone too: discovery reports
//    into an inline status row (a label + an indeterminate bar) in this panel.
//    There is no modal anywhere in the capture path.
//
// 2. TAB ROLES (item 8). Capture creates new scan projects and does nothing
//    else. The project LIBRARY and the preview of a recorded scan belong to the
//    Projects workspace; this panel has no project list, no open/replay
//    controls and no file picker.
//
// 3. EVERY START IS A NEW PROJECT (item 9). One optional "Project name" field
//    plus one big Start. An empty name auto-names the project from a persisted,
//    monotonically increasing series number plus the date and time —
//    `Scan-014 2026-08-17 19-32.lscan`, colons replaced because Windows
//    forbids them in a path and macOS Finder shows them as '/'. The series
//    number lives in QSettings ("capture/seriesNumber"), so it survives
//    restarts and never repeats.
//
// 4. LIVE PREVIEW BEFORE (AND DURING) RECORDING (item 10). Opening the
//    workspace runs auto-detect inline, and a Mid-360 that answers is ARMED
//    automatically into a live-preview session (EngineHost::startSession with
//    an empty lscan_dir, record=false — the pattern ReplayController uses):
//    points stream into the viewport, nothing hits disk. The display
//    parameters are adjustable throughout — point size / gamma / brightness /
//    colour mode / colormap inline here, the full A14 set in the DISPLAY dock
//    next to it, and the live REFRESH RATE (ViewportWindow::setMaxFps, which
//    this pass added) which is not an A14 parameter because it is a property of
//    the window, not of the cloud.
//
// 5. NO SELF-TEST GATE (item 7, "live preview showing points IS the proof a
//    device works"). "Test device" and the SELF-TEST REQUIRED badge are gone
//    from ui/RecordCluster. What remains of the old self-test is its
//    MEASUREMENT: arming still waits for the first packet and still reports
//    pass/fail through selfTestFinished(), because that signal is what
//    main.cpp's --mid360-selftest (CI + field evidence) waits on. PASS = first
//    packet arrived; the same code path a user's auto-arm takes.
//
// 6. D6 IS PHONE-ONLY (item 11, owner-verified: the D6 has no IMU, so 3D comes
//    from the phone's ARCore VIO plus the A8 pushbroom). Desktop CAPTURE is
//    Mid-360 + RTK(UM982) only: the COIN-D6 tab, its serial port picker, its
//    QSerialPort read path and its D6Config wiring are gone from this file.
//    Auto-detect still PROBES serial for a D6 (that probe also finds the
//    UM982) but a hit now renders as a passive information line — "capture it
//    with the phone app" — and offers no capture affordance. Desktop D6
//    support that is NOT capture (project replay, post-processing, merge,
//    ReplayController, Project::importRawD6) is untouched.
//
// WHAT DID NOT CHANGE
//
// RECORD-ALWAYS (Tech Spec §3 key rule 2): every phase other than live preview
// is either fully recording or fully paused. Pause = stop the recording session
// and start a fresh preview one (the device keeps streaming into the viewport,
// nothing more is written); Resume = the reverse, at the SAME project
// directory. record/lscan.h's writer appends on every open(), so a paused
// capture's stream files grow across each resume and the manifest's `sealed`
// flag toggles false/true each time — A5's documented contract, not a bug.
//
// SESSION SUMMARY: duration is wall-clock time actually spent Recording (paused
// intervals excluded); bytes/chunks are accumulated per segment from
// Engine::recorder().stats(), which FileRecordWriter resets on every open();
// points are DeviceHealth::points_out relative to the Start baseline, which
// also counts points that arrived during a pause (there is no per-driver
// "points written to disk" counter on the C++ API — see NOTES.md).
//
// DISCOVERY <-> DEVICE SERIALIZATION (NOTES.md §16.7) is preserved exactly:
// scanengine::discovery and the Livox SDK's push channel both want UDP 56201,
// so a pass in flight is cancelled — and WAITED FOR, until its socket is
// provably closed — before anything arms a device, and a pass may not start
// while a device session is live. Round 5 only added the third case the new
// flow creates: a manual auto-detect click while merely PREVIEWING disarms the
// preview first (and re-arms afterwards) instead of being refused, because
// after auto-arm the operator would otherwise never be able to re-run
// auto-detect at all. A recording is still never interrupted.
//
// Owner: C1 (skeleton) / C2 (the capture machine) / round-5 workflow pass.
#pragma once

#include <QDockWidget>
#include <QElapsedTimer>
#include <QString>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "app/DisplayAwake.h"
#include "app/WalkSpeedEstimator.h"

#include "scanengine/cloud/display_params.h"
#include "scanengine/core/types.h"
#include "scanengine/record/lscan.h"

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QThread;
class QTimer;
class QVBoxLayout;

namespace scanengine {
// Forward-declared rather than included: drivers/mid360/mid360_driver.h is an
// engine DRIVER header, and this panel's header has never named one. The
// definition is only needed in CaptureWindow.cpp (which includes EngineHost.h),
// so a unique_ptr to an incomplete type plus an out-of-line destructor keeps it
// that way. Round-5 field bug C needs the config kept so a re-arm can re-add the
// device it already had.
struct Mid360Config;
}  // namespace scanengine

namespace lidarscan {

class EngineHost;
class RecordCluster;
class SliderRow;
struct DiscoveryResult;  // app/DeviceDiscovery.h — kept out of this header on
                          // purpose so CaptureWindow.h never names an engine
                          // discovery type; see DeviceDiscovery.h's file
                          // comment.
class DiscoveryGate;      // ditto — the cancel token, app/DeviceDiscovery.h

class CaptureWindow : public QDockWidget {
  Q_OBJECT
 public:
  // `params` is MainWindow's ONE scanengine::DisplayParamsController — the same
  // model DisplayParamsDock and InspectorCard bind to. There is no second
  // display model anywhere in the app (NOTES.md §1.7), so the inline live
  // controls in this panel and the full A14 dock beside it cannot disagree.
  CaptureWindow(EngineHost* host, scanengine::DisplayParamsController* params,
                QWidget* parent = nullptr);
  ~CaptureWindow() override;

  // Where new projects are created. Round 5: capture no longer BELONGS to a
  // project (every Start makes a new one), so this only sets the parent
  // directory — passing a .lscan path uses its parent. Empty leaves the
  // persisted default ("capture/root", itself defaulting to ~/LidarScan).
  void setProjectDir(const QString& dir);

  // The model changed somewhere else (the A14 dock, the inspector card, a
  // profile, a loaded JSON document): re-read the inline live controls.
  void refreshDisplayControls();

  // Round-5 item 17: the refresh slider's MAXIMUM is this machine's own display
  // refresh rate (QScreen::refreshRate, handed in by MainWindow because only it
  // knows which screen the viewport is on). Re-ranges the row and clamps the
  // current value into it.
  void setLiveRefreshCeiling(double hz);
  // Round-5 item 17: the viewport's measured refresh governor moved the
  // effective cap — down (it cannot sustain the current one) or back up (field
  // bug B: it now recovers). Moves the slider to match (without re-emitting a
  // change) and shows a quiet inline note — never a dialog, never a warning
  // sound, and never anything that touches the recording. It does NOT persist:
  // the saved setting is what the operator asked for, not what a bad minute
  // measured.
  void noteRefreshGovernor(double hz, bool down, const QString& why);

  // Re-emit the live refresh rate so the viewport picks up the PERSISTED value.
  // Called once by MainWindow right after it connects: the rate is restored from
  // QSettings inside this widget's constructor, i.e. before anything is
  // connected to hear about it, and a throttle nobody was told about is not a
  // throttle. (Found by the first evidence run, where a second run inherited
  // "24 fps" in the panel while the viewport was still uncapped.)
  void applyLiveRefreshRate();

 Q_SIGNALS:
  // Emitted once recording actually begins (not on arm/preview).
  void captureStarted(const QString& project_dir);
  // Carries the sealed project directory so the Projects workspace can list and
  // preview it immediately (round 5 item 9: "Stop = seal -> project appears in
  // Projects"). Empty if the session never recorded anything.
  void captureStopped(const QString& project_dir);
  // A live-preview session just started: the viewport should mirror the live
  // PageStore. Recording emits captureStarted() instead (and MainWindow points
  // the viewport at the same store either way).
  void previewStarted();
  // Every Phase transition into or out of kRecording/kPaused. MainWindow
  // renders it as the RECORDING / PAUSED badge on the viewport — the badge is a
  // VIEW of this phase, never a second copy of it.
  void recordingStateChanged(bool recording, bool paused);
  // One of the inline live display controls moved the shared A14 model.
  void displayParamsChanged();
  // The live refresh rate moved. NOT an A14 parameter (see the file header):
  // MainWindow forwards it to ViewportWindow::setMaxFps(), the one place that
  // can throttle presentation.
  void liveRefreshHzChanged(double hz);
  // What used to be the self-test verdict, kept verbatim as a SIGNAL because
  // main.cpp's --mid360-selftest (CI + field evidence) waits on it. It now
  // reports the arm-and-first-packet result rather than gating anything: PASS =
  // the first packet arrived and live preview is up.
  void selfTestFinished(bool passed, const QString& detail);
  // Fires after every completed discovery pass — the "Auto-detect devices"
  // button AND the automatic run on open alike — once the result has been
  // applied to the UI. What --auto-detect-selftest (main.cpp) waits on.
  void autoDetectFinished(bool mid360Found, bool d6Found, bool um982Found);
  // Round-5 item 18: the accumulated walking path, in the session's local metric
  // frame. MainWindow hands it to ViewportWindow::setTrajectoryTrail(), which
  // coalesces it into one geometry rebuild per presented frame.
  void trajectoryTrailChanged(const std::vector<std::array<float, 3>>& path);
  // Round-5 field bug A evidence: every 10 Hz trajectory poll's verdict, exactly
  // as the walk hint renders it. `valid` is false while the estimator has not
  // yet seen a full window of pose time in one frame (a hint is never shown
  // then). --walk-soak (main.cpp) listens to this against the S2 simulator's
  // STATIONARY stream, where the correct answer is 0.00 m/s for the whole soak.
  void walkSpeedMeasured(double mps, bool valid, unsigned discontinuities);

  // Capture's log line, for the shell's LOG dock. This panel keeps its own
  // compact log strip too; both render the same text, and log() also writes
  // stderr for headless runs.
  void logLine(const QString& line);

  // --- CLI test hooks (main.cpp) -------------------------------------------
 public:
  // --mid360-selftest: fills the Mid-360 addresses in and ARMS, i.e. the
  // arm+preview path a user reaches by opening the workspace. selfTestFinished()
  // fires on the first packet (PASS) or the arm timeout (FAIL).
  void runMid360SelfTestForCli(const QString& hostIp, const QString& lidarIp);
  // --mid360-record-into: Start with an EXPLICIT project directory (which is
  // otherwise only ever auto-derived, since there is no file picker any more).
  void triggerRecordForCli(const QString& projectDir);
  // --capture-flow-demo: Start with the name field left EMPTY, i.e. the
  // auto-naming path (round 5 item 9). Returns the directory it created.
  QString triggerStartWithAutoNameForCli();
  void triggerPauseResumeForCli();
  void triggerStopForCli();
  // Drives the SAME "Auto-detect devices" button a click would.
  void triggerAutoDetectForCli();
  // Drives the real live-refresh-rate and point-size controls through their own
  // signal paths (the same code a drag runs), so an evidence run proves the
  // inline controls are bound to the live model rather than decorating it.
  double setLiveRefreshHzForCli(double hz);
  double setPointSizeForCli(double px);
  // Round-5 item 18 evidence: seed the trail with a known path. It goes into the
  // panel's OWN trail buffer (not straight to the viewport) because the 10 Hz
  // poll would otherwise overwrite it a tenth of a second later — the poll then
  // extends this path exactly as it extends a real one. Empty clears it.
  void injectTrailForCli(const std::vector<std::array<float, 3>>& path);

  // main.cpp calls this when a device-ARMING CLI hook owns the run: the
  // on-open discovery pass must not fire at all, because the hook is about to
  // bind UDP 56201 through the Livox SDK and a pass holding that port faults
  // the device (NOTES.md §16.7).
  void suppressSilentAutoDetectForCli();
  // main.cpp calls this when a CLI hook arms the device ITSELF: a discovery hit
  // must not also auto-arm, or two things race for the same port and for the
  // same phase machine. Interactive runs always auto-arm; this is the one
  // difference between a CLI evidence run and a user's.
  void suppressAutoArmForCli();

 private:
  // The live-preview machine. There is no kReady/kTesting gate pair any more:
  // arming leads straight to a live preview, and Start is available from it.
  enum class Phase { kIdle, kArming, kPreview, kRecording, kPaused };

  void buildUi();
  QWidget* buildDeviceColumn();
  QWidget* buildLinkColumn();
  QWidget* buildScanColumn();
  QWidget* buildDisplayColumn();

  // Arm the Mid-360 into a live-preview session. Everything that starts a
  // device goes through here (auto-arm after discovery, Reconnect, Start from
  // idle, the CLI self-test hook), so the discovery serialization and the
  // first-packet measurement exist exactly once.
  bool armPreview(QString* err);
  // Give UDP 56201 back to discovery: stop the session, remove the device.
  // Refuses while recording (nothing may interrupt a recording).
  bool disarmPreview(const QString& why);

  void onStart();          // the big pill: create a NEW project + record
  void onPauseResume();
  void onStop();
  void onConnect();        // the manual row's Connect / a retry after "no data"
  // Round-5 follow-up item 1: the inline manual fallback. Opens BY ITSELF when a
  // detect pass finds nothing (the operator is not left staring at "not seen"
  // with nowhere to type), and is reachable at any time from the "Manual setup"
  // toggle even when detection succeeded — so a second lidar, a hand-configured
  // host alias or a link auto-detect got wrong can always be entered. Inline,
  // never a dialog.
  void setManualSetupOpen(bool open, bool focus = false);

  void updateHealth();
  // Round-5 item 18 (walkthrough-first): poll the live-SLAM trajectory and hand
  // the path to the viewport, plus derive the walking speed for the "slow down"
  // hint. Runs on its own 10 Hz timer while a device is armed.
  void pollTrajectory();
  // Drop the accumulated trail and the speed window. Called on every engine
  // session (re)start — Start, Pause, Resume, Stop and arm all restart
  // LioOdometry, and its poses come back numbered from the origin.
  void resetWalkTracking(const char* why);
  void evaluateArming();   // first-packet-or-timeout, and the selfTestFinished signal
  void log(const QString& s);
  void setPhase(Phase p);
  void updateRecordCluster();
  void updateNameHint();
  double recordedSecondsNow() const;

  // Round 5 item 9. `<root>/Scan-014 2026-08-17 19-32.lscan` when the name
  // field is empty; `<root>/<typed name>.lscan` otherwise. Never returns a path
  // that already exists (a numeric suffix is appended if it would).
  QString resolveNewProjectDir(const QString& typedName, bool* auto_named);
  QString captureRoot() const;

  bool startPreviewSession(QString* err);
  bool startRecordingSession(QString* err);
  void accumulateRecorderStats();

  // --- round-5 field bug C: "it only record when the first connected" -------
  //
  // Engine::start_session() STARTS every registered device and stop_session()
  // STOPS them, so every Start / Pause / Resume / Stop tears the Mid-360's Livox
  // SDK2 backend down and brings it back up (the engine log shows "SDK2 torn
  // down" / "SDK2 up" once per transition). On loopback the device is streaming
  // again in milliseconds; on a real link it is not, and if the device is still
  // in kStarting for the whole of a short recording, the .lscan is sealed with
  // ZERO chunks while the panel cheerfully showed REC and an elapsed clock.
  // Reproduced exactly (spikes/s2-mid360-sim --drop-link-after 8 --link-down-for
  // 6 --repeat): cycles 2 and 5 of 6 recorded 0 chunks, device state
  // "starting -> stopping", never "streaming".
  //
  // So: after EVERY session restart, watch for the device's points to start
  // moving again, and if they do not, re-arm the device IN PLACE — remove_device
  // + add_device, which Engine starts immediately mid-session (engine.cpp: "a
  // device added mid-session starts immediately"). That leaves the recorder open
  // and the .lscan intact; only the driver is rebuilt. Nothing here can stop a
  // recording, and every raw byte that does arrive is still written.
  void beginDataWatch(const QString& why);
  void updateDataWatch();
  bool rearmDeviceInPlace(QString* err);
  void endDataWatch();

  // Mid-360 link fields, persisted GLOBALLY now rather than per project: a
  // project is created BY a capture in this flow, so there is no project whose
  // settings could be loaded before one exists. Group "mid360/last".
  void loadMid360Settings();
  void saveMid360Settings();

  // --- inline live display controls (round 5 item 10) ---------------------
  void pushDisplayParams();

  // --- Auto-detect (round 4 item 5; inline since round 5 item 7) ----------
  void buildAutoDetectSection(QVBoxLayout* v);
  void onAutoDetectClicked();
  void startDiscovery(bool silent);
  void handleDiscoveryFinished(const DiscoveryResult& r, bool silent);
  // --- Discovery <-> device serialization (NOTES.md §16.7) ----------------
  //
  //   * startDiscovery() refuses outright while a device session is live.
  //   * stopDiscoveryForDeviceUse() runs before every engine call that can arm
  //     a device and BLOCKS until the worker's socket is actually closed — a
  //     canceled-but-still-bound socket faults the device exactly as a running
  //     one does. False = the port did not come free, which the caller reports
  //     rather than papering over.
  bool stopDiscoveryForDeviceUse(const QString& what);
  void setAutoDetectStatus(const QString& text, const char* tone);
  void setDiscoveryRunning(bool running, const QString& phase_label);
  void applyMid360Result(const DiscoveryResult& r, bool silent);
  void applyD6Result(const DiscoveryResult& r);
  void applyUm982Result(const DiscoveryResult& r, bool silent);
  void showEvent(QShowEvent* event) override;

  EngineHost* host_ = nullptr;
  scanengine::DisplayParamsController* params_ = nullptr;
  QTimer* health_timer_ = nullptr;
  QTimer* trajectory_timer_ = nullptr;
  // Round-5 item 18: the display must not sleep while the operator is walking
  // the space. Held for as long as a device is armed. app/DisplayAwake.h is
  // explicit about which platforms really do it.
  DisplayAwake awake_;

  QString project_root_;      // where new projects are created
  QString last_project_dir_;  // the most recent recording's directory
  // Set by triggerRecordForCli() only: the ONE way an explicit project path can
  // enter this flow (--mid360-record-into). Consumed by the next onStart().
  QString last_cli_project_dir_;
  scanengine::DeviceId device_ = scanengine::kInvalidDeviceId;

  Phase phase_ = Phase::kIdle;

  // arm bookkeeping (what used to be the self-test)
  QElapsedTimer arm_clock_;
  double arm_window_s_ = 8.0;   // Mid-360: first packet before the A3 connect timeout
  std::uint64_t arm_baseline_points_ = 0;

  // Post-restart data watch (field bug C). `last_mid360_cfg_` is what a re-arm
  // re-adds: exactly the config the current device was opened with.
  std::unique_ptr<scanengine::Mid360Config> last_mid360_cfg_;
  QElapsedTimer data_watch_clock_;
  std::uint64_t data_watch_baseline_points_ = 0;
  bool data_watch_active_ = false;
  double data_watch_window_s_ = 6.0;  // doubles, capped, after each failed re-arm
  int rearm_attempts_ = 0;
  QString data_watch_why_;

  // recording-session bookkeeping (see the file header's "SESSION SUMMARY")
  QElapsedTimer record_segment_clock_;
  double recorded_seconds_accum_ = 0.0;
  std::uint64_t record_baseline_points_ = 0;
  std::uint64_t cum_bytes_written_ = 0;
  std::uint64_t cum_chunks_written_ = 0;

  // --- Auto-detect --------------------------------------------------------
  QPushButton* auto_detect_btn_ = nullptr;
  QWidget* auto_detect_panel_ = nullptr;
  QLabel* auto_detect_status_line_ = nullptr;   // why a pass did not run / was cut short
  QLabel* discovery_phase_label_ = nullptr;     // "Listening for Mid-360 heartbeat…"
  QProgressBar* discovery_bar_ = nullptr;       // indeterminate, INLINE (no dialog)
  QLabel* auto_detect_mid360_line_ = nullptr;
  QLabel* auto_detect_d6_line_ = nullptr;
  QLabel* auto_detect_um982_line_ = nullptr;
  QLabel* auto_detect_fix_line_ = nullptr;
  QPushButton* auto_detect_copy_btn_ = nullptr;
  QPushButton* manual_toggle_ = nullptr;   // "Manual setup" — always available
  QWidget* manual_box_ = nullptr;          // the inline lidar/host IP + Connect row
  QPushButton* connect_btn_ = nullptr;
  QString auto_detect_copy_payload_;
  QThread* discovery_thread_ = nullptr;
  bool discovery_in_flight_ = false;
  std::shared_ptr<DiscoveryGate> discovery_gate_;
  bool discovery_canceled_ = false;
  bool suppress_silent_auto_detect_ = false;
  bool suppress_auto_arm_ = false;
  // Set when a manual pass disarmed a live preview to get the port: the pass's
  // completion handler re-arms rather than leaving the operator with a dead
  // viewport.
  bool rearm_after_discovery_ = false;
  bool auto_detect_ran_for_session_ = false;
  bool had_saved_mid360_settings_ = false;

  // Mid-360 link
  QLineEdit* host_ip_ = nullptr;
  QLineEdit* lidar_ip_ = nullptr;
  QSpinBox* point_port_ = nullptr;
  QSpinBox* imu_port_ = nullptr;
  QSpinBox* cmd_port_ = nullptr;
  QLabel* mid_hint_ = nullptr;

  // RTK (UM982) — auto-detect prefill only in this build; there is no
  // engine-side GNSS serial wiring on the desktop capture path yet (unlike the
  // Mid-360, GnssSource takes pushed NMEA bytes, not a device this panel
  // opens/closes — see NOTES.md §16.2). The fields exist so a UM982 hit has
  // somewhere honest to land instead of being silently dropped.
  QComboBox* um982_port_ = nullptr;
  QSpinBox* um982_baud_ = nullptr;
  QLabel* um982_heading_ = nullptr;
  QLabel* um982_hint_ = nullptr;

  // new scan
  QLineEdit* name_edit_ = nullptr;
  QLabel* name_hint_ = nullptr;   // "-> <root>/Scan-014 ….lscan"
  QComboBox* profile_ = nullptr;
  QLabel* arm_label_ = nullptr;   // "waiting for the first packet…" / "live"
  QLabel* health_ = nullptr;
  QLabel* summary_ = nullptr;

  // inline live display controls, bound to params_
  SliderRow* refresh_hz_ = nullptr;
  QLabel* refresh_note_ = nullptr;   // the auto-downshift note (item 17)
  SliderRow* point_size_ = nullptr;
  SliderRow* gamma_ = nullptr;
  SliderRow* brightness_ = nullptr;
  QComboBox* color_mode_ = nullptr;
  QComboBox* colormap_ = nullptr;
  bool updating_display_ = false;

  // walkthrough state (item 18)
  QLabel* walk_label_ = nullptr;          // trail length + speed + the "slow down" hint
  std::vector<std::array<float, 3>> trail_;
  // Round-5 field bug A: the speed is measured by this, from POSE timestamps
  // over a windowed net displacement — never from two consecutive polls on a
  // wall clock. See WalkSpeedEstimator.h for the five separate faults that
  // produced "walking" from a stationary tripod.
  WalkSpeedEstimator walk_;
  bool live_slam_running_ = false;
  // Rate-limits the "live odometry is drifting" note to once a minute.
  QElapsedTimer drift_clock_;
  // Set whenever the engine session is (re)started, which restarts LioOdometry
  // at the origin: the trail and the speed window belong to the OLD pose frame
  // and must not be carried across. Cleared by the first pose of the new frame.
  bool walk_frame_reset_pending_ = true;

  RecordCluster* record_cluster_ = nullptr;
  QTimer* elapsed_timer_ = nullptr;
  bool last_arm_failed_ = false;

  QPlainTextEdit* log_ = nullptr;
};

}  // namespace lidarscan
