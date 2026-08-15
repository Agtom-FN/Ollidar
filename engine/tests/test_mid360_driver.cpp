// Mid-360 driver + wire layer. NO LIVOX SDK IS LINKED BY THIS FILE.
//
// Everything here runs on synthetic datagrams built to the byte layout S2
// cross-checked against the SDK source, the published protocol tables and a
// real Livox recording. That is deliberate: the SDK is fetched, not
// committed, so the CI legs that never run fetch_sdk2.sh must still prove
// the parts that can actually be wrong — the free-running udp_cnt loss
// model, the no-return/tag filter, the metric conversion, and the watchdog /
// forced-re-init state machine.
//
// The reconnect tests drive Mid360Driver::tick() with a scripted clock
// rather than sleeping, so they assert the state machine itself instead of
// racing it.
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "scanengine/cloud/page_store.h"
#include "scanengine/core/event_bus.h"
#include "scanengine/drivers/mid360/mid360_driver.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/transport/udp_source.h"

using namespace scanengine;
using namespace scanengine::mid360;

namespace {

// --- a scripted clock -----------------------------------------------------
//
// ClockFn is a plain function pointer (so replay is reproducible and the C
// ABI can carry it), hence the file-scope current time.
std::int64_t g_now_ns = 0;
TimePoint fake_clock() { return TimePoint{g_now_ns}; }
void advance_ms(std::int64_t ms) { g_now_ns += ms * 1000000LL; }

// --- synthetic packets ----------------------------------------------------

struct PointPacket {
  std::vector<std::uint8_t> bytes;
  DataHeader* header() { return reinterpret_cast<DataHeader*>(bytes.data()); }
  CartesianHigh* points() {
    return reinterpret_cast<CartesianHigh*>(bytes.data() + sizeof(DataHeader));
  }
};

// One canonical 1380-byte Cartesian-high packet: 36-byte header + 96 points.
PointPacket make_point_packet(std::uint16_t udp_cnt, std::uint16_t dot_num = kPointsPerPacket) {
  PointPacket p;
  p.bytes.assign(sizeof(DataHeader) + dot_num * sizeof(CartesianHigh), 0);
  DataHeader* h = p.header();
  h->version = 0;
  h->length = static_cast<std::uint16_t>(p.bytes.size());
  h->time_interval = 4750;  // 95 x 5 us in 0.1 us units, as the real device sends
  h->dot_num = dot_num;
  h->udp_cnt = udp_cnt;
  h->frame_cnt = 0;  // real firmware never increments this
  h->data_type = kDataTypeCartesianHigh;
  h->time_type = 0;
  h->timestamp = 1'000'000'000ULL + static_cast<std::uint64_t>(udp_cnt) * 480'000ULL;
  CartesianHigh* pts = p.points();
  for (std::uint16_t i = 0; i < dot_num; ++i) {
    pts[i].x = 1000 + i;  // 1.000 m and up
    pts[i].y = 2000;
    pts[i].z = -500;
    pts[i].reflectivity = 100;
    pts[i].tag = 0;
  }
  return p;
}

std::vector<std::uint8_t> make_imu_packet(std::uint16_t udp_cnt, float gyro_x, float acc_z) {
  std::vector<std::uint8_t> b(sizeof(DataHeader) + sizeof(ImuRaw), 0);
  auto* h = reinterpret_cast<DataHeader*>(b.data());
  h->version = 0;
  h->length = static_cast<std::uint16_t>(b.size());
  h->dot_num = 1;
  h->udp_cnt = udp_cnt;
  h->data_type = kDataTypeImu;
  h->timestamp = 5'000'000'000ULL;
  auto* s = reinterpret_cast<ImuRaw*>(b.data() + sizeof(DataHeader));
  s->gyro_x = gyro_x;
  s->gyro_y = 0.f;
  s->gyro_z = 0.f;
  s->acc_x = 0.f;
  s->acc_y = 0.f;
  s->acc_z = acc_z;
  return b;
}

void fix_crc(std::vector<std::uint8_t>& b) {
  auto* h = reinterpret_cast<DataHeader*>(b.data());
  h->crc32 = crc32_iso_hdlc(b.data() + offsetof(DataHeader, timestamp),
                            b.size() - offsetof(DataHeader, timestamp));
}

// --- a driver wired to a real PageStore + EventBus ------------------------

struct Rig {
  EventBus bus;
  PageStore points;
  DriverContext ctx;

  Rig() : points(PageStoreConfig{1000, 8}) {
    ctx.bus = &bus;
    ctx.points = &points;
    ctx.clock = &fake_clock;
  }
};

Mid360Config inject_config() {
  Mid360Config c;
  c.backend = Mid360Backend::kInject;
  c.internal_supervisor_thread = false;  // tests drive tick() themselves
  c.live_points_per_sec = 0;             // no decimation: count every point
  c.max_batch_points = 96;               // one packet per flush, easy to reason about
  return c;
}

}  // namespace

// ===========================================================================
// Wire layout — the S2 cross-check, machine-checked.
// ===========================================================================

TEST_CASE("mid360/wire_layout_matches_the_S2_cross_check") {
  CHECK(sizeof(DataHeader) == 36);
  CHECK(sizeof(CartesianHigh) == 14);
  CHECK(sizeof(CartesianLow) == 8);
  CHECK(sizeof(ImuRaw) == 24);
  CHECK(offsetof(DataHeader, crc32) == 24);
  CHECK(offsetof(DataHeader, timestamp) == 28);
  CHECK(kPointPacketBytes == 1380);
  CHECK(kImuPacketBytes == 60);
  // The real device's ports, from the SDK's own define.h and the published
  // port table (they agreed).
  CHECK(kLidarPointPort == 56300);
  CHECK(kLidarImuPort == 56400);
}

TEST_CASE("mid360/parse_rejects_what_it_should") {
  PointPacket p = make_point_packet(1);
  CHECK(parse_packet(p.bytes.data(), p.bytes.size()).valid());

  // Too short for a header.
  CHECK_FALSE(parse_packet(p.bytes.data(), 10).valid());
  // `length` disagreeing with the datagram: a truncated read or a firmware
  // we do not understand. Either way the payload bounds are untrustworthy.
  CHECK_FALSE(parse_packet(p.bytes.data(), p.bytes.size() - 1).valid());
  // A data_type we never request (spherical / debug).
  PointPacket q = make_point_packet(2);
  q.header()->data_type = 7;
  CHECK_FALSE(parse_packet(q.bytes.data(), q.bytes.size()).valid());
  // dot_num claiming more points than the payload holds.
  PointPacket r = make_point_packet(3);
  r.header()->dot_num = 200;
  CHECK_FALSE(parse_packet(r.bytes.data(), r.bytes.size()).valid());

  const PacketView v = parse_packet(p.bytes.data(), p.bytes.size());
  CHECK(v.point_count == 96);
  CHECK(v.payload_bytes == 96 * sizeof(CartesianHigh));
}

