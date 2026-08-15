#include "app/TransferDialog.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
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

void revealInFileManager(const QString& path) {
#if defined(Q_OS_MACOS)
  QProcess::startDetached("open", {"-R", path});
#elif defined(Q_OS_WIN)
  QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

}  // namespace

// ============================================================================
// TransferExportDialog
// ============================================================================

TransferExportDialog::TransferExportDialog(const QString& projectDir, QWidget* parent)
    : QDialog(parent), project_dir_(projectDir) {
  setWindowTitle("Export transfer bundle");
  setModal(false);
  buildUi();
  poll_timer_ = new QTimer(this);
  poll_timer_->setInterval(120);
  connect(poll_timer_, &QTimer::timeout, this, &TransferExportDialog::poll);

  // Into exports/, never straight into the project directory itself — a zip
  // written alongside streams/ would recursively include itself on the next
  // export (zip_export() walks every regular file under lscan_dir).
  const QString base = QFileInfo(projectDir).completeBaseName();
  QDir().mkpath(QDir(projectDir).filePath("exports"));
  path_edit_->setText(QDir(projectDir).filePath(
      "exports/" + (base.isEmpty() ? QString("transfer") : base) + ".lscan.zip"));
}

TransferExportDialog::~TransferExportDialog() {
  if (running_.load()) cancel_token_.request_cancel();
  if (thread_.joinable()) thread_.join();
}

void TransferExportDialog::buildUi() {
  auto* v = new QVBoxLayout(this);
  v->addWidget(new QLabel(QString("Project: %1").arg(project_dir_)));

  auto* pathRow = new QWidget();
  auto* pl = new QHBoxLayout(pathRow);
  pl->setContentsMargins(0, 0, 0, 0);
  path_edit_ = new QLineEdit();
  auto* browse = new QPushButton("…");
  browse->setFixedWidth(32);
  connect(browse, &QPushButton::clicked, this, &TransferExportDialog::onBrowse);
  pl->addWidget(path_edit_, 1);
  pl->addWidget(browse);
  v->addWidget(new QLabel("Bundle (.lscan.zip)"));
  v->addWidget(pathRow);

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
  connect(export_button_, &QPushButton::clicked, this, &TransferExportDialog::onExport);
  connect(cancel_button_, &QPushButton::clicked, this, &TransferExportDialog::onCancel);
  connect(open_button_, &QPushButton::clicked, this, &TransferExportDialog::onOpenContaining);
  connect(close_button_, &QPushButton::clicked, this, &QDialog::close);
  bl->addWidget(export_button_);
  bl->addWidget(cancel_button_);
  bl->addWidget(open_button_);
  bl->addStretch(1);
  bl->addWidget(close_button_);
  v->addWidget(brow);

  resize(520, 220);
}

void TransferExportDialog::setZipPathForCli(const QString& path) { path_edit_->setText(path); }

void TransferExportDialog::onBrowse() {
  const QString p = QFileDialog::getSaveFileName(this, "Export transfer bundle", path_edit_->text(),
                                                  "LidarScan transfer bundle (*.lscan.zip)");
  if (!p.isEmpty()) path_edit_->setText(p);
}

void TransferExportDialog::onExport() {
  const QString path = path_edit_->text().trimmed();
  if (path.isEmpty() || project_dir_.isEmpty()) {
    status_->setText("Choose an output path first.");
    return;
  }
  QDir().mkpath(QFileInfo(path).absolutePath());

  out_path_ = path;
  progress_.store(0.0f);
  done_.store(false);
  running_.store(true);
  cancel_token_.reset();
  result_ = scanengine::Status();

  export_button_->setEnabled(false);
  cancel_button_->setEnabled(true);
  open_button_->setEnabled(false);
  progress_bar_->setValue(0);
  status_->setText("Exporting…");

  const std::string src = project_dir_.toStdString();
  const std::string dst = path.toStdString();
  std::atomic<float>* progressPtr = &progress_;
  thread_ = std::thread([this, src, dst, progressPtr] {
    const auto st = scanengine::lscan::zip_export(
        src, dst,
        [progressPtr](std::uint64_t doneBytes, std::uint64_t totalBytes, const char*) {
          if (totalBytes > 0) progressPtr->store(float(double(doneBytes) / double(totalBytes)));
        },
        &cancel_token_);
    result_ = st;
    done_.store(true);
  });
  poll_timer_->start();
}

