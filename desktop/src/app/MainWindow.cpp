#include "app/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "app/CaptureWindow.h"
#include "app/DisplayParamsDock.h"
#include "app/EngineHost.h"
#include "app/ExportDialog.h"
#include "app/MeasureDock.h"
#include "app/MergeDock.h"
#include "app/PlanDock.h"
#include "app/ProcessingDock.h"
#include "app/ReplayController.h"
#include "app/SyntheticBuilding.h"
#include "app/TransferDialog.h"
#include "render/ViewportWindow.h"
#include "ui/IconRail.h"
#include "ui/InspectorCard.h"
#include "ui/Theme.h"
#include "ui/ViewportHost.h"
#include "ui/Widgets.h"

namespace lidarscan {
namespace {

// Space-grouped thousands — the mockup's fmt(). Every point count in the
// redesign reads this way, so it lives here rather than at four call sites.
QString groupedCount(quint64 n) {
  QString s = QString::number(n);
  for (int i = s.size() - 3; i > 0; i -= 3) s.insert(i, QChar(0x2009));  // thin space
  return s;
}

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

  // C7: drag-drop of a .lscan.zip transfer bundle anywhere on the main
  // window, plus the macOS file-association/"Open With" path, which Qt
  // delivers as QEvent::FileOpen to the APPLICATION object, not any widget —
  // an application-level event filter is the documented way to catch it.
  // Harmless to install unconditionally: FileOpen never fires on Windows/
  // Linux without an explicit sender, so this is a no-op there.
  setAcceptDrops(true);
  qApp->installEventFilter(this);

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

MainWindow::~MainWindow() {
  // CaptureWindow is created LAZILY (captureWindow(), first used from the
  // Capture menu or the --mid360-selftest CLI hook), so it always becomes a
  // LATER QObject child of `this` than the docks buildUi() creates up
  // front — including the Projects dock that owns `log_`. QObject destroys
  // its children list front-to-back (construction order, not reverse), so
  // on exit the Projects dock (and log_ with it) would be torn down BEFORE
  // `capture_` otherwise. CaptureWindow's own destructor calls onStop() ->
  // EngineHost::stopSession(), which emits logLine() -> this window's own
  // lambda (see the constructor), which dereferences log_ — by then
  // dangling. A real, reproducible SIGSEGV on exit whenever captureWindow()
  // had ever been called; found (crash log symbolicated back to this path)
  // while verifying C4/C5, pre-existing in C1/C2's code, not new here.
  // Deleting it explicitly, first, sidesteps the ordering question entirely
  // rather than relying on child-list order.
  delete capture_;
  qApp->removeEventFilter(this);
}

void MainWindow::buildUi() {
  // --- viewport (native child window embedded in the widget tree) ---
  viewport_ = new ViewportWindow();
  viewport_->setTopLevel(this);
  viewport_->setDisplayParams(params_->get());
  QWidget* container = QWidget::createWindowContainer(viewport_, this);
  container->setMinimumSize(480, 320);
  container->setFocusPolicy(Qt::StrongFocus);

  // The viewport now lives inside a host that can float chrome over it — the
  // redesign's project chip, render-stats chip, RECORDING badge and the
  // display inspector. ViewportHost.h documents why that needs native
  // sibling surfaces rather than ordinary child widgets.
  viewport_host_ = new ViewportHost(this);
  viewport_host_->setViewportWidget(container);
  setCentralWidget(viewport_host_);

  connect(viewport_, &ViewportWindow::statusChanged, this, [this](const QString& s) {
    // The verbose renderer line stays available as the tooltip and on the
    // viewport's bottom-left chip; the status bar's right-hand segment is the
    // mockup's compact "<backend> · dpr <n> · engine <v> · ABI v<n>".
    const auto& st = viewport_->stats();
    status_render_->setText(QString("%1 · dpr %2 · %3 · ABI v%4")
                                .arg(viewport_->surfaceDescription().section(" · ", 0, 0))
                                .arg(st.dpr, 0, 'f', 2)
                                .arg(host_->versionString().section(" (", 0, 0))
                                // The C++ constant, NOT capi/scanengine_c.h's
                                // SCAN_ABI_VERSION: NOTES.md §1.2 keeps the C
                                // ABI header out of the desktop entirely.
                                .arg(scanengine::kEngineAbiVersion));
    status_render_->setToolTip(s);
    updateViewportChips();
  });
  connect(viewport_, &ViewportWindow::initFailed, this, [this](const QString& why) {
    log_->appendPlainText("VIEWPORT INIT FAILED: " + why);
    status_render_->setText("renderer unavailable: " + why);
  });

  buildRail();

  // --- left dock: projects + replay + log ---
  {
    auto* dock = new QDockWidget("PROJECTS", this);
    projects_dock_ = dock;
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);

    auto* libBox = new QGroupBox("Library");
    auto* lv = new QVBoxLayout(libBox);
    recents_ = new QListWidget();
    recents_->setToolTip("Recently opened .lscan projects");
    recents_->setSpacing(2);
    // Single click opens, matching the mockup's `.lrow`; double click still
    // works because it fires a click first.
    connect(recents_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
      const QString dir = it->data(Qt::UserRole).toString();
      if (dir == project_.dir) return;
      QString err;
      if (!openProject(dir, &err)) {
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
    // Beside the rail, not above/below it: two docks added to the same area
    // stack vertically by default, which would put the 56 px rail on top of
    // a 360 px panel instead of alongside it.
    splitDockWidget(rail_dock_, dock, Qt::Horizontal);
  }

  // --- right docks -------------------------------------------------------
  //
  // These used to be five tabs in one tabified group — the "wide navigation"
  // the redesign replaces. They are now five INDEPENDENT docks in the right
  // area, of which showWorkspace() shows exactly one; with a single visible
  // dock per area Qt draws no tab bar at all, so the rail is the only
  // navigation on screen. Every dock keeps every capability it had (float,
  // resize, close, View-menu toggle).
  params_dock_ = new DisplayParamsDock(params_.get(), this);
  connect(params_dock_, &DisplayParamsDock::changed, this, [this] {
    viewport_->setDisplayParams(params_->get());
    if (inspector_) inspector_->refreshFromModel();
  });
  addDockWidget(Qt::RightDockWidgetArea, params_dock_);

  measure_dock_ = new MeasureDock(viewport_, this);
  addDockWidget(Qt::RightDockWidgetArea, measure_dock_);

  processing_dock_ = new ProcessingDock(host_, this);
  addDockWidget(Qt::RightDockWidgetArea, processing_dock_);
  connect(processing_dock_, &ProcessingDock::logLine, this,
          [this](const QString& l) { log_->appendPlainText(l); });
  connect(processing_dock_, &ProcessingDock::loadResultRequested, this,
          [this](std::shared_ptr<scanengine::PageStore> store, quint64 jobId) {
            loaded_result_store_ = store;
            viewport_->setPointStore(loaded_result_store_.get());
            viewport_->fitView();
            plan_dock_->setPointStore(loaded_result_store_.get());
            log_->appendPlainText(
                QString("job #%1's result is now the viewport/plan cloud (%2 points)")
                    .arg(jobId)
                    .arg(loaded_result_store_->total_points()));
          });

  // --- floor plan workspace (C5) ---
  plan_dock_ = new PlanDock(this);
  addDockWidget(Qt::RightDockWidgetArea, plan_dock_);
  plan_dock_->setPointStore(host_->points());
  connect(plan_dock_, &PlanDock::logLine, this,
          [this](const QString& l) { log_->appendPlainText(l); });

  // --- merge workbench (C6) ---
  merge_dock_ = new MergeDock(viewport_, this);
  addDockWidget(Qt::RightDockWidgetArea, merge_dock_);
  connect(merge_dock_, &MergeDock::logLine, this,
          [this](const QString& l) { log_->appendPlainText(l); });
  connect(merge_dock_, &MergeDock::exportMergedRequested, this, &MainWindow::onExportMerged);

  // --- the floating inspector + the viewport's own chrome -----------------
  inspector_ = new InspectorCard(params_.get());
  connect(inspector_, &InspectorCard::changed, this, [this] {
    viewport_->setDisplayParams(params_->get());
    params_dock_->refreshFromModel();
  });
  connect(inspector_, &InspectorCard::exportRequested, this, &MainWindow::onExport);
  connect(inspector_, &InspectorCard::moreRequested, this, [this] {
    params_dock_->show();
    params_dock_->raise();
  });

  project_chip_ = new Chip();
  project_chip_->setTone(Chip::Tone::kNone);
  project_chip_->setDotVisible(true);
  project_chip_->setText("no project open");

  stats_chip_ = new Chip();
  stats_chip_->setText("renderer starting");

  rec_badge_ = new Chip();
  rec_badge_->setTone(Chip::Tone::kBad);
  rec_badge_->setDotVisible(true);
  rec_badge_->setEmphasis(true);
  rec_badge_->setText("RECORDING");
  rec_badge_->hide();

  viewport_host_->addOverlay(project_chip_, ViewportHost::Anchor::kTopLeft, 14);
  viewport_host_->addOverlay(stats_chip_, ViewportHost::Anchor::kBottomLeft, 14);
  viewport_host_->addOverlay(rec_badge_, ViewportHost::Anchor::kTopRight, 12);
  viewport_host_->addOverlay(inspector_, ViewportHost::Anchor::kTopRight, 14);

  // --- status bar: state · measure · georef σ · engine ---------------------
  status_engine_ = new QLabel("engine ready · idle");
  status_measure_ = new QLabel("MEASURE —");
  status_georef_ = new QLabel("local frame · no CRS");
  status_render_ = new QLabel("renderer: starting");
  status_render_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  statusBar()->addWidget(status_engine_);
  statusBar()->addWidget(status_measure_);
  statusBar()->addWidget(status_georef_);
  statusBar()->addWidget(new QLabel(), 1);  // the mockup's `margin-left:auto`
  statusBar()->addPermanentWidget(status_render_);
  statusBar()->setSizeGripEnabled(false);

  resize(1760, 980);
  showWorkspace(IconRail::Item::kProjects);
}

void MainWindow::buildRail() {
  rail_ = new IconRail(this);
  connect(rail_, &IconRail::activated, this, &MainWindow::onRailActivated);

  // The rail is a dock only so that it can live to the left of the projects
  // panel without a QSplitter fighting the central native window for space.
  // Everything that makes a dock a dock is turned off: no title bar, no
  // float, no close, no move.
  rail_dock_ = new QDockWidget(this);
  rail_dock_->setObjectName("railDock");
  rail_dock_->setTitleBarWidget(new QWidget(rail_dock_));
  rail_dock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
  rail_dock_->setAllowedAreas(Qt::LeftDockWidgetArea);
  rail_dock_->setWidget(rail_);
  rail_dock_->setFixedWidth(theme::kRailWidth);
  addDockWidget(Qt::LeftDockWidgetArea, rail_dock_);
}

// Every rail button, including the two that open a dialog rather than a
// workspace and the one that is a toggle. Nothing here can fail silently: an
// action that cannot proceed says why and leaves the rail on the workspace
// that is actually showing (IconRail.cpp's comment on why the rail does not
// self-light).
void MainWindow::onRailActivated(IconRail::Item item) {
  switch (item) {
    case IconRail::Item::kCapture:
      captureWindow()->setProjectDir(project_.dir);
      captureWindow()->show();
      captureWindow()->raise();
      captureWindow()->activateWindow();
      showWorkspace(IconRail::Item::kCapture);
      return;
    case IconRail::Item::kTransfer:
      // The transfer bundle has no dock — it is two dialogs (C7). Export
      // needs an open project; import does not, so offer import in that case
      // rather than refusing outright.
      if (project_.valid) {
        onExportTransferBundle();
      } else {
        onImportTransferBundle();
      }
      return;
    case IconRail::Item::kExport:
      onExport();
      return;
    case IconRail::Item::kInspector:
      // From anywhere else this takes you to Review first, exactly as the
      // mockup's rail does; on Review it toggles.
      if (workspace_ != IconRail::Item::kReview) {
        inspector_visible_ = true;
        showWorkspace(IconRail::Item::kReview);
      } else {
        inspector_visible_ = !inspector_visible_;
        applyInspectorPlacement();
        rail_->setInspectorOn(inspector_visible_);
      }
      return;
    default:
      showWorkspace(item);
      return;
  }
}

void MainWindow::showWorkspace(IconRail::Item item) {
  workspace_ = item;

  // Review is the only full-bleed workspace: nothing but the viewport, its
  // chips and the floating inspector.
  const bool review = item == IconRail::Item::kReview;
  projects_dock_->setVisible(item == IconRail::Item::kProjects ||
                             item == IconRail::Item::kCapture);

  // Exactly one right-hand dock, so Qt draws no tab bar.
  params_dock_->setVisible(false);
  measure_dock_->setVisible(false);
  processing_dock_->setVisible(item == IconRail::Item::kJobs);
  plan_dock_->setVisible(item == IconRail::Item::kPlan);
  merge_dock_->setVisible(item == IconRail::Item::kMerge);

  // Each workspace gets the width its dock actually needs. Under the old
  // tabbed group all five shared ONE width — whatever the user had dragged
  // the group to — so the merge workbench's seven-column session table and
  // the display dock's narrow form fought each other permanently. Now that a
  // workspace owns the area alone, it can ask for the right size. (These are
  // starting widths, not constraints: the splitter still drags.)
  QDockWidget* shown = nullptr;
  int wantWidth = 0;
  switch (item) {
    case IconRail::Item::kPlan: shown = plan_dock_; wantWidth = 620; break;
    case IconRail::Item::kMerge: shown = merge_dock_; wantWidth = 720; break;
    case IconRail::Item::kJobs: shown = processing_dock_; wantWidth = 560; break;
    default: break;
  }
  if (shown) resizeDocks({shown}, {qMin(wantWidth, width() / 2)}, Qt::Horizontal);

  applyInspectorPlacement();
  rail_->setCurrent(item);
  rail_->setInspectorOn(review && inspector_visible_);
  updateViewportChips();
}

bool MainWindow::showWorkspaceNamed(const QString& name) {
  static const struct {
    const char* n;
    IconRail::Item i;
  } kMap[] = {
      {"projects", IconRail::Item::kProjects}, {"capture", IconRail::Item::kCapture},
      {"review", IconRail::Item::kReview},     {"plan", IconRail::Item::kPlan},
      {"merge", IconRail::Item::kMerge},       {"jobs", IconRail::Item::kJobs},
  };
  for (const auto& m : kMap) {
    if (name.compare(m.n, Qt::CaseInsensitive) == 0) {
      onRailActivated(m.i);
      return true;
    }
  }
  return false;
}

// The 880 px reflow (redesign brief item 3: "Below ~880px width it reflows
// into a normal dock — don't break small windows"). The SAME InspectorCard
// instance moves between the two homes, so no state is duplicated and none is
// lost across a resize.
void MainWindow::applyInspectorPlacement() {
  if (!inspector_ || !viewport_host_) return;
  const bool wanted = workspace_ == IconRail::Item::kReview && inspector_visible_;
  const bool narrow = width() < theme::kInspectorReflowWidth;

  if (wanted && narrow) {
    if (!inspector_dock_) {
      inspector_dock_ = new QDockWidget("DISPLAY INSPECTOR", this);
      inspector_dock_->setObjectName("inspectorDock");
      inspector_dock_->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);
      addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
    }
    if (inspector_->parentWidget() != inspector_dock_) {
      viewport_host_->removeOverlay(inspector_);
      inspector_->setFloatingLook(false);
      inspector_dock_->setWidget(inspector_);
    }
    inspector_dock_->show();
    inspector_->show();
    return;
  }

  if (inspector_dock_ && inspector_->parentWidget() != viewport_host_) {
    inspector_dock_->setWidget(nullptr);
    inspector_dock_->hide();
    inspector_->setFloatingLook(true);
    viewport_host_->addOverlay(inspector_, ViewportHost::Anchor::kTopRight, 14);
  }
  if (inspector_dock_) inspector_dock_->setVisible(false);
  inspector_->setVisible(wanted);
  viewport_host_->layoutOverlays();
}

void MainWindow::updateViewportChips() {
  if (!project_chip_) return;

  const quint64 pts = host_ && host_->points() ? host_->points()->total_points() : 0;
  if (project_.valid) {
    project_chip_->setText(QString("%1 · %2 pts · %3")
                               .arg(project_.name)
                               .arg(groupedCount(pts))
                               .arg(project_.crs.isEmpty() ? QString("local frame")
                                                           : QString(project_.crs).replace(':', ' ')));
    project_chip_->setTone(project_.crs.isEmpty() ? Chip::Tone::kNone : Chip::Tone::kGood);
  } else {
    project_chip_->setText(QString("no project · %1 pts in store").arg(groupedCount(pts)));
    project_chip_->setTone(Chip::Tone::kNone);
  }

  const auto& st = viewport_->stats();
  stats_chip_->setText(QString("%1 pts · %2 fps · cpu p95 %3 ms · gpu p95 %4 ms · %5")
                           .arg(groupedCount(st.cloud.resident_points))
                           .arg(st.fps, 0, 'f', 1)
                           .arg(st.cpu_ms_p95, 0, 'f', 2)
                           .arg(st.gpu_ms_p95, 0, 'f', 2)
                           .arg(viewport_->surfaceDescription()));
  viewport_host_->layoutOverlays();
}

void MainWindow::setCaptureBadge(bool recording, bool paused) {
  if (!rec_badge_) return;
  if (!recording && !paused) {
    rec_badge_->hide();
  } else {
    rec_badge_->setText(paused ? "PAUSED" : "RECORDING");
    rec_badge_->setTone(paused ? Chip::Tone::kWarn : Chip::Tone::kBad);
    rec_badge_->setDotPulsing(recording && !paused);
    rec_badge_->show();
  }
  viewport_host_->layoutOverlays();
}

// The export button's "georef ✓" and the status bar's georef segment are the
// same fact, read from the same place — Engine::crs_epsg(), which the engine
// documents as empty until the transform converges — so they cannot disagree.
void MainWindow::refreshInspectorGeoref() {
  if (!host_ || !host_->ok()) return;
  const std::string epsg = host_->engine()->crs_epsg();
  const auto sol = host_->engine()->georef_solution();
  const bool converged = !epsg.empty();
  const QString epsgQ = QString::fromStdString(epsg);

  if (inspector_) inspector_->setGeorefState(converged, epsgQ, sol.horizontal_sigma_m);

  if (status_georef_) {
    if (converged) {
      status_georef_->setText(QString("georef converged · %1 · σh %2 m")
                                  .arg(QString(epsgQ).replace(':', ' '))
                                  .arg(sol.horizontal_sigma_m, 0, 'f', 3));
      status_georef_->setStyleSheet(QString("color:%1;").arg(theme::css(theme::good())));
    } else if (!project_.crs.isEmpty()) {
      // The open project WAS georeferenced when it was recorded, even though
      // this process's engine has no live fix. Say which of the two it is
      // rather than flattening both to "local frame".
      status_georef_->setText(
          QString("project georef %1 · live σ n/a").arg(QString(project_.crs).replace(':', ' ')));
      status_georef_->setStyleSheet(QString("color:%1;").arg(theme::css(theme::pose())));
    } else {
      status_georef_->setText("local frame · no CRS");
      status_georef_->setStyleSheet(QString("color:%1;").arg(theme::css(theme::faint())));
    }
  }
}

void MainWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  applyInspectorPlacement();
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
  file->addAction("Export transfer &bundle…", this, &MainWindow::onExportTransferBundle);
  file->addAction("&Import transfer bundle (.lscan.zip)…", this, &MainWindow::onImportTransferBundle);
  file->addSeparator();
  file->addAction("E&xit", QKeySequence::Quit, qApp, &QApplication::quit);

