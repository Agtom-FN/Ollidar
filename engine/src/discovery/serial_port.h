// serial_port.h — the ONE place the engine opens a serial port itself.
//
// The engine's standing rule (transport/usb_serial_source.h, Tech Spec §3.1)
// is that platform serial code lives in the APPS and the engine only ever
// receives pushed bytes. A16 is the deliberate exception, and it is a narrow
// one: you cannot identify a device by its protocol without reading its
// bytes, and asking every app to reimplement a baud sweep is how the three
// apps end up disagreeing about what a UM982 looks like. So: this header,
// used by src/discovery/serial_probe.cpp and nothing else, opens a port
// READ-MOSTLY for at most a couple of seconds and closes it.
//
// It is deliberately NOT a ByteSource and deliberately not public — a
// capture session still goes through UsbSerialSource and the app's own port
// handling.
//
// INTERNAL to src/discovery/.
#ifndef SCANENGINE_SRC_DISCOVERY_SERIAL_PORT_H
#define SCANENGINE_SRC_DISCOVERY_SERIAL_PORT_H

#include <cstdint>
#include <string>

namespace scanengine {
namespace discovery_serial {

enum class OpenResult {
  kOk = 0,
  kBusy,       // someone else holds it — SKIP IT SILENTLY (the owner's rule)
  kNoAccess,   // no permission (dialout group, macOS TCC) — skip, log once
  kNotFound,   // vanished between enumeration and open
  kError,      // anything else
};

const char* to_string(OpenResult r);

class SerialPort {
 public:
  SerialPort() = default;
  ~SerialPort();
  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;

  // 8N1, raw, no flow control, non-blocking reads. `baud` may be any rate the
  // platform can express; kError if it cannot (Linux needs a B-constant,
  // macOS takes arbitrary rates).
  OpenResult Open(const std::string& path, std::uint32_t baud);
  void Close();
  bool is_open() const { return open_; }

  // Wait up to `timeout_ms` for bytes and read what is there.
  // Returns the count, 0 on timeout, -1 on error/disconnect.
  int Read(std::uint8_t* buf, std::size_t cap, int timeout_ms);

  // Best-effort, whole-buffer write. Only ever called with the D6's 4-byte
  // start/stop commands — see the write policy in discovery.h.
  bool Write(const std::uint8_t* data, std::size_t n);

  // Discard anything the driver buffered before we got here, so a probe's
  // dwell time measures the LIVE stream rather than a stale backlog.
  void FlushInput();

 private:
  bool open_ = false;
#if defined(_WIN32)
  void* handle_ = nullptr;  // HANDLE, kept void* so no consumer needs windows.h
#else
  int fd_ = -1;
#endif
};

}  // namespace discovery_serial
}  // namespace scanengine

#endif  // SCANENGINE_SRC_DISCOVERY_SERIAL_PORT_H
