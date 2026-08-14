// usb_serial_source.h — app-pushed serial bytes (CH340 → D6).
//
// Owns no OS handle and spawns no thread. push() runs the whole downstream
// chain (parser → points → page store → events) on the CALLER'S thread, so
// the app controls where that work happens: the Android JNI reader thread,
// or the Qt readyRead() slot. That is deliberate — it keeps the engine free
// of platform serial code (Tech Spec §3.1's per-OS matrix lives entirely in
// the apps) and makes replay bit-identical to capture (engine_cli --replay
// pushes file bytes through this same class).
//
// Owner: A1 / A2 (reconnect, health, fault states).
#ifndef SCANENGINE_TRANSPORT_USB_SERIAL_SOURCE_H
#define SCANENGINE_TRANSPORT_USB_SERIAL_SOURCE_H

#include <mutex>

#include "scanengine/transport/byte_source.h"

namespace scanengine {

struct UsbSerialConfig {
  // Informational: the engine never opens the port, but drivers log it and
  // .lscan records it in the manifest.
  const char* port_name = "";
  std::uint32_t baud = 230400;  // D6: 230400 8N1 (Tech Spec §2.1)

  SerialWriteFn write_fn = nullptr;  // null → write() returns kNotSupported
  void* write_user_data = nullptr;
};

class UsbSerialSource final : public ByteSource {
 public:
  explicit UsbSerialSource(const UsbSerialConfig& cfg);
  ~UsbSerialSource() override;

  const char* name() const override { return "usb-serial"; }
  Status start() override;
  Status stop() override;
  bool running() const override;
  void set_sink(ByteSink sink) override;
  Status write(ByteSpan bytes) override;
  TransportStats stats() const override;

  // App → engine. `t_arrival` of {0} means "stamp now" (the normal case:
  // the app has no better stamp than arrival). Replay passes an explicit
  // stamp so a recorded session reproduces exactly.
  Status push(ByteSpan bytes, TimePoint t_arrival = TimePoint{0});

  const UsbSerialConfig& config() const { return cfg_; }

 private:
  UsbSerialConfig cfg_;
  mutable std::mutex m_;
  ByteSink sink_;
  bool running_ = false;
  TransportStats stats_{};
};

}  // namespace scanengine

#endif  // SCANENGINE_TRANSPORT_USB_SERIAL_SOURCE_H
