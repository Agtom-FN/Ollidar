// LidarScan desktop app (workstream C, task C1).
//
// Qt 6 Widgets + a native Filament viewport, linking libscanengine's C++ API
// directly (Tech Spec §3 key rule 1 — no FFI on the desktop side).
//
// The command-line flags exist so the whole app can be driven headlessly enough
// to produce its own verification evidence (see desktop/scripts/verify.sh):
//
//   --project DIR            open a .lscan project at startup
//   --import-raw FILE        import a raw D6 byte capture into --project first
//   --replay[=SPEED]         start replaying the opened project (0 = unpaced)
//   --shot PATH              screenshot the viewport into PATH after --shot-delay
//   --shot-delay SECONDS     default 6
//   --quit-after SECONDS     exit after N seconds (0 = never)
//   --vsync=on|off           default on
//   --size WxH               initial window size
//   --display-profile NAME   survey|floorplan|research|quickscan (A14 defaults)
//   --display-params FILE    load an A14 display-parameter JSON document
//   --resize-storm SECONDS   rerun S3's continuous-resize stability stress
//   --export FORMAT:PATH     C2/C3 evidence hook: after any --replay has had
//                            time to land points, call the SAME
//                            scanengine::export_points() entry point
//                            ExportDialog uses (ply|las|pcd), synchronously,
//                            and print the result. See NOTES.md.
//   --measure-selftest       C2/C3 evidence hook: after replay, turn on
//                            measure mode and synthesize two real QMouseEvent
//                            clicks through ViewportWindow's own event path
//                            (not a private-method call — this exercises
//                            exactly what a real click does), then print the
//                            resulting segment.
//
// Owner: C1 (flags above --resize-storm) / C2+C3 (--export, --measure-selftest).
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "app/CaptureWindow.h"
#include "app/DisplayParamsDock.h"  // paramsPathFor — --capture-flow-demo's saved-view check
#include "app/EngineHost.h"
#include "app/MainWindow.h"
#include "app/MergeDock.h"
#include "app/MergeFixture.h"
#include "app/PlanDock.h"
#include "app/ProcessingDock.h"
#include "app/Project.h"  // ProjectInfo/readProject for --mid360-selftest's post-record report
#include "app/QtHttpTransport.h"
#include "app/SyntheticMid360.h"
#include "app/TransferDialog.h"
#include "render/ViewportWindow.h"
#include "ui/InspectorCard.h"
#include "ui/Theme.h"
#include "scanengine/core/instance_guard.h"  // scanengine::InstanceGuard — see the check below
#include "scanengine/export/exporter.h"
#include "scanengine/jobs/job_queue.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/zip.h"

namespace {

// Mean ABSOLUTE per-pixel difference between two screenshots, plus the fraction
// of the frame that changed by more than a noise threshold. Used by two evidence
// hooks that must prove "this control actually changed what is on screen":
// --inspector-demo (point size) and --capture-flow-demo (the trajectory trail).
// Absolute, not signed: growing points or drawing a trail can make a frame
// DARKER (near surfaces occluding far ones), and a brightness test would call
// that a failure.
double frameDelta(const QString& a, const QString& b, double* changedFrac) {
  QImage ia(a), ib(b);
  if (ia.isNull() || ib.isNull() || ia.size() != ib.size()) return -1.0;
  ia = ia.convertToFormat(QImage::Format_Grayscale8);
  ib = ib.convertToFormat(QImage::Format_Grayscale8);
  double sum = 0.0;
  qint64 changed = 0;
  for (int y = 0; y < ia.height(); ++y) {
    const uchar* ra = ia.constScanLine(y);
    const uchar* rb = ib.constScanLine(y);
    for (int x = 0; x < ia.width(); ++x) {
      const int d = std::abs(int(ra[x]) - int(rb[x]));
      sum += d;
      if (d > 16) ++changed;
    }
  }
  const double n = double(ia.width()) * ia.height();
  if (changedFrac) *changedFrac = double(changed) / n;
  return sum / n;
}

// How many pixels in this shot are the trajectory trail's EMBER, i.e. strongly
// red-dominant? The cloud renders through a grayscale/intensity colormap, so
// r-b is ~0 for every cloud pixel and the count is a decisive test that the
// trail geometry reached the swapchain — unlike a frame delta, which a live
// capture's own growing cloud can produce all by itself.
qint64 countEmberPixels(const QString& path) {
  QImage im(path);
  if (im.isNull()) return -1;
  im = im.convertToFormat(QImage::Format_RGB32);
  qint64 n = 0;
  for (int y = 0; y < im.height(); ++y) {
    const QRgb* row = reinterpret_cast<const QRgb*>(im.constScanLine(y));
    for (int x = 0; x < im.width(); ++x) {
      const QRgb c = row[x];
      if (qRed(c) > 120 && int(qRed(c)) - int(qBlue(c)) > 60) ++n;
    }
  }
  return n;
}

// --walk-speed-selftest — round-5 field bug A, the unit test for the estimator
// CaptureWindow::pollTrajectory() now delegates to.
//
// This is the POSITIVE and NEGATIVE proof the simulator cannot give: mid360_sim
// models a STATIONARY sensor (its option list has rate/loss/jitter/noise/link
// faults and no motion at all — spikes/s2-mid360-sim/sim/mid360_sim.cpp), so the
// sim proves "stationary reads zero" against real LIO output and this proves
// "walking reads the walk", plus the four specific fault modes that produced
// "walking" from a tripod. No Qt, no engine, no device — pure arithmetic against
// WalkSpeedEstimator, so it runs anywhere, including CI with no display.
int runWalkSpeedSelfTest() {
  using lidarscan::WalkSpeedEstimator;
  int failures = 0;
  auto check = [&failures](const char* name, bool ok, const QString& detail) {
    std::fprintf(stderr, "[lidarscan] walk-speed-selftest: %-34s %s — %s\n", name,
                 ok ? "PASS" : "FAIL", detail.toUtf8().constData());
    if (!ok) ++failures;
  };
  // A deterministic, repeatable pseudo-noise so a failure can be reproduced.
  std::mt19937 rng(20260817u);
  std::normal_distribution<double> jitter_m(0.0, 0.015);  // 1.5 cm 1-sigma per axis

  // 1. STATIONARY, 60 s at 10 Hz, with LIO-scale pose jitter. The hint fires at
  //    1.5 m/s; the requirement is far stronger — it must read a hard zero.
  {
    WalkSpeedEstimator est;
    double peak = 0.0;
    int fired = 0;
    for (int i = 0; i < 600; ++i) {
      const double p[3] = {jitter_m(rng), jitter_m(rng), jitter_m(rng)};
      est.update(std::int64_t(i) * 100'000'000LL, p);
      if (est.valid()) {
        peak = std::max(peak, est.speedMps());
        if (est.speedMps() > 1.5) ++fired;
      }
    }
    check("stationary 60 s (1.5 cm jitter)", peak == 0.0 && fired == 0,
          QString("peak %1 m/s, hint fired %2x").arg(peak, 0, 'f', 3).arg(fired));
  }

  // 2. WALKING at 1.4 m/s: the measurement has to actually register, within
  //    10% — a hint that never fires is as useless as one that always does.
  {
    WalkSpeedEstimator est;
    double last = 0.0;
    for (int i = 0; i < 200; ++i) {
      const double t = double(i) * 0.1;
      const double p[3] = {1.4 * t + jitter_m(rng), jitter_m(rng), jitter_m(rng)};
      est.update(std::int64_t(i) * 100'000'000LL, p);
      if (est.valid()) last = est.speedMps();
    }
    check("walking 1.4 m/s", std::abs(last - 1.4) < 0.14,
          QString("measured %1 m/s").arg(last, 0, 'f', 3));
  }

  // 3. A BRISK walk must trip the 1.5 m/s hint (the positive case for the
  //    warning itself, not just for the number).
  {
    WalkSpeedEstimator est;
    double last = 0.0;
    for (int i = 0; i < 200; ++i) {
      const double t = double(i) * 0.1;
      const double p[3] = {2.2 * t, 0.0, 0.0};
      est.update(std::int64_t(i) * 100'000'000LL, p);
      if (est.valid()) last = est.speedMps();
    }
    check("brisk 2.2 m/s trips the hint", last > 1.5,
          QString("measured %1 m/s").arg(last, 0, 'f', 3));
  }

  // 4. FRAME RESET. Every Start/Pause/Resume/Stop restarts LioOdometry at the
  //    origin. 40 m of walked trajectory collapsing to (0,0,0) in one 100 ms
  //    sample must NOT be reported as 400 m/s of walking — the failure the owner
  //    saw. It must reset and re-measure zero for a stationary rig.
  {
    WalkSpeedEstimator est;
    for (int i = 0; i < 400; ++i) {
      const double t = double(i) * 0.1;
      const double p[3] = {1.0 * t, 0.0, 0.0};
      est.update(std::int64_t(i) * 100'000'000LL, p);
    }
    double peak_after = 0.0;
    // the new odometry: back at the origin, standing still, its own timeline
    for (int i = 0; i < 200; ++i) {
      const double p[3] = {jitter_m(rng), jitter_m(rng), jitter_m(rng)};
      est.update(std::int64_t(400 + i) * 100'000'000LL, p);
      if (est.valid()) peak_after = std::max(peak_after, est.speedMps());
    }
    check("LIO frame reset after 40 m walk", peak_after == 0.0 && est.discontinuities() == 1,
          QString("peak after reset %1 m/s, %2 discontinuity/ies")
              .arg(peak_after, 0, 'f', 3)
              .arg(est.discontinuities()));
  }

  // 5. COALESCED POLLS. Two poses 1 ms apart (a Qt timer catching up) used to be
  //    accepted by the old `dt > 1e-3` guard and multiplied a centimetre of
  //    noise by ~90.
  {
    WalkSpeedEstimator est;
    double peak = 0.0;
    std::int64_t t_ns = 0;
    for (int i = 0; i < 400; ++i) {
      t_ns += (i % 7 == 0) ? 1'000'000LL : 100'000'000LL;  // an occasional 1 ms gap
      const double p[3] = {jitter_m(rng), jitter_m(rng), jitter_m(rng)};
      est.update(t_ns, p);
      if (est.valid()) peak = std::max(peak, est.speedMps());
    }
    check("coalesced 1 ms polls", peak == 0.0 && est.staleSamples() > 0,
          QString("peak %1 m/s, %2 sub-floor samples rejected")
              .arg(peak, 0, 'f', 3)
              .arg(est.staleSamples()));
  }

  // 6. A STALLED pose stream polled at 10 Hz: latest() keeps answering with the
  //    same pose. Nothing new happened, so nothing may be measured — and the
  //    estimator must not decay a real speed to zero on the strength of it
  //    either, so it simply ignores the repeats.
  {
    WalkSpeedEstimator est;
    for (int i = 0; i < 40; ++i) {
      const double p[3] = {1.0 * double(i) * 0.1, 0.0, 0.0};
      est.update(std::int64_t(i) * 100'000'000LL, p);
    }
    const double before = est.speedMps();
    const std::uint32_t stale_before = est.staleSamples();
    const double frozen[3] = {1.0 * 39.0 * 0.1, 0.0, 0.0};
    for (int i = 0; i < 50; ++i) est.update(39LL * 100'000'000LL, frozen);
    check("stalled stream (repeated poses)",
          est.speedMps() == before && est.staleSamples() == stale_before + 50,
          QString("held %1 m/s, %2 repeats ignored")
              .arg(est.speedMps(), 0, 'f', 3)
              .arg(est.staleSamples() - stale_before));
  }

  // --- MotionGate: the drift-free "is this rig being carried?" test --------
  //
  // Three IMU windows at the Mid-360's 200 Hz, one second each.
  using lidarscan::MotionGate;
  auto imu_window = [&rng](double gait_hz, double gait_a_m_s2, double gyro_rad_s,
                           std::vector<double>* g, std::vector<double>* a) {
    std::normal_distribution<double> gn(0.0, 0.0015);  // the sim's own gyro noise
    std::normal_distribution<double> an(0.0, 0.008);   // measured |a| deviation at rest
    const int n = 200;
    g->clear();
    a->clear();
    for (int i = 0; i < n; ++i) {
      const double t = double(i) / 200.0;
      const double gait = gait_a_m_s2 * std::sin(2 * M_PI * gait_hz * t);
      g->push_back(gyro_rad_s * std::sin(2 * M_PI * gait_hz * t) + gn(rng));
      g->push_back(gn(rng));
      g->push_back(gn(rng));
      a->push_back(an(rng));
      a->push_back(an(rng));
      a->push_back(9.8066 + gait + an(rng));  // gravity plus the gait's own force
    }
  };
  {
    std::vector<double> g, a;
    // Parked on a tripod: the case the owner was looking at.
    imu_window(0.0, 0.0, 0.0, &g, &a);
    MotionGate::Reading r = MotionGate::measure(g.data(), a.data(), g.size() / 3);
    check("IMU gate: parked rig is STILL", r.valid && r.still,
          QString("|a| dev %1 m/s², gyro %2 rad/s")
              .arg(r.accel_dev_m_s2, 0, 'f', 4)
              .arg(r.gyro_rms_rad_s, 0, 'f', 4));

    // A walking operator: ~2 Hz gait, ±1.5 m/s², a third of a rad/s of sway.
    imu_window(2.0, 1.5, 0.35, &g, &a);
    r = MotionGate::measure(g.data(), a.data(), g.size() / 3);
    check("IMU gate: walking rig is NOT still", r.valid && !r.still,
          QString("|a| dev %1 m/s², gyro %2 rad/s")
              .arg(r.accel_dev_m_s2, 0, 'f', 4)
              .arg(r.gyro_rms_rad_s, 0, 'f', 4));

    // Even a gentle, deliberate walk has to clear the gate — this is the
    // threshold's real margin test, not the loud case.
    imu_window(1.6, 0.6, 0.18, &g, &a);
    r = MotionGate::measure(g.data(), a.data(), g.size() / 3);
    check("IMU gate: gentle walk is NOT still", r.valid && !r.still,
          QString("|a| dev %1 m/s², gyro %2 rad/s")
              .arg(r.accel_dev_m_s2, 0, 'f', 4)
              .arg(r.gyro_rms_rad_s, 0, 'f', 4));

    // No IMU at all must be UNKNOWN, never "still": a gate that suppresses on
    // missing data would silently disable the hint on a rig with a dead IMU.
    r = MotionGate::measure(nullptr, nullptr, 0);
    check("IMU gate: no samples is UNKNOWN", !r.valid && !r.still,
          QString("valid=%1 still=%2").arg(r.valid).arg(r.still));
  }

  std::fprintf(stderr, "[lidarscan] walk-speed-selftest: %s (%d failure(s))\n",
               failures == 0 ? "PASS" : "FAIL", failures);
  return failures == 0 ? 0 : 8;
}

}  // namespace

