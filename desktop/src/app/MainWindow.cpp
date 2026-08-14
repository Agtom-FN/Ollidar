#include "app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/CaptureWindow.h"
#include "app/DisplayParamsDock.h"
#include "app/EngineHost.h"
#include "app/ExportDialog.h"
#include "app/MeasureDock.h"
#include "app/ReplayController.h"
#include "render/ViewportWindow.h"

namespace lidarscan {
namespace {

QString humanBytes(quint64 b) {
  static const char* u[] = {"B", "KB", "MB", "GB"};
  double v = double(b);
  int i = 0;
  while (v >= 1024.0 && i < 3) {
    v /= 1024.0;
    ++i;
  }
  return QString("%1 %2").arg(v, 0, 'f', i ? 1 : 0).arg(u[i]);
}

}  // namespace

MainWindow::MainWindow(EngineHost* host, QWidget* parent) : QMainWindow(parent), host_(host) {
  setWindowTitle("LidarScan — Desktop");
  params_ = std::make_unique<scanengine::DisplayParamsController>(
      scanengine::profile_defaults(scanengine::DisplayProfile::kQuickScan));
  replay_ = new ReplayController(host_, this);
  buildUi();
  buildMenus();
  refreshRecents();

  connect(host_, &EngineHost::logLine, this, [this](const QString& l) {
    log_->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz ") + l);
  });
  connect(host_, &EngineHost::tick, this, &MainWindow::updateStatus);
  connect(replay_, &ReplayController::started, this, [this](const QString& d, double s) {
    log_->appendPlainText(QString("replay started: %1 at %2x").arg(d).arg(s <= 0 ? "max" : QString::number(s)));
    replay_button_->setEnabled(false);
    replay_stop_->setEnabled(true);
  });
  connect(replay_, &ReplayController::finished, this, [this](const QString& s) {
    log_->appendPlainText("replay finished: " + s);
    replay_button_->setEnabled(project_.has_d6_raw);
    replay_stop_->setEnabled(false);
  });
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
  // --- viewport (native child window embedded in the widget tree) ---
  viewport_ = new ViewportWindow();
  viewport_->setTopLevel(this);
  viewport_->setDisplayParams(params_->get());
  QWidget* container = QWidget::createWindowContainer(viewport_, this);
  container->setMinimumSize(480, 320);
  container->setFocusPolicy(Qt::StrongFocus);
  setCentralWidget(container);

  connect(viewport_, &ViewportWindow::statusChanged, this,
          [this](const QString& s) { status_render_->setText(s); });
  connect(viewport_, &ViewportWindow::initFailed, this, [this](const QString& why) {
    log_->appendPlainText("VIEWPORT INIT FAILED: " + why);
    status_render_->setText("renderer unavailable: " + why);
  });

  // --- left dock: projects + replay + log ---
  {
    auto* dock = new QDockWidget("Projects", this);
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);

    auto* libBox = new QGroupBox("Library");
    auto* lv = new QVBoxLayout(libBox);
    recents_ = new QListWidget();
    recents_->setToolTip("Recently opened .lscan projects");
    connect(recents_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
      QString err;
      if (!openProject(it->data(Qt::UserRole).toString(), &err)) {
        QMessageBox::warning(this, "Open project", err);
      }
    });
    lv->addWidget(recents_);
    auto* brow = new QWidget();
    auto* bl = new QHBoxLayout(brow);
    bl->setContentsMargins(0, 0, 0, 0);
    auto* openBtn = new QPushButton("Open…");
    auto* newBtn = new QPushButton("New…");
    auto* importBtn = new QPushButton("Import raw D6…");
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::onOpenProject);
    connect(newBtn, &QPushButton::clicked, this, &MainWindow::onNewProject);
    connect(importBtn, &QPushButton::clicked, this, &MainWindow::onImportRaw);
    bl->addWidget(openBtn);
    bl->addWidget(newBtn);
    bl->addWidget(importBtn);
    lv->addWidget(brow);
    v->addWidget(libBox);

    auto* infoBox = new QGroupBox("Project");
    auto* iv = new QVBoxLayout(infoBox);
    project_label_ = new QLabel("no project open");
    project_label_->setWordWrap(true);
    project_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    iv->addWidget(project_label_);
    streams_ = new QTreeWidget();
    streams_->setColumnCount(4);
    streams_->setHeaderLabels({"Stream", "Chunks", "Payload", "Span"});
    streams_->setRootIsDecorated(false);
    streams_->setMinimumHeight(110);
    iv->addWidget(streams_);
    warnings_label_ = new QLabel();
    warnings_label_->setWordWrap(true);
    iv->addWidget(warnings_label_);
    v->addWidget(infoBox);

    auto* replayBox = new QGroupBox("Replay (D6 raw → engine → viewport)");
    auto* rv = new QVBoxLayout(replayBox);
    auto* speedRow = new QWidget();
    auto* sl = new QHBoxLayout(speedRow);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(new QLabel("Speed"));
    replay_speed_ = new QDoubleSpinBox();
    replay_speed_->setRange(0.0, 100.0);
    replay_speed_->setSingleStep(0.5);
    replay_speed_->setValue(1.0);
    replay_speed_->setSpecialValueText("max");
    replay_speed_->setToolTip(
        "1.0 replays with the capture's own pacing; 0 (\"max\") pushes chunks as "
        "fast as the engine decodes them.");
    sl->addWidget(replay_speed_, 1);
    rv->addWidget(speedRow);
    auto* rrow = new QWidget();
    auto* rl = new QHBoxLayout(rrow);
    rl->setContentsMargins(0, 0, 0, 0);
    replay_button_ = new QPushButton("Replay");
    replay_stop_ = new QPushButton("Stop");
    replay_button_->setEnabled(false);
    replay_stop_->setEnabled(false);
    connect(replay_button_, &QPushButton::clicked, this, [this] {
      QString err;
      if (!startReplay(replay_speed_->value(), &err)) {
        QMessageBox::warning(this, "Replay", err);
      }
    });
    connect(replay_stop_, &QPushButton::clicked, this, [this] { replay_->stop(); });
    rl->addWidget(replay_button_);
    rl->addWidget(replay_stop_);
    rv->addWidget(rrow);
    v->addWidget(replayBox);

    log_ = new QPlainTextEdit();
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setMinimumHeight(120);
    v->addWidget(log_, 1);

    dock->setWidget(w);
    dock->setMinimumWidth(360);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
  }

  // --- right dock: display parameters (A14) ---
  params_dock_ = new DisplayParamsDock(params_.get(), this);
  connect(params_dock_, &DisplayParamsDock::changed, this, [this] {
    viewport_->setDisplayParams(params_->get());
  });
  addDockWidget(Qt::RightDockWidgetArea, params_dock_);

  // --- right dock (tabbed): measure tool (C3) ---
  measure_dock_ = new MeasureDock(viewport_, this);
  addDockWidget(Qt::RightDockWidgetArea, measure_dock_);
  tabifyDockWidget(params_dock_, measure_dock_);
  params_dock_->raise();

  // --- status bar ---
  status_engine_ = new QLabel("engine: starting");
  status_render_ = new QLabel("renderer: starting");
  statusBar()->addWidget(status_engine_, 1);
  statusBar()->addPermanentWidget(status_render_, 2);

  resize(1580, 940);
}

