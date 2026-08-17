#include "app/CaptureWindow.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyle>
#include <QSysInfo>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "app/DeviceDiscovery.h"
#include "app/EngineHost.h"
#include "ui/RecordCluster.h"
#include "ui/Theme.h"

namespace lidarscan {
namespace {

// WCH's CH340/CH341 USB-serial chip — the vendor's own D6 adapter, and the
// one every per-OS driver-guidance string in this file already names.
constexpr quint16 kCh340VendorId = 0x1A86;

// The self-test verdict used two hard-coded Material greens/reds
// (#2e7d32 / #c62828) that predate the palette; they are now the semantic
// good/bad tokens, so the verdict matches every other pass/fail in the app.
QString kPassStyle() {
  return QString("color:%1;font-weight:600;").arg(theme::css(theme::good()));
}
QString kFailStyle() {
  return QString("color:%1;font-weight:600;").arg(theme::css(theme::bad()));
}

// A dynamic property change (tone=good/warn/bad, the ember accent, …) needs
// an explicit re-polish once the widget has already been shown once — QSS
// property selectors are matched at polish time (RecordCluster.cpp already
// established this pattern for the badge/accent properties this file reuses).
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

}  // namespace

CaptureWindow::CaptureWindow(EngineHost* host, QWidget* parent)
    : QDialog(parent), host_(host) {
  setWindowTitle("Capture");
  setModal(false);
  buildUi();
  refreshPorts();
  setPhase(Phase::kIdle);

  health_timer_ = new QTimer(this);
  connect(health_timer_, &QTimer::timeout, this, &CaptureWindow::updateHealth);
  health_timer_->start(300);

  port_refresh_timer_ = new QTimer(this);
  connect(port_refresh_timer_, &QTimer::timeout, this, [this] {
    if (phase_ == Phase::kIdle && tabs_->currentIndex() == 0) refreshPorts();
  });
  port_refresh_timer_->start(2000);
}

CaptureWindow::~CaptureWindow() {
  if (phase_ != Phase::kIdle) onStop();
}

void CaptureWindow::setProjectDir(const QString& dir) {
  project_dir_ = dir;
  if (project_edit_) project_edit_->setText(dir);
  // A different project may have different "never configured" status; let
  // showEvent() re-decide whether the silent auto-run applies to it.
  auto_detect_prompted_for_project_ = false;
  loadMid360Settings();
}

void CaptureWindow::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  // Silent, once per project, and only when nothing has ever been saved for
  // it — see had_saved_mid360_settings_'s comment in the header. Never fires
  // mid-capture (phase_ != kIdle) or while a pass is already running.
  if (!auto_detect_prompted_for_project_ && phase_ == Phase::kIdle &&
      !had_saved_mid360_settings_ && !discovery_in_flight_) {
    auto_detect_prompted_for_project_ = true;
    startDiscovery(/*silent=*/true);
  }
}

