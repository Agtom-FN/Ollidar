// byte_source.h — how bytes get into the engine.
//
// Tech Spec §3 key rule 1: "UIs pass opaque buffers in ... and get render
// handles + status events out". The engine deliberately does NOT own serial
// ports or sockets on the app side:
//   • Android cannot open /dev/ttyUSB* — usb-serial-for-android reads in
//     Kotlin and hands buffers down through JNI.
//   • The Qt desktop already owns a QSerialPort with the OS's port naming,
//     permissions and hot-plug signals.
// So the D6 path is a PUSH source: the app calls push() with whatever chunk
// sizes the OS gave it (the parser reassembles across arbitrary boundaries —
// proven in S1), and the engine calls back through a write function when it
// needs to send a command (start/stop: AA 55 F0 0F / AA 55 F5 0A).
//
// The Mid-360 path is different — SDK2 owns its own sockets — and lands in
// transport/udp_source.h + drivers/mid360 (A3).
//
// Owner: A1 (interfaces, UsbSerialSource) / A2 (D6 reconnect + fault states)
// / A3 (UdpSource).
#ifndef SCANENGINE_TRANSPORT_BYTE_SOURCE_H
#define SCANENGINE_TRANSPORT_BYTE_SOURCE_H

#include <cstdint>
#include <functional>

#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/timesync/clock.h"

namespace scanengine {

// Called once per arriving chunk, on whichever thread delivered it. `bytes`
// is only valid for the duration of the call.
using ByteSink = std::function<void(ByteSpan bytes, TimePoint t_arrival)>;

// Host → device. Implemented by the app (JNI shim / QSerialPort wrapper).
// Must be callable from the thread that calls into the engine; returns a
// ScanError so a failed write surfaces as a device fault rather than a
// silently ignored command.
using SerialWriteFn = ScanError (*)(const std::uint8_t* data, std::size_t len, void* user_data);

struct TransportStats {
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint64_t chunks_in = 0;
  std::uint64_t write_errors = 0;
  std::int64_t t_last_rx_ns = 0;
};

class ByteSource {
 public:
  virtual ~ByteSource() = default;

  virtual const char* name() const = 0;
  virtual Status start() = 0;
  virtual Status stop() = 0;
  virtual bool running() const = 0;

  // Install the consumer (a driver). Must be set before start().
  virtual void set_sink(ByteSink sink) = 0;

  // Host → device. kNotSupported on a read-only source.
  virtual Status write(ByteSpan bytes) = 0;

  virtual TransportStats stats() const = 0;
};

// Datagram sources (Mid-360 UDP, and later the Phase-2 WiFi bridges). Split
// from ByteSource because packet boundaries are meaningful: a datagram is
// never reassembled, and loss is detected per packet (S2-sim: Mid-360
// udp_cnt free-runs and frame_cnt stays 0 — loss detection in A3 must use
// the free-running model, contrary to the published table).
using PacketSink = std::function<void(ByteSpan datagram, TimePoint t_arrival)>;

class PacketSource {
 public:
  virtual ~PacketSource() = default;

  virtual const char* name() const = 0;
  virtual Status start() = 0;
  virtual Status stop() = 0;
  virtual bool running() const = 0;
  virtual void set_sink(PacketSink sink) = 0;
  virtual Status send(ByteSpan datagram) = 0;
  virtual TransportStats stats() const = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_TRANSPORT_BYTE_SOURCE_H
