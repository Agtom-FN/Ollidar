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
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdio>

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

// The app's own name and version, for the title bar and the status strip.
// QCoreApplication::applicationVersion() is set once in main() from the
// repo-root VERSION file (via CMake's LIDARSCAN_APP_VERSION), so this is a
// VIEW of that one value rather than a second copy of it — the same posture as
// every other model in this shell. Unobtrusive by design: the owner asked for
// the version to be visible, not prominent.
QString appTitle() {
  // ROUND 26 item 122: the app's DISPLAY name is "Ollidar". The bundle id,
  // the project directory and the repository keep the lidarscan name — see
  // `Wording.APP_NAME` on the Android side for why an identifier rename is
  // not a rename at all.
  return QString("Ollidar Desktop %1").arg(QCoreApplication::applicationVersion().section(' ', 0, 0));
}

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
  setWindowTitle(appTitle());
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
    status_render_->setText(QString("%1 · dpr %2 · %3 · ABI v%4 · app %5")
                                .arg(viewport_->surfaceDescription().section(" · ", 0, 0))
                                .arg(st.dpr, 0, 'f', 2)
                                .arg(host_->versionString().section(" (", 0, 0))
                                // The C++ constant, NOT capi/scanengine_c.h's
                                // SCAN_ABI_VERSION: NOTES.md §1.2 keeps the C
                                // ABI header out of the desktop entirely.
                                .arg(scanengine::kEngineAbiVersion)
                                // ...and the APP's own version, right next to
                                // the engine's, because they are different
                                // numbers on purpose and a support log needs
                                // both. Full "x.y.z (build N)" in the tooltip.
                                .arg(QCoreApplication::applicationVersion()
                                         .section(' ', 0, 0)));
    status_render_->setToolTip(QString("LidarScan %1\n%2")
                                   .arg(QCoreApplication::applicationVersion(), s));
    updateViewportChips();
  });
  connect(viewport_, &ViewportWindow::initFailed, this, [this](const QString& why) {
    log_->appendPlainText("VIEWPORT INIT FAILED: " + why);
    status_render_->setText("renderer unavailable: " + why);
  });

  buildRail();

  // --- LOG dock (round 5 item 8) ------------------------------------------
  //
  // Built BEFORE the projects dock because half of buildUi()/the constructor
  // appends to log_. Hidden by default: it is the app's diagnostics pane, not
  // part of the Projects or Capture workspace, and the owner's round-5 note is
  // explicit that Projects carries the library and the preview and nothing else.
  {
    log_dock_ = new QDockWidget("LOG", this);
    log_dock_->setObjectName("logDock");
    log_ = new QPlainTextEdit();
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    log_->setMinimumHeight(120);
    log_dock_->setWidget(log_);
    addDockWidget(Qt::BottomDockWidgetArea, log_dock_);
    log_dock_->hide();
  }

  // --- left dock: the project library + the selected scan's preview --------
  //
  // Round 5 item 8, verbatim: "Projects = list of projects + preview of the
  // selected scan, nothing else." So: the library list, the selected project's
  // own card (manifest/streams/reader warnings — the preview's metadata), and
  // the replay transport that PUTS that scan in the viewport. What left: the
  // "New…" button (a new project is a capture now — the Capture workspace owns
  // creation), "Import raw D6…" (a File-menu conversion, not library browsing;
  // still there), and the log pane (its own dock, above). "Open…" stays: adding
  // an existing project on disk to the list IS list management.
  {
    auto* dock = new QDockWidget("PROJECTS", this);
    projects_dock_ = dock;
    auto* w = new QWidget();
    auto* v = new QVBoxLayout(w);

    auto* libBox = new QGroupBox("Library");
    auto* lv = new QVBoxLayout(libBox);
    recents_ = new QListWidget();
    recents_->setToolTip(
        "The .lscan project library. Click one to preview it; Cmd/Ctrl- or "
        "Shift-click two or more to unlock Merge.");
    recents_->setSpacing(2);
    // Multi-select, because the merge entry point is now "select 2+ projects"
    // (round-5 follow-up item 4) rather than a workspace of its own.
    recents_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // Selection, not clicks, drives everything: a Cmd-click that ADDS to a
    // multi-selection must not also swap the previewed project out from under
    // it, so a project is opened only when it is the ONLY one selected.
    connect(recents_, &QListWidget::itemSelectionChanged, this, [this] {
      const QStringList sel = selectedProjectDirs();
      updateProjectSelectionActions();
      if (sel.size() != 1 || sel.first() == project_.dir) return;
      QString err;
      if (!openProject(sel.first(), &err)) {
        QMessageBox::warning(this, "Open project", err);
      }
    });
    lv->addWidget(recents_);
    auto* openBtn = new QPushButton("Open…");
    openBtn->setToolTip("Add an existing .lscan project on disk to this list.");
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::onOpenProject);
    lv->addWidget(openBtn);
    auto* newHint = new QLabel("New scans are created in the Capture workspace.");
    newHint->setWordWrap(true);
    newHint->setProperty("role", "hint");
    lv->addWidget(newHint);
    v->addWidget(libBox);

    // --- what you can DO with the selection (round-5 follow-up item 4) ------
    //
    // The Processing and Merge workspaces are gone from the rail; these are
    // their entry points, next to the preview they act on. Nothing about the
    // job queue or the merge workbench changed — only where you start them.
    {
      auto* actBox = new QGroupBox("Selected scan");
      auto* av = new QVBoxLayout(actBox);
      auto* row = new QWidget();
      auto* rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      process_btn_ = new QPushButton("Process…");
      process_btn_->setToolTip(
          "Open the job queue for this project — A15's kPostProcess/kColorize/"
          "kExport/kCloudSubmit jobs, the same queue and worker as before.");
      export_btn_ = new QPushButton("Export…");
      export_btn_->setToolTip("Export the cloud currently in the viewport (PLY / LAS / PCD).");
      connect(process_btn_, &QPushButton::clicked, this, &MainWindow::onProcessSelected);
      connect(export_btn_, &QPushButton::clicked, this, &MainWindow::onExport);
      rl->addWidget(process_btn_);
      rl->addWidget(export_btn_);
      av->addWidget(row);
      merge_btn_ = new QPushButton("Merge selected…");
      merge_btn_->setToolTip(
          "Select two or more projects in the library to merge them: each one is "
          "loaded as a merge session (MergeSessionLoader) and the merge workbench "
          "opens on them.");
      connect(merge_btn_, &QPushButton::clicked, this, &MainWindow::onMergeSelected);
      av->addWidget(merge_btn_);
      selection_hint_ = new QLabel();
      selection_hint_->setWordWrap(true);
      selection_hint_->setProperty("role", "hint");
      av->addWidget(selection_hint_);
      v->addWidget(actBox);
    }

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

    // The preview transport for the selected scan: replay decodes its D6 raw
    // chunks back through the engine and into the viewport, which is what
    // "preview of the selected scan" means for a recorded project.
    auto* replayBox = new QGroupBox("Preview this scan (replay → viewport)");
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
    v->addStretch(1);

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
    if (capture_) capture_->refreshDisplayControls();
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
    if (capture_) capture_->refreshDisplayControls();
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
      // Round 5 item 7: no popup. The capture panel is a dock across the foot of
      // this window, so switching to the workspace IS opening it — and the live
      // preview it drives is the viewport directly above it.
      captureWindow();
      showWorkspace(IconRail::Item::kCapture);
      return;
    case IconRail::Item::kJobs:
    case IconRail::Item::kMerge:
      // No rail button leads here any more (round-5 follow-up item 4), but the
      // View menu, Cmd-5/Cmd-6 and `--workspace jobs|merge` still do — and they
      // now mean "Projects, with that panel raised", which is where those two
      // live. Nothing in ProcessingDock/MergeDock changed.
      projects_panel_ = item;
      showWorkspace(IconRail::Item::kProjects);
      return;
    case IconRail::Item::kProjects:
      // Coming back to Projects from elsewhere shows the library and its
      // preview; a sub-panel is raised only by its own action.
      projects_panel_ = IconRail::Item::kProjects;
      showWorkspace(IconRail::Item::kProjects);
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

