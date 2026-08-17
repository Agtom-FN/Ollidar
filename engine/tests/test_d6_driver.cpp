// D6Driver hardening (task A2): the state machine, health surface, stall
// watchdog, restart policy and checksum-variant hook layered on top of S1's
// (unmodified) parser. Unlike test_engine.cpp, these tests talk to D6Driver
// directly — no Engine — so a synthetic wall clock can drive the watchdog
// deterministically instead of sleeping in real time.
#include <cstdint>
#include <vector>

#include "doctest.h"
#include "packet_builder.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/error.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/drivers/d6/d6_driver.h"

using namespace scanengine;

namespace {

// A plain-function-pointer clock (DriverContext::clock's required shape) that
// reads a file-local variable a test can move by hand. This is exactly the
// "replay/tests substitute a deterministic clock" seam clock.h documents;
// nothing here depends on real elapsed time.
std::int64_t g_fake_now_ns = 0;
TimePoint fake_clock() { return TimePoint{g_fake_now_ns}; }

DriverContext make_ctx(EventBus* bus, PageStore* points, bool deterministic_clock = false) {
  DriverContext ctx;
  ctx.bus = bus;
  ctx.points = points;
  if (deterministic_clock) ctx.clock = &fake_clock;
  return ctx;
}

// push_bytes() has no default t_arrival (unlike Engine::push_serial_bytes) --
// this helper keeps the call sites short. TimePoint{0} means "stamp with the
// real clock now" (UsbSerialSource::push's contract), which is fine for the
// tests that do not exercise watchdog timing.
Status push(D6Driver* d, const std::vector<std::uint8_t>& bytes, TimePoint t = TimePoint{0}) {
  return d->push_bytes(ByteSpan(bytes.data(), bytes.size()), t);
}

struct Writer {
  std::vector<std::uint8_t> written;
  static ScanError write(const std::uint8_t* data, std::size_t len, void* user) {
    auto* self = static_cast<Writer*>(user);
    self->written.insert(self->written.end(), data, data + len);
    return ScanError::kOk;
  }
};

std::vector<std::uint8_t> ack_frame(std::uint8_t type, std::uint8_t trailer) {
  return {0xA5, 0x5A, type, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, trailer};
}

std::vector<std::uint8_t> good_packets(int count) {
  std::vector<std::uint8_t> out;
  for (int i = 0; i < count; ++i) {
    d6test::PacketSpec sp;
    sp.first_angle_deg = 0.0;
    sp.last_angle_deg = 9.0;
    for (int k = 0; k < 10; ++k) sp.samples.push_back(d6test::Sample{1000, 100, false});
    d6test::append(&out, d6test::build(sp));
  }
  return out;
}

std::vector<std::uint8_t> corrupt_packets(int count) {
  std::vector<std::uint8_t> out;
  for (int i = 0; i < count; ++i) {
    d6test::PacketSpec sp;
    sp.cs_mode = d6test::CsMode::kCorrupt;
    sp.first_angle_deg = 0.0;
    sp.last_angle_deg = 9.0;
    for (int k = 0; k < 10; ++k) sp.samples.push_back(d6test::Sample{1000, 100, false});
    d6test::append(&out, d6test::build(sp));
  }
  return out;
}

std::vector<std::uint8_t> spec_only_packets(int count) {
  // Varying, asymmetric sample bytes (as REPORT.md's own cross-check case
  // does): identical samples repeated an even number of times XOR-cancel and
  // make the vendor/spec checksums coincide by accident, which would defeat
  // the point of this fixture.
  std::vector<std::uint8_t> out;
  for (int i = 0; i < count; ++i) {
    d6test::PacketSpec sp;
    sp.cs_mode = d6test::CsMode::kSpec;
    sp.first_angle_deg = 0.0;
    sp.last_angle_deg = 9.0;
    for (int k = 0; k < 10; ++k) {
      sp.samples.push_back(d6test::Sample{static_cast<std::uint16_t>(500 + k * 37),
                                          static_cast<std::uint8_t>(k * 5), (k % 7) == 0});
    }
    d6test::append(&out, d6test::build(sp));
  }
  return out;
}

}  // namespace

