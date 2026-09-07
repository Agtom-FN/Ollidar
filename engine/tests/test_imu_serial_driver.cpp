// Serial IMU (JuxiTech ICM-42670-P) — frame parser, de-burst stamper, driver.
//
// The frames used here are built by juxi_ref::Frame below, a SECOND encoder
// written from the vendor's Arduino reference (IMU_UART_SendCommand's frame
// layout and its checksum loop) rather than from juxi::encode_rate_command or
// juxi::checksum8. Two independent implementations of the same wire format
// cross-check each other, which is the whole point of tests/packet_builder.h
// doing the same thing for the D6.
//
// The stamper scenarios are the three the ROS driver's simulation ran, with
// the tolerances that simulation established: bursty-but-honest arrivals must
// come out at the TRUE rate, a multi-second outage must not corrupt the
// learned period, and a host clock step backwards must never emit a
// microsecond dt.
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/drivers/imu_serial/imu_serial_driver.h"
#include "scanengine/drivers/imu_serial/imu_stamper.h"
#include "scanengine/drivers/imu_serial/juxi_frames.h"

using namespace scanengine;

namespace {

// --- an independent encoder, from the vendor reference ---------------------
//
// imu_uart_driver.cpp, IMU_UART_SendCommand():
//     frame = {0x7E, 0x23, 0, function, params...}
//     frame_len = 4 + param_len + 1;  frame[2] = frame_len;
//     checksum = sum of frame[0 .. frame_len-2];  frame[frame_len-1] = checksum
// The receiver's _parse_frame_data() reads the payload back with to_int16()
// (little-endian) at offsets 0/2/4 accel, 6/8/10 gyro, 12/14/16 mag.
namespace juxi_ref {

std::vector<std::uint8_t> frame(std::uint8_t func, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> f;
  f.push_back(0x7E);
  f.push_back(0x23);
  f.push_back(static_cast<std::uint8_t>(4 + payload.size() + 1));
  f.push_back(func);
  for (std::uint8_t b : payload) f.push_back(b);
  int sum = 0;
  for (std::size_t i = 0; i < f.size(); ++i) sum += f[i];
  f.push_back(static_cast<std::uint8_t>(sum & 0xFF));
  return f;
}

void put_i16(std::vector<std::uint8_t>* v, int value) {
  const std::uint16_t u = static_cast<std::uint16_t>(static_cast<std::int16_t>(value));
  v->push_back(static_cast<std::uint8_t>(u & 0xFF));
  v->push_back(static_cast<std::uint8_t>((u >> 8) & 0xFF));
}

// func 0x04: ax ay az gx gy gz mx my mz, int16 LE.
std::vector<std::uint8_t> raw_imu(int ax, int ay, int az, int gx, int gy, int gz, int mx = 0,
                                  int my = 0, int mz = 0) {
  std::vector<std::uint8_t> p;
  put_i16(&p, ax);
  put_i16(&p, ay);
  put_i16(&p, az);
  put_i16(&p, gx);
  put_i16(&p, gy);
  put_i16(&p, gz);
  put_i16(&p, mx);
  put_i16(&p, my);
  put_i16(&p, mz);
  return frame(0x04, p);
}

std::vector<std::uint8_t> quaternion() { return frame(0x16, std::vector<std::uint8_t>(16, 0x11)); }
std::vector<std::uint8_t> euler() { return frame(0x26, std::vector<std::uint8_t>(12, 0x22)); }
std::vector<std::uint8_t> barometer() { return frame(0x32, std::vector<std::uint8_t>(16, 0x33)); }
std::vector<std::uint8_t> version() { return frame(0x01, {1, 2, 3}); }
std::vector<std::uint8_t> return_state() { return frame(0x81, {0x70, 0x01}); }

}  // namespace juxi_ref

void append(std::vector<std::uint8_t>* dst, const std::vector<std::uint8_t>& src) {
  dst->insert(dst->end(), src.begin(), src.end());
}

std::vector<juxi::RawImuFrame> parse_all(juxi::FrameParser* p, const std::vector<std::uint8_t>& b,
                                         std::size_t chunk = 0) {
  std::vector<juxi::RawImuFrame> out;
  const auto cb = [&out](const juxi::RawImuFrame& f) { out.push_back(f); };
  if (chunk == 0) {
    p->push(b.data(), b.size(), cb);
  } else {
    for (std::size_t i = 0; i < b.size(); i += chunk) {
      p->push(b.data() + i, std::min(chunk, b.size() - i), cb);
    }
  }
  return out;
}

// --- scripted clock -------------------------------------------------------
std::int64_t g_now_ns = 0;
TimePoint fake_clock() { return TimePoint{g_now_ns}; }

DriverContext make_ctx(EventBus* bus, PageStore* points) {
  DriverContext ctx;
  ctx.bus = bus;
  ctx.points = points;
  ctx.clock = &fake_clock;
  return ctx;
}

ImuSerialConfig test_config() {
  ImuSerialConfig cfg;
  cfg.serial.port_name = "test";
  cfg.serial.baud = 115200;
  cfg.internal_supervisor_thread = false;  // tests drive tick() themselves
  cfg.send_rate_command = false;
  return cfg;
}

struct SinkCollector {
  std::vector<ImuSerialSample> samples;
  static void on_samples(const ImuSerialSample* s, std::size_t n, void* user) {
    auto* self = static_cast<SinkCollector*>(user);
    for (std::size_t i = 0; i < n; ++i) self->samples.push_back(s[i]);
  }
};

struct Writer {
  std::vector<std::uint8_t> written;
  static ScanError write(const std::uint8_t* data, std::size_t len, void* user) {
    auto* self = static_cast<Writer*>(user);
    self->written.insert(self->written.end(), data, data + len);
    return ScanError::kOk;
  }
};

constexpr double kNs = 1e9;
std::int64_t sec_ns(double s) { return static_cast<std::int64_t>(std::llround(s * kNs)); }

}  // namespace

