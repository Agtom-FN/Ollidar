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
#include <algorithm>
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
#include "scanengine/slam/post/d6_resolve.h"
#include "scanengine/slam/post/mount_watch.h"
#include "scanengine/slam/post/lscan_plan.h"
#include "scanengine/slam/post/map_consistency.h"
#include "scanengine/slam/post/reprocess.h"
#include "scanengine/slam/post/trajectory_loop.h"

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
      "  engine_cli --d6-timesweep <lscan-dir> [--from MS] [--to MS] [--step MS]\n"
      "                              [--no-densify] [--up X|Y|Z]\n"
      "      ROUND 10 item 36. Resolve a COIN-D6 capture at a sweep of\n"
      "      lidar->pose time offsets and print a crispness metric for each,\n"
      "      so the transport delay is MEASURED on real data instead of\n"
      "      guessed. Defaults: -60..+60 ms in 5 ms steps.\n"
      "\n"
      "  engine_cli --d6-loopclose <lscan-dir> [--radius M] [--min-seconds S]\n"
      "                              [--min-path M] [--window S] [--up X|Y|Z]\n"
      "      ROUND 11 item 41. Resolve a COIN-D6 capture twice — once as\n"
      "      shipped, once with trajectory loop closure — and print the\n"
      "      decision, the measured drift, the same-place mismatch before and\n"
      "      after, and the ROUND 10 crispness score for both. Prints WHY it\n"
      "      refused when it refuses, which for a one-way walk is the correct\n"
      "      answer and not a failure.\n"
      "\n"
      "  engine_cli --d6-dump <lscan-dir> [--out PREFIX] [--mount-from OTHER.lscan]\n"
      "                          [--offset-ms MS] [--no-densify]\n"
      "      ROUND 12 dev tool. Resolve a COIN-D6 capture and dump the world\n"
      "      points WITH their point times plus the trajectory, so an analysis\n"
      "      script can separate an out-and-back walk into its two legs.\n"
      "      --mount-from resolves THESE bytes with ANOTHER capture's mount\n"
      "      extrinsic, which is how a trim hypothesis is adjudicated.\n"
      "\n"
      "  engine_cli --d6-stitch <lscan-dir> [--no-refine] [--no-densify]\n"
      "                            [--window S] [--cell M] [--max-refine M]\n"
      "      ROUND 13. A section break is ARCore RE-ANCHORING, and the frame\n"
      "      change it applied is recorded in the pose jump itself. Resolve a\n"
      "      capture as shipped and again with the sections put back into ONE\n"
      "      frame, and print what moved, what each seam decided, and whether\n"
      "      the map agrees with itself better afterwards. The check that does\n"
      "      NOT come from the same measurement: the operator walks on a FLAT\n"
      "      FLOOR, so the trajectory vertical extent must shrink.\n"
      "\n"
      "  engine_cli --d6-mountcheck <lscan-dir> [--window S] [--up X|Y|Z]\n"
      "      ROUND 13 item 48. Has the puck been rotated since the mount\n"
      "      reference was set? The fan plane cannot answer (every return\n"
      "      leaves the formula at z=0, so it lies in the assumed plane by\n"
      "      construction) — where the returns LAND can. On the owner's own\n"
      "      captures the contaminated scan-026 puts 15.2%% of returns more\n"
      "      than 2.5 m above or below the sensor; scan-020/028/029/030 put\n"
      "      exactly 0.0%%, at the same median range.\n"
      "\n"
      "  engine_cli --d6-selfcheck <lscan-dir> [--window S] [--cell M]\n"
      "  engine_cli --d6-reprocess <lscan-dir> [--no-refine] [--no-densify] [--no-loopend]\n"
      "      The phone's own Process action, from a terminal: stitch, close the loop\n"
      "      end, measure, and write processed/map_stitched.bin + stitch.json +\n"
      "      trajectory.bin.\n"
      "  engine_cli --d6-loopend <lscan-dir> [--min-excursion M] [--max-correction M]\n"
      "      [--submap S] [--window S] [--cell M] [--up X|Y|Z]\n"
      "      ROUND 16 item 60: close the gap at the END of a walk with the rotation\n"
      "      frozen at the tracker's own (which the recorded 400 Hz gyro agrees with)\n"
      "      and only the translation solved. Two resolves, both stitched, scored with\n"
      "      the ROUND 12 self-check ruler and the ROUND 10 crispness metric at the same\n"
      "      wall probes. Prints the gate that refused when it refuses.\n"
      "                               [--mount-from OTHER.lscan] [--offset-ms MS]\n"
      "      ROUND 12. How far the map disagrees with ITSELF: surfaces painted\n"
      "      twice, compared along their own normal, bucketed by how far apart\n"
      "      in time the two paintings were. Works at walking pace, which the\n"
      "      wall-probe metric does not (it selects zero probes there).\n"
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

// --- ROUND 10 item 36: measuring the lidar->pose time offset ---------------
//
// THE PROBLEM, in one sentence: a constant offset between the clock the D6's
// returns are dated in and the clock ARCore's poses are dated in costs
// `v * dt` along the walk (invisible: the whole cloud shifts together, walls
// stay straight) and `omega * dt` of yaw (glaring: at a 60 deg/s turn, 20 ms
// is 1.2 deg, which is 6 cm of tangential smear at 3 m, and it REVERSES SIGN
// with the turn direction — so the same wall is painted in two places when
// the operator walks past it out and back).
//
// It cannot be derived, because it is a property of the USB stack, the CH340's
// buffering and the reader thread's scheduling. So it is MEASURED, from a real
// capture, the way a focus is measured: resolve the SAME container at a sweep
// of candidate offsets and keep the one that makes the map sharpest. Nothing
// here is a fit to a model — it is the container's own geometry disagreeing
// with itself less at one offset than at another.
//
// THREE METRICS, all computed on the same resolved cloud, all deterministic
// (no RNG, no threading, fixed grids, sorts broken by index):
//
//  1. `wall_rms_cm` — THE headline. Take the horizontal band between the floor
//     and ceiling peaks, grid it at 4 cm in the two ground axes, and for the
//     busiest cells fit a LINE (2-D PCA) to the neighbours within 25 cm. The
//     RMS perpendicular distance is literally "how thick is this wall". A
//     mis-paired pose smears a wall into a wedge, and this is the number that
//     goes up. Cells that are not wall-like (too few points, or not elongated)
//     are rejected, so a corner or a chair cannot dominate the average.
//  2. `occupied_voxels` — how many 3 cm voxels the same points need. Smearing
//     spreads the same returns over more voxels; this is a shape-free check on
//     (1) that no fitting decision can influence.
//  3. `entropy_bits` — Shannon entropy of the voxel occupancy histogram, the
//     mean-map-entropy family's crispness measure. Same direction as (2), but
//     sensitive to HOW the mass redistributes rather than only to its support.
//
// A sweep is trustworthy only if the population being measured does not move
// with the offset, so `points` is printed per row: it must be flat (only the
// handful of returns at the very ends of the capture can change bracket).
struct CrispnessMetrics {
  std::uint64_t points = 0;
  std::uint64_t occupied_voxels = 0;
  double entropy_bits = 0.0;
  double wall_rms_cm = 0.0;
  std::uint64_t wall_cells = 0;
};

// Which world axis is "up".
//
// NOT detected, and that is deliberate. A COIN-D6 project is by construction
// an ARCore project — the D6 has no IMU, so the ONLY thing that can supply
// its trajectory is the phone, and ARCore's world frame is gravity-aligned
// with +Y up. So the up axis is known from the container's sensor list, not
// guessed from its point distribution.
//
// The first version of this tool DID guess (tallest histogram peak relative
// to the median) and picked X on the owner's scan-020, which silently emptied
// the wall band and reported zero walls at every offset. A heuristic that can
// be wrong about the single most load-bearing assumption in the metric is
// worse than a constant, because a constant cannot be wrong quietly.
// `--up X|Y|Z` overrides it for a Mid-360 (+Z up) container.
constexpr int kArCoreUpAxis = 1;  // +Y

// A candidate wall segment: a fixed world location in the two ground axes.
//
// The centres are chosen ONCE, from the REFERENCE resolve, and every offset in
// the sweep is then measured at those same places. That is what makes the
// comparison honest: if each offset picked its own wall segments, the metric
// would be measuring which offset produces the most selectable walls rather
// than how thick the same walls are, and "crispest" would be circular.
struct WallProbe {
  double u = 0.0, v = 0.0;
};

struct BandGrid {
  std::vector<float> u, v;              // band points, two ground axes
  std::vector<std::uint32_t> head;      // CSR cell starts, size nu*nv+1
  std::vector<std::uint32_t> order;     // point indices, bucketed by cell
  double umin = 0, vmin = 0, cell = 0;
  int nu = 0, nv = 0;
  bool ok = false;
};

