// engine_cli — the engine's headless front end.
//
// Jobs today:
//   --selftest              build an engine, run a synthetic D6 capture
//                           through it, assert the whole path works. This is
//                           what CI (and a developer on a new machine) runs
//                           to answer "is the engine alive?".
//   --synth <out.bin> [s]   write a synthetic COIN-D6 capture (the S1
//                           d6synth room: 4x3 m with a reflective post).
//   --replay <file>         push a capture through the real engine and print
//                           decoded statistics.
//   --post <lscan-dir>      HEADLESS POST-PROCESSING (INT-34). This is the
//                           D1 cloud-worker entry point: A7's pipeline (and
//                           optionally A9's export) driven through A15's job
//                           queue, progress on stderr, a real exit code.
//   --synth-lscan <dir>     write a tiny synthetic Mid-360 .lscan, so --post
//                           has something to chew on with no capture rig.
//   --post-selftest         the two above, back to back, in a temp dir. What
//                           ctest runs.
//
// It grows into workstream D1's containerized worker CLI (headless Linux
// build running the post pipeline on an uploaded .lscan), which is why it
// links the engine exactly the way an app does — no privileged access, no
// test-only hooks.
//
// EXIT CODES (a worker's caller reads these, not the text):
//   0  success
//   1  the work failed — a bad .lscan, an unwritable output, a pipeline error
//   2  usage: a missing or unrecognized argument
//   3  cancelled
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "packet_builder.h"
#include "scanengine/color/colorizer.h"
#include "scanengine/color/frames_idx.h"
#include "scanengine/core/engine.h"
#include "scanengine/discovery/discovery.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/jobs/colorize_wiring.h"
#include "scanengine/jobs/job_runner_adapter.h"
#include "scanengine/poses/se3.h"
#include "scanengine/record/lscan.h"

using namespace scanengine;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Exit codes, named where they are produced.
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitCancelled = 3;

int usage() {
  std::printf(
      "engine_cli — LidarScan engine command line\n"
      "\n"
      "  engine_cli --selftest [--quiet]\n"
      "  engine_cli --synth <out.bin> [seconds] [--noise]\n"
      "  engine_cli --replay <capture.bin> [--chunk N]\n"
      "  engine_cli --post <lscan-dir> [--out <dir>] [--no-loops] [--no-outlier]\n"
      "                                [--dedup <metres>] [--quiet]\n"
      "                                [--colorize --sync-quality good|gated|poor\n"
      "                                 [--allow-poor-sync] [--clock-offset <ns>]]\n"
      "  engine_cli --synth-lscan <dir> [seconds] [--frames N]\n"
      "  engine_cli --post-selftest [--quiet]\n"
      "  engine_cli --discover [seconds] [--no-serial] [--no-lidar]\n"
      "  engine_cli --version\n"
      "\n"
      "--sync-quality is MANDATORY with --colorize and has no default: it is A4's\n"
      "verdict on the capture's camera/lidar time sync, only the capture side knows\n"
      "it, and colorizing on a guess produces a plausible, wrong cloud. Omitting it\n"
      "is refused (docs/A11-color.md §2 — the gate fails closed).\n"
      "\n"
      "exit: 0 ok, 1 failed, 2 usage, 3 cancelled\n");
  return kExitUsage;
}

// Ray-cast the S1 synthetic room: 4 x 3 m, reflective post at (0.8, 0.6).
std::uint16_t range_at(double deg) {
  const double a = deg * kPi / 180.0;
  const double dx = std::sin(a), dy = std::cos(a);  // 0 deg = +y
  const double hx = 2.0, hy = 1.5;
  double t = 1e9;
  if (std::fabs(dx) > 1e-9) t = std::fmin(t, std::fmax(hx / dx, -hx / dx));
  if (std::fabs(dy) > 1e-9) t = std::fmin(t, std::fmax(hy / dy, -hy / dy));
  const double px = 0.8, py = 0.6, pr = 0.15;
  const double b = dx * px + dy * py;
  const double c = px * px + py * py - pr * pr;
  const double disc = b * b - c;
  if (disc > 0) {
    const double th = b - std::sqrt(disc);
    if (th > 0.05 && th < t) t = th;
  }
  return static_cast<std::uint16_t>(std::lround(t * 1000.0));
}

std::vector<std::uint8_t> synth_capture(double seconds, bool noise) {
  const int revolutions = static_cast<int>(seconds * 10.0);
  const int packets = 10, per_packet = 40;
  const double step = 360.0 / (packets * per_packet);
  std::vector<std::uint8_t> out;
  unsigned rng = 12345;
  auto rand8 = [&]() {
    rng = rng * 1103515245u + 12345u;
    return static_cast<std::uint8_t>((rng >> 16) & 0xFF);
  };

  for (int r = 0; r < revolutions; ++r) {
    if (noise && (r % 7) == 3) {
      const std::uint8_t sp[4] = {0xFE, 0xFE, 0xFF, 0xFF};
      out.insert(out.end(), sp, sp + 4);
      for (int i = 0; i < 5; ++i) out.push_back(rand8());
    }
    {
      d6test::PacketSpec s;
      s.start_packet = true;
      s.scan_freq = 10;
      s.samples = {d6test::Sample{range_at(0.0), 120, false}};
      d6test::append(&out, d6test::build(s));
    }
    for (int k = 0; k < packets; ++k) {
      d6test::PacketSpec s;
      const double a0 = step * (k * per_packet);
      s.first_angle_deg = a0;
      s.last_angle_deg = a0 + step * (per_packet - 1);
      for (int i = 0; i < per_packet; ++i) {
        const double a = a0 + step * i;
        const std::uint16_t d = range_at(a);
        const bool hi = (a > 100.0 && a < 104.0);
        const std::uint8_t inten = static_cast<std::uint8_t>(
            hi ? 255 : std::fmin(250.0, 40000.0 / static_cast<double>(d ? d : 1)));
        s.samples.push_back(d6test::Sample{d, inten, hi});
      }
      d6test::append(&out, d6test::build(s));
    }
  }
  return out;
}

