#include "app/MergeDock.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "app/MergeFixture.h"
#include "app/MergeSessionLoader.h"
#include "render/ViewportWindow.h"
#include "scanengine/poses/se3.h"

namespace lidarscan {
namespace {

using scanengine::merge::AlignSource;
using scanengine::merge::MergeProject;
using scanengine::merge::MergeReport;
using scanengine::merge::to_string;

QString fmtMm(double m) { return QString::number(m * 1000.0, 'f', 3) + " mm"; }
QString fmtPct(double frac) { return QString::number(frac * 100.0, 'f', 1) + "%"; }

// A small, deterministic, high-contrast palette for "colour by session" —
// distinct hues spaced around the wheel rather than anything derived from the
// session's real intensity/colour, so the run-table boundaries (A13-merge.md
// §6's "one contiguous run per session") are unmistakable in the viewport.
std::array<std::uint8_t, 3> sessionColor(std::uint32_t sessionIndex) {
  static const std::uint8_t kPalette[8][3] = {
      {230, 60, 60},  {60, 160, 230}, {90, 200, 90},  {230, 170, 40},
      {170, 90, 220}, {40, 200, 190}, {230, 90, 170}, {150, 150, 60},
  };
  const auto& c = kPalette[sessionIndex % 8];
  return {c[0], c[1], c[2]};
}

}  // namespace

// ============================================================================
// MergeIcpChart
// ============================================================================

MergeIcpChart::MergeIcpChart(QWidget* parent) : QWidget(parent) {
  setMinimumHeight(140);
}

void MergeIcpChart::setTrace(const std::vector<scanengine::merge::IcpIteration>& trace,
                             const QString& title) {
  trace_ = trace;
  title_ = title;
  update();
}

void MergeIcpChart::clearTrace() {
  trace_.clear();
  title_.clear();
  update();
}

void MergeIcpChart::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.fillRect(rect(), palette().base());

  const int margin = 30;
  const QRectF plot(margin, 18, width() - margin - 10, height() - margin - 18);
  p.setPen(palette().text().color());
  p.drawText(QRectF(4, 2, width() - 8, 16), Qt::AlignLeft, title_.isEmpty() ? "no pair selected" : title_);

  if (trace_.empty() || plot.width() <= 1 || plot.height() <= 1) return;

  double maxRms = 1e-9;
  for (const auto& it : trace_) maxRms = std::max(maxRms, it.rms_m);
  maxRms *= 1.08;  // headroom so the first point is not clipped at the top

  auto toPoint = [&](std::size_t i, double rms) {
    const double x = plot.left() + (trace_.size() <= 1
                                        ? 0.0
                                        : plot.width() * double(i) / double(trace_.size() - 1));
    const double y = plot.bottom() - plot.height() * (rms / maxRms);
    return QPointF(x, y);
  };

  // gridlines + y-axis labels (0, half, max, in mm)
  p.setPen(QPen(palette().mid().color(), 1, Qt::DotLine));
  for (int i = 0; i <= 2; ++i) {
    const double y = plot.top() + plot.height() * (1.0 - double(i) / 2.0);
    p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    p.setPen(palette().text().color());
    p.drawText(QRectF(0, y - 8, margin - 4, 16), Qt::AlignRight | Qt::AlignVCenter,
               QString::number(maxRms * 1000.0 * double(i) / 2.0, 'f', 0));
    p.setPen(QPen(palette().mid().color(), 1, Qt::DotLine));
  }

  // shade the coarse-vs-fine correspondence-gate stage boundary, if any: the
  // step where gate_m changes is exactly the "step down" A13-merge.md §5 says
  // "must not look like a bug" — marking it is the point.
  for (std::size_t i = 1; i < trace_.size(); ++i) {
    if (trace_[i].gate_m != trace_[i - 1].gate_m) {
      const QPointF top = toPoint(i, maxRms);
      p.setPen(QPen(QColor(200, 120, 40), 1, Qt::DashLine));
      p.drawLine(QPointF(top.x(), plot.top()), QPointF(top.x(), plot.bottom()));
    }
  }

  QPolygonF poly;
  for (std::size_t i = 0; i < trace_.size(); ++i) poly << toPoint(i, trace_[i].rms_m);
  p.setPen(QPen(QColor(60, 140, 220), 2));
  p.drawPolyline(poly);
  p.setBrush(QColor(60, 140, 220));
  p.setPen(Qt::NoPen);
  for (const QPointF& pt : poly) p.drawEllipse(pt, 2.5, 2.5);

  p.setPen(palette().text().color());
  p.drawText(QRectF(plot.left(), plot.bottom() + 2, plot.width(), 14), Qt::AlignLeft,
             QString("it 0 (%1 mm)").arg(trace_.front().rms_m * 1000.0, 0, 'f', 1));
  p.drawText(QRectF(plot.left(), plot.bottom() + 2, plot.width(), 14), Qt::AlignRight,
             QString("it %1 (%2 mm)").arg(trace_.size() - 1).arg(trace_.back().rms_m * 1000.0, 0, 'f', 1));
}

// ============================================================================
// MergeDock::SessionRow
// ============================================================================

scanengine::PageStore* MergeDock::SessionRow::store() {
  if (engine) return &engine->points();
  return ownStore.get();
}