// The horizontal band between the floor and the ceiling, bucketed into a
// uniform grid by counting sort. No hashing and no per-cell vector, so the
// layout — and therefore every sum computed from it — is bit-reproducible.
BandGrid build_band_grid(const std::vector<PointVertex>& pts, int up_axis, double cell) {
  BandGrid g;
  g.cell = cell;
  if (pts.size() < 2000) return g;
  const int a0 = (up_axis + 1) % 3;
  const int a1 = (up_axis + 2) % 3;

  // Cut the band from the QUANTILES of the height distribution rather than
  // from its range: the floor and the ceiling are by construction the two
  // extremes, and [20 %, 80 %] drops both without assuming a ceiling height.
  std::vector<float> hs;
  hs.reserve(pts.size());
  for (const PointVertex& p : pts) hs.push_back((&p.x)[up_axis]);
  std::sort(hs.begin(), hs.end());
  const float lo_h = hs[static_cast<std::size_t>(0.20 * (hs.size() - 1))];
  const float hi_h = hs[static_cast<std::size_t>(0.80 * (hs.size() - 1))];

  double umin = 1e30, vmin = 1e30, umax = -1e30, vmax = -1e30;
  for (const PointVertex& p : pts) {
    const float h = (&p.x)[up_axis];
    if (h < lo_h || h > hi_h) continue;
    const float u = (&p.x)[a0], v = (&p.x)[a1];
    g.u.push_back(u);
    g.v.push_back(v);
    umin = std::min(umin, static_cast<double>(u));
    vmin = std::min(vmin, static_cast<double>(v));
    umax = std::max(umax, static_cast<double>(u));
    vmax = std::max(vmax, static_cast<double>(v));
  }
  if (g.u.size() < 2000) return g;

  // One cell of margin on every side, so a probe centre from the reference
  // pass still has its full neighbourhood in range when a different offset
  // shifts the cloud by a few centimetres.
  const double pad = 1.0;
  g.umin = umin - pad;
  g.vmin = vmin - pad;
  g.nu = static_cast<int>((umax - umin + 2 * pad) / cell) + 2;
  g.nv = static_cast<int>((vmax - vmin + 2 * pad) / cell) + 2;
  if (g.nu < 4 || g.nv < 4) return g;
  if (static_cast<std::int64_t>(g.nu) * g.nv > 40'000'000) return g;

  auto cell_of = [&](std::size_t i) {
    const int cu = static_cast<int>((g.u[i] - g.umin) / cell);
    const int cv = static_cast<int>((g.v[i] - g.vmin) / cell);
    return static_cast<std::size_t>(cu) * g.nv + static_cast<std::size_t>(cv);
  };
  g.head.assign(static_cast<std::size_t>(g.nu) * g.nv + 1, 0);
  for (std::size_t i = 0; i < g.u.size(); ++i) ++g.head[cell_of(i) + 1];
  for (std::size_t i = 1; i < g.head.size(); ++i) g.head[i] += g.head[i - 1];
  g.order.resize(g.u.size());
  {
    std::vector<std::uint32_t> cur(g.head.begin(), g.head.end() - 1);
    for (std::uint32_t i = 0; i < g.u.size(); ++i) g.order[cur[cell_of(i)]++] = i;
  }
  g.ok = true;
  return g;
}

// Second moment of the neighbourhood of (u0, v0) within `radius`, in the
// direction ACROSS the local line — i.e. half the wall's thickness, as a
// standard deviation. Returns false when the neighbourhood is too sparse or
// too round to be a wall.
bool probe_thickness(const BandGrid& g, double u0, double v0, double radius,
                     std::size_t min_points, double min_elongation, double* out_sigma_m,
                     std::size_t* out_n) {
  const int ring = static_cast<int>(std::ceil(radius / g.cell));
  const int cu = static_cast<int>((u0 - g.umin) / g.cell);
  const int cv = static_cast<int>((v0 - g.vmin) / g.cell);
  if (cu < ring || cv < ring || cu + ring >= g.nu || cv + ring >= g.nv) return false;

  const double r2 = radius * radius;
  std::size_t n = 0;
  double su = 0, sv = 0, suu = 0, suv = 0, svv = 0;
  for (int du = -ring; du <= ring; ++du) {
    for (int dv = -ring; dv <= ring; ++dv) {
      const std::size_t c =
          static_cast<std::size_t>(cu + du) * g.nv + static_cast<std::size_t>(cv + dv);
      for (std::uint32_t k = g.head[c]; k < g.head[c + 1]; ++k) {
        const std::uint32_t i = g.order[k];
        const double du2 = g.u[i] - u0, dv2 = g.v[i] - v0;
        if (du2 * du2 + dv2 * dv2 > r2) continue;
        ++n;
        su += du2; sv += dv2;
        suu += du2 * du2; suv += du2 * dv2; svv += dv2 * dv2;
      }
    }
  }
  if (out_n != nullptr) *out_n = n;
  if (n < min_points) return false;
  const double inv = 1.0 / static_cast<double>(n);
  const double mu = su * inv, mv = sv * inv;
  const double cuu = suu * inv - mu * mu;
  const double cuv = suv * inv - mu * mv;
  const double cvv = svv * inv - mv * mv;
  // Closed-form eigenvalues of a symmetric 2x2 — no Eigen (determinism
  // doctrine), and a 2x2 needs none.
  const double tr = cuu + cvv;
  const double det = cuu * cvv - cuv * cuv;
  const double disc = std::sqrt(std::max(0.0, tr * tr * 0.25 - det));
  const double l1 = tr * 0.5 + disc;  // along the wall
  const double l2 = tr * 0.5 - disc;  // across it
  if (l2 <= 0.0 || l1 < min_elongation * l2) return false;
  *out_sigma_m = std::sqrt(l2);
  return true;
}

// Radius, minimum neighbourhood and elongation for a wall probe.
//
// 0.5 m is chosen so the discrimination actually works. Over a 1 m span a
// straight wall has an along-length variance of ~0.083 m^2 against a 3 cm
// thickness's 0.0009 m^2 — a ratio near 90, so an elongation floor of 20
// separates walls from corners and furniture with a wide margin AND still
// admits a smeared wall up to ~6 cm thick, which matters: a filter that
// rejects the blurry version of the same wall would measure how many walls
// survive selection instead of how thick they are. At 0.25 m the same ratio
// is only ~23 and the filter starts selecting for the answer.
constexpr double kProbeRadiusM = 0.5;
// ROUND 12: these were constants, and 200 points inside a 0.5 m radius cell is
// a density only the 5.3 cm/s crawl of scan-020 reaches. On BOTH of the
// owner's walking-pace captures the selection returns **zero probes** and
// --d6-timesweep prints nothing at all — so every crispness claim this
// repository has made came from the one capture walked twenty times slower
// than the product is meant to be used at.
//
// They are now variables with the same defaults (so scan-020's published
// numbers are unchanged) and a --probe-min-points flag, and
// --d6-selfcheck exists for the case this metric structurally cannot serve.
std::size_t kProbeMinPoints = 200;
double kProbeMinElongation = 20.0;

// Pick the probe centres from the reference cloud: the busiest 10 cm cells
// that pass the wall test, spread out so two probes cannot sit on the same
// half-metre of wall and count it twice.
std::vector<WallProbe> choose_wall_probes(const BandGrid& g, std::size_t max_probes) {
  std::vector<WallProbe> out;
  if (!g.ok) return out;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> cells;
  for (std::size_t c = 0; c + 1 < g.head.size(); ++c) {
    const std::uint32_t n = g.head[c + 1] - g.head[c];
    if (n >= 40) cells.push_back({n, static_cast<std::uint32_t>(c)});
  }
  // (count desc, cell index asc) — ties broken by index so two runs cannot
  // disagree about the order.
  std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second < b.second;
  });
  std::vector<std::uint8_t> taken(g.head.size(), 0);
  const int excl = static_cast<int>(std::ceil(0.5 * kProbeRadiusM / g.cell));
  for (const auto& c : cells) {
    if (out.size() >= max_probes) break;
    if (taken[c.second]) continue;
    const int cu = static_cast<int>(c.second / static_cast<std::uint32_t>(g.nv));
    const int cv = static_cast<int>(c.second % static_cast<std::uint32_t>(g.nv));
    const double u0 = g.umin + (cu + 0.5) * g.cell;
    const double v0 = g.vmin + (cv + 0.5) * g.cell;
    double sigma = 0.0;
    if (!probe_thickness(g, u0, v0, kProbeRadiusM, kProbeMinPoints, kProbeMinElongation, &sigma,
                         nullptr)) {
      continue;
    }
    out.push_back(WallProbe{u0, v0});
    for (int du = -excl; du <= excl; ++du) {
      for (int dv = -excl; dv <= excl; ++dv) {
        const int nu2 = cu + du, nv2 = cv + dv;
        if (nu2 < 0 || nv2 < 0 || nu2 >= g.nu || nv2 >= g.nv) continue;
        taken[static_cast<std::size_t>(nu2) * g.nv + static_cast<std::size_t>(nv2)] = 1;
      }
    }
  }
  return out;
}

CrispnessMetrics measure_crispness(const std::vector<PointVertex>& pts, int up_axis,
                                   const BandGrid& band,
                                   const std::vector<WallProbe>& probes) {
  CrispnessMetrics m;
  m.points = pts.size();
  (void)up_axis;
  if (pts.size() < 1000) return m;

  // --- (2) and (3): 3 cm voxels over everything --------------------------
  constexpr double kVoxel = 0.03;
  {
    // A packed 64-bit key, sorted. std::map would be O(n log n) with a huge
    // constant on 600k points; the packing is exact for any room inside
    // +/-16 km at 3 cm, which is every room.
    std::vector<std::uint64_t> keys;
    keys.reserve(pts.size());
    for (const PointVertex& p : pts) {
      const std::int64_t i = static_cast<std::int64_t>(std::floor(p.x / kVoxel));
      const std::int64_t j = static_cast<std::int64_t>(std::floor(p.y / kVoxel));
      const std::int64_t k = static_cast<std::int64_t>(std::floor(p.z / kVoxel));
      keys.push_back((static_cast<std::uint64_t>(i + 2'097'152) << 42) |
                     (static_cast<std::uint64_t>(j + 2'097'152) << 21) |
                     static_cast<std::uint64_t>(k + 2'097'152));
    }
    std::sort(keys.begin(), keys.end());
    const double n = static_cast<double>(keys.size());
    std::size_t i = 0;
    while (i < keys.size()) {
      std::size_t j = i;
      while (j < keys.size() && keys[j] == keys[i]) ++j;
      const double p = static_cast<double>(j - i) / n;
      m.entropy_bits -= p * std::log2(p);
      ++m.occupied_voxels;
      i = j;
    }
  }

  // --- (1) wall thickness, at the REFERENCE probe locations --------------
  if (band.ok && !probes.empty()) {
    double sum = 0.0;
    std::uint64_t n = 0;
    for (const WallProbe& w : probes) {
      double sigma = 0.0;
      // The elongation floor is dropped to 1.0 HERE (i.e. off): the probe has
      // already been established as a wall by the reference pass, and
      // re-applying the shape filter at every offset would quietly delete the
      // worst-smeared walls from the average — which is the one bias that
      // would make a bad offset look good.
      if (!probe_thickness(band, w.u, w.v, kProbeRadiusM, kProbeMinPoints, 1.0, &sigma, nullptr)) {
        continue;
      }
      sum += sigma;
      ++n;
    }
    if (n > 0) {
      m.wall_rms_cm = 100.0 * sum / static_cast<double>(n);
      m.wall_cells = n;
    }
  }
  return m;
}