struct RunResult {
  DeviceHealth health{};
  std::uint64_t points = 0;
  std::size_t pages = 0;
  int points_events = 0;
  int rotation_events = 0;
  int device_events = 0;
  float bounds_min[3] = {0, 0, 0};
  float bounds_max[3] = {0, 0, 0};
};

// Feed a byte stream through a real Engine exactly as an app would.
bool run_capture(const std::vector<std::uint8_t>& bytes, std::size_t chunk, RunResult* out,
                 bool quiet) {
  EngineConfig cfg;
  cfg.app_name = "engine_cli";
  cfg.log_level = quiet ? LogLevel::kWarn : LogLevel::kInfo;
  cfg.event_queue_capacity = 65536;

  auto engine = Engine::create(cfg);
  if (!engine.ok()) {
    std::fprintf(stderr, "engine create failed: %s\n", last_error_message());
    return false;
  }
  Engine& e = *engine.value();

  DeviceConfig dc;
  dc.kind = DeviceKind::kD6;
  dc.d6.serial.port_name = "replay";
  auto id = e.add_device(dc);
  if (!id.ok()) {
    std::fprintf(stderr, "add_device failed: %s\n", last_error_message());
    return false;
  }

  SessionConfig sc;
  sc.record = false;  // A5 turns this into a real .lscan write
  if (!e.start_session(sc).ok()) {
    std::fprintf(stderr, "start_session failed: %s\n", last_error_message());
    return false;
  }

  for (std::size_t off = 0; off < bytes.size();) {
    const std::size_t n = std::min(chunk, bytes.size() - off);
    if (!e.push_serial_bytes(id.value(), ByteSpan(bytes.data() + off, n)).ok()) {
      std::fprintf(stderr, "push failed: %s\n", last_error_message());
      return false;
    }
    off += n;
  }

  Event ev;
  while (e.events().poll(e.app_subscription(), &ev)) {
    switch (ev.type) {
      case EventType::kPointsAvailable: ++out->points_events; break;
      case EventType::kRotation: ++out->rotation_events; break;
      case EventType::kDeviceState: ++out->device_events; break;
      default: break;
    }
  }

  auto h = e.device_health(id.value());
  if (h.ok()) out->health = h.value();
  out->points = e.points().total_points();
  out->pages = e.points().page_count();
  const auto ids = e.points().page_ids();
  if (!ids.empty()) {
    const PageView pv = e.points().page_view(ids.front());
    for (int i = 0; i < 3; ++i) {
      out->bounds_min[i] = pv.bounds_min[i];
      out->bounds_max[i] = pv.bounds_max[i];
    }
  }
  (void)e.stop_session();
  return true;
}

void print_result(const RunResult& r, std::size_t bytes_in) {
  std::printf("  bytes in            : %zu\n", bytes_in);
  std::printf("  packets ok / bad    : %llu / %llu\n",
              static_cast<unsigned long long>(r.health.packets_ok),
              static_cast<unsigned long long>(r.health.packets_bad));
  std::printf("  checksum pass rate  : %.4f\n", r.health.checksum_pass_rate);
  std::printf("  points decoded      : %llu\n", static_cast<unsigned long long>(r.points));
  std::printf("  pages / dropped     : %zu / %llu\n", r.pages,
              static_cast<unsigned long long>(r.health.drops));
  std::printf("  device state        : %s\n", to_string(r.health.state));
  std::printf("  events pts/rot/dev  : %d / %d / %d\n", r.points_events, r.rotation_events,
              r.device_events);
  std::printf("  page bounds (m)     : [%.3f %.3f %.3f] .. [%.3f %.3f %.3f]\n", r.bounds_min[0],
              r.bounds_min[1], r.bounds_min[2], r.bounds_max[0], r.bounds_max[1],
              r.bounds_max[2]);
}

int cmd_selftest(bool quiet) {
  std::printf("%s\n", engine_version_string());
  std::printf("selftest: 2 s synthetic COIN-D6 capture through the full engine path\n");

  const auto bytes = synth_capture(2.0, /*noise=*/true);
  RunResult r;
  if (!run_capture(bytes, 512, &r, quiet)) return 1;
  print_result(r, bytes.size());

  // 2 s at 10 Hz x 401 points = 8020 points; the noise injection costs none
  // of them (speed-adjust bytes and garbage are dropped, not misparsed).
  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    if (!ok) {
      std::printf("  FAIL: %s\n", what);
      ++failures;
    }
  };
  expect(r.points == 8020, "point count == 8020");
  expect(r.health.checksum_pass_rate >= 0.995, "checksum pass rate >= 0.995 (S1 exit criterion)");
  expect(r.health.state == DeviceState::kStreaming, "device reached kStreaming");
  expect(r.rotation_events == 19, "19 closed rotations out of 20 revolutions");
  expect(r.health.drops == 0, "no dropped points");
  expect(r.bounds_max[0] > 0.5f && r.bounds_min[0] < -0.5f, "room bounds look like a room");

  std::printf("selftest: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}

int cmd_synth(const char* path, double seconds, bool noise) {
  const auto bytes = synth_capture(seconds, noise);
  FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::perror("fopen");
    return 1;
  }
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  std::printf("wrote %zu bytes (%.1f s%s) to %s\n", bytes.size(), seconds,
              noise ? ", with noise" : "", path);
  return 0;
}

int cmd_replay(const char* path, std::size_t chunk) {
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::perror("fopen");
    return 1;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buf[65536];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
  std::fclose(f);

  std::printf("replay: %s (%zu bytes, %zu-byte chunks)\n", path, bytes.size(), chunk);
  RunResult r;
  if (!run_capture(bytes, chunk, &r, /*quiet=*/true)) return 1;
  print_result(r, bytes.size());
  return r.health.packets_ok > 0 ? 0 : 1;
}

