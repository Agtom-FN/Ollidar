// Widgets.h — the small shared pieces the redesign needs in more than one
// place: the pill "chip" (viewport overlays, state badges), and the
// label / slider / mono-readout row the inspector is built from.
//
// Both are drawn rather than styled where QSS cannot express the design (a
// chip's leading status dot, a pill whose radius is exactly half its height at
// any font size). Everything else defers to Theme's stylesheet.
//
// Owner: redesign pass.
#pragma once

#include <QSlider>
#include <QString>
#include <QWidget>

class QLabel;

namespace lidarscan {

// A pill. `tone` picks the semantic colour; kNone is the neutral panel chip
// the viewport stats use.
class Chip : public QWidget {
  Q_OBJECT
 public:
  enum class Tone { kNone, kGood, kWarn, kBad, kEmber, kPose };

  explicit Chip(QWidget* parent = nullptr);

  void setText(const QString& t);
  QString text() const { return text_; }
  void setTone(Tone t);
  // Draws a filled status dot before the text, in the tone colour.
  void setDotVisible(bool on);
  // Pulses the dot at ~1.15 s, the mockup's `recpulse`. Only meaningful with
  // the dot shown; honours the "reduce motion" case by simply being off.
  void setDotPulsing(bool on);
  // Uses the display face at a larger size — for the RECORDING badge.
  void setEmphasis(bool on);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override { return sizeHint(); }

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  QColor toneColor() const;

  QString text_;
  Tone tone_ = Tone::kNone;
  bool dot_ = false;
  bool pulsing_ = false;
  bool emphasis_ = false;
  int pulse_phase_ = 0;
};

// One inspector row: a mono label, an ember-filled slider, and a right-aligned
// mono readout. The slider is integer-valued underneath (Qt has no double
// slider), so the row owns the mapping between the model's double and the
// slider's step index — one place, rather than at every call site.
class SliderRow : public QWidget {
  Q_OBJECT
 public:
  SliderRow(const QString& label, double lo, double hi, double step, QWidget* parent = nullptr);

  double value() const;
  void setValue(double v);  // does not emit valueChanged()

  // Re-range an existing row, keeping the current value where it still fits and
  // clamping it where it does not. Added for the live refresh-rate row, whose
  // MAXIMUM is not a design constant but this machine's own display refresh rate
  // (round-5 item 17) — which is only known once there is a window on a screen.
  // Does not emit valueChanged().
  void setRange(double lo, double hi, double step);

  // "px" -> "2.0 px", "M" -> "8 M", "m" -> "3.20 m", "1"/"2"/"3" -> fixed
  // decimals, "i" -> integer. Matches the mockup's fmtVal().
  void setFormat(const QString& fmt);
  void setSuffix(const QString& s);

  void setEnabled(bool on);
  QSlider* slider() { return slider_; }

 Q_SIGNALS:
  void valueChanged(double v);

 private:
  void refreshReadout();

  QLabel* label_ = nullptr;
  QSlider* slider_ = nullptr;
  QLabel* readout_ = nullptr;
  double lo_ = 0.0, hi_ = 1.0, step_ = 0.1;
  QString fmt_ = "2";
  QString suffix_;
  bool updating_ = false;
};

}  // namespace lidarscan
