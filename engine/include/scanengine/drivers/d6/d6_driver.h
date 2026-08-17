// d6_driver.h — COIN-D6 driver: S1's parser behind the Driver interface.
//
// The parser itself (d6_parser.h/.cpp, commands.h) is the finished S1 spike
// artefact, copied in unmodified except for include paths; the S1 tree stays
// as the historical record. This file is the engine integration: transport
// in, ACK handling, health model, points into the PageStore, status onto the
// EventBus.
//
// Coordinate frame produced here is the SENSOR frame of a single 2D sweep:
//   x = d·sin(θ), y = d·cos(θ), z = 0   (θ = 0 is +y, matching the S1 tools)
// The D6 is mounted VERTICALLY and the 3-D cloud comes from sweeping that
// profile along a trajectory — that assembly is A8 (pushbroom), which will
// replace this driver's page writes with trajectory-transformed points.
// Until then these points are exactly what the S1 polar plot showed, which
// is what makes the live "is the sensor seeing anything" view work in M1.
//
// --- A2 hardening -----------------------------------------------------------
//
// The public core::DeviceState (core/types.h, owned by A1) only has seven
// values and is not this task's to extend. D6Driver instead runs a richer
// internal state machine, D6Phase, and maps it down onto DeviceState for
// state()/health() and for every event this driver publishes:
//
//   D6Phase              -> DeviceState
//   kIdle                -> kIdle
//   kStarting             -> kStarting
//   kStreaming            -> kStreaming
//   kDegradedChecksum     -> kDegraded   (checksum pass rate below threshold)
//   kStalled              -> kDegraded   (watchdog fired: silent or garbage)
//   kRestarting           -> kStarting   (actively retrying start/stop)
//   kStopping             -> kStopping
//   kFault                -> kFault      (restart budget exhausted, or a
//                                          device error ACK — see commands.h)
//
// D6HealthSnapshot (below) exposes the full D6Phase plus both checksum
// variant counters and the restart/backoff state; health() keeps returning
// the core DeviceHealth for engine-wide consumers.
//
// Watchdog note: the engine owns no threads (DESIGN.md §2), so the stall
// watchdog cannot run on a timer. It is instead re-evaluated opportunistically
// against wall-clock time every time this driver does *anything* observable:
// on_bytes() (bytes actually arrived) and state()/health()/snapshot() (an app
// polling for status — S1's d6cli already does this once a second). A device
// that goes completely silent is only caught by the latter path, so an app
// that never polls health() will not see a silent-stall transition; that
// matches "polled by app UIs" in core/types.h's DeviceHealth doc. Tests and
// engine_cli can also drive it directly and deterministically with
// check_watchdog(TimePoint), independent of DriverContext::clock.
//
// Owner: A1 (this integration) / A2 (reconnect, speed-adjust filtering,
// fault states) / A8 (pushbroom assembly replaces the point transform).
#ifndef SCANENGINE_DRIVERS_D6_D6_DRIVER_H
#define SCANENGINE_DRIVERS_D6_D6_DRIVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "scanengine/drivers/d6/d6_parser.h"
#include "scanengine/drivers/driver.h"
#include "scanengine/transport/usb_serial_source.h"

namespace scanengine {

// Optional zero-copy profile seam for A8's pushbroom assembler (requested in
// docs/A8-pushbroom.md §7.2 item 3). One call per decoded return, in the POLAR
// form the wire carries, so the sensor-frame convention is applied in exactly
// one place — the assembler — instead of being applied here and un-applied
// there. `t_engine_ns` is the carrying packet's arrival stamp (the D6 has no
// device clock, so arrival IS engine time; see docs/A4-timesync.md).
//
// Called on the byte-pushing thread with no driver lock held: it must be quick
// and must not re-enter the driver. Unset by default — the live preview path
// (PageStore, sensor frame) is unchanged whether this is set or not.
using D6ProfileSink = void (*)(float angle_deg, float range_m, std::uint8_t intensity,
                               std::uint8_t high_reflectivity, std::int64_t t_engine_ns,
                               void* user_data);

struct D6Config {
  UsbSerialConfig serial{};
  d6::Config parser{};

  // Send AA 55 F0 0F on start() / AA 55 F5 0A on stop() when the transport
  // has a write function. Off when the app drives the device itself.
  bool send_start_stop_commands = true;