// --- a tiny synthetic Mid-360 .lscan ---------------------------------------
//
// Deliberately the SAME shape tests/test_jobs.cpp writes: a stationary
// sensor, gravity-only IMU at 200 Hz, and point packets on a fixed 3 m sphere
// shell. It is not a scene — it is the smallest input that makes A7's
// pipeline run end to end in well under a second, which is what a smoke test
// wants and what a developer with no Mid-360 on the desk needs.
void put_mid360_header(std::uint8_t* buf, std::uint16_t len, std::uint16_t dot_num,
                       std::uint16_t udp_cnt, std::uint8_t data_type, std::uint64_t timestamp) {
  mid360::DataHeader h{};
  std::memset(&h, 0, sizeof(h));
  h.version = 0;
  h.length = len;
  h.time_interval = 5;
  h.dot_num = dot_num;
  h.udp_cnt = udp_cnt;
  h.frame_cnt = 0;
  h.data_type = data_type;
  h.time_type = 0;
  h.crc32 = 0;
  h.timestamp = timestamp;
  std::memcpy(buf, &h, sizeof(h));
}

bool write_synth_lscan(const std::string& dir, double duration_s) {
  lscan::FileRecordWriter w;
  if (!w.open(dir).ok()) {
    std::fprintf(stderr, "synth-lscan: cannot open '%s': %s\n", dir.c_str(), last_error_message());
    return false;
  }
  w.set_profile("quickscan");
  w.add_sensor("mid360-0", "lidar", "Livox Mid-360");
  // A11 §8.4 / INT-34: the manifest now carries the camera clock offset, so a
  // synthetic session exercises that key too.
  w.add_clock_offset("default", 0, 0.0);

  const std::int64_t t0 = 1'700'000'000'000'000'000LL;
  const double points_per_sec = 4000.0;
  const double dt_pkt = static_cast<double>(mid360::kPointsPerPacket) / points_per_sec;
  const double dt_imu = 1.0 / 200.0;

  std::vector<std::uint8_t> pbuf(mid360::kPointPacketBytes);
  std::vector<std::uint8_t> ibuf(mid360::kImuPacketBytes);
  std::uint16_t udp_cnt = 0;
  double t_pkt = 0.0, t_imu = 0.0;
  bool ok = true;

  while (ok && (t_pkt < duration_s || t_imu < duration_s)) {
    if (t_imu <= t_pkt) {
      put_mid360_header(ibuf.data(), static_cast<std::uint16_t>(mid360::kImuPacketBytes), 1,
                        udp_cnt++, mid360::kDataTypeImu,
                        static_cast<std::uint64_t>(t_imu * 1e9) + 1);
      const mid360::ImuRaw raw{0.f, 0.f, 0.f, 0.f, 0.f, 1.f};  // stationary, +1 g on z
      std::memcpy(ibuf.data() + sizeof(mid360::DataHeader), &raw, sizeof(raw));
      ok = w.write_chunk(lscan::ChunkType::kMid360Imu,
                         t0 + static_cast<std::int64_t>(t_imu * 1e9) + 1,
                         ByteSpan(ibuf.data(), ibuf.size()))
               .ok();
      t_imu += dt_imu;
      continue;
    }
    put_mid360_header(pbuf.data(), static_cast<std::uint16_t>(mid360::kPointPacketBytes),
                      mid360::kPointsPerPacket, udp_cnt++, mid360::kDataTypeCartesianHigh,
                      static_cast<std::uint64_t>(t_pkt * 1e9) + 1);
    auto* pts = reinterpret_cast<mid360::CartesianHigh*>(pbuf.data() + sizeof(mid360::DataHeader));
    for (std::uint32_t i = 0; i < mid360::kPointsPerPacket; ++i) {
      const double az = std::fmod(static_cast<double>(i) * 2.399963, 2.0 * kPi);
      const double el = (-7.0 + 59.0 * std::fmod(static_cast<double>(i) * 0.0173, 1.0)) * kPi / 180.0;
      pts[i].x = static_cast<std::int32_t>(std::cos(el) * std::cos(az) * 3000.0);
      pts[i].y = static_cast<std::int32_t>(std::cos(el) * std::sin(az) * 3000.0);
      pts[i].z = static_cast<std::int32_t>(std::sin(el) * 3000.0);
      pts[i].reflectivity = 120;
      pts[i].tag = 0;
    }
    ok = w.write_chunk(lscan::ChunkType::kMid360Points,
                       t0 + static_cast<std::int64_t>(t_pkt * 1e9) + 1,
                       ByteSpan(pbuf.data(), pbuf.size()))
             .ok();
    t_pkt += dt_pkt;
  }
  if (!ok) {
    std::fprintf(stderr, "synth-lscan: write failed: %s\n", last_error_message());
    (void)w.close();
    return false;
  }
  return w.close().ok();
}

// --- synthetic camera keyframes (INT-FINAL) ---------------------------------
//
// `--post --colorize` needs a session with a camera, and the whole point of
// --synth-lscan is that no rig and no committed binary fixture is required. So
// the keyframes are synthesized the same way the points are: N images written
// to streams/frames/ and N records written to streams/frames/frames.idx
// through A11's own KeyframeIndexWriter — the same writer B8's capture path
// uses, so this exercises the real format and not a test-only one.
//
// The images are PNG rather than JPEG because a PNG can be produced exactly,
// in ~40 lines, with no encoder: deflate has a STORED block type, so the
// "compressed" stream is the raw bytes plus a 5-byte header per block. The
// vendored stb_image decodes PNG and JPEG both, and a keyframe's image name
// carries no extension requirement.

std::uint32_t crc32_of(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0xFFFFFFFFu) {
  static std::uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    built = true;
  }
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return crc;
}