TEST_CASE("mid360/crc32_covers_timestamp_onwards") {
  PointPacket p = make_point_packet(42);
  fix_crc(p.bytes);
  CHECK(crc32_ok(parse_packet(p.bytes.data(), p.bytes.size())));

  // One flipped payload bit must be caught.
  p.bytes[sizeof(DataHeader) + 3] ^= 0x01;
  CHECK_FALSE(crc32_ok(parse_packet(p.bytes.data(), p.bytes.size())));
}

// ===========================================================================
// Loss — the free-running udp_cnt model (S2's headline finding).
// ===========================================================================

TEST_CASE("mid360/loss_free_running_counter_is_the_model") {
  LossTracker t;
  std::uint32_t lost = 0;

  CHECK(t.observe(17473, &lost) == LossTracker::Step::kFirst);
  CHECK(lost == 0);
  CHECK(t.observe(17474, &lost) == LossTracker::Step::kInSequence);
  CHECK(lost == 0);

  // Three packets dropped in flight.
  CHECK(t.observe(17478, &lost) == LossTracker::Step::kLoss);
  CHECK(lost == 3);
  CHECK(t.lost() == 3);

  // A repeat of the same counter is a duplicate / stalled sender, NOT loss.
  CHECK(t.observe(17478, &lost) == LossTracker::Step::kDuplicate);
  CHECK(lost == 0);
  CHECK(t.duplicates() == 1);
  CHECK(t.lost() == 3);
}

TEST_CASE("mid360/loss_wraps_at_65535_without_inventing_a_loss_event") {
  // This is what "free-running" costs if you get the arithmetic wrong: a
  // uint16 wrap would look like 65,534 lost packets under signed or wider
  // arithmetic.
  LossTracker t;
  std::uint32_t lost = 0;
  CHECK(t.observe(65534, &lost) == LossTracker::Step::kFirst);
  CHECK(t.observe(65535, &lost) == LossTracker::Step::kInSequence);
  CHECK(t.observe(0, &lost) == LossTracker::Step::kInSequence);
  CHECK(t.observe(1, &lost) == LossTracker::Step::kInSequence);
  CHECK(t.lost() == 0);
}

TEST_CASE("mid360/loss_large_jump_is_a_reset_not_a_loss") {
  // The DOCUMENTED (per-frame-reset) firmware model, and a replay looping
  // back to the top of a file, both land here. Attributing it to loss is the
  // mistake the published protocol table invites; S2 measured the cost.
  LossTracker t;
  std::uint32_t lost = 0;
  CHECK(t.observe(20000, &lost) == LossTracker::Step::kFirst);
  CHECK(t.observe(0, &lost) == LossTracker::Step::kUnattributable);
  CHECK(lost == 0);
  CHECK(t.lost() == 0);
  CHECK(t.resets() == 1);
  // ...and it keeps counting from the new base.
  CHECK(t.observe(1, &lost) == LossTracker::Step::kInSequence);
}

TEST_CASE("mid360/loss_fraction_matches_injected_loss") {
  // S2 Run C injected 1.961% and the detector reported 1.9612%. Same rule,
  // deterministic here: drop every 50th packet out of 5,000.
  LossTracker t;
  std::uint16_t cnt = 0;
  int sent = 0;
  for (int i = 0; i < 5000; ++i) {
    if (i % 50 == 49) {  // this one never arrives
      ++cnt;
      continue;
    }
    (void)t.observe(cnt++);
    ++sent;
  }
  CHECK(t.packets() == static_cast<std::uint64_t>(sent));
  CHECK(t.lost() == 99);  // the first dropped packet has no predecessor gap
  CHECK(t.loss_fraction() == doctest::Approx(99.0 / 5000.0).epsilon(0.01));
}

// ===========================================================================
// The point filter — defaults chosen from the REAL fixtures, not the sim.
// ===========================================================================

TEST_CASE("mid360/filter_defaults_drop_no_returns") {
  PointFilterConfig cfg;  // defaults
  FilterStats st;

  CartesianHigh good{1000, 2000, 3000, 120, 0};
  CartesianHigh no_return{0, 0, 0, 0, 0};

  CHECK(point_passes(good, cfg, &st));
  CHECK_FALSE(point_passes(no_return, cfg, &st));
  CHECK(st.dropped_no_return == 1);
  CHECK(st.kept == 1);
  CHECK(st.seen == 2);

  // ...and can be told not to, for a diagnostic / post-processing run.
  cfg.drop_no_return = false;
  cfg.min_range_m = 0.f;
  CHECK(point_passes(no_return, cfg, nullptr));
}

TEST_CASE("mid360/filter_tag_policy_against_the_real_indoor_histogram") {
  // The measured tag distribution of a real 5 s indoor slice
  // (spikes/s2-mid360-sim/FIXTURES.md §1, 1,000,128 points). The simulator
  // emits tag == 0 for everything, so this histogram is the ONLY thing that
  // exercises the tag path — which is exactly why the fixture exists.
  struct Bin { std::uint8_t tag; std::uint32_t n; };
  const Bin hist[] = {{0, 983484}, {1, 5638}, {2, 1549},  {4, 3959}, {5, 34},   {6, 9},
                      {8, 573},    {9, 1},    {16, 4607}, {18, 1},   {32, 273}};

  PointFilterConfig cfg;  // defaults: reject spatial-noise bits only
  CHECK(cfg.tag_reject_mask == kTagSpatialNoiseMask);

  std::uint64_t total = 0, kept = 0;
  FilterStats st;
  for (const Bin& b : hist) {
    CartesianHigh p{1500, 0, 0, 90, b.tag};
    const bool ok = point_passes(p, cfg, nullptr);
    total += b.n;
    if (ok) kept += b.n;
    (void)st;
  }
  CHECK(total == 1000128u);
  // Rejected: tags 4, 5, 6 (spatial level 1) and 8, 9 (level 2).
  const std::uint64_t rejected = 3959 + 34 + 9 + 573 + 1;
  CHECK(kept == total - rejected);
  CHECK(rejected * 10000 / total == 45);  // 0.45%

  // Intensity-noise flags (16, 32) are deliberately KEPT by default: the
  // geometry of a retro-reflector return is fine even when its intensity is
  // not. Asking for 0x3C drops them too.
  cfg.tag_reject_mask = static_cast<std::uint8_t>(kTagSpatialNoiseMask | kTagIntensityNoiseMask);
  CHECK_FALSE(point_passes(CartesianHigh{1500, 0, 0, 90, 16}, cfg, nullptr));
  CHECK_FALSE(point_passes(CartesianHigh{1500, 0, 0, 90, 32}, cfg, nullptr));
  // Return-number bits are never a rejection reason under either mask.
  CHECK(point_passes(CartesianHigh{1500, 0, 0, 90, 1}, cfg, nullptr));
  CHECK(point_passes(CartesianHigh{1500, 0, 0, 90, 2}, cfg, nullptr));
}

