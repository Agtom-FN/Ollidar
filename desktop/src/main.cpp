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
#include <QFileInfo>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/CaptureWindow.h"
#include "app/EngineHost.h"
#include "app/MainWindow.h"
#include "app/Project.h"  // ProjectInfo/readProject for --mid360-selftest's post-record report
#include "render/ViewportWindow.h"
#include "scanengine/export/exporter.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("LidarScan");
  QCoreApplication::setApplicationName("LidarScan Desktop");
  QCoreApplication::setApplicationVersion("0.1.0");

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
  QCommandLineOption optMid360RecordInto(
      "mid360-record-into",
      "With --mid360-selftest: on a PASSED self-test, also Record into this NEW "
      ".lscan directory for 3 s, Stop, and print the resulting project's chunk/byte "
      "counts (a fresh directory — unlike --project this does not open an existing one)",
      "dir");
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
  parser.addOption(optMid360RecordInto);
  parser.process(app);

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
