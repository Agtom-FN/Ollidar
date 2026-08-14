// MeasureDock.h — the C3 §3.13 "review workspace ... measure" panel.
//
// Owns none of the picking/geometry logic — that lives in ViewportWindow
// (setMeasureMode()/measurements()/removeMeasurement()/clearMeasurements()),
// exactly the same split DisplayParamsDock uses against
// DisplayParamsController: this dock is a thin UI over state the viewport
// already keeps, so the two can never disagree about what has been measured.
//
// Units (m/ft) are a per-app QSettings preference — there is no engine or
// project concept of a display unit (DisplayParams is metric-only, by
// design: PointVertex positions are metres). "ESC clears" is handled in
// ViewportWindow::keyPressEvent, not here, because ESC only makes sense
// while the viewport has keyboard focus.
//
// Owner: C3.
#pragma once

#include <QDockWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace lidarscan {

class ViewportWindow;

class MeasureDock : public QDockWidget {
  Q_OBJECT
 public:
  explicit MeasureDock(ViewportWindow* viewport, QWidget* parent = nullptr);

 private:
  void buildUi();
  void refresh();
  void onDeleteSelected();
  void onClearAll();
  bool imperial() const;

  ViewportWindow* viewport_ = nullptr;

  QCheckBox* measure_mode_ = nullptr;
  QComboBox* units_ = nullptr;
  QLabel* status_ = nullptr;
  QListWidget* list_ = nullptr;
  QPushButton* delete_button_ = nullptr;
  QPushButton* clear_button_ = nullptr;
};

}  // namespace lidarscan
