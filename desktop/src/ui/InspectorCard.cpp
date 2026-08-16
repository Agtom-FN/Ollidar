#include "ui/InspectorCard.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <cmath>

#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace lidarscan {
namespace {

constexpr int kCardWidth = 236;   // the brief's "~236px"
constexpr int kCardPad = 12;
constexpr int kMaxHeightCap = 520;

}  // namespace

InspectorCard::InspectorCard(scanengine::DisplayParamsController* controller, QWidget* parent)
    : QFrame(parent), controller_(controller) {
  setObjectName("inspectorCard");
  setFixedWidth(kCardWidth);
  setMaximumHeight(kMaxHeightCap);
  // A native overlay cannot be alpha-composited against the live 3D content
  // (ViewportHost.h explains why), so the translucency is baked: paintEvent
  // fills the card at 94 % panel over the viewport's near-black ground, which
  // lands within a couple of RGB units of the mockup's rgba(24,30,37,.9).
  setAttribute(Qt::WA_StyledBackground, false);

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll_->setWidget(buildBody());
  scroll_->viewport()->setAutoFillBackground(false);
  scroll_->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(2, 4, 2, 4);
  outer->setSpacing(0);
  outer->addWidget(scroll_);

  refreshFromModel();
  setGeorefState(false, QString(), 0.0);
}

void InspectorCard::addSection(QVBoxLayout* into, const QString& title) {
  auto* l = new QLabel(title.toUpper());
  l->setProperty("role", "section");
  QFont f(theme::monoFamily(), 9);
  f.setBold(true);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
  l->setFont(f);
  l->setStyleSheet(QString("color:%1;").arg(theme::css(theme::faint())));
  into->addSpacing(into->count() ? 12 : 0);
  into->addWidget(l);
  into->addSpacing(4);
}

QWidget* InspectorCard::buildBody() {
  auto* body = new QWidget();
  body->setObjectName("inspectorBody");
  // Hard cap: the scroll area has no horizontal bar, so anything that asks for
  // more than the card's width would be silently clipped. Constraining the
  // body instead makes over-wide children wrap or elide, which is visible.
  body->setFixedWidth(kCardWidth - 12);
  auto* v = new QVBoxLayout(body);
  v->setContentsMargins(kCardPad, kCardPad - 2, kCardPad, kCardPad);
  v->setSpacing(0);

  // --- DISPLAY ------------------------------------------------------------
  addSection(v, "Display");

  point_size_ = new SliderRow("point size", 0.5, 16.0, 0.5, body);
  point_size_->setFormat("px");
  v->addWidget(point_size_);

  // The model's budget is a point count; the mockup's slider is in millions,
  // which is the unit the readout has to show either way.
  lod_ = new SliderRow("LOD budget", 1.0, 50.0, 1.0, body);
  lod_->setFormat("M");
  v->addWidget(lod_);

  gamma_ = new SliderRow("gamma", 0.1, 4.0, 0.05, body);
  gamma_->setFormat("2");
  v->addWidget(gamma_);

  brightness_ = new SliderRow("brightness", 0.1, 3.0, 0.05, body);
  brightness_->setFormat("2");
  v->addWidget(brightness_);

  for (SliderRow* r : {point_size_, lod_, gamma_, brightness_}) {
    connect(r, &SliderRow::valueChanged, this, [this](double) { pushToModel(); });
  }

  // --- colour-mode chips --------------------------------------------------
  v->addSpacing(8);
  auto* chipHost = new QWidget(body);
  chipHost->setProperty("chipgroup", true);
  // A14 has five colour modes and the card is 236 px wide, so they wrap onto
  // two rows rather than being clipped or abbreviated into ambiguity.
  auto* ch = new QGridLayout(chipHost);
  ch->setContentsMargins(3, 3, 3, 3);
  ch->setSpacing(2);
  auto* group = new QButtonGroup(this);
  group->setExclusive(true);
  for (int i = 0; i < scanengine::kColorModeCount; ++i) {
    const auto mode = static_cast<scanengine::ColorMode>(i);
    // A14's own names, uppercased — except "fixQuality", which does not fit a
    // 71 px chip and elides to a meaningless "XQUALI". Truncating at the
    // capital keeps it honest ("FIX") and the full name is in the tooltip.
    QString label = QString(scanengine::to_string(mode));
    const qsizetype cap = label.indexOf(QRegularExpression("[A-Z]"));
    if (cap > 0) label = label.left(cap);
    auto* b = new QPushButton(label.toUpper(), chipHost);
    b->setProperty("chip", true);
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // A14 has five modes; two of them (time, fix quality) have no per-point
    // source in PointVertex and fall back to RGB. They stay reachable and say
    // so, rather than being hidden — the same posture DisplayParamsDock has
    // taken since C1.
    b->setToolTip(QString("Colour mode: %1").arg(scanengine::to_string(mode)));
    if (i == 3) b->setToolTip("time — no per-point time in PointVertex, falls back to RGB");
    if (i == 4) b->setToolTip("fixQuality — no per-point fix quality in PointVertex, falls back to RGB");
    group->addButton(b, i);
    color_chips_.push_back(b);
    ch->addWidget(b, i / 3, i % 3);
  }
  connect(group, &QButtonGroup::idClicked, this, [this](int) { pushToModel(); });
  v->addWidget(chipHost);

  // --- EXPORT -------------------------------------------------------------
  addSection(v, "Export");

  export_btn_ = new QPushButton(body);
  export_btn_->setProperty("accent", "ember");
  export_btn_->setCursor(Qt::PointingHandCursor);
  export_btn_->setMinimumHeight(38);
  export_btn_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  export_btn_->setIconSize(QSize(15, 15));
  export_btn_->setIcon(icons::icon(icons::Glyph::kShare, QColor("#1A0D08"), QColor("#1A0D08"), 15,
                                   devicePixelRatioF()));
  connect(export_btn_, &QPushButton::clicked, this, &InspectorCard::exportRequested);
  v->addWidget(export_btn_);

  georef_line_ = new QLabel(body);
  georef_line_->setWordWrap(true);
  georef_line_->setStyleSheet(
      QString("font-family:'%1';font-size:9.5px;color:%2;")
          .arg(theme::monoFamily(), theme::css(theme::faint())));
  v->addSpacing(6);
  v->addWidget(georef_line_);

  // --- everything else ----------------------------------------------------
  addSection(v, "More");
  more_btn_ = new QPushButton("All parameters…", body);
  more_btn_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  more_btn_->setToolTip(
      "Clipping, adaptive point sizing, EDL, overlays, the background picker and "
      "the workflow-profile selector — every A14 control this card does not have "
      "room for.");
  connect(more_btn_, &QPushButton::clicked, this, &InspectorCard::moreRequested);
  v->addWidget(more_btn_);

  v->addStretch(1);
  return body;
}