TEST_CASE("d6driver/clean_stream_reaches_streaming") {
  EventBus bus;
  PageStore points;
  D6Driver driver(1, D6Config{}, make_ctx(&bus, &points));

  REQUIRE(driver.start().ok());
  CHECK(driver.snapshot().phase == D6Phase::kStarting);

  const auto bytes = d6test::build_revolution(20, 40);
  REQUIRE(push(&driver, bytes).ok());

  CHECK(driver.state() == DeviceState::kStreaming);
  const D6HealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == D6Phase::kStreaming);
  CHECK(snap.stall == D6StallKind::kNone);
  CHECK(snap.restart_attempts == 0);
  CHECK(snap.checksum_pass_rate == doctest::Approx(1.0));
  CHECK(snap.bytes_in == bytes.size());
}

TEST_CASE("d6driver/corrupted_checksum_degrades_with_correct_stats") {
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.health_min_packets = 4;  // rate quickly in a unit test
  D6Driver driver(2, cfg, make_ctx(&bus, &points));
  REQUIRE(driver.start().ok());

  std::vector<std::uint8_t> stream = good_packets(2);
  const auto bad = corrupt_packets(6);
  stream.insert(stream.end(), bad.begin(), bad.end());
  REQUIRE(push(&driver, stream).ok());

  const D6HealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == D6Phase::kDegradedChecksum);
  CHECK(snap.stall == D6StallKind::kNone);  // demoted by checksum rate, not the watchdog
  CHECK(snap.checksum_pass_rate < 0.995);
  CHECK(driver.state() == DeviceState::kDegraded);
  CHECK(driver.health().packets_ok == 2);
  CHECK(driver.health().packets_bad == 6);
  CHECK(driver.health().last_error == ScanError::kChecksumFailed);

  // Pass rate is cumulative since start() (like the parser's own stats), so
  // a handful of good packets is not enough to out-vote 6 bad ones already
  // on the books -- recovery needs the rate back over min_checksum_pass_rate.
  CHECK(driver.snapshot().phase == D6Phase::kDegradedChecksum);
}

TEST_CASE("d6driver/no_write_channel_stalls_but_never_auto_restarts_or_faults") {
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.startup_grace_ns = 100;
  cfg.silent_stall_timeout_ns = 50;
  cfg.garbage_stall_timeout_ns = 80;
  // cfg.serial.write_fn stays null: no command channel.
  g_fake_now_ns = 0;
  D6Driver driver(3, cfg, make_ctx(&bus, &points, /*deterministic_clock=*/true));
  REQUIRE(driver.start().ok());

  g_fake_now_ns = 1000;  // well past grace + silent timeout; no bytes ever arrived
  D6HealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == D6Phase::kStalled);
  CHECK(snap.stall == D6StallKind::kSilent);
  CHECK(snap.restart_attempts == 0);  // nothing to attempt without a write channel
  CHECK(driver.state() == DeviceState::kDegraded);

  // Repeated polling must not manufacture a fault out of nothing.
  g_fake_now_ns = 50000;
  CHECK(driver.snapshot().phase == D6Phase::kStalled);
  CHECK(driver.snapshot().restart_attempts == 0);
}

TEST_CASE("d6driver/silent_device_restarts_with_backoff_then_faults") {
  Writer writer;
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.serial.write_fn = &Writer::write;
  cfg.serial.write_user_data = &writer;
  cfg.startup_grace_ns = 100;
  cfg.silent_stall_timeout_ns = 50;
  cfg.garbage_stall_timeout_ns = 80;
  cfg.max_restart_attempts = 2;
  cfg.restart_backoff_base_ns = 200;
  cfg.restart_backoff_max_ns = 200;  // fixed backoff -- keeps the math simple

  g_fake_now_ns = 0;
  D6Driver driver(4, cfg, make_ctx(&bus, &points, /*deterministic_clock=*/true));
  REQUIRE(driver.start().ok());
  writer.written.clear();  // drop the initial start command

  int device_events = 0, error_events = 0;
  auto sub = bus.subscribe(SubscriptionOptions{});
  REQUIRE(sub.ok());

  // Past grace + silent timeout, still nothing received: first restart.
  g_fake_now_ns = 1000;
  D6HealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == D6Phase::kRestarting);
  CHECK(snap.stall == D6StallKind::kSilent);
  CHECK(snap.restart_attempts == 1);
  CHECK(driver.state() == DeviceState::kStarting);  // kRestarting maps to kStarting
  REQUIRE(writer.written.size() == 8);              // stop (4B) then start (4B)
  CHECK(writer.written[2] == 0xF5);
  CHECK(writer.written[6] == 0xF0);

  // Inside the backoff window: no second attempt yet.
  g_fake_now_ns = 1050;
  CHECK(driver.snapshot().restart_attempts == 1);
  CHECK(writer.written.size() == 8);

  // Backoff elapsed: second (and, per max_restart_attempts, last) attempt.
  g_fake_now_ns = 1201;
  CHECK(driver.snapshot().restart_attempts == 2);
  CHECK(driver.snapshot().phase == D6Phase::kRestarting);
  REQUIRE(writer.written.size() == 16);

  // Budget exhausted: the next stall check is terminal.
  g_fake_now_ns = 1401;
  CHECK(driver.snapshot().phase == D6Phase::kFault);
  CHECK(driver.state() == DeviceState::kFault);
  CHECK(driver.health().last_error == ScanError::kDeviceNotResponding);

  Event ev;
  while (bus.poll(sub.value(), &ev)) {
    if (ev.type == EventType::kDeviceState) ++device_events;
    if (ev.type == EventType::kError) ++error_events;
  }
  // starting -> stalled -> restarting -> fault, at minimum.
  CHECK(device_events >= 3);
  CHECK(error_events >= 2);  // one kError per restart attempt
}