void TransferExportDialog::onCancel() {
  if (!running_.load()) return;
  cancel_token_.request_cancel();
  cancel_button_->setEnabled(false);
  status_->setText("Cancelling…");
}

void TransferExportDialog::poll() {
  progress_bar_->setValue(int(progress_.load() * 100.0f));
  if (!done_.load()) return;
  poll_timer_->stop();
  if (thread_.joinable()) thread_.join();
  running_.store(false);

  export_button_->setEnabled(true);
  cancel_button_->setEnabled(false);

  if (result_.ok()) {
    const quint64 bytes = quint64(QFileInfo(out_path_).size());
    progress_bar_->setValue(100);
    status_->setText(QString("Export complete: %1 (%2)").arg(out_path_, humanBytes(bytes)));
    open_button_->setEnabled(true);
  } else if (result_.error() == scanengine::ScanError::kCancelled) {
    status_->setText("Export cancelled — the partial bundle was removed (record/zip.h's "
                     "documented behaviour: a half-written zip looks openable and is not).");
  } else {
    status_->setText(QString("Export failed: %1").arg(scanengine::error_str(result_.error())));
  }
}

void TransferExportDialog::onOpenContaining() {
  if (!out_path_.isEmpty()) revealInFileManager(out_path_);
}

// ============================================================================
// TransferImportDialog
// ============================================================================

TransferImportDialog::TransferImportDialog(const QString& zipPath, const QString& defaultDestDir,
                                           QWidget* parent)
    : QDialog(parent) {
  setWindowTitle("Import transfer bundle");
  setModal(false);
  buildUi();
  poll_timer_ = new QTimer(this);
  poll_timer_->setInterval(120);
  connect(poll_timer_, &QTimer::timeout, this, &TransferImportDialog::poll);

  if (!zipPath.isEmpty()) zip_edit_->setText(zipPath);
  const QString base = zipPath.isEmpty() ? QString("imported") : QFileInfo(zipPath).completeBaseName();
  const QString destRoot = defaultDestDir.isEmpty() ? QDir::homePath() : defaultDestDir;
  QString name = base;
  if (name.endsWith(".lscan")) name.chop(6);
  dest_edit_->setText(QDir(destRoot).filePath(name + ".lscan"));
}

TransferImportDialog::~TransferImportDialog() {
  if (running_.load()) cancel_token_.request_cancel();
  if (thread_.joinable()) thread_.join();
}

