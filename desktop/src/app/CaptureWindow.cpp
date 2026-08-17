#include "app/CaptureWindow.h"

#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollArea>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/DeviceDiscovery.h"
#include "app/EngineHost.h"
#include "ui/RecordCluster.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace lidarscan {
namespace {

// A dynamic property change (tone=good/warn/bad, the ember accent, …) needs an
// explicit re-polish once the widget has already been shown once — QSS property
// selectors are matched at polish time.
void repolish(QWidget* w) {
  w->style()->unpolish(w);
  w->style()->polish(w);
}

QString humanBytesLocal(quint64 b) {
  static const char* u[] = {"B", "KB", "MB", "GB"};
  double v = double(b);
  int i = 0;
  while (v >= 1024.0 && i < 3) {
    v /= 1024.0;
    ++i;
  }
  return QString("%1 %2").arg(v, 0, 'f', i ? 1 : 0).arg(u[i]);
}

QLabel* sectionLabel(const QString& text) {
  auto* l = new QLabel(text.toUpper());
  QFont f(theme::monoFamily(), 9);
  f.setBold(true);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
  l->setFont(f);
  l->setStyleSheet(QString("color:%1;").arg(theme::css(theme::faint())));
  return l;
}

QLabel* hintLabel(const QString& text) {
  auto* l = new QLabel(text);
  l->setWordWrap(true);
  l->setProperty("role", "hint");
  return l;
}

// Windows forbids ':' in a path and macOS Finder renders it as '/', so the
// owner's example ("Scan-014 2026-08-17 19:32") becomes 19-32 on disk. Spaces
// are kept: they are legal everywhere and the owner asked for that shape.
constexpr const char* kAutoNameTimeFormat = "yyyy-MM-dd HH-mm";

}  // namespace

CaptureWindow::CaptureWindow(EngineHost* host, scanengine::DisplayParamsController* params,
                             QWidget* parent)
    : QDockWidget("CAPTURE — NEW SCAN", parent), host_(host), params_(params) {
  setObjectName("captureDock");
  setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  buildUi();
  setPhase(Phase::kIdle);

  health_timer_ = new QTimer(this);
  connect(health_timer_, &QTimer::timeout, this, &CaptureWindow::updateHealth);
  health_timer_->start(300);

  // Item 18: the trail is polled at 10 Hz — LioPoseSource exposes latest()/
  // size()/trajectory_length_m() but NO way to enumerate the ring (see NOTES.md
  // §17's engine-seam list), so the app accumulates the path itself from the
  // newest pose. 10 Hz matches LIO's own pose rate; the viewport coalesces
  // whatever arrives into one rebuild per presented frame.
  trajectory_timer_ = new QTimer(this);
  connect(trajectory_timer_, &QTimer::timeout, this, &CaptureWindow::pollTrajectory);
  trajectory_timer_->start(100);
}

CaptureWindow::~CaptureWindow() {
  // Recording must be sealed, and the device/session must be gone, before the
  // engine host outlives us. Both are idempotent.
  if (phase_ == Phase::kRecording || phase_ == Phase::kPaused) onStop();
  if (phase_ != Phase::kIdle) disarmPreview("shutdown");
}

void CaptureWindow::setProjectDir(const QString& dir) {
  if (dir.isEmpty()) return;
  // Round 5: capture creates projects, it does not live inside one. A .lscan
  // path names the project a caller (a CLI hook) wants; its PARENT is the root
  // new projects go into.
  const QFileInfo fi(dir);
  project_root_ = dir.endsWith(".lscan", Qt::CaseInsensitive) ? fi.absolutePath()
                                                              : fi.absoluteFilePath();
  QSettings().setValue("capture/root", project_root_);
  updateNameHint();
}

QString CaptureWindow::captureRoot() const {
  if (!project_root_.isEmpty()) return project_root_;
  const QString saved = QSettings().value("capture/root").toString();
  if (!saved.isEmpty()) return saved;
  // Documents, not the home directory. The first default here was
  // `~/LidarScan`, which on a case-insensitive macOS filesystem is the SAME
  // DIRECTORY as a checkout named `~/lidarscan` — the first verification run
  // wrote eight .lscan projects into the middle of the source tree. Documents is
  // where a scan belongs anyway, and QStandardPaths gets the localized/redirected
  // (OneDrive, XDG) path right on all three platforms.
  QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (docs.isEmpty()) docs = QDir::homePath();
  return docs + "/LidarScan Projects";
}

