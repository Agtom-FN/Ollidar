// stl27l_driver.h — LDROBOT STL-27L driver: the ITEM 119 parser behind the
// Driver interface.
//
// A deliberate mirror of drivers/d6/d6_driver.h. The two sensors are the same
// KIND of thing — a 2-D spinning rangefinder on a USB-serial link, swept along
// a trajectory by A8's pushbroom assembler — so they present the same seams:
// bytes in through push_bytes(), a polar profile out through
// Stl27lProfileSink, sensor-frame points into the PageStore, status onto the
// EventBus.
//
// COORDINATE FRAME. There is exactly ONE fan-frame convention in this engine
// and it lives in drivers/d6/d6_fan.h:
//     x = -d*sin(theta), y = d*cos(theta), z = 0
//     (+y = the 0-degree beam / zero mark, +z = the spin axis out of the BASE)
// This driver CALLS d6::fan_point(). It does not restate the formula and it
// does not define a second convention — ROUND 9 item 34 was a whole round
// spent finding a mirrored cloud caused by exactly that, and a second sensor
// with its own private copy of the geometry is how that bug comes back.
//
// UNVERIFIED, AND IT MATTERS HERE. Whether the STL-27L's reported angle
// increases in the same rotational sense as the COIN-D6's is PROTOCOL-DERIVED,
// not observed — no STL-27L hardware exists on this project. If it turns out
// to sweep the other way, the fix is one line (`Stl27lConfig::invert_angle`,
// below) and NOT a second fan formula. The falsifiable bench test is
// docs/bench's chirality check: scan a room with an asymmetric feature and
// confirm it does not come out mirrored — tests/test_round9_chirality.cpp is
// the shape of it, and tests/test_stl27l.cpp asserts only that this driver
// agrees with d6::fan_point(), which is a consistency claim and not a
// hardware claim.
//
// NO COMMAND CHANNEL. The LD-series free-runs the moment it is powered: there
// is no start/stop handshake to send and no ACK to wait for, which is why this
// header has no `commands.h` sibling and why the restart policy the D6 carries
// is absent. A stalled STL-27L is reported (kStalled -> DeviceState::kDegraded)
// and left for the app to power-cycle or re-open, exactly as the D6 behaves
// when its transport has no write function.
//
// Owner: ITEM 119.
#ifndef SCANENGINE_DRIVERS_STL27L_STL27L_DRIVER_H
#define SCANENGINE_DRIVERS_STL27L_STL27L_DRIVER_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "scanengine/drivers/driver.h"
#include "scanengine/drivers/stl27l/stl27l_parser.h"
#include "scanengine/transport/usb_serial_source.h"

namespace scanengine {

// The A8 pushbroom seam, in the POLAR form the wire carries so the frame
// convention is applied in exactly one place (the assembler). Signature is
// IDENTICAL to D6ProfileSink on purpose: core/engine.cpp routes both sensors
// through the same shim, and a divergence here would fork that path.
// `high_reflectivity` is always 0 — the LD protocol has no such flag — and is
// kept so the two sinks are interchangeable.
using Stl27lProfileSink = void (*)(float angle_deg, float range_m, std::uint8_t intensity,
                                   std::uint8_t high_reflectivity, std::int64_t t_engine_ns,
                                   void* user_data);

struct Stl27lConfig {
  UsbSerialConfig serial{};       // baud defaults to 921600 in the constructor
  stl27l::Config parser{};

  // Health thresholds. The LD CRC is 8 bits, so a corrupt packet slips through
  // 1 time in 256 — the bar is on the OBSERVED pass rate, not on individual
  // packets, and below it the device is kDegraded (data still usable), never
  // kFault.
  double min_crc_pass_rate = 0.99;
  std::uint64_t health_min_packets = 200;  // before rating the pass rate

  // Points are batched into the PageStore rather than appended one at a time.
  std::uint32_t max_batch_points = 4096;

  bool drop_zero_range_points = true;  // no-return samples are not geometry

  // Reverse the sense of the reported angle before it reaches the fan frame:
  //   theta' = 360 - theta
  // The ONE knob for "this unit sweeps the other way". See the file header:
  // it exists so that a hardware surprise is a config change and not a second
  // copy of d6_fan.h's formula. OFF until hardware says otherwise.
  bool invert_angle = false;

  // --- per-point arrival time inside one byte chunk (the D6's ROUND 7 fix) --
  //
  // A UART delivers bytes at a known constant rate, so byte position inside a
  // chunk IS time. on_bytes() feeds the parser in slices of this many bytes,
  // each stamped with its own back-dated arrival time, so a point carries the
  // time of the slice that completed its packet rather than the time the whole
  // (possibly very large) read finished.
  //
  // 256 bytes is ~2.8 ms at 921600 8N1 — the same wall-clock granularity the
  // D6 gets from 64 bytes at 230400, which is the number that matters — and is
  // a little over five STL-27L packets. 0 restores one stamp per chunk.
  std::uint32_t time_slice_bytes = 256;

  // Bits per byte on the wire: 8N1 = 1 start + 8 data + 1 stop.
  std::uint32_t wire_bits_per_byte = 10;

  // A8 pushbroom seam. The Engine installs one when SessionConfig::pushbroom
  // is on; an app driving the assembler itself may install its own.
  Stl27lProfileSink profile_sink = nullptr;
  void* profile_sink_user_data = nullptr;