// Resolve `lscan_dir` once at `offset_ns` and return every world point.
bool resolve_at(const std::string& lscan_dir, std::int64_t offset_ns, bool densify,
                std::vector<PointVertex>* out, post::D6ResolveStats* out_stats) {
  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);

  post::D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.densify_with_phone_imu = densify;
  cfg.pushbroom.pose_time_offset_ns = offset_ns;
  post::D6ResolvePipeline pipe(cfg);
  const Status s = pipe.run(lscan_dir);
  if (!s.ok()) {
    std::fprintf(stderr, "d6-timesweep: resolve failed at %+.2f ms: %s\n",
                 static_cast<double>(offset_ns) / 1e6, error_str(s.error()));
    return false;
  }
  if (out_stats != nullptr) *out_stats = pipe.stats();
  out->clear();
  out->reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) out->push_back(v.data[k]);
  }
  return true;
}


// --- ROUND 12: raw dump for offline adjudication -----------------------------
//
// A development tool, not a product feature. ROUND 12 has to answer "is the
// shift the mount trim or something else?" on two real captures, and every
// metric this file already owns is a SCALAR — it can say a cloud is blurrier
// but not why, and it cannot separate the outbound leg of a walk from the
// return leg, which is the one distinction ROUND 11 item 45c proved matters.
//
// So: resolve the container through the production pipeline exactly as
// --d6-timesweep does, then write the points WITH THEIR OWN TIMESTAMPS and
// the trajectory to two flat files that an analysis script can slice any way
// it likes. No geometry is computed here, deliberately — the point of the
// dump is that the adjudication happens somewhere it can be re-run and argued
// with, not inside a printf.
//
// `--mount-from OTHER.lscan` is the decisive experiment: resolve THESE bytes
// with THAT capture's mount extrinsic. If the shift is the trim, swapping the
// trim moves it; if it does not move, the trim is not the cause.
int cmd_d6_dump(const std::string& lscan_dir, const std::string& out_prefix,
                const std::string& mount_from, std::int64_t offset_ns, bool densify) {
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok() || !is_d6) {
    std::fprintf(stderr, "d6-dump: '%s' is not a readable COIN-D6 project\n", lscan_dir.c_str());
    return kExitFailed;
  }

  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);

  std::vector<post::TrajPose> traj;
  std::vector<std::int64_t> ptimes;

  post::D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.densify_with_phone_imu = densify;
  cfg.pushbroom.pose_time_offset_ns = offset_ns;
  cfg.out_trajectory = &traj;
  cfg.out_point_times = &ptimes;

  if (!mount_from.empty()) {
    double m[16];
    if (!post::read_manifest_mount(mount_from, m)) {
      std::fprintf(stderr, "d6-dump: cannot read mountCalibration from '%s'\n",
                   mount_from.c_str());
      return kExitFailed;
    }
    cfg.have_mount = true;
    for (int i = 0; i < 16; ++i) cfg.mount_phone_from_lidar[i] = m[i];
    std::printf("d6-dump: mount extrinsic OVERRIDDEN from %s\n", mount_from.c_str());
  }

  post::D6ResolvePipeline pipe(cfg);
  const Status s = pipe.run(lscan_dir);
  if (!s.ok()) {
    std::fprintf(stderr, "d6-dump: resolve failed: %s\n", error_str(s.error()));
    return kExitFailed;
  }
  const post::D6ResolveStats& st = pipe.stats();

  std::vector<PointVertex> pts;
  pts.reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) pts.push_back(v.data[k]);
  }

  // points: float32 x, y, z, intensity(as float) + int64 t_ns, per record.
  const std::string pfile = out_prefix + ".points.bin";
  std::FILE* pf = std::fopen(pfile.c_str(), "wb");
  if (pf == nullptr) {
    std::fprintf(stderr, "d6-dump: cannot write %s\n", pfile.c_str());
    return kExitFailed;
  }
  const std::size_t n = std::min(pts.size(), ptimes.size());
  for (std::size_t i = 0; i < n; ++i) {
    float rec[4] = {pts[i].x, pts[i].y, pts[i].z, static_cast<float>(pts[i].r)};
    std::int64_t t = ptimes[i];
    std::fwrite(rec, sizeof(float), 4, pf);
    std::fwrite(&t, sizeof(std::int64_t), 1, pf);
  }
  std::fclose(pf);

  const std::string tfile = out_prefix + ".traj.csv";
  std::FILE* tf = std::fopen(tfile.c_str(), "wb");
  if (tf == nullptr) {
    std::fprintf(stderr, "d6-dump: cannot write %s\n", tfile.c_str());
    return kExitFailed;
  }
  std::fprintf(tf, "t_ns,px,py,pz,qx,qy,qz,qw\n");
  for (const post::TrajPose& tp : traj) {
    std::fprintf(tf, "%lld,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n",
                 static_cast<long long>(tp.t_ns), tp.p[0], tp.p[1], tp.p[2], tp.q[0], tp.q[1],
                 tp.q[2], tp.q[3]);
  }
  std::fclose(tf);

  std::printf("d6-dump: %s\n", lscan_dir.c_str());
  std::printf("  points %zu (paired %zu), poses %llu read / %llu accepted, traj %zu\n", pts.size(),
              n, static_cast<unsigned long long>(st.poses_read),
              static_cast<unsigned long long>(st.poses_accepted), traj.size());
  std::printf("  mount from manifest: %s; imu extrinsics from manifest: %s\n",
              st.mount_from_manifest ? "yes" : "NO", st.imu_extrinsics_from_manifest ? "yes" : "NO");
  // ROUND 14: every bucket, and the sum, because the reasons are only worth
  // printing if they account for the whole of `fell back` — see
  // ImuDensifyStats. A `sum` that does not equal `fell back` is a missing
  // counter, not a rounding detail.
  std::printf("  densify: %llu on gyro path, %llu fell back "
              "(no-pose %llu, gated %llu, no-imu %llu, gap %llu, wide-bracket %llu, "
              "closing %llu; sum %llu)\n",
              static_cast<unsigned long long>(st.imu_densified),
              static_cast<unsigned long long>(st.imu_fallbacks),
              static_cast<unsigned long long>(st.imu.fallback_no_pose),
              static_cast<unsigned long long>(st.imu.fallback_gate),
              static_cast<unsigned long long>(st.imu.fallback_no_imu),
              static_cast<unsigned long long>(st.imu.fallback_gap),
              static_cast<unsigned long long>(st.imu.fallback_bracket),
              static_cast<unsigned long long>(st.imu.fallback_closing),
              static_cast<unsigned long long>(
                  st.imu.fallback_no_pose + st.imu.fallback_gate + st.imu.fallback_no_imu +
                  st.imu.fallback_gap + st.imu.fallback_bracket + st.imu.fallback_closing));
  std::printf("  imu closing error: mean %.4f deg, worst %.4f deg; bias (%.5f, %.5f, %.5f) rad/s\n",
              st.imu.mean_closing_deg, st.imu.worst_closing_deg, st.imu.bias_rad_s[0],
              st.imu.bias_rad_s[1], st.imu.bias_rad_s[2]);
  {
    const PushbroomStats& pb = st.pushbroom;
    std::printf("  pushbroom: in %llu -> out %llu (range %llu, no-pose %llu, overflow %llu, "
                "page-full %llu); flagged lost %llu stale %llu lowconf %llu emitted %llu\n",
                static_cast<unsigned long long>(pb.points_in),
                static_cast<unsigned long long>(pb.points_out),
                static_cast<unsigned long long>(pb.dropped_range),
                static_cast<unsigned long long>(pb.dropped_no_pose),
                static_cast<unsigned long long>(pb.dropped_overflow),
                static_cast<unsigned long long>(pb.dropped_page_full),
                static_cast<unsigned long long>(pb.flagged_tracking_lost),
                static_cast<unsigned long long>(pb.flagged_stale_pose),
                static_cast<unsigned long long>(pb.flagged_low_confidence),
                static_cast<unsigned long long>(pb.flagged_emitted));
  }
  std::printf("  wrote %s and %s\n", pfile.c_str(), tfile.c_str());
  return kExitOk;
}


// --- ROUND 12: does the map agree with ITSELF? -------------------------------
//
// The metric --d6-timesweep scores with (wall-probe thickness) selects ZERO
// probes on both of the owner's walking-pace captures, so this project had no
// way to score a scan taken at the speed it is meant to be used at. See
// slam/post/map_consistency.h for why plane fits and voxel counts structurally
// cannot see a surface painted twice in two places.
//
// `--offset-ms` and `--mount-from` are here for the same reason they are on
// --d6-dump: the two adjudication experiments ROUND 12 needed were "resolve
// these bytes with that capture's trim" and "resolve them at a different
// lidar->pose offset", and a score is only useful if the thing being scored
// can be varied.

