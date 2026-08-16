// InspectorCard.h — the review workspace's floating display/export inspector
// (redesign brief item 3; mockup `.inspector`, docs/design/lidarscan-interfaces.html
// lines 468-474 and 2244-2276, and redesign-exports/05-desktop-macos.png).
//
// The card overlays the top-right corner of the full-bleed viewport. It binds
// to the SAME scanengine::DisplayParamsController as DisplayParamsDock — there
// is no second display model, so moving a slider here and opening the full
// parameter dock show the same numbers by construction, exactly as
// NOTES.md §1.7 requires of anything that touches A14.
//
// The card carries the controls the mockup puts on it (point size, LOD budget,
// gamma, brightness, colour mode, and the ember export button) plus one link
// into the full parameter dock, because the dock still owns the ~30 controls
// that do not fit a 236 px card — clipping boxes, adaptive point sizing, EDL,
// overlays, the background picker. Nothing was removed to make room.
//
// GEOREF. The export button's label is not decoration: `setGeorefState()` is
// fed from Engine::crs_epsg(), which the engine documents as EMPTY until the
// georeference transform converges ("what CRS may I LABEL this cloud with").
// So `LAS 1.4 · georef ✓` appears exactly when a LAS written right now would
// carry a real CRS, and `LAS 1.4 · local frame` when A9 would embed its
// documented local-frame placeholder instead.
//
// REFLOW. Below theme::kInspectorReflowWidth the card stops floating: the
// owner of this widget (MainWindow) takes body() out and hands it to an
// ordinary dock. setFloating(false) drops the card's own padding, radius and
// max-height so the same widgets read correctly in a dock.
//
// Owner: redesign pass.
#pragma once

#include <QFrame>
#include <QString>
#include <QVector>

#include "scanengine/cloud/display_params.h"

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace lidarscan {

class SliderRow;

class InspectorCard : public QFrame {
  Q_OBJECT
 public:
  explicit InspectorCard(scanengine::DisplayParamsController* controller,
                         QWidget* parent = nullptr);

  // Re-reads every control from the controller. Call after anything else
  // (the dock, a profile, a loaded JSON document) changes the model.
  void refreshFromModel();

  // `epsg` is Engine::crs_epsg() verbatim ("EPSG:32650" or empty);
  // `sigma_h_m` is GeorefSolution::horizontal_sigma_m.
  void setGeorefState(bool converged, const QString& epsg, double sigma_h_m);

  // --- CLI evidence hook ---------------------------------------------------
  //
  // Drives the point-size slider through its real QSlider::setValue path (the
  // same signal a mouse drag emits), so scripts/verify_redesign.sh can prove
  // "inspector slider -> visible viewport change" rather than asserting it.
  // Returns the value the model actually took after A14's clamping.
  double setPointSizeForCli(double px);

  // Floating (over the viewport) vs docked (reflowed, < 880 px wide).
  void setFloatingLook(bool floating);
  bool floatingLook() const { return floating_; }

 Q_SIGNALS:
  void changed();            // a display parameter moved
  void exportRequested();    // the ember LAS button
  void moreRequested();      // "All display parameters…"

 protected:
  void paintEvent(QPaintEvent* e) override;

 private:
  QWidget* buildBody();
  void addSection(QVBoxLayout* into, const QString& title);
  void pushToModel();

  scanengine::DisplayParamsController* controller_ = nullptr;
  bool updating_ = false;
  bool floating_ = true;

  QScrollArea* scroll_ = nullptr;
  SliderRow* point_size_ = nullptr;
  SliderRow* lod_ = nullptr;
  SliderRow* gamma_ = nullptr;
  SliderRow* brightness_ = nullptr;
  QVector<QPushButton*> color_chips_;
  QPushButton* export_btn_ = nullptr;
  QLabel* georef_line_ = nullptr;
  QPushButton* more_btn_ = nullptr;
};

}  // namespace lidarscan