int main(int argc, char** argv) {
  // Before QApplication and before the single-instance guard: this hook touches
  // no display, no port and no engine, so it must run even while the real app is
  // open (and in CI, where neither exists).
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--walk-speed-selftest") == 0) return runWalkSpeedSelfTest();
  }

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("LidarScan");
  QCoreApplication::setApplicationName("LidarScan Desktop");
  // Owner rule (2026-08-17): every shipped update gets a version code, and it
  // comes from the repo-root VERSION file — CMakeLists.txt reads that file and
  // defines LIDARSCAN_APP_VERSION / _CODE. This is what `--version`
  // (parser.addVersionOption(), below) prints. Distinct from the ENGINE's
  // version (`scanengine 0.1.0`, printed a few lines down and reported by
  // EngineHost::versionString()): the engine versions independently of the apps
  // that embed it, and conflating them would make a desktop-only fix look like
  // an engine change.
#ifndef LIDARSCAN_APP_VERSION
#define LIDARSCAN_APP_VERSION "0.0.0"
#define LIDARSCAN_APP_VERSION_CODE 0
#endif
  QCoreApplication::setApplicationVersion(
      QString("%1 (build %2)").arg(LIDARSCAN_APP_VERSION).arg(LIDARSCAN_APP_VERSION_CODE));

  // The redesign's fonts, palette and stylesheet. BEFORE any widget exists:
  // a stylesheet applied later still restyles, but a widget that cached a
  // font metric while it was being constructed does not re-layout, which
  // shows up as clipped labels in exactly the places that were laid out
  // first. (ui/Theme.h; this call is the whole of the theme's public API.)
  lidarscan::theme::install(app);

  // --- Single-instance guard --------------------------------------------
  //
  // docs/design/REVIEW_FEEDBACK.md, 2026-08-17 round 4 item 6 (owner, field
  // session): "a leftover instance holding UDP ports makes the next launch's
  // SDK init fail with an opaque I/O error" — captures/FIELD_SESSION_2026-
  // 08-17.md logs exactly this: "a first attempt left a port-holding
  // process; later runs failed SdkInit until killed". scanengine::
  // InstanceGuard (engine/include/scanengine/core/instance_guard.h, A16)
  // claims an advisory process-wide lock before anything below touches the
  // engine (EngineHost's Mid-360 driver is what actually opens the fixed UDP
  // ports, once a device is added from CaptureWindow) — if a prior instance
  // still holds it, this one says so plainly and exits instead of
  // reproducing that opaque SDK failure a second time. A plain local, NOT
  // `static`: declared directly in main()'s own scope, an ordinary automatic
  // variable already lives for the whole of `app.exec()` below and is
  // destroyed only once main() returns — which is everything the header
  // comment ("keep it alive for the process") actually needs. A `static`
  // here was tried first and crashed at exit ("mutex lock failed: Invalid
  // argument"): a function-local static's destructor runs during the
  // process's static-destruction sequence, AFTER other function-local
  // statics it might depend on (instance_guard.cpp's own registry mutex,
  // also a function-local static, guards the release path) may already have
  // been destroyed — the classic static destruction-order fiasco. A plain
  // stack local sidesteps it entirely: it is destroyed during main()'s own
  // return, strictly before any static-destruction sequence begins.
  scanengine::InstanceGuard g_instance_guard;
  const scanengine::Status guard_status = g_instance_guard.Acquire();
  if (!guard_status.ok()) {
    // last_error_message() carries the operator-facing sentence the guard's
    // own header promises ("another LidarScan is running (pid 4242)");
    // guard_status.message() is only the generic per-ScanError string, kept
    // as a fallback in case a future guard failure mode does not set it.
    QString detail = QString::fromUtf8(scanengine::last_error_message());
    if (detail.isEmpty()) detail = QString::fromUtf8(guard_status.message());
    QMessageBox::critical(nullptr, "LidarScan",
                          QString("LidarScan is already running.\n\n%1").arg(detail));
    return 1;
  }

  QCommandLineParser parser;
  parser.setApplicationDescription("LidarScan desktop — capture + workstation");
  parser.addHelpOption();
  parser.addVersionOption();
  QCommandLineOption optProject({"p", "project"}, "Open .lscan project DIR", "dir");
  QCommandLineOption optImport("import-raw", "Import raw D6 capture FILE into --project", "file");
  QCommandLineOption optNewProject(
      "new-project",
      "PROFILE (quickscan|survey|floorplan|research) — create --project fresh with this "
      "workflow profile before opening it (evidence hook for the A14 "
      "profile-defaults-on-open path; the GUI's New-project dialog does the same thing "
      "through createProject())",
      "profile");
  QCommandLineOption optReplay("replay", "Replay the project at SPEED (0 = unpaced)", "speed",
                               "1.0");
  QCommandLineOption optShot("shot", "Write a viewport screenshot to PATH", "path");
  QCommandLineOption optShotDelay("shot-delay", "Seconds before the screenshot", "s", "6");
  QCommandLineOption optQuit("quit-after", "Exit after N seconds", "s", "0");
  QCommandLineOption optVsync("vsync", "on|off", "mode", "on");
  QCommandLineOption optSize("size", "Initial window size WxH", "WxH");
  QCommandLineOption optDisplayParams("display-params",
                                      "Load a display-parameter JSON document (A14 from_json)",
                                      "file");
  QCommandLineOption optDisplayProfile(
      "display-profile", "survey|floorPlan|research|quickScan", "name");
  QCommandLineOption optResizeStorm(
      "resize-storm", "Resize the window continuously for N seconds (S3's stability stress)", "s",
      "0");
  QCommandLineOption optExport(
      "export", "ply|las|pcd:PATH — export the current PageStore via export_points()", "spec");
  QCommandLineOption optExportDelay(
      "export-delay", "Seconds to wait (after --replay starts) before exporting", "s", "6");
  QCommandLineOption optMeasureSelftest(
      "measure-selftest", "Synthesize two viewport clicks and report the measured segment");
  QCommandLineOption optMid360Selftest(
      "mid360-selftest",
      "HOST_IP:LIDAR_IP — drive CaptureWindow's guided Mid-360 self-test headlessly "
      "(engine add_device + start, first-data-or-timeout) against a device already "
      "listening at LIDAR_IP, e.g. the S2 simulator on loopback",
      "spec");
  QCommandLineOption optAutoDetectSelftest(
      "auto-detect-selftest",
      "Evidence hook: open the capture window and click 'Auto-detect devices' "
      "programmatically — real scanengine::discovery calls on a worker thread, same "
      "path the button drives, against whatever is reachable (e.g. the heartbeat "
      "replayer script for the S2 simulator, or real hardware). Prints what each "
      "sensor's pass found and exits 0 either way — 'nothing seen' is a valid, "
      "reported outcome, not a failure of this hook. Combine with --shot to capture "
      "the summary panel.");
  QCommandLineOption optAutoDetectCancelSelftest(
      "auto-detect-cancel-selftest",
      "Evidence hook for the §16.7 concurrency fix: open the capture window, start an "
      "auto-detect pass, then drive 'Test device' 400 ms INTO it — deliberately the "
      "collision the GUI has to survive (discovery must cancel, release UDP 56201 and "
      "let the device arm). Requires --mid360-selftest for the addresses; overrides the "
      "normal discovery-then-selftest chaining, which exists precisely to avoid this.");
  QCommandLineOption optAutoDetectShot(
      "auto-detect-shot",
      "With --auto-detect-selftest: QWidget::grab() the capture window (button + summary "
      "panel) to PATH once autoDetectFinished() fires",
      "path");
  QCommandLineOption optMid360RecordInto(
      "mid360-record-into",
      "With --mid360-selftest: on a PASSED self-test, also Record into this NEW "
      ".lscan directory for 3 s, Stop, and print the resulting project's chunk/byte "
      "counts (a fresh directory — unlike --project this does not open an existing one)",
      "dir");
  // Round-5 field bug C ("it only records when the first connected"): one app
  // run, N back-to-back record cycles against the SAME armed device. Anything
  // that only works on the first arm — a consumed session, a project dir eaten
  // by the first Start, a recorder that never re-opens — shows up as an empty
  // cycle 2, and this hook exits nonzero when that happens. Headless, so it can
  // be pointed at real hardware in the field exactly as it is at the simulator.
  QCommandLineOption optRecordCycles(
      "record-cycles",
      "With --mid360-selftest: after the arm passes, run N back-to-back Start/Stop "
      "record cycles (2 s each) against the SAME armed device, print each cycle's "
      "chunk/byte counts, and exit NONZERO if any cycle recorded nothing (round-5 "
      "field bug C: 'it only records when the first connected')",
      "n");
  QCommandLineOption optRecordCyclesDir(
      "record-cycles-dir",
      "Where --record-cycles creates its per-cycle projects — the capture ROOT, "
      "since each cycle goes through the panel's own auto-naming (default: a "
      "'record-cycles' folder beside the binary). Cleared first, so a rerun cannot "
      "read a previous run's chunks",
      "dir");
  QCommandLineOption optRecordCycleSeconds(
      "record-cycle-seconds", "Seconds of recording per --record-cycles cycle", "s", "2");
  // Round-5 field bug A ("wrongly detect me walking while I stay still").
  //
  // The S2 simulator's platform is NOT motionless — spikes/s2-mid360-sim's
  // scene.h drives an analytic lissajous — but it is GAIT-FREE and barely
  // accelerated: |v| <= 0.31 m/s, |a| <= 0.015 m/s², measured |accel| deviation
  // 0.008 m/s². Nobody is walking it anywhere. So the correct answer for the
  // whole soak is 0.00 m/s and no hint, and the pre-fix build answered 4.36 m/s
  // with the hint firing 226 times in 60 s. The POSITIVE case (a real walk
  // registers) cannot come from this simulator — it has no motion options and no
  // gait — so --walk-speed-selftest carries it instead.
  QCommandLineOption optWalkSoak(
      "walk-soak",
      "With --mid360-selftest: watch the capture panel's walk-speed hint for S seconds "
      "against the armed device and report the peak. Exits NONZERO if the 'ease off' hint "
      "ever fires or any non-zero speed is reported — nobody is carrying the simulator, so "
      "it must read 0.00 m/s however far the SLAM pose drifts",
      "s");
  // Round-5 field bug D ("live view not moving even i move the lidar").
  //
  // The engine's page store is bounded (64 x 1 M points by default). When it
  // filled it REFUSED every further point for the rest of the run, so the live
  // view froze at the fill instant while the recording carried on perfectly —
  // which is exactly what the owner saw, and what their engine log said 1400
  // times ("page store full (64 pages): dropped 8195 points"). A real Mid-360
  // takes ~5 minutes to fill it; --live-store-pages shrinks the ceiling so the
  // same moment arrives in seconds, through the SHIPPED store and the shipped
  // renderer.
  // FIELD BUG E's proof hook. Prints the settings file, the capture root that
  // is STORED (if any), and the root the panel resolves — so a script can show
  // that a --record-cycles run left the GUI's root alone, and that a poisoned
  // root (one inside a .app bundle) is self-healed at launch.
  QCommandLineOption optCaptureRootReport(
      "capture-root-report",
      "Print the QSettings file, the stored capture root and the root the capture panel "
      "resolves for a NEW scan, then keep running (combine with --quit-after 1)");
  QCommandLineOption optLiveStorePages(
      "live-store-pages",
      "Override the engine page store's ceiling (pages of 1 M points) for this run. "
      "Test hook for --live-map-soak: it makes the live window fill in seconds instead "
      "of minutes. 0/unset = the engine default (64)",
      "n");
  QCommandLineOption optLiveStorePagePoints(
      "live-store-page-points",
      "Override the engine page store's POINTS PER PAGE for this run (default 1048576). "
      "With --live-store-pages this is how --live-map-soak reaches the ceiling in "
      "seconds: the two together set the live window's exact size in points",
      "n");
  QCommandLineOption optLiveMapSoak(
      "live-map-soak",
      "With --mid360-selftest: run the live preview for S seconds PAST the point where "
      "the page store fills, and verify the live map keeps advancing — the newest point "
      "in the store gets newer, the page count stays at the ceiling, nothing is dropped, "
      "and the renderer keeps uploading. Exits NONZERO if the view ever stops moving "
      "(round-5 field bug D)",
      "s");
  // The control case for --live-map-soak. Without it a PASS proves only that
  // the app did not crash: this switch puts the SHIPPED store back into the
  // pre-fix hard-cap mode after arming, so the soak has to fail — and if it
  // does not, the soak is not measuring anything.
  QCommandLineOption optLiveMapNoEvict(
      "live-map-no-evict",
      "With --live-map-soak: reproduce the PRE-FIX behaviour by turning the live "
      "window's page recycling off after arming. The soak must FAIL (the live view "
      "freezes the moment the store fills) — this is the control run",
      QString());
  QCommandLineOption optLiveLodOldestFirst(
      "live-lod-oldest-first",
      "With --live-map-soak: the OTHER control run — spend the LOD budget on the oldest "
      "pages during a live capture, which is what the app did before this fix. With a "
      "budget smaller than the live window the soak must FAIL: everything is uploaded "
      "and the newest page is never drawn",
      QString());
  QCommandLineOption optBuildSynthMid360(
      "build-synth-mid360",
      "C4 evidence hook: write a real .lscan (kMid360Points/kMid360Imu chunks) at DIR from "
      "a ray-cast loop through a synthetic hall — see app/SyntheticMid360.h — so Post-process "
      "has something real to run on with no hardware",
      "dir");
  QCommandLineOption optPostE2e(
      "post-e2e",
      "C4 evidence hook: submit a REAL A15 kPostProcess job for the .lscan at DIR through "
      "ProcessingDock's JobQueue, poll it to completion, and on success load the produced "
      "kSlamMap cloud into the viewport (the same 'Load result' flow the UI button drives)",
      "dir");
  QCommandLineOption optPlanFixture(
      "plan-fixture",
      "C5 evidence hook: load A12's synthetic two-room-plus-corridor test building (see "
      "app/SyntheticBuilding.h) into the engine's PageStore and point the viewport/Plan dock "
      "at it");
  QCommandLineOption optPlanExtract(
      "plan-extract", "C5 evidence hook: run the Plan dock's 'Extract floor plan' on whatever "
                      "cloud is currently loaded (see --plan-fixture / --project / --replay)");
  QCommandLineOption optPlanExportDxf("plan-export-dxf", "C5 evidence hook: export the extracted "
                                                         "plan's DXF to PATH", "path");
  QCommandLineOption optPlanExportPdf("plan-export-pdf", "C5 evidence hook: export the extracted "
                                                         "plan's PDF to PATH", "path");
  QCommandLineOption optPlanShot("plan-shot", "C5 evidence hook: save a screenshot of the Plan "
                                              "dock (QWidget::grab, not the Filament viewport) to PATH",
                                 "path");
  QCommandLineOption optPlanDelay("plan-delay", "Seconds before the --plan-* hooks run "
                                                "(after --plan-fixture/--project has had time to land)",
                                  "s", "1");
  QCommandLineOption optMergeFixtureEvidence(
      "merge-fixture-evidence",
      "C6 evidence hook: build the 3-session synthetic overlapping-building fixture (mirrors "
      "engine/tests/test_merge.cpp), run the georeferenced auto-align path, then override "
      "session 1 with the 3-point manual pick path, then Refine (ICP, with the per-pair trace "
      "reproduced for the chart) and Build+publish (colour-by-session) into the viewport. "
      "Every step's real MergeReport numbers and the ground-truth comparison are printed. "
      "Combine with --shot to capture the merged, colour-by-session viewport.");
  QCommandLineOption optMergeAddProject(
      "merge-add-project",
      "C6 evidence hook: LSCAN_DIR:PROVENANCE_ID — real MergeSessionLoader path: a private "
      "Engine + unpaced ReplaySource decodes LSCAN_DIR's D6 raw chunks and adds the result as "
      "a merge session, exactly what 'Add from open project'/'Import .lscan project…' do", "spec");
  QCommandLineOption optMergeDockShot(
      "merge-dock-shot",
      "C6 evidence hook: raise the Merge dock, select pair row 0 (so its residual chart "
      "renders), and save a QWidget::grab() of the whole dock to PATH — run after "
      "--merge-fixture-evidence", "path");
  QCommandLineOption optMergeDockShotDelay(
      "merge-dock-shot-delay", "Seconds before --merge-dock-shot runs (after "
                               "--merge-fixture-evidence has had time to finish)", "s", "2");
  QCommandLineOption optTransferExport(
      "transfer-export", "C7 evidence hook: SRC_LSCAN_DIR:ZIP_PATH — zip_export() directly, "
                         "headless, and print size/byte stats", "spec");
  QCommandLineOption optTransferImport(
      "transfer-import",
      "C7 evidence hook: ZIP_PATH:DEST_DIR — zip_import() + readProject(), headless, and print "
      "the manifest sanity report (same one TransferImportDialog shows); run after "
      "--transfer-export against its own output for round-trip evidence", "spec");
  QCommandLineOption optTransferExportDialogShot(
      "transfer-export-dialog-shot",
      "C7 evidence hook: SRC_LSCAN_DIR:ZIP_PATH:SHOT_PATH — opens the REAL "
      "TransferExportDialog, clicks Export exactly as the button would (real zip_export() on "
      "a worker thread, real progress bar), and grabs the dialog to SHOT_PATH", "spec");
  QCommandLineOption optTransferImportDialogShot(
      "transfer-import-dialog-shot",
      "C7 evidence hook: ZIP_PATH:DEST_DIR:SHOT_PATH — opens the REAL TransferImportDialog, "
      "clicks Import exactly as the button would (real zip_import() + readProject() on a "
      "worker thread, real progress bar), and grabs the dialog (including the manifest sanity "
      "report) to SHOT_PATH", "spec");
  QCommandLineOption optCloudSelftest(
      "cloud-submit-selftest",
      "C4 evidence hook: submit a REAL kCloudSubmit job (QtHttpTransport, a genuine socket "
      "attempt) against a URL nothing listens on, and print that it fails GRACEFULLY (a "
      "failed Job with a real error/message, not a crash/hang) — the task's 'no server "
      "exists yet; expect connection failure gracefully' case", "url", "https://127.0.0.1:1/v1");
  parser.addOption(optProject);
  parser.addOption(optImport);
  parser.addOption(optNewProject);
  parser.addOption(optReplay);
  parser.addOption(optShot);
  parser.addOption(optShotDelay);
  parser.addOption(optQuit);
  parser.addOption(optVsync);
  parser.addOption(optSize);
  parser.addOption(optDisplayParams);
  parser.addOption(optDisplayProfile);
  parser.addOption(optResizeStorm);
  parser.addOption(optExport);
  parser.addOption(optExportDelay);
  parser.addOption(optMeasureSelftest);
  parser.addOption(optMid360Selftest);
  parser.addOption(optAutoDetectSelftest);
  parser.addOption(optAutoDetectCancelSelftest);
  parser.addOption(optAutoDetectShot);
  parser.addOption(optMid360RecordInto);
  parser.addOption(optRecordCycles);
  parser.addOption(optRecordCyclesDir);
  parser.addOption(optRecordCycleSeconds);
  parser.addOption(optWalkSoak);
  parser.addOption(optCaptureRootReport);
  parser.addOption(optLiveStorePages);
  parser.addOption(optLiveStorePagePoints);
  parser.addOption(optLiveMapSoak);
  parser.addOption(optLiveMapNoEvict);
  parser.addOption(optLiveLodOldestFirst);
  // Registered for --help only: this one is intercepted at the top of main(),
  // before QApplication and the single-instance guard, because it needs neither
  // and must run while the real app is open.
  parser.addOption(QCommandLineOption(
      "walk-speed-selftest",
      "Unit self-test for WalkSpeedEstimator + MotionGate — the stationary, walking, "
      "LIO-frame-reset, coalesced-poll and stalled-stream cases behind round-5 field "
      "bug A (NOTES.md §17.9). No hardware, no display, no engine; exits 8 on failure"));
  parser.addOption(optBuildSynthMid360);
  parser.addOption(optPostE2e);
  parser.addOption(optPlanFixture);
  parser.addOption(optPlanExtract);
  parser.addOption(optPlanExportDxf);
  parser.addOption(optPlanExportPdf);
  parser.addOption(optPlanShot);
  parser.addOption(optPlanDelay);
  parser.addOption(optCloudSelftest);
  parser.addOption(optMergeFixtureEvidence);
  parser.addOption(optMergeAddProject);
  parser.addOption(optMergeDockShot);
  parser.addOption(optMergeDockShotDelay);
  parser.addOption(optTransferExport);
  parser.addOption(optTransferImport);
  parser.addOption(optTransferExportDialogShot);
  parser.addOption(optTransferImportDialogShot);
  QCommandLineOption optFontReport(
      "font-report",
      "Redesign evidence hook: print which bundled typefaces QFontDatabase "
      "actually registered (and whether it fell back to the platform UI font), "
      "then continue. Proves the .qrc reached the binary.");
  parser.addOption(optFontReport);
  QCommandLineOption optInspectorDemo(
      "inspector-demo",
      "Redesign evidence hook: PREFIX — shoot the review viewport, drag the "
      "floating inspector's point-size slider to its top, shoot again, and "
      "report the mean-brightness delta. Proves the inspector is bound to the "
      "live A14 model rather than decorating one.",
      "prefix");
  parser.addOption(optInspectorDemo);
  QCommandLineOption optCaptureClusterDemo(
      "capture-cluster-demo",
      "Redesign evidence hook: PREFIX — open the capture panel and shoot the "
      "record cluster in each state the capture machine can be in without hardware "
      "(no device / arming), then, if --mid360-selftest also ran, live / "
      "recording / paused.",
      "prefix");
  parser.addOption(optCaptureClusterDemo);
  QCommandLineOption optCaptureFlowDemo(
      "capture-flow-demo",
      "Round-5 evidence hook: PREFIX — drive the WHOLE redesigned capture flow and "
      "shoot it. Selects the Capture workspace (a dock, no popup), lets the inline "
      "auto-detect pass run, then arms the addresses --mid360-selftest supplies (the "
      "same armPreview() an auto-detect hit calls), moves the live refresh-rate and "
      "point-size controls, shoots the PREVIEW state, presses Start with the name "
      "field EMPTY (so the project is auto-named Scan-NNN date time), shoots the "
      "RECORDING state, Stops, and prints the sealed project's readProject() summary "
      "plus whether it landed in the library. Requires --mid360-selftest.",
      "prefix");
  parser.addOption(optCaptureFlowDemo);
  QCommandLineOption optLiveRefresh(
      "live-refresh",
      "Set the capture panel's live viewport refresh cap (fps) at startup, through the "
      "real slider. Clamped to this display's refresh rate (round-5 item 17). Also the "
      "way an evidence run picks a known starting rate, since the panel persists the "
      "last one.",
      "fps");
  parser.addOption(optLiveRefresh);
  QCommandLineOption optProjectsActionsDemo(
      "projects-actions-demo",
      "Round-5 follow-up evidence hook: PREFIX — in the Projects workspace, select ONE "
      "library row (Process + Export unlock, Merge stays locked), shoot it, press "
      "Process… (the A15 job queue opens as a PANEL OF PROJECTS, not a tab of its own), "
      "shoot it, then select TWO rows (Merge unlocks), press Merge selected… (each "
      "project is loaded as a merge session) and shoot that. Prints the action state at "
      "every step.",
      "prefix");
  parser.addOption(optProjectsActionsDemo);
  QCommandLineOption optWorkspace(
      "workspace",
      "Select a workspace at startup, the same way clicking the icon rail does: "
      "projects|capture|review|plan|merge|jobs. Redesign evidence hook.",
      "name");
  parser.addOption(optWorkspace);
  parser.process(app);

  if (parser.isSet(optFontReport)) {
    std::fprintf(stderr, "[lidarscan] bundled typefaces (QFontDatabase):\n");
    for (const QString& l : lidarscan::theme::fontReport()) {
      std::fprintf(stderr, "[lidarscan]   %s\n", l.toUtf8().constData());
    }
    std::fprintf(stderr, "[lidarscan]   all three bundled: %s\n",
                 lidarscan::theme::fontsLoaded() ? "yes" : "NO");
  }

  // --- FIELD BUG E: an evidence hook must not touch the operator's settings --
  //
  // The owner's real project was recorded into
  // ~/Applications/LidarScan.app/Contents/MacOS/record-cycles/. Root cause:
  // --record-cycles pointed CaptureWindow at a scratch root beside the binary
  // (inside the .app, on an installed build) and CaptureWindow::setProjectDir()
  // PERSISTED it into QSettings("capture/root"), which every later NORMAL
  // launch then used as the place new scans go. setProjectDir() no longer
  // persists (CaptureWindow.cpp), and this is the second, generic half: while
  // any evidence/CI hook is driving the app, QSettings is redirected into a
  // scratch directory, so NOTHING a hook touches — the capture root, the series
  // number, the live refresh rate, the recents list, the window geometry — can
  // leak into the settings a human's next launch reads.
  //
  // Deliberately a whitelist of hooks rather than "always in CI": a developer
  // running the real app must keep their real settings.
  {
    const QList<const QCommandLineOption*> evidence_hooks = {
        &optMid360Selftest,   &optMid360RecordInto,     &optRecordCycles,
        &optRecordCyclesDir,  &optWalkSoak,             &optLiveMapSoak,
        &optAutoDetectSelftest, &optAutoDetectCancelSelftest, &optCaptureClusterDemo,
        &optCaptureFlowDemo,  &optProjectsActionsDemo,  &optShot,
        &optPostE2e,          &optBuildSynthMid360,     &optLiveStorePages,
        &optLiveStorePagePoints};
    bool hooked = false;
    for (const QCommandLineOption* o : evidence_hooks) hooked = hooked || parser.isSet(*o);
    if (hooked) {
      // SEEDED, not blank. An evidence hook has to see the same world a human
      // does — --projects-actions-demo selects rows out of the library, which
      // IS QSettings("recentProjects"), and a demo against an empty library
      // proves nothing — so the real settings are COPIED into the scratch
      // location and every write after this point lands in the copy. Read
      // alike, write isolated.
      QSettings real;  // native format, real location: read BEFORE the redirect
      QMap<QString, QVariant> snapshot;
      for (const QString& k : real.allKeys()) snapshot.insert(k, real.value(k));

      const QString scratch =
          QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
              .filePath(QString("lidarscan-evidence-settings-%1").arg(QCoreApplication::applicationPid()));
      QDir().mkpath(scratch);
      QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch);
      QSettings::setDefaultFormat(QSettings::IniFormat);

      QSettings isolated;
      for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        isolated.setValue(it.key(), it.value());
      }
      isolated.sync();
      std::fprintf(stderr,
                   "[lidarscan] evidence hook active — settings isolated to %s "
                   "(%d key(s) copied in; the operator's own settings are read-only from "
                   "here)\n",
                   scratch.toUtf8().constData(), int(snapshot.size()));
    }
  }

  // Read BEFORE MainWindow exists: CaptureWindow's constructor self-heals a bad
  // stored root, and the report has to show what was there beforehand.
  const QString storedCaptureRootAtStartup = QSettings().value("capture/root").toString();
  const QString settingsFileAtStartup = QSettings().fileName();

  const quint32 liveStorePages =
      parser.isSet(optLiveStorePages) ? parser.value(optLiveStorePages).toUInt() : 0;
  const quint32 liveStorePagePoints =
      parser.isSet(optLiveStorePagePoints) ? parser.value(optLiveStorePagePoints).toUInt() : 0;
  lidarscan::EngineHost host(nullptr, liveStorePages, liveStorePagePoints);
  if (!host.ok()) {
    QMessageBox::critical(nullptr, "LidarScan", host.createError());
    return 2;
  }
  std::fprintf(stderr, "[lidarscan] %s\n", host.versionString().toUtf8().constData());

  lidarscan::MainWindow win(&host);
  win.viewport()->setVsync(parser.value(optVsync) != "off");

  if (parser.isSet(optSize)) {
    const QStringList wh = parser.value(optSize).split('x');
    if (wh.size() == 2) win.resize(wh[0].toInt(), wh[1].toInt());
  }
  if (parser.isSet(optWorkspace)) {
    const QString ws = parser.value(optWorkspace);
    if (!win.showWorkspaceNamed(ws)) {
      std::fprintf(stderr,
                   "[lidarscan] --workspace: unknown workspace '%s' "
                   "(projects|capture|review|plan|merge|jobs)\n",
                   ws.toUtf8().constData());
    }
  }
  win.show();

  if (parser.isSet(optLiveLodOldestFirst)) {
    win.viewport()->setForceOldestFirstLodForCli(true);
    std::fprintf(stderr,
                 "[lidarscan] CONTROL RUN — the LOD budget is spent oldest-first even for a "
                 "live capture (the pre-fix order)\n");
  }

  if (parser.isSet(optCaptureRootReport)) {
    lidarscan::CaptureWindow* cap = win.captureWindow();
    const QString stored_now = QSettings().value("capture/root").toString();
    std::fprintf(stderr, "[lidarscan] capture-root-report: settings   %s\n",
                 settingsFileAtStartup.toUtf8().constData());
    std::fprintf(stderr, "[lidarscan] capture-root-report: stored     %s\n",
                 storedCaptureRootAtStartup.isEmpty()
                     ? "<none>"
                     : storedCaptureRootAtStartup.toUtf8().constData());
    std::fprintf(stderr, "[lidarscan] capture-root-report: after heal %s\n",
                 stored_now.isEmpty() ? "<none>" : stored_now.toUtf8().constData());
    std::fprintf(stderr, "[lidarscan] capture-root-report: resolved   %s\n",
                 cap->captureRootForCli().toUtf8().constData());
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
        "/LidarScan Projects";
    std::fprintf(stderr, "[lidarscan] capture-root-report: default    %s — %s\n",
                 docs.toUtf8().constData(),
                 cap->captureRootForCli() == docs ? "MATCHES (the round-5 default)"
                                                  : "differs");
  }

  QString projectDir = parser.value(optProject);
  if (parser.isSet(optNewProject)) {
    if (projectDir.isEmpty()) {
      std::fprintf(stderr, "[lidarscan] --new-project requires --project\n");
      return 2;
    }
    QString err;
    if (!lidarscan::createProject(projectDir, parser.value(optNewProject), &err)) {
      std::fprintf(stderr, "[lidarscan] createProject failed: %s\n", err.toUtf8().constData());
      return 2;
    }
  }
  if (parser.isSet(optImport)) {
    if (projectDir.isEmpty()) {
      std::fprintf(stderr, "[lidarscan] --import-raw requires --project\n");
      return 2;
    }
    QString err;
    quint64 bytes = 0, points = 0;
    if (!lidarscan::importRawD6(parser.value(optImport), projectDir, "quickscan", &err, &bytes,
                                &points)) {
      std::fprintf(stderr, "[lidarscan] import failed: %s\n", err.toUtf8().constData());
      return 3;
    }
    std::fprintf(stderr, "[lidarscan] imported %llu bytes -> %llu points -> %s\n",
                 (unsigned long long)bytes, (unsigned long long)points,
                 projectDir.toUtf8().constData());
  }

  if (parser.isSet(optBuildSynthMid360)) {
    const QString dir = parser.value(optBuildSynthMid360);
    const auto res = lidarscan::buildSyntheticMid360Project(dir);
    if (!res.ok) {
      std::fprintf(stderr, "[lidarscan] build-synth-mid360 failed: %s\n",
                   res.error.toUtf8().constData());
      return 7;
    }
    std::fprintf(stderr,
                 "[lidarscan] build-synth-mid360: %s — %llu point packets (%llu points), %llu "
                 "IMU packets, %.1f s\n",
                 dir.toUtf8().constData(), (unsigned long long)res.point_packets,
                 (unsigned long long)res.points, (unsigned long long)res.imu_packets,
                 res.duration_s);
  }

  // C7 evidence hooks: A5's zip_export()/zip_import() driven directly and
  // headlessly (no dialog, no worker thread — this is the CLI evidence path,
  // not the interactive one TransferExportDialog/TransferImportDialog drive
  // on their own std::thread). Both run immediately: they touch only disk,
  // not the engine/viewport, so there is nothing to wait for.
  if (parser.isSet(optTransferExport)) {
    const QString spec = parser.value(optTransferExport);
    const int colon = spec.indexOf(':');
    if (colon <= 0) {
      std::fprintf(stderr, "[lidarscan] --transfer-export wants SRC_DIR:ZIP_PATH\n");
      return 6;
    }
    const QString srcDir = spec.left(colon);
    const QString zipPath = spec.mid(colon + 1);
    QDir().mkpath(QFileInfo(zipPath).absolutePath());
    const auto st = scanengine::lscan::zip_export(srcDir.toStdString(), zipPath.toStdString());
    const quint64 bytes = st.ok() ? quint64(QFileInfo(zipPath).size()) : 0;
    std::fprintf(stderr, "[lidarscan] transfer-export: %s -> %s: %s (%llu bytes)\n",
                 srcDir.toUtf8().constData(), zipPath.toUtf8().constData(),
                 st.ok() ? "OK" : scanengine::error_str(st.error()), (unsigned long long)bytes);
    if (!st.ok()) return 8;
  }
  if (parser.isSet(optTransferImport)) {
    const QString spec = parser.value(optTransferImport);
    const int colon = spec.indexOf(':');
    if (colon <= 0) {
      std::fprintf(stderr, "[lidarscan] --transfer-import wants ZIP_PATH:DEST_DIR\n");
      return 6;
    }
    const QString zipPath = spec.left(colon);
    const QString destDir = spec.mid(colon + 1);
    const auto st = scanengine::lscan::zip_import(zipPath.toStdString(), destDir.toStdString());
    std::fprintf(stderr, "[lidarscan] transfer-import: %s -> %s: %s\n", zipPath.toUtf8().constData(),
                 destDir.toUtf8().constData(), st.ok() ? "OK" : scanengine::error_str(st.error()));
    if (!st.ok()) return 8;
    const lidarscan::ProjectInfo info = lidarscan::readProject(destDir);
    if (!info.valid) {
      std::fprintf(stderr, "[lidarscan] transfer-import: readProject failed: %s\n",
                   info.error.toUtf8().constData());
      return 8;
    }
    std::fprintf(stderr,
                 "[lidarscan] transfer-import: manifest %s%s, profile %s, %llu chunks, %llu "
                 "bytes, %.2f s span, %u truncated-tail, %u crc-mismatch, %u unreadable streams\n",
                 info.manifest_present ? (info.manifest_ok ? "ok" : "corrupt") : "missing",
                 info.sealed ? "" : " (NOT SEALED)", info.profile.toUtf8().constData(),
                 (unsigned long long)info.total_chunks, (unsigned long long)info.total_bytes,
                 info.duration_s, info.truncated_tail_chunks, info.crc_mismatch_chunks,
                 info.unreadable_streams);
    for (const auto& s : info.streams) {
      std::fprintf(stderr, "[lidarscan] transfer-import:   stream %s: %llu chunks, %llu bytes\n",
                   s.name.toUtf8().constData(), (unsigned long long)s.chunks,
                   (unsigned long long)s.bytes);
    }
  }

  if (parser.isSet(optTransferExportDialogShot)) {
    const QString spec = parser.value(optTransferExportDialogShot);
    const QStringList parts = spec.split(':');
    if (parts.size() != 3) {
      std::fprintf(stderr,
                   "[lidarscan] --transfer-export-dialog-shot wants SRC_DIR:ZIP_PATH:SHOT_PATH\n");
      return 6;
    }
    const QString srcDir = parts[0], zipPath = parts[1], shotPath = parts[2];
    QDir().mkpath(QFileInfo(shotPath).absolutePath());
    QDir().mkpath(QFileInfo(zipPath).absolutePath());
    auto* dlg = new lidarscan::TransferExportDialog(srcDir, &win);
    dlg->setZipPathForCli(zipPath);
    dlg->show();
    QTimer::singleShot(200, dlg, [dlg] { dlg->triggerExportForCli(); });
    QTimer::singleShot(1200, dlg, [dlg, shotPath] {
      const bool ok = dlg->grab().save(shotPath);
      std::fprintf(stderr, "[lidarscan] transfer-export-dialog-shot: %s -> %s\n", ok ? "OK" : "FAILED",
                   shotPath.toUtf8().constData());
    });
  }

  if (parser.isSet(optTransferImportDialogShot)) {
    const QString spec = parser.value(optTransferImportDialogShot);
    const QStringList parts = spec.split(':');
    if (parts.size() != 3) {
      std::fprintf(stderr,
                   "[lidarscan] --transfer-import-dialog-shot wants ZIP_PATH:DEST_DIR:SHOT_PATH\n");
      return 6;
    }
    const QString zipPath = parts[0], destDir = parts[1], shotPath = parts[2];
    QDir().mkpath(QFileInfo(shotPath).absolutePath());
    auto* dlg = new lidarscan::TransferImportDialog(zipPath, QFileInfo(destDir).absolutePath(), &win);
    dlg->setDestDirForCli(destDir);
    dlg->show();
    QTimer::singleShot(200, dlg, [dlg] { dlg->triggerImportForCli(); });
    QTimer::singleShot(1200, dlg, [dlg, shotPath] {
      const bool ok = dlg->grab().save(shotPath);
      std::fprintf(stderr, "[lidarscan] transfer-import-dialog-shot: %s -> %s\n", ok ? "OK" : "FAILED",
                   shotPath.toUtf8().constData());
    });
  }

  if (!projectDir.isEmpty()) {
    QString err;
    if (!win.openProject(projectDir, &err)) {
      std::fprintf(stderr, "[lidarscan] open failed: %s\n", err.toUtf8().constData());
      return 4;
    }
  }

  if (parser.isSet(optDisplayProfile)) {
    const QString n = parser.value(optDisplayProfile);
    int idx = -1;
    for (int i = 0; i < scanengine::kDisplayProfileCount; ++i) {
      if (n.compare(scanengine::to_string(static_cast<scanengine::DisplayProfile>(i)),
                    Qt::CaseInsensitive) == 0) {
        idx = i;
      }
    }
    if (idx < 0) {
      std::fprintf(stderr, "[lidarscan] unknown display profile '%s'\n", n.toUtf8().constData());
      return 5;
    }
    win.applyDisplayProfile(static_cast<scanengine::DisplayProfile>(idx));
  }
  if (parser.isSet(optDisplayParams)) {
    QString err;
    if (!win.loadDisplayParamsFile(parser.value(optDisplayParams), &err)) {
      std::fprintf(stderr, "[lidarscan] %s\n", err.toUtf8().constData());
      return 5;
    }
  }

  if (parser.isSet(optReplay)) {
    const double speed = parser.value(optReplay).toDouble();
    // The viewport needs one exposeEvent before Filament exists; start the
    // replay on the next event-loop turn so the first points already have a
    // renderer to land in.
    QTimer::singleShot(500, &win, [&win, speed] {
      QString err;
      if (!win.startReplay(speed, &err)) {
        std::fprintf(stderr, "[lidarscan] replay failed: %s\n", err.toUtf8().constData());
      } else {
        std::fprintf(stderr, "[lidarscan] replay started at %gx\n", speed);
      }
    });
  }

  if (parser.isSet(optShot)) {
    const QString path = QFileInfo(parser.value(optShot)).absoluteFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int ms = int(parser.value(optShotDelay).toDouble() * 1000.0);
    QTimer::singleShot(ms, &win, [&win, path] {
      const bool ok = win.viewport()->captureScreenshot(path);
      const auto& s = win.viewport()->stats();
      std::fprintf(stderr,
                   "[lidarscan] screenshot %s -> %s | %llu pts / %zu pages | %.1f fps | "
                   "cpu p95 %.2f ms | gpu p95 %.2f ms | %dx%d px dpr %.2f | swapchains %u\n",
                   ok ? "OK" : "FAILED", path.toUtf8().constData(),
                   (unsigned long long)s.cloud.resident_points, s.cloud.pages, s.fps,
                   s.cpu_ms_p95, s.gpu_ms_p95, s.px_w, s.px_h, s.dpr, s.swapchain_recreates);
    });
  }

  if (parser.isSet(optExport)) {
    const QString spec = parser.value(optExport);
    const int colon = spec.indexOf(':');
    if (colon <= 0) {
      std::fprintf(stderr, "[lidarscan] --export wants FORMAT:PATH, e.g. ply:/tmp/out.ply\n");
      return 6;
    }
    const QString fmt = spec.left(colon).toLower();
    const QString path = spec.mid(colon + 1);
    scanengine::ExportFormat format;
    if (fmt == "ply") {
      format = scanengine::ExportFormat::kPlyBinary;
    } else if (fmt == "las") {
      format = scanengine::ExportFormat::kLas14;
    } else if (fmt == "pcd") {
      format = scanengine::ExportFormat::kPcd;
    } else {
      std::fprintf(stderr, "[lidarscan] unknown export format '%s' (want ply|las|pcd)\n",
                   fmt.toUtf8().constData());
      return 6;
    }
    const int ms = int(parser.value(optExportDelay).toDouble() * 1000.0);
    QTimer::singleShot(ms, &win, [&host, format, path] {
      scanengine::ExportOptions opts;
      opts.format = format;
      opts.output_path = path.toStdString();
      opts.include_color = true;
      opts.include_intensity = true;
      const auto st = scanengine::export_points(*host.points(),
                                                 scanengine::Span<const scanengine::StreamId>{},
                                                 format, path.toStdString(), opts);
      std::fprintf(stderr, "[lidarscan] export %s -> %s: %s\n", path.toUtf8().constData(),
                   st.ok() ? "OK" : "FAILED", scanengine::error_str(st.error()));
    });
  }

  if (parser.isSet(optMeasureSelftest)) {
    const int ms = int(parser.value(optExportDelay).toDouble() * 1000.0);
    QTimer::singleShot(ms, &win, [&win] {
      lidarscan::ViewportWindow* vp = win.viewport();
      vp->fitView();
      vp->setMeasureMode(true);
      // A synthetic click at one fixed screen position is not reliable for
      // every possible cloud shape (a D6 capture is a thin room-outline
      // trace at this stage — pushbroom assembly is A8's job — so most of
      // the viewport is empty space between the walls). Walk a grid instead
      // and stop at the first two grid points that actually land on a point,
      // via the exact same ViewportWindow::pickPoint() a real click uses.
      const int w = std::max(1, vp->width());
      const int h = std::max(1, vp->height());
      for (int gy = 1; gy <= 8 && vp->measurements().empty(); ++gy) {
        for (int gx = 1; gx <= 8 && vp->measurements().empty(); ++gx) {
          const QPoint p(w * gx / 9, h * gy / 9);
          QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(vp->mapToGlobal(p)),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
          QCoreApplication::sendEvent(vp, &press);
          QMouseEvent release(QEvent::MouseButtonRelease, QPointF(p), QPointF(vp->mapToGlobal(p)),
                              Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
          QCoreApplication::sendEvent(vp, &release);
        }
      }
      const auto& segs = vp->measurements();
      if (segs.empty()) {
        std::fprintf(stderr, "[lidarscan] measure-selftest: no segment produced across an 8x8 "
                             "click grid (0 resident points, or the cloud is sparser than the "
                             "grid) — pending point: %s\n",
                     vp->hasPendingMeasurePoint() ? "yes" : "no");
      } else {
        const auto& s = segs.back();
        std::fprintf(stderr,
                     "[lidarscan] measure-selftest: segment (%.3f,%.3f,%.3f) -> "
                     "(%.3f,%.3f,%.3f) = %.4f m\n",
                     s.a[0], s.a[1], s.a[2], s.b[0], s.b[1], s.b[2], s.distance_m);
      }
    });
  }

  // --- redesign evidence hooks -------------------------------------------
  //
  // Both drive the REAL widgets through their real signal paths (a QSlider
  // value change, the record cluster's own buttons' slots), so a green run is
  // evidence about the shipped UI rather than about a parallel test harness.
  if (parser.isSet(optInspectorDemo)) {
    const QString prefix = QFileInfo(parser.value(optInspectorDemo)).absoluteFilePath();
    QDir().mkpath(QFileInfo(prefix).absolutePath());
    win.showWorkspaceNamed("review");
    const int t0 = int(parser.value(optShotDelay).toDouble() * 1000.0);
    QTimer::singleShot(t0, &win, [&win, prefix] {
      win.viewport()->captureScreenshot(prefix + "-before.png");
    });
    QTimer::singleShot(t0 + 700, &win, [&win, prefix] {
      auto* insp = win.inspector();
      if (!insp) {
        std::fprintf(stderr, "[lidarscan] inspector-demo: no inspector card\n");
        return;
      }
      const double applied = insp->setPointSizeForCli(12.0);
      std::fprintf(stderr,
                   "[lidarscan] inspector-demo: point-size slider -> 12.0 px, A14 model now "
                   "%.2f px (viewport params %.2f px)\n",
                   applied, win.viewport()->displayParams().point_size.fixed_px);
    });
    QTimer::singleShot(t0 + 2000, &win, [&win, prefix] {
      win.viewport()->captureScreenshot(prefix + "-after.png");
      double changedFrac = 0.0;
      const double d = frameDelta(prefix + "-before.png", prefix + "-after.png", &changedFrac);
      std::fprintf(stderr,
                   "[lidarscan] inspector-demo: mean |delta| %.4f over %.1f%% of the frame — %s\n",
                   d, changedFrac * 100.0,
                   (d > 1.0 && changedFrac > 0.02) ? "VIEWPORT CHANGED"
                                                   : "NO VISIBLE CHANGE (investigate)");
    });
  }

  // --- Discovery vs. device arming: one at a time, never both (NOTES.md
  // §16.7) ------------------------------------------------------------------
  //
  // scanengine::discovery listens on UDP 56201 and the Livox SDK's push
  // channel binds UDP 56201; on macOS the second bind loses, and the losing
  // side is whichever one starts later — which on a real Mid-360 was the
  // driver: `bind failed` in the SDK log, `device 1: idle -> fault I/O error`
  // in ours. CaptureWindow serializes the two at runtime (it cancels a pass in
  // flight and waits for the socket to close), but a CLI run should never get
  // into that state to begin with, so:
  //   * a device-arming hook SUPPRESSES the silent on-open pass outright, and
  //   * --auto-detect-selftest + --mid360-selftest together CHAIN, they do not
  //     both fire on a 500 ms timer (see the two blocks below).
  //
  // THIS MUST STAY ABOVE EVERY HOOK THAT SHOWS THE CAPTURE WINDOW.
  // QWidget::show() delivers QShowEvent SYNCHRONOUSLY, so CaptureWindow's
  // silent on-open pass starts inside the show() call itself — suppressing it
  // after --capture-cluster-demo's cap->show() below would be too late by one
  // stack frame.
  const bool cliArmsDevice = parser.isSet(optMid360Selftest);
  // ...except when --auto-detect-cancel-selftest asks for the collision ON
  // PURPOSE, to prove CaptureWindow survives it. That hook drives both sides
  // itself, so it opts out of the chaining below rather than out of the
  // serialization, which is the thing under test.
  const bool cancelProbe = parser.isSet(optAutoDetectCancelSelftest);
  const bool chainToSelftest =
      cliArmsDevice && parser.isSet(optAutoDetectSelftest) && !cancelProbe;
  // Round 5: an auto-detect HIT now arms the device by itself (that is the whole
  // point of the new flow). A CLI hook that arms the device explicitly must
  // therefore switch that off, or two things race for UDP 56201 and for the same
  // phase machine. --capture-flow-demo is the one hook that wants BOTH a real
  // inline discovery pass and a deterministic arm, so it too suppresses the
  // auto-arm and chains the arm itself.
  const bool flowDemo = parser.isSet(optCaptureFlowDemo);
  if (cliArmsDevice || parser.isSet(optAutoDetectSelftest)) {
    win.captureWindow()->suppressAutoArmForCli();
  }
  // Filled in by the --mid360-selftest block below; called by the auto-detect
  // block's completion handler when both hooks are set. Populated before the
  // event loop runs, so the timer-driven chain always sees it.
  auto* chainedDeviceSelftest = new std::function<void()>();
  // --capture-flow-demo deliberately does NOT suppress the on-open pass: the
  // inline auto-detect status row is part of what it is photographing, and it
  // arms only after that pass has finished and released the port.
  if (cliArmsDevice && !flowDemo) win.captureWindow()->suppressSilentAutoDetectForCli();

  if (parser.isSet(optLiveRefresh)) {
    lidarscan::CaptureWindow* cap = win.captureWindow();
    const double applied = cap->setLiveRefreshHzForCli(parser.value(optLiveRefresh).toDouble());
    std::fprintf(stderr, "[lidarscan] live-refresh: panel %.0f fps, viewport cap %.0f fps\n",
                 applied, win.viewport()->maxFps());
  }

  if (parser.isSet(optCaptureClusterDemo)) {
    const QString prefix = QFileInfo(parser.value(optCaptureClusterDemo)).absoluteFilePath();
    QDir().mkpath(QFileInfo(prefix).absolutePath());
    lidarscan::CaptureWindow* cap = win.captureWindow();
    cap->setProjectDir(parser.value(optProject));
    // Round 5: showing the panel means showing its workspace — it is a dock in
    // the shell now, not a window of its own.
    win.showWorkspaceNamed("capture");
    auto shot = [cap](const QString& path, const char* label) {
      const bool ok = cap->grab().save(path);
      std::fprintf(stderr, "[lidarscan] capture-cluster-demo: %s %s -> %s\n", label,
                   ok ? "OK" : "FAILED", path.toUtf8().constData());
    };
    // The no-device state needs no hardware at all, so it is shot first and
    // unconditionally. (Named -01-gated.png for continuity with the round-1
    // evidence this hook was written for; the gate itself is gone — round 5.)
    QTimer::singleShot(600, cap, [shot, prefix] { shot(prefix + "-01-gated.png", "gated"); });
    QObject::connect(cap, &lidarscan::CaptureWindow::selfTestFinished, &app,
                     [shot, prefix](bool passed, const QString&) {
                       if (!passed) {
                         shot(prefix + "-02-failed.png", "self-test failed");
                         return;
                       }
                       shot(prefix + "-02-armed.png", "armed");
                     });
    QObject::connect(cap, &lidarscan::CaptureWindow::captureStarted, &app,
                     [cap, shot, prefix, &win](const QString&) {
                       QTimer::singleShot(1400, cap, [cap, shot, prefix, &win] {
                         shot(prefix + "-03-recording.png", "recording");
                         // The badge rides the viewport, which is a native
                         // surface above this dock, so it needs its own grab.
                         const bool ok = win.grab().save(prefix + "-03-badge-recording.png");
                         std::fprintf(stderr,
                                      "[lidarscan] capture-cluster-demo: viewport RECORDING badge "
                                      "%s -> %s\n",
                                      ok ? "OK" : "FAILED",
                                      (prefix + "-03-badge-recording.png").toUtf8().constData());
                         cap->triggerPauseResumeForCli();
                         QTimer::singleShot(700, cap, [cap, shot, prefix, &win] {
                           shot(prefix + "-04-paused.png", "paused");
                           const bool ok2 = win.grab().save(prefix + "-04-badge-paused.png");
                           std::fprintf(stderr,
                                        "[lidarscan] capture-cluster-demo: viewport PAUSED badge "
                                        "%s -> %s\n",
                                        ok2 ? "OK" : "FAILED",
                                        (prefix + "-04-badge-paused.png").toUtf8().constData());
                           cap->triggerPauseResumeForCli();  // back to recording
                         });
                       });
                     });
  }

  // Auto-detect evidence hook: exercises the SAME button (RecordCluster's
  // sibling, "Auto-detect devices") a user would click, over the real
  // DeviceDiscovery worker thread and real scanengine::discovery calls — no
  // fake transport, matching every other *-selftest hook in this file. Runs
  // BEFORE --mid360-selftest below, deliberately: they both use CaptureWindow
  // but neither drives the record cluster, so ordering between them does not
  // matter for correctness, only for readability of the log.
  if (parser.isSet(optAutoDetectSelftest)) {
    lidarscan::CaptureWindow* cap = win.captureWindow();
    if (!projectDir.isEmpty()) cap->setProjectDir(projectDir);
    win.showWorkspaceNamed("capture");  // the panel is a dock; this is how it opens
    // Without a device-arming hook, showEvent() may ALSO fire the silent
    // on-open pass (if this project has never had Mid-360 settings saved)
    // racing this hook's own explicit trigger a few lines down —
    // CaptureWindow's own discovery_in_flight_ guard makes whichever one
    // loses the race a harmless no-op. So this hook does not just fire once:
    // it fires the explicit button click again after the FIRST pass (silent
    // or not) completes, guaranteeing at least one manual (dialog-shown,
    // always-overwrite) pass is exercised regardless of how the race went.
    //
    // WITH a device-arming hook there is no silent pass at all (suppressed
    // above) and no follow-up pass either: this hook's single pass runs to
    // completion and then hands over to the device self-test. One listener on
    // 56201 at a time, in a fixed order, is the whole point.
    const QString shotPath = parser.value(optAutoDetectShot);
    auto* firedFollowup = new bool(false);
    QObject::connect(cap, &lidarscan::CaptureWindow::autoDetectFinished, &app,
                     [cap, shotPath, firedFollowup, chainedDeviceSelftest, chainToSelftest](
                         bool mid360Found, bool d6Found, bool um982Found) {
                       std::fprintf(stderr,
                                    "[lidarscan] auto-detect-selftest: Mid-360 %s, D6 %s, "
                                    "UM982 %s\n",
                                    mid360Found ? "FOUND" : "not seen",
                                    d6Found ? "FOUND" : "not seen",
                                    um982Found ? "FOUND" : "not seen");
                       if (!shotPath.isEmpty()) {
                         QDir().mkpath(QFileInfo(shotPath).absolutePath());
                         const bool ok = cap->grab().save(shotPath);
                         std::fprintf(stderr, "[lidarscan] auto-detect-shot: %s -> %s\n",
                                      ok ? "OK" : "FAILED", shotPath.toUtf8().constData());
                       }
                       if (*firedFollowup) return;
                       *firedFollowup = true;
                       if (chainToSelftest && *chainedDeviceSelftest) {
                         std::fprintf(stderr,
                                      "[lidarscan] auto-detect-selftest: discovery complete, "
                                      "UDP 56201 released — chaining to --mid360-selftest\n");
                         QTimer::singleShot(200, cap, [chainedDeviceSelftest] {
                           (*chainedDeviceSelftest)();
                         });
                         return;
                       }
                       QTimer::singleShot(200, cap, [cap] { cap->triggerAutoDetectForCli(); });
                     });
    QTimer::singleShot(500, cap, [cap] { cap->triggerAutoDetectForCli(); });
  }

  if (parser.isSet(optMid360Selftest)) {
    const QString spec = parser.value(optMid360Selftest);
    const int colon = spec.indexOf(':');
    if (colon <= 0) {
      std::fprintf(stderr, "[lidarscan] --mid360-selftest wants HOST_IP:LIDAR_IP\n");
      return 6;
    }
    const QString hostIp = spec.left(colon);
    const QString lidarIp = spec.mid(colon + 1);
    lidarscan::CaptureWindow* cap = win.captureWindow();
    // If --mid360-record-into was also given, don't just self-test: record
    // into it for a few seconds and report what actually landed on disk —
    // the same readProject() summary the Projects panel shows, so this is
    // real evidence of the whole guided flow (self-test -> Record -> Stop)
    // against the S2 simulator, not just the self-test step.
    const QString mid360ProjectDir = parser.value(optMid360RecordInto);
    QObject::connect(
        cap, &lidarscan::CaptureWindow::selfTestFinished, &app,
        [cap, mid360ProjectDir](bool passed, const QString& detail) {
          std::fprintf(stderr, "[lidarscan] mid360-selftest %s — %s\n",
                       passed ? "PASSED" : "FAILED", detail.toUtf8().constData());
          if (!passed || mid360ProjectDir.isEmpty()) return;
          cap->triggerRecordForCli(mid360ProjectDir);
          std::fprintf(stderr, "[lidarscan] mid360-selftest: recording into %s for 3 s\n",
                       mid360ProjectDir.toUtf8().constData());
          QTimer::singleShot(3000, cap, [cap, mid360ProjectDir] {
            cap->triggerStopForCli();
            QTimer::singleShot(300, cap, [mid360ProjectDir] {
              const lidarscan::ProjectInfo info = lidarscan::readProject(mid360ProjectDir);
              if (!info.valid) {
                std::fprintf(stderr, "[lidarscan] mid360-selftest: readProject failed: %s\n",
                             info.error.toUtf8().constData());
                return;
              }
              std::fprintf(stderr,
                           "[lidarscan] mid360-selftest: recorded project — %llu chunks, "
                           "%llu bytes, %.2f s span, sealed=%s\n",
                           (unsigned long long)info.total_chunks,
                           (unsigned long long)info.total_bytes, info.duration_s,
                           info.sealed ? "true" : "false");
              for (const auto& s : info.streams) {
                std::fprintf(stderr, "[lidarscan]   stream %s: %llu chunks, %llu bytes\n",
                             s.name.toUtf8().constData(), (unsigned long long)s.chunks,
                             (unsigned long long)s.bytes);
              }
            });
          });
        });

    // --- round-5 field bug A: a still sensor must read 0.00 m/s ------------
    if (parser.isSet(optWalkSoak)) {
      const double soakS = std::max(1.0, parser.value(optWalkSoak).toDouble());
      auto* peak = new double(0.0);
      auto* fired = new int(0);
      auto* samples = new int(0);
      auto* nonzero = new int(0);
      auto* discos = new unsigned(0);
      QObject::connect(cap, &lidarscan::CaptureWindow::walkSpeedMeasured, &app,
                       [peak, fired, samples, nonzero, discos](double mps, bool valid,
                                                               unsigned d) {
                         *discos = d;
                         if (!valid) return;  // no hint is shown, so nothing is claimed
                         ++*samples;
                         if (mps > *peak) *peak = mps;
                         if (mps > 0.0) ++*nonzero;
                         if (mps > 1.5) ++*fired;  // the "ease off a little" threshold
                       });
      QObject::connect(
          cap, &lidarscan::CaptureWindow::selfTestFinished, &app,
          [soakS, peak, fired, samples, nonzero, discos](bool passed, const QString& detail) {
            if (!passed) {
              std::fprintf(stderr, "[lidarscan] walk-soak: cannot arm (%s)\n",
                           detail.toUtf8().constData());
              QCoreApplication::exit(9);
              return;
            }
            std::fprintf(stderr,
                         "[lidarscan] walk-soak: armed — watching the walk hint for %.0f s "
                         "against a gait-free source (nobody is carrying it)\n",
                         soakS);
            QTimer::singleShot(int(soakS * 1000.0), qApp,
                               [peak, fired, samples, nonzero, discos, soakS] {
                                 const bool ok = *fired == 0 && *nonzero == 0 && *samples > 0;
                                 std::fprintf(stderr,
                                              "[lidarscan] walk-soak: %.0f s, %d measured "
                                              "samples, peak %.3f m/s, %d non-zero, hint fired "
                                              "%dx, %u pose-frame reset(s) — %s\n",
                                              soakS, *samples, *peak, *nonzero, *fired, *discos,
                                              ok ? "PASS" : "FAIL");
                                 QCoreApplication::exit(ok ? 0 : 9);
                               });
          });
    }

    // --- round-5 field bug D: the live map must never stop moving ----------
    //
    // Runs the shipped preview against the shipped store and renderer with a
    // deliberately small ceiling (--live-store-pages), waits for the store to
    // FILL, and then keeps watching. PASS requires all four of:
    //   1. the store actually reached its ceiling (else the soak proved nothing);
    //   2. after that, the NEWEST point in the store keeps getting newer, in
    //      every window — this is "the live view is still moving";
    //   3. the page count never exceeds the ceiling and memory stays bounded;
    //   4. nothing is dropped once the window is recycling, and the renderer
    //      keeps uploading (the GPU mirror follows the store).
    // The pre-fix build fails 2 and 4 the instant the store fills.
    if (parser.isSet(optLiveMapSoak)) {
      const double soakS = std::max(2.0, parser.value(optLiveMapSoak).toDouble());
      const bool no_evict = parser.isSet(optLiveMapNoEvict);
      auto* fill_seen = new bool(false);
      auto* failures = new int(0);
      auto* windows = new int(0);
      auto* stalled = new int(0);
      auto* over_ceiling = new int(0);
      auto* newest_ns = new qint64(0);
      auto* uploads = new quint64(0);
      auto* upload_stalls = new int(0);
      auto* undrawn = new int(0);
      auto* dropped_at_fill = new quint64(0);
      QObject::connect(
          cap, &lidarscan::CaptureWindow::liveWindowEvicting, &app,
          [](quint64 resident, quint64 evicted) {
            std::fprintf(stderr,
                         "[lidarscan] live-map-soak: the window started recycling — "
                         "%llu points resident, %llu evicted so far\n",
                         (unsigned long long)resident, (unsigned long long)evicted);
          });
      QObject::connect(
          cap, &lidarscan::CaptureWindow::selfTestFinished, &app,
          [&host, &win, soakS, no_evict, fill_seen, failures, windows, stalled, over_ceiling,
           newest_ns, uploads, upload_stalls, undrawn,
           dropped_at_fill](bool passed, const QString& detail) {
            if (!passed) {
              std::fprintf(stderr, "[lidarscan] live-map-soak: cannot arm (%s)\n",
                           detail.toUtf8().constData());
              QCoreApplication::exit(10);
              return;
            }
            if (no_evict) {
              host.setLivePageEviction(false);
              std::fprintf(stderr,
                           "[lidarscan] live-map-soak: CONTROL RUN — page recycling turned "
                           "OFF after arming; this is the pre-fix behaviour and it must "
                           "FAIL\n");
            }
            auto* sampler = new QTimer(qApp);
            auto* clock = new QElapsedTimer();
            clock->start();
            QObject::connect(sampler, &QTimer::timeout, qApp, [&host, &win, sampler, clock,
                                                               soakS, fill_seen, failures,
                                                               windows, stalled, over_ceiling,
                                                               newest_ns, uploads,
                                                               upload_stalls, undrawn,
                                                               dropped_at_fill] {
              const scanengine::PageStoreStats st = host.pageStats();
              // The newest point in the store, by page time — "is the view
              // showing data that did not exist a moment ago?"
              qint64 newest = 0;
              if (const scanengine::PageStore* store = host.points()) {
                for (const scanengine::PageId id : store->page_ids()) {
                  const scanengine::PageView v = store->page_view(id);
                  if (v.valid()) newest = std::max<qint64>(newest, v.t_last_ns);
                }
              }
              const auto& cloud = win.viewport()->stats().cloud;
              const quint64 up = cloud.uploads;
              // "Is the newest page on screen?" — the LOD budget half of the
              // same bug: a live view that uploads everything and DRAWS only
              // the oldest pages is just as frozen as one that drops points.
              const bool newest_drawn = cloud.newest_page_drawn;
              // FULL is the moment the window ran out of room — the store
              // either started recycling (fixed) or started refusing points
              // (pre-fix). Both are measured, so the control run reaches the
              // same instant the fixed run does.
              if ((st.evicting || st.dropped_points > 0) && !*fill_seen) {
                *fill_seen = true;
                *dropped_at_fill = st.dropped_points;
                *newest_ns = newest;
                *uploads = up;
                return;  // start measuring from the fill moment
              }
              if (*fill_seen) {
                ++*windows;
                if (newest <= *newest_ns) ++*stalled;   // THE bug: the view froze
                if (up <= *uploads) ++*upload_stalls;   // the GPU mirror froze
                if (!newest_drawn) ++*undrawn;         // uploaded but not on screen
                if (st.pages > st.max_pages) ++*over_ceiling;
                if (st.dropped_points != *dropped_at_fill) ++*failures;
                *newest_ns = newest;
                *uploads = up;
              }
              if (clock->elapsed() < qint64(soakS * 1000.0)) return;
              sampler->stop();
              const bool ok = *fill_seen && *windows > 0 && *stalled == 0 &&
                              *upload_stalls == 0 && *undrawn == 0 && *over_ceiling == 0 &&
                              *failures == 0;
              std::fprintf(stderr,
                           "[lidarscan] live-map-soak: %.0f s — ceiling %u pages, full=%s, "
                           "recycling=%s, %d windows past the fill, %d stalled, %d upload "
                           "stalls, %d windows with the newest page NOT drawn, %d over "
                           "ceiling, %llu dropped, %llu pages evicted (%llu points), %llu "
                           "resident — %s\n",
                           soakS, st.max_pages, *fill_seen ? "yes" : "NO",
                           st.evicting ? "yes" : "NO", *windows, *stalled,
                           *upload_stalls, *undrawn, *over_ceiling,
                           (unsigned long long)st.dropped_points,
                           (unsigned long long)st.evicted_pages,
                           (unsigned long long)st.evicted_points,
                           (unsigned long long)st.resident_points, ok ? "PASS" : "FAIL");
              if (!*fill_seen) {
                std::fprintf(stderr,
                             "[lidarscan] live-map-soak: the store never filled — soak "
                             "longer or lower --live-store-pages; this run proved nothing\n");
              } else if (*undrawn > 0) {
                std::fprintf(stderr,
                             "[lidarscan] live-map-soak: the newest page was UPLOADED but "
                             "not DRAWN in %d of %d windows — the LOD budget is being spent "
                             "on the oldest pages, so the operator sees a frozen map while "
                             "the store is perfectly healthy\n",
                             *undrawn, *windows);
              } else if (*stalled > 0) {
                std::fprintf(stderr,
                             "[lidarscan] live-map-soak: the live map STOPPED ADVANCING in "
                             "%d of %d windows after the store filled — this is exactly the "
                             "field bug ('live view not moving even i move the lidar')\n",
                             *stalled, *windows);
              }
              QCoreApplication::exit(ok ? 0 : 10);
            });
            std::fprintf(stderr,
                         "[lidarscan] live-map-soak: armed; watching the live map for %.0f s "
                         "across the page-store ceiling\n",
                         soakS);
            sampler->start(500);
          });
    }

    // --- round-5 field bug C: unlimited record cycles per connect ----------
    //
    // Drives the SHIPPED slots (RecordCluster's Start and Stop, through
    // trigger*ForCli) N times over ONE arm, and reads each cycle's project back
    // off disk with the same readProject() the Projects panel uses. A cycle that
    // recorded nothing is a FAILURE, not a note: that is precisely the bug the
    // owner hit ("it only record when the first connected"). The last cycle's
    // exit code is the process's.
    const int cycles = parser.isSet(optRecordCycles) ? parser.value(optRecordCycles).toInt() : 0;
    if (cycles > 0) {
      const double cycleSeconds = std::max(0.5, parser.value(optRecordCycleSeconds).toDouble());
      QString root = parser.value(optRecordCyclesDir);
      if (root.isEmpty()) {
        // NOT applicationDirPath() any more. On an installed macOS build that
        // is LidarScan.app/Contents/MacOS — inside the bundle, which the next
        // install replaces — and this hook's scratch root is exactly what
        // ended up as the owner's capture root (field bug E). A temp
        // directory is what a scratch root should always have been.
        root = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                   .filePath("lidarscan-record-cycles");
      }
      // A stale project from a previous run would make an empty cycle look
      // healthy, so the whole root is cleared before the run.
      QDir(root).removeRecursively();
      QDir().mkpath(root);
      // Drive the SHIPPED Start button, not the explicit-directory CLI path:
      // setProjectDir() only sets the capture ROOT, and each cycle then goes
      // through resolveNewProjectDir()'s auto-naming exactly as a click with an
      // empty name field does. If anything in "every Start is a new project"
      // only works once, this is the path that shows it.
      cap->setProjectDir(root);
      auto* results = new std::vector<quint64>();
      auto* runCycle = new std::function<void(int)>();
      *runCycle = [cap, cycles, cycleSeconds, results, runCycle](int i) {
        const QString dir = cap->triggerStartWithAutoNameForCli();
        std::fprintf(stderr, "[lidarscan] record-cycles: cycle %d/%d -> %s (%.1f s)\n", i,
                     cycles, dir.isEmpty() ? "<Start refused>" : dir.toUtf8().constData(),
                     cycleSeconds);
        QTimer::singleShot(int(cycleSeconds * 1000.0), cap, [cap, dir, i, cycles, results,
                                                            runCycle] {
          cap->triggerStopForCli();
          // Stop seals synchronously, but the Projects-side reader wants the
          // manifest rewrite to have landed; the same 300 ms the
          // --mid360-record-into hook above waits.
          QTimer::singleShot(300, cap, [dir, i, cycles, results, runCycle] {
            const lidarscan::ProjectInfo info = lidarscan::readProject(dir);
            const quint64 chunks = info.valid ? info.total_chunks : 0;
            results->push_back(chunks);
            std::fprintf(stderr,
                         "[lidarscan] record-cycles: cycle %d -> %llu chunks, %llu bytes, "
                         "%.2f s, sealed=%s%s\n",
                         i, (unsigned long long)chunks,
                         (unsigned long long)(info.valid ? info.total_bytes : 0),
                         info.valid ? info.duration_s : 0.0,
                         info.valid && info.sealed ? "true" : "false",
                         info.valid ? "" : (" [readProject: " + info.error).toUtf8().constData());
            if (i < cycles) {
              QTimer::singleShot(400, qApp, [runCycle, i] { (*runCycle)(i + 1); });
              return;
            }
            // Verdict. "Comparable" is deliberately loose (a tenth of the best
            // cycle): the point is empty-vs-not, not sample-accurate parity.
            quint64 best = 0;
            for (quint64 c : *results) best = std::max(best, c);
            int bad = 0;
            for (std::size_t k = 0; k < results->size(); ++k) {
              const quint64 c = (*results)[k];
              const bool ok = c > 0 && (best == 0 || c * 10 >= best);
              if (!ok) ++bad;
              std::fprintf(stderr, "[lidarscan] record-cycles: cycle %d %s (%llu chunks)\n",
                           int(k + 1), ok ? "OK" : "EMPTY/DEGENERATE", (unsigned long long)c);
            }
            std::fprintf(stderr, "[lidarscan] record-cycles: %d/%d cycles recorded — %s\n",
                         cycles - bad, cycles, bad == 0 ? "PASS" : "FAIL");
            QCoreApplication::exit(bad == 0 ? 0 : 7);
          });
        });
      };
      auto* startedCycles = new bool(false);
      QObject::connect(cap, &lidarscan::CaptureWindow::selfTestFinished, &app,
                       [runCycle, startedCycles, cycles](bool passed, const QString& detail) {
                         if (*startedCycles) return;  // a re-arm must not restart the run
                         *startedCycles = true;
                         if (!passed) {
                           std::fprintf(stderr,
                                        "[lidarscan] record-cycles: cannot arm (%s) — no cycles "
                                        "run\n",
                                        detail.toUtf8().constData());
                           QCoreApplication::exit(7);
                           return;
                         }
                         std::fprintf(stderr,
                                      "[lidarscan] record-cycles: armed; running %d record "
                                      "cycles on this ONE connect\n",
                                      cycles);
                         (*runCycle)(1);
                       });
    }

    // The one place the self-test is actually kicked off. Either a 500 ms
    // timer owns it (this hook alone) or the auto-detect chain does (both
    // hooks) — never both, and never concurrently: the two hooks used to fire
    // on identical 500 ms timers, which put a discovery listener and an
    // SdkInit on UDP 56201 at the same instant.
    *chainedDeviceSelftest = [cap, hostIp, lidarIp] {
      cap->runMid360SelfTestForCli(hostIp, lidarIp);
    };
    if (cancelProbe) {
      // The deliberate collision: a pass starts, and 400 ms later — well
      // inside its first 1 s listen slice — the device arms. CaptureWindow
      // must cancel the pass, WAIT for the socket, and then start the device.
      win.showWorkspaceNamed("capture");
      std::fprintf(stderr,
                   "[lidarscan] auto-detect-cancel-selftest: starting a discovery pass, "
                   "then arming the device 400 ms into it\n");
      QTimer::singleShot(500, cap, [cap] { cap->triggerAutoDetectForCli(); });
      QTimer::singleShot(900, cap, [chainedDeviceSelftest] { (*chainedDeviceSelftest)(); });
      // ...and the other direction of the same exclusion. Round 5 changed what
      // "the other direction" MEANS for a live PREVIEW: a click with the device
      // merely previewing now disarms the preview, takes the port, runs the pass
      // and re-arms (see CaptureWindow::onAutoDetectClicked — refusing here
      // would leave an auto-armed operator unable to ever re-detect). The
      // exclusion itself is unchanged: never two holders of 56201 at once, and a
      // RECORDING is still refused outright.
      QTimer::singleShot(5000, cap, [cap] {
        std::fprintf(stderr,
                     "[lidarscan] auto-detect-cancel-selftest: clicking Auto-detect with "
                     "the device armed — the preview must step aside, not overlap\n");
        cap->triggerAutoDetectForCli();
      });
    } else if (chainToSelftest) {
      std::fprintf(stderr,
                   "[lidarscan] mid360-selftest: chained behind --auto-detect-selftest "
                   "(no concurrent UDP 56201 listener)\n");
    } else if (flowDemo) {
      // --capture-flow-demo owns the arm: it fires once the inline on-open
      // discovery pass has finished and released UDP 56201 (see its block
      // below), so the two never overlap.
      std::fprintf(stderr,
                   "[lidarscan] mid360-selftest: chained behind --capture-flow-demo's "
                   "inline auto-detect pass\n");
    } else {
      QTimer::singleShot(500, cap, [chainedDeviceSelftest] { (*chainedDeviceSelftest)(); });
    }
  }

  // --- round-5 workflow evidence: the whole capture flow, end to end -------
  //
  // Everything this hook drives is the SHIPPED path: the workspace switch is the
  // rail's own, the discovery pass is the panel's own on-open pass, the arm is
  // armPreview() (what an auto-detect hit calls), the display controls go through
  // their real sliders, and Start/Stop are the record cluster's own slots. The
  // only substitution is WHERE the addresses come from — the S2 simulator is on
  // loopback and the replayed real beacon names a 192.168.1.x lidar, so the
  // addresses are supplied by --mid360-selftest instead of by the beacon. See
  // NOTES.md §17.
  if (flowDemo) {
    if (!cliArmsDevice) {
      std::fprintf(stderr,
                   "[lidarscan] --capture-flow-demo needs --mid360-selftest HOST:LIDAR for "
                   "the addresses to arm\n");
      return 6;
    }
    const QString prefix = QFileInfo(parser.value(optCaptureFlowDemo)).absoluteFilePath();
    QDir().mkpath(QFileInfo(prefix).absolutePath());
    lidarscan::CaptureWindow* cap = win.captureWindow();
    win.showWorkspaceNamed("capture");

    // 1. the inline auto-detect pass (no dialog) -> then arm.
    QObject::connect(cap, &lidarscan::CaptureWindow::autoDetectFinished, &app,
                     [chainedDeviceSelftest, cap](bool mid360, bool d6, bool um982) {
                       std::fprintf(stderr,
                                    "[lidarscan] capture-flow-demo: inline auto-detect done "
                                    "(Mid-360 %s, D6 %s, UM982 %s) — arming\n",
                                    mid360 ? "FOUND" : "not seen", d6 ? "detected" : "not seen",
                                    um982 ? "FOUND" : "not seen");
                       QTimer::singleShot(200, cap, [chainedDeviceSelftest] {
                         (*chainedDeviceSelftest)();
                       });
                     });

    // 2. live preview up -> move the live display controls, shoot the PREVIEW
    //    state (viewport + panel composited by captureScreenshot's -window.png).
    QObject::connect(
        cap, &lidarscan::CaptureWindow::selfTestFinished, &app,
        [cap, prefix, &win](bool passed, const QString& detail) {
          std::fprintf(stderr, "[lidarscan] capture-flow-demo: arm %s — %s\n",
                       passed ? "LIVE" : "FAILED", detail.toUtf8().constData());
          if (!passed) return;
          QTimer::singleShot(1200, cap, [cap, prefix, &win] {
            // TWO moves, deliberately: the rate is persisted in QSettings, so a
            // single move to a fixed value is a no-op on the run after the one
            // that set it (which is exactly how the missing applyLiveRefreshRate()
            // hop was found). Two different targets guarantee a real change, and
            // the viewport's own cap is read back after each.
            const double hz1 = cap->setLiveRefreshHzForCli(12.0);
            const double cap1 = win.viewport()->maxFps();
            const double hz2 = cap->setLiveRefreshHzForCli(24.0);
            const double px = cap->setPointSizeForCli(2.5);
            std::fprintf(stderr,
                         "[lidarscan] capture-flow-demo: live controls — refresh %.0f fps "
                         "(viewport cap %.0f) then %.0f fps (viewport cap %.0f), point "
                         "size %.2f px (A14 model)\n",
                         hz1, cap1, hz2, win.viewport()->maxFps(), px);
            QTimer::singleShot(900, cap, [cap, prefix, &win] {
              win.viewport()->captureScreenshot(prefix + "-preview.png");
              std::fprintf(stderr,
                           "[lidarscan] capture-flow-demo: PREVIEW shot -> %s (+ -window.png)\n",
                           (prefix + "-preview.png").toUtf8().constData());

              // 2b. THE TRAJECTORY TRAIL (item 18), measured rather than
              // asserted. The live LIO trail against a STATIONARY simulator is
              // half a metre long inside a 20 m cloud — real, but too small to
              // see in a screenshot — so this pushes a metre-scale synthetic
              // path through the SAME ViewportWindow::setTrajectoryTrail() the
              // panel drives, shoots it, and reports how much of the frame
              // changed. The synthetic path is labelled as such in the log and
              // is dropped immediately afterwards; the panel's next 10 Hz poll
              // puts the real (live) trail back.
              // Deliberately ABOVE the cloud: a path at floor height inside a
              // scanned room is depth-tested behind the ceiling from this
              // camera, which is correct rendering but useless as evidence.
              std::vector<std::array<float, 3>> synth;
              for (int i = 0; i <= 40; ++i) {
                const float t = float(i) / 40.0f;
                synth.push_back({-4.0f + 8.0f * t, -3.0f + 6.0f * t * t, 4.0f + 2.0f * t});
              }
              cap->injectTrailForCli(synth);
              QTimer::singleShot(700, cap, [cap, prefix, &win] {
                win.viewport()->captureScreenshot(prefix + "-trail.png");
                double changedFrac = 0.0;
                const double d =
                    frameDelta(prefix + "-preview.png", prefix + "-trail.png", &changedFrac);
                const qint64 emberBefore = countEmberPixels(prefix + "-preview.png");
                const qint64 emberAfter = countEmberPixels(prefix + "-trail.png");
                std::fprintf(stderr,
                             "[lidarscan] capture-flow-demo: TRAIL shot -> %s | synthetic "
                             "41-vertex path | ember pixels %lld -> %lld | mean |delta| %.4f "
                             "over %.2f%% of the frame — %s\n",
                             (prefix + "-trail.png").toUtf8().constData(),
                             (long long)emberBefore, (long long)emberAfter, d, changedFrac * 100.0,
                             (emberAfter > emberBefore + 200)
                                 ? "TRAIL DRAWN"
                                 : "NO TRAIL PIXELS (investigate)");
                cap->injectTrailForCli({});

                // 3. Start with an EMPTY name — the auto-naming path.
                const QString dir = cap->triggerStartWithAutoNameForCli();
                std::fprintf(stderr,
                             "[lidarscan] capture-flow-demo: Start pressed with an EMPTY name "
                             "-> %s\n",
                             dir.toUtf8().constData());
              });
            });
          });
        });

    // 4. recording -> shoot the RECORDING state, Stop, report what was sealed.
    QObject::connect(
        cap, &lidarscan::CaptureWindow::captureStarted, &app,
        [cap, prefix, &win](const QString& dir) {
          QTimer::singleShot(2500, cap, [cap, prefix, &win, dir] {
            win.viewport()->captureScreenshot(prefix + "-recording.png");
            std::fprintf(stderr,
                         "[lidarscan] capture-flow-demo: RECORDING shot -> %s (+ -window.png)\n",
                         (prefix + "-recording.png").toUtf8().constData());
            cap->triggerStopForCli();
            QTimer::singleShot(600, cap, [prefix, &win, dir] {
              const lidarscan::ProjectInfo info = lidarscan::readProject(dir);
              std::fprintf(stderr,
                           "[lidarscan] capture-flow-demo: sealed %s — valid=%s, %llu chunks, "
                           "%llu bytes, %.2f s, sealed=%s, profile %s\n",
                           dir.toUtf8().constData(), info.valid ? "yes" : "no",
                           (unsigned long long)info.total_chunks,
                           (unsigned long long)info.total_bytes, info.duration_s,
                           info.sealed ? "true" : "false", info.profile.toUtf8().constData());
              for (const auto& s : info.streams) {
                std::fprintf(stderr, "[lidarscan] capture-flow-demo:   stream %s: %llu chunks\n",
                             s.name.toUtf8().constData(), (unsigned long long)s.chunks);
              }
              // Did it land in the LIBRARY (round 5 item 9: "project appears in
              // the Projects tab")? The library is QSettings("recentProjects"),
              // and the open project is what the Projects panel previews.
              const QStringList recents = QSettings().value("recentProjects").toStringList();
              std::fprintf(stderr,
                           "[lidarscan] capture-flow-demo: in the library: %s · previewed as "
                           "the open project: %s\n",
                           recents.contains(dir) ? "yes" : "NO",
                           win.project().dir == dir ? "yes" : "NO");
              // And the display parameters the operator set during the capture
              // are the project's saved default view (round-5 follow-up item 3).
              const QString pp = lidarscan::DisplayParamsDock::paramsPathFor(dir);
              std::fprintf(stderr,
                           "[lidarscan] capture-flow-demo: display params saved with the "
                           "project: %s (%s)\n",
                           QFileInfo::exists(pp) ? "yes" : "NO", pp.toUtf8().constData());
              win.showWorkspaceNamed("projects");
              QTimer::singleShot(500, &win, [prefix, &win] {
                win.viewport()->captureScreenshot(prefix + "-projects.png");
                std::fprintf(stderr,
                             "[lidarscan] capture-flow-demo: PROJECTS shot -> %s (+ -window.png)\n",
                             (prefix + "-projects.png").toUtf8().constData());
              });
            });
          });
        });
  }

  // --- round-5 follow-up item 4: Processing and Merge folded into Projects ---
  //
  // Drives the REAL selection model and the REAL action buttons' slots, so what
  // is proven is the shipped gating (one project -> Process/Export; two or more
  // -> Merge), not a parallel harness. ProcessingDock and MergeDock are the same
  // docks doing the same work — only their entry point moved.
  if (parser.isSet(optProjectsActionsDemo)) {
    const QString prefix = QFileInfo(parser.value(optProjectsActionsDemo)).absoluteFilePath();
    QDir().mkpath(QFileInfo(prefix).absolutePath());
    win.showWorkspaceNamed("projects");
    QTimer::singleShot(900, &win, [&win, prefix] {
      const int one = win.selectRecentProjectsForCli(1);
      std::fprintf(stderr, "[lidarscan] projects-actions-demo: selected %d — %s\n", one,
                   win.projectActionStateForCli().toUtf8().constData());
      win.viewport()->captureScreenshot(prefix + "-one-selected.png");
      QTimer::singleShot(600, &win, [&win, prefix] {
        win.triggerProcessSelectedForCli();
        std::fprintf(stderr, "[lidarscan] projects-actions-demo: Process… — %s\n",
                     win.projectActionStateForCli().toUtf8().constData());
        win.viewport()->captureScreenshot(prefix + "-processing.png");
        QTimer::singleShot(600, &win, [&win, prefix] {
          const int two = win.selectRecentProjectsForCli(2);
          std::fprintf(stderr, "[lidarscan] projects-actions-demo: selected %d — %s\n", two,
                       win.projectActionStateForCli().toUtf8().constData());
          if (two < 2) {
            std::fprintf(stderr,
                         "[lidarscan] projects-actions-demo: fewer than two projects in the "
                         "library — Merge cannot be exercised on this machine\n");
            return;
          }
          win.triggerMergeSelectedForCli();
          QTimer::singleShot(2500, &win, [&win, prefix] {
            std::fprintf(stderr, "[lidarscan] projects-actions-demo: Merge selected… — %s\n",
                         win.projectActionStateForCli().toUtf8().constData());
            if (const auto* rep = win.mergeDock()->reportOrNull()) {
              std::fprintf(stderr,
                           "[lidarscan] projects-actions-demo: merge workbench holds %zu "
                           "sessions\n",
                           rep->sessions.size());
            }
            win.viewport()->captureScreenshot(prefix + "-merge.png");
          });
        });
      });
    });
  }

  // C4 evidence hook: a REAL A15 kPostProcess job through ProcessingDock's own
  // JobQueue (the exact queue "Post-process…" submits to), polled to
  // completion and, on success, loaded into the viewport via the same
  // produced_store() path the "Load result" button drives.
  if (parser.isSet(optPostE2e)) {
    const QString dir = parser.value(optPostE2e);
    QTimer::singleShot(300, &win, [&win, dir] {
      lidarscan::ProcessingDock* pd = win.processingDock();
      scanengine::jobs::JobSpec spec;
      spec.kind = scanengine::jobs::JobKind::kPostProcess;
      spec.post.lscan_dir = dir.toStdString();
      const auto sub = pd->queue().submit(spec);
      if (!sub.ok()) {
        std::fprintf(stderr, "[lidarscan] post-e2e: submit failed: %s\n",
                     scanengine::error_str(sub.error()));
        return;
      }
      const quint64 jobId = sub.value();
      std::fprintf(stderr, "[lidarscan] post-e2e: job #%llu submitted for %s\n",
                   (unsigned long long)jobId, dir.toUtf8().constData());
      auto* poll = new QTimer(&win);
      poll->setInterval(200);
      QObject::connect(poll, &QTimer::timeout, &win, [&win, pd, jobId, poll] {
        const auto job = pd->queue().status(jobId);
        std::fprintf(stderr, "[lidarscan] post-e2e: job #%llu %s — %.0f%% (%s) %s\n",
                     (unsigned long long)jobId, scanengine::jobs::to_string(job.state),
                     double(job.progress) * 100.0, job.stage.c_str(), job.message.c_str());
        if (job.state != scanengine::jobs::JobState::kDone &&
            job.state != scanengine::jobs::JobState::kFailed) {
          return;
        }
        poll->stop();
        poll->deleteLater();
        if (job.state == scanengine::jobs::JobState::kFailed) {
          std::fprintf(stderr, "[lidarscan] post-e2e: FAILED — %s (%s)\n",
                       scanengine::error_str(job.error), job.message.c_str());
          return;
        }
        auto store = pd->queue().produced_store(jobId);
        if (!store) {
          std::fprintf(stderr, "[lidarscan] post-e2e: job done but produced_store() is null\n");
          return;
        }
        std::fprintf(stderr, "[lidarscan] post-e2e: DONE — %llu points in the result cloud\n",
                     (unsigned long long)store->total_points());
        win.viewport()->setPointStore(store.get());
        win.viewport()->fitView();
        win.planDock()->setPointStore(store.get());
        std::fprintf(stderr, "[lidarscan] post-e2e: result loaded into the viewport\n");
      });
      poll->start();
    });
  }

  // C4 evidence hook: prove the cloud path fails GRACEFULLY with no server —
  // a real socket attempt (QtHttpTransport), not the scripted fake transport
  // A15's own tests use, per the task's explicit "the fake-transport path is
  // not for UI".
  if (parser.isSet(optCloudSelftest)) {
    const QString url = parser.value(optCloudSelftest);
    // A real, existing file (its CONTENT is irrelevant — this exercises the
    // network path, not the zip format) so CloudSubmitClient gets past its
    // local size-cap check and actually reaches the (unreachable) URL.
    const QString zipPath = QDir::temp().filePath("cloud-submit-selftest.zip");
    QFile f(zipPath);
    if (f.open(QIODevice::WriteOnly)) {
      f.write("not a real zip, just needs to exist");
      f.close();
    }
    QTimer::singleShot(300, &win, [&win, url, zipPath] {
      static lidarscan::QtHttpTransport transport(2000);
      lidarscan::ProcessingDock* pd = win.processingDock();
      scanengine::jobs::JobSpec spec;
      spec.kind = scanengine::jobs::JobKind::kCloudSubmit;
      spec.cloud.transport = &transport;
      spec.cloud.cloud_config.base_url = url.toStdString();
      spec.cloud.cloud_config.auth_token = "selftest-token";
      spec.cloud.local_zip_path = zipPath.toStdString();
      const auto sub = pd->queue().submit(spec);
      if (!sub.ok()) {
        std::fprintf(stderr, "[lidarscan] cloud-submit-selftest: submit failed: %s\n",
                     scanengine::error_str(sub.error()));
        return;
      }
      const quint64 jobId = sub.value();
      std::fprintf(stderr, "[lidarscan] cloud-submit-selftest: job #%llu submitted against %s\n",
                   (unsigned long long)jobId, url.toUtf8().constData());
      auto* poll = new QTimer(&win);
      poll->setInterval(200);
      QObject::connect(poll, &QTimer::timeout, &win, [pd, jobId, poll] {
        const auto job = pd->queue().status(jobId);
        if (job.state != scanengine::jobs::JobState::kDone &&
            job.state != scanengine::jobs::JobState::kFailed) {
          return;
        }
        poll->stop();
        poll->deleteLater();
        std::fprintf(stderr,
                     "[lidarscan] cloud-submit-selftest: job #%llu settled as %s (error=%s, "
                     "message=%s) — the app is still running, which is the point\n",
                     (unsigned long long)jobId, scanengine::jobs::to_string(job.state),
                     scanengine::error_str(job.error), job.message.c_str());
      });
      poll->start();
    });
  }

  // C5 evidence hooks: load the A12 test-fixture building, extract, export.
  const int planDelayMs = int(parser.value(optPlanDelay).toDouble() * 1000.0);
  if (parser.isSet(optPlanFixture)) {
    QTimer::singleShot(planDelayMs, &win, [&win] {
      win.loadSyntheticBuildingFixture();
      std::fprintf(stderr, "[lidarscan] plan-fixture: loaded\n");
    });
  }
  if (parser.isSet(optPlanExtract)) {
    QTimer::singleShot(planDelayMs + 300, &win, [&win] {
      win.planDock()->runExtractionForCli();
      const auto& m = win.planDock()->model();
      std::fprintf(stderr,
                   "[lidarscan] plan-extract: %zu walls, %zu openings, %zu rooms, %.2f m2 total "
                   "area, slice %.2f..%.2f m\n",
                   m.walls.size(), m.openings.size(), m.rooms.size(), m.stats.total_room_area_m2,
                   m.slice_z_min_m, m.slice_z_max_m);
      for (const auto& r : m.rooms) {
        std::fprintf(stderr, "[lidarscan] plan-extract:   room %s: %.4f m2\n", r.label.c_str(),
                     r.area_m2);
      }
    });
  }
  if (parser.isSet(optPlanExportDxf)) {
    const QString path = parser.value(optPlanExportDxf);
    QTimer::singleShot(planDelayMs + 600, &win, [&win, path] {
      QString err;
      const bool ok = win.planDock()->exportDxfForCli(path, &err);
      std::fprintf(stderr, "[lidarscan] plan-export-dxf: %s -> %s\n",
                   ok ? "OK" : ("FAILED: " + err).toUtf8().constData(), path.toUtf8().constData());
    });
  }
  if (parser.isSet(optPlanExportPdf)) {
    const QString path = parser.value(optPlanExportPdf);
    QTimer::singleShot(planDelayMs + 600, &win, [&win, path] {
      QString err;
      const bool ok = win.planDock()->exportPdfForCli(path, &err);
      std::fprintf(stderr, "[lidarscan] plan-export-pdf: %s -> %s\n",
                   ok ? "OK" : ("FAILED: " + err).toUtf8().constData(), path.toUtf8().constData());
    });
  }
  if (parser.isSet(optPlanShot)) {
    const QString path = QFileInfo(parser.value(optPlanShot)).absoluteFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // Raise the Floor plan tab first: it is tabbed behind Display parameters
    // by default (MainWindow::buildUi()), and a background tab's
    // QGraphicsView may never have received a real resize/layout pass to
    // fit the plan into, since Qt does not necessarily lay out a dock tab
    // that has never been shown.
    win.planDock()->show();
    win.planDock()->raise();
    QTimer::singleShot(planDelayMs + 900, &win, [&win, path] {
      const bool ok = win.planDock()->grab().save(path);
      std::fprintf(stderr, "[lidarscan] plan-shot: %s -> %s\n", ok ? "OK" : "FAILED",
                   path.toUtf8().constData());
    });
  }

  // C6 evidence hook: the whole merge workbench flow against a real
  // MergeProject, driven through MergeDock's own *ForCli methods (the same
  // code the buttons call). See engine/docs/A13-merge.md for what each
  // number below means.
  if (parser.isSet(optMergeFixtureEvidence)) {
    QTimer::singleShot(500, &win, [&win] {
      lidarscan::MergeDock* md = win.mergeDock();
      lidarscan::MergeFixture fixture;  // deterministic — same geometry addFixtureSessionsForCli() builds

      auto reportTruth = [&](int session, const char* label) {
        const auto* rep = md->reportOrNull();
        if (!rep) return;
        double truth[16];
        fixture.truthWorldFromSession(session, truth);
        for (const auto& sm : rep->sessions) {
          if (int(sm.id) != session) continue;
          double rotDeg = 0.0, transMm = 0.0;
          scanengine::se3::transform_error(sm.world_from_session, truth, &rotDeg, &transMm);
          std::fprintf(stderr,
                       "[lidarscan] merge-fixture-evidence: %s session %d vs ground truth: "
                       "%.4f mm / %.6f deg (align=%s)\n",
                       label, session, transMm, rotDeg, scanengine::merge::to_string(sm.align));
        }
      };

      const int n = md->addFixtureSessionsForCli(true);
      std::fprintf(stderr,
                   "[lidarscan] merge-fixture-evidence: %d sessions added (30x12x3 m building, "
                   "sessions 0-1 / 1-2 overlap 4 m, 0-2 share nothing — see "
                   "engine/tests/test_merge.cpp)\n",
                   n);

      scanengine::merge::MergeProject::GeorefAlignReport georefRep;
      QString gerr;
      const bool gok = md->alignGeoreferencedForCli(&georefRep, &gerr);
      std::fprintf(stderr,
                   "[lidarscan] merge-fixture-evidence: georeferenced auto-align %s — aligned=%u "
                   "skipped=%u max ENU-origin separation=%.2f m%s\n",
                   gok ? "OK" : "FAILED", georefRep.aligned, georefRep.skipped,
                   georefRep.max_origin_separation_m,
                   gok ? "" : (" (" + gerr + ")").toUtf8().constData());
      reportTruth(0, "georef");
      reportTruth(1, "georef");
      reportTruth(2, "georef");

      // Override session 1 with the 3-point MANUAL path, against the anchor
      // (session 0) — the exact 4 physical features
      // engine/tests/test_merge.cpp calls kPickWorld, exact (no click noise),
      // fed through the SAME recordPick()/align_from_correspondences() path
      // a real 3-click UI sequence would produce.
      std::vector<std::array<float, 3>> src, tgt;
      for (std::size_t i = 0; i < 3; ++i) {
        float s[3], t[3];
        fixture.pick(1, i, 0.0, 1, s, t);
        src.push_back({s[0], s[1], s[2]});
        tgt.push_back({t[0], t[1], t[2]});
      }
      scanengine::merge::CorrespondenceSolution sol;
      QString merr;
      const bool mok = md->alignManualForCli(1, src, tgt, &sol, &merr);
      std::fprintf(stderr,
                   "[lidarscan] merge-fixture-evidence: 3-point manual align (session 1, exact "
                   "picks) %s — rms=%.4f mm max_residual=%.4f mm implied_scale=%.6f%s\n",
                   mok ? "OK" : "FAILED", sol.rms_m * 1000.0, sol.max_residual_m * 1000.0,
                   sol.implied_scale, mok ? "" : (" (" + merr + ")").toUtf8().constData());
      reportTruth(1, "manual-3pt");

      QString rerr;
      const bool rok = md->refineForCli(&rerr);
      std::fprintf(stderr, "[lidarscan] merge-fixture-evidence: refine %s\n",
                   rok ? "OK" : ("FAILED: " + rerr).toUtf8().constData());
      if (const auto* rep = md->reportOrNull()) {
        for (const auto& pr : rep->pairs) {
          std::fprintf(
              stderr,
              "[lidarscan] merge-fixture-evidence: pair %u<->%u: rms %.2f mm -> %.2f mm, "
              "overlap %.3f/%.3f, %u iterations (%u rolled back), converged=%s, low_overlap=%s%s\n",
              pr.session_a, pr.session_b, pr.rms_before_m * 1000.0, pr.rms_residual_m * 1000.0,
              pr.overlap_a_in_b, pr.overlap_b_in_a, pr.iterations, pr.rejected_steps,
              pr.converged ? "yes" : "no", pr.low_overlap ? "yes" : "no",
              *pr.blocker ? (QString(" blocker=") + pr.blocker).toUtf8().constData() : "");
        }
        if (rep->relaxed) {
          std::fprintf(stderr,
                       "[lidarscan] merge-fixture-evidence: global relaxation chi2 %.6g -> %.6g "
                       "in %u iterations\n",
                       rep->graph.initial_chi2, rep->graph.final_chi2, rep->graph.iterations);
        }
      }
      reportTruth(0, "post-refine");
      reportTruth(1, "post-refine");
      reportTruth(2, "post-refine");

      QString berr;
      const bool bok = md->buildAndPublishForCli(true, &berr);
      if (bok) {
        if (const auto* rep = md->reportOrNull()) {
          std::fprintf(stderr,
                       "[lidarscan] merge-fixture-evidence: build OK — %llu input -> %llu merged "
                       "points (%llu dedup-dropped, %llu priority-dropped), %u pages (%u shared)\n",
                       (unsigned long long)rep->input_points, (unsigned long long)rep->merged_points,
                       (unsigned long long)rep->dedup_dropped_points,
                       (unsigned long long)rep->priority_dropped_points, rep->pages_appended,
                       rep->pages_shared);
        }
      } else {
        std::fprintf(stderr, "[lidarscan] merge-fixture-evidence: build FAILED: %s\n",
                     berr.toUtf8().constData());
      }
    });
  }

  if (parser.isSet(optMergeAddProject)) {
    const QString spec = parser.value(optMergeAddProject);
    const int colon = spec.indexOf(':');
    if (colon <= 0) {
      std::fprintf(stderr, "[lidarscan] --merge-add-project wants LSCAN_DIR:PROVENANCE_ID\n");
      return 6;
    }
    const QString dir = spec.left(colon);
    const QString provenance = spec.mid(colon + 1);
    QTimer::singleShot(300, &win, [&win, dir, provenance] {
      QString err;
      const bool ok = win.mergeDock()->addProjectSessionForCli(dir, provenance, &err);
      std::fprintf(stderr, "[lidarscan] merge-add-project: %s (%s) -> %s%s\n",
                   dir.toUtf8().constData(), provenance.toUtf8().constData(), ok ? "OK" : "FAILED",
                   ok ? "" : (": " + err).toUtf8().constData());
      if (ok) {
        if (const auto* rep = win.mergeDock()->reportOrNull()) {
          for (const auto& sm : rep->sessions) {
            if (sm.provenance_id == provenance.toStdString()) {
              std::fprintf(stderr,
                           "[lidarscan] merge-add-project: session %u '%s' — %llu points "
                           "decoded via unpaced ReplaySource into a real Engine/PageStore\n",
                           sm.id, sm.provenance_id.c_str(),
                           (unsigned long long)sm.input_points);
            }
          }
        }
      }
    });
  }

  if (parser.isSet(optMergeDockShot)) {
    const QString path = QFileInfo(parser.value(optMergeDockShot)).absoluteFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int ms = int(parser.value(optMergeDockShotDelay).toDouble() * 1000.0);
    win.mergeDock()->show();
    win.mergeDock()->raise();
    // Float and resize tall so the grab() below captures the residual chart
    // too, not just whatever fits in the tabbed dock area's allotted height.
    win.mergeDock()->setFloating(true);
    win.mergeDock()->resize(660, 1350);
    QTimer::singleShot(ms, &win, [&win, path] {
      win.mergeDock()->selectPairForCli(0);
      const bool ok = win.mergeDock()->grab().save(path);
      std::fprintf(stderr, "[lidarscan] merge-dock-shot: %s -> %s\n", ok ? "OK" : "FAILED",
                   path.toUtf8().constData());
    });
  }

  // S3 resized the window every frame for 10 s at ~12M points and survived
  // 1,105 swapchain recreates. The productionized viewport coalesces resizes to
  // at most one recreate per frame, so this reruns the same stress against the
  // real app and reports what it cost.
  const double stormSeconds = parser.value(optResizeStorm).toDouble();
  if (stormSeconds > 0) {
    auto* storm = new QTimer(&win);
    auto* elapsed = new QElapsedTimer();
    elapsed->start();
    storm->setTimerType(Qt::PreciseTimer);
    QObject::connect(storm, &QTimer::timeout, &win, [&win, storm, elapsed, stormSeconds] {
      const double t = elapsed->elapsed() / 1000.0;
      if (t > stormSeconds) {
        storm->stop();
        win.resize(1580, 940);
        const auto& s = win.viewport()->stats();
        std::fprintf(stderr,
                     "[lidarscan] resize storm: %.1f s, %u resize events -> %u swapchain "
                     "recreates, still rendering at %.1f fps (cpu p95 %.2f ms)\n",
                     stormSeconds, s.resize_events, s.swapchain_recreates, s.fps, s.cpu_ms_p95);
        return;
      }
      const int w = 900 + int(520 * (0.5 + 0.5 * std::sin(t * 2.7)));
      const int h = 620 + int(300 * (0.5 + 0.5 * std::sin(t * 1.37 + 0.9)));
      win.resize(w, h);
    });
    storm->start(8);
  }

  const double quitAfter = parser.value(optQuit).toDouble();
  if (quitAfter > 0) {
    QTimer::singleShot(int(quitAfter * 1000.0), &app, &QApplication::quit);
  }

  return app.exec();
}