// ROUND 15 item 56. The floor plan, from a sealed container, on the command
// line — the same post::floor_plan_from_lscan() the C ABI and therefore the
// phone call, so what the owner sees here is what the phone produces.
int cmd_d6_plan(const std::string& lscan_dir, post::LscanPlanOptions opts) {
  post::LscanPlanReport r;
  const Status s = post::floor_plan_from_lscan(lscan_dir, opts, &r);
  if (!s.ok()) {
    std::fprintf(stderr, "d6-plan: %s (%s)\n", error_str(s.error()), last_error_message());
    return kExitFailed;
  }
  std::printf("d6-plan: %s\n", lscan_dir.c_str());
  std::printf("  cloud %llu points from %s\n", static_cast<unsigned long long>(r.cloud_points),
              r.cloud_source);
  std::printf("  slice %.2f-%.2f m (up %s), %llu points in band, %u occupied cells "
              "%ux%u @ %.3f m (min %u pts/cell)\n",
              r.slice_min_m, r.slice_max_m, plan::to_string(opts.up),
              static_cast<unsigned long long>(r.band_points), r.occupied_cells, r.grid_w, r.grid_h,
              r.grid_res_used_m, r.min_cell_points_used);
  std::printf("  MODE %s — %s\n", plan::to_string(r.mode), r.summary);
  std::printf("  floor map: %llu points spanning >=%.2f m in a %.2f m cell -> %u cells\n",
              static_cast<unsigned long long>(r.map_band_points), opts.map_min_span_m,
              opts.map_res_m, r.map_cells);
  std::printf("  walls %u (%u with MEASURED thickness) from %s, openings %u (%u door, %u window), "
              "rooms %u\n",
              r.walls, r.walls_paired, r.walls_from_floor_map ? "the FLOOR MAP" : "the plan slice",
              r.openings, r.doors, r.windows, r.rooms);
  std::printf("  wall length %.2f m, room area %.2f m2 (largest %.2f m2), extent %.2f x %.2f m\n",
              r.total_wall_length_m, r.total_room_area_m2, r.largest_room_area_m2, r.extent_x_m,
              r.extent_y_m);
  if (!r.png_path.empty()) {
    std::printf("  PNG %s (%ux%u, %.1f px/m, scale bar %.2f m)\n", r.png_path.c_str(), r.png_w,
                r.png_h, r.png_px_per_m, r.png_scale_bar_m);
  }
  if (!r.dxf_path.empty()) std::printf("  DXF %s\n", r.dxf_path.c_str());
  if (!r.pdf_path.empty()) std::printf("  PDF %s\n", r.pdf_path.c_str());
  return kExitOk;
}

int cmd_d6_selfcheck(const std::string& lscan_dir, const std::string& mount_from,
                     std::int64_t offset_ns, bool densify, double window_s, double cell_m) {
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok() || !is_d6) {
    std::fprintf(stderr, "d6-selfcheck: '%s' is not a readable COIN-D6 project\n",
                 lscan_dir.c_str());
    return kExitFailed;
  }

  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);
  std::vector<post::TrajPose> traj;
  std::vector<std::int64_t> ptimes;

  post::D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.densify_with_phone_imu = densify;
  cfg.pushbroom.pose_time_offset_ns = offset_ns;
  cfg.out_trajectory = &traj;
  cfg.out_point_times = &ptimes;
  if (!mount_from.empty()) {
    double m[16];
    if (!post::read_manifest_mount(mount_from, m)) {
      std::fprintf(stderr, "d6-selfcheck: cannot read mountCalibration from '%s'\n",
                   mount_from.c_str());
      return kExitFailed;
    }
    cfg.have_mount = true;
    for (int i = 0; i < 16; ++i) cfg.mount_phone_from_lidar[i] = m[i];
  }

  post::D6ResolvePipeline pipe(cfg);
  const Status s = pipe.run(lscan_dir);
  if (!s.ok()) {
    std::fprintf(stderr, "d6-selfcheck: resolve failed: %s\n", error_str(s.error()));
    return kExitFailed;
  }

  std::vector<PointVertex> pts;
  pts.reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) pts.push_back(v.data[k]);
  }

  double path_m = 0.0;
  for (std::size_t i = 1; i < traj.size(); ++i) {
    const double dx = traj[i].p[0] - traj[i - 1].p[0];
    const double dy = traj[i].p[1] - traj[i - 1].p[1];
    const double dz = traj[i].p[2] - traj[i - 1].p[2];
    path_m += std::sqrt(dx * dx + dy * dy + dz * dz);
  }
  const double dur_s = traj.size() >= 2
                           ? static_cast<double>(traj.back().t_ns - traj.front().t_ns) * 1e-9
                           : 0.0;

  post::MapConsistencyConfig mcfg;
  mcfg.window_seconds = window_s;
  mcfg.cell_m = cell_m;
  const post::MapConsistencyReport r = post::measure_map_consistency(pts, ptimes, mcfg);

  std::printf("d6-selfcheck: %s\n", lscan_dir.c_str());
  if (!mount_from.empty()) std::printf("  mount extrinsic OVERRIDDEN from %s\n", mount_from.c_str());
  if (offset_ns != 0) {
    std::printf("  lidar->pose offset %+.1f ms\n", static_cast<double>(offset_ns) / 1e6);
  }
  std::printf("  %zu points, %zu poses, %.1f s, %.2f m walked (%.3f m/s), "
              "window %.1f s, cell %.2f m\n",
              pts.size(), traj.size(), dur_s, path_m, dur_s > 0 ? path_m / dur_s : 0.0, window_s,
              cell_m);
  if (!r.measurable) {
    std::printf("  NOT MEASURABLE: %s\n", r.blocker);
    return kExitFailed;
  }
  std::printf("  measurement floor (one window against itself): %.2f cm over %zu cells\n",
              r.self_floor_m * 100.0, r.self_cells);
  std::printf("  separation      median      p90    cells\n");
  for (const post::MapConsistencySeparation& sp : r.by_separation) {
    std::printf("   %5.0f s      %7.2f cm %7.2f cm %6zu\n", sp.seconds, sp.median_offset_m * 100.0,
                sp.p90_offset_m * 100.0, sp.cells);
  }
  std::printf("\n  SELF-CONSISTENCY at %.0f s: %.2f cm (floor %.2f cm)\n",
              static_cast<double>(r.nearest_separation) * window_s, r.nearest_offset_m * 100.0,
              r.self_floor_m * 100.0);
  return kExitOk;
}

// ROUND 13. Resolve a COIN-D6 capture twice — as shipped, and with section
// stitching — and report what moved and whether the map agrees with itself
// better afterwards.
int cmd_d6_stitch(const std::string& lscan_dir, bool refine, bool densify, double window_s,
                  double cell_m, double max_refine_m) {
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok() || !is_d6) {
    std::fprintf(stderr, "d6-stitch: '%s' is not a readable COIN-D6 project\n", lscan_dir.c_str());
    return kExitFailed;
  }

  struct Run {
    std::vector<PointVertex> pts;
    std::vector<std::int64_t> times;
    std::vector<post::TrajPose> traj;
    post::D6ResolveStats stats;
  };
  Run before, after;

  auto resolve = [&](bool stitch, Run* r) -> bool {
    PageStoreConfig psc;
    psc.page_capacity = 1u << 20;
    psc.max_pages = 4096;
    PageStore store(psc);
    post::D6ResolveConfig cfg;
    cfg.store = &store;
    cfg.densify_with_phone_imu = densify;
    cfg.out_trajectory = &r->traj;
    cfg.out_point_times = &r->times;
    cfg.stitch_sections = stitch;
    cfg.sections.refine = refine;
    cfg.sections.max_refine_translation_m = max_refine_m;
    post::D6ResolvePipeline pipe(cfg);
    const Status s = pipe.run(lscan_dir);
    if (!s.ok()) {
      std::fprintf(stderr, "d6-stitch: resolve failed: %s\n", error_str(s.error()));
      return false;
    }
    r->stats = pipe.stats();
    r->pts.reserve(static_cast<std::size_t>(store.total_points()));
    for (const PageId id : store.page_ids()) {
      const PageView v = store.page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) r->pts.push_back(v.data[k]);
    }
    return true;
  };
  if (!resolve(false, &before)) return kExitFailed;
  if (!resolve(true, &after)) return kExitFailed;

  auto vertical = [](const std::vector<post::TrajPose>& t) {
    if (t.empty()) return 0.0;
    double lo = t[0].p[1], hi = t[0].p[1];
    for (const post::TrajPose& p : t) {
      lo = std::min(lo, p.p[1]);
      hi = std::max(hi, p.p[1]);
    }
    return hi - lo;
  };
  auto endgap = [](const std::vector<post::TrajPose>& t) {
    if (t.size() < 2) return 0.0;
    const double dx = t.back().p[0] - t.front().p[0];
    const double dy = t.back().p[1] - t.front().p[1];
    const double dz = t.back().p[2] - t.front().p[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };

  post::MapConsistencyConfig mcfg;
  mcfg.window_seconds = window_s;
  mcfg.cell_m = cell_m;
  const post::MapConsistencyReport rb =
      post::measure_map_consistency(before.pts, before.times, mcfg);
  const post::MapConsistencyReport ra = post::measure_map_consistency(after.pts, after.times, mcfg);

  const post::SectionStitchReport& sr = after.stats.sections;
  std::printf("d6-stitch: %s\n", lscan_dir.c_str());
  std::printf("  %zu points, %zu poses, %zu sections, refine=%s\n", before.pts.size(),
              before.traj.size(), sr.sections, refine ? "on" : "off");
  if (sr.sections <= 1) {
    std::printf("  one section — nothing to stitch (this is the clean case)\n");
  }
  for (const post::SectionSeam& s : sr.seams) {
    std::printf("  seam %zu at t=%.2f s: ARCore jumped %.3f m / %.2f deg in %.0f ms -> %s\n",
                s.index + 1, static_cast<double>(s.t_ns - before.traj.front().t_ns) * 1e-9,
                s.jump_translation_m, s.jump_rotation_deg, s.gap_s * 1e3,
                post::to_string(s.decision));
    std::printf("       %s\n", s.reason);
    if (s.pairs > 0 || s.submap_before_points > 0) {
      std::printf("       submaps %zu/%zu pts, %zu pairs, observability %.3f\n",
                  s.submap_before_points, s.submap_after_points, s.pairs, s.observability);
    }
    if (s.decision == post::SeamDecision::kRefined ||
        s.decision == post::SeamDecision::kMapGotWorse) {
      std::printf("       across-seam mismatch %.2f cm -> %.2f cm; refinement %+.3f %+.3f %+.3f m\n",
                  s.mismatch_analytic_m * 100.0, s.mismatch_refined_m * 100.0, s.refine_delta[0],
                  s.refine_delta[1], s.refine_delta[2]);
    }
  }
  // ROUND 17 item 63: the long gaps this pass LOOKED at. A gap that was
  // refused or found negligible moved nothing and is therefore in no seam
  // list, which is precisely how scan-040's six blind seconds came to be
  // reported as "1 section".
  for (const post::SectionSeam& g : sr.gaps_examined) {
    std::printf("  gap at t=%.2f s: %.3f s blind; tracker %.3f m / %.2f deg, gyro %.2f deg, "
                "residual %.3f m / %.2f deg -> %s\n",
                static_cast<double>(g.t_ns - before.traj.front().t_ns) * 1e-9, g.gap_s,
                g.jump_translation_m, g.jump_rotation_deg, g.gyro_rotation_deg,
                g.residual_translation_m, g.residual_rotation_deg,
                post::to_string(g.decision));
    std::printf("       %s\n", g.reason);
  }
  if (sr.longest_gap_s > 0.0) {
    std::printf("  longest blind stretch %.3f s; %zu gap(s) refused\n", sr.longest_gap_s,
                sr.gaps_refused);
  }
  std::printf("\n  first section moved %.3f m / %.2f deg into the last section's frame\n",
              sr.total_translation_m, sr.total_rotation_deg);
  std::printf("  trajectory VERTICAL extent (flat floor, so smaller is right): %.3f m -> %.3f m\n",
              vertical(before.traj), vertical(after.traj));
  std::printf("  trajectory start->end gap: %.3f m -> %.3f m\n", endgap(before.traj),
              endgap(after.traj));
  std::printf("  map self-consistency at %.0f s: ", window_s);
  if (!rb.measurable || !ra.measurable) {
    std::printf("not measurable (%s)\n", rb.measurable ? ra.blocker : rb.blocker);
  } else {
    std::printf("%.2f cm -> %.2f cm  (floor %.2f -> %.2f cm, %zu -> %zu cells)\n",
                rb.nearest_offset_m * 100.0, ra.nearest_offset_m * 100.0, rb.self_floor_m * 100.0,
                ra.self_floor_m * 100.0, rb.self_cells, ra.self_cells);
    std::printf("  separation      before       after     cells(before/after)\n");
    for (std::size_t i = 0; i < rb.by_separation.size() && i < ra.by_separation.size(); ++i) {
      std::printf("   %5.0f s      %7.2f cm  %7.2f cm   %5zu / %zu\n", rb.by_separation[i].seconds,
                  rb.by_separation[i].median_offset_m * 100.0,
                  ra.by_separation[i].median_offset_m * 100.0, rb.by_separation[i].cells,
                  ra.by_separation[i].cells);
    }
  }
  return kExitOk;
}