// ===========================================================================
// juxi_frames
// ===========================================================================

TEST_CASE("imu_serial/raw_frame_decodes_to_g_and_rad_per_s") {
  // Known int16s through the vendor's own scale factors, computed here in
  // double from the constants in the reference (16/32767, 2000/32767 deg/s).
  const std::vector<std::uint8_t> b =
      juxi_ref::raw_imu(2048, -2048, 32767, 1000, -1000, 16384, 500, -500, 250);
  juxi::FrameParser p;
  const auto frames = parse_all(&p, b);
  REQUIRE(frames.size() == 1);
  CHECK(p.stats().raw_frames == 1);
  CHECK(p.stats().checksum_failures == 0);
  CHECK(p.stats().resyncs == 0);
  CHECK(p.stats().bytes_in == juxi::kRawImuFrameBytes);
  CHECK(b.size() == juxi::kRawImuFrameBytes);

  const double accel = 16.0 / 32767.0;
  const double gyro = (2000.0 / 32767.0) * 3.14159265358979323846 / 180.0;
  const double mag = 800.0 / 32767.0;

  CHECK(frames[0].accel_g[0] == doctest::Approx(2048 * accel).epsilon(1e-5));
  CHECK(frames[0].accel_g[1] == doctest::Approx(-2048 * accel).epsilon(1e-5));
  CHECK(frames[0].accel_g[2] == doctest::Approx(32767 * accel).epsilon(1e-5));
  CHECK(frames[0].gyro_rad_s[0] == doctest::Approx(1000 * gyro).epsilon(1e-5));
  CHECK(frames[0].gyro_rad_s[1] == doctest::Approx(-1000 * gyro).epsilon(1e-5));
  CHECK(frames[0].gyro_rad_s[2] == doctest::Approx(16384 * gyro).epsilon(1e-5));
  CHECK(frames[0].mag_ut[0] == doctest::Approx(500 * mag).epsilon(1e-5));
  CHECK(frames[0].mag_ut[1] == doctest::Approx(-500 * mag).epsilon(1e-5));
  CHECK(frames[0].mag_ut[2] == doctest::Approx(250 * mag).epsilon(1e-5));

  // A full-scale accel reading is 16 g and a full-scale gyro 2000 deg/s —
  // the module's ranges, stated in the reference. If the scales were ever
  // "simplified" these two would move.
  CHECK(frames[0].accel_g[2] == doctest::Approx(16.0).epsilon(1e-4));
  CHECK(16384 * gyro * 180.0 / 3.14159265358979323846 == doctest::Approx(1000.03).epsilon(1e-4));
}