void InspectorCard::refreshFromModel() {
  if (!controller_) return;
  updating_ = true;
  const auto p = controller_->get();

  point_size_->setValue(p.point_size.fixed_px);
  // Fixed-pixel size is the only mode this slider can express; in adaptive /
  // world-size mode it would be lying, so it says so and defers to the dock.
  const bool fixed = p.point_size.mode == scanengine::PointSizeMode::kFixedPixels;
  point_size_->setEnabled(fixed);
  point_size_->setToolTip(fixed ? QString("Point size in pixels (A14 kFixedPixels).")
                                : QString("Point size is in %1 mode — set it in "
                                          "\"All display parameters…\".")
                                      .arg(scanengine::to_string(p.point_size.mode)));

  lod_->setValue(double(p.lod_point_budget) / 1e6);

  // Gamma/brightness live on the ACTIVE scalar channel, exactly as
  // DisplayParamsDock resolves them — height/intensity/time each carry their
  // own, and RGB carries none.
  const scanengine::ScalarColorParams* s = nullptr;
  switch (p.color_mode) {
    case scanengine::ColorMode::kHeight: s = &p.height; break;
    case scanengine::ColorMode::kIntensity: s = &p.intensity; break;
    case scanengine::ColorMode::kTime: s = &p.time; break;
    default: break;
  }
  const scanengine::ScalarColorParams shown = s ? *s : scanengine::ScalarColorParams{};
  gamma_->setValue(shown.gamma);
  brightness_->setValue(shown.brightness);
  gamma_->setEnabled(s != nullptr);
  brightness_->setEnabled(s != nullptr);

  const int mode = static_cast<int>(p.color_mode);
  for (int i = 0; i < color_chips_.size(); ++i) {
    color_chips_[i]->setChecked(i == mode);
  }

  updating_ = false;
}

