// PlanDock.h — C5: the floor-plan workspace. Runs A12's extraction on the
// current cloud, renders the PlanModel in a 2D QGraphicsView (walls /
// openings / rooms layers, room-area labels), and drives every live control
// through plan::PlanEditState via the RECOMPUTE SPLIT plan_editor.h
// documents: a slice-band change (the slider, the band-width spinbox, a
// region add/remove, the sill-check toggle) rebuilds the cached occupancy
// grid(s) — the one streaming pass over the cloud — while everything else
// (orthogonality snap on/off) re-runs ONLY wall extraction against the
// cached grids, in milliseconds, exactly as engine/docs/A12-plan.md §5
// describes ("hold the two OccupancyGrids ... and only redo the full pass
// when the band or the regions actually move").
//
// UNITS. The QGraphicsScene is built directly in METRES (1 scene unit = 1 m)
// with y NEGATED (scene y = -world y) so north (+world y) renders upward —
// Qt's scene convention is y-down. QGraphicsView::fitInView() then supplies
// the pixel scale; nothing here hand-rolls a meters-to-pixels transform.
// Region rectangles are drawn by PlanGraphicsView in SCENE coordinates and
// converted back to world coordinates (worldY = -sceneY) at the one place
// that needs to know the mapping, onRegionDrawn().
//
// Owner: C5.
#pragma once

#include <QDockWidget>
#include <QGraphicsView>
#include <QPointF>
#include <QRectF>
#include <QString>

#include "scanengine/cloud/page_store.h"
#include "scanengine/plan/floor_plan.h"
#include "scanengine/plan/occupancy.h"
#include "scanengine/plan/plan_editor.h"
#include "scanengine/plan/plan_model.h"
#include "scanengine/plan/plan_writers.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGraphicsRectItem;
class QGraphicsScene;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

namespace lidarscan {

// A QGraphicsView that, in "draw region" mode, drags out a rubber-band
// rectangle and reports it in SCENE coordinates rather than acting on it
// itself — PlanDock owns the world<->scene mapping and the PlanEditState.
// Outside draw mode it is a plain pannable/zoomable view (ScrollHandDrag).
class PlanGraphicsView : public QGraphicsView {
  Q_OBJECT
 public:
  explicit PlanGraphicsView(QWidget* parent = nullptr);

  void setDrawMode(bool on);
  // The scene rect to fit the view to, in scene (metre) coordinates — the
  // MODEL's bounds, not QGraphicsScene::itemsBoundingRect(): a room-area
  // label is a QGraphicsTextItem with ItemIgnoresTransformations so it reads
  // at a constant pixel size regardless of zoom, and itemsBoundingRect()
  // folds its LOCAL (pixel-sized) bounding rect straight into scene-unit
  // (metre) coordinates, which silently inflates the "whole plan" bounding
  // box to tens of scene units and zooms the real ~8 m building down to a
  // speck in the corner. The model's own PlanBounds has no such item in it.
  void fitToRect(QRectF sceneRect);

 Q_SIGNALS:
  void regionDrawn(QRectF sceneRect);

 protected:
  void mousePressEvent(QMouseEvent* ev) override;
  void mouseMoveEvent(QMouseEvent* ev) override;
  void mouseReleaseEvent(QMouseEvent* ev) override;
  // fitInView() depends on the viewport's CURRENT pixel size, which at
  // construction time (this dock is built before MainWindow is ever shown or
  // laid out to its final geometry) is whatever tiny default QGraphicsView
  // starts with. Re-fitting on every resize is what keeps the plan filling
  // the view once the real window size lands, and on the first show of a
  // background (non-raised) tab.
  void resizeEvent(QResizeEvent* ev) override;

 private:
  bool draw_mode_ = false;
  bool dragging_ = false;
  QPointF drag_start_scene_;
  QGraphicsRectItem* rubber_ = nullptr;
  QRectF fit_rect_;
};

class PlanDock : public QDockWidget {
  Q_OBJECT
 public:
  explicit PlanDock(QWidget* parent = nullptr);

  // Not a live binding — a snapshot the "Extract" button reads, same posture
  // ExportDialog documents for DisplayParams. Set by MainWindow whenever the
  // project/viewport's point source changes.
  void setPointStore(const scanengine::PageStore* store) { store_ = store; }
  void setProjectDir(const QString& dir) { project_dir_ = dir; }

  const scanengine::plan::PlanModel& model() const { return model_; }
  bool hasModel() const { return model_valid_; }

  // Headless entry points for main.cpp's evidence CLI hooks — the same code
  // the "Extract…"/"Export DXF…"/"Export PDF…" buttons drive, minus the
  // interactive QFileDialog a scripted run cannot answer.
  void runExtractionForCli() { runFullExtraction(); }
  bool exportDxfForCli(const QString& path, QString* err = nullptr);
  bool exportPdfForCli(const QString& path, QString* err = nullptr);

 Q_SIGNALS:
  void logLine(const QString& text);

 private:
  void buildUi();
  void runFullExtraction();
  void rebuildGridsAndWalls();  // slow path: recompute_grids() + recompute_walls()
  void rebuildWallsOnly();      // fast path: recompute_walls() against cached grids
  void renderModel();
  void refreshRegionList();
  void updateSliceLabel();

  void onSliderMoved(int);
  void onSliderReleased();
  void onBandWidthChanged(double);
  void onOrthogonalityChanged(bool);
  void onSillChanged(bool);
  void onRegionDrawnFromView(QRectF sceneRect);
  void onDeleteSelectedRegion();
  void onClearRegions();
  void onExportDxf();
  void onExportPdf();

  const scanengine::PageStore* store_ = nullptr;
  QString project_dir_;

  scanengine::plan::PlanEditState state_;
  scanengine::plan::OccupancyGrid main_grid_;
  scanengine::plan::OccupancyGrid sill_grid_;
  bool grids_valid_ = false;
  scanengine::plan::PlanModel model_;
  bool model_valid_ = false;

  QPushButton* extract_button_ = nullptr;
  QSlider* slice_slider_ = nullptr;
  QDoubleSpinBox* band_width_spin_ = nullptr;
  QLabel* slice_label_ = nullptr;
  QCheckBox* ortho_check_ = nullptr;
  QCheckBox* sill_check_ = nullptr;
  QComboBox* region_mode_ = nullptr;
  QPushButton* draw_region_button_ = nullptr;
  QListWidget* region_list_ = nullptr;
  QLabel* stats_label_ = nullptr;
  QPushButton* dxf_button_ = nullptr;
  QPushButton* pdf_button_ = nullptr;

  PlanGraphicsView* view_ = nullptr;
  QGraphicsScene* scene_ = nullptr;

  QString last_dxf_path_;
  QString last_pdf_path_;
};

}  // namespace lidarscan