// ROUND 13 (owner item 48). Does the PHYSICAL mount still match the stored
// trim? Judged from where the returns land, because the fan's own attitude is
// unobservable from the fan — see slam/post/mount_watch.h.
int cmd_d6_mountcheck(const std::string& lscan_dir, double window_s, int up_axis, bool densify) {
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok() || !is_d6) {
    std::fprintf(stderr, "d6-mountcheck: '%s' is not a readable COIN-D6 project\n",
                 lscan_dir.c_str());
    return kExitFailed;
  }
  PageStoreConfig psc;
  psc.page_capacity = 1u << 20;
  psc.max_pages = 4096;
  PageStore store(psc);
  std::vector<post::TrajPose> traj;
  std::vector<std::int64_t> ptimes;
  post::D6ResolveConfig cfg;
  cfg.store = &store;
  cfg.densify_with_phone_imu = densify;
  cfg.out_trajectory = &traj;
  cfg.out_point_times = &ptimes;
  post::D6ResolvePipeline pipe(cfg);
  const Status s = pipe.run(lscan_dir);
  if (!s.ok()) {
    std::fprintf(stderr, "d6-mountcheck: resolve failed: %s\n", error_str(s.error()));
    return kExitFailed;
  }
  std::vector<PointVertex> pts;
  pts.reserve(static_cast<std::size_t>(store.total_points()));
  for (const PageId id : store.page_ids()) {
    const PageView v = store.page_view(id);
    if (!v.valid()) continue;
    for (std::uint32_t k = 0; k < v.count; ++k) pts.push_back(v.data[k]);
  }
  post::MountWatchConfig mc;
  mc.window_seconds = window_s;
  mc.up_axis = up_axis;
  const post::MountWatchReport r = post::check_mount_consistency(
      traj, Span<const PointVertex>(pts.data(), pts.size()),
      Span<const std::int64_t>(ptimes.data(), ptimes.size()), mc);

  std::printf("d6-mountcheck: %s\n", lscan_dir.c_str());
  std::printf("  window %.1f s (0 = whole capture), up axis %d\n", window_s, up_axis);
  std::printf("  %zu revolutions, %zu returns, median range %.2f m\n", r.revolutions, r.points,
              r.median_range_m);
  std::printf("  median per-revolution vertical extent : %.2f m\n", r.median_revolution_extent_m);
  std::printf("  returns at impossible elevations      : %.2f %%\n", 100.0 * r.impossible_fraction);
  std::printf("\n  VERDICT: %s — %s\n", post::to_string(r.verdict), r.reason);
  if (r.operator_message[0] != '\0') std::printf("  operator: \"%s\"\n", r.operator_message);
  return r.verdict == post::MountWatchVerdict::kMismatch ? kExitFailed : kExitOk;
}

int cmd_d6_timesweep(const std::string& lscan_dir, double from_ms, double to_ms, double step_ms,
                     bool densify, int up_axis) {
  if (step_ms <= 0.0 || to_ms < from_ms) {
    std::fprintf(stderr, "d6-timesweep: bad sweep range\n");
    return kExitUsage;
  }
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok()) {
    std::fprintf(stderr, "d6-timesweep: cannot read '%s': %s\n", lscan_dir.c_str(),
                 error_str(probe.error()));
    return kExitFailed;
  }
  if (!is_d6) {
    std::fprintf(stderr, "d6-timesweep: '%s' is not a COIN-D6 project\n", lscan_dir.c_str());
    return kExitFailed;
  }

  std::printf("d6-timesweep: %s\n", lscan_dir.c_str());
  std::printf("  sweep %+.1f .. %+.1f ms step %.1f ms, IMU densification %s, up = %c\n", from_ms,
              to_ms, step_ms, densify ? "ON" : "off", "XYZ"[up_axis]);

  // --- the reference pass -------------------------------------------------
  //
  // Offset 0 — i.e. what the app shipped — chooses the wall probes, and every
  // row below is measured at those same places. See choose_wall_probes().
  std::vector<PointVertex> ref;
  post::D6ResolveStats rstats;
  if (!resolve_at(lscan_dir, 0, densify, &ref, &rstats)) return kExitFailed;
  const BandGrid ref_grid = build_band_grid(ref, up_axis, 0.10);
  const std::vector<WallProbe> probes = choose_wall_probes(ref_grid, 400);
  std::printf("  reference: %zu points, %zu band points, %zu wall probes "
              "(r = %.2f m, min %zu pts, elongation >= %.0f)\n",
              ref.size(), ref_grid.u.size(), probes.size(), kProbeRadiusM, kProbeMinPoints,
              kProbeMinElongation);
  std::printf("  densify  : %llu returns on the gyro path, %llu fell back\n",
              static_cast<unsigned long long>(rstats.imu_densified),
              static_cast<unsigned long long>(rstats.imu_fallbacks));
  std::printf("             fallbacks by reason: no-pose %llu, gated %llu, no-imu %llu, "
              "imu-gap %llu, wide-bracket %llu, closing %llu (sum %llu)\n",
              static_cast<unsigned long long>(rstats.imu.fallback_no_pose),
              static_cast<unsigned long long>(rstats.imu.fallback_gate),
              static_cast<unsigned long long>(rstats.imu.fallback_no_imu),
              static_cast<unsigned long long>(rstats.imu.fallback_gap),
              static_cast<unsigned long long>(rstats.imu.fallback_bracket),
              static_cast<unsigned long long>(rstats.imu.fallback_closing),
              static_cast<unsigned long long>(
                  rstats.imu.fallback_no_pose + rstats.imu.fallback_gate +
                  rstats.imu.fallback_no_imu + rstats.imu.fallback_gap +
                  rstats.imu.fallback_bracket + rstats.imu.fallback_closing));
  std::printf("             closing error: mean %.3f deg, worst %.3f deg; "
              "gyro bias (%.5f, %.5f, %.5f) rad/s from %llu updates\n",
              rstats.imu.mean_closing_deg, rstats.imu.worst_closing_deg, rstats.imu.bias_rad_s[0],
              rstats.imu.bias_rad_s[1], rstats.imu.bias_rad_s[2],
              static_cast<unsigned long long>(rstats.imu.bias_updates));

  {
    const PushbroomStats& pb = rstats.pushbroom;
    std::printf("  pushbroom: in %llu -> out %llu  (dropped: range %llu, no-pose %llu, "
                "overflow %llu, page-full %llu)\n",
                static_cast<unsigned long long>(pb.points_in),
                static_cast<unsigned long long>(pb.points_out),
                static_cast<unsigned long long>(pb.dropped_range),
                static_cast<unsigned long long>(pb.dropped_no_pose),
                static_cast<unsigned long long>(pb.dropped_overflow),
                static_cast<unsigned long long>(pb.dropped_page_full));
    std::printf("             flagged: tracking-lost %llu, stale %llu, low-confidence %llu "
                "(emitted %llu)\n",
                static_cast<unsigned long long>(pb.flagged_tracking_lost),
                static_cast<unsigned long long>(pb.flagged_stale_pose),
                static_cast<unsigned long long>(pb.flagged_low_confidence),
                static_cast<unsigned long long>(pb.flagged_emitted));
  }  if (probes.empty()) {
    std::fprintf(stderr,
                 "d6-timesweep: no wall probes — this capture has no straight surface to "
                 "measure against\n");
    return kExitFailed;
  }
  std::printf("\n");
  std::printf("  offset_ms      points  occupied_vox  entropy_bits  wall_rms_cm  probes\n");

  double best_ms = 0.0;
  double best_rms = 1e30;
  double zero_rms = -1.0;
  const int steps = static_cast<int>(std::llround((to_ms - from_ms) / step_ms));
  std::vector<PointVertex> pts;
  for (int i = 0; i <= steps; ++i) {
    const double off_ms = from_ms + step_ms * i;
    const std::int64_t off_ns = static_cast<std::int64_t>(std::llround(off_ms * 1e6));
    if (!resolve_at(lscan_dir, off_ns, densify, &pts, nullptr)) return kExitFailed;
    const BandGrid grid = build_band_grid(pts, up_axis, 0.10);
    const CrispnessMetrics m = measure_crispness(pts, up_axis, grid, probes);
    std::printf("  %+9.2f  %10llu  %12llu  %12.5f  %11.4f  %6llu%s\n", off_ms,
                static_cast<unsigned long long>(m.points),
                static_cast<unsigned long long>(m.occupied_voxels), m.entropy_bits, m.wall_rms_cm,
                static_cast<unsigned long long>(m.wall_cells),
                std::fabs(off_ms) < 1e-9 ? "   <- zero (what 0.6.0 shipped)" : "");
    std::fflush(stdout);
    if (m.wall_cells > 0 && m.wall_rms_cm < best_rms) {
      best_rms = m.wall_rms_cm;
      best_ms = off_ms;
    }
    if (std::fabs(off_ms) < 1e-9) zero_rms = m.wall_rms_cm;
  }

  std::printf("\n");
  std::printf("  BEST offset: %+.2f ms  (wall RMS %.4f cm)\n", best_ms, best_rms);
  if (zero_rms > 0.0) {
    std::printf("  at zero    :  +0.00 ms  (wall RMS %.4f cm)  ->  %.1f%% thinner walls\n",
                zero_rms, 100.0 * (zero_rms - best_rms) / zero_rms);
  }
  return kExitOk;
}

