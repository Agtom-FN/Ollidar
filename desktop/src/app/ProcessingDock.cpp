#include "app/ProcessingDock.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <vector>

#include "app/EngineHost.h"
#include "app/QtHttpTransport.h"
#include "app/SyntheticMid360.h"
#include "scanengine/jobs/local_runner.h"
#include "scanengine/slam/post/post_pipeline.h"

namespace lidarscan {
namespace {

using scanengine::jobs::Job;
using scanengine::jobs::JobKind;
using scanengine::jobs::JobSpec;
using scanengine::jobs::JobState;

// Column layout for the job table.
enum Col { kColId = 0, kColKind, kColState, kColProgress, kColStage, kColMessage, kColActions, kColCount };

// The "fast" post-process preset engine/tests/test_post.cpp uses for its own
// synthetic-input case (fast_synth_config()) — smaller keyframes/voxels than
// A7's full-density production defaults, so an interactive UI run over a
// desktop-generated synthetic capture finishes in about a second rather than
// tuned for accuracy on a real 30-minute capture. Ported rather than shared:
// it is a test-local function in a file this task does not own.
scanengine::post::PostConfig desktopPostConfig() {
  scanengine::post::PostConfig cfg;
  cfg.keyframe_translation_m = 0.6;
  cfg.keyframe_rotation_deg = 12.0;
  cfg.keyframe_voxel_m = 0.25;
  cfg.max_points_per_keyframe = 6000;
  cfg.scan_context.min_index_gap = 8;
  cfg.scan_context.min_time_gap_s = 3.0;
  cfg.loop_submap_half_span = 4;
  cfg.dedup.voxel_size_m = 0.05;
  cfg.outlier.enabled = true;
  cfg.outlier.std_dev_mul = 2.0;
  cfg.progress_chunk_interval = 256;
  return cfg;
}

QString stateColor(JobState s) {
  switch (s) {
    case JobState::kDone: return "#2e7d32";
    case JobState::kFailed: return "#c62828";
    case JobState::kRunning: return "#1565c0";
    case JobState::kCancelling: return "#ef6c00";
    default: return "#616161";
  }
}

// A small modal form: label -> QLineEdit, built once per dialog rather than
// factored into a reusable widget — every call site here wants a different,
// small set of fields and OK/Cancel is all the chrome any of them need.
class SimpleFormDialog : public QDialog {
 public:
  SimpleFormDialog(const QString& title, QWidget* parent) : QDialog(parent) {
    setWindowTitle(title);
    layout_ = new QFormLayout(this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons_row_ = buttons;
  }
  void finishLayout() { layout_->addRow(buttons_row_); }
  QFormLayout* layout_ = nullptr;
  QDialogButtonBox* buttons_row_ = nullptr;
};

QWidget* pathRow(QLineEdit** editOut, const QString& initial, std::function<void(QLineEdit*)> browseFn) {
  auto* row = new QWidget();
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  auto* edit = new QLineEdit(initial);
  auto* btn = new QPushButton("…");
  btn->setFixedWidth(32);
  QObject::connect(btn, &QPushButton::clicked, row, [edit, browseFn] { browseFn(edit); });
  h->addWidget(edit, 1);
  h->addWidget(btn);
  *editOut = edit;
  return row;
}

}  // namespace

ProcessingDock::ProcessingDock(EngineHost* host, QWidget* parent)
    : QDockWidget("Processing", parent), host_(host) {
  setObjectName("ProcessingDock");
  cloud_transport_ = std::make_unique<QtHttpTransport>();
  buildUi();

  poll_timer_ = new QTimer(this);
  poll_timer_->setInterval(200);
  connect(poll_timer_, &QTimer::timeout, this, &ProcessingDock::refresh);
  poll_timer_->start();
}

ProcessingDock::~ProcessingDock() = default;

scanengine::jobs::JobQueue& ProcessingDock::queue() { return host_->engine()->jobs(); }

void ProcessingDock::buildUi() {
  auto* w = new QWidget();
  auto* v = new QVBoxLayout(w);

  auto* brow = new QWidget();
  auto* bl = new QHBoxLayout(brow);
  bl->setContentsMargins(0, 0, 0, 0);
  auto* postBtn = new QPushButton("Post-process…");
  auto* colorBtn = new QPushButton("Colorize…");
  auto* exportBtn = new QPushButton("Export…");
  auto* transferBtn = new QPushButton("Transfer bundle…");
  auto* cloudBtn = new QPushButton("Submit to cloud…");
  postBtn->setToolTip("Run A7's PostSlamPipeline on a .lscan directory (Mid-360 sessions).");
  colorBtn->setToolTip("Run A11's PointColorizer against a project with streams/frames/.");
  exportBtn->setToolTip("Export the result of a finished Post-process/Colorize job.");
  transferBtn->setToolTip("Package a .lscan project into a .lscan.zip via A5's zip_export().");
  cloudBtn->setToolTip("Upload a bundle to a cloud worker (Tech Spec §3.8). No server exists "
                       "yet in this environment, so this will fail — gracefully — with a "
                       "network error, which the row will show.");
  connect(postBtn, &QPushButton::clicked, this, &ProcessingDock::onPostProcess);
  connect(colorBtn, &QPushButton::clicked, this, &ProcessingDock::onColorize);
  connect(exportBtn, &QPushButton::clicked, this, &ProcessingDock::onExportChain);
  connect(transferBtn, &QPushButton::clicked, this, &ProcessingDock::onTransferBundle);
  connect(cloudBtn, &QPushButton::clicked, this, &ProcessingDock::onSubmitCloud);
  bl->addWidget(postBtn);
  bl->addWidget(colorBtn);
  bl->addWidget(exportBtn);
  bl->addWidget(transferBtn);
  bl->addWidget(cloudBtn);
  bl->addStretch(1);
  v->addWidget(brow);

  table_ = new QTableWidget(0, kColCount);
  table_->setHorizontalHeaderLabels({"ID", "Kind", "State", "Progress", "Stage", "Message", ""});
  table_->horizontalHeader()->setStretchLastSection(false);
  table_->horizontalHeader()->setSectionResizeMode(kColMessage, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(kColActions, QHeaderView::ResizeToContents);
  table_->verticalHeader()->setVisible(false);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setMinimumHeight(160);
  v->addWidget(table_, 1);

  setWidget(w);
  setMinimumWidth(420);
}

std::shared_ptr<scanengine::PageStore> ProcessingDock::currentStoreShared() const {
  if (!host_ || !host_->engine()) return nullptr;
  // Non-owning shared_ptr over the engine's own mutable PageStore — the same
  // "wrap, do not copy" pattern the store's own subscriber contract implies.
  // Safe: Engine::jobs() is destroyed (its worker joined) at the very start
  // of ~Engine, before the PageStore — see core/engine.h's own doc on
  // Engine::jobs() — so no job can still be touching this pointer once the
  // store it wraps is gone.
  scanengine::PageStore* raw = &host_->engine()->points();
  return std::shared_ptr<scanengine::PageStore>(raw, [](scanengine::PageStore*) {});
}

void ProcessingDock::refresh() {
  const auto jobs = queue().list();

  // Table rows track job ids 1:1 in submission order; rebuild is cheap at
  // interactive job counts (a handful to a few dozen per session) and keeps
  // this dock free of incremental-diff bookkeeping bugs.
  if (int(jobs.size()) != table_->rowCount()) {
    table_->setRowCount(int(jobs.size()));
  }
  row_of_job_.clear();
  for (int i = 0; i < int(jobs.size()); ++i) {
    row_of_job_[jobs[i].id] = i;
    rebuildRow(i, jobs[i]);
  }

  // A colorizer only needs to stay alive while its job is actually running;
  // once a job has settled (Done/Failed) the pipeline's own colorize() call
  // already returned, so it is safe to drop.
  for (auto it = colorizers_.begin(); it != colorizers_.end();) {
    bool stillLive = false;
    for (const auto& j : jobs) {
      if (j.id == it->first && (j.state == JobState::kQueued || j.state == JobState::kRunning ||
                                j.state == JobState::kCancelling)) {
        stillLive = true;
        break;
      }
    }
    it = stillLive ? std::next(it) : colorizers_.erase(it);
  }
}

void ProcessingDock::rebuildRow(int row, const Job& job) {
  auto setText = [&](int col, const QString& s) {
    auto* item = table_->item(row, col);
    if (!item) {
      item = new QTableWidgetItem();
      table_->setItem(row, col, item);
    }
    if (item->text() != s) item->setText(s);
  };
  setText(kColId, QString::number(job.id));
  setText(kColKind, scanengine::jobs::to_string(job.kind));
  auto* stateItem = table_->item(row, kColState);
  if (!stateItem) {
    stateItem = new QTableWidgetItem();
    table_->setItem(row, kColState, stateItem);
  }
  stateItem->setText(scanengine::jobs::to_string(job.state));
  stateItem->setForeground(QColor(stateColor(job.state)));

  auto* bar = qobject_cast<QProgressBar*>(table_->cellWidget(row, kColProgress));
  if (!bar) {
    bar = new QProgressBar();
    bar->setRange(0, 1000);
    bar->setTextVisible(true);
    table_->setCellWidget(row, kColProgress, bar);
  }
  bar->setValue(int(job.progress * 1000.0f));
  bar->setFormat(QString("%1%").arg(int(job.progress * 100.0f)));

  setText(kColStage, QString::fromStdString(job.stage));
  QString msg = QString::fromStdString(job.message);
  if (job.state == JobState::kFailed && job.error != scanengine::ScanError::kOk) {
    msg = QString("[%1] %2").arg(scanengine::error_str(job.error), msg);
  }
  setText(kColMessage, msg);

  auto* actions = qobject_cast<QWidget*>(table_->cellWidget(row, kColActions));
  if (!actions) {
    actions = new QWidget();
    auto* h = new QHBoxLayout(actions);
    h->setContentsMargins(2, 0, 2, 0);
    auto* cancel = new QPushButton("Cancel");
    cancel->setObjectName("cancel");
    auto* load = new QPushButton("Load result");
    load->setObjectName("load");
    h->addWidget(cancel);
    h->addWidget(load);
    table_->setCellWidget(row, kColActions, actions);
  }
  auto* cancelBtn = actions->findChild<QPushButton*>("cancel");
  auto* loadBtn = actions->findChild<QPushButton*>("load");
  const quint64 id = job.id;
  cancelBtn->disconnect();
  loadBtn->disconnect();
  cancelBtn->setEnabled(job.state == JobState::kQueued || job.state == JobState::kRunning);
  connect(cancelBtn, &QPushButton::clicked, this, [this, id] { onCancelJob(id); });
  const bool canLoad = job.kind == JobKind::kPostProcess && job.state == JobState::kDone;
  loadBtn->setVisible(job.kind == JobKind::kPostProcess);
  loadBtn->setEnabled(canLoad);
  connect(loadBtn, &QPushButton::clicked, this, [this, id] { onLoadResult(id); });
}

void ProcessingDock::onCancelJob(quint64 jobId) {
  const auto st = queue().cancel(jobId);
  if (!st.ok()) {
    Q_EMIT logLine(QString("cancel job #%1 failed: %2")
                       .arg(jobId)
                       .arg(scanengine::error_str(st.error())));
  }
  refresh();
}

void ProcessingDock::onLoadResult(quint64 jobId) {
  auto store = queue().produced_store(jobId);
  if (!store) {
    QMessageBox::warning(this, "Load result", "No produced PageStore for this job (not done, or not a Post-process job).");
    return;
  }
  Q_EMIT loadResultRequested(store, jobId);
  Q_EMIT logLine(QString("job #%1: result loaded into viewport (%2 points)")
                     .arg(jobId)
                     .arg(store->total_points()));
}

// --- Post-process -----------------------------------------------------------

void ProcessingDock::onPostProcess() {
  SimpleFormDialog dlg("Post-process", this);
  QLineEdit* dirEdit = nullptr;
  dlg.layout_->addRow(
      "Project (.lscan)",
      pathRow(&dirEdit, project_dir_, [&dlg](QLineEdit* e) {
        const QString d = QFileDialog::getExistingDirectory(&dlg, "Select .lscan project",
                                                             e->text().isEmpty() ? QDir::homePath()
                                                                                 : e->text());
        if (!d.isEmpty()) e->setText(d);
      }));
  auto* buildBtn = new QPushButton("Build a synthetic Mid-360 test project…");
  buildBtn->setToolTip(
      "Writes a real .lscan with real kMid360Points/kMid360Imu chunks from a ray-cast loop "
      "through a synthetic 24x18x3.5 m hall (engine/docs/A7-post.md's own recipe, ported — "
      "see NOTES.md), so Post-process has something real to run on with no hardware.");
  connect(buildBtn, &QPushButton::clicked, &dlg, [&dlg, dirEdit] {
    const QString dir = QFileDialog::getSaveFileName(&dlg, "New synthetic Mid-360 project",
                                                      QDir::homePath() + "/synthetic-mid360.lscan",
                                                      "LidarScan project (*.lscan)");
    if (dir.isEmpty()) return;
    const auto res = buildSyntheticMid360Project(dir.endsWith(".lscan") ? dir : dir + ".lscan");
    if (!res.ok) {
      QMessageBox::warning(&dlg, "Build synthetic project", res.error);
      return;
    }
    dirEdit->setText(dir.endsWith(".lscan") ? dir : dir + ".lscan");
    QMessageBox::information(&dlg, "Build synthetic project",
                             QString("Wrote %1 point packets (%2 points) + %3 IMU packets over %4 s.")
                                 .arg(res.point_packets)
                                 .arg(res.points)
                                 .arg(res.imu_packets)
                                 .arg(res.duration_s, 0, 'f', 1));
  });
  dlg.layout_->addRow(buildBtn);
  dlg.finishLayout();
  if (dlg.exec() != QDialog::Accepted) return;

  const QString dir = dirEdit->text().trimmed();
  if (dir.isEmpty()) {
    QMessageBox::warning(this, "Post-process", "Choose a .lscan project directory first.");
    return;
  }

  JobSpec spec;
  spec.kind = JobKind::kPostProcess;
  spec.post.lscan_dir = dir.toStdString();
  spec.post.config = desktopPostConfig();
  const auto sub = queue().submit(spec);
  if (!sub.ok()) {
    QMessageBox::warning(this, "Post-process", scanengine::error_str(sub.error()));
    return;
  }
  Q_EMIT logLine(QString("post-process job #%1 submitted for %2").arg(sub.value()).arg(dir));
  refresh();
}

// --- Colorize ----------------------------------------------------------------

void ProcessingDock::onColorize() {
  SimpleFormDialog dlg("Colorize", this);
  QLineEdit* dirEdit = nullptr;
  dlg.layout_->addRow(
      "Project (.lscan, needs streams/frames/)",
      pathRow(&dirEdit, project_dir_, [&dlg](QLineEdit* e) {
        const QString d = QFileDialog::getExistingDirectory(&dlg, "Select .lscan project",
                                                             e->text().isEmpty() ? QDir::homePath()
                                                                                 : e->text());
        if (!d.isEmpty()) e->setText(d);
      }));

  auto* source = new QComboBox();
  source->addItem("Currently loaded cloud (viewport)");
  const auto jobs = queue().list();
  for (const auto& j : jobs) {
    if ((j.kind == JobKind::kPostProcess || j.kind == JobKind::kColorize) &&
        j.state == JobState::kDone) {
      source->addItem(QString("Job #%1 (%2, done)")
                          .arg(j.id)
                          .arg(scanengine::jobs::to_string(j.kind)),
                      qulonglong(j.id));
    }
  }
  dlg.layout_->addRow("Colour these points", source);

  auto* syncCombo = new QComboBox();
  syncCombo->addItem("Assume good sync (<=5 ms)", int(scanengine::SyncQuality::kGood));
  syncCombo->addItem("Assume gated sync (<=15 ms)", int(scanengine::SyncQuality::kGated));
  syncCombo->setCurrentIndex(0);
  syncCombo->setToolTip(
      "The desktop has no A4 TimeSync wiring for a camera+lidar capture (that pairing is "
      "Android-only per §3.5), so there is no measured jitter to gate on here. This picks "
      "the ColorizeConfig::sync_quality policy_for() would otherwise derive from A4 — see "
      "NOTES.md.");
  dlg.layout_->addRow("Sync quality", syncCombo);
  dlg.finishLayout();
  if (dlg.exec() != QDialog::Accepted) return;

  const QString dir = dirEdit->text().trimmed();
  if (dir.isEmpty()) {
    QMessageBox::warning(this, "Colorize", "Choose a .lscan project directory first.");
    return;
  }

  scanengine::color::ColorizeConfig cfg;
  cfg.sync_quality = static_cast<scanengine::SyncQuality>(syncCombo->currentData().toInt());
  auto colorizer = std::make_shared<scanengine::color::PointColorizer>(cfg);

  JobSpec spec;
  spec.kind = JobKind::kColorize;
  spec.colorize.colorizer = colorizer.get();
  spec.colorize.lscan_dir = dir.toStdString();  // load_keyframes() source
  if (source->currentIndex() == 0) {
    spec.colorize.store = currentStoreShared();
    if (!spec.colorize.store) {
      QMessageBox::warning(this, "Colorize", "No engine PageStore available.");
      return;
    }
  } else {
    spec.colorize.chain_from = source->currentData().toULongLong();
  }

  const auto sub = queue().submit(spec);
  if (!sub.ok()) {
    QMessageBox::warning(this, "Colorize", scanengine::error_str(sub.error()));
    return;
  }
  colorizers_[sub.value()] = colorizer;
  Q_EMIT logLine(QString("colorize job #%1 submitted for %2").arg(sub.value()).arg(dir));
  refresh();
}

// --- Export (chains from a finished job) -------------------------------------

void ProcessingDock::onExportChain() {
  std::vector<Job> candidates;
  for (const auto& j : queue().list()) {
    if ((j.kind == JobKind::kPostProcess || j.kind == JobKind::kColorize) &&
        j.state == JobState::kDone) {
      candidates.push_back(j);
    }
  }
  if (candidates.empty()) {
    QMessageBox::information(this, "Export",
                             "No finished Post-process/Colorize job to chain from yet. Run one "
                             "first, or use File -> Export… for the live viewport cloud.");
    return;
  }

  SimpleFormDialog dlg("Export (chained)", this);
  auto* srcCombo = new QComboBox();
  for (const auto& j : candidates) {
    srcCombo->addItem(QString("Job #%1 (%2, done)").arg(j.id).arg(scanengine::jobs::to_string(j.kind)),
                      qulonglong(j.id));
  }
  dlg.layout_->addRow("Source job", srcCombo);
  auto* fmt = new QComboBox();
  fmt->addItem("PLY (binary)", int(scanengine::ExportFormat::kPlyBinary));
  fmt->addItem("LAS 1.4", int(scanengine::ExportFormat::kLas14));
  fmt->addItem("PCD (binary)", int(scanengine::ExportFormat::kPcd));
  dlg.layout_->addRow("Format", fmt);
  QLineEdit* pathEdit = nullptr;
  dlg.layout_->addRow("Output file",
                      pathRow(&pathEdit, project_dir_ + "/exports/post-result.ply",
                              [&dlg](QLineEdit* e) {
                                const QString p =
                                    QFileDialog::getSaveFileName(&dlg, "Export destination", e->text());
                                if (!p.isEmpty()) e->setText(p);
                              }));
  dlg.finishLayout();
  if (dlg.exec() != QDialog::Accepted) return;

  const QString path = pathEdit->text().trimmed();
  if (path.isEmpty()) {
    QMessageBox::warning(this, "Export", "Choose an output file first.");
    return;
  }
  QDir().mkpath(QFileInfo(path).absolutePath());

  JobSpec spec;
  spec.kind = JobKind::kExportPoints;
  spec.export_points.chain_from = srcCombo->currentData().toULongLong();
  spec.export_points.format = static_cast<scanengine::ExportFormat>(fmt->currentData().toInt());
  spec.export_points.output_path = path.toStdString();
  const auto sub = queue().submit(spec);
  if (!sub.ok()) {
    QMessageBox::warning(this, "Export", scanengine::error_str(sub.error()));
    return;
  }
  Q_EMIT logLine(QString("export job #%1 submitted (chained from #%2) -> %3")
                     .arg(sub.value())
                     .arg(spec.export_points.chain_from)
                     .arg(path));
  refresh();
}

// --- Transfer bundle ----------------------------------------------------------

void ProcessingDock::onTransferBundle() {
  SimpleFormDialog dlg("Transfer bundle…", this);
  QLineEdit* dirEdit = nullptr;
  dlg.layout_->addRow(
      "Project (.lscan)",
      pathRow(&dirEdit, project_dir_, [&dlg](QLineEdit* e) {
        const QString d = QFileDialog::getExistingDirectory(&dlg, "Select .lscan project",
                                                             e->text().isEmpty() ? QDir::homePath()
                                                                                 : e->text());
        if (!d.isEmpty()) e->setText(d);
      }));
  QLineEdit* zipEdit = nullptr;
  const QString defaultZip =
      (project_dir_.isEmpty() ? QDir::homePath() + "/project" : project_dir_) + ".lscan.zip";
  dlg.layout_->addRow("Destination .zip",
                      pathRow(&zipEdit, defaultZip, [&dlg](QLineEdit* e) {
                        const QString p = QFileDialog::getSaveFileName(&dlg, "Transfer bundle destination",
                                                                       e->text(), "Zip (*.zip)");
                        if (!p.isEmpty()) e->setText(p);
                      }));
  auto* includeResults = new QCheckBox("Include processed/ results (uncheck for raw-only transfer)");
  includeResults->setChecked(true);
  dlg.layout_->addRow(includeResults);
  dlg.finishLayout();
  if (dlg.exec() != QDialog::Accepted) return;

  const QString dir = dirEdit->text().trimmed();
  const QString zip = zipEdit->text().trimmed();
  if (dir.isEmpty() || zip.isEmpty()) {
    QMessageBox::warning(this, "Transfer bundle", "Choose both a project directory and a destination zip.");
    return;
  }
  QDir().mkpath(QFileInfo(zip).absolutePath());

  JobSpec spec;
  spec.kind = JobKind::kTransferExport;
  spec.transfer.project_dir = dir.toStdString();
  spec.transfer.zip_path = zip.toStdString();
  spec.transfer.include_results = includeResults->isChecked();
  const auto sub = queue().submit(spec);
  if (!sub.ok()) {
    QMessageBox::warning(this, "Transfer bundle", scanengine::error_str(sub.error()));
    return;
  }
  Q_EMIT logLine(QString("transfer job #%1 submitted: %2 -> %3").arg(sub.value()).arg(dir, zip));
  refresh();
}

// --- Submit to cloud -----------------------------------------------------------

void ProcessingDock::onSubmitCloud() {
  QSettings settings;
  SimpleFormDialog dlg("Submit to cloud…", this);
  auto* urlEdit = new QLineEdit(settings.value("cloud/baseUrl", "https://cloud.lidarscan.example/v1").toString());
  dlg.layout_->addRow("Server URL", urlEdit);
  auto* tokenEdit = new QLineEdit(settings.value("cloud/token").toString());
  tokenEdit->setEchoMode(QLineEdit::Password);
  dlg.layout_->addRow("Token", tokenEdit);

  QLineEdit* zipEdit = nullptr;
  dlg.layout_->addRow(".lscan.zip to upload",
                      pathRow(&zipEdit, QString(), [&dlg](QLineEdit* e) {
                        const QString p = QFileDialog::getOpenFileName(&dlg, "Bundle to upload",
                                                                       QDir::homePath(), "Zip (*.zip)");
                        if (!p.isEmpty()) e->setText(p);
                      }));
  auto* chainCombo = new QComboBox();
  chainCombo->addItem("(use the path above)", qulonglong(0));
  for (const auto& j : queue().list()) {
    if (j.kind == JobKind::kTransferExport && j.state == JobState::kDone) {
      chainCombo->addItem(QString("Job #%1 (Transfer bundle, done)").arg(j.id), qulonglong(j.id));
    }
  }
  dlg.layout_->addRow("Or chain from", chainCombo);

  QLineEdit* resultDirEdit = nullptr;
  const QString defaultResult =
      (project_dir_.isEmpty() ? QDir::homePath() : project_dir_) + "/cloud-result";
  dlg.layout_->addRow("Result directory",
                      pathRow(&resultDirEdit, defaultResult, [&dlg](QLineEdit* e) {
                        const QString d = QFileDialog::getExistingDirectory(&dlg, "Result directory", e->text());
                        if (!d.isEmpty()) e->setText(d);
                      }));

  auto* note = new QLabel(
      "No cloud server exists in this environment — this will attempt a real network "
      "connection and fail (connection refused / DNS / timeout). The job row will show "
      "the failure rather than the app crashing or hanging.");
  note->setWordWrap(true);
  dlg.layout_->addRow(note);
  dlg.finishLayout();
  if (dlg.exec() != QDialog::Accepted) return;

  const QString url = urlEdit->text().trimmed();
  const QString token = tokenEdit->text();
  settings.setValue("cloud/baseUrl", url);
  settings.setValue("cloud/token", token);

  const quint64 chainFrom = chainCombo->currentData().toULongLong();
  const QString zip = zipEdit->text().trimmed();
  if (chainFrom == 0 && zip.isEmpty()) {
    QMessageBox::warning(this, "Submit to cloud", "Choose a bundle to upload, or a Transfer job to chain from.");
    return;
  }
  QDir().mkpath(resultDirEdit->text());

  JobSpec spec;
  spec.kind = JobKind::kCloudSubmit;
  spec.cloud.transport = cloud_transport_.get();
  spec.cloud.cloud_config.base_url = url.toStdString();
  spec.cloud.cloud_config.auth_token = token.toStdString();
  spec.cloud.local_zip_path = zip.toStdString();
  spec.cloud.chain_from = chainFrom;
  spec.cloud.result_dir = resultDirEdit->text().toStdString();
  const auto sub = queue().submit(spec);
  if (!sub.ok()) {
    QMessageBox::warning(this, "Submit to cloud", scanengine::error_str(sub.error()));
    return;
  }
  Q_EMIT logLine(QString("cloud submit job #%1 submitted to %2").arg(sub.value()).arg(url));
  refresh();
}

}  // namespace lidarscan
