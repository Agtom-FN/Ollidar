// RecordCluster.h — the owner's round-1 item 2, in Qt.
//
//   "Desktop (macOS + Windows) · Capture: start-record / end-record buttons
//    missing — must be prominent."
//   → resolved in mockup v4 as: "Desktop capture has a record cluster pinned
//     under the viewport on both shells: Test device → Start recording (large
//     ember pill) → Pause/Resume → Stop recording (red-tinted) with a ticking
//     elapsed clock and a REC/PAUSED/ARMED badge. Start stays disabled until
//     the self-test passes and states the threshold in words."
//   (docs/design/REVIEW_FEEDBACK.md, 2026-08-16 round 1.)
//
// WHAT IT REPLACES. C2 put Test / Record / Pause / Stop in one QHBoxLayout of
// four identical default-sized QPushButtons, inside a QFormLayout row, inside
// a "Session" QGroupBox, below two device tabs and above a health line, a
// summary line and a log pane. Four 12 px pills in the middle of a form is
// exactly the "buried" the owner is objecting to. This widget is the ONE
// obvious cluster that replaces them; CaptureWindow no longer builds those
// buttons at all.
//
// IT ADDS NO STATE. Every transition is still CaptureWindow::Phase's; this
// widget renders a Phase and emits an intent. It cannot start, stop or gate
// anything by itself, which is what makes it safe to drop onto a shipped
// capture flow — the self-test gate is still evaluateSelfTest()'s
// >= 3,000 pts/s over 3 s (D6) / first packet within 8 s (Mid-360), and this
// widget only says so out loud.
//
// Owner: redesign pass.
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace lidarscan {

class Chip;

class RecordCluster : public QWidget {
  Q_OBJECT
 public:
  // A 1:1 rendering of CaptureWindow::Phase, plus the one distinction the
  // phase enum does not carry: a failed self-test also lands in kIdle, and the
  // gate sentence has to say something different about it.
  enum class State {
    kUntested,   // Phase::kIdle,   no self-test has passed yet
    kTesting,    // Phase::kTesting
    kFailed,     // Phase::kIdle,   after a self-test failed
    kArmed,      // Phase::kReady
    kRecording,  // Phase::kRecording
    kPaused,     // Phase::kPaused
  };

  explicit RecordCluster(QWidget* parent = nullptr);

  void setState(State s);
  State state() const { return state_; }

  // Selects the D6 or Mid-360 wording of the gate sentence and tooltip.
  void setTransportIsD6(bool d6);

  // Seconds spent in the Recording state; rendered MM:SS, floored, zero
  // padded. CaptureWindow feeds this from the same accumulator the session
  // summary uses, so the clock and the summary can never disagree.
  void setElapsedSeconds(double s);

 Q_SIGNALS:
  void testRequested();
  void startRequested();
  void pauseResumeRequested();
  void stopRequested();

 private:
  void refresh();
  QString gateSentence() const;
  QString gateTooltip() const;

  State state_ = State::kUntested;
  bool d6_ = true;
  double elapsed_s_ = 0.0;

  QPushButton* test_ = nullptr;
  QPushButton* main_ = nullptr;   // Start recording / Stop recording
  QPushButton* pause_ = nullptr;  // Pause / Resume
  Chip* dot_ = nullptr;
  QLabel* clock_ = nullptr;
  QLabel* clock_caption_ = nullptr;
  QLabel* why_ = nullptr;
  QLabel* badge_ = nullptr;
};

}  // namespace lidarscan
