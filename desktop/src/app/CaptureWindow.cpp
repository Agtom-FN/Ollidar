#include "app/CaptureWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QSpinBox>
#include <QSysInfo>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "app/EngineHost.h"

namespace lidarscan {

CaptureWindow::CaptureWindow(EngineHost* host, QWidget* parent)
    : QDialog(parent), host_(host) {
  setWindowTitle("Capture");
  setModal(false);
  buildUi();
  refreshPorts();
  health_timer_ = new QTimer(this);
  connect(health_timer_, &QTimer::timeout, this, &CaptureWindow::updateHealth);
  health_timer_->start(500);
}

CaptureWindow::~CaptureWindow() {
  if (running_) onStop();
}

void CaptureWindow::setProjectDir(const QString& dir) {
  project_dir_ = dir;
  if (project_edit_) project_edit_->setText(dir);
}

void CaptureWindow::buildUi() {
  auto* v = new QVBoxLayout(this);
  tabs_ = new QTabWidget();

  // --- D6 tab ---
  {
    auto* w = new QWidget();
    auto* f = new QFormLayout(w);
    port_ = new QComboBox();
    auto* refresh = new QPushButton("Refresh");
    connect(refresh, &QPushButton::clicked, this, &CaptureWindow::refreshPorts);
    auto* row = new QWidget();
    auto* rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(port_, 1);
    rl->addWidget(refresh);
    f->addRow("Serial port", row);

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
        "adapter appears as a COM port. C2 owns the guided install.");
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
        "192.168.1.x. Power is 9-27 V / ~6.5 W — an external battery is required.");
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
        "PageStore, so the GPU and the UI thread stay out of the capture's way.");
    f->addRow(live_mode_);

    start_ = new QPushButton("Start capture");
    stop_ = new QPushButton("Stop");
    stop_->setEnabled(false);
    connect(start_, &QPushButton::clicked, this, &CaptureWindow::onStart);
    connect(stop_, &QPushButton::clicked, this, &CaptureWindow::onStop);
    auto* brow = new QWidget();
    auto* bl = new QHBoxLayout(brow);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addWidget(start_);
    bl->addWidget(stop_);
    bl->addStretch(1);
    f->addRow(brow);
    v->addWidget(g);
  }

  health_ = new QLabel("idle");
  health_->setWordWrap(true);
  v->addWidget(health_);

  log_ = new QPlainTextEdit();
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(500);
  log_->setMinimumHeight(120);
  v->addWidget(log_, 1);

  resize(620, 640);
}

void CaptureWindow::refreshPorts() {
  const QString current = port_ ? port_->currentText() : QString();
  port_->clear();
  for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
    QString label = info.systemLocation();
    if (!info.description().isEmpty()) label += "  —  " + info.description();
    if (info.hasVendorIdentifier()) {
      label += QString("  [%1:%2]")
                   .arg(info.vendorIdentifier(), 4, 16, QChar('0'))
                   .arg(info.productIdentifier(), 4, 16, QChar('0'));
    }
    port_->addItem(label, info.systemLocation());
  }
  if (port_->count() == 0) port_->addItem("(no serial ports found)", QString());
  const int idx = port_->findText(current);
  if (idx >= 0) port_->setCurrentIndex(idx);
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