void CaptureWindow::buildUi() {
  auto* v = new QVBoxLayout(this);
  tabs_ = new QTabWidget();

  // --- D6 tab ---
  {
    auto* w = new QWidget();
    auto* f = new QFormLayout(w);
    port_ = new QComboBox();
    connect(port_, &QComboBox::currentIndexChanged, this, [this](int) { applyCh340Hint(); });
    auto* refresh = new QPushButton("Refresh");
    connect(refresh, &QPushButton::clicked, this, &CaptureWindow::refreshPorts);
    auto* row = new QWidget();
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(port_, 1);
    rl->addWidget(refresh);
    f->addRow("Serial port (auto-refreshes every 2 s while idle)", row);

    ch340_hint_ = new QLabel();
    ch340_hint_->setWordWrap(true);
    ch340_hint_->setStyleSheet(QString("color:%1;").arg(theme::css(theme::good())));
    f->addRow(ch340_hint_);

    d6_auto_tag_ = new QLabel("auto-detected");
    d6_auto_tag_->setProperty("badge", true);
    d6_auto_tag_->setProperty("tone", "ember");
    d6_auto_tag_->setVisible(false);
    f->addRow(d6_auto_tag_);

    baud_ = new QSpinBox();
    baud_->setRange(9600, 3000000);
    baud_->setValue(230400);  // Tech Spec §2.1
    baud_->setSingleStep(9600);
    f->addRow("Baud (8N1)", baud_);

    send_commands_ = new QCheckBox("Send start/stop command frames (AA 55 F0 0F / AA 55 F5 0A)");
    send_commands_->setChecked(true);
    f->addRow(send_commands_);

    port_hint_ = new QLabel();
    port_hint_->setWordWrap(true);
    port_hint_->setTextFormat(Qt::PlainText);
#if defined(Q_OS_MACOS)
    port_hint_->setText(
        "macOS: the CH340 driver is in-box on modern releases; the adapter shows "
        "up as /dev/cu.usbserial-* or /dev/cu.wchusbserial-*. Choose the cu.* "
        "node, not tty.*, or open() blocks on carrier detect.");
#elif defined(Q_OS_WIN)
    port_hint_->setText(
        "Windows: install the CH340 driver shipped in the vendor kit "
        "(english coind6/4 Windows Host Software/CH340 Driver/) before the "
        "adapter appears as a COM port. C8 owns the installer-bundled driver "
        "pointer; this dialog can only tell you it is needed.");
#else
    port_hint_->setText(
        "Linux: the adapter appears as /dev/ttyUSB*. Membership of the dialout "
        "group (or the udev rule the .deb installs) is required to open it.");
#endif
    f->addRow(port_hint_);
    tabs_->addTab(w, "COIN-D6 (serial)");
  }

  // --- Mid-360 tab ---
  {
    auto* w = new QWidget();
    auto* f = new QFormLayout(w);
    host_ip_ = new QLineEdit("192.168.1.5");
    lidar_ip_ = new QLineEdit("192.168.1.100");
    f->addRow("Host IP (static)", host_ip_);
    f->addRow("Lidar IP", lidar_ip_);
    point_port_ = new QSpinBox();
    point_port_->setRange(1, 65535);
    point_port_->setValue(56300);
    imu_port_ = new QSpinBox();
    imu_port_->setRange(1, 65535);
    imu_port_->setValue(56400);
    cmd_port_ = new QSpinBox();
    cmd_port_->setRange(1, 65535);
    cmd_port_->setValue(56100);
    f->addRow("Point port", point_port_);
    f->addRow("IMU port", imu_port_);
    f->addRow("Command port", cmd_port_);
    mid_hint_ = new QLabel(
        "The lidar IP is REQUIRED on macOS: stock SDK2 broadcast discovery fails "
        "with EADDRNOTAVAIL there (S2-sim finding), so the engine's vendored SDK2 "
        "needs an explicit address. Lidar on 192.168.1.1xx, host static on "
        "192.168.1.x. Power is 9-27 V / ~6.5 W — an external battery is required. "
        "Settings below persist per project.");
    mid_hint_->setWordWrap(true);
    f->addRow(mid_hint_);
    tabs_->addTab(w, "Livox Mid-360 (Ethernet)");
  }

  // --- RTK / UM982 tab ---
  //
  // Auto-detect prefill only (see the field comment on um982_port_ in the
  // header): there is no engine-side wiring on this desktop capture path yet
  // that opens a GNSS serial port the way openDeviceForTab() opens D6/Mid-360
  // — GnssSource (engine/include/scanengine/gnss/gnss_source.h) is fed NMEA
  // bytes already in hand, it does not own a port. This tab exists so
  // ProbeSerialUm982()'s port+baud+has_heading hit (docs/design/
  // REVIEW_FEEDBACK.md round 4 item 5: "UM982 via port+baud sweep") has
  // somewhere honest to land instead of being silently dropped, and so a
  // manual auto-detect run confirms what the field session already found by
  // hand (captures/FIELD_SESSION_2026-08-17.md: /dev/cu.usbserial-21140 @
  // 230400, GPTHS present -> dual-antenna heading enabled).
  {
    auto* w = new QWidget();
    auto* f = new QFormLayout(w);
    um982_port_ = new QComboBox();
    um982_port_->setEditable(true);  // the port may not be enumerated yet (probe result only)
    f->addRow("Serial port", um982_port_);
    um982_baud_ = new QSpinBox();
    um982_baud_->setRange(4800, 921600);
    um982_baud_->setValue(115200);  // Unicore factory default; the field unit needed 230400
    f->addRow("Baud", um982_baud_);
    um982_heading_ = new QLabel("Dual-antenna heading: unknown (run Auto-detect)");
    um982_heading_->setWordWrap(true);
    f->addRow(um982_heading_);
    um982_hint_ = new QLabel(
        "Not yet wired into Record/Test below — this tab only holds what "
        "Auto-detect finds (port, baud, whether the firmware reports a dual-"
        "antenna heading solution) so it is not lost. See NOTES.md.");
    um982_hint_->setWordWrap(true);
    um982_hint_->setProperty("role", "hint");
    f->addRow(um982_hint_);
    tabs_->addTab(w, "RTK (UM982)");
  }

  // Prominent, above every per-sensor tab (owner, field session: "Manual IP
  // entry defeated the GUI on first contact" — this is the fix).
  buildAutoDetectSection(v);

  v->addWidget(tabs_);

  // --- session ---
  {
    auto* g = new QGroupBox("Session");
    auto* f = new QFormLayout(g);
    project_edit_ = new QLineEdit();
    auto* browse = new QPushButton("…");
    browse->setFixedWidth(32);
    connect(browse, &QPushButton::clicked, this, [this] {
      const QString d = QFileDialog::getSaveFileName(this, "New .lscan project", project_dir_,
                                                     "LidarScan project (*.lscan)");
      if (!d.isEmpty()) project_edit_->setText(d.endsWith(".lscan") ? d : d + ".lscan");
    });
    auto* row = new QWidget();
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(project_edit_, 1);
    rl->addWidget(browse);
    f->addRow(".lscan project", row);

    profile_ = new QComboBox();
    profile_->addItems({"quickscan", "survey", "floorplan", "research"});
    f->addRow("Profile", profile_);

    live_mode_ = new QCheckBox("Live (render while capturing)");
    live_mode_->setChecked(true);
    live_mode_->setToolTip(
        "Unchecked = Record-only: the engine still records every raw byte "
        "(record-always, §3 key rule 2) but the viewport stops mirroring the "
        "PageStore, so the GPU and the UI thread stay out of the capture's way. "
        "MainWindow reads this via project()/viewport wiring at capture start.");
    f->addRow(live_mode_);

    // The four transport buttons that used to live HERE — four equal 12 px
    // pills in the middle of this form, which is exactly the "start-record /
    // end-record buttons missing" the owner reported — are gone. They are now
    // the record cluster pinned across the foot of this window; see below and
    // ui/RecordCluster.h. This note stays in the UI too, so the next person
    // looking for Record in the Session card is told where it went.
    auto* moved = new QLabel(
        "Transport, session and self-test are configured here; <b>Test device · "
        "Start · Pause · Stop</b> live in the record cluster at the foot of this "
        "window.");
    moved->setWordWrap(true);
    moved->setProperty("role", "hint");
    f->addRow(moved);

    self_test_label_ = new QLabel();
    self_test_label_->setWordWrap(true);
    f->addRow(self_test_label_);
    self_test_progress_ = new QProgressBar();
    self_test_progress_->setRange(0, 100);
    self_test_progress_->setVisible(false);
    f->addRow(self_test_progress_);

    v->addWidget(g);
  }

  health_ = new QLabel("idle");
  health_->setWordWrap(true);
  v->addWidget(health_);

  summary_ = new QLabel();
  summary_->setWordWrap(true);
  summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(summary_);

  log_ = new QPlainTextEdit();
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(500);
  log_->setMinimumHeight(120);
  v->addWidget(log_, 1);

  // --- THE record cluster -------------------------------------------------
  //
  // Pinned to the foot of the window, outside the layout's margins, so it
  // reads as a control bar rather than one more form row. It owns no state:
  // every button emits an intent that lands on the same onTestDevice() /
  // onRecord() / onPauseResume() / onStop() slots the old buttons called, so
  // the C2 state machine is byte-for-byte the one that was verified against
  // the S2 simulator.
  record_cluster_ = new RecordCluster(this);
  connect(record_cluster_, &RecordCluster::testRequested, this, &CaptureWindow::onTestDevice);
  connect(record_cluster_, &RecordCluster::startRequested, this, &CaptureWindow::onRecord);
  connect(record_cluster_, &RecordCluster::pauseResumeRequested, this,
          &CaptureWindow::onPauseResume);
  connect(record_cluster_, &RecordCluster::stopRequested, this, &CaptureWindow::onStop);
  v->setContentsMargins(v->contentsMargins().left(), v->contentsMargins().top(),
                        v->contentsMargins().right(), 0);
  v->addWidget(record_cluster_);

  // Switching transport re-gates Start: a self-test result for the D6 says
  // nothing about the Mid-360 link and vice versa. Index 2 (RTK/UM982) has no
  // Test/Record wiring at all (see the RTK tab's own comment in buildUi()),
  // so the whole cluster is disabled rather than gated there.
  connect(tabs_, &QTabWidget::currentChanged, this, [this](int idx) {
    if (phase_ != Phase::kIdle) return;
    last_self_test_failed_ = false;
    self_test_passed_ = false;
    const bool capturable = idx != 2;
    record_cluster_->setEnabled(capturable);
    record_cluster_->setToolTip(
        capturable ? QString()
                   : "RTK/UM982 capture is not wired into this window yet — "
                     "Auto-detect only fills in the port/baud above.");
    if (capturable) record_cluster_->setTransportIsD6(idx == 0);
    updateRecordCluster();
  });

  // The clock ticks at 4 Hz off its own timer rather than the 300 ms health
  // timer, so the seconds digit never visibly stalls.
  elapsed_timer_ = new QTimer(this);
  connect(elapsed_timer_, &QTimer::timeout, this, [this] {
    if (record_cluster_) record_cluster_->setElapsedSeconds(recordedSecondsNow());
  });
  elapsed_timer_->start(250);

  resize(920, 800);
}