void put_be32(std::vector<std::uint8_t>* out, std::uint32_t v) {
  out->push_back(static_cast<std::uint8_t>(v >> 24));
  out->push_back(static_cast<std::uint8_t>(v >> 16));
  out->push_back(static_cast<std::uint8_t>(v >> 8));
  out->push_back(static_cast<std::uint8_t>(v));
}

void png_chunk(std::vector<std::uint8_t>* out, const char tag[4],
               const std::vector<std::uint8_t>& body) {
  put_be32(out, static_cast<std::uint32_t>(body.size()));
  const std::size_t start = out->size();
  out->insert(out->end(), tag, tag + 4);
  out->insert(out->end(), body.begin(), body.end());
  put_be32(out, crc32_of(out->data() + start, out->size() - start) ^ 0xFFFFFFFFu);
}

// 8-bit RGB PNG from a row-major buffer (stride = w*3).
std::vector<std::uint8_t> encode_png_rgb(std::uint32_t w, std::uint32_t h,
                                         const std::vector<std::uint8_t>& rgb) {
  std::vector<std::uint8_t> raw;  // filter byte + row, per row
  raw.reserve(static_cast<std::size_t>(h) * (1 + static_cast<std::size_t>(w) * 3));
  for (std::uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);  // filter: none
    const std::uint8_t* row = rgb.data() + static_cast<std::size_t>(y) * w * 3;
    raw.insert(raw.end(), row, row + static_cast<std::size_t>(w) * 3);
  }

  // zlib stream: 0x78 0x01, stored deflate blocks, Adler-32.
  std::vector<std::uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  for (std::size_t off = 0; off < raw.size();) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
    const bool last = (off + n) >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n & 0xFF));
    z.push_back(static_cast<std::uint8_t>(n >> 8));
    z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
    z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
             raw.begin() + static_cast<std::ptrdiff_t>(off + n));
    off += n;
  }
  std::uint32_t a = 1, b = 0;
  for (std::uint8_t c : raw) {
    a = (a + c) % 65521u;
    b = (b + a) % 65521u;
  }
  put_be32(&z, (b << 16) | a);

  std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> ihdr;
  put_be32(&ihdr, w);
  put_be32(&ihdr, h);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type: truecolour
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  png_chunk(&png, "IHDR", ihdr);
  png_chunk(&png, "IDAT", z);
  png_chunk(&png, "IEND", {});
  return png;
}

// Writes `count` keyframes around the synthetic 3 m shell: the camera sits at
// the origin (where the synthetic sensor is) and looks outward, yawing all the
// way round so the whole shell is covered. Each image is a flat, per-keyframe
// colour, which is what makes the selftest's assertion meaningful: a coloured
// point must carry one of the colours that were actually written.
bool write_synth_keyframes(const std::string& dir, int count, std::int64_t t0_ns,
                           double duration_s) {
  if (count <= 0) return true;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dir) / "streams" / "frames", ec);

  const std::uint32_t w = 64, h = 48;
  // A 90-degree horizontal field of view: fx = (w/2) / tan(45 deg) = w/2.
  const float fx = static_cast<float>(w) * 0.5f;

  color::KeyframeIndexWriter idx;
  if (!idx.open(dir).ok()) {
    std::fprintf(stderr, "synth-frames: cannot open the frame index: %s\n", last_error_message());
    return false;
  }

  for (int i = 0; i < count; ++i) {
    const std::uint8_t r = static_cast<std::uint8_t>(40 + (i * 60) % 200);
    const std::uint8_t g = static_cast<std::uint8_t>(90 + (i * 35) % 150);
    const std::uint8_t b = static_cast<std::uint8_t>(200 - (i * 45) % 180);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t p = 0; p < rgb.size(); p += 3) {
      rgb[p] = r;
      rgb[p + 1] = g;
      rgb[p + 2] = b;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "kf_%06d.png", i);
    const auto png = encode_png_rgb(w, h, rgb);
    const std::string path = dir + "/streams/frames/" + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
      std::fprintf(stderr, "synth-frames: cannot write %s\n", path.c_str());
      (void)idx.close();
      return false;
    }
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);

    // world_from_camera: at the origin, yawing round, tilted up into the
    // shell's elevation band, with image y pointing down (world -z).
    const double yaw = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(count);
    const double pitch = 20.0 * kPi / 180.0;
    double z_cam[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                       std::sin(pitch)};
    const double up[3] = {0.0, 0.0, 1.0};
    double x_cam[3];
    se3::cross3(z_cam, up, x_cam);
    se3::normalize3(x_cam);
    double y_cam[3];
    se3::cross3(z_cam, x_cam, y_cam);
    // Column-major fill of a row-major 3x3: column k is the camera axis.
    const double R[9] = {x_cam[0], y_cam[0], z_cam[0], x_cam[1], y_cam[1],
                         z_cam[1], x_cam[2], y_cam[2], z_cam[2]};
    Keyframe kf;
    se3::matrix_to_quat(R, kf.pose.orientation);
    kf.t_mono_ns = t0_ns + static_cast<std::int64_t>(duration_s * 1e9 *
                                                     (static_cast<double>(i) + 0.5) /
                                                     static_cast<double>(count));
    kf.pose.t_mono_ns = kf.t_mono_ns;
    kf.pose.source = StreamId::kPoseAr;
    kf.pose.quality = PoseQuality::kGood;
    kf.image_path = std::string("streams/frames/") + name;
    kf.intrinsics.fx = fx;
    kf.intrinsics.fy = fx;
    kf.intrinsics.cx = static_cast<float>(w) * 0.5f;
    kf.intrinsics.cy = static_cast<float>(h) * 0.5f;
    kf.intrinsics.width = w;
    kf.intrinsics.height = h;
    kf.flags = kKeyframeFlagMotionValid;  // stationary rig: the motion gate passes
    kf.angular_rate_rad_s = 0.f;
    kf.linear_speed_m_s = 0.f;
    kf.image_bytes = static_cast<std::uint32_t>(png.size());
    if (!idx.add(kf).ok()) {
      std::fprintf(stderr, "synth-frames: frames.idx rejected keyframe %d: %s\n", i,
                   last_error_message());
      (void)idx.close();
      return false;
    }
  }
  return idx.close().ok();
}