// ============================================================================
// MergeDock
// ============================================================================

MergeDock::MergeDock(ViewportWindow* viewport, QWidget* parent)
    : QDockWidget("Merge workbench", parent), viewport_(viewport) {
  project_ = std::make_unique<MergeProject>();
  buildUi();
  if (viewport_) {
    connect(viewport_, &ViewportWindow::measurementsChanged, this,
            &MergeDock::onViewportMeasurementsChanged);
  }
}

MergeDock::~MergeDock() = default;

void MergeDock::buildUi() {
  auto* scroll = new QScrollArea();
  scroll->setWidgetResizable(true);
  auto* root = new QWidget();
  auto* rootLayout = new QVBoxLayout(root);

  // --- sessions -------------------------------------------------------------
  auto* sessBox = new QGroupBox("Sessions");
  auto* sv = new QVBoxLayout(sessBox);
  session_table_ = new QTableWidget(0, 6);
  session_table_->setHorizontalHeaderLabels(
      {"Provenance", "Points", "Georef", "Align", "Anchor", "Kept/Dropped"});
  session_table_->horizontalHeader()->setStretchLastSection(true);
  session_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  session_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  session_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  session_table_->setMinimumHeight(120);
  connect(session_table_, &QTableWidget::itemSelectionChanged, this,
          &MergeDock::onSessionTableSelectionChanged);
  sv->addWidget(session_table_);

  auto* sessBtnRow1 = new QWidget();
  auto* sb1 = new QHBoxLayout(sessBtnRow1);
  sb1->setContentsMargins(0, 0, 0, 0);
  auto* addOpenBtn = new QPushButton("Add from open project");
  auto* importBtn = new QPushButton("Import .lscan project…");
  auto* fixtureBtn = new QPushButton("Build synthetic fixture (evidence)");
  connect(addOpenBtn, &QPushButton::clicked, this, &MergeDock::onAddFromOpenProject);
  connect(importBtn, &QPushButton::clicked, this, &MergeDock::onImportProject);
  connect(fixtureBtn, &QPushButton::clicked, this, &MergeDock::onBuildFixture);
  sb1->addWidget(addOpenBtn);
  sb1->addWidget(importBtn);
  sb1->addWidget(fixtureBtn);
  sv->addWidget(sessBtnRow1);

  auto* sessBtnRow2 = new QWidget();
  auto* sb2 = new QHBoxLayout(sessBtnRow2);
  sb2->setContentsMargins(0, 0, 0, 0);
  set_anchor_btn_ = new QPushButton("Set as anchor");
  remove_btn_ = new QPushButton("Remove selected");
  connect(set_anchor_btn_, &QPushButton::clicked, this, &MergeDock::onSetAnchor);
  connect(remove_btn_, &QPushButton::clicked, this, &MergeDock::onRemoveSelected);
  sb2->addWidget(set_anchor_btn_);
  sb2->addWidget(remove_btn_);
  sb2->addStretch(1);
  sv->addWidget(sessBtnRow2);
  rootLayout->addWidget(sessBox);

  // --- coarse alignment -------------------------------------------------
  auto* alignBox = new QGroupBox("Coarse alignment (§3.10)");
  auto* av = new QVBoxLayout(alignBox);
  auto* af = new QFormLayout();
  align_session_combo_ = new QComboBox();
  af->addRow("Session to align", align_session_combo_);
  av->addLayout(af);

  auto* alignRow1 = new QWidget();
  auto* ar1 = new QHBoxLayout(alignRow1);
  ar1->setContentsMargins(0, 0, 0, 0);
  georef_btn_ = new QPushButton("Auto (georeferenced)");
  georef_btn_->setToolTip(
      "Composes every georeferenced session through its own ENU frame into the anchor's "
      "(align.h::enu_from_enu) — exact, per docs/A13-merge.md §3. Ignores the 'session to "
      "align' picker: it aligns every eligible session at once.");
  connect(georef_btn_, &QPushButton::clicked, this, &MergeDock::onAlignGeoref);
  ar1->addWidget(georef_btn_);
  av->addWidget(alignRow1);

  auto* alignRow2 = new QWidget();
  auto* ar2 = new QHBoxLayout(alignRow2);
  ar2->setContentsMargins(0, 0, 0, 0);
  pick_btn_ = new QPushButton("3-point manual…");
  pick_btn_->setToolTip(
      "Sequential picking, reusing the C3 measure tool: click a feature in the SOURCE "
      "session's cloud, then the matching feature in the TARGET (anchor) cloud, three "
      "times. Same mechanism as a two-click measure segment — see MergeDock.h.");
  pick_cancel_btn_ = new QPushButton("Cancel pick");
  pick_cancel_btn_->setEnabled(false);
  connect(pick_btn_, &QPushButton::clicked, this, &MergeDock::onStartPick);
  connect(pick_cancel_btn_, &QPushButton::clicked, this, &MergeDock::onCancelPick);
  ar2->addWidget(pick_btn_);
  ar2->addWidget(pick_cancel_btn_);
  av->addWidget(alignRow2);

  auto* yawRow = new QWidget();
  auto* yr = new QHBoxLayout(yawRow);
  yr->setContentsMargins(0, 0, 0, 0);
  yaw_reference_combo_ = new QComboBox();
  yaw_btn_ = new QPushButton("Yaw search vs reference");
  yaw_btn_->setToolTip(
      "The Manhattan fallback (align.h::yaw_translation_search). Applies only when it "
      "clears its own overlap/margin gates; an ambiguous or low-overlap result is reported, "
      "not applied — see docs/A13-merge.md §5 for exactly where this is known to be "
      "confidently wrong (an extruded/repetitive building at partial overlap).");
  connect(yaw_btn_, &QPushButton::clicked, this, &MergeDock::onYawSearch);
  yr->addWidget(new QLabel("Reference:"));
  yr->addWidget(yaw_reference_combo_, 1);
  yr->addWidget(yaw_btn_);
  av->addWidget(yawRow);

  align_status_ = new QLabel("no alignment attempted yet");
  align_status_->setWordWrap(true);
  align_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  av->addWidget(align_status_);
  rootLayout->addWidget(alignBox);

  // --- refine -----------------------------------------------------------
  auto* refineBox = new QGroupBox("Refine (voxel-downsampled point-to-plane ICP)");
  auto* rv = new QVBoxLayout(refineBox);
  auto* refineRow = new QWidget();
  auto* rr = new QHBoxLayout(refineRow);
  rr->setContentsMargins(0, 0, 0, 0);
  survey_btn_ = new QPushButton("Survey overlap");
  survey_btn_->setToolTip("Overlap only, at current alignment — what a workbench shows before "
                          "pressing align (icp.h::estimate_overlap).");
  refine_btn_ = new QPushButton("Refine");
  connect(survey_btn_, &QPushButton::clicked, this, &MergeDock::onSurveyOverlap);
  connect(refine_btn_, &QPushButton::clicked, this, &MergeDock::onRefine);
  rr->addWidget(survey_btn_);
  rr->addWidget(refine_btn_);
  rv->addWidget(refineRow);
  refine_status_ = new QLabel("not refined yet");
  refine_status_->setWordWrap(true);
  rv->addWidget(refine_status_);
  rootLayout->addWidget(refineBox);

  // --- pairs + chart ------------------------------------------------------
  auto* pairBox = new QGroupBox("Pairs (MergeReport — click a row for its residual trace)");
  auto* pv = new QVBoxLayout(pairBox);
  pair_table_ = new QTableWidget(0, 8);
  pair_table_->setHorizontalHeaderLabels(
      {"A", "B", "RMS before", "RMS after", "Ovl A→B", "Ovl B→A", "Iters", "Status"});
  pair_table_->horizontalHeader()->setStretchLastSection(true);
  pair_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  pair_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  pair_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  pair_table_->setMinimumHeight(110);
  connect(pair_table_, &QTableWidget::itemSelectionChanged, this,
          &MergeDock::onPairTableSelectionChanged);
  pv->addWidget(pair_table_);
  chart_ = new MergeIcpChart();
  pv->addWidget(chart_);
  rootLayout->addWidget(pairBox);

  // --- build + export -----------------------------------------------------
  auto* buildBox = new QGroupBox("Build merged cloud");
  auto* bv = new QVBoxLayout(buildBox);
  auto* bf = new QFormLayout();
  color_by_session_ = new QCheckBox("Colour by session (run-table ranges → distinct colours)");
  color_by_session_->setChecked(true);
  bf->addRow(color_by_session_);
  dedup_voxel_ = new QDoubleSpinBox();
  dedup_voxel_->setRange(0.0, 5.0);
  dedup_voxel_->setDecimals(3);
  dedup_voxel_->setSingleStep(0.01);
  dedup_voxel_->setValue(0.03);
  dedup_voxel_->setSuffix(" m");
  bf->addRow("Dedup voxel", dedup_voxel_);
  bv->addLayout(bf);
  auto* buildRow = new QWidget();
  auto* br = new QHBoxLayout(buildRow);
  br->setContentsMargins(0, 0, 0, 0);
  build_btn_ = new QPushButton("Build merged cloud → viewport");
  export_btn_ = new QPushButton("Export merged…");
  export_btn_->setEnabled(false);
  connect(build_btn_, &QPushButton::clicked, this, &MergeDock::onBuild);
  connect(export_btn_, &QPushButton::clicked, this, &MergeDock::onExport);
  br->addWidget(build_btn_);
  br->addWidget(export_btn_);
  bv->addWidget(buildRow);
  build_status_ = new QLabel("not built yet");
  build_status_->setWordWrap(true);
  bv->addWidget(build_status_);
  rootLayout->addWidget(buildBox);

  rootLayout->addStretch(1);
  scroll->setWidget(root);
  setWidget(scroll);
  setMinimumWidth(420);
}