  // Wait for the start ACK before declaring kStreaming. When false (or when
  // no write function exists) the first decoded packet promotes the state.
  bool require_start_ack = false;

  // Health thresholds. The S1 exit criterion is a checksum pass rate above
  // 99.5%; below it the device is kDegraded, not kFault — data is still
  // usable. Also used as the minimum sample size for the checksum-variant
  // verdict (checksum_verdict()) below.
  double min_checksum_pass_rate = 0.995;
  std::uint64_t health_min_packets = 200;  // before rating pass rate / verdict

  // Points are batched into the PageStore rather than appended one at a
  // time: one 40-sample packet per append is ~640 B, which is the granularity
  // the S3 renderer measured at 0.11 ms p95 for 200k pts/s.
  std::uint32_t max_batch_points = 4096;

  bool drop_zero_range_points = true;  // no-return samples are not geometry

  // --- ROUND 7: per-point arrival time inside one byte chunk ---------------
  //
  // The A8 assembler interpolates a pose PER POINT and always did
  // (pushbroom_assembler.cpp's resolve_()). What it was being handed was not
  // per-point time: `Parser::feed(data, n, t_rx_ns)` stamps EVERY sample it
  // decodes out of that call with the one `t_rx_ns` of the whole chunk, and
  // this driver then passed a single `t_current_ns_` to `profile_sink` on top
  // of that. Both headers here and in pushbroom_assembler.h claimed per-point
  // time; neither the parser nor this driver ever produced it.
  //
  // On a phone that is not a rounding error. `D6SerialConnection` reads into a
  // 4096-byte buffer, and 4096 bytes at 230400 8N1 is **178 ms of wire time**
  // — nearly two full 10 Hz revolutions — collapsed onto one instant. Every
  // point in that chunk resolves against ONE pose, so a walk at 1 m/s lays
  // each chunk down as a rigid slab offset from its neighbour by up to 18 cm:
  // walls come out shingled rather than flat, which is the owner's "not a
  // stable scan with straight walls" and his "sections", exactly.
  //
  // The fix needs no new clock and no ABI: a UART delivers bytes at a KNOWN
  // constant rate, so byte position inside the chunk IS time. `on_bytes` feeds
  // the parser in slices of this many bytes, each stamped with its own
  // back-dated arrival time (`t_chunk_end - remaining_bytes * byte_period`),
  // and each point therefore carries the time of the slice that completed its
  // packet, to within one slice.
  //
  // 64 bytes is ~2.8 ms at 230400 (~2.8 mm of rig travel at 1 m/s, an order of
  // magnitude under the D6's own range noise) and is under two D6 packets, so
  // the slicing costs a handful of extra `feed()` calls per chunk and nothing
  // else — the parser is a streaming parser and has always buffered across
  // calls. 0 restores the pre-ROUND-7 behaviour (one stamp per chunk).
  std::uint32_t time_slice_bytes = 64;

  // Bits per byte on the wire, for the back-dating above: 8N1 = 1 start + 8
  // data + 1 stop. Named rather than hard-coded so a future 8E1/7-bit variant
  // does not silently mis-date every point.
  std::uint32_t wire_bits_per_byte = 10;

  // A8 pushbroom seam (see D6ProfileSink above). The Engine installs one of
  // these when SessionConfig::pushbroom is on; an app driving the assembler
  // itself may install its own.
  D6ProfileSink profile_sink = nullptr;
  void* profile_sink_user_data = nullptr;

  // --- A2: stall watchdog + restart policy ---------------------------------
  //
  // Wall-clock timeouts. Evaluated opportunistically (see the file header):
  // every push_bytes() call and every state()/health()/snapshot() poll
  // re-checks elapsed time against the last byte / last valid packet.
  std::int64_t silent_stall_timeout_ns = 1'500'000'000;   // no bytes at all
  std::int64_t garbage_stall_timeout_ns = 3'000'000'000;  // bytes, no valid packet

  // Startup grace period, measured from start() (and not extended by
  // restarts): the S1 spec's pre-lock 0xFE/0xFF speed-adjustment traffic,
  // and whatever silence goes with the device settling on its rotation
  // frequency, must not read as a fault. Neither watchdog is evaluated
  // until this elapses. REPORT.md's synthetic --noise stream is exactly
  // this traffic.
  std::int64_t startup_grace_ns = 2'500'000'000;