TEST_CASE("mid360/filter_tag_accessors") {
  CHECK(tag_return_number(0x03) == 3);
  CHECK(tag_spatial_noise(0x04) == 1);
  CHECK(tag_spatial_noise(0x08) == 2);
  CHECK(tag_intensity_noise(0x10) == 1);
  CHECK(tag_intensity_noise(0x20) == 2);
  CHECK(tag_spatial_noise(0x30) == 0);  // intensity bits must not leak across
}

TEST_CASE("mid360/filter_range_and_reflectivity_are_configurable") {
  PointFilterConfig cfg;
  cfg.min_range_m = 0.5f;
  cfg.max_range_m = 10.0f;
  cfg.min_reflectivity = 20;
  FilterStats st;

  CHECK_FALSE(point_passes(CartesianHigh{100, 0, 0, 200, 0}, cfg, &st));   // 0.1 m
  CHECK_FALSE(point_passes(CartesianHigh{20000, 0, 0, 200, 0}, cfg, &st)); // 20 m
  CHECK_FALSE(point_passes(CartesianHigh{2000, 0, 0, 5, 0}, cfg, &st));    // too dim
  CHECK(point_passes(CartesianHigh{2000, 0, 0, 200, 0}, cfg, &st));
  CHECK(st.dropped_range == 2);
  CHECK(st.dropped_reflectivity == 1);
  CHECK(st.kept == 1);
  CHECK(st.keep_fraction() == doctest::Approx(0.25));
}

// ===========================================================================
// The driver: decode → PageStore, IMU → ring (never the PageStore).
// ===========================================================================

TEST_CASE("mid360/driver_converts_mm_to_metres_and_fills_the_page_store") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());
  CHECK(d.state() == DeviceState::kStarting);

  PointPacket p = make_point_packet(100);
  d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());

  CHECK(d.state() == DeviceState::kStreaming);  // first data promotes
  const Mid360Stats st = d.stats();
  CHECK(st.point_packets == 1);
  CHECK(st.points_received == 96);
  CHECK(st.points_kept == 96);
  CHECK(st.points_appended == 96);

  const auto ids = rig.points.page_ids();
  REQUIRE(ids.size() == 1);
  const PageView v = rig.points.page_view(ids[0]);
  REQUIRE(v.valid());
  CHECK(v.stream == StreamId::kLidarMid360);  // provenance survives to export
  CHECK(v.count == 96);
  // 1000 mm, 2000 mm, −500 mm → metres.
  CHECK(v.data[0].x == doctest::Approx(1.0f));
  CHECK(v.data[0].y == doctest::Approx(2.0f));
  CHECK(v.data[0].z == doctest::Approx(-0.5f));
  CHECK(v.data[95].x == doctest::Approx(1.095f));
  CHECK(v.data[0].r == 100);  // reflectivity carried as greyscale
  CHECK(v.data[0].a == 255);

  CHECK(d.stop().ok());
}

TEST_CASE("mid360/driver_drops_no_returns_out_of_the_cloud") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());

  PointPacket p = make_point_packet(1);
  for (std::uint16_t i = 0; i < 96; ++i) {
    if (i % 3 == 0) {  // 32 no-returns, the real-data failure mode
      p.points()[i].x = 0;
      p.points()[i].y = 0;
      p.points()[i].z = 0;
    }
    if (i == 1) p.points()[i].tag = 0x04;  // one spatial-noise flag
  }
  d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());

  const Mid360Stats st = d.stats();
  CHECK(st.points_received == 96);
  CHECK(st.filter.dropped_no_return == 32);
  CHECK(st.filter.dropped_tag == 1);
  CHECK(st.points_kept == 63);
  // 63 survivors is under the 96-point batch threshold, so nothing has
  // reached the store yet — batching is what keeps 2,083 packets a second
  // from becoming 2,083 PageStore locks.
  CHECK(st.points_appended == 0);
  CHECK(d.stop().ok());  // ...and stop() flushes the remainder.
  CHECK(d.stats().points_appended == 63);
  CHECK(rig.points.total_points() == 63);
}

TEST_CASE("mid360/driver_decimates_deterministically_to_the_live_budget") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.live_points_per_sec = 40000;  // Tech Spec §3.3: 1-in-5 of 200k
  cfg.max_batch_points = 8;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  for (std::uint16_t k = 0; k < 5; ++k) {
    PointPacket p = make_point_packet(k);
    d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
  }
  const Mid360Stats st = d.stats();
  CHECK(st.points_received == 480);
  CHECK(st.points_kept == 480);
  // 1-in-5 of 480, modulo whatever is still sitting in the partial batch.
  CHECK(st.points_appended >= 88);
  CHECK(st.points_appended <= 96);
  CHECK(d.stop().ok());
  CHECK(d.stats().points_appended == 96);  // stop() flushes the partial batch
}

