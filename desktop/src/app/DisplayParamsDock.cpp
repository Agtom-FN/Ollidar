#include "app/DisplayParamsDock.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

namespace lidarscan {
namespace {

QDoubleSpinBox* spin(double lo, double hi, double step, int decimals) {
  auto* s = new QDoubleSpinBox();
  s->setRange(lo, hi);
  s->setSingleStep(step);
  s->setDecimals(decimals);
  s->setKeyboardTracking(false);
  return s;
}

}  // namespace

DisplayParamsDock::DisplayParamsDock(scanengine::DisplayParamsController* controller,
                                     QWidget* parent)
    : QDockWidget("Display parameters", parent), controller_(controller) {
  buildUi();
  refreshFromModel();
}

void DisplayParamsDock::buildUi() {
  auto* root = new QWidget();
  auto* v = new QVBoxLayout(root);
  v->setContentsMargins(8, 8, 8, 8);

  auto connectAll = [this](auto* w) {
    if (auto* c = qobject_cast<QCheckBox*>(w)) {
      connect(c, &QCheckBox::toggled, this, [this] { pushToModel(); });
    }
  };
  (void)connectAll;

  // --- profile ---
  {
    auto* g = new QGroupBox("Workflow profile");
    auto* f = new QFormLayout(g);
    profile_ = new QComboBox();
    // Index 0 is "(custom)": a DisplayParams does not record which profile it
    // came from, so showing a profile name for arbitrary parameters would lie.
    profile_->addItem("(custom)", -1);
    for (int i = 0; i < scanengine::kDisplayProfileCount; ++i) {
      profile_->addItem(
          scanengine::to_string(static_cast<scanengine::DisplayProfile>(i)), i);
    }
    profile_->setToolTip(
        "Profiles set defaults (Tech Spec §3.9). Selecting one replaces every "
        "parameter below with A14's profile_defaults().");
    connect(profile_, &QComboBox::currentIndexChanged, this, [this](int idx) {
      if (updating_ || !controller_ || idx <= 0) return;
      controller_->set(
          scanengine::profile_defaults(static_cast<scanengine::DisplayProfile>(idx - 1)));
      refreshFromModel();
      Q_EMIT changed();
    });
    f->addRow("Profile", profile_);
    v->addWidget(g);
  }

  // --- colour ---
  {
    auto* g = new QGroupBox("Colour");
    auto* f = new QFormLayout(g);
    color_mode_ = new QComboBox();
    for (int i = 0; i < scanengine::kColorModeCount; ++i) {
      color_mode_->addItem(scanengine::to_string(static_cast<scanengine::ColorMode>(i)), i);
    }
    color_mode_->setItemData(3, "No per-point time in PointVertex — falls back to RGB",
                             Qt::ToolTipRole);
    color_mode_->setItemData(4, "No per-point fix quality in PointVertex — falls back to RGB",
                             Qt::ToolTipRole);
    f->addRow("Mode", color_mode_);

    colormap_ = new QComboBox();
    for (int i = 0; i < scanengine::kColormapCount; ++i) {
      colormap_->addItem(scanengine::to_string(static_cast<scanengine::Colormap>(i)), i);
    }
    f->addRow("Colormap", colormap_);

    auto_range_ = new QCheckBox("Auto range from data bounds");
    auto_range_->setToolTip(
        "Height mode: the renderer refreshes min/max every frame from "
        "PageView::bounds_min[2]/bounds_max[2].");
    f->addRow(auto_range_);

    range_min_ = spin(-10000, 10000, 0.1, 3);
    range_max_ = spin(-10000, 10000, 0.1, 3);
    f->addRow("Range min", range_min_);
    f->addRow("Range max", range_max_);

    gamma_ = spin(0.1, 4.0, 0.05, 2);
    brightness_ = spin(0.1, 3.0, 0.05, 2);
    invert_ = new QCheckBox("Invert ramp");
    f->addRow("Gamma", gamma_);
    f->addRow("Brightness", brightness_);
    f->addRow(invert_);
    v->addWidget(g);
  }

  // --- point size ---
  {
    auto* g = new QGroupBox("Point size");
    auto* f = new QFormLayout(g);
    size_mode_ = new QComboBox();
    for (int i = 0; i < 3; ++i) {
      size_mode_->addItem(scanengine::to_string(static_cast<scanengine::PointSizeMode>(i)), i);
    }
    size_mode_->setItemData(
        2,
        "World-size is the portable fallback for backends where gl_PointSize is "
        "unreliable (S3 §8.3). On Metal it is implemented with gl_PointSize too.",
        Qt::ToolTipRole);
    f->addRow("Mode", size_mode_);
    fixed_px_ = spin(0.5, 64.0, 0.5, 1);
    adaptive_min_ = spin(0.5, 64.0, 0.5, 1);
    adaptive_max_ = spin(0.5, 64.0, 0.5, 1);
    adaptive_ref_ = spin(0.01, 1000.0, 0.5, 2);
    world_size_ = spin(0.0005, 1.0, 0.005, 4);
    f->addRow("Fixed (px)", fixed_px_);
    f->addRow("Adaptive min (px)", adaptive_min_);
    f->addRow("Adaptive max (px)", adaptive_max_);
    f->addRow("Adaptive ref (m)", adaptive_ref_);
    f->addRow("World size (m)", world_size_);
    v->addWidget(g);
  }

  // --- LOD + EDL ---
  {
    auto* g = new QGroupBox("Detail");
    auto* f = new QFormLayout(g);
    lod_budget_ = new QSpinBox();
    lod_budget_->setRange(1000, 200000000);
    lod_budget_->setSingleStep(500000);
    lod_budget_->setGroupSeparatorShown(true);
    lod_budget_->setKeyboardTracking(false);
    lod_budget_->setToolTip(
        "Soft render-time ceiling. C1 implements it by dropping whole pages out "
        "of the scene past the budget; true coarse-to-fine LOD is C3 work.");
    f->addRow("LOD point budget", lod_budget_);

    edl_enabled_ = new QCheckBox("EDL shading");
    edl_strength_ = spin(0.0, 1.0, 0.05, 2);
    edl_enabled_->setEnabled(false);
    edl_strength_->setEnabled(false);
    const QString edlTip =
        "Eye-dome lighting is a full-screen post-process. S3 left its cost "
        "unmeasured (REPORT §7) and C1 does not implement the pass — the "
        "parameter is stored and persisted, but nothing renders it yet.";
    edl_enabled_->setToolTip(edlTip);
    edl_strength_->setToolTip(edlTip);
    f->addRow(edl_enabled_);
    f->addRow("EDL strength", edl_strength_);
    v->addWidget(g);
  }

  // --- background ---
  {
    auto* g = new QGroupBox("Background");
    auto* f = new QFormLayout(g);
    background_ = new QPushButton("Choose…");
    connect(background_, &QPushButton::clicked, this, [this] {
      if (!controller_) return;
      auto p = controller_->get();
      const QColor initial(p.background.r, p.background.g, p.background.b);
      const QColor c = QColorDialog::getColor(initial, this, "Viewport background");
      if (!c.isValid()) return;
      p.background = scanengine::RGBA8{std::uint8_t(c.red()), std::uint8_t(c.green()),
                                       std::uint8_t(c.blue()), 255};
      controller_->set(p);
      refreshFromModel();
      Q_EMIT changed();
    });
    f->addRow("Colour", background_);
    v->addWidget(g);
  }

  // --- clipping ---
  {
    auto* g = new QGroupBox("Clipping");
    auto* f = new QFormLayout(g);
    clip_height_ = new QCheckBox("Height band");
    clip_height_min_ = spin(-1000, 1000, 0.05, 3);
    clip_height_max_ = spin(-1000, 1000, 0.05, 3);
    f->addRow(clip_height_);
    f->addRow("Z min (m)", clip_height_min_);
    f->addRow("Z max (m)", clip_height_max_);

    clip_box_ = new QCheckBox("Box");
    f->addRow(clip_box_);
    const char* axes[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
      clip_box_min_[i] = spin(-10000, 10000, 0.5, 2);
      clip_box_max_[i] = spin(-10000, 10000, 0.5, 2);
      f->addRow(QString("Box %1 min").arg(axes[i]), clip_box_min_[i]);
      f->addRow(QString("Box %1 max").arg(axes[i]), clip_box_max_[i]);
    }
    v->addWidget(g);
  }

  // --- overlays ---
  {
    auto* g = new QGroupBox("Overlays");
    auto* f = new QFormLayout(g);
    show_trajectory_ = new QCheckBox("Trajectory");
    show_pose_graph_ = new QCheckBox("Pose graph");
    show_trajectory_->setEnabled(false);
    show_pose_graph_->setEnabled(false);
    const QString tip =
        "Stored and persisted, but there is no trajectory or pose graph to draw "
        "until A8/A7 produce one; the overlay renderer is C3.";
    show_trajectory_->setToolTip(tip);
    show_pose_graph_->setToolTip(tip);
    f->addRow(show_trajectory_);
    f->addRow(show_pose_graph_);
    v->addWidget(g);
  }

  v->addStretch(1);

  auto* scroll = new QScrollArea();
  scroll->setWidgetResizable(true);
  scroll->setWidget(root);
  setWidget(scroll);
  setMinimumWidth(300);

  // One handler for every value control.
  const QList<QComboBox*> combos{color_mode_, colormap_, size_mode_};
  for (auto* c : combos) {
    connect(c, &QComboBox::currentIndexChanged, this, [this](int) { pushToModel(); });
  }
  const QList<QCheckBox*> checks{auto_range_,  invert_,    edl_enabled_,     clip_height_,
                                 clip_box_,    show_trajectory_, show_pose_graph_};
  for (auto* c : checks) {
    connect(c, &QCheckBox::toggled, this, [this](bool) { pushToModel(); });
  }
  QList<QDoubleSpinBox*> spins{range_min_,    range_max_,       gamma_,        brightness_,
                               fixed_px_,     adaptive_min_,    adaptive_max_, adaptive_ref_,
                               world_size_,   edl_strength_,    clip_height_min_,
                               clip_height_max_};
  for (int i = 0; i < 3; ++i) {
    spins << clip_box_min_[i] << clip_box_max_[i];
  }
  for (auto* s : spins) {
    connect(s, &QDoubleSpinBox::valueChanged, this, [this](double) { pushToModel(); });
  }
  connect(lod_budget_, &QSpinBox::valueChanged, this, [this](int) { pushToModel(); });
}

scanengine::ScalarColorParams* DisplayParamsDock::activeScalar(
    scanengine::DisplayParams& p) const {
  switch (p.color_mode) {
    case scanengine::ColorMode::kHeight: return &p.height;
    case scanengine::ColorMode::kIntensity: return &p.intensity;
    case scanengine::ColorMode::kTime: return &p.time;
    default: return nullptr;
  }
}

void DisplayParamsDock::refreshFromModel() {
  if (!controller_) return;
  updating_ = true;
  auto p = controller_->get();

  // Show the profile whose defaults these parameters exactly match, else custom.
  int matched = 0;
  for (int i = 0; i < scanengine::kDisplayProfileCount; ++i) {
    if (scanengine::profile_defaults(static_cast<scanengine::DisplayProfile>(i)) == p) {
      matched = i + 1;
      break;
    }
  }
  profile_->setCurrentIndex(matched);

  color_mode_->setCurrentIndex(static_cast<int>(p.color_mode));
  const scanengine::ScalarColorParams* s = activeScalar(p);
  const bool scalar = s != nullptr;
  colormap_->setEnabled(scalar);
  auto_range_->setEnabled(scalar);
  gamma_->setEnabled(scalar);
  brightness_->setEnabled(scalar);
  invert_->setEnabled(scalar);
  const scanengine::ScalarColorParams shown = scalar ? *s : scanengine::ScalarColorParams{};
  colormap_->setCurrentIndex(static_cast<int>(shown.colormap));
  auto_range_->setChecked(shown.auto_range);
  range_min_->setValue(shown.manual_min);
  range_max_->setValue(shown.manual_max);
  range_min_->setEnabled(scalar && !shown.auto_range);
  range_max_->setEnabled(scalar && !shown.auto_range);
  gamma_->setValue(shown.gamma);
  brightness_->setValue(shown.brightness);
  invert_->setChecked(shown.invert);

  size_mode_->setCurrentIndex(static_cast<int>(p.point_size.mode));
  fixed_px_->setValue(p.point_size.fixed_px);
  adaptive_min_->setValue(p.point_size.adaptive_min_px);
  adaptive_max_->setValue(p.point_size.adaptive_max_px);
  adaptive_ref_->setValue(p.point_size.adaptive_reference_m);
  world_size_->setValue(p.point_size.world_size_m);
  fixed_px_->setEnabled(p.point_size.mode == scanengine::PointSizeMode::kFixedPixels);
  const bool adaptive = p.point_size.mode == scanengine::PointSizeMode::kAdaptive;
  adaptive_min_->setEnabled(adaptive);
  adaptive_max_->setEnabled(adaptive);
  adaptive_ref_->setEnabled(adaptive);
  world_size_->setEnabled(p.point_size.mode == scanengine::PointSizeMode::kWorldSize);

  lod_budget_->setValue(int(p.lod_point_budget));
  edl_enabled_->setChecked(p.edl_enabled);
  edl_strength_->setValue(p.edl_strength);

  background_->setText(QString("#%1%2%3")
                           .arg(p.background.r, 2, 16, QChar('0'))
                           .arg(p.background.g, 2, 16, QChar('0'))
                           .arg(p.background.b, 2, 16, QChar('0'))
                           .toUpper());

  clip_height_->setChecked(p.clip_height_enabled);
  clip_height_min_->setValue(p.clip_height_min);
  clip_height_max_->setValue(p.clip_height_max);
  clip_height_min_->setEnabled(p.clip_height_enabled);
  clip_height_max_->setEnabled(p.clip_height_enabled);
  clip_box_->setChecked(p.clip_box_enabled);
  for (int i = 0; i < 3; ++i) {
    clip_box_min_[i]->setValue(p.clip_box_min[i]);
    clip_box_max_[i]->setValue(p.clip_box_max[i]);
    clip_box_min_[i]->setEnabled(p.clip_box_enabled);
    clip_box_max_[i]->setEnabled(p.clip_box_enabled);
  }

  show_trajectory_->setChecked(p.show_trajectory);
  show_pose_graph_->setChecked(p.show_pose_graph);

  updating_ = false;
}

void DisplayParamsDock::pushToModel() {
  if (updating_ || !controller_) return;
  auto p = controller_->get();

  p.color_mode = static_cast<scanengine::ColorMode>(color_mode_->currentIndex());
  if (auto* s = activeScalar(p)) {
    s->colormap = static_cast<scanengine::Colormap>(colormap_->currentIndex());
    s->auto_range = auto_range_->isChecked();
    s->manual_min = float(range_min_->value());
    s->manual_max = float(range_max_->value());
    s->gamma = float(gamma_->value());
    s->brightness = float(brightness_->value());
    s->invert = invert_->isChecked();
  }

  p.point_size.mode = static_cast<scanengine::PointSizeMode>(size_mode_->currentIndex());
  p.point_size.fixed_px = float(fixed_px_->value());
  p.point_size.adaptive_min_px = float(adaptive_min_->value());
  p.point_size.adaptive_max_px = float(adaptive_max_->value());
  p.point_size.adaptive_reference_m = float(adaptive_ref_->value());
  p.point_size.world_size_m = float(world_size_->value());

  p.lod_point_budget = std::uint32_t(lod_budget_->value());
  p.edl_enabled = edl_enabled_->isChecked();
  p.edl_strength = float(edl_strength_->value());

  p.clip_height_enabled = clip_height_->isChecked();
  p.clip_height_min = float(clip_height_min_->value());
  p.clip_height_max = float(clip_height_max_->value());
  p.clip_box_enabled = clip_box_->isChecked();
  for (int i = 0; i < 3; ++i) {
    p.clip_box_min[i] = float(clip_box_min_[i]->value());
    p.clip_box_max[i] = float(clip_box_max_[i]->value());
  }
  p.show_trajectory = show_trajectory_->isChecked();
  p.show_pose_graph = show_pose_graph_->isChecked();

  controller_->set(p);
  refreshFromModel();  // re-read: the controller clamps
  Q_EMIT changed();
}

QString DisplayParamsDock::paramsPathFor(const QString& project_dir) {
  return QDir(project_dir).filePath("processed/display_params.json");
}

bool DisplayParamsDock::loadFromProject(const QString& project_dir) {
  if (!controller_) return false;
  QFile f(paramsPathFor(project_dir));
  if (!f.open(QIODevice::ReadOnly)) return false;
  const QByteArray blob = f.readAll();
  scanengine::DisplayParams p{};
  const auto st = scanengine::from_json(blob.toStdString(), &p);
  if (!st.ok()) return false;
  controller_->set(p);
  refreshFromModel();
  Q_EMIT changed();
  return true;
}

bool DisplayParamsDock::saveToProject(const QString& project_dir, QString* err) {
  if (!controller_) return false;
  const QString path = paramsPathFor(project_dir);
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (err) *err = QString("cannot write %1").arg(path);
    return false;
  }
  const std::string json = scanengine::to_json(controller_->get());
  f.write(json.data(), qint64(json.size()));
  return true;
}

}  // namespace lidarscan