  // Restart policy: exponential backoff (base, doubling, capped at max)
  // between attempts, capped attempt count. Exhausting the cap is the one
  // D6 condition that reaches kFault by itself — checksum loss never does
  // (DESIGN.md §3 item 5) — because it means the command channel itself
  // stopped getting a response.
  std::uint32_t max_restart_attempts = 5;
  std::int64_t restart_backoff_base_ns = 2'000'000'000;
  std::int64_t restart_backoff_max_ns = 30'000'000'000;
};

// See the file header for the full mapping onto DeviceState.
enum class D6Phase : std::uint8_t {
  kIdle = 0,
  kStarting = 1,
  kStreaming = 2,
  kDegradedChecksum = 3,
  kStalled = 4,
  kRestarting = 5,
  kStopping = 6,
  kFault = 7,
};

const char* to_string(D6Phase p) noexcept;

// What the watchdog last observed. Distinguishes "device went quiet" (no
// bytes at all) from "device is talking but nothing decodes" (a garbage
// stream, or a false AA55 header stalling the parser — REPORT.md §7 item 6).
enum class D6StallKind : std::uint8_t {
  kNone = 0,
  kSilent = 1,   // no bytes at all within silent_stall_timeout_ns
  kGarbage = 2,  // bytes arriving, but no valid packet within garbage_stall_timeout_ns
};

const char* to_string(D6StallKind k) noexcept;

// Verdict on the S1 checksum-variant open question (REPORT.md §2 / §7 item
// 1): once enough packets have been observed, whichever counter —
// cs_ok_vendor / cs_ok_spec — actually tracks the accepted packets is the
// variant this device's firmware implements.
enum class D6ChecksumVerdict : std::uint8_t {
  kUndetermined = 0,  // fewer than health_min_packets observed yet
  kVendorConfirmed = 1,
  kSpecConfirmed = 2,
  kAmbiguous = 3,     // both track equally well — should not happen on real HW
};

const char* to_string(D6ChecksumVerdict v) noexcept;

// The D6-specific health surface: everything the core DeviceHealth does not
// carry. health() (the Driver interface) still returns the core struct for
// engine-wide consumers (device panels, the C ABI); this is for a D6-aware
// caller — tests, engine_cli, a future device-detail view — that wants the
// full picture.
struct D6HealthSnapshot {
  D6Phase phase = D6Phase::kIdle;
  D6StallKind stall = D6StallKind::kNone;

  double points_per_sec = 0.0;
  double rotation_hz = 0.0;
  double checksum_pass_rate = 0.0;
  std::uint64_t resyncs = 0;
  std::uint64_t bytes_in = 0;

  // Both checksum-variant acceptance counters (S1 REPORT.md §2); whichever
  // dominates on real hardware settles the open question.
  std::uint64_t cs_ok_vendor = 0;
  std::uint64_t cs_ok_spec = 0;
  d6::ChecksumVariant accepted_variant = d6::ChecksumVariant::kVendorSdk;
  D6ChecksumVerdict checksum_verdict = D6ChecksumVerdict::kUndetermined;

  std::uint32_t restart_attempts = 0;
  std::int64_t t_last_bytes_ns = 0;
  std::int64_t t_last_valid_packet_ns = 0;
};

class D6Driver final : public Driver {
 public:
  D6Driver(DeviceId id, const D6Config& cfg, const DriverContext& ctx);
  ~D6Driver() override;

  const char* name() const override { return "d6"; }
  DeviceKind kind() const override { return DeviceKind::kD6; }
  DeviceId id() const override { return id_; }

  Status start() override;
  Status stop() override;
  DeviceState state() const override;
  DeviceHealth health() const override;
  Status push_bytes(ByteSpan bytes, TimePoint t_arrival) override;

  // Diagnostics used by tests and engine_cli --replay.
  d6::Stats parser_stats() const;
  UsbSerialSource& transport() { return *serial_; }

  // --- A2 additions ---------------------------------------------------------