QStringList MainWindow::selectedProjectDirs() const {
  QStringList dirs;
  if (!recents_) return dirs;
  for (QListWidgetItem* it : recents_->selectedItems()) {
    const QString d = it->data(Qt::UserRole).toString();
    if (!d.isEmpty()) dirs << d;
  }
  return dirs;
}

void MainWindow::updateProjectSelectionActions() {
  if (!process_btn_) return;
  const QStringList sel = selectedProjectDirs();
  const bool one = sel.size() == 1;
  const bool many = sel.size() >= 2;
  process_btn_->setEnabled(one);
  export_btn_->setEnabled(one);
  merge_btn_->setEnabled(many);
  merge_btn_->setText(many ? QString("Merge %1 selected…").arg(sel.size())
                           : QString("Merge selected…"));
  if (sel.isEmpty()) {
    selection_hint_->setText("Select a scan to preview, process or export it.");
  } else if (one) {
    selection_hint_->setText(
        "Process runs the A15 job queue on this scan; Export writes the cloud in the "
        "viewport. Select a second scan to unlock Merge.");
  } else {
    selection_hint_->setText(
        QString("%1 scans selected — Merge loads each one as a session in the merge "
                "workbench.")
            .arg(sel.size()));
  }
}

int MainWindow::selectRecentProjectsForCli(int n) {
  if (!recents_) return 0;
  recents_->clearSelection();
  const int count = qMin(n, recents_->count());
  for (int i = 0; i < count; ++i) recents_->item(i)->setSelected(true);
  return count;
}

