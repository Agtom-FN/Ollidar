#include "ui/RecordCluster.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace lidarscan {
namespace {

// The leading glyph inside the main pill: a filled circle for "start", a
// rounded square for "stop". 15 px, matching the mockup's `.recmain i`.
QPixmap recGlyph(bool square, const QColor& c, qreal dpr) {
  if (dpr <= 0.0) dpr = 1.0;
  const int px = 15;
  QPixmap pm(int(px * dpr), int(px * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPainterPath path;
  if (square) {
    path.addRoundedRect(QRectF(1.0, 1.0, px - 2.0, px - 2.0), 4.0, 4.0);
  } else {
    path.addEllipse(QRectF(0.5, 0.5, px - 1.0, px - 1.0));
  }
  p.fillPath(path, c);
  p.end();
  return pm;
}

QString mmss(double seconds) {
  if (seconds < 0.0) seconds = 0.0;
  const long long total = static_cast<long long>(std::floor(seconds));
  return QString("%1:%2")
      .arg(total / 60, 2, 10, QChar('0'))
      .arg(total % 60, 2, 10, QChar('0'));
}

}  // namespace

RecordCluster::RecordCluster(QWidget* parent) : QWidget(parent) {
  setObjectName("recordCluster");
  setAutoFillBackground(true);
  setStyleSheet(QString("#recordCluster { background: %1; border-top: 1px solid %2; }")
                    .arg(theme::css(theme::panel2()), theme::css(theme::line())));

  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(16, 12, 16, 12);
  h->setSpacing(12);

  // 1. Test device -----------------------------------------------------
  test_ = new QPushButton("Test device", this);
  test_->setCursor(Qt::PointingHandCursor);
  test_->setMinimumHeight(38);
  test_->setToolTip("Pre-capture self-test — step 1 of the capture sequence");
  connect(test_, &QPushButton::clicked, this, &RecordCluster::testRequested);
  h->addWidget(test_);

  // 2. the large pill --------------------------------------------------
  main_ = new QPushButton(this);
  main_->setCursor(Qt::PointingHandCursor);
  main_->setFixedHeight(48);
  main_->setIconSize(QSize(15, 15));
  {
    QFont f(theme::displayFamily(), 15);
    f.setWeight(QFont::DemiBold);
    main_->setFont(f);
  }
  main_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  connect(main_, &QPushButton::clicked, this, [this] {
    if (state_ == State::kRecording || state_ == State::kPaused) {
      Q_EMIT stopRequested();
    } else if (state_ == State::kArmed) {
      Q_EMIT startRequested();
    }
    // In every other state the button is disabled, so this is unreachable —
    // the gate is not enforced here, it is enforced by CaptureWindow::onRecord()
    // refusing any phase but kReady. This widget just cannot ASK.
  });
  h->addWidget(main_);

  // 3. the clock -------------------------------------------------------
  auto* clockBox = new QWidget(this);
  auto* ch = new QHBoxLayout(clockBox);
  ch->setContentsMargins(4, 0, 4, 0);
  ch->setSpacing(9);

  dot_ = new Chip(clockBox);
  dot_->setDotVisible(true);
  dot_->setText(QString());
  dot_->setFixedSize(16, 16);
  dot_->setVisible(false);
  ch->addWidget(dot_);

  auto* clockCol = new QWidget(clockBox);
  auto* cv = new QVBoxLayout(clockCol);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  clock_ = new QLabel("00:00", clockCol);
  {
    QFont f(theme::monoFamily(), 20);
    f.setWeight(QFont::DemiBold);
    clock_->setFont(f);
  }
  clock_caption_ = new QLabel("ELAPSED", clockCol);
  {
    QFont f(theme::monoFamily(), 9);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
    clock_caption_->setFont(f);
  }
  clock_caption_->setStyleSheet(QString("color:%1;").arg(theme::css(theme::faint())));
  cv->addWidget(clock_);
  cv->addWidget(clock_caption_);
  ch->addWidget(clockCol);
  h->addWidget(clockBox);

  // 4. Pause / Resume --------------------------------------------------
  pause_ = new QPushButton("Pause", this);
  pause_->setCursor(Qt::PointingHandCursor);
  pause_->setMinimumHeight(38);
  pause_->setMinimumWidth(84);
  connect(pause_, &QPushButton::clicked, this, &RecordCluster::pauseResumeRequested);
  h->addWidget(pause_);

  // 5. the reason, pushed right ----------------------------------------
  h->addStretch(1);
  why_ = new QLabel(this);
  why_->setWordWrap(true);
  why_->setTextFormat(Qt::RichText);
  why_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  // A floor as well as a ceiling: without the minimum the stretch above
  // squeezes the sentence into a 4-word-per-line column as soon as the window
  // is anything short of wide, which is how the FIRST evidence run of this
  // widget came out.
  why_->setMinimumWidth(210);
  why_->setMaximumWidth(430);
  why_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  why_->setStyleSheet(QString("font-size:11.5px;color:%1;").arg(theme::css(theme::faint())));
  h->addWidget(why_, 0, Qt::AlignVCenter);

  badge_ = new QLabel(this);
  badge_->setProperty("badge", true);
  badge_->setAlignment(Qt::AlignCenter);
  // Fixed both ways, or the layout stretches the pill into a tall box and
  // wraps its own text — a badge is one line by definition.
  badge_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  h->addWidget(badge_, 0, Qt::AlignVCenter);

  // The cluster is the widest thing in the capture window, and it is meant to
  // stay on ONE row: Test | pill | clock | Pause | reason | badge.
  setMinimumWidth(880);

  refresh();
}

void RecordCluster::setState(State s) {
  if (state_ == s) return;
  state_ = s;
  if (s == State::kUntested || s == State::kFailed || s == State::kTesting) elapsed_s_ = 0.0;
  refresh();
}

void RecordCluster::setTransportIsD6(bool d6) {
  if (d6_ == d6) return;
  d6_ = d6;
  refresh();
}

void RecordCluster::setElapsedSeconds(double s) {
  elapsed_s_ = s;
  clock_->setText(mmss(s));
}

QString RecordCluster::gateSentence() const {
  switch (state_) {
    case State::kTesting:
      return "Self-test running — Start unlocks when it passes.";
    case State::kFailed:
      return "Self-test failed. Fix the link, then test the device again.";
    case State::kUntested:
      return QString("Run <b>Test device</b> first — %1 unlocks Start.")
          .arg(d6_ ? "≥ 3,000 pts/s over a 3 s window" : "the first packet within 8 s");
    case State::kArmed:
      return "Self-test passed — the engine is armed and ready to record.";
    case State::kRecording:
    case State::kPaused:
      return "Recording is open — the engine keeps every raw byte, paused or not.";
  }
  return QString();
}

QString RecordCluster::gateTooltip() const {
  return QString("Start recording is locked until the pre-capture self-test passes (%1).")
      .arg(d6_ ? "≥ 3,000 pts/s over 3 s" : "first packet within 8 s");
}

void RecordCluster::refresh() {
  const bool live = state_ == State::kRecording || state_ == State::kPaused;
  const bool paused = state_ == State::kPaused;
  const bool gated = state_ == State::kUntested || state_ == State::kTesting ||
                     state_ == State::kFailed;

  // --- the main pill ---
  main_->setText(live ? "  Stop recording" : "  Start recording");
  main_->setProperty("accent", live ? "danger" : "ember");
  main_->setEnabled(live || state_ == State::kArmed);
  main_->setIcon(recGlyph(live, live ? QColor("#FF6B6B") : QColor("#1A0D08"),
                          devicePixelRatioF()));
  main_->setToolTip(live ? "Stop recording and write the session summary"
                         : gated ? gateTooltip()
                                 : "Start recording into the .lscan project");
  // A property change needs an explicit re-polish; QSS is matched at polish
  // time, not on every paint.
  main_->style()->unpolish(main_);
  main_->style()->polish(main_);

  // --- Test device ---
  test_->setEnabled(state_ == State::kUntested || state_ == State::kFailed);

  // --- Pause / Resume ---
  pause_->setText(paused ? "Resume" : "Pause");
  pause_->setEnabled(live);
  pause_->setToolTip(live ? (paused ? "Resume the open session"
                                    : "Pause — the recording stays open")
                          : "Nothing is recording");

  // --- clock ---
  clock_->setStyleSheet(
      QString("color:%1;").arg(theme::css(live ? theme::ink() : theme::faint())));
  clock_caption_->setText(paused ? "PAUSED" : "ELAPSED");
  clock_->setText(mmss(elapsed_s_));
  dot_->setVisible(live);
  dot_->setTone(paused ? Chip::Tone::kWarn : Chip::Tone::kBad);
  dot_->setDotPulsing(state_ == State::kRecording);

  // --- the sentence and the badge ---
  why_->setText(gateSentence());
  if (live) {
    badge_->setText(paused ? "PAUSED" : "REC");
    badge_->setProperty("tone", "bad");
  } else if (gated) {
    badge_->setText("SELF-TEST REQUIRED");
    badge_->setProperty("tone", "warn");
  } else {
    badge_->setText("ARMED");
    badge_->setProperty("tone", "good");
  }
  badge_->style()->unpolish(badge_);
  badge_->style()->polish(badge_);
}

}  // namespace lidarscan
