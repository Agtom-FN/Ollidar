// DisplayParamsDock.h — the §3.9 display-parameter panel, bound directly to
// engine task A14's scanengine::DisplayParams / DisplayParamsController.
//
// The dock owns no parameter state of its own: every control reads and writes
// the DisplayParamsController, which clamps, versions and notifies. The
// viewport subscribes to the same controller. That is A14's documented shape
// ("wiring a settings panel's Apply button straight to a live viewer") and it
// means the panel cannot drift from what is rendered.
//
// Persistence is A14's to_json()/from_json() written to
// <project>/processed/display_params.json — §3.9's "settings persist per
// project".
//
// Owner: C1 (panel) over A14 (model).
#pragma once

#include <QDockWidget>
#include <QString>

#include "scanengine/cloud/display_params.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace lidarscan {

class DisplayParamsDock : public QDockWidget {
  Q_OBJECT
 public:
  explicit DisplayParamsDock(scanengine::DisplayParamsController* controller,
                             QWidget* parent = nullptr);

  // Called when the model changed from somewhere else (profile load, project
  // open, auto-range refresh).
  void refreshFromModel();

  // <project>/processed/display_params.json
  static QString paramsPathFor(const QString& project_dir);
  bool loadFromProject(const QString& project_dir);
  bool saveToProject(const QString& project_dir, QString* err = nullptr);

 Q_SIGNALS:
  void changed();

 private:
  void buildUi();
  void pushToModel();
  scanengine::ScalarColorParams* activeScalar(scanengine::DisplayParams& p) const;

  scanengine::DisplayParamsController* controller_ = nullptr;
  bool updating_ = false;

  QComboBox* profile_ = nullptr;
  QComboBox* color_mode_ = nullptr;
  QComboBox* colormap_ = nullptr;
  QCheckBox* auto_range_ = nullptr;
  QDoubleSpinBox* range_min_ = nullptr;
  QDoubleSpinBox* range_max_ = nullptr;
  QDoubleSpinBox* gamma_ = nullptr;
  QDoubleSpinBox* brightness_ = nullptr;
  QCheckBox* invert_ = nullptr;

  QComboBox* size_mode_ = nullptr;
  QDoubleSpinBox* fixed_px_ = nullptr;
  QDoubleSpinBox* adaptive_min_ = nullptr;
  QDoubleSpinBox* adaptive_max_ = nullptr;
  QDoubleSpinBox* adaptive_ref_ = nullptr;
  QDoubleSpinBox* world_size_ = nullptr;

  QSpinBox* lod_budget_ = nullptr;

  QCheckBox* edl_enabled_ = nullptr;
  QDoubleSpinBox* edl_strength_ = nullptr;

  QPushButton* background_ = nullptr;

  QCheckBox* clip_height_ = nullptr;
  QDoubleSpinBox* clip_height_min_ = nullptr;
  QDoubleSpinBox* clip_height_max_ = nullptr;
  QCheckBox* clip_box_ = nullptr;
  QDoubleSpinBox* clip_box_min_[3] = {nullptr, nullptr, nullptr};
  QDoubleSpinBox* clip_box_max_[3] = {nullptr, nullptr, nullptr};

  QCheckBox* show_trajectory_ = nullptr;
  QCheckBox* show_pose_graph_ = nullptr;
};

}  // namespace lidarscan