TEST_CASE("d6driver/start_ack_gates_streaming") {
  Writer writer;
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.serial.write_fn = &Writer::write;
  cfg.serial.write_user_data = &writer;
  cfg.require_start_ack = true;
  D6Driver driver(5, cfg, make_ctx(&bus, &points));
  REQUIRE(driver.start().ok());
  CHECK(driver.snapshot().phase == D6Phase::kStarting);

  // Valid points arrive before the ACK -- must not promote yet.
  REQUIRE(push(&driver, d6test::build_revolution(2, 10)).ok());
  CHECK(driver.snapshot().phase == D6Phase::kStarting);

  // The ACK arrives, followed by more data in the same chunk.
  std::vector<std::uint8_t> chunk = ack_frame(0x50, 0xA8);  // start-ok
  const auto more = d6test::build_revolution(2, 10);
  chunk.insert(chunk.end(), more.begin(), more.end());
  REQUIRE(push(&driver, chunk).ok());
  CHECK(driver.snapshot().phase == D6Phase::kStreaming);
}

TEST_CASE("d6driver/error_ack_faults_immediately") {
  Writer writer;
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.serial.write_fn = &Writer::write;
  cfg.serial.write_user_data = &writer;
  D6Driver driver(6, cfg, make_ctx(&bus, &points));
  REQUIRE(driver.start().ok());

  REQUIRE(push(&driver, ack_frame(0x55, 0xE9)).ok());  // deliberately-wrong-XOR error frame
  CHECK(driver.snapshot().phase == D6Phase::kFault);
  CHECK(driver.state() == DeviceState::kFault);
  CHECK(driver.health().last_error == ScanError::kDeviceFault);
}

TEST_CASE("d6driver/speed_adjust_filler_tolerated_in_grace_then_garbage_stall_after") {
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.startup_grace_ns = 500;
  cfg.silent_stall_timeout_ns = 10'000;  // keep silence out of the picture
  cfg.garbage_stall_timeout_ns = 500;
  g_fake_now_ns = 0;
  D6Driver driver(7, cfg, make_ctx(&bus, &points, /*deterministic_clock=*/true));
  REQUIRE(driver.start().ok());

  const std::vector<std::uint8_t> filler(50, 0xFE);  // pure speed-adjust filler

  // Well past what garbage_stall_timeout_ns alone would allow, but still
  // inside the startup grace window: must not stall.
  g_fake_now_ns = 200;
  REQUIRE(push(&driver, filler, TimePoint{g_fake_now_ns}).ok());
  CHECK(driver.snapshot().phase == D6Phase::kStarting);
  CHECK(driver.parser_stats().speed_adjust_bytes == 50);

  // More filler right as grace ends -- still nothing decoded: a real
  // garbage-stream stall (bytes ARE flowing; nothing about it is silent).
  g_fake_now_ns = 550;
  REQUIRE(push(&driver, filler, TimePoint{g_fake_now_ns}).ok());
  const D6HealthSnapshot snap = driver.snapshot();
  CHECK(snap.phase == D6Phase::kStalled);
  CHECK(snap.stall == D6StallKind::kGarbage);
  CHECK(driver.parser_stats().speed_adjust_bytes == 100);
}