void CaptureWindow::showEvent(QShowEvent* event) {
  QDockWidget::showEvent(event);
  // A device-arming CLI hook owns this run: the on-open pass must not fire at
  // all, or it holds UDP 56201 while the hook's SdkInit tries to bind it
  // (NOTES.md §16.7).
  if (suppress_silent_auto_detect_) {
    if (!auto_detect_ran_for_session_) {
      auto_detect_ran_for_session_ = true;
      log("auto-detect (on open) suppressed — a device-arming CLI hook owns this run "
          "and needs UDP 56201");
    }
    return;
  }
  // Round 5 item 7: opening the capture workspace IS the auto-detect step. It
  // runs once per app run (not once per project — there is no project yet),
  // inline, and a hit arms a live preview by itself.
  if (!auto_detect_ran_for_session_ && phase_ == Phase::kIdle && !discovery_in_flight_) {
    auto_detect_ran_for_session_ = true;
    startDiscovery(/*silent=*/true);
  }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void CaptureWindow::buildUi() {
  auto* body = new QWidget();
  auto* v = new QVBoxLayout(body);
  v->setContentsMargins(14, 10, 14, 0);
  v->setSpacing(8);

  // Four columns across the foot of the shell, with the live viewport directly
  // above them: devices · link · new scan · live display. No tabs (the D6 tab
  // that made them necessary is gone), no dialogs, nothing to open.
  auto* cols = new QWidget();
  auto* ch = new QHBoxLayout(cols);
  ch->setContentsMargins(0, 0, 0, 0);
  ch->setSpacing(18);
  ch->addWidget(buildDeviceColumn(), 3);
  ch->addWidget(buildLinkColumn(), 2);
  ch->addWidget(buildScanColumn(), 3);
  ch->addWidget(buildDisplayColumn(), 3);
  // In a SCROLL AREA, and this is not decoration: a dock's height is the user's
  // to drag, and the manual-setup row (round-5 follow-up item 1) appears at
  // runtime — the first evidence run of this panel had the Mid-360 form, its
  // Connect button and its hint drawn ON TOP of each other, because Qt squeezes
  // past a layout's minimum rather than clipping when the space is not there.
  // With this, a short dock scrolls; nothing ever overlaps.
  auto* scroll = new QScrollArea(body);
  scroll->setWidget(cols);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->viewport()->setAutoFillBackground(false);
  scroll->setStyleSheet(
      "QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
  v->addWidget(scroll, 1);

  // A compact log strip, full width. Small on purpose: the shell's LOG dock has
  // the scrollback, this is the last few lines in the operator's eyeline.
  log_ = new QPlainTextEdit(body);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(200);
  log_->setFixedHeight(66);
  v->addWidget(log_);

  // --- THE record cluster (one Start, one Stop) ----------------------------
  record_cluster_ = new RecordCluster(body);
  connect(record_cluster_, &RecordCluster::startRequested, this, &CaptureWindow::onStart);
  connect(record_cluster_, &RecordCluster::pauseResumeRequested, this,
          &CaptureWindow::onPauseResume);
  connect(record_cluster_, &RecordCluster::stopRequested, this, &CaptureWindow::onStop);
  v->addWidget(record_cluster_);

  // The clock ticks at 4 Hz off its own timer rather than the 300 ms health
  // timer, so the seconds digit never visibly stalls.
  elapsed_timer_ = new QTimer(this);
  connect(elapsed_timer_, &QTimer::timeout, this, [this] {
    if (record_cluster_) record_cluster_->setElapsedSeconds(recordedSecondsNow());
  });
  elapsed_timer_->start(250);

  setWidget(body);

  loadMid360Settings();
  refreshDisplayControls();
  updateNameHint();
}

QWidget* CaptureWindow::buildDeviceColumn() {
  auto* w = new QWidget();
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  v->addWidget(sectionLabel("Devices"));
  buildAutoDetectSection(v);
  v->addStretch(1);
  return w;
}

QWidget* CaptureWindow::buildLinkColumn() {
  auto* w = new QWidget();
  auto* outer = new QVBoxLayout(w);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(4);
  outer->addWidget(sectionLabel("Mid-360 link"));

  // Round-5 follow-up item 1. Auto-detect is the normal path, so the manual
  // fields start COLLAPSED — but they are one inline click away at any time
  // ("Manual setup"), and a detect pass that finds nothing opens them itself
  // (handleDiscoveryFinished) with the cursor in the lidar IP field. No dialog.
  manual_box_ = new QWidget();
  auto* v = new QVBoxLayout(manual_box_);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  outer->addWidget(manual_box_);

  auto* f = new QFormLayout();
  f->setContentsMargins(0, 0, 0, 0);
  f->setSpacing(4);
  host_ip_ = new QLineEdit("192.168.1.5");
  lidar_ip_ = new QLineEdit("192.168.1.100");
  f->addRow("Host IP", host_ip_);
  f->addRow("Lidar IP", lidar_ip_);

  // The three ports are defaults nobody has ever needed to change in the field;
  // they stay reachable (this is not a wizard that hides state) but on one row
  // rather than three, because the operator's eye belongs on Start.
  auto* ports = new QWidget();
  auto* pl = new QHBoxLayout(ports);
  pl->setContentsMargins(0, 0, 0, 0);
  pl->setSpacing(4);
  auto mkPort = [&](int value, const QString& tip) {
    auto* s = new QSpinBox();
    s->setRange(1, 65535);
    s->setValue(value);
    s->setToolTip(tip);
    pl->addWidget(s);
    return s;
  };
  point_port_ = mkPort(56300, "Point-cloud UDP port");
  imu_port_ = mkPort(56400, "IMU UDP port");
  cmd_port_ = mkPort(56100, "Command UDP port");
  f->addRow("Ports (point/imu/cmd)", ports);
  v->addLayout(f);

  connect_btn_ = new QPushButton("Connect");
  connect_btn_->setProperty("accent", "ember");
  connect_btn_->setCursor(Qt::PointingHandCursor);
  connect_btn_->setMinimumHeight(32);
  connect_btn_->setToolTip(
      "Arm the Mid-360 with the addresses above and start the live preview. Same code "
      "path an auto-detect hit takes.");
  connect(connect_btn_, &QPushButton::clicked, this, &CaptureWindow::onConnect);
  v->addWidget(connect_btn_);

  mid_hint_ = hintLabel(
      "Auto-detect normally fills these in. The lidar IP is REQUIRED on macOS (stock "
      "SDK2 broadcast discovery fails with EADDRNOTAVAIL there — S2-sim finding).");
  v->addWidget(mid_hint_);

  manual_box_->setVisible(false);

  outer->addSpacing(6);
  outer->addWidget(sectionLabel("RTK (UM982)"));
  // From here on the RTK block is always visible (it holds a probe result, it is
  // not a manual entry path — there is no engine seam to connect it to).
  v = outer;
  auto* rf = new QFormLayout();
  rf->setContentsMargins(0, 0, 0, 0);
  rf->setSpacing(4);
  um982_port_ = new QComboBox();
  um982_port_->setEditable(true);  // the probe hit may not be enumerated by name yet
  rf->addRow("Serial port", um982_port_);
  um982_baud_ = new QSpinBox();
  um982_baud_->setRange(4800, 921600);
  um982_baud_->setValue(115200);  // Unicore factory default; the field unit needed 230400
  rf->addRow("Baud", um982_baud_);
  v->addLayout(rf);
  um982_heading_ = new QLabel("Dual-antenna heading: unknown");
  um982_heading_->setWordWrap(true);
  v->addWidget(um982_heading_);
  um982_hint_ = hintLabel(
      "Not wired into Start yet: these hold what Auto-detect found so it is not lost. "
      "No engine seam opens a GNSS serial port the way the Mid-360 is opened — "
      "NOTES.md §16.2/§17.");
  v->addWidget(um982_hint_);
  v->addStretch(1);
  return w;
}

QWidget* CaptureWindow::buildScanColumn() {
  auto* w = new QWidget();
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  v->addWidget(sectionLabel("New scan"));

  // ONE field. Round 5 item 9: Start always creates a NEW project, and an empty
  // name is the normal case, not an error to be validated.
  name_edit_ = new QLineEdit();
  name_edit_->setPlaceholderText("Project name (optional)");
  name_edit_->setClearButtonEnabled(true);
  name_edit_->setMinimumHeight(32);
  connect(name_edit_, &QLineEdit::textChanged, this, [this](const QString&) { updateNameHint(); });
  v->addWidget(name_edit_);

  name_hint_ = new QLabel();
  name_hint_->setWordWrap(true);
  name_hint_->setTextFormat(Qt::PlainText);
  name_hint_->setStyleSheet(QString("font-family:'%1';font-size:10px;color:%2;")
                                .arg(theme::monoFamily(), theme::css(theme::faint())));
  v->addWidget(name_hint_);

  auto* f = new QFormLayout();
  f->setContentsMargins(0, 4, 0, 0);
  f->setSpacing(4);
  profile_ = new QComboBox();
  profile_->addItems({"quickscan", "survey", "floorplan", "research"});
  profile_->setToolTip(
      "The workflow profile written into the project's manifest — it also picks the "
      "display-parameter defaults A14 hands the viewport when the project is opened.");
  f->addRow("Profile", profile_);
  v->addLayout(f);

  arm_label_ = new QLabel();
  arm_label_->setWordWrap(true);
  v->addWidget(arm_label_);

  // Item 18, walkthrough-first: the operator is walking, so the panel says how
  // far they have walked, how fast, and — gently — when that is too fast.
  walk_label_ = new QLabel();
  walk_label_->setWordWrap(true);
  walk_label_->setVisible(false);
  v->addWidget(walk_label_);

  health_ = new QLabel("idle");
  health_->setWordWrap(true);
  health_->setStyleSheet(QString("font-family:'%1';font-size:10px;").arg(theme::monoFamily()));
  v->addWidget(health_);

  summary_ = new QLabel();
  summary_->setWordWrap(true);
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(summary_);
  v->addStretch(1);
  return w;
}

QWidget* CaptureWindow::buildDisplayColumn() {
  auto* w = new QWidget();
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(2);
  v->addWidget(sectionLabel("Live display"));

  // The refresh rate is NOT an A14 parameter (see the header): it is how often
  // the window presents, and it is here because the owner asked for it next to
  // the other live controls.
  // 60 is a placeholder maximum only: item 17 makes the ceiling this machine's
  // own display refresh rate, and MainWindow re-ranges the row through
  // setLiveRefreshCeiling() as soon as the viewport has a screen.
  refresh_hz_ = new SliderRow("refresh", 2.0, 60.0, 1.0, w);
  refresh_hz_->setFormat("i");
  refresh_hz_->setSuffix(" fps");
  refresh_hz_->setToolTip(
      "Live viewport refresh cap (ViewportWindow::setMaxFps). The maximum is THIS "
      "machine's display refresh rate; if a frame cannot be sustained the app steps the "
      "cap down by itself and says so below. The display link still ticks at the "
      "display's own rate — a tick that arrives early returns without syncing the cloud "
      "or presenting, which is where a frame's cost is. RECORDING IS NEVER THROTTLED: "
      "this caps what is drawn, never what is captured.");
  v->addWidget(refresh_hz_);

  refresh_note_ = new QLabel();
  refresh_note_->setWordWrap(true);
  refresh_note_->setVisible(false);
  v->addWidget(refresh_note_);

  // Round-5 follow-up item 2, verbatim: "POINT SIZE range: min 0.1, max 3.0,
  // step 0.1 (px)". ENGINE SEAM MISSING (documented in NOTES.md §17): A14's own
  // controller clamps fixed_px to [0.5, 64.0] (engine/src/cloud/
  // display_params.cpp:206) and engine/** is read-only here, so a value under
  // 0.5 is accepted by this slider and comes back as 0.5 from the model — which
  // the readout then shows, because refreshDisplayControls() re-reads rather
  // than trusting what it sent.
  point_size_ = new SliderRow("point size", 0.1, 3.0, 0.1, w);
  point_size_->setFormat("2");
  point_size_->setSuffix(" px");
  point_size_->setToolTip(
      "Point size in pixels. A14 clamps below 0.5 px, so the readout snaps back there.");
  v->addWidget(point_size_);

  gamma_ = new SliderRow("gamma", 0.1, 4.0, 0.05, w);
  gamma_->setFormat("2");
  v->addWidget(gamma_);

  brightness_ = new SliderRow("brightness", 0.1, 3.0, 0.05, w);
  brightness_->setFormat("2");
  v->addWidget(brightness_);

  connect(refresh_hz_, &SliderRow::valueChanged, this, [this](double hz) {
    if (updating_display_) return;
    QSettings().setValue("capture/liveRefreshHz", hz);
    Q_EMIT liveRefreshHzChanged(hz);
  });
  for (SliderRow* r : {point_size_, gamma_, brightness_}) {
    connect(r, &SliderRow::valueChanged, this, [this](double) { pushDisplayParams(); });
  }

  auto* f = new QFormLayout();
  f->setContentsMargins(0, 4, 0, 0);
  f->setSpacing(4);
  color_mode_ = new QComboBox();
  for (int i = 0; i < scanengine::kColorModeCount; ++i) {
    color_mode_->addItem(scanengine::to_string(static_cast<scanengine::ColorMode>(i)));
  }
  color_mode_->setToolTip(
      "A14 colour mode. time and fixQuality have no per-point source in PointVertex and "
      "fall back to RGB — they stay reachable and say so rather than being hidden.");
  f->addRow("colour", color_mode_);
  colormap_ = new QComboBox();
  for (int i = 0; i < scanengine::kColormapCount; ++i) {
    colormap_->addItem(scanengine::to_string(static_cast<scanengine::Colormap>(i)));
  }
  f->addRow("colormap", colormap_);
  v->addLayout(f);
  connect(color_mode_, &QComboBox::currentIndexChanged, this,
          [this](int) { pushDisplayParams(); });
  connect(colormap_, &QComboBox::currentIndexChanged, this, [this](int) { pushDisplayParams(); });

  v->addWidget(hintLabel(
      "Same A14 model as the DISPLAY panel beside this one, live during preview AND "
      "recording — and saved with the project when the scan is sealed. Display refresh "
      "adapts to what this machine can sustain; the capture itself never does."));
  v->addStretch(1);
  return w;
}

// ---------------------------------------------------------------------------
// Live display controls
// ---------------------------------------------------------------------------

void CaptureWindow::refreshDisplayControls() {
  if (!params_ || !point_size_) return;
  updating_display_ = true;
  const auto p = params_->get();

  point_size_->setValue(p.point_size.fixed_px);
  const bool fixed = p.point_size.mode == scanengine::PointSizeMode::kFixedPixels;
  point_size_->setEnabled(fixed);
  point_size_->setToolTip(fixed ? QString("Point size in pixels (A14 kFixedPixels).")
                                : QString("Point size is in %1 mode — set it in the "
                                          "DISPLAY panel.")
                                      .arg(scanengine::to_string(p.point_size.mode)));

  // Gamma/brightness/colormap live on the ACTIVE scalar channel, exactly as
  // DisplayParamsDock and InspectorCard resolve them.
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
  colormap_->setCurrentIndex(static_cast<int>(shown.colormap));
  colormap_->setEnabled(s != nullptr);
  color_mode_->setCurrentIndex(static_cast<int>(p.color_mode));

  refresh_hz_->setValue(QSettings().value("capture/liveRefreshHz", 60.0).toDouble());
  updating_display_ = false;
}

void CaptureWindow::pushDisplayParams() {
  if (updating_display_ || !params_) return;
  auto p = params_->get();
  p.color_mode = static_cast<scanengine::ColorMode>(color_mode_->currentIndex());
  if (p.point_size.mode == scanengine::PointSizeMode::kFixedPixels) {
    p.point_size.fixed_px = float(point_size_->value());
  }
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
    s->colormap = static_cast<scanengine::Colormap>(colormap_->currentIndex());
  }
  params_->set(p);
  refreshDisplayControls();  // the controller clamps; re-read rather than assume
  Q_EMIT displayParamsChanged();
}

void CaptureWindow::applyLiveRefreshRate() {
  if (refresh_hz_) Q_EMIT liveRefreshHzChanged(refresh_hz_->value());
}

void CaptureWindow::setLiveRefreshCeiling(double hz) {
  if (!refresh_hz_ || hz < 5.0) return;
  const double ceiling = std::floor(hz + 0.5);
  updating_display_ = true;
  // 2 fps floor stays: a very slow refresh is a legitimate choice on a laptop
  // battery in the field, and it is the same floor the auto-downshift walks to.
  refresh_hz_->setRange(2.0, ceiling, 1.0);
  updating_display_ = false;
  refresh_hz_->setToolTip(refresh_hz_->toolTip() +
                          QString("\n\nThis machine's display: %1 Hz — that is the maximum.")
                              .arg(ceiling, 0, 'f', 0));
  // A persisted value above the new ceiling was clamped by setRange(); push the
  // clamped value out so the viewport and the panel agree.
  applyLiveRefreshRate();
}

void CaptureWindow::noteRefreshDownshift(double hz, const QString& why) {
  if (!refresh_hz_) return;
  updating_display_ = true;
  refresh_hz_->setValue(hz);  // does not emit — the viewport already applied it
  updating_display_ = false;
  QSettings().setValue("capture/liveRefreshHz", hz);
  if (refresh_note_) {
    refresh_note_->setText(QString("Live refresh eased to %1 fps — this machine could not "
                                   "sustain the previous rate (%2). The capture is "
                                   "unaffected: recording is never throttled.")
                               .arg(hz, 0, 'f', 0)
                               .arg(why));
    refresh_note_->setProperty("tone", "warn");
    repolish(refresh_note_);
    refresh_note_->setVisible(true);
  }
  log(QString("live refresh auto-downshift -> %1 fps (%2); recording untouched")
          .arg(hz, 0, 'f', 0)
          .arg(why));
}

// --- item 18: walkthrough-first ------------------------------------------
//
// The operator walks the space with the rig, so the two things they cannot see
// from behind the screen are "where have I been" and "am I going too fast".
//
// TWO ENGINE SEAMS ARE MISSING HERE, and both are worked around rather than
// papered over (NOTES.md §17):
//   1. LioPoseSource exposes latest() / size() / trajectory_length_m() but NO
//      way to READ the pose ring, so the trail cannot be reconstructed from the
//      engine — this polls the newest pose at 10 Hz (LIO's own rate) and
//      accumulates the path on the app side. A pass that misses a pose loses a
//      corner of the trail, never a point of the capture.
//   2. There is no motion-gate event on this path at all: EventType has no
//      "moving too fast" (event.h's list stops at kError), and the A8 pushbroom's
//      skipped-turning counters belong to the phone-only D6 flow. So the speed is
//      DERIVED here from consecutive pose positions, and the hint says what it
//      measured rather than claiming to be the engine's own gate.
void CaptureWindow::pollTrajectory() {
  const bool armed = phase_ == Phase::kArming || phase_ == Phase::kPreview ||
                     phase_ == Phase::kRecording || phase_ == Phase::kPaused;
  if (!armed || !host_ || !host_->ok()) return;
  auto* slam = host_->engine()->live_slam();
  if (!slam) {
    // Record-only session (live SLAM refused to start — see armPreview): there
    // is no trajectory to draw, and saying nothing is better than an empty trail.
    return;
  }
  scanengine::Pose latest{};
  if (!slam->poses().latest(&latest)) return;

  const std::array<float, 3> p{float(latest.position[0]), float(latest.position[1]),
                              float(latest.position[2])};
  const double now_s = walk_clock_.isValid() ? walk_clock_.elapsed() / 1000.0 : 0.0;
  if (!walk_clock_.isValid()) walk_clock_.start();

  bool appended = false;
  if (trail_.empty()) {
    trail_.push_back(p);
    appended = true;
  } else {
    const auto& last = trail_.back();
    const double dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
    const double step = std::sqrt(dx * dx + dy * dy + dz * dz);
    // 2 cm of movement before a new trail vertex: LIO publishes at 10 Hz whether
    // the rig moved or not, and a standing operator must not grow the buffer.
    if (step > 0.02) {
      const double dt = now_s - trail_last_t_s_;
      if (dt > 1e-3) {
        // Lightly smoothed so a single jumpy pose does not flash the hint.
        walk_speed_mps_ = 0.6 * walk_speed_mps_ + 0.4 * (step / dt);
      }
      trail_last_t_s_ = now_s;
      trail_.push_back(p);
      appended = true;
      // ~40 m of 2 cm steps before the oldest vertex is dropped; the trail is a
      // recent-history overlay, not the recorded trajectory (that is in the
      // .lscan).
      constexpr std::size_t kMaxTrailVertices = 2000;
      if (trail_.size() > kMaxTrailVertices) {
        trail_.erase(trail_.begin(), trail_.begin() + (trail_.size() - kMaxTrailVertices));
      }
    } else if (now_s - trail_last_t_s_ > 1.0) {
      walk_speed_mps_ *= 0.5;  // decay to zero while standing still
      trail_last_t_s_ = now_s;
    }
  }
  if (appended) Q_EMIT trajectoryTrailChanged(trail_);

  // 1.5 m/s is a brisk walk; above it a 10 Hz LIO scan-match starts to see
  // between-scan motion it must undistort rather than register, which is the
  // regime where a walkthrough smears. Gentle, inline, no modal, no sound.
  const double len_m = slam->poses().trajectory_length_m();
  const bool too_fast = walk_speed_mps_ > 1.5;
  walk_label_->setText(
      too_fast ? QString("Walking %1 m/s — ease off a little; %2 m of path so far.")
                     .arg(walk_speed_mps_, 0, 'f', 2)
                     .arg(len_m, 0, 'f', 1)
               : QString("Walking %1 m/s · %2 m of path · %3 poses")
                     .arg(walk_speed_mps_, 0, 'f', 2)
                     .arg(len_m, 0, 'f', 1)
                     .arg(slam->poses().size()));
  walk_label_->setProperty("tone", too_fast ? "warn" : "");
  repolish(walk_label_);
  walk_label_->setVisible(true);
}

double CaptureWindow::setLiveRefreshHzForCli(double hz) {
  if (!refresh_hz_) return 0.0;
  // Through the slider, not setValue(): this has to be the code path a drag
  // takes, signal and all.
  refresh_hz_->slider()->setValue(int(std::lround(hz - 2.0)));
  return refresh_hz_->value();
}

double CaptureWindow::setPointSizeForCli(double px) {
  if (!point_size_ || !params_) return 0.0;
  // Through the slider (0.1 px lo, 0.1 px step — item 2's range), so this is the
  // code path a drag takes, including the model's own clamping on the way back.
  point_size_->slider()->setValue(int(std::lround((px - 0.1) / 0.1)));
  return params_->get().point_size.fixed_px;
}

// ---------------------------------------------------------------------------
// Arming / live preview
// ---------------------------------------------------------------------------

bool CaptureWindow::startPreviewSession(QString* err) {
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  if (host_->sessionActive() && !host_->stopSession(err)) return false;
  // Empty lscan_dir + record=false: the live-preview pattern ReplayController
  // also uses. Points flow into the viewport; nothing hits disk.
  //
  // LIVE SLAM ON (round-5 item 18, walkthrough-first): a walked scan has to be
  // registered as it goes, and Engine::live_slam()->poses() is where the trail
  // comes from. If LIO refuses to start, the capture must NOT fail with it —
  // record-always outranks the overlay — so this falls back to Record-only and
  // says so once.
  if (host_->startSession(QString(), profile_->currentText(), false, err,
                          /*live_slam=*/true)) {
    live_slam_running_ = true;
    return true;
  }
  live_slam_running_ = false;
  log(QString("live SLAM would not start (%1) — continuing Record-only, so there is no "
              "trajectory trail this session")
          .arg(err ? *err : QString("unknown")));
  return host_->startSession(QString(), profile_->currentText(), false, err,
                             /*live_slam=*/false);
}

bool CaptureWindow::startRecordingSession(QString* err) {
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  if (host_->sessionActive() && !host_->stopSession(err)) return false;
  // Same live-SLAM decision as the preview session, and the same fallback: a
  // recording never fails because the odometry could not start.
  if (host_->startSession(last_project_dir_, profile_->currentText(), true, err,
                          /*live_slam=*/true)) {
    live_slam_running_ = true;
    return true;
  }
  live_slam_running_ = false;
  log(QString("live SLAM would not start for the recording (%1) — Record-only; every raw "
              "byte is still recorded")
          .arg(err ? *err : QString("unknown")));
  return host_->startSession(last_project_dir_, profile_->currentText(), true, err,
                             /*live_slam=*/false);
}

bool CaptureWindow::armPreview(QString* err) {
  if (phase_ != Phase::kIdle) return true;  // already armed/recording
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  if (lidar_ip_->text().trimmed().isEmpty()) {
    if (err) *err = "no lidar IP yet (macOS cannot discover by broadcast — S2 finding)";
    return false;
  }
  // Discovery and the Livox SDK both want UDP 56201; the device must own it
  // alone. This BLOCKS (bounded by one DiscoveryGate slice, ~1 s) until the
  // discovery worker's socket is really closed — NOTES.md §16.7.
  if (!stopDiscoveryForDeviceUse("live preview")) {
    if (err) *err = "auto-detect is still holding UDP 56201 — try again in a moment";
    return false;
  }

  if (!startPreviewSession(err)) return false;

  scanengine::Mid360Config cfg;
  cfg.udp.host_ip = host_ip_->text().trimmed().toStdString();
  cfg.udp.lidar_ip = lidar_ip_->text().trimmed().toStdString();
  cfg.udp.point_port = std::uint16_t(point_port_->value());
  cfg.udp.imu_port = std::uint16_t(imu_port_->value());
  cfg.udp.cmd_port = std::uint16_t(cmd_port_->value());
  device_ = host_->addMid360(cfg, err);
  if (device_ == scanengine::kInvalidDeviceId) {
    QString stop_err;
    (void)host_->stopSession(&stop_err);
    return false;
  }
  saveMid360Settings();
  log(QString("Mid-360 %1 -> host %2, device #%3 — live preview (not recording)")
          .arg(lidar_ip_->text(), host_ip_->text())
          .arg(device_));

  // Item 18: hold the display awake for as long as the device is armed — the
  // operator is walking, not typing. Honest about platforms that cannot.
  if (!awake_.held()) {
    if (awake_.acquire("LidarScan capture in progress")) {
      log("display sleep inhibited — " + awake_.reason());
    } else {
      log("display sleep NOT inhibited — " + awake_.reason());
    }
  }
  trail_.clear();
  walk_speed_mps_ = 0.0;
  trail_last_t_s_ = 0.0;
  walk_clock_.restart();
  Q_EMIT trajectoryTrailChanged(trail_);

  arm_clock_.start();
  auto h = host_->engine()->device_health(device_);
  arm_baseline_points_ = h.ok() ? h.value().points_out : 0;
  last_arm_failed_ = false;
  setPhase(Phase::kArming);
  arm_label_->setStyleSheet(QString());
  arm_label_->setText("Arming — waiting for the first Mid-360 packet…");
  Q_EMIT previewStarted();
  return true;
}

bool CaptureWindow::disarmPreview(const QString& why) {
  if (phase_ == Phase::kRecording || phase_ == Phase::kPaused) {
    log("refusing to disarm: a recording is open — Stop it first");
    return false;
  }
  if (phase_ == Phase::kIdle) return true;
  QString err;
  if (device_ != scanengine::kInvalidDeviceId && host_) {
    (void)host_->removeDevice(device_, &err);
    device_ = scanengine::kInvalidDeviceId;
  }
  if (host_ && host_->sessionActive()) (void)host_->stopSession(&err);
  live_slam_running_ = false;
  setPhase(Phase::kIdle);
  awake_.release();
  trail_.clear();
  Q_EMIT trajectoryTrailChanged(trail_);
  if (walk_label_) walk_label_->setVisible(false);
  log("live preview stopped — " + why);
  return true;
}

void CaptureWindow::evaluateArming() {
  if (!host_ || !host_->ok() || device_ == scanengine::kInvalidDeviceId) return;
  auto h = host_->engine()->device_health(device_);
  const double elapsed = arm_clock_.elapsed() / 1000.0;
  const std::uint64_t pts = h.ok() ? h.value().points_out : 0;
  const std::uint64_t gained = pts > arm_baseline_points_ ? pts - arm_baseline_points_ : 0;

  if (gained > 0) {
    const QString detail = QString("first packet after %1 s").arg(elapsed, 0, 'f', 2);
    last_arm_failed_ = false;
    arm_label_->setStyleSheet(QString("color:%1;font-weight:600;").arg(theme::css(theme::good())));
    arm_label_->setText("Live — " + detail + ". Start records into a new project.");
    setPhase(Phase::kPreview);
    log("live preview up: " + detail);
    // The same signal the self-test gate used to emit, with the same meaning for
    // main.cpp's --mid360-selftest: PASS = the device produced data.
    Q_EMIT selfTestFinished(true, detail);
    return;
  }
  if (elapsed >= arm_window_s_) {
    const QString state = h.ok() ? scanengine::to_string(h.value().state) : "unknown";
    const QString detail = QString("no packet within %1 s (device state: %2)")
                               .arg(arm_window_s_, 0, 'f', 0)
                               .arg(state);
    last_arm_failed_ = true;
    arm_label_->setStyleSheet(QString("color:%1;font-weight:600;").arg(theme::css(theme::bad())));
    arm_label_->setText("No data — " + detail);
    // Give the port back: a faulted device holding 56201 blocks the auto-detect
    // pass the operator is about to need.
    QString err;
    if (device_ != scanengine::kInvalidDeviceId) {
      (void)host_->removeDevice(device_, &err);
      device_ = scanengine::kInvalidDeviceId;
    }
    (void)host_->stopSession(&err);
    live_slam_running_ = false;
    setPhase(Phase::kIdle);
    awake_.release();
    if (walk_label_) walk_label_->setVisible(false);
    log("arm failed: " + detail);
    Q_EMIT selfTestFinished(false, detail);
    return;
  }
  arm_label_->setText(QString("Arming — waiting for the first packet (%1 / %2 s)…")
                          .arg(elapsed, 0, 'f', 1)
                          .arg(arm_window_s_, 0, 'f', 0));
}

void CaptureWindow::setManualSetupOpen(bool open, bool focus) {
  if (!manual_box_) return;
  manual_box_->setVisible(open);
  if (manual_toggle_ && manual_toggle_->isChecked() != open) {
    // Programmatic opens (the "nothing found" fallback) must leave the toggle
    // telling the truth. setChecked() re-enters this slot; the guard above ends
    // the recursion after one hop.
    manual_toggle_->setChecked(open);
  }
  // Focus only when the OPERATOR asked for the row. Focusing on the automatic
  // open scrolls the panel to the field — and, worse, hands focus onward to the
  // next widget when arming later disables it, scrolling the panel again for no
  // reason (seen in the first evidence run, which photographed a panel scrolled
  // down to the RTK combo).
  if (open && focus && lidar_ip_ && lidar_ip_->isEnabled()) lidar_ip_->setFocus();
}

void CaptureWindow::onConnect() {
  if (phase_ == Phase::kRecording || phase_ == Phase::kPaused) return;
  if (phase_ != Phase::kIdle && !disarmPreview("reconnecting with the addresses shown")) return;
  QString err;
  if (!armPreview(&err)) log("connect: " + err);
}

// ---------------------------------------------------------------------------
// Start / pause / stop
// ---------------------------------------------------------------------------

QString CaptureWindow::resolveNewProjectDir(const QString& typedName, bool* auto_named) {
  QSettings s;
  const QString root = captureRoot();
  QDir().mkpath(root);

  QString base = typedName.trimmed();
  if (auto_named) *auto_named = base.isEmpty();
  if (base.isEmpty()) {
    // Round 5 item 9: series number + date + time. The counter is bumped HERE
    // (i.e. once per created project) and persisted immediately, so a crash
    // cannot hand the same number out twice.
    const int series = s.value("capture/seriesNumber", 0).toInt() + 1;
    s.setValue("capture/seriesNumber", series);
    base = QString("Scan-%1 %2")
               .arg(series, 3, 10, QChar('0'))
               .arg(QDateTime::currentDateTime().toString(kAutoNameTimeFormat));
  } else {
    // A typed name is still a path component: strip the separators rather than
    // silently creating a nested directory the operator did not ask for.
    base.replace('/', '-').replace('\\', '-').replace(':', '-');
  }
  if (base.endsWith(".lscan", Qt::CaseInsensitive)) base.chop(6);

  QString dir = QDir(root).filePath(base + ".lscan");
  int suffix = 2;
  while (QFileInfo::exists(dir)) {
    dir = QDir(root).filePath(QString("%1-%2.lscan").arg(base).arg(suffix++));
  }
  return dir;
}

void CaptureWindow::updateNameHint() {
  if (!name_hint_) return;
  const QString typed = name_edit_->text().trimmed();
  const QString root = captureRoot();
  if (typed.isEmpty()) {
    const int next = QSettings().value("capture/seriesNumber", 0).toInt() + 1;
    name_hint_->setText(
        QString("→ %1/Scan-%2 %3.lscan  (auto-named)")
            .arg(root)
            .arg(next, 3, 10, QChar('0'))
            .arg(QDateTime::currentDateTime().toString(kAutoNameTimeFormat)));
  } else {
    name_hint_->setText(QString("→ %1/%2.lscan").arg(root, typed));
  }
}

void CaptureWindow::onStart() {
  if (phase_ == Phase::kRecording || phase_ == Phase::kPaused) return;

  // One click, even from cold: if nothing is armed yet (auto-detect found the
  // link but the arm failed, or the operator typed the addresses by hand), Start
  // arms first rather than telling them to press something else.
  if (phase_ == Phase::kIdle) {
    QString arm_err;
    if (!armPreview(&arm_err)) {
      log("start: cannot arm the device — " + arm_err);
      return;
    }
  }

  bool auto_named = false;
  const QString dir = last_cli_project_dir_.isEmpty()
                          ? resolveNewProjectDir(name_edit_->text(), &auto_named)
                          : last_cli_project_dir_;
  last_cli_project_dir_.clear();
  last_project_dir_ = dir;

  // Record restarts the session, which restarts every registered device — i.e.
  // it re-binds the SDK's push port. Same exclusion as arming.
  if (!stopDiscoveryForDeviceUse("Start")) {
    log("start: refusing — the auto-detect worker still holds UDP 56201");
    return;
  }
  QString err;
  if (!startRecordingSession(&err)) {
    log("start: " + err);
    return;
  }
  auto h = host_->engine()->device_health(device_);
  record_baseline_points_ = h.ok() ? h.value().points_out : 0;
  cum_bytes_written_ = 0;
  cum_chunks_written_ = 0;
  recorded_seconds_accum_ = 0.0;
  record_segment_clock_.start();
  summary_->clear();

  setPhase(Phase::kRecording);
  log(QString("recording started -> %1%2").arg(dir, auto_named ? "  (auto-named)" : ""));
  updateNameHint();
  Q_EMIT captureStarted(dir);
}

void CaptureWindow::onPauseResume() {
  if (phase_ != Phase::kRecording && phase_ != Phase::kPaused) return;
  // Both directions stop the current session and start another one, and
  // Engine::start_session() restarts every still-registered device — so both
  // re-bind the SDK's push port and both need the port to themselves. In
  // practice discovery can never be in flight here; this is the invariant
  // stated in code.
  if (!stopDiscoveryForDeviceUse(phase_ == Phase::kRecording ? "Pause" : "Resume")) {
    log("pause/resume: refusing — the auto-detect worker still holds UDP 56201");
    return;
  }
  QString err;
  if (phase_ == Phase::kRecording) {
    accumulateRecorderStats();
    recorded_seconds_accum_ += record_segment_clock_.elapsed() / 1000.0;
    if (!startPreviewSession(&err)) {
      log("pause: " + err);
      return;
    }
    setPhase(Phase::kPaused);
    log("capture paused — still streaming to the viewport, not recording");
  } else {
    if (!startRecordingSession(&err)) {
      log("resume: " + err);
      return;
    }
    record_segment_clock_.start();
    setPhase(Phase::kRecording);
    log("capture resumed -> " + last_project_dir_);
  }
}

void CaptureWindow::onStop() {
  if (phase_ != Phase::kRecording && phase_ != Phase::kPaused) return;

  if (phase_ == Phase::kRecording) {
    accumulateRecorderStats();
    recorded_seconds_accum_ += record_segment_clock_.elapsed() / 1000.0;
  }

  std::uint64_t points_now = record_baseline_points_;
  std::uint64_t drops_now = 0;
  if (host_ && host_->ok() && device_ != scanengine::kInvalidDeviceId) {
    auto h = host_->engine()->device_health(device_);
    if (h.ok()) {
      points_now = h.value().points_out;
      drops_now = h.value().drops;
    }
  }
  const auto* store = host_ ? host_->points() : nullptr;
  const quint64 store_dropped = store ? store->dropped_points() : 0;
  const QString sealed_dir = last_project_dir_;

  // Round 5: Stop SEALS the project and drops straight back to live preview —
  // it does not tear the device down, because the next scan is one click away
  // and re-arming would cost another SDK handshake. startPreviewSession()
  // stops the recording session first, which is what seals the .lscan.
  QString err;
  const bool back_to_preview = startPreviewSession(&err);
  if (!back_to_preview) {
    log("stop: could not return to live preview (" + err + ") — device released");
    if (device_ != scanengine::kInvalidDeviceId && host_) {
      (void)host_->removeDevice(device_, &err);
      device_ = scanengine::kInvalidDeviceId;
    }
    if (host_ && host_->sessionActive()) (void)host_->stopSession(&err);
    setPhase(Phase::kIdle);
  } else {
    setPhase(Phase::kPreview);
  }

  const QString sum =
      QString("Sealed %1 — %2 s recording · %3 chunks / %4 written · %5 points decoded "
              "since Start (device counters also include any paused time) · %6 drops "
              "(device) / %7 dropped (store)")
          .arg(QFileInfo(sealed_dir).fileName())
          .arg(recorded_seconds_accum_, 0, 'f', 1)
          .arg(cum_chunks_written_)
          .arg(humanBytesLocal(cum_bytes_written_))
          .arg(points_now - record_baseline_points_)
          .arg(drops_now)
          .arg(store_dropped);
  summary_->setText(sum);
  log(sum);

  recorded_seconds_accum_ = 0.0;
  if (record_cluster_) record_cluster_->setElapsedSeconds(0.0);
  name_edit_->clear();
  updateNameHint();
  Q_EMIT captureStopped(sealed_dir);
}

void CaptureWindow::accumulateRecorderStats() {
  if (!host_ || !host_->ok()) return;
  const auto stats = host_->engine()->recorder().stats();
  cum_bytes_written_ += stats.bytes_written;
  cum_chunks_written_ += stats.chunks_written;
}

double CaptureWindow::recordedSecondsNow() const {
  double s = recorded_seconds_accum_;
  if (phase_ == Phase::kRecording && record_segment_clock_.isValid()) {
    s += record_segment_clock_.elapsed() / 1000.0;
  }
  return s;
}

void CaptureWindow::updateHealth() {
  if (!host_) return;
  if (phase_ == Phase::kArming) evaluateArming();

  if (device_ != scanengine::kInvalidDeviceId && host_->ok()) {
    auto h = host_->engine()->device_health(device_);
    if (h.ok()) {
      const auto& d = h.value();
      QString flag;
      if (d.state == scanengine::DeviceState::kDegraded) {
        flag = QString(" · DEGRADED (%1)").arg(scanengine::error_str(d.last_error));
      } else if (d.state == scanengine::DeviceState::kFault) {
        flag = QString(" · FAULT (%1)").arg(scanengine::error_str(d.last_error));
      }
      health_->setText(QString("%1 · %2 pts/s · %3 Hz IMU · %4% ok · %5 pts / %6 in · "
                               "%7 drops%8")
                           .arg(scanengine::to_string(d.state))
                           .arg(d.points_per_sec, 0, 'f', 0)
                           .arg(d.rotation_hz, 0, 'f', 2)
                           .arg(d.checksum_pass_rate * 100.0, 0, 'f', 1)
                           .arg(d.points_out)
                           .arg(humanBytesLocal(d.bytes_in))
                           .arg(d.drops)
                           .arg(flag));
      return;
    }
  }
  health_->setText(host_->healthLine());
}

void CaptureWindow::updateRecordCluster() {
  if (!record_cluster_) return;
  RecordCluster::State s = RecordCluster::State::kNoDevice;
  switch (phase_) {
    case Phase::kIdle:
      s = last_arm_failed_ ? RecordCluster::State::kNoData : RecordCluster::State::kNoDevice;
      break;
    case Phase::kArming: s = RecordCluster::State::kArming; break;
    case Phase::kPreview: s = RecordCluster::State::kLive; break;
    case Phase::kRecording: s = RecordCluster::State::kRecording; break;
    case Phase::kPaused: s = RecordCluster::State::kPaused; break;
  }
  record_cluster_->setState(s);
  record_cluster_->setElapsedSeconds(recordedSecondsNow());
}

void CaptureWindow::setPhase(Phase p) {
  const bool was_live = phase_ == Phase::kRecording || phase_ == Phase::kPaused;
  phase_ = p;
  const bool idle = p == Phase::kIdle;
  const bool recording = p == Phase::kRecording;
  const bool paused = p == Phase::kPaused;

  // The link fields configure the device that is about to be opened; while one
  // is open they describe it, so they are read-only rather than misleading.
  for (QWidget* w : {static_cast<QWidget*>(host_ip_), static_cast<QWidget*>(lidar_ip_),
                     static_cast<QWidget*>(point_port_), static_cast<QWidget*>(imu_port_),
                     static_cast<QWidget*>(cmd_port_)}) {
    if (w) w->setEnabled(idle);
  }
  if (profile_) profile_->setEnabled(!recording && !paused);
  // Naming a project the moment before you record it is normal; renaming one
  // mid-recording is not (the directory already exists on disk).
  if (name_edit_) name_edit_->setEnabled(!recording && !paused);
  if (connect_btn_) {
    connect_btn_->setEnabled(!recording && !paused);
    connect_btn_->setText(idle ? "Connect" : "Reconnect");
  }
  if (manual_toggle_) manual_toggle_->setEnabled(!recording && !paused);
  if (auto_detect_btn_) {
    auto_detect_btn_->setEnabled(!discovery_in_flight_ && !recording && !paused);
  }

  updateRecordCluster();

  const bool live = recording || paused;
  if (live || was_live) Q_EMIT recordingStateChanged(recording, paused);
}

// ---------------------------------------------------------------------------
// Mid-360 link persistence
// ---------------------------------------------------------------------------

void CaptureWindow::loadMid360Settings() {
  if (!host_ip_) return;
  QSettings s;
  s.beginGroup("mid360/last");
  had_saved_mid360_settings_ = s.contains("hostIp");
  host_ip_->setText(s.value("hostIp", host_ip_->text()).toString());
  lidar_ip_->setText(s.value("lidarIp", lidar_ip_->text()).toString());
  point_port_->setValue(s.value("pointPort", point_port_->value()).toInt());
  imu_port_->setValue(s.value("imuPort", imu_port_->value()).toInt());
  cmd_port_->setValue(s.value("cmdPort", cmd_port_->value()).toInt());
  s.endGroup();
}

void CaptureWindow::saveMid360Settings() {
  if (!host_ip_) return;
  QSettings s;
  s.beginGroup("mid360/last");
  s.setValue("hostIp", host_ip_->text());
  s.setValue("lidarIp", lidar_ip_->text());
  s.setValue("pointPort", point_port_->value());
  s.setValue("imuPort", imu_port_->value());
  s.setValue("cmdPort", cmd_port_->value());
  s.endGroup();
}

// ---------------------------------------------------------------------------
// CLI hooks
// ---------------------------------------------------------------------------

void CaptureWindow::runMid360SelfTestForCli(const QString& hostIp, const QString& lidarIp) {
  host_ip_->setText(hostIp);
  lidar_ip_->setText(lidarIp);
  QString err;
  if (!armPreview(&err)) {
    log("mid360 arm (CLI): " + err);
    // A caller waiting on selfTestFinished() must hear something either way, or
    // a headless run hangs until --quit-after.
    Q_EMIT selfTestFinished(false, err);
  }
}

void CaptureWindow::triggerRecordForCli(const QString& projectDir) {
  last_cli_project_dir_ = projectDir;
  onStart();
}

QString CaptureWindow::triggerStartWithAutoNameForCli() {
  name_edit_->clear();
  onStart();
  return last_project_dir_;
}

void CaptureWindow::triggerPauseResumeForCli() { onPauseResume(); }

void CaptureWindow::triggerStopForCli() { onStop(); }

void CaptureWindow::triggerAutoDetectForCli() { onAutoDetectClicked(); }

void CaptureWindow::suppressSilentAutoDetectForCli() {
  suppress_silent_auto_detect_ = true;
}

void CaptureWindow::suppressAutoArmForCli() { suppress_auto_arm_ = true; }

void CaptureWindow::injectTrailForCli(const std::vector<std::array<float, 3>>& path) {
  trail_ = path;
  Q_EMIT trajectoryTrailChanged(trail_);
}

// ---------------------------------------------------------------------------
// Auto-detect — inline, no dialog (round 5 item 7)
// ---------------------------------------------------------------------------
//
// docs/design/REVIEW_FEEDBACK.md round 4 item 5 is why this exists at all ("the
// apps must auto-detect device settings … manual IP entry defeated the GUI on
// first contact"); round 5 item 7 is why it reports INLINE ("no popup windows").
// captures/FIELD_SESSION_2026-08-17.md is the field session whose numbers (SN
// MCP7K0034759, fw 35010108, lidar 192.168.1.159, persisted host 192.168.1.5,
// UM982 on /dev/cu.usbserial-21140 @ 230400 with GPTHS present) are exactly the
// shape of the beacon/probe data this section renders.

void CaptureWindow::buildAutoDetectSection(QVBoxLayout* v) {
  auto* row = new QWidget();
  auto* rl = new QHBoxLayout(row);
  rl->setContentsMargins(0, 0, 0, 0);
  rl->setSpacing(6);
  auto_detect_btn_ = new QPushButton("Auto-detect devices");
  auto_detect_btn_->setProperty("accent", "ember");
  auto_detect_btn_->setCursor(Qt::PointingHandCursor);
  auto_detect_btn_->setMinimumHeight(34);
  auto_detect_btn_->setToolTip(
      "Runs by itself when this panel opens. Listens for a Mid-360 heartbeat and "
      "sweeps serial ports for a UM982 (and a COIN-D6, which desktop capture does not "
      "use — see the D6 line below). A Mid-360 hit arms the live preview "
      "automatically.");
  connect(auto_detect_btn_, &QPushButton::clicked, this, &CaptureWindow::onAutoDetectClicked);
  rl->addWidget(auto_detect_btn_, 1);

  // Round-5 follow-up item 1: reachable AT ANY TIME, including when detection
  // succeeded — a checkable toggle, not a dialog, and not something that only
  // appears on failure.
  manual_toggle_ = new QPushButton("Manual setup");
  manual_toggle_->setCheckable(true);
  manual_toggle_->setCursor(Qt::PointingHandCursor);
  manual_toggle_->setMinimumHeight(34);
  manual_toggle_->setToolTip(
      "Type the lidar and host IP by hand and Connect. Opens by itself when "
      "auto-detect finds nothing.");
  connect(manual_toggle_, &QPushButton::toggled, this,
          [this](bool on) { setManualSetupOpen(on, /*focus=*/true); });
  rl->addWidget(manual_toggle_);
  v->addWidget(row);

  // The inline replacement for the progress DIALOG this pass deleted: one phase
  // label ("Listening for Mid-360 heartbeat…" / "Probing serial ports…", pushed
  // by the worker before each stage) plus an indeterminate bar, both living in
  // the panel and both hidden while idle.
  discovery_phase_label_ = new QLabel();
  discovery_phase_label_->setWordWrap(true);
  discovery_phase_label_->setVisible(false);
  v->addWidget(discovery_phase_label_);
  discovery_bar_ = new QProgressBar();
  discovery_bar_->setRange(0, 0);  // indeterminate: the two phases take different real time
  discovery_bar_->setTextVisible(false);
  discovery_bar_->setFixedHeight(6);
  discovery_bar_->setVisible(false);
  v->addWidget(discovery_bar_);

  auto_detect_panel_ = new QWidget();
  auto* pv = new QVBoxLayout(auto_detect_panel_);
  pv->setContentsMargins(0, 2, 0, 0);
  pv->setSpacing(3);

  // Why the pass did not run / did not finish. Above the per-sensor lines
  // because it overrides them: when this is visible, whatever those lines say
  // is from an earlier pass.
  auto_detect_status_line_ = new QLabel();
  auto_detect_status_line_->setWordWrap(true);
  auto_detect_status_line_->setVisible(false);
  pv->addWidget(auto_detect_status_line_);

  auto_detect_mid360_line_ = new QLabel();
  auto_detect_mid360_line_->setWordWrap(true);
  pv->addWidget(auto_detect_mid360_line_);

  auto_detect_fix_line_ = new QLabel();
  auto_detect_fix_line_->setWordWrap(true);
  auto_detect_fix_line_->setTextFormat(Qt::PlainText);
  auto_detect_fix_line_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto_detect_fix_line_->setVisible(false);
  pv->addWidget(auto_detect_fix_line_);

  auto_detect_copy_btn_ = new QPushButton("Copy fix command");
  auto_detect_copy_btn_->setVisible(false);
  auto_detect_copy_btn_->setCursor(Qt::PointingHandCursor);
  connect(auto_detect_copy_btn_, &QPushButton::clicked, this, [this] {
    QGuiApplication::clipboard()->setText(auto_detect_copy_payload_);
    log("copied to clipboard: " + auto_detect_copy_payload_);
  });
  pv->addWidget(auto_detect_copy_btn_, 0, Qt::AlignLeft);

  auto_detect_d6_line_ = new QLabel();
  auto_detect_d6_line_->setWordWrap(true);
  pv->addWidget(auto_detect_d6_line_);

  auto_detect_um982_line_ = new QLabel();
  auto_detect_um982_line_->setWordWrap(true);
  pv->addWidget(auto_detect_um982_line_);

  auto_detect_panel_->setVisible(false);  // nothing to show before the first pass
  v->addWidget(auto_detect_panel_);
}

void CaptureWindow::setAutoDetectStatus(const QString& text, const char* tone) {
  if (!auto_detect_status_line_) return;
  auto_detect_status_line_->setText(text);
  auto_detect_status_line_->setProperty("tone", tone);
  repolish(auto_detect_status_line_);
  auto_detect_status_line_->setVisible(!text.isEmpty());
  if (auto_detect_panel_ && !text.isEmpty()) auto_detect_panel_->setVisible(true);
}

void CaptureWindow::setDiscoveryRunning(bool running, const QString& phase_label) {
  if (!discovery_bar_) return;
  discovery_bar_->setVisible(running);
  discovery_phase_label_->setVisible(running);
  if (running) discovery_phase_label_->setText(phase_label);
}

bool CaptureWindow::stopDiscoveryForDeviceUse(const QString& what) {
  if (!discovery_in_flight_) return true;

  // Several call sites deliberately overlap (onStart -> armPreview, say), and
  // discovery_in_flight_ stays true until the worker's queued finished() lands —
  // so the second call still has work to do (re-confirm the port is free) but
  // nothing new to SAY. Only the first one narrates.
  const bool already = discovery_canceled_;
  discovery_canceled_ = true;
  // 3 s is generous against a bound of one DiscoveryGate::kChunkMs slice; the
  // margin is for a machine under load, not for a second listen window.
  const bool released = discovery_gate_ ? discovery_gate_->cancelAndWaitForSockets(3000) : true;
  setDiscoveryRunning(false, QString());
  if (already) return released;

  const QString msg =
      released
          ? QString("auto-detect canceled so %1 can have UDP 56201 — port released").arg(what)
          : QString("auto-detect canceled for %1 but its UDP socket did not come free in "
                    "time — not starting the device")
                .arg(what);
  setAutoDetectStatus(msg, released ? "warn" : "bad");
  log(msg);
  return released;
}

void CaptureWindow::onAutoDetectClicked() {
  if (discovery_in_flight_) return;
  // Round 5's new case. After auto-arm the panel is normally in kPreview, and
  // refusing here would mean the operator could never re-run auto-detect at all
  // (e.g. after plugging the Ethernet cable in properly). So a merely-PREVIEWING
  // device steps aside — the preview is stopped, the port released, discovery
  // runs, and the completion handler re-arms. A RECORDING is never interrupted:
  // startDiscovery() below still refuses in that case.
  if (phase_ == Phase::kArming || phase_ == Phase::kPreview) {
    if (!disarmPreview("auto-detect needs UDP 56201")) return;
    rearm_after_discovery_ = true;
  }
  startDiscovery(/*silent=*/false);
}

void CaptureWindow::startDiscovery(bool silent) {
  if (discovery_in_flight_) return;
  // BOTH ways (NOTES.md §16.7). A recording session owns UDP 56201 for as long
  // as it lives, so no discovery pass — automatic or clicked — may start while
  // one is open.
  if (phase_ != Phase::kIdle) {
    const QString msg =
        "Auto-detect is unavailable while a recording is open — it listens on UDP "
        "56201, the same port the Mid-360 driver binds. Stop the recording first.";
    if (silent) {
      log("auto-detect (on open) skipped — a capture session holds UDP 56201");
    } else {
      setAutoDetectStatus(msg, "warn");
      log("auto-detect skipped — a capture session holds UDP 56201");
    }
    rearm_after_discovery_ = false;
    return;
  }

  discovery_in_flight_ = true;
  discovery_canceled_ = false;
  setAutoDetectStatus(QString(), "warn");
  auto_detect_btn_->setEnabled(false);
  setDiscoveryRunning(true, "Listening for Mid-360 heartbeat…");

  // Mid-360 heartbeat is ~1 Hz (spikes/s2-mid360-sim REPORT.md); 3 s gives it
  // several windows. discovery.h's ProbeSerialD6/Um982 spend `per_port_ms` PER
  // ENUMERATED PORT, so the serial phase's total time scales with how many
  // serial devices this machine has — 700 ms keeps a typical 2-4-port machine
  // near ~6 s total.
  //
  // NO parent on the QThread — deliberately (NOTES.md §16.3 bug 2): a parented
  // child is synchronously deleted when this widget is destroyed, and deleting a
  // QThread whose run() is still inside a scanengine::discovery call is
  // "Destroyed while thread is still running" -> abort(). Unparented, the
  // worker/thread pair has a self-contained cleanup chain, and the one thing
  // that touches this widget (the finished lambda) is connected with `this` as
  // context, which Qt auto-disconnects on destruction.
  auto* thread = new QThread();
  discovery_thread_ = thread;
  auto* worker = new DiscoveryWorker(3000, 700);
  // Grabbed BEFORE the thread starts: this is what a device start cancels
  // against, and it must exist from the instant discovery_in_flight_ is true.
  discovery_gate_ = worker->gate();
  worker->moveToThread(thread);
  connect(thread, &QThread::started, worker, &DiscoveryWorker::run);
  connect(worker, &DiscoveryWorker::phase, this, [this](const QString& label) {
    if (discovery_phase_label_ && discovery_phase_label_->isVisible()) {
      discovery_phase_label_->setText(label);
    }
  });
  connect(worker, &DiscoveryWorker::finished, this,
          [this, silent](DiscoveryResult r) { handleDiscoveryFinished(r, silent); });
  connect(worker, &DiscoveryWorker::finished, thread, &QThread::quit);
  connect(worker, &DiscoveryWorker::finished, worker, &QObject::deleteLater);
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();
}

void CaptureWindow::handleDiscoveryFinished(const DiscoveryResult& r, bool silent) {
  discovery_in_flight_ = false;
  if (discovery_thread_) discovery_thread_ = nullptr;  // it is finishing itself off
  discovery_gate_.reset();
  if (auto_detect_btn_) auto_detect_btn_->setEnabled(true);
  setDiscoveryRunning(false, QString());

  // A canceled pass has NO verdict. Its "not seen" fields mean "not looked
  // for", so nothing is applied and autoDetectFinished() is not emitted —
  // main.cpp's --auto-detect-selftest chain keys off that signal and must not
  // treat an aborted listen as a result.
  if (r.canceled || discovery_canceled_) {
    discovery_canceled_ = false;
    rearm_after_discovery_ = false;
    log(QString("auto-detect%1: canceled before completion — nothing applied")
            .arg(silent ? " (on open)" : ""));
    return;
  }

  applyMid360Result(r, silent);
  applyD6Result(r);
  applyUm982Result(r, silent);
  if (auto_detect_panel_) auto_detect_panel_->setVisible(true);

  log(QString("auto-detect%1: Mid-360 %2, D6 %3 (phone-only), UM982 %4")
          .arg(silent ? " (on open)" : "")
          .arg(r.mid360.found ? "found" : "not seen")
          .arg(r.d6.found ? "detected" : "not seen")
          .arg(r.um982.found ? "found" : "not seen"));
  Q_EMIT autoDetectFinished(r.mid360.found, r.d6.found, r.um982.found);

  // Round-5 follow-up item 1: nothing found -> the inline manual row opens by
  // itself, with a sentence saying why, so the operator has somewhere to type
  // instead of a dead end. (It stays reachable from "Manual setup" when
  // detection DID succeed — that toggle is never hidden.)
  if (!r.mid360.found) {
    setManualSetupOpen(true);
    setAutoDetectStatus(
        "No Mid-360 answered. Type the lidar IP (and the host IP this Mac holds) in "
        "Manual setup, then Connect — or fix the link and run Auto-detect again.",
        "warn");
  }

  // Round 5 item 10: a device that answered goes straight to live preview. No
  // button, no gate — the points on screen are the proof it works. A CLI hook
  // that arms the device itself opts out (suppressAutoArmForCli).
  const bool want_arm = rearm_after_discovery_ || r.mid360.found;
  rearm_after_discovery_ = false;
  if (!want_arm || suppress_auto_arm_ || phase_ != Phase::kIdle) return;
  QString err;
  if (!armPreview(&err)) {
    log("auto-arm after auto-detect: " + err);
  }
}

void CaptureWindow::applyMid360Result(const DiscoveryResult& r, bool silent) {
  const auto& m = r.mid360;
  auto_detect_fix_line_->setVisible(false);
  auto_detect_copy_btn_->setVisible(false);
  auto_detect_copy_payload_.clear();

  if (!m.found) {
    const QString why = r.mid360_error.isEmpty() ? QStringLiteral("no heartbeat heard")
                                                  : r.mid360_error;
    auto_detect_mid360_line_->setText(
        QString("Mid-360: not seen (%1) — check power, the Ethernet cable, and that this "
                "Mac has an address on the lidar's network.")
            .arg(why));
    auto_detect_mid360_line_->setProperty("tone", "warn");
    repolish(auto_detect_mid360_line_);
    return;
  }

  auto_detect_mid360_line_->setText(
      QString("Found Mid-360 SN %1, fw %2, at %3.").arg(m.sn, m.fw_version, m.lidar_ip));
  auto_detect_mid360_line_->setProperty("tone", "good");
  repolish(auto_detect_mid360_line_);

  // Prefill guard: on the automatic (on-open) pass, a field already holding
  // something other than the hard-coded placeholder is left alone — "never
  // overwrite user-entered values". A clicked pass always fills in what it
  // found; that is the point of clicking it.
  const bool lidar_ip_is_default = lidar_ip_->text().trimmed() == "192.168.1.100";
  const bool host_ip_is_default = host_ip_->text().trimmed() == "192.168.1.5";
  if (!silent || lidar_ip_is_default) lidar_ip_->setText(m.lidar_ip);

  // discovery.h's CheckHostReachability() always sets suggested_host_ip to the
  // beacon's OWN persisted_host_ip when the beacon carried one; a locally-held
  // address is only ever suggested for a beacon with NO persisted host. So the
  // meaningful branch is "does this Mac already hold it" — see NOTES.md §16.4.
  if (m.host_ip_is_local) {
    if (!silent || host_ip_is_default) {
      host_ip_->setText(m.persisted_host_ip.isEmpty() ? m.suggested_host_ip : m.persisted_host_ip);
    }
  } else if (m.persisted_host_ip.isEmpty() && !m.suggested_host_ip.isEmpty()) {
    if (!silent || host_ip_is_default) host_ip_->setText(m.suggested_host_ip);
    auto_detect_fix_line_->setText(
        QString("lidar has no host address configured yet; using %1 — the first connect "
                "will configure it")
            .arg(m.suggested_host_ip));
    auto_detect_fix_line_->setToolTip(m.host_check_note);
    auto_detect_fix_line_->setProperty("tone", "warn");
    repolish(auto_detect_fix_line_);
    auto_detect_fix_line_->setVisible(true);
  } else if (!m.persisted_host_ip.isEmpty()) {
    // A persisted host exists and this Mac does not hold it — the field-session
    // case. host_ip_ still gets the persisted value (that IS what the driver
    // needs to declare); the alias below is what makes it real on this machine.
    if (!silent || host_ip_is_default) host_ip_->setText(m.persisted_host_ip);
    const QString iface = m.suggested_interface.isEmpty() ? QStringLiteral("<if>")
                                                            : m.suggested_interface;
    auto_detect_copy_payload_ =
        QString("sudo ifconfig %1 alias %2 255.255.255.255").arg(iface, m.persisted_host_ip);
    auto_detect_fix_line_->setText(
        QString("this Mac needs an address on the lidar's network — e.g. `%1`")
            .arg(auto_detect_copy_payload_));
    auto_detect_fix_line_->setToolTip(m.host_check_note);
    auto_detect_fix_line_->setProperty("tone", "bad");
    repolish(auto_detect_fix_line_);
    auto_detect_fix_line_->setVisible(true);
    auto_detect_copy_btn_->setVisible(true);
  } else {
    auto_detect_fix_line_->setText(
        m.host_check_note.isEmpty()
            ? QStringLiteral("this Mac has no address on the lidar's network and the lidar "
                             "has no host configured either — connect this Mac to the "
                             "lidar's network and run Auto-detect again")
            : m.host_check_note);
    auto_detect_fix_line_->setProperty("tone", "bad");
    repolish(auto_detect_fix_line_);
    auto_detect_fix_line_->setVisible(true);
  }
}

// Round 5 item 11: the D6 is PHONE-ONLY. The serial probe still runs (it is the
// same sweep that finds the UM982, and knowing the sensor is plugged in here is
// still useful information), but there is nothing to configure and nothing to
// start on the desktop, so this is a passive line — no port picker, no badge on
// a device tab, no capture affordance at all.
void CaptureWindow::applyD6Result(const DiscoveryResult& r) {
  const auto& d = r.d6;
  if (!d.found) {
    auto_detect_d6_line_->setText(
        "COIN-D6: not seen on any serial port — desktop capture does not use it either "
        "way (it is phone-only).");
    auto_detect_d6_line_->setProperty("tone", "");
    repolish(auto_detect_d6_line_);
    return;
  }
  auto_detect_d6_line_->setText(
      QString("COIN-D6 detected on %1 (%2 valid packets) — capture it with the PHONE app. "
              "The D6 has no IMU, so the phone's ARCore supplies the 6-DoF trajectory and "
              "the A8 pushbroom builds the 3D cloud. This desktop still replays and "
              "post-processes D6 projects.")
          .arg(d.port)
          .arg(d.packets_ok));
  auto_detect_d6_line_->setProperty("tone", "pose");
  repolish(auto_detect_d6_line_);
}

void CaptureWindow::applyUm982Result(const DiscoveryResult& r, bool silent) {
  const auto& u = r.um982;
  if (!u.found) {
    auto_detect_um982_line_->setText(
        "UM982: not seen — check power and the USB-serial cable. A probe hit only needs "
        "the receiver to talk NMEA, not a satellite fix, so this is not about sky "
        "visibility.");
    auto_detect_um982_line_->setProperty("tone", "warn");
    repolish(auto_detect_um982_line_);
    if (um982_heading_) um982_heading_->setText("Dual-antenna heading: unknown");
    return;
  }
  auto_detect_um982_line_->setText(
      QString("UM982: found on %1 @ %2 baud, %3 heading (%4 valid sentences seen).")
          .arg(u.port)
          .arg(u.baud)
          .arg(u.has_heading ? "dual-antenna" : "single-antenna, no")
          .arg(u.sentences_ok));
  auto_detect_um982_line_->setProperty("tone", "good");
  repolish(auto_detect_um982_line_);

  const bool port_unselected = um982_port_->currentText().trimmed().isEmpty();
  if (!silent || port_unselected) {
    const int idx = um982_port_->findData(u.port);
    if (idx >= 0) {
      um982_port_->setCurrentIndex(idx);
    } else {
      um982_port_->addItem(u.port, u.port);
      um982_port_->setCurrentIndex(um982_port_->count() - 1);
    }
    um982_baud_->setValue(u.baud);
  }
  if (um982_heading_) {
    um982_heading_->setText(QString("Dual-antenna heading: %1")
                                 .arg(u.has_heading ? "yes (GPTHS sentence present)" : "no"));
  }
}

void CaptureWindow::log(const QString& s) {
  // Also to stderr, next to the engine's own [scanengine][...] lines: a headless
  // CLI evidence run (every field-Mac session is one) must be able to see the
  // app's side of a capture, including the discovery/device serialization
  // messages that say WHY a device did or did not arm (NOTES.md §16.7).
  std::fprintf(stderr, "[lidarscan][capture] %s\n", s.toUtf8().constData());
  Q_EMIT logLine("[capture] " + s);
  if (!log_) return;
  log_->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz ") + s);
}

}  // namespace lidarscan
