// TransferDialog.h — C7 §3.8/§3.13 "Extract for transfer": package a project
// as a `.lscan.zip` and validate/land one that arrived from elsewhere.
//
// Both dialogs drive engine/include/scanengine/record/zip.h's zip_export()/
// zip_import() DIRECTLY (not through jobs::run_transfer_export()/
// jobs::import_and_validate() — see NOTES.md's C7 section for why: this is a
// project-context action, parallel to C3's ExportDialog, distinct from C4's
// ProcessingDock "Transfer bundle…" which already goes through the job
// queue). Both hooks INT-34 added (ZipProgressFn + ZipCancelToken,
// docs/A15-jobs.md §7.4 / docs/INT34-wiring.md §7) are exercised for real
// here: a genuine progress bar over payload bytes and a Cancel button that
// removes the half-written archive (export) or leaves what was extracted
// (import) — record/zip.h's own documented asymmetry.
//
// THREADING: same shape as ExportDialog — zip_export()/zip_import() block, so
// each runs on its own std::thread, polled via an atomic<float> read by a
// QTimer on the GUI thread. Neither function ever touches a QWidget.
//
// Owner: C7.
#pragma once

#include <QDialog>
#include <QString>

#include <atomic>
#include <thread>

#include "app/Project.h"
#include "scanengine/record/zip.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace lidarscan {

class TransferExportDialog : public QDialog {
  Q_OBJECT
 public:
  // `projectDir` must be a valid, open-able .lscan directory.
  TransferExportDialog(const QString& projectDir, QWidget* parent = nullptr);
  ~TransferExportDialog() override;

  // Evidence/CLI hooks, same posture as TransferImportDialog's.
  void setZipPathForCli(const QString& path);
  void triggerExportForCli() { onExport(); }

 private:
  void buildUi();
  void onBrowse();
  void onExport();
  void onCancel();
  void onOpenContaining();
  void poll();

  QString project_dir_;
  QLineEdit* path_edit_ = nullptr;
  QProgressBar* progress_bar_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* export_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;
  QPushButton* open_button_ = nullptr;
  QPushButton* close_button_ = nullptr;
  QTimer* poll_timer_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> done_{false};
  std::atomic<float> progress_{0.0f};
  scanengine::lscan::ZipCancelToken cancel_token_;
  scanengine::Status result_;
  QString out_path_;
};

class TransferImportDialog : public QDialog {
  Q_OBJECT
 public:
  // `zipPath` may be empty (the user browses); `defaultDestDir` seeds the
  // destination-directory field (e.g. the projects library root).
  TransferImportDialog(const QString& zipPath, const QString& defaultDestDir,
                       QWidget* parent = nullptr);
  ~TransferImportDialog() override;

  // Evidence/CLI hooks (mirror CaptureWindow::triggerRecordForCli's posture).
  // Public so main.cpp's --transfer-import-dialog-shot can drive the real
  // dialog (set an exact destination, click Import exactly as the button
  // would) and screenshot it, including the manifest sanity report, with no
  // synthetic QMouseEvent needed.
  void setDestDirForCli(const QString& dir);
  void triggerImportForCli() { onImport(); }

 Q_SIGNALS:
  // Emitted once import succeeds and the operator clicks "Open in library" —
  // MainWindow wires this straight to openProject().
  void projectReady(const QString& destDir);

 private:
  void buildUi();
  void onBrowseZip();
  void onBrowseDest();
  void onImport();
  void onCancel();
  void onOpenInLibrary();
  void poll();
  void showReport(const ProjectInfo& info);

  QLineEdit* zip_edit_ = nullptr;
  QLineEdit* dest_edit_ = nullptr;
  QProgressBar* progress_bar_ = nullptr;
  QLabel* status_ = nullptr;
  QTreeWidget* report_tree_ = nullptr;
  QLabel* warnings_label_ = nullptr;
  QPushButton* import_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;
  QPushButton* open_lib_button_ = nullptr;
  QPushButton* close_button_ = nullptr;
  QTimer* poll_timer_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> done_{false};
  std::atomic<float> progress_{0.0f};
  scanengine::lscan::ZipCancelToken cancel_token_;
  scanengine::Status result_;
  QString dest_dir_;
};

}  // namespace lidarscan