  // --- stall watchdog ------------------------------------------------------
  //
  // Wall-clock timeouts, evaluated opportunistically (the engine owns no
  // threads): every push_bytes() and every state()/health()/snapshot() poll
  // re-checks elapsed time against the last byte / last valid packet. Both are
  // tighter than the D6's because the STL-27L talks 4x faster and never goes
  // quiet on purpose — it has no idle mode.
  std::int64_t silent_stall_timeout_ns = 1'000'000'000;   // no bytes at all
  std::int64_t garbage_stall_timeout_ns = 2'000'000'000;  // bytes, no valid packet

  // Startup grace, measured from start(): the motor has to come up to speed
  // and the first frames out of a cold unit are not trustworthy.
  std::int64_t startup_grace_ns = 2'000'000'000;
};

// Internal state machine, mapped down onto the seven public DeviceState values
// for state()/health() and for every event this driver publishes:
//
//   Stl27lPhase        -> DeviceState
//   kIdle              -> kIdle
//   kStarting          -> kStarting
//   kStreaming         -> kStreaming
//   kDegradedCrc       -> kDegraded   (CRC pass rate below threshold)
//   kStalled           -> kDegraded   (watchdog fired: silent or garbage)
//   kStopping          -> kStopping
//   kFault             -> kFault
//
// There is no kRestarting: see the file header — no command channel, nothing
// to retry with.
enum class Stl27lPhase : std::uint8_t {
  kIdle = 0,
  kStarting = 1,
  kStreaming = 2,
  kDegradedCrc = 3,
  kStalled = 4,
  kStopping = 5,
  kFault = 6,
};

const char* to_string(Stl27lPhase p) noexcept;

// What the watchdog last observed. "Device went quiet" and "device is talking
// but nothing decodes" are different faults with different operator actions
// (check power / check the baud rate).
enum class Stl27lStallKind : std::uint8_t {
  kNone = 0,
  kSilent = 1,   // no bytes at all within silent_stall_timeout_ns
  kGarbage = 2,  // bytes arriving, but no valid packet within garbage_stall_timeout_ns
};

const char* to_string(Stl27lStallKind k) noexcept;

// The STL-27L-specific health surface: what the core DeviceHealth cannot
// carry. health() still returns the core struct for engine-wide consumers.
struct Stl27lHealthSnapshot {
  Stl27lPhase phase = Stl27lPhase::kIdle;
  Stl27lStallKind stall = Stl27lStallKind::kNone;

  double points_per_sec = 0.0;
  double rotation_hz = 0.0;
  double crc_pass_rate = 0.0;
  double sample_hz_est = 0.0;
  std::uint16_t speed_dps = 0;
  std::uint64_t resyncs = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t packets_bad_crc = 0;
  std::uint64_t packets_malformed = 0;

  std::int64_t t_last_bytes_ns = 0;
  std::int64_t t_last_valid_packet_ns = 0;
};

class Stl27lDriver final : public Driver {
 public:
  Stl27lDriver(DeviceId id, const Stl27lConfig& cfg, const DriverContext& ctx);
  ~Stl27lDriver() override;

  const char* name() const override { return "stl27l"; }
  DeviceKind kind() const override { return DeviceKind::kStl27l; }
  DeviceId id() const override { return id_; }

  Status start() override;
  Status stop() override;
  DeviceState state() const override;
  DeviceHealth health() const override;
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;

  // Diagnostics used by tests and engine_cli --replay.
  stl27l::Stats parser_stats() const;
  UsbSerialSource& transport() { return *serial_; }

  // Cheap; safe from any thread. Triggers the same opportunistic watchdog
  // check as state()/health() before returning.
  Stl27lHealthSnapshot snapshot() const;

  // Re-evaluate the stall watchdog against `now`. Called automatically from
  // on_bytes() (with the arriving bytes' own stamp) and from
  // state()/health()/snapshot() (with DriverContext::clock()). Exposed so
  // tests and engine_cli can drive it with synthetic time.
  void check_watchdog(TimePoint now);

 private:
  void on_bytes(ByteSpan bytes, TimePoint t);
  void on_point(const stl27l::Point& p);
  void flush_batch(std::int64_t t_ns);
  void feed_time_sliced(ByteSpan bytes, std::int64_t t_chunk_end_ns);
  std::int64_t byte_period_ns() const;
  void set_phase(Stl27lPhase next, ScanError err);
  TimePoint current_time() const;
  Stl27lPhase get_phase() const;

  DeviceId id_;
  Stl27lConfig cfg_;
  DriverContext ctx_;

  std::unique_ptr<UsbSerialSource> serial_;
  stl27l::Parser parser_;

  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kDisconnected;
  Stl27lPhase phase_ = Stl27lPhase::kIdle;
  Stl27lStallKind stall_ = Stl27lStallKind::kNone;
  ScanError last_error_ = ScanError::kOk;
  std::vector<PointVertex> batch_;
  std::int64_t t_current_ns_ = 0;
  std::uint64_t points_out_ = 0;
  std::uint64_t rotations_seen_ = 0;
  std::uint32_t points_in_rotation_ = 0;
  std::uint64_t drops_ = 0;

  // Watchdog bookkeeping (m_-protected).
  std::int64_t t_start_ns_ = 0;
  std::int64_t t_last_bytes_ns_ = 0;
  std::int64_t t_last_valid_packet_ns_ = 0;
  std::uint64_t last_packets_ok_seen_ = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_STL27L_STL27L_DRIVER_H