TEST_CASE("mid360/imu_never_enters_the_page_store") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());

  for (std::uint16_t i = 0; i < 10; ++i) {
    auto b = make_imu_packet(i, 0.25f, 1.0f);
    d.on_imu_packet(b.data(), b.size(), fake_clock());
  }

  CHECK(d.stats().imu_packets == 10);
  // THE RULE: IMU is not geometry. Nothing reached the cloud.
  CHECK(rig.points.total_points() == 0);
  CHECK(rig.points.page_count() == 0);

  Mid360ImuSample out[16];
  const std::size_t n = d.drain_imu(out, 16);
  CHECK(n == 10);
  CHECK(out[0].gyro[0] == doctest::Approx(0.25f));
  CHECK(out[0].acc[2] == doctest::Approx(1.0f));  // g, as the device reports it
  CHECK(out[0].t_device_ns == 5'000'000'000ULL);
  CHECK(d.drain_imu(out, 16) == 0);  // drained
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/imu_ring_is_bounded_and_reports_overflow") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.imu_ring_capacity = 4;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  for (std::uint16_t i = 0; i < 10; ++i) {
    auto b = make_imu_packet(i, static_cast<float>(i), 1.0f);
    d.on_imu_packet(b.data(), b.size(), fake_clock());
  }
  CHECK(d.stats().imu_dropped == 6);

  Mid360ImuSample out[8];
  const std::size_t n = d.drain_imu(out, 8);
  CHECK(n == 4);
  CHECK(out[0].gyro[0] == doctest::Approx(6.f));  // oldest survivors
  CHECK(out[3].gyro[0] == doctest::Approx(9.f));
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/imu_sink_receives_every_sample") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  static std::vector<Mid360ImuSample> got;
  got.clear();
  Mid360Config cfg = inject_config();
  cfg.imu_sink = [](const Mid360ImuSample* s, std::size_t n, void*) {
    for (std::size_t i = 0; i < n; ++i) got.push_back(s[i]);
  };
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());
  for (std::uint16_t i = 0; i < 5; ++i) {
    auto b = make_imu_packet(i, 1.f, 1.f);
    d.on_imu_packet(b.data(), b.size(), fake_clock());
  }
  CHECK(got.size() == 5);
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/bad_packets_are_counted_not_decoded") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.verify_crc = true;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  PointPacket good = make_point_packet(1);
  fix_crc(good.bytes);
  d.on_point_packet(good.bytes.data(), good.bytes.size(), fake_clock());

  PointPacket bad = make_point_packet(2);  // CRC left at 0
  d.on_point_packet(bad.bytes.data(), bad.bytes.size(), fake_clock());

  const std::uint8_t garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  d.on_point_packet(garbage, sizeof(garbage), fake_clock());

  const Mid360Stats st = d.stats();
  CHECK(st.point_packets == 1);
  CHECK(st.bad_packets == 2);
  CHECK(st.points_appended == 96);
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/push_bytes_routes_datagrams_only_for_the_inject_backend") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Driver d(1, inject_config(), rig.ctx);
  REQUIRE(d.start().ok());

  PointPacket p = make_point_packet(7);
  CHECK(d.push_bytes(ByteSpan(p.bytes.data(), p.bytes.size()), fake_clock()).ok());
  auto imu = make_imu_packet(7, 0.5f, 1.f);
  CHECK(d.push_bytes(ByteSpan(imu.data(), imu.size()), fake_clock()).ok());
  CHECK(d.stats().point_packets == 1);
  CHECK(d.stats().imu_packets == 1);
  CHECK(d.push_bytes(ByteSpan(p.bytes.data(), 4), fake_clock()).error() ==
        ScanError::kProtocolError);
  CHECK(d.stop().ok());

  // ...and is refused on a socket-owning backend.
  Mid360Config sdk = inject_config();
  sdk.backend = Mid360Backend::kRawUdp;
  Mid360Driver e(2, sdk, rig.ctx);
  CHECK(e.push_bytes(ByteSpan(p.bytes.data(), p.bytes.size()), fake_clock()).error() ==
        ScanError::kNotSupported);
}

// ===========================================================================
// Health and the reconnect state machine.
// ===========================================================================

TEST_CASE("mid360/health_window_reports_rates_and_loss") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.health_period_ms = 1000;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  // A second of traffic at 1/10th the real rate, with 5 packets lost.
  std::uint16_t cnt = 0;
  for (int i = 0; i < 208; ++i) {
    if (i >= 100 && i < 105) {  // a 5-packet burst never arrives
      ++cnt;
      continue;
    }
    PointPacket p = make_point_packet(cnt++);
    d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
    if (i % 10 == 0) {
      auto b = make_imu_packet(static_cast<std::uint16_t>(i), 0.f, 1.f);
      d.on_imu_packet(b.data(), b.size(), fake_clock());
    }
    advance_ms(5);
  }
  d.tick(fake_clock());

  const Mid360Stats st = d.stats();
  CHECK(st.packets_lost == 5);
  CHECK(st.points_per_sec > 0.0);
  CHECK(st.imu_hz > 0.0);
  CHECK(st.loss_pct_window > 2.0);
  CHECK(st.loss_pct_window < 3.0);
  // Sustained loss above max_loss_pct demotes, and says why.
  CHECK(d.state() == DeviceState::kDegraded);
  CHECK(d.health().last_error == ScanError::kNetworkError);
  CHECK(d.health().checksum_pass_rate < 1.0);
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/watchdog_is_the_only_thing_that_can_see_a_link_drop") {
  // S2's Scenario A: a 15 s cable pull produced ZERO counted losses, three
  // times over, because the device's udp_cnt keeps advancing while the wire
  // is down. The wall-clock watchdog is the primary outage signal; the
  // counter is for in-band loss on a live link.
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.reconnect.data_timeout_ms = 1000;
  cfg.reconnect.reinit_after_silence_ms = 5000;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  std::uint16_t cnt = 0;
  for (int i = 0; i < 10; ++i) {
    PointPacket p = make_point_packet(cnt++);
    d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
    advance_ms(50);
    d.tick(fake_clock());
  }
  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);

  // --- the wire goes away for 3 s; the device keeps counting -------------
  // At the real 2,083 pkt/s the device's counter advances ~6,250 times
  // during a 3 s outage, so the first packet after resume shows a gap far
  // past kResetThreshold and lands in the "not attributable" bucket. That is
  // why S2 measured ZERO counted losses across a 15 s cable pull, three
  // times over: the counter simply cannot see an outage of any real length.
  const int missed = 6250;
  cnt = static_cast<std::uint16_t>(cnt + missed);
  advance_ms(1500);
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid360LinkState::kSilent);
  CHECK(d.state() == DeviceState::kDegraded);
  CHECK(d.health().last_error == ScanError::kDeviceNotResponding);
  CHECK(d.stats().watchdog_trips == 1);

  advance_ms(1500);
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid360LinkState::kSilent);  // not yet re-init time

  // --- the cable is plugged back in; the SDK recovers on its own ---------
  PointPacket p = make_point_packet(cnt++);
  d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
  d.tick(fake_clock());

  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.stats().clean_resumes == 1);
  CHECK(d.stats().forced_reinits == 0);  // a cable pull needs NO re-init
  // And the punchline: the counter saw nothing at all.
  CHECK(d.stats().packets_lost == 0);
  CHECK(d.stats().counter_resets == 1);  // the outage showed up HERE, unattributed
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/prolonged_silence_forces_a_full_sdk_reinit") {
  // S2's Scenario B: a power-cycled device is never re-configured by the
  // SDK — HandleDetectionData() returns early for a handle it already knows
  // — so it stays dead forever. The driver must force a teardown/re-init
  // instead of waiting for a self-heal that cannot come.
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.reconnect.data_timeout_ms = 1000;
  cfg.reconnect.reinit_after_silence_ms = 5000;
  cfg.reconnect.reinit_backoff_initial_ms = 1000;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  PointPacket p = make_point_packet(1);
  d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid360LinkState::kUp);

  // Silence past the watchdog...
  advance_ms(1200);
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid360LinkState::kSilent);
  CHECK(d.stats().forced_reinits == 0);

  // ...and on past the re-init threshold.
  advance_ms(4000);
  d.tick(fake_clock());
  CHECK(d.stats().forced_reinits == 1);
  CHECK(d.link_state() == Mid360LinkState::kReinitializing);
  CHECK(d.state() == DeviceState::kDegraded);

  // Still dead: the next attempt waits for the backoff window rather than
  // spinning socket setup once per tick.
  advance_ms(1000);
  d.tick(fake_clock());
  CHECK(d.stats().forced_reinits == 1);

  advance_ms(5000);
  d.tick(fake_clock());
  CHECK(d.stats().forced_reinits == 2);

  // The device comes back after the re-init: that is the power-cycle path,
  // and it is counted separately from a clean resume.
  PointPacket q = make_point_packet(2);
  d.on_point_packet(q.bytes.data(), q.bytes.size(), fake_clock());
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.stats().clean_resumes == 0);  // NOT a cable-class recovery
  CHECK(d.stats().forced_reinits == 2);
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/reconnect_can_be_capped_and_then_faults") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.reconnect.data_timeout_ms = 500;
  cfg.reconnect.reinit_after_silence_ms = 1000;
  cfg.reconnect.reinit_backoff_initial_ms = 100;
  cfg.reconnect.max_reinits = 2;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());

  PointPacket p = make_point_packet(1);
  d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
  d.tick(fake_clock());

  for (int i = 0; i < 8; ++i) {
    advance_ms(1500);
    d.tick(fake_clock());
  }
  CHECK(d.stats().forced_reinits == 2);
  CHECK(d.state() == DeviceState::kFault);
  CHECK(d.stop().ok());
}

