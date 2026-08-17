// MainWindow.h — the desktop shell: Projects library, 3D viewport, display
// parameters, engine health (Tech Spec §3.13 "Desktop (Qt, capture +
// workstation)").
//
// C1 delivers the frame and the two workspaces that prove the engine linkage
// end to end (project viewing + replay). The rest of workstream C hangs off the
// seams named in NOTES.md: C2 the capture flows, C3 the review workspace, C4
// the processing queue, C5 floor plans, C6 the merge workbench, C7 transfer
// import, C8 packaging.
//
// Owner: C1.
#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>

#include <memory>

#include "scanengine/cloud/display_params.h"
#include "app/Project.h"
#include "ui/IconRail.h"

class QCloseEvent;
class QDockWidget;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QDoubleSpinBox;
class QResizeEvent;
class QTreeWidget;

namespace scanengine {
class PageStore;
}

namespace lidarscan {

class CaptureWindow;
class Chip;
class DisplayParamsDock;
class EngineHost;
class ExportDialog;
class InspectorCard;
class MeasureDock;
class MergeDock;
class PlanDock;
class ProcessingDock;
class ReplayController;
class ViewportHost;
class ViewportWindow;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(EngineHost* host, QWidget* parent = nullptr);
  ~MainWindow() override;

  ViewportWindow* viewport() { return viewport_; }
  // Lazily creates the capture PANEL (a dock across the foot of this window
  // since round 5 — it is not a popup any more) exactly like the rail's Capture
  // button does. Public so main.cpp's --mid360-selftest CLI hook can drive it
  // headlessly.
  CaptureWindow* captureWindow();
  // Public so main.cpp's C4/C5 evidence hooks can drive the processing queue
  // and the floor-plan workspace headlessly, the same posture captureWindow()
  // already established for C2/C3.
  ProcessingDock* processingDock() { return processing_dock_; }
  PlanDock* planDock() { return plan_dock_; }
  MergeDock* mergeDock() { return merge_dock_; }

  bool openProject(const QString& dir, QString* err = nullptr);
  void closeProject();
  bool startReplay(double speed, QString* err = nullptr);

  // C7: import a `.lscan.zip` transfer bundle (drag-drop, file-association,
  // File menu). Public so main.cpp's evidence hooks and the drag-drop/
  // QFileOpenEvent handlers can all funnel through one path.
  void importTransferBundle(const QString& zipPath);

  // C5 evidence/verification fixture: appends the A12 test-fixture building
  // (SyntheticBuilding.h) into the engine's own PageStore and points the
  // viewport + Plan dock at it — "any dense synthetic cloud" per the task.
  void loadSyntheticBuildingFixture();

  // Apply a display-parameter document (A14 from_json) — used by
  // --display-params so a scripted run can render a specific configuration.
  bool loadDisplayParamsFile(const QString& path, QString* err = nullptr);
  void applyDisplayProfile(scanengine::DisplayProfile profile);

  const ProjectInfo& project() const { return project_; }

  // --- redesign: the icon rail's workspaces ---------------------------------
  //
  // Switching workspace is exactly "show one right-hand dock, hide the rest",
  // plus the review workspace's full-bleed special case. Every dock keeps its
  // QDockWidget identity (floatable, resizable, toggleable from View), so
  // nothing a dock could do before this pass it cannot do now — the rail
  // replaces the five-deep TAB STRIP, not the docks.
  void showWorkspace(IconRail::Item item);
  // "projects"|"capture"|"review"|"plan"|"merge"|"jobs" — the --workspace CLI
  // hook and scripts/verify_redesign.sh drive the rail through this.
  bool showWorkspaceNamed(const QString& name);
  IconRail::Item workspace() const { return workspace_; }
  // Public so main.cpp's --inspector-demo evidence hook can drive the real
  // slider, the same posture captureWindow()/processingDock() already
  // established for C2/C4's hooks.
  InspectorCard* inspector() { return inspector_; }

  // Driven by CaptureWindow's phase so the RECORDING / PAUSED badge rides the
  // viewport (redesign brief item 4) even though capture is its own window.
  void setCaptureBadge(bool recording, bool paused);

  // --- CLI evidence hooks for the folded Projects tab (round-5 follow-up item
  // 4) ---------------------------------------------------------------------
  //
  // Selects the first `n` rows of the library through QListWidget's own
  // selection model — the same path a Cmd-click takes, so the selection-driven
  // action gating is exercised rather than asserted. Returns how many were
  // selected (fewer if the library is shorter).
  int selectRecentProjectsForCli(int n);
  // One line naming what the selection unlocked, for --projects-actions-demo.
  QString projectActionStateForCli() const;
  void triggerProcessSelectedForCli() { onProcessSelected(); }
  void triggerMergeSelectedForCli() { onMergeSelected(); }

