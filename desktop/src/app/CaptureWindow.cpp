#include "app/CaptureWindow.h"

#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
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
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "app/DeviceDiscovery.h"
#include "app/EngineHost.h"
#include "app/FieldLog.h"
#include "app/Project.h"
#include "app/SerialReader.h"
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

// Space-grouped thousands, the same reading MainWindow's fmt() gives every
// other point count in the redesign (a local copy: MainWindow's lives in its
// own translation unit's anonymous namespace).
QString groupedCount(quint64 n) {
  QString s = QString::number(n);
  for (int i = s.size() - 3; i > 0; i -= 3) s.insert(i, QChar(0x2009));  // thin space
  return s;
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

// Hide a whole QFormLayout row, LABEL INCLUDED. QFormLayout::setRowVisible()
// would do it in one call but only from Qt 6.4, and this app still configures
// against whatever Qt the Windows/Linux CI legs have; labelForField() has been
// there since Qt 4. Hiding only the field leaves a caption pointing at nothing,
// which is how a panel ends up saying "Broadcast code" beside a Mid-360.
void setFormRowVisible(QFormLayout* form, QWidget* field, bool visible) {
  if (!form || !field) return;
  field->setVisible(visible);
  if (QWidget* label = form->labelForField(field)) label->setVisible(visible);
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

// --- FIELD BUG E: where a capture is allowed to land ----------------------
//
// The owner's second field session recorded a real project INTO THE APP
// BUNDLE: ~/Applications/LidarScan.app/Contents/MacOS/record-cycles/Scan-011…
// .lscan. A .app is a signed, replaceable artefact — the next install deletes
// the operator's scan — so no capture may EVER be created inside one, whatever
// QSettings says. These three predicates are the guard; NOTES.md §19.2 has the
// root cause (a CLI evidence hook persisted its own scratch root).

// Documents, not the home directory (§17.5), and not the bundle.
QString defaultCaptureRoot() {
  QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  if (docs.isEmpty()) docs = QDir::homePath();
  return docs + "/LidarScan Projects";
}

// True if any component of `path` is a macOS bundle (…/Something.app/…). A
// string test on purpose: it must hold on Windows and Linux too, where a copied
// settings file or a synced home directory can still name one.
bool insideAppBundle(const QString& path) {
  const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
  for (const QString& part : clean.split('/')) {
    if (part.endsWith(".app", Qt::CaseInsensitive)) return true;
  }
  return false;
}

// Usable = not in a bundle, not inside the installed application's own
// directory, and either an existing writable directory or creatable inside one.
// `why` gets the one sentence the log line needs.
bool captureRootUsable(const QString& path, QString* why) {
  if (path.isEmpty()) {
    if (why) *why = "it is empty";
    return false;
  }
  const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  if (insideAppBundle(clean)) {
    if (why) *why = "it is inside a .app bundle, which the next install replaces";
    return false;
  }
  const QString appDir =
      QDir::cleanPath(QFileInfo(QCoreApplication::applicationDirPath()).absoluteFilePath());
  if (!appDir.isEmpty() && (clean == appDir || clean.startsWith(appDir + "/"))) {
    if (why) *why = "it is inside the installed application's own directory";
    return false;
  }
  // Walk up to the nearest component that exists and ask whether a directory
  // could be made there. Nothing is created by this check.
  QString probe = clean;
  while (!probe.isEmpty() && !QFileInfo::exists(probe)) {
    const QString parent = QFileInfo(probe).absolutePath();
    if (parent == probe) break;
    probe = parent;
  }
  const QFileInfo fi(probe);
  if (!fi.exists() || !fi.isDir()) {
    if (why) *why = "no part of it exists";
    return false;
  }
  if (!fi.isWritable()) {
    if (why) *why = QString("%1 is not writable").arg(probe);
    return false;
  }
  return true;
}

}  // namespace

CaptureWindow::CaptureWindow(EngineHost* host, scanengine::DisplayParamsController* params,
                             QWidget* parent)
    : QDockWidget("CAPTURE — NEW SCAN", parent), host_(host), params_(params) {
  setObjectName("captureDock");
  setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
  buildUi();
  setPhase(Phase::kIdle);
  healCaptureRootSetting();  // field bug E: a bad stored root never survives a launch

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

void CaptureWindow::setProjectDir(const QString& dir, bool persist) {
  if (dir.isEmpty()) return;
  // Round 5: capture creates projects, it does not live inside one. A .lscan
  // path names the project a caller (a CLI hook) wants; its PARENT is the root
  // new projects go into.
  const QFileInfo fi(dir);
  project_root_ = dir.endsWith(".lscan", Qt::CaseInsensitive) ? fi.absolutePath()
                                                              : fi.absoluteFilePath();
  // FIELD BUG E. This used to write "capture/root" UNCONDITIONALLY, and every
  // caller it has is a CLI hook — so --record-cycles' scratch directory (which
  // defaulted to a path inside the .app bundle) became the GUI's permanent
  // capture root on the next normal launch. A hook's scratch path is a fact
  // about that run, never a preference; only an explicit, validated choice by
  // the operator is allowed to persist, and `persist` defaults to false so a
  // future hook cannot make this mistake by omission.
  if (persist) {
    QString why;
    if (captureRootUsable(project_root_, &why)) {
      QSettings s;
      s.setValue("capture/root", project_root_);
      // The marker that separates "the operator chose this" from "a hook
      // touched it": see healCaptureRootSetting().
      s.setValue("capture/rootChosenByOperator", true);
    } else {
      log(QString("not saving '%1' as the default capture folder — %2")
              .arg(project_root_, why));
    }
  }
  updateNameHint();
}

// Startup self-heal, called once from the constructor. A stored root that
// points inside a .app bundle, at something that cannot exist, or at something
// unwritable is not a preference worth keeping: it is how the owner's scans
// ended up in Contents/MacOS. Reset to the Documents default, one log line,
// no dialog.
void CaptureWindow::healCaptureRootSetting() {
  QSettings s;
  const QString saved = s.value("capture/root").toString();
  if (saved.isEmpty()) return;

  // A root that no HUMAN chose. Until this fix, "capture/root" had exactly one
  // writer — setProjectDir(), and every caller of setProjectDir() is a CLI
  // evidence hook — so a stored root with no "chosen by the operator" marker
  // beside it is, by construction, a hook's scratch directory that leaked into
  // the settings a normal launch reads. That is how the owner's scans ended up
  // in ~/Applications/LidarScan.app/Contents/MacOS/record-cycles/. Drop it
  // once; from this build on, only an explicit operator choice writes either
  // key, and no hook writes settings at all (main.cpp isolates them).
  if (!s.value("capture/rootChosenByOperator", false).toBool()) {
    s.remove("capture/root");
    log(QString("saved capture folder '%1' was left behind by a CLI evidence run, not "
                "chosen by you — cleared; new scans go to %2")
            .arg(saved, defaultCaptureRoot()));
    return;
  }

  QString why;
  if (captureRootUsable(saved, &why)) return;
  s.remove("capture/root");
  s.remove("capture/rootChosenByOperator");
  log(QString("saved capture folder '%1' rejected (%2) — new scans go to %3")
          .arg(saved, why, defaultCaptureRoot()));
}

QString CaptureWindow::captureRoot() const {
  // Every answer is validated, including a CLI hook's, so nothing downstream
  // has to remember to check: this is the ONE function that says where a new
  // scan may be created. Documents, not the home directory — the first default
  // here was `~/LidarScan`, which on a case-insensitive macOS filesystem is the
  // SAME DIRECTORY as a checkout named `~/lidarscan` (§17.5) — and
  // QStandardPaths gets the localized/redirected (OneDrive, XDG) path right on
  // all three platforms.
  if (!project_root_.isEmpty() && captureRootUsable(project_root_, nullptr)) {
    return project_root_;
  }
  const QString saved = QSettings().value("capture/root").toString();
  if (!saved.isEmpty() && captureRootUsable(saved, nullptr)) return saved;
  return defaultCaptureRoot();
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
  outer->addWidget(sectionLabel("Lidar link"));

  // A17: the model selector lives OUTSIDE the collapsible manual box, because
  // it is not a manual fallback — it decides which driver the whole panel is
  // talking about, including which discovery hit is allowed to auto-arm. It is
  // the first thing in the column for the same reason.
  {
    auto* mf = new QFormLayout();
    mf->setContentsMargins(0, 0, 0, 0);
    mf->setSpacing(4);
    lidar_model_ = new QComboBox();
    lidar_model_->addItem("Livox Mid-360", int(LidarModel::kMid360));
    lidar_model_->addItem("Livox Mid-70", int(LidarModel::kMid70));
    lidar_model_->setToolTip(
        "Two different protocols, not two settings of one. The Mid-360 speaks Livox "
        "SDK2 (heartbeat on UDP 56201, three data ports, a built-in IMU); the Mid-70 "
        "speaks SDK v1 (broadcast on UDP 55000, a 15-character broadcast code, and NO "
        "IMU of its own \u2014 pair it with the serial IMU module below). Auto-detect sets "
        "this for you when it hears one.");
    connect(lidar_model_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
              onLidarModelChanged();
              // The two models keep their addresses in SEPARATE QSettings
              // groups, so switching the combo also swaps which set the fields
              // show. Deliberately not done on the initial build (the combo is
              // populated before this is connected, and buildUi() calls
              // loadMid360Settings() itself) — only a real change reloads.
              if (lidarModel() == LidarModel::kMid70) {
                loadMid70Settings();
              } else {
                loadMid360Settings();
              }
            });
    mf->addRow("Lidar model", lidar_model_);
    outer->addLayout(mf);
  }

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
  link_form_ = f;  // onLidarModelChanged() hides whole rows through it
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
  mid360_ports_row_ = ports;  // hidden for a Mid-70: SDK v1 does not use these
  f->addRow("Ports (point/imu/cmd)", ports);

  // A17: the broadcast code. READ-ONLY on purpose. It is a 15-character serial
  // printed on the device, the only name SDK v1's AddLidarToConnect() accepts,
  // and a typo in it does not fail loudly \u2014 it silently connects to nothing,
  // or to the wrong lidar on a shared switch. Auto-detect is the way it gets
  // filled; empty means "the first Mid-70 that broadcasts", which is exactly
  // right on a bench with one lidar and exactly wrong on a site with two, so
  // the hint says so.
  mid70_code_ = new QLineEdit();
  mid70_code_->setReadOnly(true);
  mid70_code_->setPlaceholderText("(any Mid-70 that broadcasts)");
  mid70_code_->setToolTip(
      "Filled by auto-detect from the SDK v1 broadcast. Empty = connect to the first "
      "Mid-70 heard, which is right for one lidar on a bench and wrong for two on a "
      "switch. Read-only: it is the device's own serial, not a setting.");
  mid70_code_row_ = mid70_code_;
  f->addRow("Broadcast code", mid70_code_);
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

  // --- A18: the serial IMU row (Mid-70 sessions only) ---------------------
  //
  // OUTSIDE manual_box_, because it is not a fallback for a failed detection:
  // a Mid-70 has no IMU at all, so choosing one is part of setting the session
  // up, not part of repairing it. Hidden wholesale for a Mid-360, which has its
  // own IMU inside the SDK2 stream and would be actively harmed by a second,
  // unaligned one.
  imu_row_ = new QWidget();
  {
    auto* iv = new QVBoxLayout(imu_row_);
    iv->setContentsMargins(0, 6, 0, 0);
    iv->setSpacing(4);
    iv->addWidget(sectionLabel("IMU (Mid-70 has none of its own)"));
    auto* jf = new QFormLayout();
    jf->setContentsMargins(0, 0, 0, 0);
    jf->setSpacing(4);
    imu_serial_port_ = new QComboBox();
    imu_serial_port_->setToolTip(
        "The JuxiTech ICM-42670-P module, 115200 8N1. The app owns this port and pushes "
        "its bytes into the engine (transport/byte_source.h: the engine never opens a "
        "serial device). Auto-detect selects it when its probe identifies one.");
    jf->addRow("Serial port", imu_serial_port_);
    iv->addLayout(jf);
    imu_hint_ = hintLabel(
        "Optional, and honestly so: without it a Mid-70 session records every point but "
        "live LIO has no gyro to initialise on. The module also STOPS TRANSMITTING for "
        "~2.9 s roughly every 35 s \u2014 measured vendor-firmware behaviour that nothing in "
        "its protocol turns off. The driver counts those blackouts and shows them in the "
        "health line rather than smoothing them into invented motion.");
    iv->addWidget(imu_hint_);
  }
  outer->addWidget(imu_row_);
  refreshImuPortList();

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
  // Everything the model decides is applied from ONE place, so the initial
  // state and every later change go through the same code.
  onLidarModelChanged();
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

  // The live map is a WINDOW once the engine's page store reaches its ceiling
  // (engine ABI 7). Quiet, inline, and never a popup: nothing is wrong, and
  // nothing the operator does can make it not happen on a long scan.
  live_window_note_ = new QLabel();
  live_window_note_->setWordWrap(true);
  live_window_note_->setVisible(false);
  live_window_note_->setToolTip(
      "The live view holds a bounded number of points so it can keep up forever. "
      "Once that ceiling is reached the OLDEST points leave the view to make room for "
      "the newest — the view never stops moving. The recording is a separate path and "
      "keeps every point; post-processing rebuilds the whole cloud from it.");
  v->addWidget(live_window_note_);

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

void CaptureWindow::noteRefreshGovernor(double hz, bool down, const QString& why) {
  if (!refresh_hz_) return;
  updating_display_ = true;
  refresh_hz_->setValue(hz);  // does not emit — the viewport already applied it
  updating_display_ = false;
  // ROUND-5 FIELD BUG B: this used to PERSIST the governed value into
  // "capture/liveRefreshHz". A machine that stuttered once therefore came back
  // from the next launch permanently capped at whatever notch the governor had
  // reached — the setting the operator chose was overwritten by a measurement.
  // The persisted value is the REQUEST, and only setLiveRefreshHz (a slider
  // move) writes it; the governor is a temporary, measured override.
  if (refresh_note_) {
    refresh_note_->setText(
        down ? QString("Live refresh eased to %1 fps — this machine could not sustain the "
                       "previous rate (%2). It will come back up on its own once frames "
                       "are cheap again. The capture is unaffected: recording is never "
                       "throttled.")
                   .arg(hz, 0, 'f', 0)
                   .arg(why)
             : QString("Live refresh back up to %1 fps (%2).").arg(hz, 0, 'f', 0).arg(why));
    refresh_note_->setProperty("tone", down ? "warn" : "");
    repolish(refresh_note_);
    refresh_note_->setVisible(true);
  }
  log(QString("live refresh governor %1 -> %2 fps (%3); recording untouched")
          .arg(down ? "downshift" : "recovery")
          .arg(hz, 0, 'f', 0)
          .arg(why));
}

// --- item 18: walkthrough-first ------------------------------------------
//
// The operator walks the space with the rig, so the two things they cannot see
// from behind the screen are "where have I been" and "am I going too fast".
//
// THREE ENGINE SEAMS ARE MISSING HERE, and all three are worked around rather
// than papered over (NOTES.md §17.6 / §17.9):
//   1. LioPoseSource exposes latest() / size() / trajectory_length_m() but NO
//      way to READ the pose ring, so the trail cannot be reconstructed from the
//      engine — this polls the newest pose at 10 Hz (LIO's own rate) and
//      accumulates the path on the app side. A pass that misses a pose loses a
//      corner of the trail, never a point of the capture.
//   2. There is no motion-gate event on this path at all: EventType has no
//      "moving too fast" (event.h's list stops at kError), and the A8 pushbroom's
//      skipped-turning counters belong to the phone-only D6 flow. So the speed is
//      DERIVED here from pose positions, and the hint says what it measured
//      rather than claiming to be the engine's own gate.
//   3. There is no way to ask whether the pose FRAME changed. Every session
//      restart builds a new LioOdometry whose first pose is the origin, and
//      nothing in the pose API distinguishes "I walked back to where I started"
//      from "this is a different odometry". So the app tells ITSELF, via
//      resetWalkTracking(), at each of the five places it restarts the session.
//
// ROUND-5 FIELD BUG A. This used to compute the speed from two consecutive
// polls on a Qt wall clock, with a 1 ms dt floor, no check that the pose was
// new, and no idea that the frame had reset — five independent ways to report
// "walking" from a rig on a tripod. WalkSpeedEstimator.h enumerates them; the
// measurement now lives there and is driven by Pose::t_mono_ns.
void CaptureWindow::resetWalkTracking(const char* why) {
  walk_.reset();
  if (!trail_.empty()) {
    trail_.clear();
    Q_EMIT trajectoryTrailChanged(trail_);
  }
  if (walk_label_) walk_label_->setText(QString("Walking 0.00 m/s · new pose frame (%1)")
                                            .arg(QString::fromUtf8(why)));
}

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

  // --- the IMU gate, read FIRST (round-5 field bug A) ---------------------
  //
  // ImuIngest is engine-lifetime, not session-lifetime, so this survives the
  // session restarts that Start/Pause/Resume/Stop perform. `recent()` returns
  // the newest samples oldest-first; 240 of them is ~1.2 s of the Mid-360's
  // 200 Hz IMU, i.e. the same window the speed estimator uses.
  constexpr std::size_t kImuWindow = 240;
  scanengine::ImuSample imu_buf[kImuWindow];
  const std::size_t imu_n = host_->engine()->imu().recent(imu_buf, kImuWindow);
  // Stack buffers, not vectors: this runs ten times a second for the whole of a
  // capture, and a heap round-trip per poll on the GUI thread is exactly the
  // kind of avoidable cost the refresh governor would end up reacting to.
  double gyro[3 * kImuWindow], accel[3 * kImuWindow];
  for (std::size_t i = 0; i < imu_n; ++i) {
    for (int k = 0; k < 3; ++k) {
      gyro[3 * i + k] = double(imu_buf[i].gyro_rad_s[k]);
      accel[3 * i + k] = double(imu_buf[i].accel_m_s2[k]);
    }
  }
  const MotionGate::Reading gate = MotionGate::measure(gyro, accel, imu_n);

  // The estimator owns "is this pose new", "is this dt usable" and "did the
  // frame just reset". It returns true only for a genuinely new, in-frame
  // sample, which is also the only kind that may extend the trail.
  const bool fresh = walk_.update(latest.t_mono_ns, latest.position);

  // A rig the IMU says is parked has not moved, whatever the odometry's
  // position did. Reporting 0 here is the fix for "wrongly detect me walking
  // while I stay still", and NOT extending the trail is half the fix for "the
  // live view stops changing": a drifting pose was pushing a new trail vertex
  // every 100 ms, and every one of them cost the viewport a full trail-geometry
  // rebuild on the next presented frame.
  // LIO DOES flag a pose it knows is bad — `LioOdometry` sets
  // Pose::quality = kInvalid and tracking_lost = 1 once its ESKF passes
  // Eskf::diverged() (default 30 m/s). That threshold is far above the 1-4 m/s
  // of false velocity ordinary drift produces, so this is a backstop rather than
  // the fix (the IMU gate below is the fix) — but a pose the odometry itself
  // disowns must never become a number on screen.
  const bool pose_disowned =
      latest.tracking_lost != 0 || latest.quality == scanengine::PoseQuality::kInvalid;

  const bool still = gate.valid && gate.still;
  const double v = (still || pose_disowned) ? 0.0 : walk_.speedMps();

  const std::array<float, 3> p{float(latest.position[0]), float(latest.position[1]),
                               float(latest.position[2])};
  if (fresh && !still && !pose_disowned) {
    bool appended = false;
    if (trail_.empty()) {
      trail_.push_back(p);
      appended = true;
    } else {
      const auto& last = trail_.back();
      const double dx = p[0] - last[0], dy = p[1] - last[1], dz = p[2] - last[2];
      // 2 cm of movement before a new trail vertex: LIO publishes at 10 Hz
      // whether the rig moved or not, and a standing operator must not grow the
      // buffer.
      if (std::sqrt(dx * dx + dy * dy + dz * dz) > 0.02) {
        trail_.push_back(p);
        appended = true;
        // ~40 m of 2 cm steps before the oldest vertex is dropped; the trail is a
        // recent-history overlay, not the recorded trajectory (that is in the
        // .lscan).
        constexpr std::size_t kMaxTrailVertices = 2000;
        if (trail_.size() > kMaxTrailVertices) {
          trail_.erase(trail_.begin(), trail_.begin() + (trail_.size() - kMaxTrailVertices));
        }
      }
    }
    if (appended) Q_EMIT trajectoryTrailChanged(trail_);
  }

  // ODOMETRY DIVERGENCE, said out loud once a minute rather than swallowed.
  // The engine has no pose covariance, no quality flag on a LIO pose and no
  // "odometry diverged" event (NOTES.md §17.9's seam list), so this disagreement
  // — the IMU is certain the rig is parked while the pose keeps moving — is the
  // only signal an operator or a support log will ever get. It is a note, never
  // a dialog, and it never touches the recording: every raw byte is still
  // written whatever the odometry believes.
  if ((pose_disowned || (still && walk_.valid() && walk_.speedMps() > 0.25)) &&
      (!drift_clock_.isValid() || drift_clock_.elapsed() > 60000)) {
    drift_clock_.restart();
    log(QString("live odometry is %1: the IMU reads the rig as %2 (|a| dev %3 m/s², "
                "gyro %4 rad/s) while the SLAM pose implies %5 m/s (pose quality %6, "
                "sigma %7 m). The walk hint reports 0; recording is unaffected — every "
                "raw byte is still written.")
            .arg(pose_disowned ? "LOST (the engine flagged it)" : "drifting")
            .arg(still ? "stationary" : "moving")
            .arg(gate.accel_dev_m_s2, 0, 'f', 3)
            .arg(gate.gyro_rms_rad_s, 0, 'f', 3)
            .arg(walk_.speedMps(), 0, 'f', 2)
            .arg(int(latest.quality))
            .arg(latest.position_sigma_m, 0, 'f', 3));
  }

  // 1.5 m/s is a brisk walk; above it a 10 Hz LIO scan-match starts to see
  // between-scan motion it must undistort rather than register, which is the
  // regime where a walkthrough smears. Gentle, inline, no modal, no sound.
  //
  // The hint fires only on a VALID measurement — a full window of pose time in
  // one frame, with the IMU corroborating that the rig is being carried at all.
  // Anything less says so instead of guessing, because a guess here is precisely
  // the bug the owner reported.
  const double len_m = slam->poses().trajectory_length_m();
  const bool measured = !pose_disowned && (still || walk_.valid());
  const bool too_fast = measured && v > 1.5;
  if (pose_disowned) {
    walk_label_->setText(QString("Odometry lost tracking — no speed to report · %1 m of "
                                 "path · %2 poses · recording is unaffected")
                             .arg(len_m, 0, 'f', 1)
                             .arg(slam->poses().size()));
  } else if (still) {
    walk_label_->setText(QString("Holding still (0.00 m/s) · %1 m of path · %2 poses")
                             .arg(len_m, 0, 'f', 1)
                             .arg(slam->poses().size()));
  } else if (!walk_.valid()) {
    walk_label_->setText(QString("Measuring walking speed… · %1 m of path · %2 poses")
                             .arg(len_m, 0, 'f', 1)
                             .arg(slam->poses().size()));
  } else {
    walk_label_->setText(
        too_fast ? QString("Walking %1 m/s — ease off a little; %2 m of path so far.")
                       .arg(v, 0, 'f', 2)
                       .arg(len_m, 0, 'f', 1)
                 : QString("Walking %1 m/s · %2 m of path · %3 poses")
                       .arg(v, 0, 'f', 2)
                       .arg(len_m, 0, 'f', 1)
                       .arg(slam->poses().size()));
  }
  walk_label_->setProperty("tone", too_fast ? "warn" : "");
  repolish(walk_label_);
  walk_label_->setVisible(true);
  Q_EMIT walkSpeedMeasured(v, measured, walk_.discontinuities());
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
// A17/A18 — lidar model, and the serial IMU that a Mid-70 session needs
// ---------------------------------------------------------------------------

CaptureWindow::LidarModel CaptureWindow::lidarModel() const {
  if (!lidar_model_) return LidarModel::kMid360;
  return LidarModel(lidar_model_->currentData().toInt());
}

void CaptureWindow::setLidarModel(LidarModel m) {
  if (!lidar_model_) return;
  const int idx = lidar_model_->findData(int(m));
  if (idx >= 0 && idx != lidar_model_->currentIndex()) lidar_model_->setCurrentIndex(idx);
}

// Everything that differs between the two models, in one function, so the panel
// cannot end up half-configured for each.
void CaptureWindow::onLidarModelChanged() {
  const bool mid70 = lidarModel() == LidarModel::kMid70;

  // The three UDP ports are SDK2's data/IMU/command channels. SDK v1 does not
  // have them — it handshakes on the lidar's port 65000 after a broadcast on
  // 55000 — so showing them for a Mid-70 would be three settings that do
  // nothing, which is worse than three settings that are missing.
  setFormRowVisible(link_form_, mid360_ports_row_, !mid70);
  setFormRowVisible(link_form_, mid70_code_row_, mid70);
  if (imu_row_) imu_row_->setVisible(mid70);

  if (mid_hint_) {
    mid_hint_->setText(
        mid70 ? QString("Auto-detect normally fills these in. For a Mid-70 the LIDAR IP is "
                        "informational (SDK v1 finds the device by its broadcast); the HOST "
                        "IP must be an address this Mac actually holds on the lidar's "
                        "network, because that is where the lidar is told to stream.")
              : QString("Auto-detect normally fills these in. The lidar IP is REQUIRED on "
                        "macOS (stock SDK2 broadcast discovery fails with EADDRNOTAVAIL "
                        "there — S2-sim finding)."));
  }
  if (imu_serial_port_) refreshImuPortList();
}

// A17. LioConfig's near gate defaults to 0.5 m, which is sized for a Mid-360.
// The Mid-70's blind zone is 0.05 m, and the ROS work that preceded this port
// measured a 0.5 m gate discarding most returns in a tight room — so a Mid-70
// session asks for 0.2 m. 0 means "leave the engine's own default alone", which
// is what every Mid-360 session gets and why that path is unchanged.
float CaptureWindow::lioNearGateForModel() const {
  return lidarModel() == LidarModel::kMid70 ? 0.2f : 0.0f;
}

void CaptureWindow::loadMid70Settings() {
  if (!mid70_code_) return;
  QSettings s;
  s.beginGroup("mid70/last");
  // Its OWN group beside "mid360/last": the two models have different addresses
  // on the same bench, and one overwriting the other is how an operator ends up
  // arming a Mid-70 at the Mid-360's IP.
  if (s.contains("hostIp")) host_ip_->setText(s.value("hostIp").toString());
  if (s.contains("lidarIp")) lidar_ip_->setText(s.value("lidarIp").toString());
  mid70_code_->setText(s.value("broadcastCode", mid70_code_->text()).toString());
  const QString port = s.value("imuPort").toString();
  if (!port.isEmpty() && imu_serial_port_) {
    const int idx = imu_serial_port_->findData(port);
    if (idx >= 0) {
      imu_serial_port_->setCurrentIndex(idx);
    } else {
      // The adapter is not plugged in right now. Keep the name visible rather
      // than silently forgetting it: "the port I used last time is gone" is
      // information, and the combo's own list says which ports DO exist.
      imu_serial_port_->addItem(port + " (not present)", port);
      imu_serial_port_->setCurrentIndex(imu_serial_port_->count() - 1);
    }
  }
  s.endGroup();
}

void CaptureWindow::saveMid70Settings() {
  if (!mid70_code_) return;
  QSettings s;
  s.beginGroup("mid70/last");
  s.setValue("hostIp", host_ip_->text());
  s.setValue("lidarIp", lidar_ip_->text());
  s.setValue("broadcastCode", mid70_code_->text());
  s.setValue("imuPort", selectedImuPort());
  s.endGroup();
}

void CaptureWindow::refreshImuPortList() {
  if (!imu_serial_port_) return;
  const QString keep = selectedImuPort();
  const QSignalBlocker block(imu_serial_port_);
  imu_serial_port_->clear();
  // "(none)" FIRST and selected by default. A Mid-70 with no IMU is a real
  // configuration — it records every point — so the default must not be to
  // grab whatever serial device happens to be plugged in.
  imu_serial_port_->addItem("(none)", QString());
  for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
    // systemLocation() is what QSerialPort::setPortName() and the engine's own
    // probe both name a port by (/dev/cu.usbserial-… on macOS, COM3 on
    // Windows); portName() drops the /dev/ prefix and would not match the
    // probe's hit.
    const QString path = info.systemLocation();
    QString label = path;
    if (!info.description().isEmpty()) label += "  " + info.description();
    imu_serial_port_->addItem(label, path);
  }
  if (!keep.isEmpty()) {
    const int idx = imu_serial_port_->findData(keep);
    if (idx >= 0) {
      imu_serial_port_->setCurrentIndex(idx);
    } else {
      imu_serial_port_->addItem(keep + " (not present)", keep);
      imu_serial_port_->setCurrentIndex(imu_serial_port_->count() - 1);
    }
  }
}

QString CaptureWindow::selectedImuPort() const {
  if (!imu_serial_port_) return QString();
  return imu_serial_port_->currentData().toString();
}

bool CaptureWindow::armImuSerial(QString* err) {
  const QString port = selectedImuPort();
  if (port.isEmpty()) return true;  // "(none)" is a choice, not a failure
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }

  imu_reader_ = new SerialReader(this);
  connect(imu_reader_, &SerialReader::logLine, this, &CaptureWindow::log);
  connect(imu_reader_, &SerialReader::disconnected, this, [this](const QString& why) {
    // The module was unplugged mid-session. Say it once, plainly, and leave the
    // lidar alone: the recording continues and every point still lands. The
    // health line goes on reporting the IMU device's own state, which the
    // engine will move to degraded/fault on its blackout watchdog.
    log(QString("serial IMU disconnected (%1) — the lidar is unaffected and the recording "
                "continues, but live odometry now has no gyro").arg(why));
  });

  QString oerr;
  // 115200 8N1 and only that: A18's module has no other rate (discovery.h,
  // ProbeSerialJuxiImu — "there is no sweep, because the module has no other
  // rate").
  if (!imu_reader_->open(port, 115200, &oerr)) {
    if (err) *err = oerr;
    imu_reader_->deleteLater();
    imu_reader_ = nullptr;
    return false;
  }

  scanengine::ImuSerialConfig cfg;
  cfg.serial.port_name = "";  // EngineHost owns the string; see addImuSerial()
  cfg.serial.baud = 115200;
  // report_rate_hz / send_rate_command are left at their header defaults (100 Hz,
  // on). That single frame is the only thing anything here ever writes to the
  // port, and it goes out through the write_fn bridge EngineHost installs.
  imu_device_ = host_->addImuSerial(cfg, imu_reader_, err);
  if (imu_device_ == scanengine::kInvalidDeviceId) {
    imu_reader_->close();
    imu_reader_->deleteLater();
    imu_reader_ = nullptr;
    return false;
  }
  return true;
}