TEST_CASE("mid360/watchdog_can_be_disabled") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg = inject_config();
  cfg.reconnect.enabled = false;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());
  PointPacket p = make_point_packet(1);
  d.on_point_packet(p.bytes.data(), p.bytes.size(), fake_clock());
  d.tick(fake_clock());
  advance_ms(60000);
  d.tick(fake_clock());
  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(d.stats().watchdog_trips == 0);
  CHECK(d.stats().forced_reinits == 0);
  CHECK(d.stop().ok());
}

// ===========================================================================
// Configuration guard rails.
// ===========================================================================

TEST_CASE("mid360/explicit_lidar_and_host_ip_are_required") {
  g_now_ns = 1'000'000'000;
  Rig rig;

  // There is no broadcast discovery on macOS (S2 REPORT.md §3): failing here
  // by name beats waiting forever for an answer that cannot arrive.
  Mid360Config cfg;
  cfg.backend = Mid360Backend::kRawUdp;
  cfg.internal_supervisor_thread = false;
  Mid360Driver d(1, cfg, rig.ctx);
  CHECK(d.start().error() == ScanError::kInvalidArgument);
  CHECK(std::string(last_error_message()).find("lidar_ip") != std::string::npos);
  CHECK(d.state() == DeviceState::kFault);

  cfg.udp.lidar_ip = "192.168.1.100";
  Mid360Driver e(2, cfg, rig.ctx);
  CHECK(e.start().error() == ScanError::kInvalidArgument);
  CHECK(std::string(last_error_message()).find("host_ip") != std::string::npos);
}

TEST_CASE("mid360/sdk2_backend_reports_its_absence_usefully") {
  g_now_ns = 1'000'000'000;
  Rig rig;
  Mid360Config cfg;
  cfg.backend = Mid360Backend::kSdk2;
  cfg.udp.lidar_ip = "192.168.1.100";
  cfg.udp.host_ip = "192.168.1.5";  // not a local address in CI
  cfg.internal_supervisor_thread = false;
  Mid360Driver d(1, cfg, rig.ctx);

  const Status s = d.start();
  CHECK_FALSE(s.ok());
  const std::string msg = last_error_message();
  if (s.error() == ScanError::kNotSupported) {
    // Built without the SDK: the message must name the way out.
    CHECK(msg.find("fetch_sdk2.sh") != std::string::npos);
  } else {
    // Built WITH the SDK, but there is no 192.168.1.5 on this machine.
    CHECK(s.error() == ScanError::kIoError);
    CHECK(msg.find("host_ip") != std::string::npos);
  }
  CHECK(d.state() == DeviceState::kFault);
}

TEST_CASE("mid360/to_string_covers_every_enumerator") {
  CHECK(std::string(to_string(Mid360Backend::kSdk2)) == "sdk2");
  CHECK(std::string(to_string(Mid360Backend::kRawUdp)) == "raw-udp");
  CHECK(std::string(to_string(Mid360Backend::kInject)) == "inject");
  CHECK(std::string(to_string(Mid360LinkState::kDown)) == "down");
  CHECK(std::string(to_string(Mid360LinkState::kWaiting)) == "waiting");
  CHECK(std::string(to_string(Mid360LinkState::kUp)) == "up");
  CHECK(std::string(to_string(Mid360LinkState::kSilent)) == "silent");
  CHECK(std::string(to_string(Mid360LinkState::kReinitializing)) == "reinitializing");
}

// ===========================================================================
// UdpSource — a real socket, on loopback.
// ===========================================================================