double CaptureWindow::recordedSecondsNow() const {
  double s = recorded_seconds_accum_;
  if (phase_ == Phase::kRecording && record_segment_clock_.isValid()) {
    s += record_segment_clock_.elapsed() / 1000.0;
  }
  return s;
}

void CaptureWindow::updateRecordCluster() {
  if (!record_cluster_) return;
  RecordCluster::State s = RecordCluster::State::kUntested;
  switch (phase_) {
    case Phase::kIdle:
      s = last_self_test_failed_ ? RecordCluster::State::kFailed
                                 : RecordCluster::State::kUntested;
      break;
    case Phase::kTesting: s = RecordCluster::State::kTesting; break;
    case Phase::kReady: s = RecordCluster::State::kArmed; break;
    case Phase::kRecording: s = RecordCluster::State::kRecording; break;
    case Phase::kPaused: s = RecordCluster::State::kPaused; break;
  }
  record_cluster_->setState(s);
  record_cluster_->setElapsedSeconds(recordedSecondsNow());
}

void CaptureWindow::refreshPorts() {
  const QString current = port_ ? port_->currentData().toString() : QString();
  port_->blockSignals(true);
  port_->clear();
  for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
    QString label = info.systemLocation();
    if (!info.description().isEmpty()) label += "  —  " + info.description();
    const bool is_ch340 = info.hasVendorIdentifier() && info.vendorIdentifier() == kCh340VendorId;
    if (info.hasVendorIdentifier()) {
      label += QString("  [%1:%2%3]")
                   .arg(info.vendorIdentifier(), 4, 16, QChar('0'))
                   .arg(info.productIdentifier(), 4, 16, QChar('0'))
                   .arg(is_ch340 ? " CH340" : "");
    }
    port_->addItem(label, info.systemLocation());
    port_->setItemData(port_->count() - 1, is_ch340, Qt::UserRole + 1);
  }
  if (port_->count() == 0) port_->addItem("(no serial ports found)", QString());
  const int idx = port_->findData(current);
  if (idx >= 0) port_->setCurrentIndex(idx);
  port_->blockSignals(false);
  applyCh340Hint();
}

void CaptureWindow::applyCh340Hint() {
  if (!port_ || !ch340_hint_) return;
  const int idx = port_->currentIndex();
  const bool is_ch340 = idx >= 0 && port_->itemData(idx, Qt::UserRole + 1).toBool();
  ch340_hint_->setText(is_ch340
                            ? "This looks like the vendor's own CH340 USB-serial adapter."
                            : QString());
}

scanengine::ScanError CaptureWindow::serialWrite(const std::uint8_t* data, std::size_t n,
                                                 void* user) {
  auto* self = static_cast<CaptureWindow*>(user);
  if (!self || !self->serial_ || !self->serial_->isOpen()) {
    return scanengine::ScanError::kDisconnected;
  }
  const qint64 written = self->serial_->write(reinterpret_cast<const char*>(data), qint64(n));
  if (written != qint64(n)) return scanengine::ScanError::kIoError;
  self->serial_->flush();
  return scanengine::ScanError::kOk;
}