 protected:
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void buildUi();
  void buildRail();
  void buildMenus();
  void onRailActivated(IconRail::Item item);
  void applyInspectorPlacement();  // floating card <-> reflowed dock at 880 px
  void updateViewportChips();
  void refreshInspectorGeoref();
  void refreshProjectPanel();
  void refreshRecents();
  void addRecent(const QString& dir);
  void updateStatus();
  void onNewProject();
  void onOpenProject();
  void onImportRaw();
  void onScreenshot();
  void onExport();
  void onExportTransferBundle();
  void onImportTransferBundle();
  void onExportMerged();
  void persistDisplayParamsIfProjectOpen();

  // --- Projects-tab actions (round-5 follow-up item 4) ---------------------
  //
  // "There are NO separate Processing/Merge tabs. Fold them into the Projects
  // tab: selecting one project shows Process + Export actions with the preview;
  // selecting 2+ unlocks Merge." These reposition the ENTRY POINTS only — the
  // job queue (ProcessingDock/A15) and the merge workbench (MergeDock/A13) are
  // the same docks doing the same work.
  QStringList selectedProjectDirs() const;
  void updateProjectSelectionActions();
  void onProcessSelected();
  void onMergeSelected();
  // Which panel is raised inside the Projects workspace: kProjects (none),
  // kJobs or kMerge.
  IconRail::Item projects_panel_ = IconRail::Item::kProjects;
  QPushButton* process_btn_ = nullptr;
  QPushButton* export_btn_ = nullptr;
  QPushButton* merge_btn_ = nullptr;
  QLabel* selection_hint_ = nullptr;

  EngineHost* host_ = nullptr;
  ViewportWindow* viewport_ = nullptr;
  ViewportHost* viewport_host_ = nullptr;
  DisplayParamsDock* params_dock_ = nullptr;
  MeasureDock* measure_dock_ = nullptr;
  ProcessingDock* processing_dock_ = nullptr;
  PlanDock* plan_dock_ = nullptr;
  MergeDock* merge_dock_ = nullptr;
  CaptureWindow* capture_ = nullptr;
  ExportDialog* export_dialog_ = nullptr;
  ReplayController* replay_ = nullptr;

  // --- redesign chrome ---
  IconRail* rail_ = nullptr;
  QDockWidget* rail_dock_ = nullptr;
  QDockWidget* projects_dock_ = nullptr;
  // The engine/app log used to live at the foot of the PROJECTS panel. Round 5
  // item 8 ("Projects = list of projects + preview of the selected scan,
  // nothing else") moved it into its own dock, hidden by default and reachable
  // from View — it is diagnostics, not part of either workspace's job.
  QDockWidget* log_dock_ = nullptr;
  InspectorCard* inspector_ = nullptr;
  QDockWidget* inspector_dock_ = nullptr;  // only used below 880 px
  bool inspector_visible_ = true;
  IconRail::Item workspace_ = IconRail::Item::kProjects;
  Chip* project_chip_ = nullptr;
  Chip* stats_chip_ = nullptr;
  Chip* rec_badge_ = nullptr;

  std::unique_ptr<scanengine::DisplayParamsController> params_;

  QListWidget* recents_ = nullptr;
  QTreeWidget* streams_ = nullptr;
  QLabel* project_label_ = nullptr;
  QLabel* warnings_label_ = nullptr;
  QPushButton* replay_button_ = nullptr;
  QPushButton* replay_stop_ = nullptr;
  QDoubleSpinBox* replay_speed_ = nullptr;
  QPlainTextEdit* log_ = nullptr;

  // The redesign's four status-bar segments: state · measure · georef σ ·
  // engine/render. `status_engine_` keeps its old name because updateStatus()
  // and every verify script's expectations are written against it.
  QLabel* status_engine_ = nullptr;
  QLabel* status_measure_ = nullptr;
  QLabel* status_georef_ = nullptr;
  QLabel* status_render_ = nullptr;

  ProjectInfo project_;
  QStringList recent_dirs_;

  // Keeps a loaded Post-process result (ProcessingDock::loadResultRequested)
  // alive for as long as the viewport/plan dock might read it — see
  // ProcessingDock.h's comment on why this is the mechanism rather than a
  // fresh engine/replay of processed/ (A7 does not persist the final cloud).
  std::shared_ptr<scanengine::PageStore> loaded_result_store_;
};

}  // namespace lidarscan