void InspectorCard::pushToModel() {
  if (updating_ || !controller_) return;
  auto p = controller_->get();

  for (int i = 0; i < color_chips_.size(); ++i) {
    if (color_chips_[i]->isChecked()) {
      p.color_mode = static_cast<scanengine::ColorMode>(i);
      break;
    }
  }
  if (p.point_size.mode == scanengine::PointSizeMode::kFixedPixels) {
    p.point_size.fixed_px = float(point_size_->value());
  }
  p.lod_point_budget = std::uint32_t(std::llround(lod_->value() * 1e6));

  scanengine::ScalarColorParams* s = nullptr;
  switch (p.color_mode) {
    case scanengine::ColorMode::kHeight: s = &p.height; break;
    case scanengine::ColorMode::kIntensity: s = &p.intensity; break;
    case scanengine::ColorMode::kTime: s = &p.time; break;
    default: break;
  }
  if (s) {
    s->gamma = float(gamma_->value());
    s->brightness = float(brightness_->value());
  }

  controller_->set(p);
  refreshFromModel();  // the controller clamps; re-read rather than assume
  Q_EMIT changed();
}

double InspectorCard::setPointSizeForCli(double px) {
  if (!point_size_ || !controller_) return 0.0;
  // Not setValue(): that one deliberately does not emit. Going through the
  // slider is the point — this has to be the same code path a drag takes.
  const double lo = 0.5, step = 0.5;
  point_size_->slider()->setValue(int(std::lround((px - lo) / step)));
  return controller_->get().point_size.fixed_px;
}

void InspectorCard::setGeorefState(bool converged, const QString& epsg, double sigma_h_m) {
  if (!export_btn_) return;
  export_btn_->setText(converged ? "  LAS 1.4 · georef ✓" : "  LAS 1.4 · local frame");
  if (converged) {
    QString sigma;
    if (sigma_h_m > 0.0) sigma = QString(" · σh %1 m").arg(sigma_h_m, 0, 'f', 3);
    georef_line_->setText(QString("%1 — georef converged ✓%2")
                              .arg(epsg.isEmpty() ? QString("no EPSG") : QString(epsg).replace(':', ' '),
                                   sigma));
    georef_line_->setStyleSheet(QString("font-family:'%1';font-size:9.5px;color:%2;")
                                    .arg(theme::monoFamily(), theme::css(theme::good())));
    export_btn_->setToolTip(
        "Export the live cloud. The georeference transform has converged, so "
        "Engine::crs_epsg() is non-empty and LAS 1.4 carries a real CRS.");
  } else {
    georef_line_->setText("local frame — LAS gets A9's placeholder CRS");
    georef_line_->setStyleSheet(QString("font-family:'%1';font-size:9.5px;color:%2;")
                                    .arg(theme::monoFamily(), theme::css(theme::faint())));
    export_btn_->setToolTip(
        "Export the live cloud. Engine::crs_epsg() is empty — the georeference "
        "transform has not converged — so a LAS written now embeds the documented "
        "local/ungeoreferenced placeholder WKT rather than a CRS it cannot back up.");
  }
}

void InspectorCard::setFloatingLook(bool floating) {
  if (floating_ == floating) return;
  floating_ = floating;
  if (floating) {
    setFixedWidth(kCardWidth);
    setMaximumHeight(kMaxHeightCap);
    layout()->setContentsMargins(2, 4, 2, 4);
  } else {
    setMinimumWidth(kCardWidth);
    setMaximumWidth(QWIDGETSIZE_MAX);
    setMaximumHeight(QWIDGETSIZE_MAX);
    layout()->setContentsMargins(0, 0, 0, 0);
  }
  update();
}

void InspectorCard::paintEvent(QPaintEvent* e) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  if (!floating_) {
    // Docked: flat panel, one top hairline, no radius — the mockup's
    // @media (max-width:880px) rule.
    p.fillRect(rect(), theme::panel());
    p.setPen(QPen(theme::line(), 1.0));
    p.drawLine(0, 0, width(), 0);
    QFrame::paintEvent(e);
    return;
  }

  const QRectF r(0.5, 0.5, width() - 1.0, height() - 1.0);
  QPainterPath card;
  card.addRoundedRect(r, theme::kRadiusPanel, theme::kRadiusPanel);

  // The baked translucency: panel at 94 % over the viewport's #0B0E12 ground.
  QColor fill = theme::panel();
  fill.setAlphaF(0.94);
  p.fillPath(card, fill);

  // A 1 px light border, brighter than the app's hairline — the mockup uses
  // rgba(96,110,126,.35), which reads as a rim-lit glass edge.
  p.setPen(QPen(QColor(96, 110, 126, 110), 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawPath(card);

  QFrame::paintEvent(e);
}

}  // namespace lidarscan