void CaptureWindow::onStart() {
  if (!host_ || !host_->ok()) {
    log("engine unavailable");
    return;
  }
  const QString dir = project_edit_->text().trimmed();
  if (dir.isEmpty()) {
    log("choose a .lscan project directory first");
    return;
  }

  QString err;
  if (host_->sessionActive() && !host_->stopSession(&err)) {
    log("stop previous session: " + err);
  }
  if (!host_->startSession(dir, profile_->currentText(), true, &err)) {
    log("start_session failed: " + err);
    return;
  }
  project_dir_ = dir;

  if (tabs_->currentIndex() == 0) {
    // --- D6 over serial ---
    const QString location = port_->currentData().toString();
    if (location.isEmpty()) {
      log("no serial port selected");
      (void)host_->stopSession(&err);
      return;
    }
    serial_ = new QSerialPort(this);
    serial_->setPortName(location);
    serial_->setBaudRate(baud_->value());
    serial_->setDataBits(QSerialPort::Data8);
    serial_->setParity(QSerialPort::NoParity);
    serial_->setStopBits(QSerialPort::OneStop);
    serial_->setFlowControl(QSerialPort::NoFlowControl);
    if (!serial_->open(QIODevice::ReadWrite)) {
      log(QString("cannot open %1: %2").arg(location, serial_->errorString()));
      serial_->deleteLater();
      serial_ = nullptr;
      (void)host_->stopSession(&err);
      return;
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
    d6_device_ = host_->addD6(cfg, &err);
    if (d6_device_ == scanengine::kInvalidDeviceId) {
      log("add_device(D6) failed: " + err);
      serial_->close();
      serial_->deleteLater();
      serial_ = nullptr;
      (void)host_->stopSession(&err);
      return;
    }
    log(QString("D6 on %1 @ %2 8N1, device #%3").arg(location).arg(baud_->value()).arg(d6_device_));
  } else {
    // --- Mid-360 over Ethernet ---
    if (lidar_ip_->text().trimmed().isEmpty()) {
      log("a lidar IP is required (macOS cannot discover by broadcast — S2 finding)");
      (void)host_->stopSession(&err);
      return;
    }
    scanengine::Mid360Config cfg;
    cfg.udp.host_ip = host_ip_->text().trimmed().toStdString();
    cfg.udp.lidar_ip = lidar_ip_->text().trimmed().toStdString();
    cfg.udp.point_port = std::uint16_t(point_port_->value());
    cfg.udp.imu_port = std::uint16_t(imu_port_->value());
    cfg.udp.cmd_port = std::uint16_t(cmd_port_->value());
    mid_device_ = host_->addMid360(cfg, &err);
    if (mid_device_ == scanengine::kInvalidDeviceId) {
      log("add_device(Mid-360) failed: " + err);
      (void)host_->stopSession(&err);
      return;
    }
    log(QString("Mid-360 %1 -> host %2, device #%3")
            .arg(lidar_ip_->text(), host_ip_->text())
            .arg(mid_device_));
  }

  running_ = true;
  start_->setEnabled(false);
  stop_->setEnabled(true);
  tabs_->setEnabled(false);
  Q_EMIT captureStarted(project_dir_);
}

void CaptureWindow::onSerialReadyRead() {
  if (!serial_ || !host_ || d6_device_ == scanengine::kInvalidDeviceId) return;
  const QByteArray data = serial_->readAll();
  if (data.isEmpty()) return;
  // Arrival-stamped by the engine (TimePoint{0} = "stamp now"): the app has no
  // better stamp than arrival for a USB serial device (timesync/clock.h).
  const auto st = host_->engine()->push_serial_bytes(
      d6_device_,
      scanengine::ByteSpan(reinterpret_cast<const std::uint8_t*>(data.constData()),
                           std::size_t(data.size())));
  if (!st.ok() && st.error() != scanengine::ScanError::kAgain) {
    log(QString("push_serial_bytes: %1").arg(scanengine::error_str(st.error())));
  }
}

void CaptureWindow::onStop() {
  if (!running_) return;
  QString err;
  if (serial_) {
    serial_->close();
    serial_->deleteLater();
    serial_ = nullptr;
  }
  if (d6_device_ != scanengine::kInvalidDeviceId) {
    (void)host_->removeDevice(d6_device_, &err);
    d6_device_ = scanengine::kInvalidDeviceId;
  }
  if (mid_device_ != scanengine::kInvalidDeviceId) {
    (void)host_->removeDevice(mid_device_, &err);
    mid_device_ = scanengine::kInvalidDeviceId;
  }
  (void)host_->stopSession(&err);
  running_ = false;
  start_->setEnabled(true);
  stop_->setEnabled(false);
  tabs_->setEnabled(true);
  log("capture stopped, .lscan sealed");
  Q_EMIT captureStopped();
}

void CaptureWindow::updateHealth() {
  if (!host_) return;
  health_->setText(host_->healthLine());
}

void CaptureWindow::log(const QString& s) {
  if (!log_) return;
  log_->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss.zzz ") + s);
}

}  // namespace lidarscan