int cmd_synth_lscan(const char* dir, double seconds, int frames) {
  if (!write_synth_lscan(dir, seconds)) return kExitFailed;
  if (!write_synth_keyframes(dir, frames, 1'700'000'000'000'000'000LL, seconds)) {
    return kExitFailed;
  }
  std::printf("wrote a %.1f s synthetic Mid-360 .lscan to %s", seconds, dir);
  if (frames > 0) std::printf(" (with %d camera keyframes)", frames);
  std::printf("\n");
  return kExitOk;
}

// --- --post ----------------------------------------------------------------

// Post-processing, headless, through the same job queue an app uses. Nothing
// here is CLI-specific: `Engine::jobs()` is the queue, `QueueJobRunner` is
// A1's JobRunner seam over it, and the request is the one B6's mode chooser
// would build. That is the point — the cloud worker must run the SAME path a
// phone runs, or "one pipeline in three places" (§3.8) is a slogan.
// The knobs a worker genuinely chooses per input. Everything else in A7's
// PostConfig is a measured default, not a per-run decision.
struct PostOptions {
  bool detect_loops = true;
  bool outlier_filter = true;
  double dedup_voxel_m = 0.0;  // 0 = A7's default

  // --- colorization (INT-FINAL, closing docs/INT34-wiring.md §9 item 6) ----
  //
  // §9 item 6 named the two things a --colorize flag needed before it could
  // exist: keyframe JPEGs in the uploaded bundle, and "a decision about
  // sync_quality that only the capture side can make". The first is now a
  // property of the bundle the worker is handed (frames/ is either there or it
  // is not, and its absence is §3.5's "gracefully unavailable", not a
  // failure); the second is this field, and it FAILS CLOSED.
  //
  // kUnknown is what an un-converged A4 estimator reports and what a caller
  // who never wired A4 gets, and `policy_for(kUnknown)` refuses
  // (kNotSupported) — docs/A11-color.md §2. So --colorize without
  // --sync-quality does not colorize; it says which flag is missing and why
  // the engine will not guess. A worker gets this value from the capture
  // side's own A4 convergence, carried in the job it was given.
  bool colorize = false;
  SyncQuality sync_quality = SyncQuality::kUnknown;
  bool allow_poor_sync = false;
  std::int64_t camera_clock_offset_ns = 0;
};

const char* sync_quality_name(SyncQuality q) {
  switch (q) {
    case SyncQuality::kGood: return "good (<= 5 ms)";
    case SyncQuality::kGated: return "gated (<= 15 ms)";
    case SyncQuality::kPoor: return "poor (> 15 ms)";
    case SyncQuality::kUnknown: break;
  }
  return "unknown (not converged)";
}

bool parse_sync_quality(const char* s, SyncQuality* out) {
  if (std::strcmp(s, "good") == 0) { *out = SyncQuality::kGood; return true; }
  if (std::strcmp(s, "gated") == 0) { *out = SyncQuality::kGated; return true; }
  if (std::strcmp(s, "poor") == 0) { *out = SyncQuality::kPoor; return true; }
  if (std::strcmp(s, "unknown") == 0) { *out = SyncQuality::kUnknown; return true; }
  return false;
}

