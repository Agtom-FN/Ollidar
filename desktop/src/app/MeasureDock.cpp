#include "app/MeasureDock.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>

#include "render/ViewportWindow.h"

namespace lidarscan {
namespace {
constexpr double kMetresToFeet = 3.280839895;
}  // namespace

MeasureDock::MeasureDock(ViewportWindow* viewport, QWidget* parent)
    : QDockWidget("Measure", parent), viewport_(viewport) {
  buildUi();
  connect(viewport_, &ViewportWindow::measurementsChanged, this, &MeasureDock::refresh);
  refresh();
}

void MeasureDock::buildUi() {
  auto* root = new QWidget();
  auto* v = new QVBoxLayout(root);

  measure_mode_ = new QCheckBox("Measure mode (click two points in the viewport)");
  connect(measure_mode_, &QCheckBox::toggled, this, [this](bool on) {
    if (viewport_) viewport_->setMeasureMode(on);
    refresh();
  });
  v->addWidget(measure_mode_);

  auto* f = new QFormLayout();
  units_ = new QComboBox();
  units_->addItem("Metric (m)", false);
  units_->addItem("Imperial (ft)", true);
  QSettings s;
  units_->setCurrentIndex(s.value("measure/imperial", false).toBool() ? 1 : 0);
  connect(units_, &QComboBox::currentIndexChanged, this, [this](int idx) {
    QSettings st;
    st.setValue("measure/imperial", units_->itemData(idx).toBool());
    refresh();
  });
  f->addRow("Units", units_);
  v->addLayout(f);

  status_ = new QLabel();
  status_->setWordWrap(true);
  v->addWidget(status_);

  list_ = new QListWidget();
  list_->setToolTip("Completed point-to-point measurements. ESC in the viewport "
                    "clears an in-progress pick (not an item here).");
  v->addWidget(list_, 1);

  auto* brow = new QWidget();
  auto* bl = new QHBoxLayout(brow);
  bl->setContentsMargins(0, 0, 0, 0);
  delete_button_ = new QPushButton("Delete selected");
  clear_button_ = new QPushButton("Clear all");
  connect(delete_button_, &QPushButton::clicked, this, &MeasureDock::onDeleteSelected);
  connect(clear_button_, &QPushButton::clicked, this, &MeasureDock::onClearAll);
  bl->addWidget(delete_button_);
  bl->addWidget(clear_button_);
  v->addWidget(brow);

  setWidget(root);
  setMinimumWidth(260);
}

bool MeasureDock::imperial() const {
  return units_ && units_->currentData().toBool();
}

void MeasureDock::refresh() {
  if (!viewport_) return;

  measure_mode_->blockSignals(true);
  measure_mode_->setChecked(viewport_->measureMode());
  measure_mode_->blockSignals(false);

  if (!viewport_->measureMode()) {
    status_->setText("Measure mode is off.");
  } else if (viewport_->hasPendingMeasurePoint()) {
    status_->setText("First point picked (yellow marker) — click a second point to "
                     "complete the measurement, or press ESC to cancel it.");
  } else {
    status_->setText("Click a point in the viewport to start a measurement.");
  }

  const int prev_row = list_->currentRow();
  list_->clear();
  const auto& segs = viewport_->measurements();
  const bool ft = imperial();
  for (size_t i = 0; i < segs.size(); ++i) {
    const double d = ft ? segs[i].distance_m * kMetresToFeet : segs[i].distance_m;
    list_->addItem(QString("%1: %2 %3").arg(i + 1).arg(d, 0, 'f', 3).arg(ft ? "ft" : "m"));
  }
  if (prev_row >= 0 && prev_row < list_->count()) list_->setCurrentRow(prev_row);

  delete_button_->setEnabled(!segs.empty());
  clear_button_->setEnabled(!segs.empty());
}

void MeasureDock::onDeleteSelected() {
  if (!viewport_) return;
  const int row = list_->currentRow();
  if (row < 0) return;
  viewport_->removeMeasurement(row);
}

void MeasureDock::onClearAll() {
  if (viewport_) viewport_->clearMeasurements();
}

}  // namespace lidarscan