void CaptureWindow::disarmImuSerial(const QString& why) {
  if (imu_device_ != scanengine::kInvalidDeviceId && host_) {
    QString err;
    // remove_device() first: until it returns, the driver may still call the
    // write_fn that points at this reader.
    (void)host_->removeDevice(imu_device_, &err);
    imu_device_ = scanengine::kInvalidDeviceId;
  }
  if (imu_reader_) {
    imu_reader_->clearTarget();
    imu_reader_->close();
    imu_reader_->deleteLater();
    imu_reader_ = nullptr;
    log("serial IMU port closed — " + why);
  }
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
  // Engine::start_session() builds a NEW LioOdometry whose first pose is the
  // origin. Whatever the trail and the speed window held belongs to the previous
  // odometry's frame; carrying it across is field bug A's largest single source
  // (a whole trajectory's worth of displacement inside one 100 ms poll).
  resetWalkTracking("preview session started");
  const float near_gate = lioNearGateForModel();
  if (host_->startSession(QString(), profile_->currentText(), false, err,
                          /*live_slam=*/true, near_gate)) {
    live_slam_running_ = true;
    return true;
  }
  live_slam_running_ = false;
  log(QString("live SLAM would not start (%1) — continuing Record-only, so there is no "
              "trajectory trail this session")
          .arg(err ? *err : QString("unknown")));
  return host_->startSession(QString(), profile_->currentText(), false, err,
                             /*live_slam=*/false, near_gate);
}