TEST_CASE("imu_serial/chunk_splits_at_every_boundary_reproduce_the_same_frames") {
  std::vector<std::uint8_t> stream;
  for (int i = 0; i < 5; ++i) {
    append(&stream, juxi_ref::raw_imu(100 + i, 200, 300, -400, 500, -600));
    append(&stream, juxi_ref::quaternion());
    append(&stream, juxi_ref::euler());
    append(&stream, juxi_ref::barometer());
  }

  juxi::FrameParser whole;
  const auto reference = parse_all(&whole, stream);
  REQUIRE(reference.size() == 5);

  // Every chunk size from 1 byte up to the whole stream: the same five frames,
  // the same values, no resyncs and no checksum failures.
  for (std::size_t chunk = 1; chunk <= stream.size(); ++chunk) {
    juxi::FrameParser p;
    const auto got = parse_all(&p, stream, chunk);
    REQUIRE_MESSAGE(got.size() == reference.size(), "chunk size " << chunk);
    for (std::size_t i = 0; i < got.size(); ++i) {
      CHECK(got[i].accel_g[0] == doctest::Approx(reference[i].accel_g[0]));
      CHECK(got[i].gyro_rad_s[2] == doctest::Approx(reference[i].gyro_rad_s[2]));
    }
    CHECK(p.stats().checksum_failures == 0);
    CHECK(p.stats().resyncs == 0);
    CHECK(p.stats().raw_frames == 5);
    CHECK(p.stats().quaternion_frames == 5);
  }
}

