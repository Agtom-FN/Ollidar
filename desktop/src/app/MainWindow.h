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

class QCloseEvent;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QDoubleSpinBox;
class QTreeWidget;

namespace lidarscan {

class CaptureWindow;
class DisplayParamsDock;
class EngineHost;
class ExportDialog;
class MeasureDock;
class ReplayController;
class ViewportWindow;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(EngineHost* host, QWidget* parent = nullptr);
  ~MainWindow() override;

  ViewportWindow* viewport() { return viewport_; }
  // Lazily creates the capture window exactly like the "Capture" menu does.
  // Public so main.cpp's --mid360-selftest CLI hook can drive it headlessly.
  CaptureWindow* captureWindow();

  bool openProject(const QString& dir, QString* err = nullptr);
  void closeProject();
  bool startReplay(double speed, QString* err = nullptr);

  // Apply a display-parameter document (A14 from_json) — used by
  // --display-params so a scripted run can render a specific configuration.
  bool loadDisplayParamsFile(const QString& path, QString* err = nullptr);
  void applyDisplayProfile(scanengine::DisplayProfile profile);

  const ProjectInfo& project() const { return project_; }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildUi();
  void buildMenus();
  void refreshProjectPanel();
  void refreshRecents();
  void addRecent(const QString& dir);
  void updateStatus();
  void onNewProject();
  void onOpenProject();
  void onImportRaw();
  void onScreenshot();
  void onExport();
  void persistDisplayParamsIfProjectOpen();

  EngineHost* host_ = nullptr;
  ViewportWindow* viewport_ = nullptr;
  DisplayParamsDock* params_dock_ = nullptr;
  MeasureDock* measure_dock_ = nullptr;
  CaptureWindow* capture_ = nullptr;
  ExportDialog* export_dialog_ = nullptr;
  ReplayController* replay_ = nullptr;

  std::unique_ptr<scanengine::DisplayParamsController> params_;

  QListWidget* recents_ = nullptr;
  QTreeWidget* streams_ = nullptr;
  QLabel* project_label_ = nullptr;
  QLabel* warnings_label_ = nullptr;
  QPushButton* replay_button_ = nullptr;
  QPushButton* replay_stop_ = nullptr;
  QDoubleSpinBox* replay_speed_ = nullptr;
  QPlainTextEdit* log_ = nullptr;

  QLabel* status_engine_ = nullptr;
  QLabel* status_render_ = nullptr;

  ProjectInfo project_;
  QStringList recent_dirs_;
};

}  // namespace lidarscan
