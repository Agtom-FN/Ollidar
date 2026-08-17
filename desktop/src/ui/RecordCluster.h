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
// capture flow.
//
// ROUND 5 (REVIEW_FEEDBACK 2026-08-17 items 7/10) RETIRED THE SELF-TEST GATE.
// "Live preview showing points IS the proof a device works", so the "Test
// device" button, the SELF-TEST REQUIRED badge and the "Start unlocks when the
// self-test passes" sentence are gone: the states below are the live-preview
// machine (no device -> arming -> live -> recording/paused), Start is enabled
// the moment a device is streaming, and the sentence says what the app is doing
// rather than what the operator must do first.
//
// Owner: redesign pass / round-5 workflow pass.
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
  // phase enum does not carry: an arm that produced no packets also lands in
  // kIdle, and the sentence has to say something different about it.
  enum class State {
    kNoDevice,   // Phase::kIdle,    nothing armed (auto-detect has not found one yet)
    kArming,     // Phase::kArming
    kNoData,     // Phase::kIdle,    armed but no packet arrived — the honest failure
    kLive,       // Phase::kPreview, streaming into the viewport, not recording
    kRecording,  // Phase::kRecording
    kPaused,     // Phase::kPaused
  };

  explicit RecordCluster(QWidget* parent = nullptr);

  void setState(State s);
  State state() const { return state_; }

  // Seconds spent in the Recording state; rendered MM:SS, floored, zero
  // padded. CaptureWindow feeds this from the same accumulator the session
  // summary uses, so the clock and the summary can never disagree.
  void setElapsedSeconds(double s);

 Q_SIGNALS:
  void startRequested();
  void pauseResumeRequested();
  void stopRequested();

 private:
  void refresh();
  QString stateSentence() const;

  State state_ = State::kNoDevice;
  double elapsed_s_ = 0.0;

  QPushButton* main_ = nullptr;   // Start recording / Stop recording
  QPushButton* pause_ = nullptr;  // Pause / Resume
  Chip* dot_ = nullptr;
  QLabel* clock_ = nullptr;
  QLabel* clock_caption_ = nullptr;
  QLabel* why_ = nullptr;
  QLabel* badge_ = nullptr;
};

}  // namespace lidarscan