TEST_CASE("mid360/udp_source_receives_datagrams_on_loopback") {
  UdpConfig cfg;
  cfg.host_ip = "127.0.0.1";
  cfg.lidar_ip = "127.0.0.1";
  cfg.bind_port = 0;
  cfg.host_point_port = 57391;  // out of the way of the Livox 563xx range
  cfg.recv_buffer_bytes = 1 << 20;

  std::vector<std::size_t> got;
  std::mutex got_m;
  UdpSource src(cfg);
  src.set_sink([&](ByteSpan d, TimePoint) {
    std::lock_guard<std::mutex> lk(got_m);
    got.push_back(d.size());
  });
  const Status started = src.start();
  if (!started.ok()) {
    MESSAGE("UdpSource bind unavailable in this environment: " << last_error_message());
    return;  // a sandbox with no loopback socket permission must not fail CI
  }
  CHECK(src.running());
  CHECK(src.bound_port() == 57391);
  CHECK(src.udp_stats().recv_buffer_granted > 0);

  // Send to ourselves through the source's own send path.
  UdpConfig tx = cfg;
  tx.cmd_port = 57391;  // aim send() at the listening port
  tx.bind_port = 57392;
  UdpSource sender(tx);
  sender.set_sink([](ByteSpan, TimePoint) {});
  REQUIRE(sender.start().ok());

  PointPacket p = make_point_packet(1);
  for (int i = 0; i < 5; ++i) {
    CHECK(sender.send(ByteSpan(p.bytes.data(), p.bytes.size())).ok());
  }

  for (int i = 0; i < 200; ++i) {
    {
      std::lock_guard<std::mutex> lk(got_m);
      if (got.size() >= 5) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  {
    std::lock_guard<std::mutex> lk(got_m);
    CHECK(got.size() == 5);
    for (std::size_t n : got) CHECK(n == kPointPacketBytes);
  }
  CHECK(src.udp_stats().datagrams == 5);
  CHECK(src.stats().bytes_in == 5 * kPointPacketBytes);

  CHECK(sender.stop().ok());
  CHECK(src.stop().ok());
  CHECK_FALSE(src.running());
}

TEST_CASE("mid360/udp_source_refuses_to_start_without_a_sink") {
  UdpConfig cfg;
  cfg.host_ip = "127.0.0.1";
  UdpSource src(cfg);
  CHECK(src.start().error() == ScanError::kInvalidState);
}

// ===========================================================================
// END-TO-END AGAINST THE S2 SIMULATOR.
//
// These cases carry doctest::skip(), so a normal `ctest` / `scanengine_tests`
// run reports them as skipped and costs nothing. CMake registers a separate
// ctest entry labelled "sim" which re-invokes this binary with --no-skip:
//
//     ctest -L sim
//
// They need the S2 spike built (spikes/s2-mid360-sim: scripts/fetch_sdk2.sh,
// then cmake + build) and this engine built WITH Livox-SDK2. Anything
// missing makes them skip loudly rather than fail — the spike's build tree
// is gitignored, so a fresh clone legitimately has none of it.
//
// What they prove that a unit test cannot: the real SDK2 handshake, the real
// callback path at 200k pts/s, and — the part S2 could only describe —
// that a forced teardown/re-init actually recovers a device that has
// forgotten its configuration.
// ===========================================================================

#if !defined(_WIN32)

#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// One child process (mid360_sim or lvx2_replay), stdout+stderr to a log.
class Child {
 public:
  ~Child() { terminate(); }

  bool spawn(const std::vector<std::string>& argv, const std::string& log_path) {
    std::vector<char*> c;
    c.reserve(argv.size() + 1);
    for (const std::string& a : argv) c.push_back(const_cast<char*>(a.c_str()));
    c.push_back(nullptr);

    pid_ = ::fork();
    if (pid_ < 0) return false;
    if (pid_ == 0) {
      // Child: the simulator is chatty and its log is the cross-check for
      // every number this test reports, so keep it.
      ::freopen(log_path.c_str(), "w", stdout);
      ::dup2(1, 2);
      ::execv(c[0], c.data());
      ::_exit(127);
    }
    return true;
  }

  void terminate() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGTERM);
    int status = 0;
    for (int i = 0; i < 50; ++i) {
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::kill(pid_, SIGKILL);
    (void)::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

  bool alive() const { return pid_ > 0; }

 private:
  pid_t pid_ = -1;
};

std::string env_or(const char* key, const std::string& fallback) {
  const char* v = std::getenv(key);
  return (v != nullptr && v[0] != '\0') ? std::string(v) : fallback;
}

int env_int(const char* key, int fallback) {
  const char* v = std::getenv(key);
  return (v != nullptr && v[0] != '\0') ? std::atoi(v) : fallback;
}

bool file_exists(const std::string& p) {
  return !p.empty() && ::access(p.c_str(), X_OK | F_OK) == 0;
}

std::string log_path(const char* leaf) {
  return env_or("TMPDIR", "/tmp") + "/scanengine_a3_" + leaf + ".log";
}

// The loopback configuration S2 established, and the one place its two
// deliberate hacks are used:
//   • host_ip is spelled "127.000.000.001" — numerically identical to
//     127.0.0.1 for inet_addr(), but a different STRING, which is what slips
//     past the SDK's `if (lidar_ip == detection_host_ip_) return;` self-IP
//     filter (device_manager.cpp:472). Without it the SDK discards every
//     packet the simulator sends, because on loopback both sides share an
//     address. This MUST NOT appear in production config.
//   • the ports are the real Mid-360 ports; the simulator binds 0.0.0.0:56000
//     with SO_REUSEADDR while the SDK binds 127.0.0.1:56000, and BSD delivers
//     unicast to the more specific bind.
Mid360Config sim_driver_config() {
  Mid360Config c;
  c.backend = Mid360Backend::kSdk2;
  c.udp.lidar_ip = "127.0.0.1";
  c.udp.host_ip = "127.000.000.001";
  c.live_points_per_sec = 40000;  // the Tech Spec §3.3 live budget, on by default
  c.max_batch_points = 8192;
  c.health_period_ms = 1000;
  c.publish_imu = true;
  c.imu_ring_capacity = 8192;
  c.internal_supervisor_thread = true;
  return c;
}

struct SimRig {
  EventBus bus;
  PageStore points;
  DriverContext ctx;
  SimRig() : points(PageStoreConfig{1u << 20, 64}) {
    ctx.bus = &bus;
    ctx.points = &points;
    // The real steady clock here: this is a timing test.
  }
};

// Wait for the driver to reach kStreaming (or give up). Returns seconds waited.
double wait_for_stream(Mid360Driver& d, double max_s) {
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < max_s) {
    if (d.stats().point_packets > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void report(const char* title, const Mid360Stats& s, double elapsed_s) {
  // doctest stringifies a bare const char* as a pointer; wrap it.
  MESSAGE("--- " << std::string(title) << " ---");
  MESSAGE("  duration            : " << elapsed_s << " s");
  MESSAGE("  point packets       : " << s.point_packets);
  MESSAGE("  points received     : " << s.points_received);
  MESSAGE("  mean point rate     : " << (s.points_received / elapsed_s) << " pts/s");
  MESSAGE("  points to PageStore : " << s.points_appended << "  ("
                                     << (s.points_appended / elapsed_s) << " pts/s)");
  MESSAGE("  IMU packets         : " << s.imu_packets << "  ("
                                     << (s.imu_packets / elapsed_s) << " Hz)");
  MESSAGE("  packets lost        : " << s.packets_lost << "  (" << s.loss_pct_total << " %)");
  MESSAGE("  duplicates / resets : " << s.packets_duplicated << " / " << s.counter_resets);
  MESSAGE("  bad packets         : " << s.bad_packets);
  MESSAGE("  no-return dropped   : " << s.filter.dropped_no_return);
  MESSAGE("  tag dropped         : " << s.filter.dropped_tag);
  MESSAGE("  watchdog trips      : " << s.watchdog_trips);
  MESSAGE("  clean resumes       : " << s.clean_resumes);
  MESSAGE("  forced re-inits     : " << s.forced_reinits << " (failures "
                                     << s.reinit_failures << ")");
  MESSAGE("  link / state        : " << std::string(to_string(s.link)) << " / "
                                     << std::string(to_string(s.state)));
}

// Returns "" when everything the case needs is present, else why it skipped.
std::string sim_preflight(std::string* sim_bin) {
  *sim_bin = env_or("SCANENGINE_SIM_BIN", "");
  if (!file_exists(*sim_bin)) {
    return "mid360_sim not found (set SCANENGINE_SIM_BIN, or build spikes/s2-mid360-sim)";
  }
  Mid360Config probe;
  probe.backend = Mid360Backend::kSdk2;
  probe.udp.lidar_ip = "127.0.0.1";
  probe.udp.host_ip = "127.000.000.001";
  probe.internal_supervisor_thread = false;
  EventBus bus;
  PageStore ps(PageStoreConfig{16, 1});
  DriverContext ctx;
  ctx.bus = &bus;
  ctx.points = &ps;
  Mid360Driver d(99, probe, ctx);
  const Status s = d.start();
  if (s.error() == ScanError::kNotSupported) {
    return "engine built without Livox-SDK2 (run engine/third_party/fetch_sdk2.sh)";
  }
  (void)d.stop();
  return "";
}

}  // namespace

TEST_CASE("mid360sim/soak_against_the_simulator" * doctest::skip()) {
  std::string sim_bin;
  const std::string why = sim_preflight(&sim_bin);
  if (!why.empty()) {
    MESSAGE("SKIPPED: " << why);
    return;
  }

  const int seconds = env_int("SCANENGINE_SIM_SECONDS", 60);
  Child sim;
  REQUIRE(sim.spawn({sim_bin, "--duration", std::to_string(seconds + 20), "--frame-model",
                     "real"},
                    log_path("soak_sim")));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  SimRig rig;
  Mid360Driver d(1, sim_driver_config(), rig.ctx);
  REQUIRE(d.start().ok());

  const double handshake_s = wait_for_stream(d, 20.0);
  MESSAGE("handshake + first packet: " << handshake_s << " s");
  REQUIRE(d.stats().point_packets > 0);

  // Measure a clean window that excludes the handshake.
  const Mid360Stats base = d.stats();
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const Mid360Stats end = d.stats();

  Mid360Stats win = end;
  win.point_packets = end.point_packets - base.point_packets;
  win.points_received = end.points_received - base.points_received;
  win.points_appended = end.points_appended - base.points_appended;
  win.imu_packets = end.imu_packets - base.imu_packets;
  win.packets_lost = end.packets_lost - base.packets_lost;
  report("60 s soak, --frame-model real", win, elapsed);

  const double pps = win.points_received / elapsed;
  const double imu_hz = win.imu_packets / elapsed;
  CHECK(pps > 0.97 * kNominalPointsPerSec);
  CHECK(pps < 1.03 * kNominalPointsPerSec);
  CHECK(imu_hz > 0.97 * kNominalImuHz);
  CHECK(imu_hz < 1.03 * kNominalImuHz);
  CHECK(win.packets_lost * 1000 < win.point_packets);  // < 0.1% loss
  CHECK(win.bad_packets == 0);
  CHECK(end.counter_resets == 0);  // real firmware model: udp_cnt never resets
  CHECK(d.state() == DeviceState::kStreaming);
  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(rig.points.total_points() > 0);

  // The IMU went to the ring, never to the cloud.
  std::vector<Mid360ImuSample> imu(4096);
  const std::size_t n = d.drain_imu(imu.data(), imu.size());
  CHECK(n > 0);
  double acc_mag = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    acc_mag += std::sqrt(static_cast<double>(imu[i].acc[0]) * imu[i].acc[0] +
                         static_cast<double>(imu[i].acc[1]) * imu[i].acc[1] +
                         static_cast<double>(imu[i].acc[2]) * imu[i].acc[2]);
  }
  MESSAGE("  IMU mean |acc|      : " << (acc_mag / static_cast<double>(n)) << " g");
  CHECK(acc_mag / static_cast<double>(n) == doctest::Approx(1.0).epsilon(0.05));

  CHECK(d.stop().ok());
  sim.terminate();
}

TEST_CASE("mid360sim/injected_loss_is_measured_by_the_free_running_counter" * doctest::skip()) {
  std::string sim_bin;
  const std::string why = sim_preflight(&sim_bin);
  if (!why.empty()) {
    MESSAGE("SKIPPED: " << why);
    return;
  }

  Child sim;
  REQUIRE(sim.spawn({sim_bin, "--duration", "45", "--loss", "2", "--frame-model", "real"},
                    log_path("loss_sim")));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  SimRig rig;
  Mid360Driver d(1, sim_driver_config(), rig.ctx);
  REQUIRE(d.start().ok());
  REQUIRE(wait_for_stream(d, 20.0) < 20.0);

  const Mid360Stats base = d.stats();
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(20));
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const Mid360Stats end = d.stats();

  const std::uint64_t pkts = end.point_packets - base.point_packets;
  const std::uint64_t lost = end.packets_lost - base.packets_lost;
  const double loss_pct = 100.0 * static_cast<double>(lost) / static_cast<double>(pkts + lost);
  MESSAGE("--- 2% injected loss, 20 s ---");
  MESSAGE("  packets received    : " << pkts);
  MESSAGE("  packets lost        : " << lost << "  (" << loss_pct << " %)");
  MESSAGE("  duplicates          : " << (end.packets_duplicated - base.packets_duplicated));
  MESSAGE("  point rate          : " << ((end.points_received - base.points_received) / elapsed)
                                     << " pts/s");

  // S2 measured 1.9612% detected against 1.961% injected. Allow slack for
  // the window boundaries, not for the model.
  CHECK(loss_pct > 1.5);
  CHECK(loss_pct < 2.5);
  CHECK((end.packets_duplicated - base.packets_duplicated) == 0);
  CHECK(end.counter_resets == 0);
  // Sustained 2% loss is above max_loss_pct, so the device must SAY so.
  CHECK(d.state() == DeviceState::kDegraded);

  CHECK(d.stop().ok());
  sim.terminate();
}

TEST_CASE("mid360sim/cable_pull_recovers_with_no_reinit" * doctest::skip()) {
  std::string sim_bin;
  const std::string why = sim_preflight(&sim_bin);
  if (!why.empty()) {
    MESSAGE("SKIPPED: " << why);
    return;
  }

  // A plain link drop: the device keeps counting through the outage and the
  // SDK resumes on its own when the wire comes back.
  Child sim;
  REQUIRE(sim.spawn({sim_bin, "--duration", "45", "--drop-link-after", "10", "--link-down-for",
                     "6"},
                    log_path("cable_sim")));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  SimRig rig;
  Mid360Config cfg = sim_driver_config();
  cfg.reconnect.data_timeout_ms = 1000;
  cfg.reconnect.reinit_after_silence_ms = 15000;  // longer than the outage
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());
  REQUIRE(wait_for_stream(d, 20.0) < 20.0);

  std::this_thread::sleep_for(std::chrono::seconds(25));
  const Mid360Stats s = d.stats();
  report("cable pull: 6 s outage at t=10 s", s, 25.0);

  CHECK(s.watchdog_trips >= 1);
  CHECK(s.clean_resumes >= 1);
  CHECK(s.forced_reinits == 0);  // a live link needs no help
  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);
  // The point of the whole watchdog: the counter saw (almost) nothing.
  MESSAGE("  udp_cnt loss across a 6 s outage: " << s.packets_lost);

  CHECK(d.stop().ok());
  sim.terminate();
}