QString MainWindow::projectActionStateForCli() const {
  if (!process_btn_) return QStringLiteral("no projects panel");
  return QString("%1 selected · Process %2 · Export %3 · Merge %4 (\"%5\") · panel: %6")
      .arg(selectedProjectDirs().size())
      .arg(process_btn_->isEnabled() ? "enabled" : "disabled")
      .arg(export_btn_->isEnabled() ? "enabled" : "disabled")
      .arg(merge_btn_->isEnabled() ? "enabled" : "disabled")
      .arg(merge_btn_->text())
      .arg(projects_panel_ == IconRail::Item::kJobs
               ? "processing"
               : projects_panel_ == IconRail::Item::kMerge ? "merge" : "none");
}

void MainWindow::onProcessSelected() {
  const QStringList sel = selectedProjectDirs();
  if (sel.size() != 1) return;
  processing_dock_->setProjectDir(sel.first());
  processing_dock_->setCurrentStore(host_->points());
  projects_panel_ = IconRail::Item::kJobs;
  showWorkspace(IconRail::Item::kProjects);
  processing_dock_->raise();
  log_->appendPlainText("processing panel opened for " + sel.first());
}

void MainWindow::onMergeSelected() {
  const QStringList sel = selectedProjectDirs();
  if (sel.size() < 2) return;
  int added = 0;
  for (const QString& dir : sel) {
    QString err;
    // The SAME loader "Add from open project" / "Import .lscan project…" use —
    // a private Engine + unpaced ReplaySource over the project's raw chunks.
    if (merge_dock_->addFromProject(dir, QFileInfo(dir).completeBaseName(), &err)) {
      ++added;
    } else {
      // stderr as well as the log dock: a headless evidence run has to be able
      // to see WHY a session was refused (the common case is a Mid-360-only
      // project — record/replay.h forwards ChunkType::kD6Raw and nothing else,
      // the same limit MainWindow::startReplay() reports).
      log_->appendPlainText("merge: could not add " + dir + ": " + err);
      std::fprintf(stderr, "[lidarscan] merge: could not add %s: %s\n", dir.toUtf8().constData(),
                   err.toUtf8().constData());
    }
  }
  projects_panel_ = IconRail::Item::kMerge;
  showWorkspace(IconRail::Item::kProjects);
  merge_dock_->raise();
  log_->appendPlainText(QString("merge workbench: %1 of %2 selected projects added as sessions")
                            .arg(added)
                            .arg(sel.size()));
}

