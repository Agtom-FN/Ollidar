#include "app/SerialReader.h"

#include <QSerialPort>

#include "app/EngineHost.h"
#include "app/FieldLog.h"

namespace lidarscan {

SerialReader::SerialReader(QObject* parent) : QObject(parent) {
  buf_.reserve(4096);
}

SerialReader::~SerialReader() { close(); }

bool SerialReader::isOpen() const { return port_ != nullptr && port_->isOpen(); }

bool SerialReader::open(const QString& port_name, qint32 baud, QString* err) {
  close();
  if (port_name.trimmed().isEmpty()) {
    if (err) *err = "no serial port named";
    return false;
  }
  port_name_ = port_name.trimmed();
  baud_ = baud;

  port_ = new QSerialPort(this);
  port_->setPortName(port_name_);
  port_->setBaudRate(baud_);
  port_->setDataBits(QSerialPort::Data8);
  port_->setParity(QSerialPort::NoParity);
  port_->setStopBits(QSerialPort::OneStop);
  // No flow control: neither the JuxiTech module nor any of the USB-serial
  // bridges in this rig wire RTS/CTS, and asking for hardware flow control on a
  // 3-wire cable is how a port opens and then never delivers a byte.
  port_->setFlowControl(QSerialPort::NoFlowControl);

  if (!port_->open(QIODevice::ReadWrite)) {
    if (err) {
      *err = QString("%1: %2").arg(port_name_, port_->errorString());
    }
    delete port_;
    port_ = nullptr;
    return false;
  }
  // A port that has been sitting open in somebody else's terminal can hand us a
  // kernel buffer full of stale bytes on the first read. Those have an arrival
  // time of "whenever they were actually received", which we do not know, and
  // stamping them now would hand the driver's de-burst stamper a fabricated
  // burst. Drop them.
  port_->clear(QSerialPort::AllDirections);

  connect(port_, &QSerialPort::readyRead, this, &SerialReader::onReadyRead);
  connect(port_, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
    if (e == QSerialPort::NoError) return;
    // TimeoutError is only ever raised by the waitFor* calls and is not fatal;
    // everything else on an already-open port means the handle is gone or
    // unusable (an unplugged CH340 raises ResourceError on Linux/Windows and
    // PermissionError on macOS, where the /dev node disappears underneath us).
    if (e == QSerialPort::TimeoutError) return;
    handleFatalError(port_ ? port_->errorString() : QString("serial error"));
  });

  // The standalone fixture (Part 2). The engine's .lscan records the SAME bytes
  // as kImuSerialRaw, but a .lscan needs a session and a lidar to exist; this
  // file is written whenever the port is open, which includes the case that
  // matters most — the operator plugged the module in and nothing else worked.
  FieldLog::openImuDump(port_name_, baud_);
  Q_EMIT logLine(QString("serial port %1 open at %2 8N1").arg(port_name_).arg(baud_));
  return true;
}

void SerialReader::close() {
  if (!port_) return;
  FieldLog::closeImuDump(QString("port %1 closed").arg(port_name_));
  // Disconnect first: closing a port whose readyRead is still queued would
  // re-enter onReadyRead() against a half-closed handle.
  port_->disconnect(this);
  if (port_->isOpen()) port_->close();
  port_->deleteLater();
  port_ = nullptr;
  push_failing_ = false;
}

void SerialReader::setTarget(EngineHost* host, scanengine::DeviceId id) {
  host_ = host;
  device_ = id;
  push_failing_ = false;
  last_push_error_.clear();
}

void SerialReader::clearTarget() {
  host_ = nullptr;
  device_ = scanengine::kInvalidDeviceId;
}

void SerialReader::onReadyRead() {
  if (!port_) return;
  // Stamp ONCE, before the read, in the same clock every other arrival in the
  // engine is stamped in (timesync/clock.h SteadyClock). Everything in this
  // slot belongs to one arrival event as far as the driver's stamper is
  // concerned; taking a fresh stamp per chunk would invent sub-millisecond
  // structure the wire does not have.
  const qint64 t_ns = EngineHost::engineNowNs();

  buf_ = port_->readAll();
  if (buf_.isEmpty()) return;
  bytes_read_ += quint64(buf_.size());
  ++reads_served_;

  // TEE, before anything can reject it. One record per read() slice with its
  // arrival stamp is exactly capture_serial_imu.py's model, so the fixture
  // reproduces the real burst pattern rather than an idealised stream — and it
  // is written even when the push below fails, which is the case a fixture is
  // most wanted for.
  FieldLog::imuBytes(buf_.constData(), std::size_t(buf_.size()));

  if (!host_ || device_ == scanengine::kInvalidDeviceId) {
    bytes_discarded_ += quint64(buf_.size());
    return;
  }

  QString err;
  if (host_->pushBytes(device_, buf_, t_ns, &err)) {
    if (push_failing_) {
      push_failing_ = false;
      last_push_error_.clear();
      Q_EMIT logLine(QString("serial %1: pushes to device #%2 are landing again")
                         .arg(port_name_)
                         .arg(device_));
    }
    return;
  }
  last_push_error_ = err;
  if (!push_failing_) {
    push_failing_ = true;
    Q_EMIT logLine(QString("serial %1: engine refused %2 bytes for device #%3 — %4 "
                           "(logged once until it recovers)")
                       .arg(port_name_)
                       .arg(buf_.size())
                       .arg(device_)
                       .arg(err));
  }
}

scanengine::ScanError SerialReader::write(const std::uint8_t* data, std::size_t len) {
  if (!port_ || !port_->isOpen()) return scanengine::ScanError::kNotSupported;
  if (data == nullptr || len == 0) return scanengine::ScanError::kInvalidArgument;

  const qint64 n = port_->write(reinterpret_cast<const char*>(data), qint64(len));
  if (n != qint64(len)) {
    Q_EMIT logLine(QString("serial %1: write of %2 bytes returned %3 — %4")
                       .arg(port_name_)
                       .arg(len)
                       .arg(n)
                       .arg(port_->errorString()));
    return scanengine::ScanError::kIoError;
  }
  // Synchronous on purpose (see the header): the driver reads a kOk here as
  // "the command is on the wire", and a queued write that never flushes would
  // make it believe it configured a module it did not reach. 250 ms is ~2500x
  // the 7-byte frame's time on the wire at 115200.
  if (!port_->waitForBytesWritten(250)) {
    Q_EMIT logLine(QString("serial %1: write of %2 bytes did not flush within 250 ms — %3")
                       .arg(port_name_)
                       .arg(len)
                       .arg(port_->errorString()));
    return scanengine::ScanError::kIoError;
  }
  bytes_written_ += quint64(len);
  return scanengine::ScanError::kOk;
}

void SerialReader::handleFatalError(const QString& what) {
  const QString name = port_name_;
  close();
  Q_EMIT logLine(QString("serial %1 lost: %2").arg(name, what));
  Q_EMIT disconnected(what);
}

}  // namespace lidarscan