  // The D6-specific health surface (see D6HealthSnapshot above). Cheap; safe
  // from any thread. Triggers the same opportunistic watchdog check as
  // state()/health() before returning.
  D6HealthSnapshot snapshot() const;

  // Re-evaluate the stall watchdog against `now`. Called automatically (with
  // the arriving bytes' own timestamp) from on_bytes(), and (with
  // DriverContext::clock(), i.e. wall-clock time) from state()/health()/
  // snapshot(). Exposed directly so tests and engine_cli can drive it with
  // synthetic time instead of a real sleep — see the file header.
  void check_watchdog(TimePoint now);

  // Checksum-variant resolution hook (S1 REPORT.md §2 / §7 item 1). The
  // parser always counts both variants (d6::Stats::cs_ok_vendor/cs_ok_spec);
  // this is the accepted one, i.e. which counter feeds packets_ok. Starts at
  // cfg.parser.checksum (kVendorSdk by default, per S1). Switching is applied
  // on the byte-processing thread (the next on_bytes()), not synchronously,
  // so it never races the parser's internal state — this is the only
  // supported way to change it after construction.
  d6::ChecksumVariant checksum_variant() const;
  void set_checksum_variant(d6::ChecksumVariant v);

  // Which variant the live packet stream actually confirms, if enough
  // packets have been observed (see D6Config::health_min_packets). This is
  // the "first real capture settles it" hook from REPORT.md §2 — read it,
  // and if it disagrees with checksum_variant(), call set_checksum_variant().
  D6ChecksumVerdict checksum_verdict() const;

 private:
  void on_bytes(ByteSpan bytes, TimePoint t);
  void on_point(const d6::Point& p);
  void flush_batch(std::int64_t t_ns);
  // ROUND 7: see D6Config::time_slice_bytes.
  void feed_time_sliced(ByteSpan bytes, std::int64_t t_chunk_end_ns);
  std::int64_t byte_period_ns() const;
  void set_phase(D6Phase next, ScanError err);
  void scan_for_acks(ByteSpan bytes);
  void attempt_restart(TimePoint now);
  TimePoint current_time() const;
  D6Phase get_phase() const;

  DeviceId id_;
  D6Config cfg_;
  DriverContext ctx_;

  std::unique_ptr<UsbSerialSource> serial_;
  d6::Parser parser_;

  mutable std::mutex m_;
  DeviceState state_ = DeviceState::kDisconnected;
  D6Phase phase_ = D6Phase::kIdle;
  D6StallKind stall_ = D6StallKind::kNone;
  ScanError last_error_ = ScanError::kOk;
  std::vector<PointVertex> batch_;
  std::int64_t t_current_ns_ = 0;
  std::uint64_t points_out_ = 0;
  std::uint64_t rotations_seen_ = 0;
  std::uint32_t points_in_rotation_ = 0;
  std::uint64_t drops_ = 0;
  bool saw_start_ack_ = false;

  // Watchdog bookkeeping (m_-protected; see check_watchdog()/attempt_restart()).
  std::int64_t t_start_ns_ = 0;
  std::int64_t t_last_bytes_ns_ = 0;
  std::int64_t t_last_valid_packet_ns_ = 0;
  std::uint64_t last_packets_ok_seen_ = 0;
  std::uint32_t restart_attempts_ = 0;
  std::int64_t t_next_restart_allowed_ns_ = 0;

  // Checksum-variant switch: applied on the byte-processing thread only
  // (see set_checksum_variant()'s doc comment). -1 = no pending change.
  std::atomic<int> desired_checksum_variant_{-1};
  std::atomic<int> current_checksum_variant_{0};  // 0 = vendor, 1 = spec

  // packets_ok/packets_bad_checksum as they stood at the moment the accepted
  // variant last changed. Packets rejected under the *old* variant were an
  // artefact of a misconfigured acceptance, not real loss, so the
  // kDegradedChecksum/kStreaming decision (on_bytes()) is made against the
  // window since that switch rather than the parser's lifetime totals —
  // otherwise a correct flip could never climb back out of kDegradedChecksum
  // in any reasonable number of packets. Byte-processing-thread-only, same
  // as the variant switch itself.
  std::uint64_t checksum_baseline_ok_ = 0;
  std::uint64_t checksum_baseline_bad_ = 0;
};

}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_D6_D6_DRIVER_H
