// CaptureWindow.h — the C1 skeleton of the desktop capture window (Tech Spec
// §3.13 "Capture window (both sensors, same wizards, per-OS driver guidance)").
//
// SCOPE. C1 owns the shell: device setup, start/stop wired to the engine, the
// Live/Record-only toggle and a health panel. C2 owns what turns this into a
// finished flow — per-OS setup wizards, self-tests, CH340/driver guidance,
// reconnect UX.
//
// D6 (serial). QSerialPortInfo enumeration + QSerialPort at 230400 8N1
// (§2.1/§3.1). The read path is `readyRead` -> Engine::push_serial_bytes(), and
// the write path is a SerialWriteFn trampoline installed in D6Config so the
// driver can send the start (AA 55 F0 0F) and stop (AA 55 F5 0A) commands
// itself. This is a real, functional serial connection: a D6 on a CH340 would
// stream through it today. It is UNTESTED against hardware — none is present.
//
// Decode runs on the GUI thread, which is deliberate for the D6 and only for
// the D6: DESIGN.md §2 makes the whole decode path synchronous on the pushing
// thread, and 4,000 pts/s (~23 kB/s) costs microseconds. A Mid-360 at 200k
// pts/s must NOT be pumped this way — its bytes arrive on the driver's own
// SDK2/UDP threads inside the engine, which is why the Mid-360 tab configures
// the engine and never touches a socket.
//
// Owner: C1 (skeleton) → C2 (flows).
#pragma once

#include <QDialog>
#include <QString>

#include "scanengine/core/types.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSerialPort;
class QSpinBox;
class QTabWidget;
class QTimer;

namespace lidarscan {

class EngineHost;

class CaptureWindow : public QDialog {
  Q_OBJECT
 public:
  explicit CaptureWindow(EngineHost* host, QWidget* parent = nullptr);
  ~CaptureWindow() override;

  void setProjectDir(const QString& dir);

 Q_SIGNALS:
  void captureStarted(const QString& project_dir);
  void captureStopped();

 private:
  void buildUi();
  void refreshPorts();
  void onStart();
  void onStop();
  void onSerialReadyRead();
  void updateHealth();
  void log(const QString& s);

  // Trampoline installed in D6Config::serial.write_fn so the driver can send
  // its own start/stop command frames.
  static scanengine::ScanError serialWrite(const std::uint8_t* data, std::size_t n, void* user);

  EngineHost* host_ = nullptr;
  QSerialPort* serial_ = nullptr;
  QTimer* health_timer_ = nullptr;

  QString project_dir_;
  scanengine::DeviceId d6_device_ = scanengine::kInvalidDeviceId;
  scanengine::DeviceId mid_device_ = scanengine::kInvalidDeviceId;
  bool running_ = false;

  QTabWidget* tabs_ = nullptr;

  // D6
  QComboBox* port_ = nullptr;
  QSpinBox* baud_ = nullptr;
  QCheckBox* send_commands_ = nullptr;
  QLabel* port_hint_ = nullptr;

  // Mid-360
  QLineEdit* host_ip_ = nullptr;
  QLineEdit* lidar_ip_ = nullptr;
  QSpinBox* point_port_ = nullptr;
  QSpinBox* imu_port_ = nullptr;
  QSpinBox* cmd_port_ = nullptr;
  QLabel* mid_hint_ = nullptr;

  // session
  QLineEdit* project_edit_ = nullptr;
  QComboBox* profile_ = nullptr;
  QCheckBox* live_mode_ = nullptr;
  QPushButton* start_ = nullptr;
  QPushButton* stop_ = nullptr;
  QLabel* health_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
};

}  // namespace lidarscan
