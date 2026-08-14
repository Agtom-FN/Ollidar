// LidarScan spike S3 -- desktop rendering architecture.
//
// Proves: Google Filament (Metal backend) rendering a paged, live-streamed
// point cloud inside a Qt 6 window on Apple silicon, via the native window
// handle (QWindow::winId() -> NSView* -> Engine::createSwapChain()).
//
// Run interactively:   ./s3_render
// Run the benchmark:   ./s3_render --bench --vsync=off --results=out.md

#include "FilamentWindow.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QScreen>
#include <QStatusBar>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void writeResults(const QString& path, const std::vector<PhaseResult>& rs,
                         const std::vector<std::string>& log, const SpikeOptions& o,
                         const QString& env) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream s(&f);
    s << "### Run: vsync=" << (o.vsync ? "on" : "off")
      << ", nativeHandle=" << (o.handle == NativeHandle::QtMetalLayer ? "Qt QMetalLayer"
                              : o.handle == NativeHandle::OwnMetalLayer ? "app CAMetalLayer" : "NSView")
      << ", postProcessing=" << (o.postProcessing ? "on" : "off") << "\n\n";
    s << env << "\n\n";
    s << "| Phase | Points | Frames | Secs | FPS | CPU frame p50 (ms) | CPU frame p95 (ms) | GPU "
         "frame p50 (ms) | GPU frame p95 (ms) | Frame interval p95 (ms) | Notes |\n";
    s << "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n";
    for (const auto& r : rs) {
        s << "| " << QString::fromStdString(r.name) << " | " << r.points << " | " << r.frames
          << " | " << QString::number(r.seconds, 'f', 2) << " | "
          << QString::number(r.fps, 'f', 1) << " | " << QString::number(r.cpuFrameP50, 'f', 2)
          << " | " << QString::number(r.cpuFrameP95, 'f', 2) << " | "
          << QString::number(r.gpuP50, 'f', 2) << " | " << QString::number(r.gpuP95, 'f', 2)
          << " | " << QString::number(r.frameIntervalP95, 'f', 2) << " | "
          << QString::fromStdString(r.note) << " |\n";
    }
    s << "\n<details><summary>run log</summary>\n\n```\n";
    for (const auto& l : log) s << QString::fromStdString(l) << "\n";
    s << "```\n</details>\n";
}

int main(int argc, char** argv) {
    SpikeOptions opts;
    QString resultPath;
    int winW = 1280, winH = 800;
    bool maximized = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](const char* key) -> const char* {
            const size_t n = std::strlen(key);
            return (a.rfind(key, 0) == 0) ? a.c_str() + n : nullptr;
        };
        if (a == "--bench") opts.bench = true;
        else if (a == "--vsync=off") opts.vsync = false;
        else if (a == "--vsync=on") opts.vsync = true;
        else if (a == "--handle=qtlayer") opts.handle = NativeHandle::QtMetalLayer;
        else if (a == "--handle=ownlayer") opts.handle = NativeHandle::OwnMetalLayer;
        else if (a == "--handle=nsview") opts.handle = NativeHandle::NSView;
        else if (a == "--no-postfx") opts.postProcessing = false;
        else if (a == "--maximized") maximized = true;
        else if (const char* v = val("--size=")) sscanf(v, "%dx%d", &winW, &winH);
        else if (a == "--no-resize-stress") opts.stressResize = false;
        else if (const char* v = val("--points=")) opts.preloadPoints = strtoull(v, nullptr, 10);
        else if (const char* v = val("--psize=")) opts.pointSize = strtof(v, nullptr);
        else if (const char* v = val("--phase=")) opts.phaseSeconds = strtod(v, nullptr);
        else if (const char* v = val("--stream-secs=")) opts.streamSeconds = strtod(v, nullptr);
        else if (const char* v = val("--rate=")) opts.streamRate = strtod(v, nullptr);
        else if (const char* v = val("--shots=")) opts.shotDir = v;
        else if (const char* v = val("--results=")) resultPath = QString::fromUtf8(v);
        else if (a == "--help") {
            std::printf("s3_render [--bench] [--vsync=on|off] [--handle=qtlayer|ownlayer|nsview]\n"
                        "          [--no-postfx] [--maximized] [--size=WxH]\n"
                        "          [--points=N] [--psize=F] [--phase=S] [--stream-secs=S]\n"
                        "          [--rate=PPS] [--shots=DIR] [--results=FILE]\n"
                        "keys: space=toggle orbit  s=toggle 200k/s stream  1/2/3=2M/5M/10M  c=clear\n");
            return 0;
        }
    }

    QApplication app(argc, argv);

    auto* win = new QMainWindow();
    win->setWindowTitle("LidarScan S3 - Filament point cloud embedded in Qt 6 (Metal)");

    auto* fw = new FilamentWindow(opts);
    fw->setTopLevel(win);
    QWidget* container = QWidget::createWindowContainer(fw, win);
    container->setMinimumSize(320, 240);
    container->setFocusPolicy(Qt::StrongFocus);
    win->setCentralWidget(container);

    auto* status = new QLabel("starting...");
    win->statusBar()->addWidget(status);
    QObject::connect(fw, &FilamentWindow::statusChanged, status, &QLabel::setText);

    win->resize(winW, winH);
    if (maximized) win->showMaximized(); else win->show();

    QScreen* sc = app.primaryScreen();
    const QString env =
            QString("Host: %1 / Qt %2 / screen %3x%4 @ %5 Hz, Qt devicePixelRatio %6")
                    .arg(QSysInfo::prettyProductName())
                    .arg(qVersion())
                    .arg(sc->geometry().width())
                    .arg(sc->geometry().height())
                    .arg(sc->refreshRate())
                    .arg(sc->devicePixelRatio());
    std::fprintf(stderr, "[s3] %s\n", env.toUtf8().constData());

    if (opts.bench) {
        QObject::connect(fw, &FilamentWindow::benchFinished, &app, [&] {
            if (!resultPath.isEmpty()) {
                writeResults(resultPath, fw->results(), fw->log(), opts, env);
                std::fprintf(stderr, "[s3] results written to %s\n",
                             resultPath.toUtf8().constData());
            }
            QTimer::singleShot(200, &app, &QApplication::quit);
        });
        // Hard watchdog so a hang can never wedge the run.
        QTimer::singleShot(240000, &app, [&] {
            std::fprintf(stderr, "[s3] WATCHDOG: bench did not finish in 240 s\n");
            if (!resultPath.isEmpty()) writeResults(resultPath, fw->results(), fw->log(), opts, env);
            app.quit();
        });
    }

    return app.exec();
}
