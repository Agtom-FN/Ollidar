// SerialReader.h — the desktop's QSerialPort, on the APP side of the engine's
// hard line (Phase 6 of the Mid-70 + serial-IMU plan).
//
// WHY THIS EXISTS AT ALL
//
// engine/include/scanengine/transport/byte_source.h states the doctrine
// plainly: the engine never opens a serial device. Android cannot open
// /dev/ttyUSB* without the USB-host permission dance, Windows and macOS name
// ports differently, and every per-OS serial quirk in Tech Spec §3.1 lives in
// the apps. So the app owns the handle, reads it, and pushes the bytes in
// through Engine::push_serial_bytes(); the engine owns everything after the
// bytes. This class is the desktop half of that sentence and nothing else — it
// contains no framing, no checksum, no protocol knowledge of any kind. It does
// not know whether the thing on the other end is a JuxiTech IMU module or a
// COIN-D6, and it must not learn.
//
// THREAD: the GUI thread, deliberately.
//
// The A18 module is 100 Hz x 23-byte frames = ~2.3 kB/s, and QSerialPort's
// readyRead() on the GUI thread services that with several orders of magnitude
// to spare. A worker thread would buy nothing and would cost the one thing that
// matters here: DESIGN.md §2's rule that a single Driver is pushed from one
// thread at a time. On the GUI thread that rule holds by construction —
// readyRead(), the arm/disarm path in CaptureWindow, and the driver's own
// start()-time write all run on the same thread, in order. (If a future device
// on this seam is fast enough to need its own thread, moveToThread() on this
// object is the change, and the Driver contract is what has to be re-argued.)
//
// THE WRITE DIRECTION
//
// ImuSerialDriver sends exactly ONE thing, once, from start(): the report-rate
// frame `7E 23 07 60 <hz> 5F <sum8>`. It reaches the port through
// UsbSerialConfig::write_fn, a C function pointer plus a void* — see
// EngineHost::addImuSerial(), which owns the trampoline and the struct it
// points at. write() below is the far end of that bridge. It is synchronous
// (waitForBytesWritten) because the driver treats a failed write as a device
// fault and a queued-but-unsent command would report success it has not earned.
//
// HONESTY ABOUT DISCONNECTS
//
// A USB-serial adapter that is unplugged mid-capture does not produce a clean
// EOF; QSerialPort raises ResourceError (or PermissionError on macOS when the
// /dev node vanishes under an open handle). Every such error closes the port
// and emits disconnected() with the OS's own text. Nothing here retries
// silently: a capture that lost its IMU has to say so, because the alternative
// is LIO quietly running on stale gyro.
//
// Owner: Phase 6 (desktop) of the Mid-70 + serial-IMU plan.
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstddef>
#include <cstdint>

#include "scanengine/core/error.h"
#include "scanengine/core/types.h"

class QSerialPort;

namespace lidarscan {

class EngineHost;

class SerialReader : public QObject {
  Q_OBJECT
 public:
  explicit SerialReader(QObject* parent = nullptr);
  ~SerialReader() override;

  // 8N1, no flow control, at `baud`. False + *err on failure — a busy port
  // (screen(1), the vendor tool, a second LidarScan) and a port that does not
  // exist are BOTH normal operator mistakes and both get the OS's own words.
  bool open(const QString& port_name, qint32 baud, QString* err);
  void close();
  bool isOpen() const;

  const QString& portName() const { return port_name_; }
  qint32 baud() const { return baud_; }

  // Where read bytes go. Both halves are set together because a reader with a
  // host and no device id would push into kInvalidDeviceId, which the engine
  // rejects once per readyRead() — a silent 100 Hz error loop. Safe to call
  // before or after open(); bytes that arrive with no target are DROPPED and
  // counted in bytesDiscarded(), never buffered, because a buffer here would
  // be a second copy of the arrival clock.
  void setTarget(EngineHost* host, scanengine::DeviceId id);
  void clearTarget();
  scanengine::DeviceId target() const { return device_; }

  // The engine's SerialWriteFn far end (see the file header). Synchronous.
  // kNotSupported if the port is not open — which is what the driver should
  // hear, because "I could not send the rate command" is exactly true then.
  scanengine::ScanError write(const std::uint8_t* data, std::size_t len);

  quint64 bytesRead() const { return bytes_read_; }
  quint64 readsServed() const { return reads_served_; }
  quint64 bytesDiscarded() const { return bytes_discarded_; }
  quint64 bytesWritten() const { return bytes_written_; }
  // Non-empty after a push_serial_bytes() rejection: the last one seen, so the
  // panel can say WHY the bytes are not reaching the driver instead of showing
  // a port that is open and a device that is silent.
  const QString& lastPushError() const { return last_push_error_; }

 Q_SIGNALS:
  // Everything this class does that an operator would want to read. Routed to
  // CaptureWindow::log(), which also writes stderr for headless runs.
  void logLine(const QString& line);
  // The port went away or errored fatally; it is already closed by the time
  // this fires. `why` is the OS's text, not a paraphrase.
  void disconnected(const QString& why);

 private:
  void onReadyRead();
  void handleFatalError(const QString& what);

  QSerialPort* port_ = nullptr;
  QString port_name_;
  qint32 baud_ = 115200;

  EngineHost* host_ = nullptr;
  scanengine::DeviceId device_ = scanengine::kInvalidDeviceId;

  // Reused across readyRead() calls so a 100 Hz read path does not allocate.
  QByteArray buf_;

  quint64 bytes_read_ = 0;
  quint64 reads_served_ = 0;
  quint64 bytes_discarded_ = 0;
  quint64 bytes_written_ = 0;
  QString last_push_error_;
  // Rate-limits the push-failure log to once per failure RUN: a driver that
  // rejects one push rejects the next 100 too, and 100 identical lines a second
  // buries the one line that matters.
  bool push_failing_ = false;
};

}  // namespace lidarscan