void MainWindow::showWorkspace(IconRail::Item item) {
  workspace_ = item;

  // Review is the only full-bleed workspace: nothing but the viewport, its
  // chips and the floating inspector.
  const bool review = item == IconRail::Item::kReview;
  const bool capture = item == IconRail::Item::kCapture;
  // Round 5 item 8: the two workspaces no longer share the projects panel.
  // Capture creates scans and shows nothing else; Projects lists and previews.
  projects_dock_->setVisible(item == IconRail::Item::kProjects);
  if (capture_) capture_->setVisible(capture);

  // Exactly one right-hand dock, so Qt draws no tab bar — except in Capture,
  // where the A14 display panel is deliberately up: round 5 item 10 asks for
  // ALL display parameters to be adjustable while the preview streams, and this
  // is the panel that owns the ~30 of them the capture column has no room for
  // (clipping, adaptive sizing, EDL, overlays, background).
  params_dock_->setVisible(capture);
  measure_dock_->setVisible(false);
  plan_dock_->setVisible(item == IconRail::Item::kPlan);
  // Round-5 follow-up item 4: the job queue and the merge workbench are PANELS
  // OF THE PROJECTS WORKSPACE now, raised by that workspace's own actions
  // (Process… / Merge selected…), not destinations of their own.
  const bool projects = item == IconRail::Item::kProjects;
  processing_dock_->setVisible(projects && projects_panel_ == IconRail::Item::kJobs);
  merge_dock_->setVisible(projects && projects_panel_ == IconRail::Item::kMerge);

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
    case IconRail::Item::kCapture: shown = params_dock_; wantWidth = 380; break;
    case IconRail::Item::kProjects:
      if (projects_panel_ == IconRail::Item::kMerge) {
        shown = merge_dock_;
        wantWidth = 720;
      } else if (projects_panel_ == IconRail::Item::kJobs) {
        shown = processing_dock_;
        wantWidth = 560;
      }
      break;
    default: break;
  }
  if (shown) resizeDocks({shown}, {qMin(wantWidth, width() / 2)}, Qt::Horizontal);
  // The capture panel is four columns and a control bar; it needs its height,
  // and the viewport above it needs the rest.
  if (capture && capture_) {
    resizeDocks({capture_}, {qMin(430, height() / 2)}, Qt::Vertical);
  }

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
  capture->addAction("Capture workspace (new scan)", this,
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
      // These two are panels INSIDE Projects now (round-5 follow-up item 4);
      // the menu entries stay as the keyboard route to them.
      {"&Merge (in Projects)", IconRail::Item::kMerge, "Ctrl+5"},
      {"Pr&ocessing (in Projects)", IconRail::Item::kJobs, "Ctrl+6"},
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
  view->addAction(log_dock_->toggleViewAction());

  auto* tools = menuBar()->addMenu("&Tools");
  tools->addAction("Load synthetic building fixture (C5 test fixture)…", this,
                   &MainWindow::loadSyntheticBuildingFixture);

  auto* help = menuBar()->addMenu("&Help");
  help->addAction("About", this, [this] {
    QMessageBox box(this);
    box.setWindowTitle("Ollidar Desktop");
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
    // ONE display model for the whole app (NOTES.md §1.7): the capture panel's
    // inline live controls bind the same DisplayParamsController the A14 dock
    // and the inspector card do.
    capture_ = new CaptureWindow(host_, params_.get(), this);
    addDockWidget(Qt::BottomDockWidgetArea, capture_);
    capture_->hide();  // shown by showWorkspace(kCapture)
    connect(capture_, &CaptureWindow::logLine, this,
            [this](const QString& l) { log_->appendPlainText(l); });
    connect(capture_, &CaptureWindow::previewStarted, this, [this] {
      // The live preview IS the viewport's cloud from here on.
      viewport_->setPointStore(host_->points());
    });
    connect(capture_, &CaptureWindow::captureStarted, this, [this](const QString& d) {
      log_->appendPlainText("capture started into " + d);
      viewport_->setPointStore(host_->points());
    });
    // Round 5 item 9: "Stop = seal -> project appears in Projects tab". The
    // display parameters the operator was capturing with are persisted into the
    // new project FIRST, so opening it below reads exactly what was on screen
    // instead of resetting to the profile defaults.
    connect(capture_, &CaptureWindow::captureStopped, this, [this](const QString& dir) {
      log_->appendPlainText("capture stopped" + (dir.isEmpty() ? QString() : " — sealed " + dir));
      if (dir.isEmpty()) return;
      QString perr;
      if (!params_dock_->saveToProject(dir, &perr)) {
        log_->appendPlainText("could not persist display parameters into " + dir + ": " + perr);
      }
      QString err;
      if (!openProject(dir, &err)) {
        log_->appendPlainText("just-recorded project could not be opened: " + err);
        addRecent(dir);  // it still belongs in the library, readable or not
      }
    });
    connect(capture_, &CaptureWindow::displayParamsChanged, this, [this] {
      viewport_->setDisplayParams(params_->get());
      params_dock_->refreshFromModel();
      if (inspector_) inspector_->refreshFromModel();
    });
    connect(capture_, &CaptureWindow::liveRefreshHzChanged, this, [this](double hz) {
      viewport_->setMaxFps(hz);
      log_->appendPlainText(QString("live viewport refresh cap: %1 fps").arg(hz, 0, 'f', 0));
    });
    // Round-5 item 18: the walked path, drawn in the viewport during preview AND
    // recording. The panel accumulates it (the engine's LioPoseSource cannot be
    // enumerated — see CaptureWindow::pollTrajectory), the viewport draws it.
    connect(capture_, &CaptureWindow::trajectoryTrailChanged, this,
            [this](const std::vector<std::array<float, 3>>& path) {
              viewport_->setTrajectoryTrail(path);
            });
    // Round-5 item 17: the viewport measured that this machine cannot sustain the
    // current live refresh and stepped down; the panel says so, quietly, inline.
    connect(viewport_, &ViewportWindow::refreshGovernorChanged, capture_,
            [this](double hz, bool down, const QString& why) {
              capture_->noteRefreshGovernor(hz, down, why);
            });
    // ...and the CEILING is the screen's own refresh rate. QScreen is only
    // meaningful once the window exists, which by here it does (MainWindow's
    // viewport was created in buildUi and shown by main()).
    {
      double ceiling = 60.0;
      if (QScreen* s = viewport_->screen()) {
        if (s->refreshRate() > 1.0) ceiling = s->refreshRate();
      }
      viewport_->setRefreshCeiling(ceiling);
      capture_->setLiveRefreshCeiling(ceiling);
      log_->appendPlainText(QString("live refresh ceiling: %1 Hz (this display)")
                                .arg(ceiling, 0, 'f', 0));
    }
    // The RECORDING / PAUSED badge rides the viewport (redesign brief item 4),
    // which since round 5 is directly above the panel that owns the phase.
    connect(capture_, &CaptureWindow::recordingStateChanged, this,
            &MainWindow::setCaptureBadge);
    // The panel restored the persisted refresh cap in its constructor, before
    // the connect above existed; this is what hands it to the viewport.
    capture_->applyLiveRefreshRate();
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
  setWindowTitle(QString("%1 — %2").arg(appTitle(), info.name));
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
  setWindowTitle(appTitle());
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
  updateProjectSelectionActions();
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
