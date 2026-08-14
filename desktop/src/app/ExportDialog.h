// ExportDialog.h — the C3 §3.13 "Export center" dialog: format (PLY/LAS/PCD),
// options (RGB/intensity, decimate, bounds-from-current-clipping), a progress
// bar with cancel, driven by engine/include/scanengine/export/exporter.h's
// export_points(). See engine/docs/A9-export.md for the writer contracts.
//
// THREADING
//   export_points() blocks and reports progress through a plain function
//   pointer called from whatever thread calls it — so, same shape as
//   ReplayController, this runs it on its own std::thread and polls an
//   atomic<float> from a QTimer on the GUI thread rather than touching any
//   widget from the export thread. Cancel is
//   scanengine::ExportCancelToken::request_cancel(), a poll-based atomic flag
//   the writer checks every 4096 raw points (A9-export.md).
//
// BOUNDS FROM CLIPPING
//   "Bounds from current clipping" reads whatever DisplayParams clip is
//   active in the viewport at the moment the dialog was opened (a snapshot,
//   not a live binding — the dialog does not re-read the dock while an
//   export is already using the bounds it captured). Box clipping maps
//   straight onto ExportBoundsFilter; height-only clipping maps onto a box
//   whose X/Y extent is effectively unbounded and whose Z extent is the
//   height band. Disabled (with a tooltip) when neither clip is enabled —
//   there is nothing to export bounds *from*.
//
// Owner: C3.
#pragma once

#include <QDialog>
#include <QString>

#include <atomic>
#include <thread>

#include "scanengine/cloud/display_params.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/export/exporter.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

namespace lidarscan {

class ExportDialog : public QDialog {
  Q_OBJECT
 public:
  // `store` must outlive the dialog (MainWindow owns it via EngineHost).
  ExportDialog(const scanengine::PageStore* store, const scanengine::DisplayParams& current_params,
              const QString& project_dir, QWidget* parent = nullptr);
  ~ExportDialog() override;

 private:
  void buildUi();
  void onFormatChanged();
  void onBrowse();
  void onExport();
  void onCancel();
  void onOpenContaining();
  void poll();
  QString defaultExtension() const;
  scanengine::ExportFormat selectedFormat() const;

  static void progressTrampoline(float fraction, void* user_data);

  const scanengine::PageStore* store_ = nullptr;
  scanengine::DisplayParams display_params_;
  QString project_dir_;

  QComboBox* format_ = nullptr;
  QCheckBox* include_rgb_ = nullptr;
  QCheckBox* include_intensity_ = nullptr;
  QSpinBox* decimate_ = nullptr;
  QCheckBox* bounds_from_clip_ = nullptr;
  QLineEdit* path_edit_ = nullptr;
  QPushButton* browse_button_ = nullptr;

  QPushButton* export_button_ = nullptr;
  QPushButton* cancel_button_ = nullptr;
  QPushButton* open_button_ = nullptr;
  QPushButton* close_button_ = nullptr;
  QProgressBar* progress_bar_ = nullptr;
  QLabel* status_ = nullptr;

  QTimer* poll_timer_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> done_{false};
  std::atomic<float> progress_{0.0f};
  scanengine::ExportCancelToken cancel_token_;
  scanengine::Status result_;
  QString out_path_;
  quint64 out_bytes_ = 0;
};

}  // namespace lidarscan