bool CaptureWindow::openDeviceForTab(QString* err) {
  device_is_d6_ = (tabs_->currentIndex() == 0);

  if (device_is_d6_) {
    const QString location = port_->currentData().toString();
    if (location.isEmpty()) {
      if (err) *err = "no serial port selected";
      return false;
    }
    serial_ = new QSerialPort(this);
    serial_->setPortName(location);
    serial_->setBaudRate(baud_->value());
    serial_->setDataBits(QSerialPort::Data8);
    serial_->setParity(QSerialPort::NoParity);
    serial_->setStopBits(QSerialPort::OneStop);
    serial_->setFlowControl(QSerialPort::NoFlowControl);
    if (!serial_->open(QIODevice::ReadWrite)) {
      if (err) *err = QString("cannot open %1: %2").arg(location, serial_->errorString());
      serial_->deleteLater();
      serial_ = nullptr;
      return false;
    }
    connect(serial_, &QSerialPort::readyRead, this, &CaptureWindow::onSerialReadyRead);

    const QByteArray name = location.toUtf8();
    static QByteArray held_name;  // D6Config keeps a const char*, not a copy
    held_name = name;
    scanengine::D6Config cfg;
    cfg.serial.port_name = held_name.constData();
    cfg.serial.baud = std::uint32_t(baud_->value());
    cfg.serial.write_fn = &CaptureWindow::serialWrite;
    cfg.serial.write_user_data = this;
    cfg.send_start_stop_commands = send_commands_->isChecked();
    device_ = host_->addD6(cfg, err);
    if (device_ == scanengine::kInvalidDeviceId) {
      serial_->close();
      serial_->deleteLater();
      serial_ = nullptr;
      return false;
    }
    log(QString("D6 on %1 @ %2 8N1, device #%3").arg(location).arg(baud_->value()).arg(device_));
    return true;
  }

  if (lidar_ip_->text().trimmed().isEmpty()) {
    if (err) *err = "a lidar IP is required (macOS cannot discover by broadcast — S2 finding)";
    return false;
  }
  scanengine::Mid360Config cfg;
  cfg.udp.host_ip = host_ip_->text().trimmed().toStdString();
  cfg.udp.lidar_ip = lidar_ip_->text().trimmed().toStdString();
  cfg.udp.point_port = std::uint16_t(point_port_->value());
  cfg.udp.imu_port = std::uint16_t(imu_port_->value());
  cfg.udp.cmd_port = std::uint16_t(cmd_port_->value());
  device_ = host_->addMid360(cfg, err);
  if (device_ == scanengine::kInvalidDeviceId) return false;
  log(QString("Mid-360 %1 -> host %2, device #%3")
          .arg(lidar_ip_->text(), host_ip_->text())
          .arg(device_));
  saveMid360Settings();
  return true;
}

void CaptureWindow::closeDevice() {
  if (device_ != scanengine::kInvalidDeviceId && host_) {
    QString err;
    (void)host_->removeDevice(device_, &err);
    device_ = scanengine::kInvalidDeviceId;
  }
  if (serial_) {
    serial_->close();
    serial_->deleteLater();
    serial_ = nullptr;
  }
}

bool CaptureWindow::startPreviewSession(QString* err) {
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  if (host_->sessionActive() && !host_->stopSession(err)) return false;
  // Empty lscan_dir + record=false: the live-preview pattern ReplayController
  // also uses. Points flow into the viewport; nothing hits disk.
  return host_->startSession(QString(), profile_->currentText(), false, err);
}

bool CaptureWindow::startRecordingSession(QString* err) {
  if (!host_ || !host_->ok()) {
    if (err) *err = "engine unavailable";
    return false;
  }
  if (host_->sessionActive() && !host_->stopSession(err)) return false;
  return host_->startSession(project_dir_, profile_->currentText(), true, err);
}

void CaptureWindow::accumulateRecorderStats() {
  if (!host_ || !host_->ok()) return;
  const auto stats = host_->engine()->recorder().stats();
  cum_bytes_written_ += stats.bytes_written;
  cum_chunks_written_ += stats.chunks_written;
}

void CaptureWindow::onTestDevice() {
  if (phase_ != Phase::kIdle) return;
  if (tabs_->currentIndex() == 2) {
    log("RTK/UM982 capture is not wired into this window yet — see the RTK tab's hint");
    return;
  }
  if (!host_ || !host_->ok()) {
    log("engine unavailable");
    return;
  }

  QString err;
  if (!startPreviewSession(&err)) {
    log("self-test: " + err);
    return;
  }
  if (!openDeviceForTab(&err)) {
    log("self-test: " + err);
    (void)host_->stopSession(&err);
    return;
  }

  self_test_passed_ = false;
  last_self_test_failed_ = false;
  self_test_clock_.start();
  auto h = host_->engine()->device_health(device_);
  self_test_baseline_points_ = h.ok() ? h.value().points_out : 0;
  self_test_window_s_ = device_is_d6_ ? 3.0 : 8.0;
  if (record_cluster_) record_cluster_->setTransportIsD6(device_is_d6_);

  setPhase(Phase::kTesting);
  self_test_label_->setStyleSheet(QString());
  self_test_label_->setText(device_is_d6_
                                 ? "Testing device: expect ~4,000 pts/s within 3 s…"
                                 : "Testing device: waiting for the first Mid-360 packet "
                                   "(engine add_device + start, first-data-or-timeout)…");
  self_test_progress_->setValue(0);
  log("self-test started");
}