void MainWindow::buildMenus() {
  auto* file = menuBar()->addMenu("&File");
  file->addAction("&New project…", this, &MainWindow::onNewProject);
  file->addAction("&Open project…", QKeySequence::Open, this, &MainWindow::onOpenProject);
  file->addAction("&Import raw D6 capture…", this, &MainWindow::onImportRaw);
  file->addSeparator();
  file->addAction("&Close project", this, &MainWindow::closeProject);
  file->addSeparator();
  file->addAction("Save display parameters to project", this, [this] {
    if (project_.dir.isEmpty()) return;
    QString err;
    if (params_dock_->saveToProject(project_.dir, &err)) {
      log_->appendPlainText("display parameters saved to " +
                            DisplayParamsDock::paramsPathFor(project_.dir));
    } else {
      QMessageBox::warning(this, "Save", err);
    }
  });
  file->addSeparator();
  file->addAction("&Export…", QKeySequence("Ctrl+E"), this, &MainWindow::onExport);
  file->addSeparator();
  file->addAction("E&xit", QKeySequence::Quit, qApp, &QApplication::quit);

  auto* capture = menuBar()->addMenu("&Capture");
  capture->addAction("Open capture window…", this, [this] {
    captureWindow()->setProjectDir(project_.dir);
    captureWindow()->show();
    captureWindow()->raise();
  });

  auto* view = menuBar()->addMenu("&View");
  view->addAction("&Fit to cloud", QKeySequence("F"), this, [this] { viewport_->fitView(); });
  auto* orbit = view->addAction("&Auto-orbit");
  orbit->setCheckable(true);
  connect(orbit, &QAction::toggled, this, [this](bool on) { viewport_->setAutoOrbit(on); });
  auto* vsync = view->addAction("&Vsync");
  vsync->setCheckable(true);
  vsync->setChecked(true);
  connect(vsync, &QAction::toggled, this, [this](bool on) { viewport_->setVsync(on); });
  view->addSeparator();
  view->addAction("&Screenshot…", this, &MainWindow::onScreenshot);
  view->addSeparator();
  view->addAction(params_dock_->toggleViewAction());
  view->addAction(measure_dock_->toggleViewAction());

  auto* help = menuBar()->addMenu("&Help");
  help->addAction("About", this, [this] {
    QMessageBox::information(
        this, "LidarScan Desktop",
        QString("%1\n\nRenderer: %2\nRender clock: %3\n\nQt %4")
            .arg(host_->versionString(), viewport_->surfaceDescription(),
                 viewport_->displayLinkName(), qVersion()));
  });
}