void MergeDock::logAndStatus(const QString& s) {
  Q_EMIT logLine(QString("[merge] %1").arg(s));
}

// ---------------------------------------------------------------------------
// session management
// ---------------------------------------------------------------------------

int MergeDock::addSession(const QString& provenanceId, const QString& sourceDir,
                          std::unique_ptr<scanengine::Engine> engine,
                          const std::vector<scanengine::PointVertex>* syntheticPoints,
                          const scanengine::merge::SessionGeoref* georef) {
  SessionRow row;
  row.provenanceId = provenanceId;
  row.sourceDir = sourceDir;
  scanengine::StreamId stream = scanengine::StreamId::kSlamMap;
  if (engine) {
    row.engine = std::move(engine);
    stream = scanengine::StreamId::kLidarD6;
  } else if (syntheticPoints) {
    row.ownStore = std::make_unique<scanengine::PageStore>();
    const scanengine::Span<const scanengine::PointVertex> span(syntheticPoints->data(),
                                                                syntheticPoints->size());
    quint32 appended = 0;
    const auto ast = row.ownStore->append(scanengine::StreamId::kSlamMap, span, 0, &appended);
    if (!ast.ok()) {
      logAndStatus(QString("session '%1': PageStore append failed: %2")
                       .arg(provenanceId, scanengine::error_str(ast.error())));
      return -1;
    }
  } else {
    logAndStatus("addSession: no cloud source given");
    return -1;
  }
  if (georef) {
    row.hasGeoref = true;
    row.georef = *georef;
  }

  scanengine::merge::SessionInput in;
  in.provenance_id = provenanceId.toStdString();
  in.lscan_dir = sourceDir.toStdString();
  const auto cst = scanengine::merge::collect_pages(*row.store(), stream, &in.cloud);
  if (!cst.ok() || in.cloud.empty()) {
    logAndStatus(QString("session '%1': no points collected (%2)")
                     .arg(provenanceId,
                          cst.ok() ? QString("empty cloud")
                                   : QString(scanengine::error_str(cst.error()))));
    return -1;
  }
  if (row.hasGeoref) in.georef = row.georef;

  const auto res = project_->add_session(in);
  if (!res.ok()) {
    logAndStatus(
        QString("add_session('%1') failed: %2").arg(provenanceId, scanengine::error_str(res.error())));
    return -1;
  }
  row.mergeId = res.value();
  row.inProject = true;
  const quint64 points = in.cloud.point_count();
  sessions_.push_back(std::move(row));
  refreshSessionTable();
  logAndStatus(QString("session '%1' added (id %2): %3 points%4")
                   .arg(provenanceId)
                   .arg(res.value())
                   .arg(points)
                   .arg(sessions_.back().hasGeoref ? " (georeferenced)" : ""));
  return int(sessions_.size()) - 1;
}