TEST_CASE("mid360sim/power_cycle_needs_a_forced_reinit" * doctest::skip()) {
  std::string sim_bin;
  const std::string why = sim_preflight(&sim_bin);
  if (!why.empty()) {
    MESSAGE("SKIPPED: " << why);
    return;
  }

  // --restart-identity clears everything the host configured before the link
  // comes back: the device forgets it was ever told where to stream. S2
  // showed the SDK never recovers from this on its own.
  Child sim;
  REQUIRE(sim.spawn({sim_bin, "--duration", "70", "--drop-link-after", "10", "--link-down-for",
                     "5", "--restart-identity"},
                    log_path("powercycle_sim")));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  SimRig rig;
  Mid360Config cfg = sim_driver_config();
  cfg.reconnect.data_timeout_ms = 1000;
  cfg.reconnect.reinit_after_silence_ms = 5000;
  cfg.reconnect.reinit_backoff_initial_ms = 2000;
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());
  REQUIRE(wait_for_stream(d, 20.0) < 20.0);

  const Mid360Stats before = d.stats();
  std::this_thread::sleep_for(std::chrono::seconds(45));
  const Mid360Stats s = d.stats();
  report("power-cycle: identity reset at t=10 s", s, 45.0);

  CHECK(s.watchdog_trips >= 1);
  CHECK(s.forced_reinits >= 1);
  CHECK(s.point_packets > before.point_packets);

  // THE ASSERTION THIS WHOLE CASE EXISTS FOR: the stream is live *now*,
  // after a device that forgot its configuration. S2 watched the SDK sit
  // there receiving that device's discovery ACKs and never re-configure it,
  // dead for the rest of the run. Sampling twice, three seconds apart, is
  // what distinguishes "recovered" from "delivered a few packets and died".
  const std::uint64_t before_probe = d.stats().point_packets;
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const std::uint64_t after_probe = d.stats().point_packets;
  MESSAGE("  packets in the 3 s after recovery: " << (after_probe - before_probe));
  CHECK(after_probe - before_probe > 5000);  // ~6,250 expected at 2,083 pkt/s
  CHECK(d.link_state() == Mid360LinkState::kUp);
  CHECK(d.state() == DeviceState::kStreaming);

  CHECK(d.stop().ok());
  sim.terminate();
}