TEST_CASE("d6driver/checksum_variant_verdict_and_switch") {
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.health_min_packets = 10;
  D6Driver driver(8, cfg, make_ctx(&bus, &points));
  REQUIRE(driver.start().ok());
  CHECK(driver.checksum_variant() == d6::ChecksumVariant::kVendorSdk);
  CHECK(driver.checksum_verdict() == D6ChecksumVerdict::kUndetermined);

  // A stream whose checksums are only valid under the spec-literal grouping
  // -- what a spec-firmware device looks like to the vendor-default driver.
  REQUIRE(push(&driver, spec_only_packets(12)).ok());

  D6HealthSnapshot snap = driver.snapshot();
  CHECK(snap.cs_ok_spec == 12);
  CHECK(snap.cs_ok_vendor == 0);
  CHECK(driver.checksum_verdict() == D6ChecksumVerdict::kSpecConfirmed);
  // Still the vendor variant is *accepted* until the caller flips it -- every
  // packet reads as bad-checksum, so the device never even reaches
  // kStreaming (that needs one accepted packet), let alone kDegradedChecksum.
  // Nothing here is a fault, per DESIGN.md §3 item 5 (checksum loss is
  // graded, never fatal) -- the verdict above is the intended early signal.
  CHECK(driver.checksum_variant() == d6::ChecksumVariant::kVendorSdk);
  CHECK(snap.checksum_pass_rate == doctest::Approx(0.0));
  CHECK(driver.snapshot().phase == D6Phase::kStarting);

  driver.set_checksum_variant(d6::ChecksumVariant::kSpecLiteral);
  // The switch is applied on the next on_bytes(), not synchronously.
  CHECK(driver.checksum_variant() == d6::ChecksumVariant::kVendorSdk);

  REQUIRE(push(&driver, spec_only_packets(3)).ok());
  CHECK(driver.checksum_variant() == d6::ChecksumVariant::kSpecLiteral);
  CHECK(driver.snapshot().checksum_pass_rate > 0.0);
  CHECK(driver.snapshot().phase == D6Phase::kStreaming);
}

TEST_CASE("d6driver/stop_resets_restart_state_and_is_idempotent") {
  Writer writer;
  EventBus bus;
  PageStore points;
  D6Config cfg;
  cfg.serial.write_fn = &Writer::write;
  cfg.serial.write_user_data = &writer;
  cfg.startup_grace_ns = 10;
  cfg.silent_stall_timeout_ns = 10;
  cfg.garbage_stall_timeout_ns = 10;
  cfg.max_restart_attempts = 1;
  cfg.restart_backoff_base_ns = 10;
  cfg.restart_backoff_max_ns = 10;

  g_fake_now_ns = 0;
  D6Driver driver(9, cfg, make_ctx(&bus, &points, /*deterministic_clock=*/true));
  REQUIRE(driver.start().ok());
  g_fake_now_ns = 1000;
  CHECK(driver.snapshot().phase == D6Phase::kRestarting);  // spends its single attempt
  g_fake_now_ns = 1050;                                    // past the backoff window
  CHECK(driver.snapshot().phase == D6Phase::kFault);       // budget now exhausted

  REQUIRE(driver.stop().ok());
  CHECK(driver.state() == DeviceState::kIdle);
  CHECK(driver.stop().ok());  // idempotent

  // A fresh start() clears the restart budget and the fault.
  g_fake_now_ns = 5000;
  REQUIRE(driver.start().ok());
  CHECK(driver.snapshot().phase == D6Phase::kStarting);
  CHECK(driver.snapshot().restart_attempts == 0);
}

// --- ROUND 7: per-point time inside one byte chunk --------------------------
//
// The field symptom this pins down is "the scan is not stable, the walls are
// not straight, it comes out in sections". A8's assembler interpolates a pose
// per point and always did — what it was handed was one timestamp per
// `push_serial_bytes` chunk, so a whole chunk of returns resolved against a
// single pose and the walk laid them down as a rigid slab. On the phone a
// chunk is 4096 bytes = 178 ms of 230400-baud wire time = ~1.8 D6 revolutions,
// which at 1 m/s is up to 18 cm of shingling per chunk.
//
// The two tests below are a matched pair: the same bytes, the same driver, the
// only difference being D6Config::time_slice_bytes. The second is the
// falsifiable control — it reproduces the old behaviour and asserts the smear
// is there, so a regression that quietly disables the slicing fails loudly
// instead of passing both ways.
namespace {

struct ProfileCapture {
  std::vector<std::int64_t> times;
  static void sink(float, float, std::uint8_t, std::uint8_t, std::int64_t t_engine_ns, void* user) {
    static_cast<ProfileCapture*>(user)->times.push_back(t_engine_ns);
  }
};

// One chunk's worth of well-formed packets, ~`bytes_wanted` long.
std::vector<std::uint8_t> chunk_of_packets(std::size_t bytes_wanted) {
  std::vector<std::uint8_t> out;
  double angle = 0.0;
  while (out.size() < bytes_wanted) {
    d6test::PacketSpec sp;
    sp.first_angle_deg = angle;
    sp.last_angle_deg = angle + 9.0;
    for (int k = 0; k < 10; ++k) sp.samples.push_back(d6test::Sample{1000, 100, false});
    d6test::append(&out, d6test::build(sp));
    angle += 10.0;
    if (angle >= 360.0) angle -= 360.0;
  }
  return out;
}

}  // namespace