bool CaptureWindow::startRecordingSession(QString* err) {
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  if (host_->sessionActive() && !host_->stopSession(err)) return false;
  // Same live-SLAM decision as the preview session, and the same fallback: a
  // recording never fails because the odometry could not start. And the same
  // pose-frame reset — a new session is a new odometry, numbered from 0.
  resetWalkTracking("recording session started");
  const float near_gate = lioNearGateForModel();
  if (host_->startSession(last_project_dir_, profile_->currentText(), true, err,
                          /*live_slam=*/true, near_gate)) {
    live_slam_running_ = true;
    return true;
  }
  live_slam_running_ = false;
  log(QString("live SLAM would not start for the recording (%1) — Record-only; every raw "
              "byte is still recorded")
          .arg(err ? *err : QString("unknown")));
  return host_->startSession(last_project_dir_, profile_->currentText(), true, err,
                             /*live_slam=*/false, near_gate);
}

bool CaptureWindow::armPreview(QString* err) {
  if (phase_ != Phase::kIdle) return true;  // already armed/recording
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  const bool mid70 = lidarModel() == LidarModel::kMid70;
  if (!mid70 && lidar_ip_->text().trimmed().isEmpty()) {
    if (err) *err = "no lidar IP yet (macOS cannot discover by broadcast — S2 finding)";
    return false;
  }
  // A Mid-70 is found by its BROADCAST, not by an address we type, so an empty
  // lidar IP is legitimate here where it is fatal for a Mid-360. What is not
  // legitimate is having neither: with no host IP the lidar has nowhere to
  // stream to, and SDK v1 names the host in the handshake every time.
  if (mid70 && host_ip_->text().trimmed().isEmpty()) {
    if (err) *err = "no host IP yet — SDK v1 tells the Mid-70 where to stream on every connect";
    return false;
  }
  // Discovery and the Livox SDK both want UDP 56201; the device must own it
  // alone. This BLOCKS (bounded by one DiscoveryGate slice, ~1 s) until the
  // discovery worker's socket is really closed — NOTES.md §16.7.
  if (!stopDiscoveryForDeviceUse("live preview")) {
    if (err) {
      *err = mid70 ? QString("auto-detect is still holding UDP 55000 — try again in a moment")
                   : QString("auto-detect is still holding UDP 56201 — try again in a moment");
    }
    return false;
  }

  // The store this preview feeds is now a LIVE CAPTURE's moving window, not a
  // review workspace: at its ceiling it recycles its oldest page instead of
  // dropping every new point for the rest of the run. That drop-forever is
  // FIELD BUG D (NOTES.md §19.1) — "live view not moving even i move the
  // lidar". Enabled BEFORE start_session(), because start_session() is what
  // empties the window for the new pose frame.
  host_->setLivePageEviction(true);

  if (!startPreviewSession(err)) return false;

  if (mid70) {
    // A17. Beside the Mid-360 path, never inside it: with the combo on Mid-360
    // this whole branch is dead code, which is what makes "the Mid-360 flow is
    // unchanged" a checkable statement rather than a hope.
    scanengine::Mid70Config cfg;
    cfg.backend = scanengine::Mid70Backend::kSdk1;  // the only backend that brings a device up
    cfg.udp.host_ip = host_ip_->text().trimmed().toStdString();
    cfg.udp.lidar_ip = lidar_ip_->text().trimmed().toStdString();
    // Empty = "the first Mid-70 heard" (mid70_driver.h). The read-only field is
    // filled by auto-detect; a bench with one lidar leaves it empty and is fine.
    cfg.broadcast_code = mid70_code_->text().trimmed().toStdString();
    // Everything else — filter, decimation budget, reconnect policy, dual
    // return — stays at the driver header's defaults. The panel has never
    // exposed the Mid-360's equivalents either, and inventing UI for knobs that
    // have not been exercised on hardware would be inventing confidence.
    last_mid70_cfg_ = std::make_unique<scanengine::Mid70Config>(cfg);
    device_ = host_->addMid70(cfg, err);
    if (device_ == scanengine::kInvalidDeviceId) {
      QString stop_err;
      (void)host_->stopSession(&stop_err);
      return false;
    }
    saveMid70Settings();

    // A18. The IMU is added AFTER the lidar so the device numbers read in the
    // order an operator would name them, and its failure never fails the arm:
    // record-always outranks the odometry, exactly as it does for live SLAM in
    // startPreviewSession().
    QString imu_err;
    const QString imu_port = selectedImuPort();
    if (!armImuSerial(&imu_err)) {
      log(QString("serial IMU on %1 would not start (%2) — continuing with the Mid-70 "
                  "alone: every point is still recorded, but live odometry has no gyro")
              .arg(imu_port, imu_err));
    }

    log(QString("Mid-70 %1 at %2 -> host %3, device #%4%5 — live preview (not recording)")
            .arg(mid70_code_->text().isEmpty() ? QStringLiteral("(any broadcast code)")
                                               : mid70_code_->text())
            .arg(lidar_ip_->text().isEmpty() ? QStringLiteral("(address from broadcast)")
                                             : lidar_ip_->text())
            .arg(host_ip_->text())
            .arg(device_)
            .arg(imu_device_ != scanengine::kInvalidDeviceId
                     ? QString("; IMU on %1, device #%2").arg(imu_port).arg(imu_device_)
                     : QString("; no IMU")));
  } else {
    scanengine::Mid360Config cfg;
    cfg.udp.host_ip = host_ip_->text().trimmed().toStdString();
    cfg.udp.lidar_ip = lidar_ip_->text().trimmed().toStdString();
    cfg.udp.point_port = std::uint16_t(point_port_->value());
    cfg.udp.imu_port = std::uint16_t(imu_port_->value());
    cfg.udp.cmd_port = std::uint16_t(cmd_port_->value());
    last_mid360_cfg_ = std::make_unique<scanengine::Mid360Config>(cfg);
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
  }

  // Item 18: hold the display awake for as long as the device is armed — the
  // operator is walking, not typing. Honest about platforms that cannot.
  if (!awake_.held()) {
    if (awake_.acquire("LidarScan capture in progress")) {
      log("display sleep inhibited — " + awake_.reason());
    } else {
      log("display sleep NOT inhibited — " + awake_.reason());
    }
  }
  resetWalkTracking("device armed");

  arm_clock_.start();
  // Set explicitly on BOTH branches, never inherited.
  //
  // 8 s is the Mid-360's: first packet before the A3 connect timeout.
  //
  // 180 s is the MID-70's, and it is not padding. A cold Mid-70 sits in the
  // SDK's kLidarStateInit while it SELF-HEATS, streaming nothing, for up to
  // about three minutes ([M] §4 / the A17 driver header's note on why
  // connect_timeout_ms is a reason to keep trying rather than to fault). The
  // SDK backend keeps re-handshaking throughout. An 8 s — or even 12 s —
  // window would therefore declare "no data" on a device that is working
  // perfectly and simply cold, disarm it, and hand the operator a failure to
  // debug. So the window is longer than the warm-up, and the label below says
  // what the wait is FOR rather than counting down at somebody in silence.
  arm_window_s_ = mid70 ? 180.0 : 8.0;
  FieldLog::info("capture", QString("event=arm arm_window_s=%1 %2")
                                .arg(arm_window_s_, 0, 'f', 0)
                                .arg(fieldConfigKv()));
  endDataWatch();  // kArming has its own first-packet measurement (evaluateArming)
  auto h = host_->engine()->device_health(device_);
  arm_baseline_points_ = h.ok() ? h.value().points_out : 0;
  last_arm_failed_ = false;
  setPhase(Phase::kArming);
  arm_label_->setStyleSheet(QString());
  arm_label_->setText(mid70
                          ? QString("Arming — waiting for the first Mid-70 datagram. A COLD "
                                    "Mid-70 self-heats for up to ~3 minutes before it streams "
                                    "anything; that is normal, not a fault.")
                          : QString("Arming — waiting for the first Mid-360 packet…"));
  Q_EMIT previewStarted();
  return true;
}