CaptureWindow* MainWindow::captureWindow() {
  if (!capture_) {
    capture_ = new CaptureWindow(host_, this);
    connect(capture_, &CaptureWindow::captureStarted, this, [this](const QString& d) {
      log_->appendPlainText("capture started into " + d);
      viewport_->setPointStore(host_->points());
    });
    connect(capture_, &CaptureWindow::captureStopped, this,
            [this] { log_->appendPlainText("capture stopped"); });
  }
  return capture_;
}

bool MainWindow::openProject(const QString& dir, QString* err) {
  if (dir.isEmpty()) return false;
  // Persist whatever the previous project's display parameters ended up as
  // before switching the model out from under it (§3.9 "settings persist per
  // project").
  persistDisplayParamsIfProjectOpen();

  const ProjectInfo info = readProject(dir);
  if (!info.valid) {
    if (err) *err = QString("%1: %2").arg(dir, info.error);
    return false;
  }
  project_ = info;
  addRecent(info.dir);
  refreshProjectPanel();
  if (!params_dock_->loadFromProject(info.dir)) {
    // No saved processed/display_params.json for this project yet — fall
    // back to A14's profile_defaults() for whatever workflow profile the
    // manifest declares (createProject()/FileRecordWriter::set_profile()),
    // rather than leaving whatever the PREVIOUS project's parameters were on
    // screen. Tech Spec §3.9: "profiles set defaults".
    scanengine::DisplayProfile prof = scanengine::DisplayProfile::kQuickScan;
    for (int i = 0; i < scanengine::kDisplayProfileCount; ++i) {
      const auto p = static_cast<scanengine::DisplayProfile>(i);
      if (info.profile.compare(scanengine::to_string(p), Qt::CaseInsensitive) == 0) {
        prof = p;
        break;
      }
    }
    params_->set(scanengine::profile_defaults(prof));
    params_dock_->refreshFromModel();
  }
  viewport_->setDisplayParams(params_->get());
  setWindowTitle(QString("LidarScan — %1").arg(info.name));
  log_->appendPlainText(QString("opened %1 (%2 chunks, %3, %4 s)")
                            .arg(info.dir)
                            .arg(info.total_chunks)
                            .arg(humanBytes(info.total_bytes))
                            .arg(info.duration_s, 0, 'f', 2));
  return true;
}