  auto* capture = menuBar()->addMenu("&Capture");
  capture->addAction("Open capture window…", this,
                     [this] { onRailActivated(IconRail::Item::kCapture); });

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
  // The rail is the primary navigation now, but every workspace keeps a
  // keyboard/menu route: Cmd-1..6 mirror the rail's six workspace buttons.
  auto* ws = view->addMenu("&Workspace");
  const struct {
    const char* label;
    IconRail::Item item;
    const char* key;
  } kWs[] = {
      {"&Projects", IconRail::Item::kProjects, "Ctrl+1"},
      {"&Capture", IconRail::Item::kCapture, "Ctrl+2"},
      {"&Review", IconRail::Item::kReview, "Ctrl+3"},
      {"&Floor plan", IconRail::Item::kPlan, "Ctrl+4"},
      {"&Merge", IconRail::Item::kMerge, "Ctrl+5"},
      {"Pr&ocessing", IconRail::Item::kJobs, "Ctrl+6"},
  };
  for (const auto& w : kWs) {
    const IconRail::Item it = w.item;
    ws->addAction(w.label, QKeySequence(w.key), this, [this, it] { onRailActivated(it); });
  }
  view->addAction("Display &inspector", QKeySequence("Ctrl+I"), this,
                  [this] { onRailActivated(IconRail::Item::kInspector); });
  view->addSeparator();
  // The docks themselves stay individually toggleable — the rail replaced the
  // tab strip, not the docks' own capabilities.
  view->addAction(params_dock_->toggleViewAction());
  view->addAction(measure_dock_->toggleViewAction());
  view->addAction(processing_dock_->toggleViewAction());
  view->addAction(plan_dock_->toggleViewAction());
  view->addAction(merge_dock_->toggleViewAction());
  view->addAction(projects_dock_->toggleViewAction());