bool CaptureWindow::disarmPreview(const QString& why) {
  FieldLog::info("capture",
                 QString("event=disarm why=\"%1\" %2").arg(why, fieldConfigKv()));
  if (phase_ == Phase::kRecording || phase_ == Phase::kPaused) {
    log("refusing to disarm: a recording is open — Stop it first");
    return false;
  }
  if (phase_ == Phase::kIdle) return true;
  QString err;
  // The IMU first: it is the dependent half of a Mid-70 session, and tearing it
  // down while the lidar is still up leaves the shorter-lived object gone
  // first, which is the order this panel can reason about. A no-op for a
  // Mid-360 session, which never had one.
  disarmImuSerial(why);
  if (device_ != scanengine::kInvalidDeviceId && host_) {
    (void)host_->removeDevice(device_, &err);
    device_ = scanengine::kInvalidDeviceId;
  }
  if (host_ && host_->sessionActive()) (void)host_->stopSession(&err);
  // Back to hard-cap semantics: with no device armed this store serves replay,
  // merge previews and loaded post-processing results, and those must SAY they
  // overran rather than silently showing the newest slice of a cloud.
  if (host_) host_->setLivePageEviction(false);
  live_slam_running_ = false;
  endDataWatch();
  if (live_window_note_) live_window_note_->setVisible(false);
  live_window_seen_evicting_ = false;
  setPhase(Phase::kIdle);
  awake_.release();
  resetWalkTracking("device disarmed");
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
    FieldLog::info("capture", QString("event=connected elapsed_s=%1 %2")
                                  .arg(elapsed, 0, 'f', 2)
                                  .arg(fieldConfigKv()));
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
    disarmImuSerial("arm failed");  // no-op unless this was a Mid-70 session
    if (device_ != scanengine::kInvalidDeviceId) {
      (void)host_->removeDevice(device_, &err);
      device_ = scanengine::kInvalidDeviceId;
    }
    (void)host_->stopSession(&err);
    live_slam_running_ = false;
    setPhase(Phase::kIdle);
    awake_.release();
    if (walk_label_) walk_label_->setVisible(false);
    FieldLog::error("capture", QString("event=arm_failed detail=\"%1\" %2")
                                   .arg(detail, fieldConfigKv()));
    log("arm failed: " + detail);
    Q_EMIT selfTestFinished(false, detail);
    return;
  }
  // A17: past the SDK v1 handshake's own connect timeout, a Mid-70 that is
  // still silent is usually WARMING UP rather than missing. Which of the two it
  // is comes from the DEVICE, not from a stopwatch: Mid70Stats::err.self_heating
  // is a bit the lidar sets in its own err_code, so when it is set this says so
  // outright, and when it is not, this says only what it actually knows — the
  // link state and the elapsed time. Getting that distinction from the sensor
  // rather than from a guess is the difference between reassuring an operator
  // correctly and reassuring them about a cable that is unplugged.
  if (lidarModel() == LidarModel::kMid70 && elapsed > 12.0) {
    bool self_heating = false;
    QString link;
    if (auto ms = host_->engine()->mid70_stats(device_); ms.ok()) {
      self_heating = ms.value().err.self_heating;
      link = QString::fromUtf8(scanengine::to_string(ms.value().link));
    }
    arm_label_->setText(
        self_heating
            ? QString("Mid-70 warming up — the device reports SELF-HEATING and streams "
                      "nothing until it is warm (%1 / %2 s). A cold unit takes up to ~3 "
                      "minutes; leave it running.")
                  .arg(elapsed, 0, 'f', 0)
                  .arg(arm_window_s_, 0, 'f', 0)
            : QString("Mid-70: no datagram yet (%1 / %2 s, link %3). The SDK keeps "
                      "re-handshaking; a cold unit self-heats for up to ~3 minutes before "
                      "it streams anything.")
                  .arg(elapsed, 0, 'f', 0)
                  .arg(arm_window_s_, 0, 'f', 0)
                  .arg(link.isEmpty() ? QStringLiteral("unknown") : link));
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
  // Field bug C: Start restarted the session, which tore the SDK down and back
  // up. If the sensor does not come back, this is what notices and re-arms it —
  // without ever closing the .lscan that is now open.
  beginDataWatch("Start restarted the sensor");
  FieldLog::info("capture", QString("event=record_start auto_named=%1 %2")
                                .arg(auto_named ? 1 : 0)
                                .arg(fieldConfigKv()));
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
    beginDataWatch("Pause restarted the sensor");
    log("capture paused — still streaming to the viewport, not recording");
  } else {
    if (!startRecordingSession(&err)) {
      log("resume: " + err);
      return;
    }
    record_segment_clock_.start();
    setPhase(Phase::kRecording);
    beginDataWatch("Resume restarted the sensor");
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
    // The seal restarted the session too, so the live preview the operator is
    // now looking at has exactly the same "did the sensor come back?" question —
    // and answering it here is what makes the NEXT Start work.
    beginDataWatch("Stop restarted the sensor");
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
  // Round-5 field bug C: an EMPTY seal must say so. The old flow showed the REC
  // badge, ran the elapsed clock and then sealed a project with zero chunks
  // without a word — which is exactly how "it only records when first
  // connected" looked from the outside.
  const bool empty_seal = cum_chunks_written_ == 0;
  summary_->setText(empty_seal ? ("NOTHING WAS RECORDED. " + sum +
                                  "  The sensor sent no data for the whole of this scan — "
                                  "check the link and try again; the live preview is still up.")
                               : sum);
  summary_->setProperty("tone", empty_seal ? "warn" : "");
  repolish(summary_);
  log(empty_seal ? ("NOTHING WAS RECORDED — " + sum) : sum);

  // Spec item (g): what the seal actually produced, read back through the SAME
  // Project reader the library uses — per stream, so "it recorded" and "it
  // recorded points but no IMU" are different sentences in the log rather than
  // one number an operator has to interpret.
  FieldLog::info("capture", QString("event=record_stop empty_seal=%1 recorded_s=%2 "
                                    "chunks_written=%3 bytes_written=%4 %5")
                                .arg(empty_seal ? 1 : 0)
                                .arg(recorded_seconds_accum_, 0, 'f', 2)
                                .arg(cum_chunks_written_)
                                .arg(cum_bytes_written_)
                                .arg(fieldConfigKv()));
  if (!sealed_dir.isEmpty()) {
    const ProjectInfo pi = readProject(sealed_dir);
    FieldLog::info("capture",
                   QString("event=sealed dir=%1 valid=%2 manifest_ok=%3 sealed=%4 "
                           "total_chunks=%5 total_bytes=%6 duration_s=%7 "
                           "truncated_tail_chunks=%8 crc_mismatch_chunks=%9")
                       .arg(sealed_dir)
                       .arg(pi.valid ? 1 : 0)
                       .arg(pi.manifest_ok ? 1 : 0)
                       .arg(pi.sealed ? 1 : 0)
                       .arg(pi.total_chunks)
                       .arg(pi.total_bytes)
                       .arg(pi.duration_s, 0, 'f', 2)
                       .arg(pi.truncated_tail_chunks)
                       .arg(pi.crc_mismatch_chunks));
    for (const StreamInfo& st : pi.streams) {
      FieldLog::info("capture", QString("event=sealed_stream dir=%1 stream=%2 chunks=%3 "
                                        "bytes=%4 duration_s=%5")
                                    .arg(QFileInfo(sealed_dir).fileName(), st.name)
                                    .arg(st.chunks)
                                    .arg(st.bytes)
                                    .arg(st.duration_s(), 0, 'f', 2));
    }
  }

  recorded_seconds_accum_ = 0.0;
  if (record_cluster_) record_cluster_->setElapsedSeconds(0.0);
  name_edit_->clear();
  updateNameHint();
  Q_EMIT captureStopped(sealed_dir);
}