void CaptureWindow::evaluateSelfTest() {
  if (!host_ || !host_->ok() || device_ == scanengine::kInvalidDeviceId) return;
  auto h = host_->engine()->device_health(device_);
  const double elapsed = self_test_clock_.elapsed() / 1000.0;
  const std::uint64_t pts = h.ok() ? h.value().points_out : 0;
  const std::uint64_t gained = pts > self_test_baseline_points_ ? pts - self_test_baseline_points_ : 0;
  const double rate = elapsed > 0.05 ? double(gained) / elapsed : 0.0;

  self_test_progress_->setValue(
      int(std::min(100.0, 100.0 * elapsed / std::max(0.1, self_test_window_s_))));

  if (device_is_d6_) {
    self_test_label_->setText(
        QString("Testing… %1 pts/s so far (target ~4,000, %2 / %3 s)")
            .arg(rate, 0, 'f', 0)
            .arg(elapsed, 0, 'f', 1)
            .arg(self_test_window_s_, 0, 'f', 0));
    if (elapsed >= self_test_window_s_) {
      const bool passed = rate >= 3000.0;  // margin below the ~4k nominal rate
      const QString detail =
          QString("%1 pts/s over %2 s (%3 points)").arg(rate, 0, 'f', 0).arg(elapsed, 0, 'f', 1).arg(gained);
      self_test_passed_ = passed;
      last_self_test_failed_ = !passed;
      self_test_progress_->setValue(100);
      if (passed) {
        self_test_label_->setStyleSheet(kPassStyle());
        self_test_label_->setText("Self-test PASSED — " + detail);
        setPhase(Phase::kReady);
        log("self-test passed: " + detail);
      } else {
        self_test_label_->setStyleSheet(kFailStyle());
        self_test_label_->setText("Self-test FAILED — " + detail);
        log("self-test failed: " + detail);
        QString err;
        (void)host_->stopSession(&err);
        closeDevice();
        setPhase(Phase::kIdle);
      }
      Q_EMIT selfTestFinished(passed, detail);
    }
  } else {
    if (gained > 0) {
      const QString detail = QString("first packet after %1 s").arg(elapsed, 0, 'f', 2);
      const bool passed = true;
      self_test_passed_ = passed;
      last_self_test_failed_ = false;
      self_test_progress_->setValue(100);
      self_test_label_->setStyleSheet(kPassStyle());
      self_test_label_->setText("Self-test PASSED — " + detail);
      setPhase(Phase::kReady);
      log("self-test passed: " + detail);
      Q_EMIT selfTestFinished(true, detail);
    } else if (elapsed >= self_test_window_s_) {
      const QString state = h.ok() ? scanengine::to_string(h.value().state) : "unknown";
      const QString detail = QString("no packet within %1 s (device state: %2)")
                                  .arg(self_test_window_s_, 0, 'f', 0)
                                  .arg(state);
      self_test_passed_ = false;
      last_self_test_failed_ = true;
      self_test_progress_->setValue(100);
      self_test_label_->setStyleSheet(kFailStyle());
      self_test_label_->setText("Self-test FAILED — " + detail);
      log("self-test failed: " + detail);
      QString err;
      (void)host_->stopSession(&err);
      closeDevice();
      setPhase(Phase::kIdle);
      Q_EMIT selfTestFinished(false, detail);
    } else {
      self_test_label_->setText(
          QString("Testing… waiting for first packet (%1 / %2 s)")
              .arg(elapsed, 0, 'f', 1)
              .arg(self_test_window_s_, 0, 'f', 0));
    }
  }
}

void CaptureWindow::onRecord() {
  if (phase_ != Phase::kReady) return;
  const QString dir = project_edit_->text().trimmed();
  if (dir.isEmpty()) {
    log("choose a .lscan project directory first");
    return;
  }
  project_dir_ = dir;

  QString err;
  if (!startRecordingSession(&err)) {
    log("record: " + err);
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
  log("recording started -> " + dir);
  Q_EMIT captureStarted(dir);
}

void CaptureWindow::onPauseResume() {
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
  } else if (phase_ == Phase::kPaused) {
    if (!startRecordingSession(&err)) {
      log("resume: " + err);
      return;
    }
    record_segment_clock_.start();
    setPhase(Phase::kRecording);
    log("capture resumed -> " + project_dir_);
  }
}

void CaptureWindow::onStop() {
  if (phase_ == Phase::kIdle) return;
  const bool was_recording_or_paused = phase_ == Phase::kRecording || phase_ == Phase::kPaused;

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

  QString err;
  (void)host_->stopSession(&err);
  closeDevice();
  setPhase(Phase::kIdle);

  if (was_recording_or_paused) {
    const QString sum =
        QString("Session summary — %1 s recording · %2 chunks / %3 written · "
                "%4 points decoded since Record (device counters also include any "
                "paused time) · %5 drops (device) / %6 dropped (store)")
            .arg(recorded_seconds_accum_, 0, 'f', 1)
            .arg(cum_chunks_written_)
            .arg(humanBytesLocal(cum_bytes_written_))
            .arg(points_now - record_baseline_points_)
            .arg(drops_now)
            .arg(store_dropped);
    summary_->setText(sum);
    log(sum);
  } else {
    summary_->clear();
    log("capture stopped");
  }
  self_test_label_->clear();
  self_test_label_->setStyleSheet(QString());
  self_test_progress_->setValue(0);
  Q_EMIT captureStopped();
}

void CaptureWindow::onSerialReadyRead() {
  if (!serial_ || !host_ || device_ == scanengine::kInvalidDeviceId || !device_is_d6_) return;
  // NOT named `data`: QWidget has a public member `QWidgetData* data`, and
  // MSVC /W4 flags the shadowing (C4458) where clang -Wall -Wextra does not.
  // The one desktop-side warning the first Windows CI build found.
  const QByteArray bytes = serial_->readAll();
  if (bytes.isEmpty()) return;
  // Arrival-stamped by the engine (TimePoint{0} = "stamp now"): the app has no
  // better stamp than arrival for a USB serial device (timesync/clock.h).
  const auto st = host_->engine()->push_serial_bytes(
      device_,
      scanengine::ByteSpan(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                           std::size_t(bytes.size())));
  if (!st.ok() && st.error() != scanengine::ScanError::kAgain) {
    log(QString("push_serial_bytes: %1").arg(scanengine::error_str(st.error())));
  }
}