TEST_CASE("mid360sim/lvx2_replay_real_scan_pattern" * doctest::skip()) {
  std::string sim_bin;
  const std::string why = sim_preflight(&sim_bin);
  if (!why.empty()) {
    MESSAGE("SKIPPED: " << why);
    return;
  }
  const std::string replay = env_or("SCANENGINE_LVX2_REPLAY_BIN", "");
  const std::string lvx2 = env_or("SCANENGINE_LVX2_FILE", "");
  if (!file_exists(replay) || lvx2.empty() || ::access(lvx2.c_str(), F_OK) != 0) {
    MESSAGE("SKIPPED: set SCANENGINE_LVX2_REPLAY_BIN and SCANENGINE_LVX2_FILE "
            "(spikes/s2-mid360-sim/datasets/Indoor_sampledata.lvx2)");
    return;
  }

  // Real recorded geometry: the Risley-prism scan pattern the simulator
  // cannot synthesize, real tag values, and ~35% no-returns — the case the
  // filter defaults were chosen for, and which the simulator never exercises
  // (it emits 0.0017% no-returns and tag == 0 for everything).
  Child rep;
  REQUIRE(rep.spawn({replay, lvx2, "--speed", "1"}, log_path("lvx2_replay")));
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  SimRig rig;
  Mid360Config cfg = sim_driver_config();
  cfg.live_points_per_sec = 0;  // keep everything the filter passes
  Mid360Driver d(1, cfg, rig.ctx);
  REQUIRE(d.start().ok());
  REQUIRE(wait_for_stream(d, 20.0) < 20.0);

  const Mid360Stats base = d.stats();
  const auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(30));
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const Mid360Stats end = d.stats();

  Mid360Stats win = end;
  win.point_packets = end.point_packets - base.point_packets;
  win.points_received = end.points_received - base.points_received;
  win.points_appended = end.points_appended - base.points_appended;
  win.imu_packets = end.imu_packets - base.imu_packets;
  win.packets_lost = end.packets_lost - base.packets_lost;
  win.filter.dropped_no_return = end.filter.dropped_no_return - base.filter.dropped_no_return;
  win.filter.dropped_tag = end.filter.dropped_tag - base.filter.dropped_tag;
  report("lvx2 replay, Indoor_sampledata, 30 s", win, elapsed);

  const double no_return_pct =
      100.0 * static_cast<double>(win.filter.dropped_no_return) /
      static_cast<double>(win.points_received);
  MESSAGE("  no-return fraction  : " << no_return_pct << " %");

  CHECK(win.points_received / elapsed > 0.95 * kNominalPointsPerSec);
  CHECK(win.bad_packets == 0);
  // FIXTURES.md measured 35.24% over a 5 s indoor slice, 34.67% over the
  // whole recording. If this ever drifts far outside that, the filter or the
  // decode changed underneath us.
  CHECK(no_return_pct > 25.0);
  CHECK(no_return_pct < 45.0);
  // Real tag values exist here and the simulator has none.
  MESSAGE("  tag-rejected points : " << win.filter.dropped_tag);
  // Livox's own indoor/outdoor samples carry ZERO IMU packages, so a correct
  // replay produces no IMU at all. That is the honest behaviour, not a gap.
  MESSAGE("  IMU packets (0 expected for this file): " << win.imu_packets);

  CHECK(d.stop().ok());
  rep.terminate();
}

#endif  // !_WIN32