// --- round-5 field bug C: the post-restart data watch ----------------------
//
// See CaptureWindow.h's beginDataWatch() comment for the reproduction and the
// reasoning. This is deliberately a WATCH, not a gate: Start never waits for the
// device, because a recording that refuses to begin is worse than one that
// begins a moment before the data does. Record-always still holds.
void CaptureWindow::beginDataWatch(const QString& why) {
  if (!host_ || !host_->ok() || device_ == scanengine::kInvalidDeviceId) return;
  auto h = host_->engine()->device_health(device_);
  data_watch_baseline_points_ = h.ok() ? h.value().points_out : 0;
  data_watch_clock_.restart();
  data_watch_active_ = true;
  data_watch_why_ = why;
}

void CaptureWindow::endDataWatch() {
  data_watch_active_ = false;
  data_watch_window_s_ = 6.0;
  rearm_attempts_ = 0;
}

bool CaptureWindow::rearmDeviceInPlace(QString* err) {
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  // remove_device stops the driver and drops it; add_device rebuilds it and —
  // because a session is live — starts it immediately. The SESSION is never
  // stopped, so the recorder stays open and the .lscan being written is
  // untouched. This is the whole reason the fix re-arms the DEVICE rather than
  // restarting the session again.
  if (device_ != scanengine::kInvalidDeviceId) {
    QString rerr;
    (void)host_->removeDevice(device_, &rerr);
    device_ = scanengine::kInvalidDeviceId;
  }
  // A17: re-arm whichever lidar this session actually opened. The IMU device is
  // deliberately NOT rebuilt here — field bug C is about a lidar that goes
  // quiet across a session restart, the serial port is still open and still
  // pushing, and removing a working IMU to fix a silent lidar would break the
  // half that was fine.
  if (lidarModel() == LidarModel::kMid70) {
    if (!last_mid70_cfg_) {
      if (err) *err = "no Mid-70 config to re-arm with";
      return false;
    }
    device_ = host_->addMid70(*last_mid70_cfg_, err);
    return device_ != scanengine::kInvalidDeviceId;
  }
  if (!last_mid360_cfg_) {
    if (err) *err = "no Mid-360 config to re-arm with";
    return false;
  }
  device_ = host_->addMid360(*last_mid360_cfg_, err);
  return device_ != scanengine::kInvalidDeviceId;
}