bool MergeDock::addFromProject(const QString& lscanDir, const QString& provenanceIdHint, QString* err) {
  if (lscanDir.isEmpty()) {
    if (err) *err = "no directory given";
    return false;
  }
  MergeSessionLoader loader;
  QString loadErr;
  if (!loader.load(lscanDir, &loadErr)) {
    if (err) *err = loadErr;
    return false;
  }
  const QString provenanceId =
      provenanceIdHint.isEmpty() ? QFileInfo(lscanDir).fileName() : provenanceIdHint;
  const quint64 decoded = loader.pointsDecoded();
  const int row = addSession(provenanceId, lscanDir, loader.takeEngine(), nullptr, nullptr);
  if (row < 0) {
    if (err) *err = QString("session '%1': loaded %2 points via replay but MergeProject rejected "
                            "it (see the log)")
                        .arg(provenanceId)
                        .arg(decoded);
    return false;
  }
  return true;
}

int MergeDock::addFixtureSessionsForCli(bool withGeoref) {
  MergeFixture fixture;
  const auto sessions = fixture.sessions(withGeoref);
  int added = 0;
  for (const auto& s : sessions) {
    const scanengine::merge::SessionGeoref* g = s.has_georef ? &s.georef : nullptr;
    if (addSession(s.provenanceId, QString(), nullptr, &s.cloud, g) >= 0) ++added;
  }
  return added;
}

bool MergeDock::addProjectSessionForCli(const QString& lscanDir, const QString& provenanceId,
                                        QString* err) {
  return addFromProject(lscanDir, provenanceId, err);
}

void MergeDock::onAddFromOpenProject() {
  if (open_project_dir_.isEmpty()) {
    QMessageBox::information(this, "Merge", "No project is currently open.");
    return;
  }
  QString err;
  if (!addFromProject(open_project_dir_, QString(), &err)) {
    QMessageBox::warning(this, "Merge — add session", err);
  }
}

void MergeDock::onImportProject() {
  const QString dir = QFileDialog::getExistingDirectory(this, "Import .lscan project for merge");
  if (dir.isEmpty()) return;
  QString err;
  if (!addFromProject(dir, QString(), &err)) {
    QMessageBox::warning(this, "Merge — import session", err);
  }
}

void MergeDock::onBuildFixture() {
  const int n = addFixtureSessionsForCli(true);
  logAndStatus(QString("synthetic fixture: %1 session(s) added (see engine/tests/test_merge.cpp — "
                       "30x12x3 m building, sessions 0-1 and 1-2 overlap 4 m, 0-2 share nothing)")
                   .arg(n));
}

void MergeDock::onRemoveSelected() {
  QMessageBox::information(this, "Merge",
                           "Sessions cannot be removed from an in-progress MergeProject "
                           "(no such API — merge/merge.h has no remove_session()). Close and "
                           "reopen the Merge dock to start over.");
}

void MergeDock::onSetAnchor() {
  const int r = session_table_->currentRow();
  if (r < 0 || r >= int(sessions_.size())) return;
  const auto st = project_->set_anchor(sessions_[r].mergeId);
  if (!st.ok()) {
    QMessageBox::warning(this, "Set anchor", scanengine::error_str(st.error()));
    return;
  }
  refreshSessionTable();
  logAndStatus(QString("session '%1' set as anchor").arg(sessions_[r].provenanceId));
}