TEST_CASE("imu_serial/corrupted_checksum_is_rejected_and_counted") {
  std::vector<std::uint8_t> good = juxi_ref::raw_imu(1, 2, 3, 4, 5, 6);
  std::vector<std::uint8_t> bad = good;
  bad.back() = static_cast<std::uint8_t>(bad.back() ^ 0xFF);

  juxi::FrameParser p;
  auto frames = parse_all(&p, bad);
  CHECK(frames.empty());
  CHECK(p.stats().checksum_failures == 1);
  CHECK(p.stats().raw_frames == 0);
  CHECK(p.stats().resyncs == 0);  // the frame was complete, just wrong

  // A corrupted PAYLOAD byte is caught the same way.
  std::vector<std::uint8_t> bad2 = good;
  bad2[6] = static_cast<std::uint8_t>(bad2[6] ^ 0x01);
  frames = parse_all(&p, bad2);
  CHECK(frames.empty());
  CHECK(p.stats().checksum_failures == 2);

  // and the parser is still able to decode the next good frame.
  frames = parse_all(&p, good);
  CHECK(frames.size() == 1);
  CHECK(p.stats().raw_frames == 1);
  CHECK(p.stats().checksum_pass_rate() == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("imu_serial/garbage_then_resync") {
  std::vector<std::uint8_t> stream;
  // Random-looking bytes, a 0x7E not followed by 0x23, a doubled 0x7E, and an
  // impossible length field — then a TRUNCATED frame, then two good ones.
  for (std::uint8_t b : {0x00, 0xFF, 0x12, 0x7E, 0x99, 0x7E, 0x7E, 0x23, 0x02, 0x04, 0x00})
    stream.push_back(b);
  const std::vector<std::uint8_t> truncated = juxi_ref::raw_imu(7, 7, 7, 7, 7, 7);
  stream.insert(stream.end(), truncated.begin(), truncated.end() - 4);
  append(&stream, juxi_ref::raw_imu(11, 22, 33, 44, 55, 66));  // eaten by the truncated frame
  append(&stream, juxi_ref::raw_imu(12, 23, 34, 45, 56, 67));  // this one survives

  juxi::FrameParser p;
  const auto frames = parse_all(&p, stream);

  // The truncated frame has a valid length field, so the state machine is
  // still collecting its body when the next frame's header arrives and
  // swallows it — the frame that completes fails its checksum and the frame
  // that was eaten is simply gone. That IS the behaviour (the vendor's
  // receiver does exactly the same); what matters is that the parser is back
  // in sync one frame later and never invents a sample out of the wreckage.
  REQUIRE(frames.size() == 1);
  const double accel = 16.0 / 32767.0;
  CHECK(frames[0].accel_g[0] == doctest::Approx(12 * accel).epsilon(1e-4));
  CHECK(p.stats().raw_frames == 1);
  CHECK(p.stats().checksum_failures == 1);
  CHECK(p.stats().resyncs >= 3);  // 0x7E 0x99, the doubled 0x7E, the bad length
}

TEST_CASE("imu_serial/other_frame_types_are_counted_and_ignored") {
  std::vector<std::uint8_t> stream;
  append(&stream, juxi_ref::quaternion());
  append(&stream, juxi_ref::euler());
  append(&stream, juxi_ref::barometer());
  append(&stream, juxi_ref::version());
  append(&stream, juxi_ref::return_state());
  append(&stream, juxi_ref::frame(0x77, {0xAB, 0xCD}));  // a func we do not model
  append(&stream, juxi_ref::raw_imu(1, 1, 1, 1, 1, 1));

  juxi::FrameParser p;
  const auto frames = parse_all(&p, stream);
  CHECK(frames.size() == 1);
  const juxi::FrameStats& st = p.stats();
  CHECK(st.quaternion_frames == 1);
  CHECK(st.euler_frames == 1);
  CHECK(st.barometer_frames == 1);
  CHECK(st.version_frames == 1);
  CHECK(st.state_frames == 1);
  CHECK(st.unknown_frames == 1);
  CHECK(st.raw_frames == 1);
  CHECK(st.checksum_failures == 0);
  CHECK(st.resyncs == 0);
  CHECK(st.frames_ok() == 7);
  CHECK(st.checksum_pass_rate() == doctest::Approx(1.0));

  CHECK(std::string(juxi::func_name(juxi::kFuncRawImu)) == "raw-imu");
  CHECK(std::string(juxi::func_name(juxi::kFuncQuaternion)) == "quaternion");
  CHECK(std::string(juxi::func_name(juxi::kFuncEuler)) == "euler");
  CHECK(std::string(juxi::func_name(juxi::kFuncBarometer)) == "barometer");
  CHECK(std::string(juxi::func_name(juxi::kFuncVersion)) == "version");
  CHECK(std::string(juxi::func_name(juxi::kFuncReturnState)) == "return-state");
  CHECK(std::string(juxi::func_name(juxi::kFuncSetRate)) == "set-rate");
  CHECK(std::string(juxi::func_name(juxi::kFuncRequestData)) == "request-data");
  CHECK(std::string(juxi::func_name(0x77)) == "unknown");
}

TEST_CASE("imu_serial/rate_command_bytes") {
  std::uint8_t out[juxi::kRateCommandBytes] = {0};
  REQUIRE(juxi::encode_rate_command(100, out) == 7);
  // 7E 23 07 60 64 5F CB — checksum from the reference's own loop.
  const std::vector<std::uint8_t> expected = {0x7E, 0x23, 0x07, 0x60, 0x64, 0x5F, 0xCB};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(static_cast<int>(out[i]) == static_cast<int>(expected[i]));
  }
  // The independent encoder agrees, header for header.
  const std::vector<std::uint8_t> ref = juxi_ref::frame(0x60, {100, 0x5F});
  REQUIRE(ref.size() == 7);
  for (std::size_t i = 0; i < ref.size(); ++i) CHECK(out[i] == ref[i]);

  // 10 Hz is the low bound; outside 10..100 nothing is emitted.
  CHECK(juxi::encode_rate_command(10, out) == 7);
  CHECK(juxi::encode_rate_command(9, out) == 0);
  CHECK(juxi::encode_rate_command(101, out) == 0);
  CHECK(juxi::encode_rate_command(0, out) == 0);
}

// ===========================================================================
// ImuStamper — the three ROS-simulated scenarios
// ===========================================================================

TEST_CASE("imu_serial/stamper_bursts_of_ten_at_true_108hz") {
  // The module was asked for 100 Hz and delivers 108: the whole reason the
  // period is measured rather than believed. Ten samples arrive at once, then
  // nothing for 92.6 ms, forever.
  ImuStamperConfig cfg;
  cfg.nominal_hz = 100.0;  // deliberately WRONG
  ImuStamper s(cfg);

  const double true_p = 1.0 / 108.0;
  std::vector<std::int64_t> out;
  const int bursts = 500;
  for (int k = 0; k < bursts; ++k) {
    const std::int64_t arrival = sec_ns((10.0 * k + 9.0) * true_p);
    for (int i = 0; i < 10; ++i) out.push_back(s.stamp(arrival));
  }

  const double mean_dt =
      static_cast<double>(out.back() - out.front()) / kNs / static_cast<double>(out.size() - 1);
  const ImuStamperStats st = s.stats();
  const double learned = static_cast<double>(st.learned_period_ns) / kNs;

  INFO("mean dt " << mean_dt * 1e3 << " ms, learned " << learned * 1e3 << " ms, gaps "
                  << st.gap_snaps << ", deep " << st.deep_bursts);
  CHECK(std::fabs(mean_dt - true_p) / true_p < 0.01);   // within 1% of the TRUE rate
  CHECK(std::fabs(learned - true_p) / true_p < 0.02);   // learned period within 2%
  CHECK(st.clamps == 0);
  CHECK(st.clock_step_backs == 0);
  CHECK(st.samples == out.size());
  CHECK(st.deep_bursts > 0);  // the link IS this bursty; the counter says so
  for (std::size_t i = 1; i < out.size(); ++i) REQUIRE(out[i] > out[i - 1]);

  // The nominal rate must NOT be what came out: this is the 2500 ppm bug the
  // measured-period model exists to kill.
  CHECK(std::fabs(mean_dt - 0.01) / 0.01 > 0.05);
}

TEST_CASE("imu_serial/stamper_three_second_outage_leaves_the_period_intact") {
  ImuStamperConfig cfg;
  cfg.nominal_hz = 100.0;
  ImuStamper s(cfg);

  const double true_p = 0.01;
  double t = 0.0;
  std::vector<std::int64_t> out;
  for (int i = 0; i < 3000; ++i) {
    out.push_back(s.stamp(sec_ns(t)));
    t += true_p;
  }
  const double before = static_cast<double>(s.stats().learned_period_ns) / kNs;
  CHECK(std::fabs(before - true_p) / true_p < 0.005);

  t += 3.0;  // the module's firmware stall, exaggerated
  for (int i = 0; i < 3000; ++i) {
    out.push_back(s.stamp(sec_ns(t)));
    t += true_p;
  }

  const ImuStamperStats st = s.stats();
  const double after = static_cast<double>(st.learned_period_ns) / kNs;
  INFO("period before " << before * 1e3 << " ms, after " << after * 1e3 << " ms");
  CHECK(std::fabs(after - true_p) / true_p < 0.005);  // untouched by the outage
  CHECK(st.gap_snaps == 1);
  CHECK(st.clamps == 0);
  CHECK(st.clock_step_backs == 0);
  CHECK(st.worst_gap_ns > sec_ns(2.9));
  for (std::size_t i = 1; i < out.size(); ++i) REQUIRE(out[i] > out[i - 1]);
}

TEST_CASE("imu_serial/stamper_host_clock_step_back") {
  ImuStamperConfig cfg;
  cfg.nominal_hz = 100.0;
  ImuStamper s(cfg);

  const double true_p = 0.01;
  double t = 0.0;
  std::vector<std::int64_t> out;
  for (int i = 0; i < 2000; ++i) {
    out.push_back(s.stamp(sec_ns(t)));
    t += true_p;
  }
  t -= 1.0;  // NTP, a VM resume, a suspend
  for (int i = 0; i < 2000; ++i) {
    out.push_back(s.stamp(sec_ns(t)));
    t += true_p;
  }

  const ImuStamperStats st = s.stats();
  CHECK(st.clock_step_backs == 1);
  CHECK(st.clamps == 0);
  CHECK(st.worst_step_back_ns > sec_ns(0.98));
  std::int64_t min_dt = INT64_MAX;
  for (std::size_t i = 1; i < out.size(); ++i) {
    REQUIRE(out[i] > out[i - 1]);  // monotonic
    min_dt = std::min(min_dt, out[i] - out[i - 1]);
  }
  INFO("min dt " << static_cast<double>(min_dt) / 1e6 << " ms");
  CHECK(min_dt >= 1000000);  // no sample closer than 1 ms: the 1 us bug
  CHECK(s.stats().resyncing);  // still free-running; the host clock never caught up
}

TEST_CASE("imu_serial/stamper_reset_and_first_sample") {
  ImuStamper s;
  const std::int64_t t0 = 1'700'000'000'000'000'000LL;  // a realistic steady-clock value
  CHECK(s.stamp(t0) == t0);                             // first sample is its own arrival
  CHECK(s.stamp(t0 + 10'000'000) > t0);
  CHECK(s.stats().samples == 2);
  s.reset();
  CHECK(s.stats().samples == 0);
  CHECK(s.stamp(t0) == t0);
}

// ===========================================================================
// ImuSerialDriver
// ===========================================================================

TEST_CASE("imu_serial/driver_samples_reach_sink_and_ring") {
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  SinkCollector sink;
  ImuSerialConfig cfg = test_config();
  cfg.sink = &SinkCollector::on_samples;
  cfg.sink_user_data = &sink;

  ImuSerialDriver d(7, cfg, make_ctx(&bus, &store));
  CHECK(d.kind() == DeviceKind::kImuSerial);
  CHECK(std::string(d.name()) == "imu-serial");
  CHECK(d.id() == 7);
  CHECK(d.state() == DeviceState::kIdle);
  REQUIRE(d.start().ok());
  CHECK(d.state() == DeviceState::kStarting);

  for (int i = 0; i < 50; ++i) {
    std::vector<std::uint8_t> chunk;
    append(&chunk, juxi_ref::raw_imu(100, 200, 300, -400, 500, -600));
    append(&chunk, juxi_ref::quaternion());  // the module always sends these too
    append(&chunk, juxi_ref::euler());
    append(&chunk, juxi_ref::barometer());
    g_now_ns += 10'000'000;  // 100 Hz
    REQUIRE(d.push_bytes(ByteSpan(chunk.data(), chunk.size()), TimePoint{g_now_ns}).ok());
  }

  CHECK(d.state() == DeviceState::kStreaming);
  const ImuSerialStats st = d.stats();
  CHECK(st.samples == 50);
  CHECK(st.frames.raw_frames == 50);
  CHECK(st.frames.quaternion_frames == 50);
  CHECK(st.frames.checksum_failures == 0);
  CHECK(st.blackouts == 0);

  REQUIRE(sink.samples.size() == 50);
  for (std::size_t i = 1; i < sink.samples.size(); ++i) {
    CHECK(sink.samples[i].t_stamped_ns > sink.samples[i - 1].t_stamped_ns);
    CHECK(sink.samples[i].t_arrival_ns > sink.samples[i - 1].t_arrival_ns);
  }
  const double gyro = (2000.0 / 32767.0) * 3.14159265358979323846 / 180.0;
  CHECK(sink.samples[0].gyro_rad_s[0] == doctest::Approx(-400 * gyro).epsilon(1e-5));
  CHECK(sink.samples[0].accel_g[2] == doctest::Approx(300 * 16.0 / 32767.0).epsilon(1e-5));

  // The ring carries the same samples, oldest first.
  std::vector<ImuSerialSample> drained(64);
  const std::size_t n = d.drain(drained.data(), drained.size());
  CHECK(n == 50);
  for (std::size_t i = 0; i < n; ++i) {
    CHECK(drained[i].t_stamped_ns == sink.samples[i].t_stamped_ns);
  }
  CHECK(d.drain(drained.data(), drained.size()) == 0);  // drained is drained

  const DeviceHealth h = d.health();
  CHECK(h.kind == DeviceKind::kImuSerial);
  CHECK(h.packets_ok == 50);
  CHECK(h.packets_bad == 0);
  CHECK(h.points_out == 0);
  CHECK(h.points_per_sec == 0.0);
  CHECK(h.checksum_pass_rate == doctest::Approx(1.0));
  CHECK(h.t_last_data_ns == g_now_ns);
  CHECK(store.total_points() == 0);  // IMU is never geometry

  REQUIRE(d.stop().ok());
  CHECK(d.state() == DeviceState::kIdle);
}

TEST_CASE("imu_serial/driver_ring_overflow_is_counted") {
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  ImuSerialConfig cfg = test_config();
  cfg.ring_capacity = 8;
  ImuSerialDriver d(1, cfg, make_ctx(&bus, &store));
  REQUIRE(d.start().ok());
  for (int i = 0; i < 20; ++i) {
    const auto f = juxi_ref::raw_imu(i, 0, 0, 0, 0, 0);
    g_now_ns += 10'000'000;
    REQUIRE(d.push_bytes(ByteSpan(f.data(), f.size()), TimePoint{g_now_ns}).ok());
  }
  CHECK(d.stats().samples == 20);
  CHECK(d.stats().samples_dropped == 12);
  std::vector<ImuSerialSample> out(32);
  CHECK(d.drain(out.data(), out.size()) == 8);
}

TEST_CASE("imu_serial/driver_start_sends_the_rate_command") {
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  Writer w;
  ImuSerialConfig cfg = test_config();
  cfg.send_rate_command = true;
  cfg.report_rate_hz = 100;
  cfg.serial.write_fn = &Writer::write;
  cfg.serial.write_user_data = &w;

  ImuSerialDriver d(3, cfg, make_ctx(&bus, &store));
  REQUIRE(d.start().ok());
  REQUIRE(w.written.size() == 7);
  const std::vector<std::uint8_t> expected = {0x7E, 0x23, 0x07, 0x60, 0x64, 0x5F, 0xCB};
  CHECK(w.written == expected);
  REQUIRE(d.stop().ok());

  // No write function: still starts (the module free-runs on its persisted
  // rate), and nothing is sent.
  Writer w2;
  ImuSerialConfig cfg2 = test_config();
  cfg2.send_rate_command = true;
  ImuSerialDriver d2(4, cfg2, make_ctx(&bus, &store));
  REQUIRE(d2.start().ok());
  CHECK(w2.written.empty());
  CHECK(d2.state() == DeviceState::kStarting);
}

TEST_CASE("imu_serial/driver_blackout_degrades_then_recovers") {
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  SubscriptionOptions opts;
  opts.category_mask = mask_of(EventCategory::kDevice);
  const auto sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  ImuSerialConfig cfg = test_config();
  cfg.blackout_threshold_s = 0.5;
  ImuSerialDriver d(9, cfg, make_ctx(&bus, &store));
  REQUIRE(d.start().ok());

  const auto push_one = [&d]() {
    const auto f = juxi_ref::raw_imu(1, 2, 3, 4, 5, 6);
    REQUIRE(d.push_bytes(ByteSpan(f.data(), f.size()), TimePoint{g_now_ns}).ok());
  };

  for (int i = 0; i < 100; ++i) {
    g_now_ns += 10'000'000;
    push_one();
    d.tick(TimePoint{g_now_ns});
  }
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.stats().blackouts == 0);

  // The module goes quiet for 2.9 s — its measured firmware stall.
  for (int i = 0; i < 58; ++i) {
    g_now_ns += 50'000'000;
    d.tick(TimePoint{g_now_ns});
  }
  CHECK(d.state() == DeviceState::kDegraded);
  CHECK(d.stats().blackouts == 1);
  CHECK(d.stats().blackout_in_progress);

  // Data resumes: back to streaming, exactly one blackout counted, and its
  // full duration recorded.
  g_now_ns += 10'000'000;
  push_one();
  d.tick(TimePoint{g_now_ns});
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.stats().blackouts == 1);
  CHECK_FALSE(d.stats().blackout_in_progress);
  CHECK(d.stats().worst_blackout_ns >= sec_ns(2.9));

  bool saw_degraded = false, saw_recovery = false;
  Event ev;
  while (bus.poll(sub.value(), &ev)) {
    if (ev.type != EventType::kDeviceState) continue;
    const DeviceStatePayload& p = ev.payload.device;
    CHECK(p.kind == DeviceKind::kImuSerial);
    CHECK(p.device == 9);
    if (p.state == DeviceState::kDegraded) {
      saw_degraded = true;
      CHECK(p.error == ScanError::kDeviceNotResponding);
    }
    if (saw_degraded && p.state == DeviceState::kStreaming) saw_recovery = true;
  }
  CHECK(saw_degraded);
  CHECK(saw_recovery);

  // The blackout is COUNTED, never smoothed: the stamper snapped to the
  // arrival rather than inventing 290 samples of motion.
  CHECK(d.stats().stamper.gap_snaps == 1);
  CHECK(d.stats().stamper.clamps == 0);
}