void MainWindow::closeProject() {
  replay_->stop();
  persistDisplayParamsIfProjectOpen();
  project_ = ProjectInfo{};
  refreshProjectPanel();
  setWindowTitle("LidarScan — Desktop");
}

void MainWindow::persistDisplayParamsIfProjectOpen() {
  if (!project_.valid) return;
  QString err;
  if (!params_dock_->saveToProject(project_.dir, &err)) {
    log_->appendPlainText("could not auto-persist display parameters: " + err);
  }
}

void MainWindow::closeEvent(QCloseEvent* event) {
  persistDisplayParamsIfProjectOpen();
  QMainWindow::closeEvent(event);
}

bool MainWindow::startReplay(double speed, QString* err) {
  if (!project_.valid) {
    if (err) *err = "no project open";
    return false;
  }
  if (!project_.has_d6_raw) {
    if (err) {
      *err =
          "this project has no D6 raw chunks. record/replay.h only forwards "
          "ChunkType::kD6Raw today — Mid-360 and GNSS raw streams need an "
          "analogous Engine push entry point first (A3/A10).";
    }
    return false;
  }
  // The viewport mirrors the live PageStore while the replay feeds it.
  viewport_->setPointStore(host_->points());
  if (!replay_->start(project_.dir, speed, err)) return false;
  return true;
}

bool MainWindow::loadDisplayParamsFile(const QString& path, QString* err) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (err) *err = QString("cannot read %1").arg(path);
    return false;
  }
  const QByteArray blob = f.readAll();
  scanengine::DisplayParams p{};
  const auto st = scanengine::from_json(blob.toStdString(), &p);
  if (!st.ok()) {
    if (err) *err = QString("%1: %2").arg(path, scanengine::error_str(st.error()));
    return false;
  }
  params_->set(p);
  params_dock_->refreshFromModel();
  viewport_->setDisplayParams(params_->get());
  log_->appendPlainText("display parameters loaded from " + path);
  return true;
}

void MainWindow::applyDisplayProfile(scanengine::DisplayProfile profile) {
  params_->set(scanengine::profile_defaults(profile));
  params_dock_->refreshFromModel();
  viewport_->setDisplayParams(params_->get());
}

void MainWindow::refreshProjectPanel() {
  streams_->clear();
  if (!project_.valid) {
    project_label_->setText("no project open");
    warnings_label_->clear();
    replay_button_->setEnabled(false);
    return;
  }
  project_label_->setText(
      QString("<b>%1</b><br>%2<br>manifest: %3%4 · profile: %5<br>%6 chunks · %7 · %8 s")
          .arg(project_.name.toHtmlEscaped(), project_.dir.toHtmlEscaped(),
               project_.manifest_present ? (project_.manifest_ok ? "ok" : "corrupt") : "missing",
               project_.sealed ? "" : ", <b>NOT SEALED</b> (session ended abnormally)",
               project_.profile.isEmpty() ? "-" : project_.profile)
          .arg(project_.total_chunks)
          .arg(humanBytes(project_.total_bytes))
          .arg(project_.duration_s, 0, 'f', 2));

  for (const auto& s : project_.streams) {
    auto* it = new QTreeWidgetItem(streams_);
    it->setText(0, s.name);
    it->setText(1, QString::number(s.chunks));
    it->setText(2, humanBytes(s.bytes));
    it->setText(3, QString("%1 s").arg(s.duration_s(), 0, 'f', 2));
  }
  for (int c = 0; c < 4; ++c) streams_->resizeColumnToContents(c);

  QStringList warn;
  if (project_.truncated_tail_chunks) {
    warn << QString("%1 truncated-tail chunks (a killed capture — expected, not an error)")
                .arg(project_.truncated_tail_chunks);
  }
  if (project_.crc_mismatch_chunks) {
    warn << QString("%1 CRC mismatches").arg(project_.crc_mismatch_chunks);
  }
  if (project_.unreadable_streams) {
    warn << QString("%1 unreadable stream files").arg(project_.unreadable_streams);
  }
  warnings_label_->setText(warn.isEmpty() ? "no reader warnings" : warn.join("\n"));

  replay_button_->setEnabled(project_.has_d6_raw && !replay_->running());
}

