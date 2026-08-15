#include "app/PlanDock.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace lidarscan {
namespace {

using namespace scanengine::plan;
using scanengine::Status;

constexpr double kPi = 3.14159265358979323846;

void openContaining(const QString& path) {
#if defined(Q_OS_MACOS)
  QProcess::startDetached("open", {"-R", path});
#elif defined(Q_OS_WIN)
  QProcess::startDetached("explorer.exe", {"/select,", QDir::toNativeSeparators(path)});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

QColor kWallColor(60, 60, 70);
QColor kDoorColor(150, 100, 40);
QColor kWindowColor(60, 150, 200);
QColor kRoomFill(120, 180, 255, 60);
QColor kRoomBorder(70, 110, 170);
QColor kIncludeColor(60, 170, 90);
QColor kExcludeColor(200, 60, 60);

}  // namespace

// --- PlanGraphicsView --------------------------------------------------------

PlanGraphicsView::PlanGraphicsView(QWidget* parent) : QGraphicsView(parent) {
  setRenderHint(QPainter::Antialiasing, true);
  setDragMode(QGraphicsView::ScrollHandDrag);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  setResizeAnchor(QGraphicsView::AnchorUnderMouse);
}

void PlanGraphicsView::setDrawMode(bool on) {
  draw_mode_ = on;
  setDragMode(on ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag);
  if (!on && rubber_) {
    delete rubber_;
    rubber_ = nullptr;
  }
}

void PlanGraphicsView::mousePressEvent(QMouseEvent* ev) {
  if (!draw_mode_ || !scene() || ev->button() != Qt::LeftButton) {
    QGraphicsView::mousePressEvent(ev);
    return;
  }
  dragging_ = true;
  drag_start_scene_ = mapToScene(ev->pos());
  rubber_ = new QGraphicsRectItem(QRectF(drag_start_scene_, drag_start_scene_));
  QPen pen(QColor(255, 200, 0));
  pen.setWidth(0);
  pen.setStyle(Qt::DashLine);
  rubber_->setPen(pen);
  rubber_->setZValue(1000.0);
  scene()->addItem(rubber_);
}

void PlanGraphicsView::mouseMoveEvent(QMouseEvent* ev) {
  if (dragging_ && rubber_) {
    const QPointF cur = mapToScene(ev->pos());
    rubber_->setRect(QRectF(drag_start_scene_, cur).normalized());
    return;
  }
  QGraphicsView::mouseMoveEvent(ev);
}

void PlanGraphicsView::resizeEvent(QResizeEvent* ev) {
  QGraphicsView::resizeEvent(ev);
  if (!fit_rect_.isEmpty()) fitInView(fit_rect_, Qt::KeepAspectRatio);
}

void PlanGraphicsView::fitToRect(QRectF sceneRect) {
  fit_rect_ = sceneRect;
  if (!fit_rect_.isEmpty()) fitInView(fit_rect_, Qt::KeepAspectRatio);
}

void PlanGraphicsView::mouseReleaseEvent(QMouseEvent* ev) {
  if (dragging_) {
    dragging_ = false;
    const QRectF r = rubber_ ? rubber_->rect() : QRectF();
    if (rubber_) {
      delete rubber_;
      rubber_ = nullptr;
    }
    if (r.width() > 1e-3 && r.height() > 1e-3) Q_EMIT regionDrawn(r);
    return;
  }
  QGraphicsView::mouseReleaseEvent(ev);
}

// --- PlanDock -----------------------------------------------------------------

PlanDock::PlanDock(QWidget* parent) : QDockWidget("Floor plan", parent) {
  setObjectName("PlanDock");
  buildUi();
}

void PlanDock::buildUi() {
  auto* w = new QWidget();
  auto* v = new QVBoxLayout(w);

  extract_button_ = new QPushButton("Extract floor plan from current cloud");
  extract_button_->setToolTip(
      "Runs A12's extract_floor_plan() (via recompute_grids()+recompute_walls(), so the "
      "grids are cached for the live controls below) against the current point source.");
  connect(extract_button_, &QPushButton::clicked, this, &PlanDock::runFullExtraction);
  v->addWidget(extract_button_);

  auto* sliceBox = new QGroupBox("Slice");
  auto* sf = new QFormLayout(sliceBox);
  slice_slider_ = new QSlider(Qt::Horizontal);
  slice_slider_->setRange(-100, 500);  // centimetres: -1.00 m .. 5.00 m
  slice_slider_->setValue(125);        // 1.25 m, the §3.6 default band's centre
  slice_slider_->setToolTip("Slice-height slider (band centre). Drag to preview; release to "
                            "re-slice the cloud.");
  connect(slice_slider_, &QSlider::valueChanged, this, &PlanDock::onSliderMoved);
  connect(slice_slider_, &QSlider::sliderReleased, this, &PlanDock::onSliderReleased);
  sf->addRow("Height (slider)", slice_slider_);

  band_width_spin_ = new QDoubleSpinBox();
  band_width_spin_->setRange(0.05, 3.0);
  band_width_spin_->setSingleStep(0.05);
  band_width_spin_->setValue(0.5);  // 1.0..1.5 m, the §3.6 default
  band_width_spin_->setSuffix(" m");
  connect(band_width_spin_, &QDoubleSpinBox::editingFinished, this,
          [this] { onBandWidthChanged(band_width_spin_->value()); });
  sf->addRow("Band width", band_width_spin_);

  slice_label_ = new QLabel();
  sf->addRow(slice_label_);

  ortho_check_ = new QCheckBox("Orthogonality snap (7 deg)");
  ortho_check_->setChecked(true);
  ortho_check_->setToolTip("Fast path: does not touch the cloud, only re-runs wall extraction "
                           "against the already-cached grid.");
  connect(ortho_check_, &QCheckBox::toggled, this, &PlanDock::onOrthogonalityChanged);
  sf->addRow(ortho_check_);

  sill_check_ = new QCheckBox("Window sill check (0.35-0.80 m)");
  sill_check_->setChecked(true);
  sill_check_->setToolTip("Slow path: builds a second occupancy grid on the same lattice to "
                          "tell a window (solid wall below) from a door (open below).");
  connect(sill_check_, &QCheckBox::toggled, this, &PlanDock::onSillChanged);
  sf->addRow(sill_check_);

  v->addWidget(sliceBox);

  auto* regionBox = new QGroupBox("Include / exclude regions");
  auto* rv = new QVBoxLayout(regionBox);
  auto* rrow = new QWidget();
  auto* rl = new QHBoxLayout(rrow);
  rl->setContentsMargins(0, 0, 0, 0);
  region_mode_ = new QComboBox();
  region_mode_->addItem("Include");
  region_mode_->addItem("Exclude");
  draw_region_button_ = new QPushButton("Draw region on plan");
  draw_region_button_->setCheckable(true);
  draw_region_button_->setToolTip(
      "Drag a rubber-band rectangle on the plan view below to add a region. Exclude always "
      "wins over include (plan_editor.h); no regions at all keeps every point.");
  rl->addWidget(region_mode_);
  rl->addWidget(draw_region_button_);
  rv->addWidget(rrow);
  region_list_ = new QListWidget();
  region_list_->setMaximumHeight(90);
  rv->addWidget(region_list_);
  auto* delRow = new QWidget();
  auto* dl = new QHBoxLayout(delRow);
  dl->setContentsMargins(0, 0, 0, 0);
  auto* delBtn = new QPushButton("Delete selected");
  auto* clearBtn = new QPushButton("Clear all");
  connect(delBtn, &QPushButton::clicked, this, &PlanDock::onDeleteSelectedRegion);
  connect(clearBtn, &QPushButton::clicked, this, &PlanDock::onClearRegions);
  dl->addWidget(delBtn);
  dl->addWidget(clearBtn);
  rv->addWidget(delRow);
  v->addWidget(regionBox);

  view_ = new PlanGraphicsView();
  scene_ = new QGraphicsScene(this);
  view_->setScene(scene_);
  view_->setMinimumHeight(260);
  connect(draw_region_button_, &QPushButton::toggled, view_, &PlanGraphicsView::setDrawMode);
  connect(view_, &PlanGraphicsView::regionDrawn, this, &PlanDock::onRegionDrawnFromView);
  v->addWidget(view_, 1);

  stats_label_ = new QLabel("no plan extracted yet");
  stats_label_->setWordWrap(true);
  stats_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(stats_label_);

  auto* exportRow = new QWidget();
  auto* el = new QHBoxLayout(exportRow);
  el->setContentsMargins(0, 0, 0, 0);
  dxf_button_ = new QPushButton("Export DXF…");
  pdf_button_ = new QPushButton("Export PDF…");
  dxf_button_->setEnabled(false);
  pdf_button_->setEnabled(false);
  connect(dxf_button_, &QPushButton::clicked, this, &PlanDock::onExportDxf);
  connect(pdf_button_, &QPushButton::clicked, this, &PlanDock::onExportPdf);
  el->addWidget(dxf_button_);
  el->addWidget(pdf_button_);
  v->addWidget(exportRow);

  setWidget(w);
  setMinimumWidth(420);
  updateSliceLabel();
}

void PlanDock::updateSliceLabel() {
  const double center = slice_slider_->value() / 100.0;
  const double width = band_width_spin_->value();
  slice_label_->setText(QString("band %1 .. %2 m")
                            .arg(center - width / 2.0, 0, 'f', 2)
                            .arg(center + width / 2.0, 0, 'f', 2));
}

// --- extraction ---------------------------------------------------------------

void PlanDock::runFullExtraction() {
  if (!store_) {
    QMessageBox::warning(this, "Floor plan", "No point cloud loaded — open/replay a project or "
                                             "load a synthetic building fixture first.");
    return;
  }
  state_ = PlanEditState{};  // back to §3.6 defaults
  const double center = slice_slider_->value() / 100.0;
  const double width = band_width_spin_->value();
  state_ = with_slice_band(state_, float(center - width / 2.0), float(center + width / 2.0));
  state_ = with_orthogonality(state_, ortho_check_->isChecked(), 7.0f);
  state_ = with_sill_check(state_, sill_check_->isChecked(),
                           state_.options.slice.sill_z_min_m, state_.options.slice.sill_z_max_m);
  refreshRegionList();
  rebuildGridsAndWalls();
}

void PlanDock::rebuildGridsAndWalls() {
  if (!store_) return;
  PlanInput in;
  in.store = store_;
  in.up = UpAxis::kZ;
  const Status gst = recompute_grids(in, state_, &main_grid_, &sill_grid_);
  if (!gst.ok()) {
    Q_EMIT logLine(QString("plan: recompute_grids failed: %1").arg(scanengine::error_str(gst.error())));
    grids_valid_ = false;
    return;
  }
  grids_valid_ = true;
  rebuildWallsOnly();
}

void PlanDock::rebuildWallsOnly() {
  if (!grids_valid_) return;
  PlanModel out;
  const Status wst = recompute_walls(main_grid_, sill_grid_.valid() ? &sill_grid_ : nullptr, state_, &out);
  if (!wst.ok()) {
    Q_EMIT logLine(QString("plan: recompute_walls failed: %1").arg(scanengine::error_str(wst.error())));
    return;
  }
  model_ = out;
  model_valid_ = true;
  renderModel();
  dxf_button_->setEnabled(true);
  pdf_button_->setEnabled(true);

  stats_label_->setText(
      QString("%1 walls, %2 openings, %3 rooms (%4 m2 total) — %5 occupied cells, dominant "
              "direction %6 deg — slice %7..%8 m")
          .arg(model_.walls.size())
          .arg(model_.openings.size())
          .arg(model_.rooms.size())
          .arg(model_.stats.total_room_area_m2, 0, 'f', 2)
          .arg(model_.stats.occupied_cells)
          .arg(model_.stats.dominant_angle_rad * 180.0 / kPi, 0, 'f', 2)
          .arg(model_.slice_z_min_m, 0, 'f', 2)
          .arg(model_.slice_z_max_m, 0, 'f', 2));
  Q_EMIT logLine(QString("plan: %1 walls / %2 openings / %3 rooms extracted")
                     .arg(model_.walls.size())
                     .arg(model_.openings.size())
                     .arg(model_.rooms.size()));
}

// --- rendering ------------------------------------------------------------------

void PlanDock::renderModel() {
  scene_->clear();
  if (!model_valid_) return;

  auto toScene = [](Vec2 p) { return QPointF(p.x, -p.y); };

  for (const auto& wallSeg : model_.walls) {
    auto* line = new QGraphicsLineItem(toScene(wallSeg.a).x(), toScene(wallSeg.a).y(),
                                       toScene(wallSeg.b).x(), toScene(wallSeg.b).y());
    QPen pen(kWallColor);
    pen.setWidthF(std::max(0.04, wallSeg.thickness_m));
    pen.setCapStyle(Qt::FlatCap);
    line->setPen(pen);
    line->setZValue(1.0);
    scene_->addItem(line);
  }

  for (const auto& op : model_.openings) {
    const QColor c = op.kind == OpeningKind::kWindowCandidate ? kWindowColor : kDoorColor;
    auto* line = new QGraphicsLineItem(toScene(op.a).x(), toScene(op.a).y(), toScene(op.b).x(),
                                       toScene(op.b).y());
    QPen pen(c);
    pen.setWidthF(0.08);
    pen.setCapStyle(Qt::FlatCap);
    line->setPen(pen);
    line->setZValue(2.0);
    scene_->addItem(line);
  }

  for (const auto& room : model_.rooms) {
    QPolygonF poly;
    for (const auto& p : room.polygon) poly << toScene(p);
    auto* item = new QGraphicsPolygonItem(poly);
    item->setBrush(QBrush(kRoomFill));
    QPen pen(kRoomBorder);
    pen.setWidthF(0.02);
    item->setPen(pen);
    item->setZValue(0.0);
    scene_->addItem(item);

    auto* text = new QGraphicsTextItem(
        QString("%1\n%2 m2").arg(QString::fromStdString(room.label)).arg(room.area_m2, 0, 'f', 2));
    QFont f = text->font();
    f.setPointSize(9);
    text->setFont(f);
    text->setDefaultTextColor(QColor(20, 20, 20));
    text->setFlag(QGraphicsItem::ItemIgnoresTransformations);
    const QPointF c = toScene(room.centroid);
    text->setPos(c);
    text->setZValue(3.0);
    scene_->addItem(text);
  }

  for (const auto& r : state_.regions) {
    QRectF rect(toScene(Vec2{r.min_x, r.max_y}), toScene(Vec2{r.max_x, r.min_y}));
    auto* item = new QGraphicsRectItem(rect);
    QPen pen(r.include ? kIncludeColor : kExcludeColor);
    pen.setWidthF(0.03);
    pen.setStyle(Qt::DashLine);
    item->setPen(pen);
    item->setZValue(4.0);
    scene_->addItem(item);
  }

  // From model_.bounds, not scene_->itemsBoundingRect() — see
  // PlanGraphicsView::fitToRect()'s header comment for why.
  const PlanBounds& b = model_.bounds;
  QRectF fitRect;
  if (b.valid) {
    const QPointF c1 = toScene(Vec2{b.min_x, b.max_y});
    const QPointF c2 = toScene(Vec2{b.max_x, b.min_y});
    fitRect = QRectF(c1, c2).normalized().adjusted(-0.6, -0.6, 0.6, 0.6);
  } else {
    fitRect = scene_->itemsBoundingRect();
  }
  view_->fitToRect(fitRect);
}

void PlanDock::refreshRegionList() {
  region_list_->clear();
  for (const auto& r : state_.regions) {
    region_list_->addItem(QString("%1  (%2,%3) - (%4,%5)")
                              .arg(r.include ? "include" : "exclude")
                              .arg(r.min_x, 0, 'f', 2)
                              .arg(r.min_y, 0, 'f', 2)
                              .arg(r.max_x, 0, 'f', 2)
                              .arg(r.max_y, 0, 'f', 2));
  }
}

// --- slots ------------------------------------------------------------------

void PlanDock::onSliderMoved(int) { updateSliceLabel(); }

void PlanDock::onSliderReleased() {
  if (!store_) return;
  const double center = slice_slider_->value() / 100.0;
  const double width = band_width_spin_->value();
  state_ = with_slice_band(state_, float(center - width / 2.0), float(center + width / 2.0));
  updateSliceLabel();
  rebuildGridsAndWalls();
}

void PlanDock::onBandWidthChanged(double) {
  if (!store_) {
    updateSliceLabel();
    return;
  }
  const double center = slice_slider_->value() / 100.0;
  const double width = band_width_spin_->value();
  state_ = with_slice_band(state_, float(center - width / 2.0), float(center + width / 2.0));
  updateSliceLabel();
  rebuildGridsAndWalls();
}

void PlanDock::onOrthogonalityChanged(bool on) {
  state_ = with_orthogonality(state_, on, 7.0f);
  if (grids_valid_) rebuildWallsOnly();
}

void PlanDock::onSillChanged(bool on) {
  state_ = with_sill_check(state_, on, state_.options.slice.sill_z_min_m,
                           state_.options.slice.sill_z_max_m);
  if (store_) rebuildGridsAndWalls();
}

void PlanDock::onRegionDrawnFromView(QRectF sceneRect) {
  // sceneY = -worldY (see the header comment), so the rectangle's world Y
  // extent comes from negating and swapping its scene Y extent.
  const double minX = sceneRect.left();
  const double maxX = sceneRect.right();
  const double minY = -sceneRect.bottom();
  const double maxY = -sceneRect.top();
  const bool include = region_mode_->currentIndex() == 0;
  state_ = include ? with_include_region(state_, minX, minY, maxX, maxY)
                   : with_exclude_region(state_, minX, minY, maxX, maxY);
  refreshRegionList();
  if (store_) rebuildGridsAndWalls();
  draw_region_button_->setChecked(false);
}

void PlanDock::onDeleteSelectedRegion() {
  const int row = region_list_->currentRow();
  if (row < 0) return;
  state_ = without_region(state_, std::size_t(row));
  refreshRegionList();
  if (store_) rebuildGridsAndWalls();
}

void PlanDock::onClearRegions() {
  state_ = with_regions_cleared(state_);
  refreshRegionList();
  if (store_) rebuildGridsAndWalls();
}

void PlanDock::onExportDxf() {
  if (!model_valid_) return;
  const QString base = project_dir_.isEmpty() ? QDir::homePath() : project_dir_;
  const QString def = QDir(base).filePath("exports/floorplan.dxf");
  const QString path = QFileDialog::getSaveFileName(this, "Export DXF", def, "DXF (*.dxf)");
  if (path.isEmpty()) return;
  QString err;
  if (!exportDxfForCli(path, &err)) {
    QMessageBox::warning(this, "Export DXF", err);
    return;
  }
  openContaining(path);
}

void PlanDock::onExportPdf() {
  if (!model_valid_) return;
  const QString base = project_dir_.isEmpty() ? QDir::homePath() : project_dir_;
  const QString def = QDir(base).filePath("exports/floorplan.pdf");
  const QString path = QFileDialog::getSaveFileName(this, "Export PDF", def, "PDF (*.pdf)");
  if (path.isEmpty()) return;
  QString err;
  if (!exportPdfForCli(path, &err)) {
    QMessageBox::warning(this, "Export PDF", err);
    return;
  }
  openContaining(path);
}

bool PlanDock::exportDxfForCli(const QString& path, QString* err) {
  if (!model_valid_) {
    if (err) *err = "no plan extracted yet";
    return false;
  }
  QDir().mkpath(QFileInfo(path).absolutePath());
  DxfOptions opts;
  const Status st = write_dxf(model_, opts, path.toStdString());
  if (!st.ok()) {
    if (err) *err = scanengine::error_str(st.error());
    return false;
  }
  last_dxf_path_ = path;
  Q_EMIT logLine("plan: DXF exported to " + path);
  return true;
}

bool PlanDock::exportPdfForCli(const QString& path, QString* err) {
  if (!model_valid_) {
    if (err) *err = "no plan extracted yet";
    return false;
  }
  QDir().mkpath(QFileInfo(path).absolutePath());
  PdfOptions opts;
  opts.title = "Floor plan";
  opts.project = QFileInfo(project_dir_).fileName().toStdString();
  const Status st = write_pdf(model_, opts, path.toStdString());
  if (!st.ok()) {
    if (err) *err = scanengine::error_str(st.error());
    return false;
  }
  last_pdf_path_ = path;
  Q_EMIT logLine("plan: PDF exported to " + path);
  return true;
}

}  // namespace lidarscan