TEST_CASE("imu_serial/driver_blackout_counted_once_without_a_tick") {
  // Same outage, but nothing ever calls tick() (an app that only pushes
  // bytes). The resume must still count exactly one blackout.
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  ImuSerialDriver d(2, test_config(), make_ctx(&bus, &store));
  REQUIRE(d.start().ok());
  for (int i = 0; i < 10; ++i) {
    g_now_ns += 10'000'000;
    const auto f = juxi_ref::raw_imu(1, 1, 1, 1, 1, 1);
    REQUIRE(d.push_bytes(ByteSpan(f.data(), f.size()), TimePoint{g_now_ns}).ok());
  }
  g_now_ns += 2'900'000'000;
  const auto f = juxi_ref::raw_imu(1, 1, 1, 1, 1, 1);
  REQUIRE(d.push_bytes(ByteSpan(f.data(), f.size()), TimePoint{g_now_ns}).ok());
  CHECK(d.stats().blackouts == 1);
  CHECK(d.stats().worst_blackout_ns >= sec_ns(2.9));
  CHECK(d.state() == DeviceState::kStreaming);
}

TEST_CASE("imu_serial/driver_checksum_degradation_and_recovery") {
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  ImuSerialConfig cfg = test_config();
  cfg.health_min_frames = 200;
  cfg.min_checksum_pass_rate = 0.99;
  ImuSerialDriver d(5, cfg, make_ctx(&bus, &store));
  REQUIRE(d.start().ok());

  const auto good = juxi_ref::raw_imu(1, 2, 3, 4, 5, 6);
  std::vector<std::uint8_t> bad = good;
  bad.back() = static_cast<std::uint8_t>(bad.back() ^ 0x5A);

  // 150 good frames: too few to rate, and clean anyway.
  for (int i = 0; i < 150; ++i) {
    g_now_ns += 10'000'000;
    REQUIRE(d.push_bytes(ByteSpan(good.data(), good.size()), TimePoint{g_now_ns}).ok());
  }
  CHECK(d.state() == DeviceState::kStreaming);

  // A burst of corruption takes the rate under 99% past 200 frames observed.
  for (int i = 0; i < 100; ++i) {
    g_now_ns += 10'000'000;
    REQUIRE(d.push_bytes(ByteSpan(bad.data(), bad.size()), TimePoint{g_now_ns}).ok());
  }
  CHECK(d.state() == DeviceState::kDegraded);
  CHECK(d.stats().frames.checksum_failures == 100);
  CHECK(d.health().packets_bad == 100);
  CHECK(d.health().checksum_pass_rate < 0.99);

  // A long clean run pulls the lifetime rate back over the bar.
  for (int i = 0; i < 20000; ++i) {
    g_now_ns += 10'000'000;
    REQUIRE(d.push_bytes(ByteSpan(good.data(), good.size()), TimePoint{g_now_ns}).ok());
  }
  CHECK(d.health().checksum_pass_rate > 0.99);
  CHECK(d.state() == DeviceState::kStreaming);
}