int cmd_post(const std::string& lscan_dir, const std::string& out_dir, const PostOptions& po,
             bool quiet, color::ColorizeStats* out_cstats = nullptr) {
  std::error_code ec;
  if (!std::filesystem::is_directory(lscan_dir, ec)) {
    std::fprintf(stderr, "post: '%s' is not a directory\n", lscan_dir.c_str());
    return kExitFailed;
  }
  if (!out_dir.empty()) {
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
      std::fprintf(stderr, "post: cannot create output directory '%s': %s\n", out_dir.c_str(),
                   ec.message().c_str());
      return kExitFailed;
    }
  }

  EngineConfig cfg;
  cfg.app_name = "engine_cli";
  cfg.log_level = quiet ? LogLevel::kWarn : LogLevel::kInfo;
  auto engine = Engine::create(cfg);
  if (!engine.ok()) {
    std::fprintf(stderr, "post: engine create failed: %s\n", last_error_message());
    return kExitFailed;
  }
  Engine& e = *engine.value();

  jobs::JobRunnerOptions opts;
  // A cloud worker's defaults are A7's defaults: loop detection on, full
  // density, statistical outlier filter on. --no-loops / --no-outlier /
  // --dedup exist because a caller who knows its input (a short stationary
  // capture, a synthetic fixture) knows better; nothing here silently
  // deviates from A7.
  opts.post_config.detect_loops = po.detect_loops;
  opts.post_config.outlier.enabled = po.outlier_filter;
  if (po.dedup_voxel_m > 0.0) opts.post_config.dedup.voxel_size_m = po.dedup_voxel_m;
  // Route the pipeline at a store this process owns, so the summary below can
  // report what was actually produced rather than what happens to be in the
  // Engine's (live-capture) store, which a headless run never fills.
  opts.store = std::make_shared<PageStore>();
  opts.export_format = ExportFormat::kPlyBinary;
  opts.export_basename = "cloud";

  // --colorize. The colorizer is built HERE and borrowed by the job, because
  // JobRunnerOptions::colorizer is not owned — the same posture B6's Android
  // service and the Qt processing panel have. Everything it is configured
  // with comes through jobs/colorize_wiring.h so the CLI is not a second
  // place that decides how A4 feeds A11.
  //
  // `timesync` is deliberately null: this process never captured anything, so
  // its TimeSync has nothing to say and reading it would be worse than
  // useless — it would report kUnknown for a session whose capture side had
  // converged. The value therefore arrives on the command line, and the
  // engine still refuses if it was not supplied.
  jobs::ColorizeWiring wiring;
  wiring.allow_poor_sync = po.allow_poor_sync;
  wiring.camera_clock_offset_ns = po.camera_clock_offset_ns;
  color::ColorizeConfig ccfg = jobs::colorize_config_from(wiring);
  ccfg.sync_quality = po.sync_quality;
  ccfg.allow_poor_sync = po.allow_poor_sync;
  color::PointColorizer colorizer(ccfg);
  if (po.colorize) {
    (void)jobs::wire_colorizer(wiring, &colorizer);
    opts.colorizer = &colorizer;
  }

  jobs::QueueJobRunner runner(&e.jobs(), opts);

  JobRequest req;
  req.mode = JobMode::kLocal;
  req.lscan_dir = lscan_dir;
  req.output_dir = out_dir;
  // With an --out, the request is "post then export"; without one it is
  // "post" and the result stays in the process's PageStore, which is what a
  // caller wanting only a validity check (or a chained in-process step)
  // wants. --colorize inserts A11's stage between the two, which is why the
  // adapter grew a "colorize-export" pipeline rather than the CLI running two
  // requests: chaining is by job id, and a second request would re-run the
  // post pipeline over the same .lscan.
  if (po.colorize) {
    req.pipeline = out_dir.empty() ? "colorize" : "colorize-export";
  } else {
    req.pipeline = out_dir.empty() ? "post" : "export";
  }
  if (po.colorize && !quiet) {
    std::fprintf(stderr, "post: colorize enabled, sync quality %s\n",
                 sync_quality_name(po.sync_quality));
  }

  auto id = runner.submit(req);
  if (!id.ok()) {
    std::fprintf(stderr, "post: submit failed: %s\n", last_error_message());
    return kExitFailed;
  }

  // Progress on STDERR, results on STDOUT: a worker's stdout is its product
  // and must stay machine-readable when it is piped.
  int last_pct = -1;
  JobStatus st;
  for (;;) {
    st = runner.status(id.value());
    const int pct = static_cast<int>(st.progress * 100.f + 0.5f);
    if (!quiet && (pct != last_pct)) {
      std::fprintf(stderr, "post: %3d%%  %s\n", pct, st.message.c_str());
      last_pct = pct;
    }
    if (st.state == JobState::kDone || st.state == JobState::kFailed ||
        st.state == JobState::kCancelled) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (po.colorize && out_cstats != nullptr) *out_cstats = colorizer.stats();

  if (st.state == JobState::kCancelled) {
    std::fprintf(stderr, "post: cancelled\n");
    return kExitCancelled;
  }
  if (st.state == JobState::kFailed) {
    std::fprintf(stderr, "post: FAILED (%s): %s\n", error_str(st.error), st.message.c_str());
    return kExitFailed;
  }

  std::printf("post: %s\n", lscan_dir.c_str());
  std::printf("  points published    : %llu\n",
              static_cast<unsigned long long>(opts.store->total_points()));
  std::printf("  pages               : %zu\n", opts.store->page_count());
  if (po.colorize) {
    const color::ColorizeStats& cs = colorizer.stats();
    std::printf("  keyframes used/total: %u / %u\n", cs.keyframes_used, cs.keyframes_total);
    std::printf("  points colorized    : %llu (%.1f%% coverage)\n",
                static_cast<unsigned long long>(cs.points_colorized),
                100.0 * static_cast<double>(cs.coverage_fraction()));
    std::printf("  low conf / uncovered: %llu / %llu\n",
                static_cast<unsigned long long>(cs.points_low_confidence),
                static_cast<unsigned long long>(cs.points_uncovered));
    if (cs.keyframes_total == 0) {
      // Tech Spec §3.5: a session with no camera is "gracefully unavailable",
      // and the Colorize job reports kOk for it. Say so, rather than leaving a
      // row of zeros that reads like a bug.
      std::printf("  (this session has no camera — nothing to colorize)\n");
    }
  }
  if (!out_dir.empty()) {
    const std::string path = out_dir + "/cloud.ply";
    const std::uintmax_t sz = std::filesystem::file_size(path, ec);
    std::printf("  exported            : %s (%llu bytes)\n", path.c_str(),
                ec ? 0ull : static_cast<unsigned long long>(sz));
    if (ec) {
      std::fprintf(stderr, "post: the job reported success but '%s' is unreadable\n", path.c_str());
      return kExitFailed;
    }
  }
  std::printf("post: OK\n");
  return kExitOk;
}