void TransferImportDialog::buildUi() {
  auto* v = new QVBoxLayout(this);

  auto* zipRow = new QWidget();
  auto* zl = new QHBoxLayout(zipRow);
  zl->setContentsMargins(0, 0, 0, 0);
  zip_edit_ = new QLineEdit();
  auto* zipBrowse = new QPushButton("…");
  zipBrowse->setFixedWidth(32);
  connect(zipBrowse, &QPushButton::clicked, this, &TransferImportDialog::onBrowseZip);
  zl->addWidget(zip_edit_, 1);
  zl->addWidget(zipBrowse);
  v->addWidget(new QLabel("Bundle (.lscan.zip)"));
  v->addWidget(zipRow);

  auto* destRow = new QWidget();
  auto* dl = new QHBoxLayout(destRow);
  dl->setContentsMargins(0, 0, 0, 0);
  dest_edit_ = new QLineEdit();
  auto* destBrowse = new QPushButton("…");
  destBrowse->setFixedWidth(32);
  connect(destBrowse, &QPushButton::clicked, this, &TransferImportDialog::onBrowseDest);
  dl->addWidget(dest_edit_, 1);
  dl->addWidget(destBrowse);
  v->addWidget(new QLabel("Destination .lscan directory (created fresh)"));
  v->addWidget(destRow);

  progress_bar_ = new QProgressBar();
  progress_bar_->setRange(0, 100);
  v->addWidget(progress_bar_);

  status_ = new QLabel();
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(status_);

  v->addWidget(new QLabel("Manifest sanity report (FileRecordReader, same as the Projects panel):"));
  report_tree_ = new QTreeWidget();
  report_tree_->setColumnCount(4);
  report_tree_->setHeaderLabels({"Stream", "Chunks", "Payload", "Span"});
  report_tree_->setRootIsDecorated(false);
  report_tree_->setMinimumHeight(110);
  v->addWidget(report_tree_);
  warnings_label_ = new QLabel();
  warnings_label_->setWordWrap(true);
  v->addWidget(warnings_label_);

  auto* brow = new QWidget();
  auto* bl = new QHBoxLayout(brow);
  bl->setContentsMargins(0, 0, 0, 0);
  import_button_ = new QPushButton("Import");
  cancel_button_ = new QPushButton("Cancel");
  open_lib_button_ = new QPushButton("Open in library");
  close_button_ = new QPushButton("Close");
  cancel_button_->setEnabled(false);
  open_lib_button_->setEnabled(false);
  connect(import_button_, &QPushButton::clicked, this, &TransferImportDialog::onImport);
  connect(cancel_button_, &QPushButton::clicked, this, &TransferImportDialog::onCancel);
  connect(open_lib_button_, &QPushButton::clicked, this, &TransferImportDialog::onOpenInLibrary);
  connect(close_button_, &QPushButton::clicked, this, &QDialog::close);
  bl->addWidget(import_button_);
  bl->addWidget(cancel_button_);
  bl->addWidget(open_lib_button_);
  bl->addStretch(1);
  bl->addWidget(close_button_);
  v->addWidget(brow);

  resize(600, 440);
}

void TransferImportDialog::setDestDirForCli(const QString& dir) { dest_edit_->setText(dir); }

void TransferImportDialog::onBrowseZip() {
  const QString p = QFileDialog::getOpenFileName(this, "Import transfer bundle", QDir::homePath(),
                                                 "LidarScan transfer bundle (*.lscan.zip *.zip)");
  if (!p.isEmpty()) zip_edit_->setText(p);
}

void TransferImportDialog::onBrowseDest() {
  const QString p = QFileDialog::getSaveFileName(this, "Import destination", dest_edit_->text(),
                                                 "LidarScan project (*.lscan)");
  if (!p.isEmpty()) dest_edit_->setText(p.endsWith(".lscan") ? p : p + ".lscan");
}

void TransferImportDialog::onImport() {
  const QString zipPath = zip_edit_->text().trimmed();
  QString destDir = dest_edit_->text().trimmed();
  if (zipPath.isEmpty() || destDir.isEmpty()) {
    status_->setText("Choose both a bundle and a destination directory first.");
    return;
  }
  if (!QFileInfo::exists(zipPath)) {
    status_->setText(QString("No such file: %1").arg(zipPath));
    return;
  }
  QDir().mkpath(QFileInfo(destDir).absolutePath());
  dest_dir_ = destDir;

  progress_.store(0.0f);
  done_.store(false);
  running_.store(true);
  cancel_token_.reset();
  result_ = scanengine::Status();
  open_lib_button_->setEnabled(false);
  report_tree_->clear();
  warnings_label_->clear();

  import_button_->setEnabled(false);
  cancel_button_->setEnabled(true);
  progress_bar_->setValue(0);
  status_->setText("Importing…");

  const std::string src = zipPath.toStdString();
  const std::string dst = destDir.toStdString();
  std::atomic<float>* progressPtr = &progress_;
  thread_ = std::thread([this, src, dst, progressPtr] {
    const auto st = scanengine::lscan::zip_import(
        src, dst,
        [progressPtr](std::uint64_t doneBytes, std::uint64_t totalBytes, const char*) {
          if (totalBytes > 0) progressPtr->store(float(double(doneBytes) / double(totalBytes)));
        },
        &cancel_token_);
    result_ = st;
    done_.store(true);
  });
  poll_timer_->start();
}

