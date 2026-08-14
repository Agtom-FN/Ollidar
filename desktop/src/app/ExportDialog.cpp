#include "app/ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

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

ExportDialog::ExportDialog(const scanengine::PageStore* store,
                           const scanengine::DisplayParams& current_params,
                           const QString& project_dir, QWidget* parent)
    : QDialog(parent), store_(store), display_params_(current_params), project_dir_(project_dir) {
  setWindowTitle("Export");
  setModal(false);
  buildUi();

  poll_timer_ = new QTimer(this);
  poll_timer_->setInterval(120);
  connect(poll_timer_, &QTimer::timeout, this, &ExportDialog::poll);

  onFormatChanged();
}

ExportDialog::~ExportDialog() {
  if (running_.load()) {
    cancel_token_.request_cancel();
  }
  if (thread_.joinable()) thread_.join();
}

void ExportDialog::buildUi() {
  auto* v = new QVBoxLayout(this);
  auto* f = new QFormLayout();

  format_ = new QComboBox();
  format_->addItem("PLY (binary, RGB + intensity)", int(scanengine::ExportFormat::kPlyBinary));
  format_->addItem("LAS 1.4 (georeferenced)", int(scanengine::ExportFormat::kLas14));
  format_->addItem("PCD (binary)", int(scanengine::ExportFormat::kPcd));
  connect(format_, &QComboBox::currentIndexChanged, this, [this](int) { onFormatChanged(); });
  f->addRow("Format", format_);

  include_rgb_ = new QCheckBox("Include colour (RGB)");
  include_rgb_->setChecked(true);
  f->addRow(include_rgb_);

  include_intensity_ = new QCheckBox("Include intensity");
  include_intensity_->setChecked(true);
  include_intensity_->setToolTip(
      "PointVertex has no separate intensity channel yet — every writer derives it "
      "from RGB luminance (export/point_source.h, docs/A9-export.md), exact for an "
      "unmodified D6 capture.");
  f->addRow(include_intensity_);

  decimate_ = new QSpinBox();
  decimate_->setRange(1, 1000);
  decimate_->setValue(1);
  decimate_->setToolTip("Keep 1 of every N points. 1 = no decimation.");
  f->addRow("Decimate (1 of N)", decimate_);

  bounds_from_clip_ = new QCheckBox("Bounds from current clipping");
  const bool have_clip = display_params_.clip_box_enabled || display_params_.clip_height_enabled;
  bounds_from_clip_->setEnabled(have_clip);
  bounds_from_clip_->setToolTip(
      have_clip ? "Crop the export to the box/height clip active in the viewport "
                  "when this dialog was opened."
                : "No clipping is active in the display-parameters dock, so there is "
                  "nothing to export bounds from.");
  f->addRow(bounds_from_clip_);

  auto* path_row = new QWidget();
  auto* pl = new QHBoxLayout(path_row);
  pl->setContentsMargins(0, 0, 0, 0);
  path_edit_ = new QLineEdit();
  browse_button_ = new QPushButton("…");
  browse_button_->setFixedWidth(32);
  connect(browse_button_, &QPushButton::clicked, this, &ExportDialog::onBrowse);
  pl->addWidget(path_edit_, 1);
  pl->addWidget(browse_button_);
  f->addRow("Output file", path_row);

  v->addLayout(f);

  progress_bar_ = new QProgressBar();
  progress_bar_->setRange(0, 100);
  v->addWidget(progress_bar_);

  status_ = new QLabel();
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(status_);

  auto* brow = new QWidget();
  auto* bl = new QHBoxLayout(brow);
  bl->setContentsMargins(0, 0, 0, 0);
  export_button_ = new QPushButton("Export");
  cancel_button_ = new QPushButton("Cancel");
  open_button_ = new QPushButton("Open containing folder");
  close_button_ = new QPushButton("Close");
  cancel_button_->setEnabled(false);
  open_button_->setEnabled(false);
  connect(export_button_, &QPushButton::clicked, this, &ExportDialog::onExport);
  connect(cancel_button_, &QPushButton::clicked, this, &ExportDialog::onCancel);
  connect(open_button_, &QPushButton::clicked, this, &ExportDialog::onOpenContaining);
  connect(close_button_, &QPushButton::clicked, this, &QDialog::close);
  bl->addWidget(export_button_);
  bl->addWidget(cancel_button_);
  bl->addWidget(open_button_);
  bl->addStretch(1);
  bl->addWidget(close_button_);
  v->addWidget(brow);

  resize(520, 340);
}

scanengine::ExportFormat ExportDialog::selectedFormat() const {
  return static_cast<scanengine::ExportFormat>(format_->currentData().toInt());
}

QString ExportDialog::defaultExtension() const {
  switch (selectedFormat()) {
    case scanengine::ExportFormat::kPlyBinary: return "ply";
    case scanengine::ExportFormat::kLas14: return "las";
    case scanengine::ExportFormat::kPcd: return "pcd";
    default: return "bin";
  }
}

void ExportDialog::onFormatChanged() {
  // Regenerate the default path only while it still looks auto-generated
  // (empty, or ending in one of the three extensions this dialog offers) —
  // once the user has hand-edited it, stop touching it.
  const QString ext = defaultExtension();
  const QString current = path_edit_->text();
  const bool looks_auto = current.isEmpty() || current.endsWith(".ply") ||
                          current.endsWith(".las") || current.endsWith(".pcd");
  if (looks_auto) {
    const QString base = QFileInfo(project_dir_).completeBaseName();
    const QString name = (base.isEmpty() ? "export" : base) + "." + ext;
    path_edit_->setText(QDir(project_dir_).filePath("exports/" + name));
  }
}