// --- ROUND 11 item 41: loop closure on a real capture -----------------------
//
// Two resolves of the same container: as shipped, and with close_loops on.
// Both are scored with the SAME ROUND 10 crispness metric, at the SAME wall
// probes (chosen from the as-shipped pass), because a metric that re-chooses
// its own reference points after the correction can only flatter it.
int cmd_d6_loopclose(const std::string& lscan_dir, const post::TrajectoryLoopConfig& lcfg,
                     int up_axis) {
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok()) {
    std::fprintf(stderr, "d6-loopclose: cannot read '%s': %s\n", lscan_dir.c_str(),
                 error_str(probe.error()));
    return kExitFailed;
  }
  if (!is_d6) {
    std::fprintf(stderr, "d6-loopclose: '%s' is not a COIN-D6 project\n", lscan_dir.c_str());
    return kExitFailed;
  }

  std::printf("d6-loopclose: %s\n", lscan_dir.c_str());
  std::printf("  gates: revisit <= %.2f m, >= %.0f s apart, >= %.1f m of path, "
              "excursion >= %.1f m, submap +/-%.1f s\n",
              lcfg.max_revisit_m, lcfg.min_loop_seconds, lcfg.min_loop_path_m,
              lcfg.min_excursion_m, lcfg.submap_half_window_s);

  std::vector<post::TrajPose> traj;
  auto run = [&](bool close, std::vector<PointVertex>* pts, post::D6ResolveStats* st) -> bool {
    PageStoreConfig psc;
    psc.page_capacity = 1u << 20;
    psc.max_pages = 4096;
    PageStore store(psc);
    post::D6ResolveConfig cfg;
    cfg.store = &store;
    cfg.close_loops = close;
    cfg.loop = lcfg;
    traj.clear();
    cfg.out_trajectory = &traj;
    post::D6ResolvePipeline pipe(cfg);
    const Status s = pipe.run(lscan_dir);
    if (!s.ok()) {
      std::fprintf(stderr, "d6-loopclose: resolve failed: %s\n", error_str(s.error()));
      return false;
    }
    *st = pipe.stats();
    pts->clear();
    pts->reserve(static_cast<std::size_t>(store.total_points()));
    for (const PageId id : store.page_ids()) {
      const PageView v = store.page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) pts->push_back(v.data[k]);
    }
    return true;
  };

  std::vector<PointVertex> before, after;
  post::D6ResolveStats sb{}, sa{};
  if (!run(false, &before, &sb)) return kExitFailed;

  // The trajectory itself, before any geometry: this is what decides whether
  // there is a loop to find at all, and a person reading a refusal needs it.
  if (traj.size() >= 2) {
    double lo[3] = {traj[0].p[0], traj[0].p[1], traj[0].p[2]};
    double hi[3] = {lo[0], lo[1], lo[2]};
    double path = 0.0;
    double far_from_start = 0.0;
    for (std::size_t i = 0; i < traj.size(); ++i) {
      for (int k = 0; k < 3; ++k) {
        if (traj[i].p[k] < lo[k]) lo[k] = traj[i].p[k];
        if (traj[i].p[k] > hi[k]) hi[k] = traj[i].p[k];
      }
      if (i > 0) {
        double d = 0.0;
        for (int k = 0; k < 3; ++k) {
          const double e = traj[i].p[k] - traj[i - 1].p[k];
          d += e * e;
        }
        path += std::sqrt(d);
      }
      double d0 = 0.0;
      for (int k = 0; k < 3; ++k) {
        const double e = traj[i].p[k] - traj[0].p[k];
        d0 += e * e;
      }
      d0 = std::sqrt(d0);
      if (d0 > far_from_start) far_from_start = d0;
    }
    double end_gap = 0.0;
    for (int k = 0; k < 3; ++k) {
      const double e = traj.back().p[k] - traj.front().p[k];
      end_gap += e * e;
    }
    end_gap = std::sqrt(end_gap);
    std::printf("  trajectory : %zu poses over %.1f s, %.1f m walked, extent "
                "%.2f x %.2f x %.2f m, furthest from start %.2f m, start->end %.2f m\n",
                traj.size(),
                static_cast<double>(traj.back().t_ns - traj.front().t_ns) * 1e-9, path,
                hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], far_from_start, end_gap);
  }

  const BandGrid ref_grid = build_band_grid(before, up_axis, 0.10);
  const std::vector<WallProbe> probes = choose_wall_probes(ref_grid, 400);
  const CrispnessMetrics mb = measure_crispness(before, up_axis, ref_grid, probes);
  std::printf("  as shipped : %zu points, %llu occupied 3 cm voxels, entropy %.5f bits, "
              "wall RMS %.4f cm over %llu probes\n",
              before.size(), static_cast<unsigned long long>(mb.occupied_voxels), mb.entropy_bits,
              mb.wall_rms_cm, static_cast<unsigned long long>(mb.wall_cells));

  if (!run(true, &after, &sa)) return kExitFailed;

  const post::LoopClosureReport& r = sa.loop;
  std::printf("\n  DECISION: %s\n", post::to_string(r.decision));
  std::printf("  %s\n", r.reason);
  std::printf("  spatial candidates examined: %zu\n", r.candidates_seen);
  if (r.decision != post::LoopDecision::kNoRevisit &&
      r.decision != post::LoopDecision::kNoTrajectory) {
    std::printf("  revisit: t=%.1f s -> t=%.1f s  (%.1f s apart, %.1f m of path, "
                "gap %.2f m, excursion %.1f m)\n",
                static_cast<double>(r.t_a_ns) * 1e-9, static_cast<double>(r.t_b_ns) * 1e-9,
                r.loop_seconds, r.loop_path_m, r.revisit_gap_m, r.excursion_m);
    std::printf("  submaps: %zu / %zu points\n", r.submap_a_points, r.submap_b_points);
    std::printf("  ICP: converged=%d, %u iterations, %llu inliers (%.3f), rms %.4f m\n",
                r.icp.converged ? 1 : 0, r.icp.iterations,
                static_cast<unsigned long long>(r.icp.inliers), r.icp.inlier_ratio, r.icp.rms_m);
    std::printf("  same place, mean nearest-neighbour distance: %.2f cm -> %.2f cm "
                "(%zu pairs)\n",
                100.0 * r.submap_mismatch_before_m, 100.0 * r.submap_mismatch_after_m,
                r.mismatch_pairs);
    std::printf("  measured drift over the loop: %.4f m, %.3f deg\n", r.drift_translation_m,
                r.drift_rotation_deg);
    if (r.occupied_voxels_before > 0) {
      std::printf("  whole-map crispness gate: %llu -> %llu occupied 3 cm voxels (%+.2f %%)\n",
                  static_cast<unsigned long long>(r.occupied_voxels_before),
                  static_cast<unsigned long long>(r.occupied_voxels_after),
                  100.0 * (static_cast<double>(r.occupied_voxels_after) -
                           static_cast<double>(r.occupied_voxels_before)) /
                      static_cast<double>(r.occupied_voxels_before));
    }
  }

  if (!sa.loop_applied) {
    std::printf("\n  NOTHING WAS MOVED. The cloud is byte-for-byte what the app produces.\n");
    return kExitOk;
  }

  const CrispnessMetrics ma = measure_crispness(after, up_axis, ref_grid, probes);
  std::printf("\n  closed     : %zu points, %llu occupied 3 cm voxels, entropy %.5f bits, "
              "wall RMS %.4f cm over %llu probes\n",
              after.size(), static_cast<unsigned long long>(ma.occupied_voxels), ma.entropy_bits,
              ma.wall_rms_cm, static_cast<unsigned long long>(ma.wall_cells));
  if (mb.occupied_voxels > 0) {
    std::printf("  occupancy  : %+.2f %% (fewer occupied voxels = the same surface painted "
                "in fewer places = crisper)\n",
                100.0 * (static_cast<double>(ma.occupied_voxels) -
                         static_cast<double>(mb.occupied_voxels)) /
                    static_cast<double>(mb.occupied_voxels));
  }
  if (mb.wall_cells > 0 && ma.wall_cells > 0 && mb.wall_rms_cm > 0.0) {
    std::printf("  wall RMS   : %+.2f %%\n",
                100.0 * (ma.wall_rms_cm - mb.wall_rms_cm) / mb.wall_rms_cm);
  }
  return kExitOk;
}