TEST_CASE("d6driver/round7_points_in_one_chunk_carry_their_own_arrival_time") {
  EventBus bus;
  PageStore points;
  ProfileCapture cap;

  D6Config cfg;
  cfg.send_start_stop_commands = false;
  cfg.serial.baud = 230400;
  cfg.profile_sink = &ProfileCapture::sink;
  cfg.profile_sink_user_data = &cap;
  // The default, named here because it is what the test is about.
  cfg.time_slice_bytes = 64;

  D6Driver driver(1, cfg, make_ctx(&bus, &points));
  REQUIRE(driver.start().ok());

  // A phone-sized read: 4096 bytes at 230400 8N1 is 177.8 ms of wire time.
  const std::vector<std::uint8_t> chunk = chunk_of_packets(4096);
  const std::int64_t t_chunk_end = 5'000'000'000LL;
  REQUIRE(push(&driver, chunk, TimePoint{t_chunk_end}).ok());

  REQUIRE(cap.times.size() > 100);

  // 1. The stamps are NOT all the same instant any more.
  const std::int64_t t_first = cap.times.front();
  const std::int64_t t_last = cap.times.back();
  CHECK(t_last > t_first);

  // 2. They span the chunk's own wire duration, to within a slice. 10 bits per
  //    byte at 230400 baud is 43.4 us; `chunk.size()` bytes is that times the
  //    byte count, and the first slice's stamp is one slice in from the start.
  const double byte_ns = 10.0 * 1e9 / 230400.0;
  const double expected_span_ns = byte_ns * static_cast<double>(chunk.size());
  const double slice_ns = byte_ns * 64.0;
  const double observed_span_ns = static_cast<double>(t_last - t_first);
  CHECK(observed_span_ns > expected_span_ns - 4.0 * slice_ns);
  CHECK(observed_span_ns <= expected_span_ns);

  // 3. Monotonic, and never later than the chunk's own arrival — a point that
  //    claims a time in the future of its transport would make the assembler
  //    wait for a pose that has already been superseded.
  for (std::size_t i = 1; i < cap.times.size(); ++i) CHECK(cap.times[i] >= cap.times[i - 1]);
  CHECK(t_last <= t_chunk_end);

  // 4. The resolution that matters: consecutive points are separated by no
  //    more than one slice, i.e. ~2.8 ms, i.e. ~2.8 mm of rig travel at 1 m/s.
  std::int64_t worst_gap = 0;
  for (std::size_t i = 1; i < cap.times.size(); ++i) {
    worst_gap = std::max(worst_gap, cap.times[i] - cap.times[i - 1]);
  }
  CHECK(static_cast<double>(worst_gap) <= 1.05 * slice_ns);
}

TEST_CASE("d6driver/round7_control_one_stamp_per_chunk_smears_the_whole_read") {
  EventBus bus;
  PageStore points;
  ProfileCapture cap;

  D6Config cfg;
  cfg.send_start_stop_commands = false;
  cfg.serial.baud = 230400;
  cfg.profile_sink = &ProfileCapture::sink;
  cfg.profile_sink_user_data = &cap;
  cfg.time_slice_bytes = 0;  // the pre-ROUND-7 behaviour, explicitly

  D6Driver driver(2, cfg, make_ctx(&bus, &points));
  REQUIRE(driver.start().ok());

  const std::vector<std::uint8_t> chunk = chunk_of_packets(4096);
  const std::int64_t t_chunk_end = 5'000'000'000LL;
  REQUIRE(push(&driver, chunk, TimePoint{t_chunk_end}).ok());

  REQUIRE(cap.times.size() > 100);
  // Every single return in ~178 ms of wire time claims the same instant. This
  // is the bug, asserted, so that turning the slicing off is a visible choice.
  for (std::int64_t t : cap.times) CHECK(t == t_chunk_end);
}