void MergeDock::onSessionTableSelectionChanged() {}
void MergeDock::onPairTableSelectionChanged() { refreshChartFor(pair_table_->currentRow()); }

void MergeDock::refreshSessionTable() {
  session_table_->setRowCount(int(sessions_.size()));
  align_session_combo_->clear();
  yaw_reference_combo_->clear();
  const MergeReport* rep = reportOrNull();
  for (int i = 0; i < int(sessions_.size()); ++i) {
    const auto& row = sessions_[i];
    const auto& sess = project_->session(row.mergeId);

    auto* pItem = new QTableWidgetItem(row.provenanceId);
    session_table_->setItem(i, 0, pItem);
    session_table_->setItem(i, 1, new QTableWidgetItem(QString::number(sess.point_count())));

    QString georefText = "-";
    if (row.hasGeoref) {
      georefText = QString("GEOREF ±%1 m").arg(sess.georef.solution.horizontal_sigma_m, 0, 'f', 3);
    }
    session_table_->setItem(i, 2, new QTableWidgetItem(georefText));
    session_table_->setItem(i, 3, new QTableWidgetItem(to_string(sess.align)));
    session_table_->setItem(i, 4, new QTableWidgetItem(sess.anchor ? "anchor" : ""));

    QString kd = "-";
    if (rep) {
      for (const auto& sm : rep->sessions) {
        if (sm.id == row.mergeId) {
          kd = QString("%1 kept / %2 dup").arg(sm.kept_points).arg(sm.dropped_duplicate_points);
          break;
        }
      }
    }
    session_table_->setItem(i, 5, new QTableWidgetItem(kd));

    if (!sess.anchor) {
      align_session_combo_->addItem(row.provenanceId, row.mergeId);
    }
    yaw_reference_combo_->addItem(row.provenanceId, row.mergeId);
  }
  for (int c = 0; c < session_table_->columnCount(); ++c) session_table_->resizeColumnToContents(c);
}

// ---------------------------------------------------------------------------
// coarse alignment
// ---------------------------------------------------------------------------

bool MergeDock::alignGeoreferencedForCli(MergeProject::GeorefAlignReport* out, QString* err) {
  MergeProject::GeorefAlignReport rep;
  const auto st = project_->align_georeferenced(&rep);
  if (out) *out = rep;
  if (!st.ok()) {
    if (err) *err = QString("%1 (%2)").arg(scanengine::error_str(st.error()), rep.blocker);
    refreshSessionTable();
    return false;
  }
  refreshSessionTable();
  return true;
}

void MergeDock::onAlignGeoref() {
  MergeProject::GeorefAlignReport rep;
  QString err;
  const bool ok = alignGeoreferencedForCli(&rep, &err);
  const QString msg =
      ok ? QString("georeferenced align: %1 aligned, %2 skipped, reference session %3%4, "
                   "max ENU-origin separation %5 m")
               .arg(rep.aligned)
               .arg(rep.skipped)
               .arg(rep.reference)
               .arg(rep.reference_is_project ? " (project CRS)" : "")
               .arg(rep.max_origin_separation_m, 0, 'f', 2)
         : QString("georeferenced align FAILED: %1").arg(err);
  align_status_->setText(msg);
  logAndStatus(msg);
}

void MergeDock::onYawSearch() {
  if (align_session_combo_->count() == 0 || yaw_reference_combo_->count() == 0) {
    QMessageBox::information(this, "Yaw search", "Need at least two sessions.");
    return;
  }
  const quint32 id = align_session_combo_->currentData().toUInt();
  const quint32 ref = yaw_reference_combo_->currentData().toUInt();
  scanengine::merge::YawSearchResult res;
  QString err;
  const bool ok = yawSearchForCli(id, ref, &res, &err);
  QString msg;
  if (!res.ok) {
    msg = QString("yaw search vs %1: NOT APPLIED (%2) — overlap %3, margin %4%5")
              .arg(ref)
              .arg(err.isEmpty() ? "gate not cleared" : err)
              .arg(res.overlap, 0, 'f', 3)
              .arg(res.margin, 0, 'f', 3)
              .arg(res.ambiguous ? " — AMBIGUOUS: two+ yaws score equally, an operator must "
                                   "pick or use georef/3-point instead"
                                 : "");
  } else {
    msg = QString("yaw search vs %1: applied — yaw %2°, overlap %3, margin %4 (runner-up %5° at "
                 "%6)")
              .arg(ref)
              .arg(res.yaw_deg, 0, 'f', 2)
              .arg(res.overlap, 0, 'f', 3)
              .arg(res.margin, 0, 'f', 3)
              .arg(res.runner_up_yaw_deg, 0, 'f', 1)
              .arg(res.runner_up_overlap, 0, 'f', 3);
  }
  (void)ok;
  align_status_->setText(msg);
  logAndStatus(msg);
  refreshSessionTable();
}

