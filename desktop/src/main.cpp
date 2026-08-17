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
#include <QMessageBox>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app/CaptureWindow.h"
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

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("LidarScan");
  QCoreApplication::setApplicationName("LidarScan Desktop");
  QCoreApplication::setApplicationVersion("0.1.0");

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
  parser.addOption(optAutoDetectShot);
  parser.addOption(optMid360RecordInto);
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
      "Redesign evidence hook: PREFIX — open the capture window and shoot the "
      "record cluster in each state the C2 machine can be in without hardware "
      "(untested / testing), then, if --mid360-selftest also ran, armed / "
      "recording / paused.",
      "prefix");
  parser.addOption(optCaptureClusterDemo);
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

  lidarscan::EngineHost host;
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
    // Mean ABSOLUTE per-pixel difference between the two shots, not a
    // brightness comparison. Growing the points does not simply "add light":
    // at 12 px the near surfaces occlude the far ones, so this fixture gets
    // measurably DARKER (53.5 -> 35.9 mean luma on the first run of this
    // hook). A signed brightness test would have called that a failure. What
    // is actually being asserted is that the frame changed at all, and by how
    // much of the frame.
    auto frameDelta = [](const QString& a, const QString& b, double* changedFrac) -> double {
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
    };
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
    QTimer::singleShot(t0 + 2000, &win, [&win, prefix, frameDelta] {
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

  if (parser.isSet(optCaptureClusterDemo)) {
    const QString prefix = QFileInfo(parser.value(optCaptureClusterDemo)).absoluteFilePath();
    QDir().mkpath(QFileInfo(prefix).absolutePath());
    lidarscan::CaptureWindow* cap = win.captureWindow();
    cap->setProjectDir(parser.value(optProject));
    cap->show();
    cap->raise();
    auto shot = [cap](const QString& path, const char* label) {
      const bool ok = cap->grab().save(path);
      std::fprintf(stderr, "[lidarscan] capture-cluster-demo: %s %s -> %s\n", label,
                   ok ? "OK" : "FAILED", path.toUtf8().constData());
    };
    // The gated state needs no hardware at all — that IS the state the owner
    // said was missing, so it is shot first and unconditionally.
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
                         // The badge lives on the MAIN window's viewport, not
                         // in the capture dialog, so it needs its own grab.
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
    cap->show();
    cap->raise();
    // showEvent() below may ALSO fire the silent on-open pass (if this
    // project has never had Mid-360 settings saved) racing this hook's own
    // explicit trigger a few lines down — CaptureWindow's own
    // discovery_in_flight_ guard makes whichever one loses the race a
    // harmless no-op. So this hook does not just fire once: it fires the
    // explicit button click again after the FIRST pass (silent or not)
    // completes, guaranteeing at least one manual (dialog-shown, always-
    // overwrite) pass is exercised regardless of how the race went.
    const QString shotPath = parser.value(optAutoDetectShot);
    auto* firedManualFollowup = new bool(false);
    QObject::connect(cap, &lidarscan::CaptureWindow::autoDetectFinished, &app,
                     [cap, shotPath, firedManualFollowup](bool mid360Found, bool d6Found,
                                                          bool um982Found) {
                       std::fprintf(stderr,
                                    "[lidarscan] auto-detect-selftest: Mid-360 %s, D6 %s, "
                                    "UM982 %s\n",
                                    mid360Found ? "FOUND" : "not seen",
                                    d6Found ? "FOUND" : "not seen",
                                    um982Found ? "FOUND" : "not seen");
                       if (!*firedManualFollowup) {
                         *firedManualFollowup = true;
                         QTimer::singleShot(200, cap, [cap] { cap->triggerAutoDetectForCli(); });
                       }
                       if (shotPath.isEmpty()) return;
                       QDir().mkpath(QFileInfo(shotPath).absolutePath());
                       const bool ok = cap->grab().save(shotPath);
                       std::fprintf(stderr, "[lidarscan] auto-detect-shot: %s -> %s\n",
                                    ok ? "OK" : "FAILED", shotPath.toUtf8().constData());
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
    QTimer::singleShot(500, cap, [cap, hostIp, lidarIp] {
      cap->runMid360SelfTestForCli(hostIp, lidarIp);
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