  auto* tools = menuBar()->addMenu("&Tools");
  tools->addAction("Load synthetic building fixture (C5 test fixture)…", this,
                   &MainWindow::loadSyntheticBuildingFixture);

  auto* help = menuBar()->addMenu("&Help");
  help->addAction("About", this, [this] {
    QMessageBox box(this);
    box.setWindowTitle("LidarScan Desktop");
    box.setText(QString("%1\n\nRenderer: %2\nRender clock: %3\n\nQt %4\n\nTypefaces\n  %5")
                    .arg(host_->versionString(), viewport_->surfaceDescription(),
                         viewport_->displayLinkName(), qVersion(),
                         theme::fontReport().join("\n  ")));
    // OFL 1.1 §2 requires the licence to travel with the font, so the bundled
    // texts are readable from inside the app, not only from the repo.
    box.setDetailedText(theme::licenceText("Inter") + "\n\n" +
                        theme::licenceText("Space Grotesk") + "\n\n" +
                        theme::licenceText("JetBrains Mono"));
    box.exec();
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
    // The RECORDING / PAUSED badge rides the MAIN window's viewport (redesign
    // brief item 4) even though capture is its own window, because the main
    // viewport is what is actually showing the cloud being recorded.
    connect(capture_, &CaptureWindow::recordingStateChanged, this,
            &MainWindow::setCaptureBadge);
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
  loaded_result_store_.reset();
  processing_dock_->setProjectDir(info.dir);
  processing_dock_->setCurrentStore(host_->points());
  plan_dock_->setProjectDir(info.dir);
  plan_dock_->setPointStore(host_->points());
  merge_dock_->setOpenProjectDir(info.dir);
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
  processing_dock_->setProjectDir(QString());
  plan_dock_->setProjectDir(QString());
  merge_dock_->setOpenProjectDir(QString());
}

void MainWindow::loadSyntheticBuildingFixture() {
  if (!host_ || !host_->engine()) return;
  loaded_result_store_.reset();
  scanengine::PageStore& store = host_->engine()->points();
  store.clear();
  const auto points = buildSyntheticBuildingPoints();
  scanengine::Span<const scanengine::PointVertex> span(points.data(), points.size());
  quint32 appended = 0;
  const auto st = store.append(scanengine::StreamId::kSlamMap, span, 0, &appended);
  viewport_->setPointStore(host_->points());
  plan_dock_->setPointStore(host_->points());
  viewport_->fitView();
  log_->appendPlainText(QString("synthetic building fixture loaded: %1 points (%2)")
                            .arg(appended)
                            .arg(st.ok() ? "ok" : scanengine::error_str(st.error())));
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
  warnings_label_->setStyleSheet(
      QString("font-family:'%1';font-size:10.5px;color:%2;")
          .arg(theme::monoFamily(), theme::css(warn.isEmpty() ? theme::good() : theme::warn())));

  replay_button_->setEnabled(project_.has_d6_raw && !replay_->running());
  updateViewportChips();
  refreshInspectorGeoref();
}

void MainWindow::refreshRecents() {
  QSettings s;
  recent_dirs_ = s.value("recentProjects").toStringList();
  recents_->clear();
  for (const QString& d : recent_dirs_) {
    auto* it = new QListWidgetItem();
    it->setData(Qt::UserRole, d);
    it->setSizeHint(QSize(0, 46));
    recents_->addItem(it);

    // The redesign's project card: name in the display face, then one mono
    // sub-line carrying the size/point figure and the georef badge.
    //
    // WHAT THE COUNT IS. A5's manifest records chunks and payload bytes, not
    // a point total — there is no per-project point count to read without
    // decoding the whole capture, which is not something a sidebar may do on
    // every repaint. So a recent shows its real payload figure, and the OPEN
    // project additionally shows the live PageStore total, which IS a point
    // count and is honest about being the live one.
    auto* card = new QWidget();
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(9, 5, 9, 5);
    v->setSpacing(1);

    auto* name = new QLabel(QFileInfo(d).fileName());
    QFont nf(theme::displayFamily(), 13);
    nf.setWeight(QFont::DemiBold);
    name->setFont(nf);
    name->setStyleSheet(QString("color:%1;").arg(theme::css(theme::ink())));

    const ProjectInfo info = (d == project_.dir) ? project_ : readProject(d);
    QString sub;
    if (!info.valid) {
      sub = "unreadable — " + info.error;
    } else if (d == project_.dir && host_ && host_->points()) {
      sub = QString("%1 pts · %2 · %3")
                .arg(groupedCount(host_->points()->total_points()))
                .arg(humanBytes(info.total_bytes))
                .arg(info.crs.isEmpty() ? "local frame" : "georef ✓");
    } else {
      // Deliberately short: a QLabel does not elide, and the sidebar is 360 px
      // wide, so a longer line is simply cut off mid-word (the first evidence
      // run of this card read "... · local fram"). The span and the full path
      // are in the tooltip and in the Project card below.
      sub = QString("%1 chunks · %2 · %3")
                .arg(info.total_chunks)
                .arg(humanBytes(info.total_bytes))
                .arg(info.crs.isEmpty() ? "local frame" : "georef ✓");
    }
    auto* meta = new QLabel(sub);
    meta->setMinimumWidth(0);
    meta->setStyleSheet(QString("font-family:'%1';font-size:10px;color:%2;")
                            .arg(theme::monoFamily(),
                                 theme::css(info.crs.isEmpty() ? theme::faint() : theme::good())));
    meta->setToolTip(
        info.valid ? QString("%1\n%2 chunks · %3 · %4 s · profile %5\nCRS: %6")
                         .arg(d)
                         .arg(info.total_chunks)
                         .arg(humanBytes(info.total_bytes))
                         .arg(info.duration_s, 0, 'f', 2)
                         .arg(info.profile.isEmpty() ? "-" : info.profile)
                         .arg(info.crs.isEmpty() ? "none (local frame)" : info.crs)
                   : d);

    v->addWidget(name);
    v->addWidget(meta);
    recents_->setItemWidget(it, card);
    if (d == project_.dir) it->setSelected(true);
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
  // Segment 1 is deliberately SHORT — "engine ready · idle", the mockup's
  // wording — because a status bar segment that grows with the device list
  // pushes the other three off the end of the bar. The full health line (every
  // device, its state, points and checksum rate) is the tooltip, and the
  // render detail lives on the viewport's own bottom-left chip.
  const QString health = host_->healthLine();
  QString shortState = health.section(" · ", 0, 1);
  if (capture_ ) {
    // A recording says so in the state segment, exactly as the mockup does.
    if (rec_badge_ && rec_badge_->isVisible()) {
      shortState = health.section(" · ", 0, 0) +
                   (rec_badge_->text() == "PAUSED" ? " · paused" : " · recording");
    }
  }
  status_engine_->setText(shortState);
  status_engine_->setToolTip(health);

  // MEASURE — the last completed segment, or an em dash. Same source the
  // measure dock lists from, so the two cannot drift.
  const auto& segs = viewport_->measurements();
  if (segs.empty()) {
    status_measure_->setText("MEASURE —");
  } else {
    status_measure_->setText(
        QString("MEASURE %1 m").arg(segs.back().distance_m, 0, 'f', 3));
  }

  refreshInspectorGeoref();
  updateViewportChips();
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

void MainWindow::onExportTransferBundle() {
  if (!project_.valid) {
    QMessageBox::information(this, "Export transfer bundle", "No project is open.");
    return;
  }
  auto* dlg = new TransferExportDialog(project_.dir, this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->show();
  dlg->raise();
  dlg->activateWindow();
}

void MainWindow::onImportTransferBundle() { importTransferBundle(QString()); }

void MainWindow::importTransferBundle(const QString& zipPath) {
  // Default destination: alongside the last project's directory when one is
  // known (so a transfer bundle lands next to the library the user is
  // already working in), otherwise the home directory.
  QString destRoot = QDir::homePath();
  if (!recent_dirs_.isEmpty()) destRoot = QFileInfo(recent_dirs_.first()).absolutePath();
  auto* dlg = new TransferImportDialog(zipPath, destRoot, this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  connect(dlg, &TransferImportDialog::projectReady, this, [this](const QString& dir) {
    QString err;
    if (!openProject(dir, &err)) {
      QMessageBox::warning(this, "Open imported project", err);
      return;
    }
    log_->appendPlainText("imported transfer bundle landed in the library: " + dir);
  });
  dlg->show();
  dlg->raise();
  dlg->activateWindow();
}

void MainWindow::onExportMerged() {
  if (!merge_dock_ || !merge_dock_->mergedStore() ||
      merge_dock_->mergedStore()->total_points() == 0) {
    QMessageBox::information(this, "Export merged cloud",
                             "Build the merged cloud first (Merge workbench -> \"Build merged "
                             "cloud -> viewport\").");
    return;
  }
  if (export_dialog_) export_dialog_->deleteLater();
  export_dialog_ =
      new ExportDialog(merge_dock_->mergedStore(), params_->get(), "merged", this);
  export_dialog_->setAttribute(Qt::WA_DeleteOnClose);
  connect(export_dialog_, &QObject::destroyed, this, [this] { export_dialog_ = nullptr; });
  export_dialog_->show();
  export_dialog_->raise();
  export_dialog_->activateWindow();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!event->mimeData()->hasUrls()) return;
  for (const QUrl& u : event->mimeData()->urls()) {
    if (!u.isLocalFile()) continue;
    const QString path = u.toLocalFile();
    if (path.endsWith(".lscan.zip", Qt::CaseInsensitive) || path.endsWith(".zip", Qt::CaseInsensitive)) {
      event->acceptProposedAction();
      return;
    }
  }
}

void MainWindow::dropEvent(QDropEvent* event) {
  if (!event->mimeData()->hasUrls()) return;
  for (const QUrl& u : event->mimeData()->urls()) {
    if (!u.isLocalFile()) continue;
    const QString path = u.toLocalFile();
    if (path.endsWith(".lscan.zip", Qt::CaseInsensitive) || path.endsWith(".zip", Qt::CaseInsensitive)) {
      event->acceptProposedAction();
      importTransferBundle(path);
      return;
    }
  }
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::FileOpen) {
    auto* fo = static_cast<QFileOpenEvent*>(event);
    const QString path = fo->file();
    if (path.endsWith(".lscan.zip", Qt::CaseInsensitive)) {
      importTransferBundle(path);
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
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