bool MergeDock::yawSearchForCli(quint32 sessionId, quint32 reference,
                                scanengine::merge::YawSearchResult* out, QString* err) {
  scanengine::merge::YawSearchResult res;
  const auto st = project_->align_yaw_search(sessionId, reference, {}, &res);
  if (out) *out = res;
  if (!st.ok()) {
    if (err) *err = scanengine::error_str(st.error());
    return false;
  }
  return true;
}

// --- 3-point manual picking -------------------------------------------------

void MergeDock::onStartPick() {
  if (picking_) return;
  if (align_session_combo_->count() == 0) {
    QMessageBox::information(this, "3-point manual", "No non-anchor session to align.");
    return;
  }
  picking_session_ = align_session_combo_->currentData().toUInt();
  picks_.clear();
  picking_ = true;
  pick_btn_->setEnabled(false);
  pick_cancel_btn_->setEnabled(true);
  beginPickPhase();
}

void MergeDock::onCancelPick() { abortPicking("cancelled by operator"); }

void MergeDock::beginPickPhase() {
  // Find the source (session being aligned) and target (anchor) rows.
  scanengine::PageStore* sourceStore = nullptr;
  QString sourceLabel, targetLabel;
  for (auto& row : sessions_) {
    if (row.mergeId == picking_session_) {
      sourceStore = row.store();
      sourceLabel = row.provenanceId;
    }
    if (project_->session(row.mergeId).anchor) targetLabel = row.provenanceId;
  }
  if (!sourceStore) {
    abortPicking("source session vanished");
    return;
  }
  pick_sub_phase_ = PickSubPhase::kAwaitingSource;
  if (viewport_) {
    viewport_->clearMeasurements();
    viewport_->setMeasureMode(true);
    viewport_->setPointStore(sourceStore);
    viewport_->fitView();
  }
  align_status_->setText(QString("pick %1 of %2: click a feature in SOURCE session '%3' "
                                 "(shown alone in the viewport)")
                             .arg(picks_.size() + 1)
                             .arg(kPickCount)
                             .arg(sourceLabel));
  logAndStatus(align_status_->text());
  (void)targetLabel;
}

void MergeDock::onViewportMeasurementsChanged() {
  if (!picking_ || !viewport_) return;

  if (pick_sub_phase_ == PickSubPhase::kAwaitingSource) {
    if (!viewport_->hasPendingMeasurePoint()) return;  // ESC or a miss; stay put
    // First click landed: swap in the TARGET (anchor) cloud for the second
    // click of this pair. The pending point itself is pure geometry inside
    // ViewportWindow (no store reference), so swapping the displayed store
    // does not disturb it.
    scanengine::PageStore* targetStore = nullptr;
    QString targetLabel;
    for (auto& row : sessions_) {
      if (project_->session(row.mergeId).anchor) {
        targetStore = row.store();
        targetLabel = row.provenanceId;
      }
    }
    if (!targetStore) {
      abortPicking("no anchor session to pick a target point in");
      return;
    }
    pick_sub_phase_ = PickSubPhase::kAwaitingTarget;
    viewport_->setPointStore(targetStore);
    viewport_->fitView();
    align_status_->setText(QString("pick %1 of %2: now click the MATCHING feature in TARGET "
                                   "(anchor) session '%3'")
                               .arg(picks_.size() + 1)
                               .arg(kPickCount)
                               .arg(targetLabel));
    logAndStatus(align_status_->text());
    return;
  }

  // kAwaitingTarget: a completed segment is the pick pair.
  if (viewport_->hasPendingMeasurePoint()) return;  // shouldn't happen, but stay safe
  const auto& segs = viewport_->measurements();
  if (segs.empty()) return;
  const auto& seg = segs.back();
  recordPick(seg.a, seg.b);

  if (picks_.size() >= kPickCount) {
    finalizePicks();
    return;
  }

  pick_sub_phase_ = PickSubPhase::kAwaitingSource;
  scanengine::PageStore* sourceStore = nullptr;
  for (auto& row : sessions_) {
    if (row.mergeId == picking_session_) sourceStore = row.store();
  }
  viewport_->clearMeasurements();
  if (sourceStore) {
    viewport_->setPointStore(sourceStore);
    viewport_->fitView();
  }
  align_status_->setText(QString("pick %1 of %2: click a feature in SOURCE session")
                             .arg(picks_.size() + 1)
                             .arg(kPickCount));
  logAndStatus(align_status_->text());
}

void MergeDock::recordPick(const float sourceLocal[3], const float targetLocal[3]) {
  scanengine::merge::PointCorrespondence c;
  for (int k = 0; k < 3; ++k) {
    c.a[k] = double(sourceLocal[k]);
    c.b[k] = double(targetLocal[k]);
  }
  picks_.push_back(c);
}

void MergeDock::finalizePicks() {
  scanengine::merge::CorrespondenceSolution sol;
  const scanengine::Span<const scanengine::merge::PointCorrespondence> span(picks_.data(),
                                                                             picks_.size());
  const auto st = project_->align_from_correspondences(picking_session_, span, {}, &sol);
  const QString msg =
      st.ok()
          ? QString("3-point manual align (session %1): OK — rms %2, max residual %3, spread "
                    "%4/%5/%6 m")
                .arg(picking_session_)
                .arg(fmtMm(sol.rms_m), fmtMm(sol.max_residual_m))
                .arg(sol.spread_m[0], 0, 'f', 2)
                .arg(sol.spread_m[1], 0, 'f', 2)
                .arg(sol.spread_m[2], 0, 'f', 2)
          : QString("3-point manual align (session %1): REFUSED — %2")
                .arg(picking_session_)
                .arg(sol.blocker);
  align_status_->setText(msg);
  logAndStatus(msg);

  picking_ = false;
  pick_btn_->setEnabled(true);
  pick_cancel_btn_->setEnabled(false);
  if (viewport_) {
    viewport_->setMeasureMode(false);
    viewport_->clearMeasurements();
  }
  refreshSessionTable();
}

