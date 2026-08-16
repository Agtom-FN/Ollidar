#include "ui/Widgets.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <cmath>

#include "ui/Theme.h"

namespace lidarscan {
namespace {

constexpr int kChipPadH = 9;
constexpr int kChipPadV = 3;
constexpr int kDotSize = 7;
constexpr int kDotGap = 6;

}  // namespace

// ---------------------------------------------------------------- Chip -----

Chip::Chip(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents, true);
  QFont f(theme::monoFamily(), 10);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
  setFont(f);
}

void Chip::setText(const QString& t) {
  if (text_ == t) return;
  text_ = t;
  updateGeometry();
  update();
}

void Chip::setTone(Tone t) {
  tone_ = t;
  update();
}

void Chip::setDotVisible(bool on) {
  if (dot_ == on) return;
  dot_ = on;
  updateGeometry();
  update();
}

void Chip::setEmphasis(bool on) {
  if (emphasis_ == on) return;
  emphasis_ = on;
  QFont f(on ? theme::monoFamily() : theme::monoFamily(), on ? 11 : 10);
  f.setBold(on);
  f.setLetterSpacing(QFont::AbsoluteSpacing, on ? 1.1 : 0.6);
  setFont(f);
  updateGeometry();
  update();
}

void Chip::setDotPulsing(bool on) {
  if (pulsing_ == on) return;
  pulsing_ = on;
  if (on) {
    // 1.15 s period, sampled at ~14 Hz: enough to read as a pulse without
    // waking the GUI thread on every vsync.
    auto* t = new QTimer(this);
    t->setObjectName("chipPulse");
    connect(t, &QTimer::timeout, this, [this] {
      pulse_phase_ = (pulse_phase_ + 1) % 16;
      update();
    });
    t->start(72);
  } else {
    if (auto* t = findChild<QTimer*>("chipPulse")) {
      t->stop();
      t->deleteLater();
    }
    pulse_phase_ = 0;
    update();
  }
}

QColor Chip::toneColor() const {
  switch (tone_) {
    case Tone::kGood: return theme::good();
    case Tone::kWarn: return theme::warn();
    case Tone::kBad: return theme::bad();
    case Tone::kEmber: return theme::ember();
    case Tone::kPose: return theme::pose();
    case Tone::kNone: break;
  }
  return theme::mute();
}

QSize Chip::sizeHint() const {
  const QFontMetrics fm(font());
  int w = fm.horizontalAdvance(text_) + 2 * kChipPadH;
  if (dot_) w += kDotSize + kDotGap;
  const int h = fm.height() + 2 * kChipPadV;
  return QSize(w, qMax(h, 20));
}

void Chip::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const QRectF r(0.5, 0.5, width() - 1.0, height() - 1.0);
  const qreal radius = r.height() / 2.0;
  QPainterPath pill;
  pill.addRoundedRect(r, radius, radius);

  QColor fill;
  QColor border;
  QColor textColor;
  if (tone_ == Tone::kNone) {
    fill = theme::panel();
    fill.setAlphaF(0.86);
    border = theme::panel3();
    border.setAlphaF(0.75);
    textColor = theme::mute();
  } else {
    const QColor t = toneColor();
    fill = t;
    fill.setAlphaF(0.13);
    border = t;
    border.setAlphaF(0.45);
    textColor = t;
  }

  p.fillPath(pill, fill);
  p.setPen(QPen(border, 1.0));
  p.setBrush(Qt::NoBrush);
  p.drawPath(pill);

  int textX = kChipPadH;
  if (dot_) {
    QColor dot = tone_ == Tone::kNone ? theme::good() : toneColor();
    if (pulsing_) {
      // 0 -> 1 -> 0 triangle over the 16-step phase, floored at .22 like the
      // mockup's keyframes.
      const double t = std::abs(8 - pulse_phase_) / 8.0;
      dot.setAlphaF(0.22 + 0.78 * t);
    }
    QPainterPath d;
    d.addEllipse(QPointF(kChipPadH + kDotSize / 2.0, height() / 2.0), kDotSize / 2.0,
                 kDotSize / 2.0);
    p.fillPath(d, dot);
    textX += kDotSize + kDotGap;
  }

  p.setPen(textColor);
  p.setFont(font());
  p.drawText(QRect(textX, 0, width() - textX - kChipPadH, height()),
             Qt::AlignVCenter | Qt::AlignLeft, text_);
}

// ----------------------------------------------------------- SliderRow -----

SliderRow::SliderRow(const QString& label, double lo, double hi, double step, QWidget* parent)
    : QWidget(parent), lo_(lo), hi_(hi), step_(step > 0 ? step : 0.1) {
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 4, 0, 4);
  h->setSpacing(8);

  label_ = new QLabel(label, this);
  label_->setProperty("role", "mono");
  label_->setStyleSheet(QString("color:%1;font-size:10px;").arg(theme::css(theme::mute())));
  label_->setFixedWidth(58);
  label_->setWordWrap(false);

  slider_ = new QSlider(Qt::Horizontal, this);
  const int steps = int(std::lround((hi_ - lo_) / step_));
  slider_->setRange(0, qMax(1, steps));
  slider_->setSingleStep(1);
  slider_->setPageStep(qMax(1, steps / 10));

  readout_ = new QLabel(this);
  readout_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  readout_->setStyleSheet(QString("font-family:'%1';font-size:10px;color:%2;")
                              .arg(theme::monoFamily(), theme::css(theme::ink())));
  readout_->setFixedWidth(52);

  h->addWidget(label_);
  h->addWidget(slider_, 1);
  h->addWidget(readout_);

  connect(slider_, &QSlider::valueChanged, this, [this](int) {
    refreshReadout();
    if (!updating_) Q_EMIT valueChanged(value());
  });
  refreshReadout();
}

double SliderRow::value() const { return lo_ + slider_->value() * step_; }

void SliderRow::setValue(double v) {
  updating_ = true;
  const int idx = int(std::lround((v - lo_) / step_));
  slider_->setValue(qBound(slider_->minimum(), idx, slider_->maximum()));
  updating_ = false;
  refreshReadout();
}

void SliderRow::setFormat(const QString& fmt) {
  fmt_ = fmt;
  refreshReadout();
}

void SliderRow::setSuffix(const QString& s) {
  suffix_ = s;
  refreshReadout();
}

void SliderRow::setEnabled(bool on) {
  QWidget::setEnabled(on);
  const QColor c = on ? theme::ink() : theme::faint();
  readout_->setStyleSheet(QString("font-family:'%1';font-size:10px;color:%2;")
                              .arg(theme::monoFamily(), theme::css(c)));
}

void SliderRow::refreshReadout() {
  const double v = value();
  QString s;
  if (fmt_ == "px") s = QString::number(v, 'f', 1) + " px";
  else if (fmt_ == "m") s = QString::number(v, 'f', 2) + " m";
  else if (fmt_ == "M") s = QString::number(v, 'f', 0) + " M";
  else if (fmt_ == "i") s = QString::number(std::lround(v));
  else if (fmt_ == "1") s = QString::number(v, 'f', 1);
  else if (fmt_ == "3") s = QString::number(v, 'f', 3);
  else s = QString::number(v, 'f', 2);
  readout_->setText(s + suffix_);
}

}  // namespace lidarscan
