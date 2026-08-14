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
//
// Owner: C1.
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>

#include <cmath>
#include <cstdio>

#include "app/EngineHost.h"
#include "app/MainWindow.h"
#include "app/Project.h"
#include "render/ViewportWindow.h"

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
  parser.addOption(optProject);
  parser.addOption(optImport);
  parser.addOption(optReplay);
  parser.addOption(optShot);
  parser.addOption(optShotDelay);
  parser.addOption(optQuit);
  parser.addOption(optVsync);
  parser.addOption(optSize);
  parser.addOption(optDisplayParams);
  parser.addOption(optDisplayProfile);
  parser.addOption(optResizeStorm);
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