void MergeDock::abortPicking(const QString& why) {
  picking_ = false;
  picks_.clear();
  pick_btn_->setEnabled(true);
  pick_cancel_btn_->setEnabled(false);
  if (viewport_) {
    viewport_->setMeasureMode(false);
    viewport_->clearMeasurements();
  }
  align_status_->setText(QString("3-point pick aborted: %1").arg(why));
  logAndStatus(align_status_->text());
}

bool MergeDock::alignManualForCli(quint32 sessionId, const std::vector<std::array<float, 3>>& sourceLocal,
                                  const std::vector<std::array<float, 3>>& targetLocal,
                                  scanengine::merge::CorrespondenceSolution* out, QString* err) {
  if (sourceLocal.size() != targetLocal.size() || sourceLocal.size() < 3) {
    if (err) *err = "need at least 3 matching source/target points";
    return false;
  }
  picking_session_ = sessionId;
  picks_.clear();
  for (std::size_t i = 0; i < sourceLocal.size(); ++i) {
    recordPick(sourceLocal[i].data(), targetLocal[i].data());
  }
  scanengine::merge::CorrespondenceSolution sol;
  const scanengine::Span<const scanengine::merge::PointCorrespondence> span(picks_.data(),
                                                                             picks_.size());
  const auto st = project_->align_from_correspondences(sessionId, span, {}, &sol);
  if (out) *out = sol;
  picks_.clear();
  refreshSessionTable();
  if (!st.ok()) {
    if (err) *err = QString("%1 (%2)").arg(scanengine::error_str(st.error()), sol.blocker);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// refine
// ---------------------------------------------------------------------------

void MergeDock::onSurveyOverlap() {
  const auto st = project_->survey_overlap();
  refine_status_->setText(st.ok() ? "overlap surveyed — see the Pairs table"
                                  : QString("survey failed: %1").arg(scanengine::error_str(st.error())));
  logAndStatus(refine_status_->text());
  refreshPairTable();
}

bool MergeDock::refineForCli(QString* err) {
  // Snapshot every pair's current relative transform BEFORE refine() moves
  // anything, so the residual chart can reproduce a specific pair's trace on
  // demand later via scanengine::merge::refine_pair() with the SAME init
  // refine() itself used — MergeReport does not carry the trace (only
  // rms_before_m/rms_residual_m), so this is the only way to get it back.
  pre_refine_init_.clear();
  const auto& sess = project_->sessions();
  for (std::size_t i = 0; i < sess.size(); ++i) {
    for (std::size_t j = i + 1; j < sess.size(); ++j) {
      double bFromA[16];
      double aInv[16];
      scanengine::se3::mat4_inverse_rigid(sess[j].world_from_session, aInv);
      scanengine::se3::mat4_mul(aInv, sess[i].world_from_session, bFromA);
      std::array<double, 16> arr;
      std::copy(bFromA, bFromA + 16, arr.begin());
      const std::uint64_t key = (std::uint64_t(sess[i].id) << 32) | std::uint64_t(sess[j].id);
      pre_refine_init_[key] = arr;
    }
  }

  const auto st = project_->refine();
  if (!st.ok()) {
    if (err) *err = scanengine::error_str(st.error());
    refreshPairTable();
    refreshSessionTable();
    return false;
  }
  refreshPairTable();
  refreshSessionTable();
  return true;
}

void MergeDock::onRefine() {
  QString err;
  const bool ok = refineForCli(&err);
  const MergeReport* rep = reportOrNull();
  QString msg;
  if (!ok) {
    msg = QString("refine FAILED: %1").arg(err);
  } else if (rep) {
    msg = QString("refine: %1 pairs refined, %2 converged, %3 low-overlap, worst rms %4%5")
              .arg(rep->pairs_refined)
              .arg(rep->pairs_converged)
              .arg(rep->pairs_low_overlap)
              .arg(fmtMm(rep->worst_rms_m))
              .arg(rep->relaxed
                       ? QString(" — global relaxation: chi2 %1 -> %2 in %3 iterations")
                             .arg(rep->graph.initial_chi2, 0, 'g', 4)
                             .arg(rep->graph.final_chi2, 0, 'g', 4)
                             .arg(rep->graph.iterations)
                       : QString());
  } else {
    msg = "refine: no report";
  }
  refine_status_->setText(msg);
  logAndStatus(msg);
}

void MergeDock::refreshPairTable() {
  const MergeReport* rep = reportOrNull();
  pair_table_->setRowCount(rep ? int(rep->pairs.size()) : 0);
  if (!rep) return;
  for (int i = 0; i < int(rep->pairs.size()); ++i) {
    const auto& pr = rep->pairs[i];
    pair_table_->setItem(i, 0, new QTableWidgetItem(QString::number(pr.session_a)));
    pair_table_->setItem(i, 1, new QTableWidgetItem(QString::number(pr.session_b)));
    pair_table_->setItem(i, 2, new QTableWidgetItem(fmtMm(pr.rms_before_m)));
    pair_table_->setItem(i, 3, new QTableWidgetItem(fmtMm(pr.rms_residual_m)));
    pair_table_->setItem(i, 4, new QTableWidgetItem(fmtPct(pr.overlap_a_in_b)));
    pair_table_->setItem(i, 5, new QTableWidgetItem(fmtPct(pr.overlap_b_in_a)));
    pair_table_->setItem(i, 6, new QTableWidgetItem(QString::number(pr.iterations)));
    QString status = pr.low_overlap ? "low overlap"
                     : pr.converged ? "converged"
                     : pr.refined   ? "refined (not converged)"
                                    : "not refined";
    if (*pr.blocker) status += QString(" — %1").arg(pr.blocker);
    pair_table_->setItem(i, 7, new QTableWidgetItem(status));
  }
  for (int c = 0; c < pair_table_->columnCount(); ++c) pair_table_->resizeColumnToContents(c);
}

void MergeDock::refreshChartFor(int pairRow) {
  const MergeReport* rep = reportOrNull();
  if (!rep || pairRow < 0 || pairRow >= int(rep->pairs.size())) {
    chart_->clearTrace();
    return;
  }
  const auto& pr = rep->pairs[pairRow];
  const std::uint64_t key = (std::uint64_t(pr.session_a) << 32) | std::uint64_t(pr.session_b);
  const auto it = pre_refine_init_.find(key);
  if (it == pre_refine_init_.end() || !pr.refined) {
    chart_->clearTrace();
    return;
  }
  const auto& sa = project_->session(pr.session_a);
  const auto& sb = project_->session(pr.session_b);
  const auto res =
      scanengine::merge::refine_pair(sa.cloud, sb.cloud, it->second.data());
  chart_->setTrace(res.trace, QString("pair %1<->%2: %3 mm -> %4 mm (%5 iterations, %6 rolled back)")
                                  .arg(pr.session_a)
                                  .arg(pr.session_b)
                                  .arg(res.rms_before_m * 1000.0, 0, 'f', 2)
                                  .arg(res.rms_after_m * 1000.0, 0, 'f', 2)
                                  .arg(res.iterations)
                                  .arg(res.rejected_steps));
}

// ---------------------------------------------------------------------------
// build + publish + export
// ---------------------------------------------------------------------------

bool MergeDock::buildAndPublishForCli(bool colorBySession, QString* err) {
  scanengine::merge::MergeOutputConfig cfg;
  cfg.dedup_voxel_m = dedup_voxel_ ? dedup_voxel_->value() : 0.03;
  const auto bst = project_->build(cfg, &last_result_);
  if (!bst.ok()) {
    if (err) *err = scanengine::error_str(bst.error());
    return false;
  }
  has_result_ = true;

  if (colorBySession) {
    for (const auto& range : last_result_.ranges) {
      const auto c = sessionColor(range.session);
      for (std::uint64_t i = range.first; i < range.first + range.count; ++i) {
        last_result_.cloud[i].r = c[0];
        last_result_.cloud[i].g = c[1];
        last_result_.cloud[i].b = c[2];
        last_result_.cloud[i].a = 255;
      }
    }
  }

  merged_store_.clear();
  const auto pst = project_->publish(&last_result_, &merged_store_, scanengine::StreamId::kSlamMap);
  // build() fills in every SessionSummary's kept/dropped counts, so the
  // session table's "Kept/Dropped" column is stale until this refresh —
  // needed here (not just in onBuild()) since CLI evidence calls this
  // directly, bypassing the button handler.
  refreshSessionTable();
  if (!pst.ok()) {
    if (err) *err = scanengine::error_str(pst.error());
    return false;
  }
  if (viewport_) {
    viewport_->setPointStore(&merged_store_);
    viewport_->fitView();
  }
  return true;
}

void MergeDock::onBuild() {
  QString err;
  const bool ok = buildAndPublishForCli(color_by_session_->isChecked(), &err);
  const MergeReport* rep = reportOrNull();
  QString msg;
  if (!ok) {
    msg = QString("build FAILED: %1").arg(err);
  } else if (rep) {
    msg = QString("built: %1 input -> %2 merged points (%3 dedup-dropped, %4 priority-dropped), "
                 "%5 pages (%6 shared)")
              .arg(rep->input_points)
              .arg(rep->merged_points)
              .arg(rep->dedup_dropped_points)
              .arg(rep->priority_dropped_points)
              .arg(rep->pages_appended)
              .arg(rep->pages_shared);
  } else {
    msg = "built: no report";
  }
  build_status_->setText(msg);
  logAndStatus(msg);
  export_btn_->setEnabled(ok);
}

void MergeDock::onExport() { Q_EMIT exportMergedRequested(); }

void MergeDock::selectPairForCli(int row) {
  if (row >= 0 && row < pair_table_->rowCount()) pair_table_->selectRow(row);
}

const MergeReport* MergeDock::reportOrNull() const {
  if (!project_) return nullptr;
  return &project_->report();
}

}  // namespace lidarscan