void MainWindow::refreshRecents() {
  QSettings s;
  recent_dirs_ = s.value("recentProjects").toStringList();
  recents_->clear();
  for (const QString& d : recent_dirs_) {
    auto* it = new QListWidgetItem(QFileInfo(d).fileName() + "   —   " + d);
    it->setData(Qt::UserRole, d);
    recents_->addItem(it);
  }
}

void MainWindow::addRecent(const QString& dir) {
  recent_dirs_.removeAll(dir);
  recent_dirs_.prepend(dir);
  while (recent_dirs_.size() > 15) recent_dirs_.removeLast();
  QSettings().setValue("recentProjects", recent_dirs_);
  refreshRecents();
}

void MainWindow::updateStatus() {
  status_engine_->setText(host_->healthLine());
}

void MainWindow::onNewProject() {
  QString dir = QFileDialog::getSaveFileName(this, "New .lscan project", QDir::homePath(),
                                             "LidarScan project (*.lscan)");
  if (dir.isEmpty()) return;
  if (!dir.endsWith(".lscan")) dir += ".lscan";
  QString err;
  if (!createProject(dir, "quickscan", &err)) {
    QMessageBox::warning(this, "New project", err);
    return;
  }
  if (!openProject(dir, &err)) QMessageBox::warning(this, "New project", err);
}

void MainWindow::onOpenProject() {
  const QString dir =
      QFileDialog::getExistingDirectory(this, "Open .lscan project", QDir::homePath());
  if (dir.isEmpty()) return;
  QString err;
  if (!openProject(dir, &err)) QMessageBox::warning(this, "Open project", err);
}

void MainWindow::onImportRaw() {
  const QString raw = QFileDialog::getOpenFileName(
      this, "Raw COIN-D6 capture (engine_cli --synth output, or a logged port)", QDir::homePath());
  if (raw.isEmpty()) return;
  QString dir = QFileDialog::getSaveFileName(this, "Create .lscan project", QDir::homePath(),
                                             "LidarScan project (*.lscan)");
  if (dir.isEmpty()) return;
  if (!dir.endsWith(".lscan")) dir += ".lscan";
  QString err;
  quint64 bytes = 0, points = 0;
  if (!importRawD6(raw, dir, "quickscan", &err, &bytes, &points)) {
    QMessageBox::warning(this, "Import", err);
    return;
  }
  log_->appendPlainText(
      QString("imported %1 (%2 bytes -> %3 points) into %4").arg(raw).arg(bytes).arg(points).arg(dir));
  if (!openProject(dir, &err)) QMessageBox::warning(this, "Import", err);
}

void MainWindow::onScreenshot() {
  const QString path = QFileDialog::getSaveFileName(this, "Save screenshot", QDir::homePath(),
                                                    "PNG image (*.png)");
  if (path.isEmpty()) return;
  if (!viewport_->captureScreenshot(path)) {
    QMessageBox::warning(this, "Screenshot", "could not acquire a drawable");
  } else {
    log_->appendPlainText("screenshot written: " + path);
  }
}

void MainWindow::onExport() {
  // A fresh dialog every time, not a cached singleton: the dialog snapshots
  // the current DisplayParams (for "bounds from current clipping") and the
  // current project directory at construction, and NOTES.md is explicit that
  // is deliberately not a live binding — reusing one instance across opens
  // would silently export against a stale clip/project from the first time
  // this menu item was used.
  if (export_dialog_) export_dialog_->deleteLater();
  export_dialog_ = new ExportDialog(host_->points(), params_->get(),
                                    project_.valid ? project_.dir : QDir::homePath(), this);
  export_dialog_->setAttribute(Qt::WA_DeleteOnClose);
  connect(export_dialog_, &QObject::destroyed, this, [this] { export_dialog_ = nullptr; });
  export_dialog_->show();
  export_dialog_->raise();
  export_dialog_->activateWindow();
}

}  // namespace lidarscan