void CaptureWindow::updateHealth() {
  if (!host_) return;
  if (phase_ == Phase::kTesting) evaluateSelfTest();

  if (device_ != scanengine::kInvalidDeviceId && host_->ok()) {
    auto h = host_->engine()->device_health(device_);
    if (h.ok()) {
      const auto& d = h.value();
      QString flag;
      if (d.state == scanengine::DeviceState::kDegraded) {
        flag = QString(" · DEGRADED (%1) — the generic DeviceHealth API has no "
                        "restart-attempt counter; see NOTES.md")
                   .arg(scanengine::error_str(d.last_error));
      } else if (d.state == scanengine::DeviceState::kFault) {
        flag = QString(" · FAULT (%1)").arg(scanengine::error_str(d.last_error));
      }
      health_->setText(
          QString("%1 · %2 pts/s · %3 %4 · %5% ok rate · %6 pts total / %7 in · %8 drops%9")
              .arg(scanengine::to_string(d.state))
              .arg(d.points_per_sec, 0, 'f', 0)
              .arg(d.rotation_hz, 0, 'f', 2)
              .arg(device_is_d6_ ? "Hz rotation" : "Hz IMU")
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

void CaptureWindow::setPhase(Phase p) {
  const bool was_live = phase_ == Phase::kRecording || phase_ == Phase::kPaused;
  phase_ = p;
  const bool idle = p == Phase::kIdle;
  const bool testing = p == Phase::kTesting;
  const bool ready = p == Phase::kReady;
  const bool recording = p == Phase::kRecording;
  const bool paused = p == Phase::kPaused;

  tabs_->setEnabled(idle);
  project_edit_->setEnabled(idle || ready);
  profile_->setEnabled(idle);
  self_test_progress_->setVisible(testing);

  updateRecordCluster();

  // The viewport badge follows the phase, not the other way round. Emitted on
  // every transition (including live -> live, i.e. pause/resume) so the badge
  // can swap RECORDING <-> PAUSED, and on the way back to idle so it clears.
  const bool live = recording || paused;
  if (live || was_live) Q_EMIT recordingStateChanged(recording, paused);
}

void CaptureWindow::loadMid360Settings() {
  if (project_dir_.isEmpty() || !host_ip_) return;
  QSettings s;
  QString key = project_dir_;
  key.replace('/', '_').replace('\\', '_').replace(':', '_');
  s.beginGroup("mid360/" + key);
  // "Never configured" (the silent auto-run's gate, see showEvent()) means
  // this project has no saved Mid-360 host IP at all — i.e. host_ip_/
  // lidar_ip_ below are about to stay at their hard-coded placeholder text.
  had_saved_mid360_settings_ = s.contains("hostIp");
  host_ip_->setText(s.value("hostIp", host_ip_->text()).toString());
  lidar_ip_->setText(s.value("lidarIp", lidar_ip_->text()).toString());
  point_port_->setValue(s.value("pointPort", point_port_->value()).toInt());
  imu_port_->setValue(s.value("imuPort", imu_port_->value()).toInt());
  cmd_port_->setValue(s.value("cmdPort", cmd_port_->value()).toInt());
  s.endGroup();
}

void CaptureWindow::saveMid360Settings() {
  if (project_dir_.isEmpty() || !host_ip_) return;
  QSettings s;
  QString key = project_dir_;
  key.replace('/', '_').replace('\\', '_').replace(':', '_');
  s.beginGroup("mid360/" + key);
  s.setValue("hostIp", host_ip_->text());
  s.setValue("lidarIp", lidar_ip_->text());
  s.setValue("pointPort", point_port_->value());
  s.setValue("imuPort", imu_port_->value());
  s.setValue("cmdPort", cmd_port_->value());
  s.endGroup();
}

void CaptureWindow::runMid360SelfTestForCli(const QString& hostIp, const QString& lidarIp) {
  tabs_->setCurrentIndex(1);  // Mid-360 tab
  host_ip_->setText(hostIp);
  lidar_ip_->setText(lidarIp);
  onTestDevice();
}

void CaptureWindow::triggerRecordForCli(const QString& projectDir) {
  project_edit_->setText(projectDir);
  onRecord();
}

void CaptureWindow::triggerPauseResumeForCli() { onPauseResume(); }

void CaptureWindow::triggerStopForCli() { onStop(); }

void CaptureWindow::triggerAutoDetectForCli() { onAutoDetectClicked(); }

// --- Auto-detect ------------------------------------------------------------
//
// docs/design/REVIEW_FEEDBACK.md, 2026-08-17 round 4 item 5 (the owner, after
// the first real-hardware GUI session): "the apps must auto-detect device
// settings — Mid-360 via broadcast heartbeat (lidar IP/SN/persisted host
// revealed; proven manually in the field session), D6 via serial protocol
// probe, UM982 via port+baud sweep. Manual IP entry defeated the GUI on first
// contact." captures/FIELD_SESSION_2026-08-17.md is that field session; its
// numbers (SN MCP7K0034759, fw 35010108, lidar 192.168.1.159, persisted host
// 192.168.1.5, UM982 on /dev/cu.usbserial-21140 @ 230400 with GPTHS present)
// are exactly the shape of the beacon/probe data this section renders.

void CaptureWindow::buildAutoDetectSection(QVBoxLayout* v) {
  auto_detect_btn_ = new QPushButton("Auto-detect devices");
  auto_detect_btn_->setProperty("accent", "ember");
  auto_detect_btn_->setCursor(Qt::PointingHandCursor);
  auto_detect_btn_->setMinimumHeight(40);
  auto_detect_btn_->setToolTip(
      "Listens for a Mid-360's heartbeat and sweeps serial ports for a D6 or a "
      "UM982 (~6 s). Fills in whatever it finds; anything it does not see is "
      "reported with a likely cause.");
  connect(auto_detect_btn_, &QPushButton::clicked, this, &CaptureWindow::onAutoDetectClicked);
  v->addWidget(auto_detect_btn_);

  // Persistent summary panel — stays after the (transient, manual-run-only)
  // progress dialog closes, and is what the silent on-open run's result
  // shows up as (there being no dialog for that path at all).
  auto_detect_panel_ = new QWidget();
  auto* pv = new QVBoxLayout(auto_detect_panel_);
  pv->setContentsMargins(2, 4, 2, 10);
  pv->setSpacing(3);

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

void CaptureWindow::onAutoDetectClicked() {
  if (discovery_in_flight_) return;
  startDiscovery(/*silent=*/false);
}

void CaptureWindow::startDiscovery(bool silent) {
  if (discovery_in_flight_) return;
  discovery_in_flight_ = true;
  auto_detect_btn_->setEnabled(false);

  if (!silent) {
    auto_detect_progress_ = new QDialog(this);
    auto_detect_progress_->setWindowTitle("Auto-detect devices");
    auto_detect_progress_->setModal(true);
    auto_detect_progress_->setMinimumWidth(380);
    auto* dv = new QVBoxLayout(auto_detect_progress_);
    auto_detect_progress_label_ = new QLabel("Listening for Mid-360 heartbeat…");
    auto_detect_progress_label_->setWordWrap(true);
    auto* bar = new QProgressBar(auto_detect_progress_);
    bar->setRange(0, 0);  // indeterminate: the two phases run for different real time
    dv->addWidget(auto_detect_progress_label_);
    dv->addWidget(bar);
    auto_detect_progress_->show();
  }

  // Mid-360 heartbeat is ~1 Hz (spikes/s2-mid360-sim REPORT.md); 3 s gives it
  // several windows. discovery.h's ProbeSerialD6/Um982 spend `per_port_ms`
  // PER ENUMERATED PORT, trying each in turn until one identifies, so the
  // serial phase's total time scales with how many serial devices this
  // machine has — 700 ms keeps a typical 2-4-port machine near the "~6 s"
  // total the task asks for (3 s + up to ~2.8 s D6 + up to ~2.8 s UM982 in
  // the worst no-match case); a machine with many more ports will simply
  // take longer, which the indeterminate progress bar tolerates fine.
  // NO parent on the QThread — deliberately. A parented child gets
  // synchronously `delete`d by Qt's normal ownership teardown the moment
  // CaptureWindow (parented under MainWindow) is destroyed, which for a
  // QThread whose run() is still blocking in a scanengine::discovery call is
  // exactly "Destroyed while thread is still running" -> abort(). Measured,
  // not theoretical: an early version of this code parented the thread to
  // `this` and reliably crashed on quit whenever a second discovery pass was
  // still in flight (--auto-detect-selftest against a live source races the
  // silent on-open pass into a second, explicit one — see main.cpp's evidence
  // hook — and a 6 s --quit-after can land mid-pass). Leaving it unparented
  // means the only thing CaptureWindow's destruction touches is the `this`-
  // context lambda below, which Qt auto-disconnects on destruction, same as
  // any other signal/slot connection — the thread and worker are on their
  // own self-contained cleanup chain (next comment) and simply finish
  // unobserved if nobody is left to hear about it.
  auto* thread = new QThread();
  discovery_thread_ = thread;
  auto* worker = new DiscoveryWorker(3000, 700);
  worker->moveToThread(thread);
  connect(thread, &QThread::started, worker, &DiscoveryWorker::run);
  connect(worker, &DiscoveryWorker::phase, this, [this](const QString& label) {
    if (auto_detect_progress_label_) auto_detect_progress_label_->setText(label);
  });
  connect(worker, &DiscoveryWorker::finished, this,
          [this, silent](DiscoveryResult r) { handleDiscoveryFinished(r, silent); });
  // The canonical Qt moveToThread cleanup chain: run() finishing asks the
  // thread to quit, and each object is deleted once IT stops needing to
  // exist, from the thread it lives in. This chain does not depend on
  // CaptureWindow at all (unlike the lambda above, which is connected with
  // `this` as context and so is automatically dropped if CaptureWindow is
  // destroyed first), so a discovery pass in flight at app shutdown cleans
  // itself up rather than being blocked on or leaked.
  connect(worker, &DiscoveryWorker::finished, thread, &QThread::quit);
  connect(worker, &DiscoveryWorker::finished, worker, &QObject::deleteLater);
  connect(thread, &QThread::finished, thread, &QObject::deleteLater);
  thread->start();
}

void CaptureWindow::handleDiscoveryFinished(const DiscoveryResult& r, bool silent) {
  discovery_in_flight_ = false;
  if (discovery_thread_) discovery_thread_ = nullptr;  // it is finishing itself off; see startDiscovery()
  if (auto_detect_btn_) auto_detect_btn_->setEnabled(true);
  if (auto_detect_progress_) {
    auto_detect_progress_->close();
    auto_detect_progress_->deleteLater();
    auto_detect_progress_ = nullptr;
    auto_detect_progress_label_ = nullptr;
  }

  applyMid360Result(r, silent);
  applyD6Result(r, silent);
  applyUm982Result(r, silent);
  if (auto_detect_panel_) auto_detect_panel_->setVisible(true);

  log(QString("auto-detect%1: Mid-360 %2, D6 %3, UM982 %4")
          .arg(silent ? " (silent, on open)" : "")
          .arg(r.mid360.found ? "found" : "not seen")
          .arg(r.d6.found ? "found" : "not seen")
          .arg(r.um982.found ? "found" : "not seen"));
  Q_EMIT autoDetectFinished(r.mid360.found, r.d6.found, r.um982.found);
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

  // Confirmation line — SN + firmware — is shown whenever a beacon was
  // heard, silent run or not: it is informational, never a field write.
  auto_detect_mid360_line_->setText(
      QString("Found Mid-360 SN %1, fw %2, at %3.").arg(m.sn, m.fw_version, m.lidar_ip));
  auto_detect_mid360_line_->setProperty("tone", "good");
  repolish(auto_detect_mid360_line_);

  // Prefill guard: on a silent (auto-run-on-open) pass, a field already
  // holding something other than the hard-coded placeholder is left alone —
  // "never overwrite user-entered values". The manual button always fills in
  // what it found; that is the point of clicking it.
  const bool lidar_ip_is_default = lidar_ip_->text().trimmed() == "192.168.1.100";
  const bool host_ip_is_default = host_ip_->text().trimmed() == "192.168.1.5";
  if (!silent || lidar_ip_is_default) lidar_ip_->setText(m.lidar_ip);

  // discovery.h's CheckHostReachability() always sets suggested_host_ip to
  // the beacon's OWN persisted_host_ip when the beacon carried one — a
  // locally-held address is only ever suggested as a NEW host to configure
  // the lidar with, for a beacon that carried no persisted host at all (a
  // factory-fresh/reset device). So the meaningful branch is not "which
  // address to use" (there is only ever one candidate value once a persisted
  // host exists) but "does this Mac already hold it" — everything else is
  // the fix for when it does not.
  if (m.host_ip_is_local) {
    // Ready: this Mac already holds the address the lidar streams to (or, if
    // the beacon carried no persisted host, whatever address discovery
    // resolved as the one to configure it with). Nothing to fix.
    if (!silent || host_ip_is_default) {
      host_ip_->setText(m.persisted_host_ip.isEmpty() ? m.suggested_host_ip : m.persisted_host_ip);
    }
  } else if (m.persisted_host_ip.isEmpty() && !m.suggested_host_ip.isEmpty()) {
    // A factory-fresh lidar with no host configured yet — genuinely a
    // DIFFERENT address than "persisted" (there is none), and connecting
    // really will push a fresh host-IP config into the device.
    if (!silent || host_ip_is_default) host_ip_->setText(m.suggested_host_ip);
    QString note = QString("lidar has no host address configured yet; using %1 — the "
                           "first connect will configure it")
                       .arg(m.suggested_host_ip);
    auto_detect_fix_line_->setText(note);
    auto_detect_fix_line_->setToolTip(m.host_check_note);
    auto_detect_fix_line_->setProperty("tone", "warn");
    repolish(auto_detect_fix_line_);
    auto_detect_fix_line_->setVisible(true);
  } else if (!m.persisted_host_ip.isEmpty()) {
    // A persisted host exists and this Mac does not hold it — the
    // field-session case (captures/FIELD_SESSION_2026-08-17.md: a host-only
    // route plus an interface alias were both needed before the SDK would
    // talk to a real Mid-360). host_ip_ still gets the persisted value
    // (that IS what the driver needs to declare) — the alias below is what
    // makes that value real on this machine. suggested_interface is set
    // whenever a local address already sits on the lidar's subnet
    // (on_lidar_subnet); otherwise <if> stays literal — guessing which
    // physical interface the lidar is wired to would be worse than asking.
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
    // Nothing to work with at all: no persisted host on the beacon AND no
    // local address on the lidar's subnet either — there is no concrete
    // command to offer (no target IP, no interface), so this falls back to
    // the engine's own operator-facing sentence rather than a hand-built
    // one that would have nothing to substitute in.
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

void CaptureWindow::applyD6Result(const DiscoveryResult& r, bool silent) {
  const auto& d = r.d6;
  if (!d.found) {
    auto_detect_d6_line_->setText(
        "COIN-D6: not seen — check power, the CH340 cable, and that no other app "
        "(Livox Viewer, a serial monitor) already has the port open.");
    auto_detect_d6_line_->setProperty("tone", "warn");
    repolish(auto_detect_d6_line_);
    return;
  }
  auto_detect_d6_line_->setText(
      QString("COIN-D6: found on %1 (%2 valid packets seen).").arg(d.port).arg(d.packets_ok));
  auto_detect_d6_line_->setProperty("tone", "good");
  repolish(auto_detect_d6_line_);

  const bool port_unselected = port_->currentData().toString().isEmpty();
  if (!silent || port_unselected) {
    int idx = port_->findData(d.port);
    if (idx < 0) {
      // refreshPorts() runs on its own 2 s timer and may not have enumerated
      // this port under this exact name yet — add it so it is selectable now
      // rather than waiting for the next tick.
      port_->addItem(d.port, d.port);
      idx = port_->count() - 1;
    }
    port_->setCurrentIndex(idx);
    if (d6_auto_tag_) d6_auto_tag_->setVisible(true);
  }
}

void CaptureWindow::applyUm982Result(const DiscoveryResult& r, bool silent) {
  const auto& u = r.um982;
  if (!u.found) {
    auto_detect_um982_line_->setText(
        "UM982: not seen — check power and the USB-serial cable. A probe hit only "
        "needs the receiver to talk NMEA, not a satellite fix, so this is not "
        "about sky visibility.");
    auto_detect_um982_line_->setProperty("tone", "warn");
    repolish(auto_detect_um982_line_);
    if (um982_heading_) um982_heading_->setText("Dual-antenna heading: unknown (run Auto-detect)");
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
  if (!log_) return;
  log_->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz ") + s);
}

}  // namespace lidarscan