void ExportDialog::onBrowse() {
  QString filter;
  switch (selectedFormat()) {
    case scanengine::ExportFormat::kPlyBinary: filter = "PLY (*.ply)"; break;
    case scanengine::ExportFormat::kLas14: filter = "LAS (*.las)"; break;
    case scanengine::ExportFormat::kPcd: filter = "PCD (*.pcd)"; break;
    default: filter = "All files (*)"; break;
  }
  const QString p = QFileDialog::getSaveFileName(this, "Export point cloud", path_edit_->text(),
                                                 filter);
  if (!p.isEmpty()) path_edit_->setText(p);
}

void ExportDialog::onExport() {
  if (!store_) {
    status_->setText("No point cloud loaded — open or replay a project first.");
    return;
  }
  const QString path = path_edit_->text().trimmed();
  if (path.isEmpty()) {
    status_->setText("Choose an output file first.");
    return;
  }
  QDir().mkpath(QFileInfo(path).absolutePath());

  scanengine::ExportOptions opts;
  opts.format = selectedFormat();
  opts.output_path = path.toStdString();
  opts.include_color = include_rgb_->isChecked();
  opts.include_intensity = include_intensity_->isChecked();
  opts.decimate = std::uint32_t(decimate_->value());
  opts.las_gps_time = false;  // no real GPS time source yet (A9-export.md)

  if (bounds_from_clip_->isEnabled() && bounds_from_clip_->isChecked()) {
    opts.bounds_filter.enabled = true;
    if (display_params_.clip_box_enabled) {
      for (int k = 0; k < 3; ++k) {
        opts.bounds_filter.min[k] = display_params_.clip_box_min[k];
        opts.bounds_filter.max[k] = display_params_.clip_box_max[k];
      }
    } else {
      // Height-only clip: X/Y effectively unbounded, Z is the height band.
      constexpr float kBig = 1e9f;
      opts.bounds_filter.min[0] = -kBig;
      opts.bounds_filter.min[1] = -kBig;
      opts.bounds_filter.max[0] = kBig;
      opts.bounds_filter.max[1] = kBig;
      opts.bounds_filter.min[2] = display_params_.clip_height_min;
      opts.bounds_filter.max[2] = display_params_.clip_height_max;
    }
  }

  out_path_ = path;
  out_bytes_ = 0;
  progress_.store(0.0f);
  done_.store(false);
  running_.store(true);
  result_ = scanengine::Status();

  export_button_->setEnabled(false);
  browse_button_->setEnabled(false);
  format_->setEnabled(false);
  cancel_button_->setEnabled(true);
  open_button_->setEnabled(false);
  close_button_->setEnabled(true);
  progress_bar_->setValue(0);
  status_->setText("Exporting…");

  const scanengine::PageStore* store = store_;
  thread_ = std::thread([this, store, opts] {
    const auto st =
        scanengine::export_points(*store, scanengine::Span<const scanengine::StreamId>{},
                                  opts.format, opts.output_path, opts, &ExportDialog::progressTrampoline,
                                  this, &cancel_token_);
    result_ = st;
    done_.store(true);
  });
  poll_timer_->start();
}

void ExportDialog::onCancel() {
  if (!running_.load()) return;
  cancel_token_.request_cancel();
  cancel_button_->setEnabled(false);
  status_->setText("Cancelling…");
}

void ExportDialog::poll() {
  progress_bar_->setValue(int(progress_.load() * 100.0f));
  if (!done_.load()) return;

  poll_timer_->stop();
  if (thread_.joinable()) thread_.join();
  running_.store(false);

  export_button_->setEnabled(true);
  browse_button_->setEnabled(true);
  format_->setEnabled(true);
  cancel_button_->setEnabled(false);

  if (result_.ok()) {
    out_bytes_ = quint64(QFileInfo(out_path_).size());
    progress_bar_->setValue(100);
    status_->setText(QString("Export complete: %1 (%2)").arg(out_path_, humanBytes(out_bytes_)));
    open_button_->setEnabled(true);
  } else if (result_.error() == scanengine::ScanError::kCancelled) {
    status_->setText("Export cancelled — the partial output file was removed.");
  } else {
    status_->setText(QString("Export failed: %1").arg(scanengine::error_str(result_.error())));
  }
}

void ExportDialog::onOpenContaining() {
  if (out_path_.isEmpty()) return;
#if defined(Q_OS_MACOS)
  QProcess::startDetached("open", {"-R", out_path_});
#elif defined(Q_OS_WIN)
  QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(out_path_)});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(out_path_).absolutePath()));
#endif
}

void ExportDialog::progressTrampoline(float fraction, void* user_data) {
  // Runs on the export thread (export/exporter.h's documented contract) —
  // must not touch any QWidget. Only the atomic is written here; poll()
  // reads it from the GUI thread on a timer, the same shape
  // ReplayController uses for its own worker thread.
  auto* self = static_cast<ExportDialog*>(user_data);
  if (self) self->progress_.store(fraction);
}

}  // namespace lidarscan