void CaptureWindow::updateDataWatch() {
  if (!data_watch_active_) return;
  const bool live = phase_ == Phase::kPreview || phase_ == Phase::kRecording ||
                    phase_ == Phase::kPaused;
  if (!live || !host_ || !host_->ok() || device_ == scanengine::kInvalidDeviceId) {
    endDataWatch();
    return;
  }
  auto h = host_->engine()->device_health(device_);
  const std::uint64_t pts = h.ok() ? h.value().points_out : 0;
  if (pts > data_watch_baseline_points_) {
    const double t = data_watch_clock_.elapsed() / 1000.0;
    // Only worth a line when it actually took a moment; a millisecond resume is
    // the normal case and does not need saying.
    if (t > 0.5 || rearm_attempts_ > 0) {
      log(QString("sensor data resumed %1 s after %2%3")
              .arg(t, 0, 'f', 2)
              .arg(data_watch_why_)
              .arg(rearm_attempts_ > 0 ? QString(" (after %1 re-arm(s))").arg(rearm_attempts_)
                                       : QString()));
    }
    if (arm_label_) {
      arm_label_->setStyleSheet(
          QString("color:%1;font-weight:600;").arg(theme::css(theme::good())));
      arm_label_->setText(phase_ == Phase::kRecording ? "Recording — sensor data flowing."
                                                      : "Live — sensor data flowing.");
    }
    endDataWatch();
    return;
  }

  const double elapsed = data_watch_clock_.elapsed() / 1000.0;
  if (arm_label_) {
    arm_label_->setStyleSheet(QString("color:%1;font-weight:600;").arg(theme::css(theme::bad())));
    arm_label_->setText(QString("NO SENSOR DATA since %1 (%2 s) — %3")
                            .arg(data_watch_why_)
                            .arg(elapsed, 0, 'f', 1)
                            .arg(phase_ == Phase::kRecording
                                     ? "this recording is EMPTY so far; re-arming the sensor"
                                     : "re-arming the sensor"));
  }
  if (elapsed < data_watch_window_s_) return;

  ++rearm_attempts_;
  QString err;
  if (rearmDeviceInPlace(&err)) {
    log(QString("no sensor data for %1 s after %2 — re-armed the Mid-360 in place "
                "(attempt %3); the recording stayed open")
            .arg(elapsed, 0, 'f', 1)
            .arg(data_watch_why_)
            .arg(rearm_attempts_));
  } else {
    log(QString("no sensor data for %1 s after %2 — re-arm attempt %3 failed: %4")
            .arg(elapsed, 0, 'f', 1)
            .arg(data_watch_why_)
            .arg(rearm_attempts_)
            .arg(err));
  }
  // Back off so a device that is genuinely unplugged is not hammered, but never
  // give up: an operator who plugs the cable back in must be picked up.
  data_watch_window_s_ = std::min(data_watch_window_s_ * 2.0, 24.0);
  data_watch_clock_.restart();
  auto h2 = host_->engine()->device_health(device_);
  data_watch_baseline_points_ = h2.ok() ? h2.value().points_out : 0;
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

// A17 — the Mid-70's health row, read from Engine::mid70_stats().
//
// EVERY NUMBER HERE IS THE DEVICE'S OWN ACCOUNT OF ITSELF, not an inference.
// The Mid-70 puts an err_code bitfield and a timestamp_type in the header of
// EVERY datagram (mid70_packets.h), so "is the clock disciplined" and "is PPS
// present" are facts the sensor states rather than things this panel guesses
// from timing. That is exactly why they are worth the width: on a rig being
// prepared for PPS+GPS they are the only way to see the sync come up, and a
// wrong guess about them would be worse than no row at all.
//
// `loss_pct_window` is THIS health window's, `packets_lost` the session total.
// Both are inferred by the driver from device-timestamp gaps, because SDK v1
// datagrams carry no sequence counter — the driver's header says so, and the
// word "est" here says it to the operator too.
QString CaptureWindow::mid70HealthText(const scanengine::DeviceHealth& d) const {
  QString line = QString("%1 · %2 pts/s · %3 pts / %4 in · %5 drops")
                     .arg(scanengine::to_string(d.state))
                     .arg(d.points_per_sec, 0, 'f', 0)
                     .arg(d.points_out)
                     .arg(humanBytesLocal(d.bytes_in))
                     .arg(d.drops);
  if (!host_ || !host_->ok() || device_ == scanengine::kInvalidDeviceId) return line;

  auto r = host_->engine()->mid70_stats(device_);
  if (!r.ok()) {
    // kInvalidArgument here means the armed device is not a Mid-70 at all,
    // which would be a bug in this panel's own model bookkeeping. Say which,
    // rather than quietly dropping half the row.
    return line + QString(" · no Mid-70 stats (%1)").arg(scanengine::error_str(r.error()));
  }
  const scanengine::Mid70Stats& m = r.value();

  line += QString(" · link %1").arg(scanengine::to_string(m.link));
  line += QString(" · loss %1% window, %2 pkts est lost")
              .arg(m.loss_pct_window, 0, 'f', 2)
              .arg(m.packets_lost);

  // PPS and time sync. `pps_ok` is a bool the device sets; `time_sync_status`
  // is an enum whose own to_string lives beside the decoder, so the words come
  // from the engine rather than from a second table here that could drift.
  // Both are only meaningful once a datagram has actually been decoded.
  if (m.device_stamp_decodable || m.point_packets > 0) {
    line += QString(" · PPS: %1").arg(m.err.pps_ok ? "locked" : "none");
    line += QString(" · sync: %1")
                .arg(QString::fromUtf8(scanengine::mid70::to_string_time_sync_status(
                    m.err.time_sync_status)));
    line += QString(" · stamps: %1")
                .arg(QString::fromUtf8(
                    scanengine::mid70::to_string_timestamp_type(m.timestamp_type)));
  } else {
    line += " · PPS/sync: no datagram decoded yet";
  }

  // The device's own "I am cold" bit. This is the ONE thing that distinguishes
  // "warming up, leave it alone" from "not talking to me", and it is why the
  // arm window is three minutes rather than eight seconds.
  if (m.err.self_heating) line += " · SELF-HEATING (cold start, streaming will follow)";
  // The remaining err_code fields, surfaced only when they are NOT normal: a
  // health row that lists four "normal"s teaches an operator to stop reading it.
  if (m.err.temp_status) line += QString(" · TEMP %1").arg(m.err.temp_status);
  if (m.err.volt_status) line += QString(" · VOLT %1").arg(m.err.volt_status);
  if (m.err.motor_status) line += QString(" · MOTOR %1").arg(m.err.motor_status);
  if (m.err.dirty_warn) line += " · WINDOW DIRTY/BLOCKED";
  if (m.err.fan_warn) line += " · FAN";
  if (m.err.firmware_err) line += " · FIRMWARE ERROR";

  // Identity, once the SDK has reported it. Empty before the handshake
  // completes, and left out rather than shown blank.
  if (!m.broadcast_code.empty()) {
    line += QString(" · %1").arg(QString::fromStdString(m.broadcast_code));
  }
  if (!m.firmware.empty()) {
    line += QString(" fw %1").arg(QString::fromStdString(m.firmware));
  }
  if (m.forced_reinits > 0) line += QString(" · %1 SDK re-init(s)").arg(m.forced_reinits);
  return line;
}

// A18 — the IMU's tail of the same row, read from Engine::imu_serial_stats().
// Empty when no IMU device is armed, so a Mid-70 running without one does not
// grow a row of blanks.
//
// THE BLACKOUT COUNTER IS THE POINT OF THIS ROW. The module stops transmitting
// for ~2.9 s roughly every 35 s — measured vendor-firmware behaviour that
// nothing in its protocol turns off (imu_serial_driver.h). The driver counts
// those and refuses to smooth them, because a driver that hid them would hand
// LIO three seconds of invented motion. So the count is shown always, not only
// when it is non-zero: an operator who sees it climbing is seeing the sensor
// behave as documented, and an operator who sees it stuck at 0 after a minute
// is looking at something that is not this module.
QString CaptureWindow::imuHealthText() const {
  if (imu_device_ == scanengine::kInvalidDeviceId || !host_ || !host_->ok()) return QString();
  auto h = host_->engine()->device_health(imu_device_);
  if (!h.ok()) return QString("  |  IMU: no health");
  const auto& d = h.value();

  QString line = QString("  |  IMU %1").arg(scanengine::to_string(d.state));

  auto r = host_->engine()->imu_serial_stats(imu_device_);
  if (!r.ok()) {
    return line + QString(" · no IMU stats (%1)").arg(scanengine::error_str(r.error()));
  }
  const scanengine::ImuSerialStats& s = r.value();

  line += QString(" · %1 Hz").arg(s.rate_hz, 0, 'f', 1);
  // frames.checksum_pass_rate() counts EVERY func, not just the 0x04 raw ones
  // LIO consumes: the module emits four frame types unprompted and cannot be
  // told not to, so a rate computed over raw frames alone would report a
  // healthy link as 25% good.
  line += QString(" · %1 raw / %2 frames, %3% ok")
              .arg(s.frames.raw_frames)
              .arg(s.frames.frames_seen())
              .arg(s.frames.checksum_pass_rate() * 100.0, 0, 'f', 1);

  if (s.blackout_in_progress) {
    line += QString(" · BLACKOUT NOW (%1 so far)").arg(s.blackouts);
  } else {
    line += QString(" · %1 blackout(s)").arg(s.blackouts);
  }
  if (s.worst_blackout_ns > 0) {
    line += QString(", worst %1 s").arg(double(s.worst_blackout_ns) / 1e9, 0, 'f', 2);
  }
  // The stamper's own backstop. `clamps` must be zero: a non-zero value means
  // the de-burst model produced a non-monotonic stamp and the backstop had to
  // rescue it, which is a fact about the model, not about the module.
  if (s.stamper.clamps > 0) line += QString(" · %1 STAMP CLAMPS").arg(s.stamper.clamps);
  if (s.samples_dropped > 0) line += QString(" · %1 dropped").arg(s.samples_dropped);
  if (imu_reader_ && !imu_reader_->isOpen()) line += " · PORT CLOSED";
  return line;
}

void CaptureWindow::updateHealth() {
  if (!host_) return;
  if (phase_ == Phase::kArming) evaluateArming();
  updateDataWatch();
  updateLiveWindowNote();

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
      if (lidarModel() == LidarModel::kMid70) {
        health_->setText(mid70HealthText(d) + flag + imuHealthText());
        return;
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

// FIELD BUG D, the honest half. The engine's live window is bounded, so on a
// long scan the map on screen stops being "everything scanned" and becomes "the
// most recent N points". That is a fact about the VIEW, not about the capture,
// and the operator has to be told once, quietly, in the panel — never in a
// dialog, and never once per revolution the way the engine used to warn.
void CaptureWindow::updateLiveWindowNote() {
  if (!live_window_note_ || !host_ || !host_->ok()) return;
  const bool live = phase_ == Phase::kPreview || phase_ == Phase::kRecording ||
                    phase_ == Phase::kPaused;
  const scanengine::PageStoreStats st = host_->pageStats();
  if (!live || !st.evicting) {
    if (!live && live_window_note_->isVisible()) live_window_note_->setVisible(false);
    return;
  }
  live_window_note_->setText(
      QString("Live map is showing the most recent %1 points (%2 of %3 pages) — the "
              "oldest points leave the view so it can keep up. The recording has "
              "every point; post-processing rebuilds the whole cloud.")
          .arg(groupedCount(st.resident_points))
          .arg(st.pages)
          .arg(st.max_pages));
  live_window_note_->setVisible(true);
  if (!live_window_seen_evicting_) {
    live_window_seen_evicting_ = true;
    log(QString("live map reached its %1-page ceiling: it now shows the most recent %2 "
                "points and keeps advancing. Recording is unaffected.")
            .arg(st.max_pages)
            .arg(groupedCount(st.resident_points)));
    Q_EMIT liveWindowEvicting(st.resident_points, st.evicted_points);
  }
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
                     static_cast<QWidget*>(cmd_port_),
                     // A17/A18: same rule, same reason. Changing the lidar
                     // MODEL or the IMU port under a live device would leave
                     // the panel describing a session it is not running.
                     static_cast<QWidget*>(lidar_model_),
                     static_cast<QWidget*>(imu_serial_port_)}) {
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
  // A17: pin the model before touching the fields. This hook is what CI and
  // every field evidence run drive, and it must arm a Mid-360 whatever the GUI
  // combo (or a discovery hit earlier in the same run) last left it on.
  setLidarModel(LidarModel::kMid360);
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

bool CaptureWindow::setLidarModelForCli(const QString& name) {
  const QString n = name.trimmed().toLower();
  if (n == "mid360" || n == "mid-360") {
    setLidarModel(LidarModel::kMid360);
  } else if (n == "mid70" || n == "mid-70") {
    setLidarModel(LidarModel::kMid70);
  } else {
    return false;
  }
  // Report what the panel now LOOKS like, not just what was asked for. The
  // capture dock is short and scrolls, so an evidence screenshot cannot show
  // the IMU row at the bottom of the link column; this line is the record that
  // it is there (or correctly gone) and how many ports it offers.
  log(QString("lidar model set to %1 (CLI) -> SDK2 port row %2, broadcast code row %3, "
              "IMU row %4 (%5 serial port(s) offered, selected \"%6\")")
          .arg(lidar_model_->currentText())
          // isHidden(), not isVisible()/isVisibleTo(): the first two are false
          // for anything inside the COLLAPSED "Manual setup" box regardless of
          // the model, which would report every row as hidden and prove
          // nothing. isHidden() asks the only question this line is about —
          // did the model choice hide this row.
          .arg(mid360_ports_row_ && !mid360_ports_row_->isHidden() ? "shown" : "hidden")
          .arg(mid70_code_row_ && !mid70_code_row_->isHidden() ? "shown" : "hidden")
          .arg(imu_row_ && !imu_row_->isHidden() ? "shown" : "hidden")
          .arg(imu_serial_port_ ? imu_serial_port_->count() - 1 : 0)
          .arg(selectedImuPort().isEmpty() ? QStringLiteral("(none)") : selectedImuPort()));
  return true;
}

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
      "Runs by itself when this panel opens. Listens for a Mid-360 heartbeat (UDP 56201) "
      "and then for a Mid-70 broadcast (UDP 55000), then sweeps serial ports for the "
      "JuxiTech IMU module and a UM982 (and a COIN-D6, which desktop capture does not "
      "use — see the D6 line below). A lidar hit arms the live preview automatically.");
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

  // A17: directly under the Mid-360 line, because the two are alternatives and
  // an operator reading top to bottom is choosing between them.
  auto_detect_mid70_line_ = new QLabel();
  auto_detect_mid70_line_->setWordWrap(true);
  pv->addWidget(auto_detect_mid70_line_);

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

  // A18: after the D6 and before the UM982, mirroring the probe order the
  // engine's own ordering contract puts them in.
  auto_detect_juxi_line_ = new QLabel();
  auto_detect_juxi_line_->setWordWrap(true);
  pv->addWidget(auto_detect_juxi_line_);

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

  // WHICH PORT, honestly. A pass holds 56201 (the Mid-360 heartbeat) and 55000
  // (the Mid-70 broadcast) in turn, and the vendored SDK binds whichever one
  // the model about to be armed needs — SDK v1's Start() now names 55000 in
  // its own bind error. One gate covers both listens; this line just says which
  // one the operator is about to care about.
  const QString port =
      lidarModel() == LidarModel::kMid70 ? QStringLiteral("55000") : QStringLiteral("56201");
  const QString msg =
      released
          ? QString("auto-detect canceled so %1 can have UDP %2 — port released")
                .arg(what, port)
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
  // 3 s of Mid-360 heartbeat (~1 Hz), then 2 s of Mid-70 broadcast (also ~1 Hz,
  // so two windows), then 700 ms per enumerated serial port. Each UDP listen
  // returns EARLY on its first hit (stop_after_devices = 1), so the common
  // "the lidar is right there" case costs a fraction of that; the numbers are
  // the WORST case, which is what an operator staring at the bar experiences
  // when nothing is plugged in.
  auto* worker = new DiscoveryWorker(3000, 2000, 700);
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
  applyMid70Result(r, silent);
  applyD6Result(r);
  applyJuxiImuResult(r, silent);
  applyUm982Result(r, silent);
  if (auto_detect_panel_) auto_detect_panel_->setVisible(true);

  log(QString("auto-detect%1: Mid-360 %2, Mid-70 %3, D6 %4 (phone-only), IMU %5, UM982 %6")
          .arg(silent ? " (on open)" : "")
          .arg(r.mid360.found ? "found" : "not seen")
          .arg(r.mid70.found ? "found" : "not seen")
          .arg(r.d6.found ? "detected" : "not seen")
          .arg(r.juxi_imu.found ? "found" : "not seen")
          .arg(r.um982.found ? "found" : "not seen"));
  // The SIGNATURE of this signal is deliberately unchanged: main.cpp's
  // --auto-detect-selftest chain binds to it, and the Mid-70/IMU results are
  // reported in the log line above and in the panel rather than by widening a
  // contract three CLI hooks depend on.
  Q_EMIT autoDetectFinished(r.mid360.found, r.d6.found, r.um982.found);

  // Round-5 follow-up item 1: nothing found -> the inline manual row opens by
  // itself, with a sentence saying why, so the operator has somewhere to type
  // instead of a dead end. (It stays reachable from "Manual setup" when
  // detection DID succeed — that toggle is never hidden.)
  if (!r.mid360.found && !r.mid70.found) {
    setManualSetupOpen(true);
    setAutoDetectStatus(
        "No lidar answered — neither a Mid-360 heartbeat on UDP 56201 nor a Mid-70 "
        "broadcast on UDP 55000. Pick the model, type the lidar IP (and the host IP this "
        "Mac holds) in Manual setup, then Connect — or fix the link and run Auto-detect "
        "again.",
        "warn");
  }

  // Round 5 item 10: a device that answered goes straight to live preview. No
  // button, no gate — the points on screen are the proof it works. A CLI hook
  // that arms the device itself opts out (suppressAutoArmForCli).
  // A17: a Mid-70 hit auto-arms exactly as a Mid-360 hit does. When BOTH answer,
  // the Mid-360 wins and applyMid70Result() says so rather than silently
  // switching the model out from under an operator — which also means that on
  // a bench with no Mid-70, every line of this is bit-for-bit the old decision.
  const bool want_arm = rearm_after_discovery_ || r.mid360.found || r.mid70.found;
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

// A17. The Mid-70 half of a detect pass. Two things make this NOT a copy of
// applyMid360Result():
//
//  * A MID-360 HIT WINS. If both answered, the model stays on Mid-360 and this
//    reports the Mid-70 as present-but-not-selected. Auto-switching would mean
//    a detect pass could change which driver Connect opens without anybody
//    asking, and on a bench where both are powered that is a coin toss. It also
//    keeps the Mid-360 flow provably untouched: with no Mid-70 broadcasting,
//    `m.found` is false and nothing below runs at all.
//
//  * THERE IS NO PERSISTED HOST TO RECONCILE. SDK v1 does not store a host
//    address on the lidar; the host is named in the handshake every time. So
//    the only host question is the one that actually bites in the field — does
//    this Mac hold an address on the lidar's network — and the answer comes
//    from the same CheckHostReachability() the Mid-360 path uses (see
//    DeviceDiscovery.cpp for why that is honest for an SDK v1 beacon).
void CaptureWindow::applyMid70Result(const DiscoveryResult& r, bool silent) {
  const auto& m = r.mid70;
  if (!auto_detect_mid70_line_) return;

  if (!m.found) {
    const QString why =
        r.mid70_error.isEmpty() ? QStringLiteral("no broadcast heard") : r.mid70_error;
    auto_detect_mid70_line_->setText(
        QString("Mid-70: not seen (%1). A Mid-70 broadcasts on UDP 55000 about once a "
                "second until something connects to it — so this also looks exactly like "
                "\"a viewer is already streaming from it\".")
            .arg(why));
    auto_detect_mid70_line_->setProperty("tone", "warn");
    repolish(auto_detect_mid70_line_);
    return;
  }

  // dev_type 6 is a Mid-70; the parser deliberately does not reject the others,
  // so a Horizon on the same switch shows up here as itself rather than as a
  // dropped datagram. Arming a non-Mid-70 through this driver is not something
  // this build has any evidence for, so it says so instead of pretending.
  const bool is_mid70 = m.dev_type == 6;
  auto_detect_mid70_line_->setText(
      QString("Found %1 %2 at %3%4.")
          .arg(m.dev_type_name.isEmpty() ? QStringLiteral("Livox SDK-v1 device")
                                         : m.dev_type_name)
          .arg(m.broadcast_code)
          .arg(m.lidar_ip)
          .arg(is_mid70 ? QString()
                        : QString(" — NOT a Mid-70 (dev_type %1); this build has only been "
                                  "written against the Mid-70")
                              .arg(m.dev_type)));
  auto_detect_mid70_line_->setProperty("tone", is_mid70 ? "good" : "warn");
  repolish(auto_detect_mid70_line_);

  if (r.mid360.found) {
    log(QString("auto-detect: a Mid-70 (%1 at %2) is also broadcasting, but a Mid-360 "
                "answered too and stays selected — switch the lidar model by hand to use "
                "the Mid-70")
            .arg(m.broadcast_code, m.lidar_ip));
    return;
  }
  if (!is_mid70) return;  // do not switch the panel onto a device we cannot vouch for

  // Selecting the model reloads the "mid70/last" fields (the combo's own
  // handler), so the discovered values are written AFTER it, not before.
  setLidarModel(LidarModel::kMid70);
  mid70_code_->setText(m.broadcast_code);

  // Same prefill guard as the Mid-360 path: a silent (on-open) pass leaves a
  // field the operator has already typed into alone; a clicked pass always
  // fills in what it found, because that is the point of clicking it.
  const bool lidar_ip_is_default = lidar_ip_->text().trimmed() == "192.168.1.100";
  const bool host_ip_is_default = host_ip_->text().trimmed() == "192.168.1.5";
  if (!silent || lidar_ip_is_default) lidar_ip_->setText(m.lidar_ip);

  auto_detect_fix_line_->setVisible(false);
  auto_detect_copy_btn_->setVisible(false);
  auto_detect_copy_payload_.clear();
  if (m.on_lidar_subnet && !m.suggested_host_ip.isEmpty()) {
    // This Mac already holds an address the lidar can reach. Nothing to fix.
    if (!silent || host_ip_is_default) host_ip_->setText(m.suggested_host_ip);
  } else {
    // The field failure, exactly: the lidar is reachable enough to broadcast at
    // us but we hold no address on its network, so anything we tell it to
    // stream to is unroutable. Offer the same copyable one-liner the Mid-360
    // path does.
    const QString iface =
        m.suggested_interface.isEmpty() ? QStringLiteral("<if>") : m.suggested_interface;
    const QString example = m.suggested_host_ip.isEmpty()
                                ? QStringLiteral("<host ip on the lidar's subnet>")
                                : m.suggested_host_ip;
    auto_detect_copy_payload_ =
        QString("sudo ifconfig %1 alias %2 255.255.255.0").arg(iface, example);
    auto_detect_fix_line_->setText(
        m.host_check_note.isEmpty()
            ? QString("this Mac has no address on the Mid-70's network — e.g. `%1`")
                  .arg(auto_detect_copy_payload_)
            : m.host_check_note);
    auto_detect_fix_line_->setToolTip(m.host_check_note);
    auto_detect_fix_line_->setProperty("tone", "bad");
    repolish(auto_detect_fix_line_);
    auto_detect_fix_line_->setVisible(true);
    auto_detect_copy_btn_->setVisible(!m.suggested_interface.isEmpty());
  }
}

// A18. The serial IMU probe's hit. Unlike the D6 line (information only) and
// the UM982 line (nowhere to connect it), this one DOES drive a control: the
// IMU port combo a Mid-70 arm reads. It is still only a prefill — "(none)"
// stays a legitimate answer and the operator can pick it.
void CaptureWindow::applyJuxiImuResult(const DiscoveryResult& r, bool silent) {
  const auto& j = r.juxi_imu;
  if (!auto_detect_juxi_line_) return;

  if (!j.found) {
    auto_detect_juxi_line_->setText(
        "Serial IMU: not seen. Only a Mid-70 session needs one (a Mid-360 has its own), "
        "so this is not a problem unless you are using a Mid-70.");
    auto_detect_juxi_line_->setProperty("tone", "");
    repolish(auto_detect_juxi_line_);
    return;
  }

  // frame_rate_hz counts func-0x04 RAW frames only, i.e. the IMU SAMPLE rate.
  // The module ships at 25 Hz and this app's driver asks for 100 Hz once at
  // start(); the probe never writes, so a 25 Hz reading here means "not yet
  // configured", not "broken".
  auto_detect_juxi_line_->setText(
      QString("Serial IMU: found on %1 @ 115200 (%2 frames, %3 raw%4).")
          .arg(j.port)
          .arg(j.frames_seen)
          .arg(j.raw_frames)
          .arg(j.frame_rate_hz > 0.0
                   ? QString(", %1 Hz now — the driver asks for 100 Hz on connect")
                         .arg(j.frame_rate_hz, 0, 'f', 1)
                   : QString(", rate not measurable in this window")));
  auto_detect_juxi_line_->setProperty("tone", "good");
  repolish(auto_detect_juxi_line_);

  if (!imu_serial_port_) return;
  const bool unselected = selectedImuPort().isEmpty();
  if (!silent || unselected) {
    refreshImuPortList();  // the adapter may have appeared since the panel opened
    const int idx = imu_serial_port_->findData(j.port);
    if (idx >= 0) {
      imu_serial_port_->setCurrentIndex(idx);
    } else {
      // The engine's probe enumerated a port QSerialPortInfo did not. Trust the
      // probe: it just read valid checksummed frames off it.
      imu_serial_port_->addItem(j.port, j.port);
      imu_serial_port_->setCurrentIndex(imu_serial_port_->count() - 1);
    }
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

QString CaptureWindow::fieldConfigKv() const {
  const bool mid70 = lidarModel() == LidarModel::kMid70;
  QStringList f;
  auto add = [&f](const char* k, const QString& v) {
    f << QString("%1=%2").arg(QString::fromUtf8(k), v.isEmpty() ? QStringLiteral("-") : v);
  };
  add("model", mid70 ? "mid70" : "mid360");
  add("host_ip", host_ip_ ? host_ip_->text().trimmed() : QString());
  add("lidar_ip", lidar_ip_ ? lidar_ip_->text().trimmed() : QString());
  add("broadcast_code", mid70 && mid70_code_ ? mid70_code_->text().trimmed() : QString());
  if (!mid70) {
    // The Mid-360's three UDP ports are its equivalent of the Mid-70's
    // broadcast code: the thing that decides whether the link comes up.
    f << QString("point_port=%1").arg(point_port_ ? point_port_->value() : 0);
    f << QString("mid360_imu_port=%1").arg(imu_port_ ? imu_port_->value() : 0);
    f << QString("cmd_port=%1").arg(cmd_port_ ? cmd_port_->value() : 0);
  }
  add("imu_serial_port", mid70 ? selectedImuPort() : QString());
  f << QString("imu_serial_baud=%1").arg(mid70 && !selectedImuPort().isEmpty() ? 115200 : 0);
  f << QString("near_gate_m=%1").arg(double(lioNearGateForModel()), 0, 'f', 2);
  add("profile", profile_ ? profile_->currentText() : QString());
  add("project_dir", last_project_dir_);
  f << QString("lidar_device=%1").arg(device_);
  f << QString("imu_device=%1").arg(imu_device_);
  return f.join(' ');
}

void CaptureWindow::log(const QString& s) {
  // The FIELD LOG gets every one of these, verbatim and unfiltered — the log
  // pane below scrolls away and the stderr stream does not exist on a
  // double-clicked .app, so this file is the only copy that survives the run.
  FieldLog::info("capture", s);
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
