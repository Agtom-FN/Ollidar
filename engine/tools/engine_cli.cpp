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
#include <string>
#include <thread>
#include <vector>

#include "packet_builder.h"
#include "scanengine/core/engine.h"
#include "scanengine/drivers/mid360/mid360_packets.h"
#include "scanengine/jobs/job_runner_adapter.h"
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
      "  engine_cli --synth-lscan <dir> [seconds]\n"
      "  engine_cli --post-selftest [--quiet]\n"
      "  engine_cli --version\n"
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

int cmd_synth_lscan(const char* dir, double seconds) {
  if (!write_synth_lscan(dir, seconds)) return kExitFailed;
  std::printf("wrote a %.1f s synthetic Mid-360 .lscan to %s\n", seconds, dir);
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
};

int cmd_post(const std::string& lscan_dir, const std::string& out_dir, const PostOptions& po,
             bool quiet) {
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
  jobs::QueueJobRunner runner(&e.jobs(), opts);

  JobRequest req;
  req.mode = JobMode::kLocal;
  req.lscan_dir = lscan_dir;
  req.output_dir = out_dir;
  // With an --out, the request is "post then export"; without one it is
  // "post" and the result stays in the process's PageStore, which is what a
  // caller wanting only a validity check (or a chained in-process step)
  // wants.
  req.pipeline = out_dir.empty() ? "post" : "export";

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
  if (write_synth_lscan(lscan, 2.2)) {
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
  } else {
    std::fprintf(stderr, "post-selftest: could not write the synthetic .lscan\n");
  }

  std::filesystem::remove_all(root, ec);
  std::printf("post-selftest: %s\n", rc == kExitOk ? "PASS" : "FAIL");
  return rc;
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
    for (int i = 3; i < argc; ++i) {
      if (std::strcmp(argv[i], "--no-loops") == 0) po.detect_loops = false;
      if (std::strcmp(argv[i], "--no-outlier") == 0) po.outlier_filter = false;
      if (i + 1 >= argc) continue;
      if (std::strcmp(argv[i], "--out") == 0) out_dir = argv[i + 1];
      if (std::strcmp(argv[i], "--dedup") == 0) po.dedup_voxel_m = std::atof(argv[i + 1]);
    }
    return cmd_post(argv[2], out_dir, po, quiet);
  }
  if (cmd == "--synth-lscan") {
    if (argc < 3 || argv[2][0] == '-') return usage();
    const double seconds = (argc > 3 && argv[3][0] != '-') ? std::atof(argv[3]) : 2.2;
    return cmd_synth_lscan(argv[2], seconds);
  }
  if (cmd == "--post-selftest") return cmd_post_selftest(quiet);
  return usage();
}