TEST_CASE("imu_serial/driver_health_window_measures_the_rate") {
  g_now_ns = 0;
  EventBus bus;
  PageStore store;
  SubscriptionOptions opts;
  opts.category_mask = mask_of(EventCategory::kDevice);
  const auto sub = bus.subscribe(opts);
  REQUIRE(sub.ok());

  ImuSerialConfig cfg = test_config();
  cfg.health_period_ms = 1000;
  ImuSerialDriver d(6, cfg, make_ctx(&bus, &store));
  REQUIRE(d.start().ok());

  const auto f = juxi_ref::raw_imu(0, 0, 8192, 0, 0, 0);
  for (int i = 0; i < 200; ++i) {  // 2 s at 100 Hz
    g_now_ns += 10'000'000;
    REQUIRE(d.push_bytes(ByteSpan(f.data(), f.size()), TimePoint{g_now_ns}).ok());
    d.tick(TimePoint{g_now_ns});
  }
  std::vector<DeviceHealthPayload> health;
  Event ev;
  while (bus.poll(sub.value(), &ev)) {
    if (ev.type == EventType::kDeviceHealth) health.push_back(ev.payload.health);
  }
  REQUIRE(health.size() >= 1);
  CHECK(health[0].device == 6);
  CHECK(health[0].points_out == 0);
  CHECK(health[0].points_per_sec == 0.0);
  CHECK(health[0].checksum_pass_rate == doctest::Approx(1.0));
  CHECK(d.stats().rate_hz == doctest::Approx(100.0).epsilon(0.02));
  CHECK(d.health().rotation_hz == doctest::Approx(100.0).epsilon(0.02));
}

TEST_CASE("imu_serial/driver_supervisor_thread_starts_and_stops") {
  // The one case that uses the real thread: it must start, tick against the
  // real clock, and join cleanly. (Everything about the state machine is
  // asserted above with the scripted clock instead.)
  EventBus bus;
  PageStore store;
  ImuSerialConfig cfg;
  cfg.serial.port_name = "test";
  cfg.send_rate_command = false;
  cfg.internal_supervisor_thread = true;
  ImuSerialDriver d(8, cfg, make_ctx(&bus, &store));
  REQUIRE(d.start().ok());
  const auto f = juxi_ref::raw_imu(1, 1, 1, 1, 1, 1);
  REQUIRE(d.push_bytes(ByteSpan(f.data(), f.size()), TimePoint{0}).ok());
  REQUIRE(d.stop().ok());
  CHECK(d.state() == DeviceState::kIdle);
  CHECK(d.stats().samples == 1);
  REQUIRE(d.stop().ok());  // idempotent
}
