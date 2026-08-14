#include "app/CaptureWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSettings>
#include <QSpinBox>
#include <QSysInfo>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "app/EngineHost.h"

namespace lidarscan {
namespace {

// WCH's CH340/CH341 USB-serial chip — the vendor's own D6 adapter, and the
// one every per-OS driver-guidance string in this file already names.
constexpr quint16 kCh340VendorId = 0x1A86;

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
  loadMid360Settings();
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
    ch340_hint_->setStyleSheet("color:#2e7d32;");
    f->addRow(ch340_hint_);

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

    test_button_ = new QPushButton("Test device");
    record_button_ = new QPushButton("Record");
    pause_button_ = new QPushButton("Pause");
    stop_button_ = new QPushButton("Stop");
    connect(test_button_, &QPushButton::clicked, this, &CaptureWindow::onTestDevice);
    connect(record_button_, &QPushButton::clicked, this, &CaptureWindow::onRecord);
    connect(pause_button_, &QPushButton::clicked, this, &CaptureWindow::onPauseResume);
    connect(stop_button_, &QPushButton::clicked, this, &CaptureWindow::onStop);
    auto* brow = new QWidget();
    auto* bl = new QHBoxLayout(brow);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addWidget(test_button_);
    bl->addWidget(record_button_);
    bl->addWidget(pause_button_);
    bl->addWidget(stop_button_);
    bl->addStretch(1);
    f->addRow(brow);

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

  resize(660, 720);
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
  self_test_clock_.start();
  auto h = host_->engine()->device_health(device_);
  self_test_baseline_points_ = h.ok() ? h.value().points_out : 0;
  self_test_window_s_ = device_is_d6_ ? 3.0 : 8.0;

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
      self_test_progress_->setValue(100);
      if (passed) {
        self_test_label_->setStyleSheet("color:#2e7d32;font-weight:600;");
        self_test_label_->setText("Self-test PASSED — " + detail);
        setPhase(Phase::kReady);
        log("self-test passed: " + detail);
      } else {
        self_test_label_->setStyleSheet("color:#c62828;font-weight:600;");
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
      self_test_progress_->setValue(100);
      self_test_label_->setStyleSheet("color:#2e7d32;font-weight:600;");
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
      self_test_progress_->setValue(100);
      self_test_label_->setStyleSheet("color:#c62828;font-weight:600;");
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
  const QByteArray data = serial_->readAll();
  if (data.isEmpty()) return;
  // Arrival-stamped by the engine (TimePoint{0} = "stamp now"): the app has no
  // better stamp than arrival for a USB serial device (timesync/clock.h).
  const auto st = host_->engine()->push_serial_bytes(
      device_,
      scanengine::ByteSpan(reinterpret_cast<const std::uint8_t*>(data.constData()),
                           std::size_t(data.size())));
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
  phase_ = p;
  const bool idle = p == Phase::kIdle;
  const bool testing = p == Phase::kTesting;
  const bool ready = p == Phase::kReady;
  const bool recording = p == Phase::kRecording;
  const bool paused = p == Phase::kPaused;

  test_button_->setEnabled(idle);
  record_button_->setEnabled(ready);
  pause_button_->setEnabled(recording || paused);
  pause_button_->setText(paused ? "Resume" : "Pause");
  stop_button_->setEnabled(testing || ready || recording || paused);
  tabs_->setEnabled(idle);
  project_edit_->setEnabled(idle || ready);
  profile_->setEnabled(idle);
  self_test_progress_->setVisible(testing);
}

void CaptureWindow::loadMid360Settings() {
  if (project_dir_.isEmpty() || !host_ip_) return;
  QSettings s;
  QString key = project_dir_;
  key.replace('/', '_').replace('\\', '_').replace(':', '_');
  s.beginGroup("mid360/" + key);
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

void CaptureWindow::triggerStopForCli() { onStop(); }

void CaptureWindow::log(const QString& s) {
  if (!log_) return;
  log_->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz ") + s);
}

}  // namespace lidarscan