void TransferImportDialog::onCancel() {
  if (!running_.load()) return;
  cancel_token_.request_cancel();
  cancel_button_->setEnabled(false);
  status_->setText("Cancelling…");
}

void TransferImportDialog::poll() {
  progress_bar_->setValue(int(progress_.load() * 100.0f));
  if (!done_.load()) return;
  poll_timer_->stop();
  if (thread_.joinable()) thread_.join();
  running_.store(false);

  import_button_->setEnabled(true);
  cancel_button_->setEnabled(false);

  if (result_.ok()) {
    progress_bar_->setValue(100);
    // record/zip.h's own contract: cancelling an import LEAVES what it
    // extracted (unlike export, which removes a half-written archive) — so
    // this branch also fires for a bundle whose extraction completed but
    // whose caller cancelled between the last byte and the return, which
    // reads as success here because it is: the files are all there.
    status_->setText(QString("Extracted to %1 — reading manifest…").arg(dest_dir_));
    const ProjectInfo info = readProject(dest_dir_);
    showReport(info);
    open_lib_button_->setEnabled(info.valid);
  } else if (result_.error() == scanengine::ScanError::kCancelled) {
    status_->setText(QString("Import cancelled — whatever had already been extracted to %1 was "
                             "left in place (record/zip.h's documented import-cancel behaviour).")
                         .arg(dest_dir_));
  } else {
    status_->setText(QString("Import failed: %1").arg(scanengine::error_str(result_.error())));
  }
}

void TransferImportDialog::showReport(const ProjectInfo& info) {
  if (!info.valid) {
    status_->setText(QString("Extracted, but the result is not a readable project: %1").arg(info.error));
    return;
  }
  status_->setText(QString("%1 — manifest %2%3, profile %4, %5 chunks, %6, %7 s")
                       .arg(info.name,
                            info.manifest_present ? (info.manifest_ok ? "ok" : "corrupt") : "missing",
                            info.sealed ? "" : " (NOT SEALED)", info.profile.isEmpty() ? "-" : info.profile)
                       .arg(info.total_chunks)
                       .arg(humanBytes(info.total_bytes))
                       .arg(info.duration_s, 0, 'f', 2));
  for (const auto& s : info.streams) {
    auto* it = new QTreeWidgetItem(report_tree_);
    it->setText(0, s.name);
    it->setText(1, QString::number(s.chunks));
    it->setText(2, humanBytes(s.bytes));
    it->setText(3, QString("%1 s").arg(s.duration_s(), 0, 'f', 2));
  }
  for (int c = 0; c < 4; ++c) report_tree_->resizeColumnToContents(c);

  QStringList warn;
  if (info.truncated_tail_chunks) {
    warn << QString("%1 truncated-tail chunks").arg(info.truncated_tail_chunks);
  }
  if (info.crc_mismatch_chunks) warn << QString("%1 CRC mismatches").arg(info.crc_mismatch_chunks);
  if (info.unreadable_streams) warn << QString("%1 unreadable stream files").arg(info.unreadable_streams);
  warnings_label_->setText(warn.isEmpty() ? "no reader warnings — bundle looks sane" : warn.join(" · "));
}

void TransferImportDialog::onOpenInLibrary() {
  if (!dest_dir_.isEmpty()) Q_EMIT projectReady(dest_dir_);
}

}  // namespace lidarscan