// --d6-loopend — ROUND 16 item 60. The same two-resolve A/B as --d6-loopclose,
// scored with the SAME ROUND 10 crispness metric at the SAME wall probes
// (chosen from the as-shipped pass), plus the ROUND 12 ruler on both sides —
// because the claim this command exists to test is "the map agrees with itself
// better afterwards", and that is what the ruler measures.
//
// Section stitching is ON for both legs. A capture that broke into sections has
// no single "end of the walk" until it is stitched, so measuring the loop end
// on an unstitched trajectory would be measuring the seams.
int cmd_d6_loopend(const std::string& lscan_dir, const post::LoopEndConfig& lcfg, int up_axis,
                   double window_s, double cell_m) {
  bool is_d6 = false;
  const Status probe = post::lscan_is_d6_project(lscan_dir, &is_d6);
  if (!probe.ok()) {
    std::fprintf(stderr, "d6-loopend: cannot read '%s': %s\n", lscan_dir.c_str(),
                 error_str(probe.error()));
    return kExitFailed;
  }
  if (!is_d6) {
    std::fprintf(stderr, "d6-loopend: '%s' is not a COIN-D6 project\n", lscan_dir.c_str());
    return kExitFailed;
  }

  std::printf("d6-loopend: %s\n", lscan_dir.c_str());
  std::printf("  gates: revisit <= %.2f m, >= %.0f s apart, >= %.1f m of path, excursion "
              ">= %.1f m AND >= %.1fx the gap, submap +/-%.1f s, |dt| <= %.2f m, "
              "observability >= %.3f\n",
              lcfg.max_revisit_m, lcfg.min_loop_seconds, lcfg.min_loop_path_m,
              lcfg.min_excursion_m, lcfg.min_excursion_over_gap, lcfg.submap_half_window_s,
              lcfg.max_close_translation_m, lcfg.min_translation_observability);

  std::vector<post::TrajPose> traj;
  std::vector<std::int64_t> ptimes;
  auto run = [&](bool close, std::vector<PointVertex>* pts,
                 post::D6ResolveStats* st) -> bool {
    PageStoreConfig psc;
    psc.page_capacity = 1u << 20;
    psc.max_pages = 4096;
    PageStore store(psc);
    post::D6ResolveConfig cfg;
    cfg.store = &store;
    cfg.stitch_sections = true;
    cfg.close_loop_end = close;
    cfg.loop_end = lcfg;
    traj.clear();
    ptimes.clear();
    cfg.out_trajectory = &traj;
    cfg.out_point_times = &ptimes;
    post::D6ResolvePipeline pipe(cfg);
    const Status s2 = pipe.run(lscan_dir);
    if (!s2.ok()) {
      std::fprintf(stderr, "d6-loopend: resolve failed: %s\n", error_str(s2.error()));
      return false;
    }
    *st = pipe.stats();
    pts->clear();
    pts->reserve(static_cast<std::size_t>(store.total_points()));
    for (const PageId id : store.page_ids()) {
      const PageView v = store.page_view(id);
      if (!v.valid()) continue;
      for (std::uint32_t k = 0; k < v.count; ++k) pts->push_back(v.data[k]);
    }
    return true;
  };

  std::vector<PointVertex> before, after;
  post::D6ResolveStats sb{}, sa{};
  if (!run(false, &before, &sb)) return kExitFailed;
  std::vector<std::int64_t> times_before = ptimes;

  post::MapConsistencyConfig mcfg;
  mcfg.window_seconds = window_s;
  mcfg.cell_m = cell_m;
  const post::MapConsistencyReport cb = post::measure_map_consistency(before, times_before, mcfg);
  const BandGrid ref_grid = build_band_grid(before, up_axis, 0.10);
  const std::vector<WallProbe> probes = choose_wall_probes(ref_grid, 400);
  const CrispnessMetrics mb = measure_crispness(before, up_axis, ref_grid, probes);
  std::printf("  stitched   : %zu sections, %zu points, %llu occupied 3 cm voxels, "
              "self-check %.2f cm (floor %.2f cm), end gap %.3f m\n",
              sb.sections.sections, before.size(),
              static_cast<unsigned long long>(mb.occupied_voxels), 100.0 * cb.nearest_offset_m,
              100.0 * cb.self_floor_m, sb.sections.trajectory_end_gap_after_m);

  if (!run(true, &after, &sa)) return kExitFailed;

  const post::LoopEndReport& r = sa.loop_end;
  std::printf("\n  DECISION: %s\n", post::to_string(r.decision));
  std::printf("  %s\n", r.reason);
  std::printf("  spatial candidates examined: %zu\n", r.candidates_seen);
  if (r.decision != post::LoopEndDecision::kNoRevisit &&
      r.decision != post::LoopEndDecision::kNoTrajectory) {
    std::printf("  revisit: t=%.1f s -> t=%.1f s  (%.1f s apart, %.1f m of path, gap %.2f m, "
                "excursion %.1f m)\n",
                static_cast<double>(r.t_a_ns) * 1e-9, static_cast<double>(r.t_b_ns) * 1e-9,
                r.loop_seconds, r.loop_path_m, r.revisit_gap_m, r.excursion_m);
    std::printf("  submaps: %zu / %zu points, %zu plane pairs, observability %.4f, "
                "%u iterations\n",
                r.submap_a_points, r.submap_b_points, r.pairs, r.observability, r.iterations);
    std::printf("  correction: %.4f m (%.4f, %.4f, %.4f), rotation %.4f deg\n",
                r.correction_translation_m, r.correction[0], r.correction[1], r.correction[2],
                r.correction_rotation_deg);
    std::printf("  same place, mean nearest-neighbour distance: %.2f cm -> %.2f cm "
                "(%zu pairs)\n",
                100.0 * r.submap_mismatch_before_m, 100.0 * r.submap_mismatch_after_m,
                r.mismatch_pairs);
    if (r.decision == post::LoopEndDecision::kClosed) {
      std::printf("  trajectory end gap: %.3f m -> %.3f m\n", r.end_gap_before_m,
                  r.end_gap_after_m);
    } else {
      std::printf("  trajectory end gap: %.3f m (unchanged — nothing was applied)\n",
                  r.end_gap_before_m);
    }
    if (r.self_check_checked) {
      std::printf("  ruler (gate 7): %.2f cm -> %.2f cm\n", 100.0 * r.self_check_before_m,
                  100.0 * r.self_check_after_m);
    }
    if (r.occupied_voxels_before > 0) {
      std::printf("  whole-map crispness gate: %llu -> %llu occupied 3 cm voxels (%+.2f %%, "
                  "overlap %.3f, %s)\n",
                  static_cast<unsigned long long>(r.occupied_voxels_before),
                  static_cast<unsigned long long>(r.occupied_voxels_after),
                  100.0 * (static_cast<double>(r.occupied_voxels_after) -
                           static_cast<double>(r.occupied_voxels_before)) /
                      static_cast<double>(r.occupied_voxels_before),
                  r.overlap_fraction, r.crispness_checked ? "voted" : "abstained");
    }
  }

  if (!sa.loop_end_applied) {
    std::printf("\n  NOTHING WAS MOVED. The cloud is byte-for-byte what stitching produces.\n");
    return kExitOk;
  }

  const post::MapConsistencyReport ca = post::measure_map_consistency(after, ptimes, mcfg);
  const CrispnessMetrics ma = measure_crispness(after, up_axis, ref_grid, probes);
  std::printf("\n  closed     : %zu points, %llu occupied 3 cm voxels, self-check %.2f cm "
              "(floor %.2f cm)\n",
              after.size(), static_cast<unsigned long long>(ma.occupied_voxels),
              100.0 * ca.nearest_offset_m, 100.0 * ca.self_floor_m);
  std::printf("  SELF-CHECK : %.2f cm -> %.2f cm (%+.2f cm)\n", 100.0 * cb.nearest_offset_m,
              100.0 * ca.nearest_offset_m,
              100.0 * (ca.nearest_offset_m - cb.nearest_offset_m));
  std::printf("  LOOP GAP   : %.3f m -> %.3f m\n", r.end_gap_before_m, r.end_gap_after_m);
  if (mb.occupied_voxels > 0) {
    std::printf("  occupancy  : %+.2f %% (fewer occupied voxels = the same surface painted in "
                "fewer places = crisper)\n",
                100.0 * (static_cast<double>(ma.occupied_voxels) -
                         static_cast<double>(mb.occupied_voxels)) /
                    static_cast<double>(mb.occupied_voxels));
  }
  if (mb.wall_cells > 0 && ma.wall_cells > 0 && mb.wall_rms_cm > 0.0) {
    std::printf("  wall RMS   : %.4f -> %.4f cm (%+.2f %%)\n", mb.wall_rms_cm, ma.wall_rms_cm,
                100.0 * (ma.wall_rms_cm - mb.wall_rms_cm) / mb.wall_rms_cm);
  }
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
  if (cmd == "--d6-timesweep") {
    if (argc < 3 || argv[2][0] == '-') return usage();
    const std::string dir = argv[2];
    double from_ms = -60.0, to_ms = 60.0, step_ms = 5.0;
    bool densify = true;
    int up_axis = kArCoreUpAxis;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--from" && i + 1 < argc) from_ms = std::atof(argv[++i]);
      else if (a == "--to" && i + 1 < argc) to_ms = std::atof(argv[++i]);
      else if (a == "--step" && i + 1 < argc) step_ms = std::atof(argv[++i]);
      else if (a == "--no-densify") densify = false;
      else if (a == "--probe-min-points" && i + 1 < argc)
        kProbeMinPoints = static_cast<std::size_t>(std::atoi(argv[++i]));
      else if (a == "--probe-elongation" && i + 1 < argc)
        kProbeMinElongation = std::atof(argv[++i]);
      else if (a == "--up" && i + 1 < argc) {
        const std::string ax = argv[++i];
        if (ax == "X" || ax == "x") up_axis = 0;
        else if (ax == "Y" || ax == "y") up_axis = 1;
        else if (ax == "Z" || ax == "z") up_axis = 2;
        else return usage();
      }
      else if (a == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_timesweep(dir, from_ms, to_ms, step_ms, densify, up_axis);
  }
  if (cmd == "--d6-selfcheck") {
    if (argc < 3) return usage();
    const std::string dir = argv[2];
    std::string mount_from;
    std::int64_t offset_ns = 0;
    bool densify = true;
    double window_s = 8.0, cell_m = 0.25;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--mount-from" && i + 1 < argc) mount_from = argv[++i];
      else if (a == "--offset-ms" && i + 1 < argc)
        offset_ns = static_cast<std::int64_t>(std::llround(std::atof(argv[++i]) * 1e6));
      else if (a == "--window" && i + 1 < argc) window_s = std::atof(argv[++i]);
      else if (a == "--cell" && i + 1 < argc) cell_m = std::atof(argv[++i]);
      else if (a == "--no-densify") densify = false;
      else if (a == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_selfcheck(dir, mount_from, offset_ns, densify, window_s, cell_m);
  }
  if (cmd == "--d6-stitch") {
    if (argc < 3) return usage();
    const std::string dir = argv[2];
    bool refine = true, densify = true;
    double window_s = 8.0, cell_m = 0.25, max_refine_m = 0.30;
    for (int i = 3; i < argc; ++i) {
      const std::string a2 = argv[i];
      if (a2 == "--no-refine") refine = false;
      else if (a2 == "--no-densify") densify = false;
      else if (a2 == "--window" && i + 1 < argc) window_s = std::atof(argv[++i]);
      else if (a2 == "--cell" && i + 1 < argc) cell_m = std::atof(argv[++i]);
      else if (a2 == "--max-refine" && i + 1 < argc) max_refine_m = std::atof(argv[++i]);
      else if (a2 == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_stitch(dir, refine, densify, window_s, cell_m, max_refine_m);
  }
  if (cmd == "--d6-mountcheck") {
    if (argc < 3) return usage();
    const std::string dir = argv[2];
    double window_s = 6.0;
    int up_axis = 1;
    bool densify = true;
    for (int i = 3; i < argc; ++i) {
      const std::string a2 = argv[i];
      if (a2 == "--window" && i + 1 < argc) window_s = std::atof(argv[++i]);
      else if (a2 == "--up" && i + 1 < argc) {
        const std::string u = argv[++i];
        up_axis = (u == "X" || u == "x") ? 0 : (u == "Z" || u == "z") ? 2 : 1;
      } else if (a2 == "--no-densify") densify = false;
      else if (a2 == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_mountcheck(dir, window_s, up_axis, densify);
  }
  if (cmd == "--d6-dump") {
    if (argc < 3) return usage();
    const std::string dir = argv[2];
    std::string out_prefix = dir + "-dump";
    std::string mount_from;
    std::int64_t offset_ns = 0;
    bool densify = true;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--out" && i + 1 < argc) out_prefix = argv[++i];
      else if (a == "--mount-from" && i + 1 < argc) mount_from = argv[++i];
      else if (a == "--offset-ms" && i + 1 < argc)
        offset_ns = static_cast<std::int64_t>(std::llround(std::atof(argv[++i]) * 1e6));
      else if (a == "--no-densify") densify = false;
      else if (a == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_dump(dir, out_prefix, mount_from, offset_ns, densify);
  }
  if (cmd == "--d6-loopclose") {
    if (argc < 3 || argv[2][0] == '-') return usage();
    const std::string dir = argv[2];
    post::TrajectoryLoopConfig lcfg;
    int up_axis = kArCoreUpAxis;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--radius" && i + 1 < argc) lcfg.max_revisit_m = std::atof(argv[++i]);
      else if (a == "--min-seconds" && i + 1 < argc) lcfg.min_loop_seconds = std::atof(argv[++i]);
      else if (a == "--min-path" && i + 1 < argc) lcfg.min_loop_path_m = std::atof(argv[++i]);
      else if (a == "--min-excursion" && i + 1 < argc) lcfg.min_excursion_m = std::atof(argv[++i]);
      else if (a == "--window" && i + 1 < argc) lcfg.submap_half_window_s = std::atof(argv[++i]);
      else if (a == "--up" && i + 1 < argc) {
        const std::string ax = argv[++i];
        if (ax == "X" || ax == "x") up_axis = 0;
        else if (ax == "Y" || ax == "y") up_axis = 1;
        else if (ax == "Z" || ax == "z") up_axis = 2;
        else return usage();
      }
      else if (a == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_loopclose(dir, lcfg, up_axis);
  }
  if (cmd == "--d6-reprocess") {
    // ROUND 16: the "Process this scan" pipeline, from the desktop, exactly as
    // the phone runs it. It existed only behind the C ABI and the JNI, so the
    // one command that produces `processed/map_stitched.bin`,
    // `processed/stitch.json` and (this round) `processed/trajectory.bin` could
    // not be run over a real container from a terminal — which is how the
    // floor-plan path overlay came to be written before anything had ever
    // written a trajectory file for it to read.
    if (argc < 3 || argv[2][0] == '-') return usage();
    const std::string dir = argv[2];
    post::ReprocessOptions ro;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--no-refine") ro.sections.refine = false;
      else if (a == "--no-densify") ro.densify_with_phone_imu = false;
      else if (a == "--no-loopend") ro.close_loop_end = false;
      else if (a == "--quiet") continue;
      else return usage();
    }
    post::ReprocessReport rep;
    const Status st = post::reprocess_d6_container(dir, ro, &rep);
    if (!st.ok()) {
      std::fprintf(stderr, "d6-reprocess: %s (%s)\n", error_str(st.error()),
                   last_error_message());
      return kExitFailed;
    }
    std::printf("d6-reprocess: %s\n", dir.c_str());
    std::printf("  %llu points, %llu poses, %zu sections, map=%s trajectory=%s\n",
                static_cast<unsigned long long>(rep.points),
                static_cast<unsigned long long>(rep.poses), rep.stitch.sections,
                rep.map_written ? "written" : "none",
                rep.trajectory_written ? "written" : "none");
    std::printf("  self-check %.2f cm (measurable=%s), loop end %s\n",
                100.0 * rep.consistency.nearest_offset_m,
                rep.consistency.measurable ? "yes" : "no",
                post::to_string(rep.loop_end.decision));
    return kExitOk;
  }
  if (cmd == "--d6-loopend") {
    if (argc < 3 || argv[2][0] == '-') return usage();
    const std::string dir = argv[2];
    post::LoopEndConfig lcfg;
    int up_axis = kArCoreUpAxis;
    double window_s = 8.0, cell_m = 0.25;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--radius" && i + 1 < argc) lcfg.max_revisit_m = std::atof(argv[++i]);
      else if (a == "--min-seconds" && i + 1 < argc) lcfg.min_loop_seconds = std::atof(argv[++i]);
      else if (a == "--min-path" && i + 1 < argc) lcfg.min_loop_path_m = std::atof(argv[++i]);
      else if (a == "--min-excursion" && i + 1 < argc) lcfg.min_excursion_m = std::atof(argv[++i]);
      else if (a == "--max-correction" && i + 1 < argc)
        lcfg.max_close_translation_m = std::atof(argv[++i]);
      else if (a == "--submap" && i + 1 < argc) lcfg.submap_half_window_s = std::atof(argv[++i]);
      else if (a == "--window" && i + 1 < argc) window_s = std::atof(argv[++i]);
      else if (a == "--cell" && i + 1 < argc) cell_m = std::atof(argv[++i]);
      else if (a == "--up" && i + 1 < argc) {
        const std::string ax = argv[++i];
        if (ax == "X" || ax == "x") up_axis = 0;
        else if (ax == "Y" || ax == "y") up_axis = 1;
        else if (ax == "Z" || ax == "z") up_axis = 2;
        else return usage();
      }
      else if (a == "--quiet") continue;
      else return usage();
    }
    return cmd_d6_loopend(dir, lcfg, up_axis, window_s, cell_m);
  }
  if (cmd == "--d6-plan") {
    if (argc < 3) return usage();
    const std::string dir = argv[2];
    post::LscanPlanOptions o;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--slice" && i + 2 < argc) {
        o.slice_min_m = std::atof(argv[i + 1]);
        o.slice_max_m = std::atof(argv[i + 2]);
        i += 2;
      } else if (a == "--res" && i + 1 < argc) {
        o.grid_res_m = std::atof(argv[++i]);
      } else if (a == "--min-cell" && i + 1 < argc) {
        o.min_cell_points = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        o.adapt_density = false;
      } else if (a == "--no-adapt") {
        o.adapt_density = false;
      } else if (a == "--out" && i + 1 < argc) {
        o.out_dir = argv[++i];
      } else if (a == "--title" && i + 1 < argc) {
        o.title = argv[++i];
      } else if (a == "--png-px" && i + 1 < argc) {
        o.png_max_px = static_cast<std::uint32_t>(std::atoi(argv[++i]));
      } else if (a == "--up" && i + 1 < argc) {
        const std::string u = argv[++i];
        o.up = u == "x" ? plan::UpAxis::kX : (u == "z" ? plan::UpAxis::kZ : plan::UpAxis::kY);
      } else if (a == "--quiet") {
        continue;
      }
    }
    return cmd_d6_plan(dir, o);
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