// Synthesize, post-process, export, verify, clean up. Registered as the
// `engine_cli_post` ctest — the only way to smoke-test --post without
// committing a binary .lscan fixture.
int cmd_post_selftest(bool quiet) {
  std::error_code ec;
  const auto root = std::filesystem::temp_directory_path() /
                    ("engine_cli_post_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string lscan = (root / "session.lscan").string();
  const std::string out = (root / "out").string();
  std::filesystem::create_directories(lscan, ec);

  std::printf("%s\n", engine_version_string());
  std::printf("post-selftest: synthesize -> --post --out -> verify\n");

  int rc = kExitFailed;
  // 6 camera keyframes around the shell (INT-FINAL), so the colorize leg
  // below has a real frames.idx and real images to read.
  if (write_synth_lscan(lscan, 2.2) &&
      write_synth_keyframes(lscan, 6, 1'700'000'000'000'000'000LL, 2.2)) {
    PostOptions po;
    // The synthetic capture's 96 points per packet repeat identically at
    // every keyframe and dedup down to ~96 points thinly scattered over a 3 m
    // sphere shell — far too sparse for cloud_filter.h's k-NN distance
    // distribution to mean anything, so it would delete the whole cloud. Off
    // here for exactly the reason tests/test_jobs.cpp turns it off: this is a
    // plumbing test, and tests/test_post.cpp owns the accuracy claims.
    po.outlier_filter = false;
    po.detect_loops = false;
    po.dedup_voxel_m = 0.05;
    rc = cmd_post(lscan, out, po, quiet);

    // The whole point of the smoke test: a REAL cloud came out the far end.
    if (rc == kExitOk) {
      const std::uintmax_t sz = std::filesystem::file_size(out + "/cloud.ply", ec);
      if (ec || sz < 512) {
        std::fprintf(stderr,
                     "post-selftest: cloud.ply is %llu bytes — the pipeline ran but produced "
                     "no points\n",
                     ec ? 0ull : static_cast<unsigned long long>(sz));
        rc = kExitFailed;
      }
    }

    // --- the colorize leg (INT-FINAL) ------------------------------------
    //
    // Same .lscan, same job queue, one more stage: post -> colorize ->
    // export. Two things are asserted, and the first matters more than the
    // second: that the gate REFUSES a run with no sync quality (which is the
    // whole reason --sync-quality is mandatory), and that a run with one
    // actually puts colour on points.
    if (rc == kExitOk) {
      const std::string cout_dir = (root / "out-colour").string();
      std::printf("post-selftest: colorize leg (refuse-then-run)\n");

      PostOptions cpo;
      cpo.outlier_filter = false;
      cpo.detect_loops = false;
      cpo.dedup_voxel_m = 0.05;
      cpo.colorize = true;
      cpo.sync_quality = SyncQuality::kUnknown;  // the fail-closed default
      const int refused = cmd_post(lscan, cout_dir, cpo, quiet);
      if (refused == kExitOk) {
        std::fprintf(stderr,
                     "post-selftest: a colorize with NO sync quality succeeded; the gate is "
                     "supposed to fail closed (docs/A11-color.md §2)\n");
        rc = kExitFailed;
      }

      if (rc == kExitOk) {
        cpo.sync_quality = SyncQuality::kGood;
        color::ColorizeStats cs;
        rc = cmd_post(lscan, cout_dir, cpo, quiet, &cs);
        if (rc == kExitOk) {
          const std::uintmax_t sz = std::filesystem::file_size(cout_dir + "/cloud.ply", ec);
          if (ec || sz < 512) {
            std::fprintf(stderr, "post-selftest: the colorized export is empty\n");
            rc = kExitFailed;
          }
          if (cs.keyframes_used == 0 || cs.points_colorized == 0) {
            std::fprintf(stderr,
                         "post-selftest: the colorize stage ran but coloured nothing "
                         "(%u/%u keyframes used, %llu points coloured)\n",
                         cs.keyframes_used, cs.keyframes_total,
                         static_cast<unsigned long long>(cs.points_colorized));
            rc = kExitFailed;
          }
        }
      }
    }
  } else {
    std::fprintf(stderr, "post-selftest: could not write the synthetic .lscan\n");
  }

  std::filesystem::remove_all(root, ec);
  std::printf("post-selftest: %s\n", rc == kExitOk ? "PASS" : "FAIL");
  return rc;
}

// --- A16: what hardware is on this machine's desk right now? ----------------
//
// The field-verification face of docs/A16-discovery.md, and the thing to run
// FIRST at the start of a hardware session: it answers "is the lidar
// broadcasting", "will it stream to this host", and "which /dev is which"
// without a single hand-typed address. Exit 0 if anything was found.
int cmd_discover(double seconds, bool do_lidar, bool do_serial) {
  int found = 0;

  if (do_lidar) {
    const int timeout_ms = static_cast<int>(seconds * 1000.0);
    std::printf("listening for Mid-360 heartbeats on udp/%u and udp/%u for %.1f s...\n",
                static_cast<unsigned>(discovery::kMid360PushPort),
                static_cast<unsigned>(discovery::kMid360PushPortAlt), seconds);
    Result<std::vector<discovery::Mid360Beacon>> r = discovery::DiscoverMid360(timeout_ms);
    if (!r.ok()) {
      std::fprintf(stderr, "  discovery failed: %s (%s)\n", error_str(r.error()),
                   last_error_message());
    } else if (r.value().empty()) {
      std::printf("  no Mid-360 is broadcasting. Check power and the ethernet link;\n"
                  "  the lidar ignores ping, so its heartbeat is the only evidence.\n");
    } else {
      for (const discovery::Mid360Beacon& b : r.value()) {
        ++found;
        std::printf("\n  %s\n", b.describe().c_str());
        std::printf("    sn %s   fw %s (%s, built %s)\n", b.sn.c_str(), b.fw_version_text.c_str(),
                    b.fw_type.c_str(), b.build_time.c_str());
        std::printf("    lidar %s/%s gw %s  mac %s\n", b.lidar_ip.c_str(), b.netmask.c_str(),
                    b.gateway.c_str(), b.mac.c_str());
        std::printf("    persisted host %s (points %u, imu %u)\n", b.persisted_host_ip.c_str(),
                    b.persisted_point_port, b.persisted_imu_port);
        std::printf("    heard on udp/%u from %s, %u beacon(s), crc %s%s\n", b.push_port_seen,
                    b.source_ip.c_str(), b.beacons_seen, b.crc_ok ? "ok" : "UNVERIFIED",
                    b.heuristic ? ", HEURISTIC PARSE" : "");
        const discovery::HostCheck hc = discovery::CheckHostReachability(b);
        std::printf("    %s\n", hc.note.c_str());
      }
    }
  }

  if (do_serial) {
    const std::vector<std::string> ports = discovery::EnumerateSerialPorts();
    std::printf("\n%zu serial port(s):\n", ports.size());
    for (const std::string& p : ports) std::printf("    %s\n", p.c_str());
    if (!ports.empty()) {
      // The D6 first: it is the one that may need a start command, and
      // identifying it removes it from the UM982 sweep's candidates.
      std::optional<discovery::D6Probe> d6 = discovery::ProbeSerialD6(ports, 1200);
      if (d6.has_value()) {
        ++found;
        std::printf("  COIN-D6 on %s @ %u (%u packets, %u bad checksum%s)\n", d6->port.c_str(),
                    d6->baud, d6->packets_ok, d6->packets_bad_checksum,
                    d6->used_start_command ? ", needed the start command" : ", already streaming");
      } else {
        std::printf("  no COIN-D6 found\n");
      }
      std::vector<std::string> rest;
      for (const std::string& p : ports) {
        if (!d6.has_value() || p != d6->port) rest.push_back(p);
      }
      std::optional<discovery::Um982Probe> um = discovery::ProbeSerialUm982(rest, 1500);
      if (um.has_value()) {
        ++found;
        std::printf("  UM982 on %s @ %u (%u sentences, heading %s)\n", um->port.c_str(),
                    um->baud, um->sentences_ok, um->has_heading ? "ENABLED" : "not seen");
      } else {
        std::printf("  no UM982 found\n");
      }
    }
  }

  std::printf("\ndiscovery: %d device(s) found\n", found);
  return found > 0 ? kExitOk : kExitFailed;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string cmd = argv[1];

  bool quiet = false, noise = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quiet") == 0) quiet = true;
    if (std::strcmp(argv[i], "--noise") == 0) noise = true;
  }

  if (cmd == "--version" || cmd == "-v") {
    std::printf("%s (ABI %u)\n", engine_version_string(), kEngineAbiVersion);
    return 0;
  }
  if (cmd == "--selftest") return cmd_selftest(quiet);
  if (cmd == "--synth") {
    if (argc < 3) return usage();
    const double seconds = (argc > 3 && argv[3][0] != '-') ? std::atof(argv[3]) : 2.0;
    return cmd_synth(argv[2], seconds, noise);
  }
  if (cmd == "--replay") {
    if (argc < 3) return usage();
    std::size_t chunk = 512;
    for (int i = 3; i + 1 < argc; ++i) {
      if (std::strcmp(argv[i], "--chunk") == 0) chunk = static_cast<std::size_t>(std::atoi(argv[i + 1]));
    }
    if (chunk == 0) chunk = 512;
    return cmd_replay(argv[2], chunk);
  }
  if (cmd == "--post") {
    if (argc < 3 || argv[2][0] == '-') return usage();
    std::string out_dir;
    PostOptions po;
    bool sync_given = true;
    for (int i = 3; i < argc; ++i) {
      if (std::strcmp(argv[i], "--no-loops") == 0) po.detect_loops = false;
      if (std::strcmp(argv[i], "--no-outlier") == 0) po.outlier_filter = false;
      if (std::strcmp(argv[i], "--colorize") == 0) {
        po.colorize = true;
        sync_given = false;  // until --sync-quality is seen
      }
      if (std::strcmp(argv[i], "--allow-poor-sync") == 0) po.allow_poor_sync = true;
      if (i + 1 >= argc) continue;
      if (std::strcmp(argv[i], "--out") == 0) out_dir = argv[i + 1];
      if (std::strcmp(argv[i], "--dedup") == 0) po.dedup_voxel_m = std::atof(argv[i + 1]);
      if (std::strcmp(argv[i], "--clock-offset") == 0) {
        po.camera_clock_offset_ns = std::atoll(argv[i + 1]);
      }
      if (std::strcmp(argv[i], "--sync-quality") == 0) {
        if (!parse_sync_quality(argv[i + 1], &po.sync_quality)) {
          std::fprintf(stderr, "post: --sync-quality '%s' is not good|gated|poor|unknown\n",
                       argv[i + 1]);
          return kExitUsage;
        }
        sync_given = true;
      }
    }
    // Fail closed, and fail EARLY: the refusal is a property of the gate
    // (docs/A11-color.md §2), and there is no reason to run a post pipeline
    // first just to reach it. The engine refuses too — this only moves the
    // same answer to where the operator can act on it.
    if (!sync_given) {
      std::fprintf(stderr,
                   "post: --colorize needs --sync-quality good|gated|poor. It is A4's verdict on "
                   "this capture's camera/lidar sync and only the capture side knows it; the "
                   "engine will not guess (the gate fails closed — docs/A11-color.md §2).\n");
      return kExitUsage;
    }
    if (po.sync_quality == SyncQuality::kPoor && !po.allow_poor_sync) {
      std::fprintf(stderr,
                   "post: --sync-quality poor is refused unless --allow-poor-sync is given "
                   "(S6: above 15 ms of jitter the colours smear off the geometry).\n");
      return kExitUsage;
    }
    return cmd_post(argv[2], out_dir, po, quiet);
  }
  if (cmd == "--synth-lscan") {
    if (argc < 3 || argv[2][0] == '-') return usage();
    const double seconds = (argc > 3 && argv[3][0] != '-') ? std::atof(argv[3]) : 2.2;
    int frames = 0;
    for (int i = 3; i + 1 < argc; ++i) {
      if (std::strcmp(argv[i], "--frames") == 0) frames = std::atoi(argv[i + 1]);
    }
    return cmd_synth_lscan(argv[2], seconds, frames);
  }
  if (cmd == "--post-selftest") return cmd_post_selftest(quiet);
  if (cmd == "--discover") {
    double seconds = (argc > 2 && argv[2][0] != '-') ? std::atof(argv[2]) : 4.0;
    if (seconds <= 0.0) seconds = 4.0;
    bool do_lidar = true, do_serial = true;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--no-serial") == 0) do_serial = false;
      if (std::strcmp(argv[i], "--no-lidar") == 0) do_lidar = false;
    }
    return cmd_discover(seconds, do_lidar, do_serial);
  }
  return usage();
}
