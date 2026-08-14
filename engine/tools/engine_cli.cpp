// engine_cli — the engine's headless front end.
//
// Three jobs today:
//   --selftest              build an engine, run a synthetic D6 capture
//                           through it, assert the whole path works. This is
//                           what CI (and a developer on a new machine) runs
//                           to answer "is the engine alive?".
//   --synth <out.bin> [s]   write a synthetic COIN-D6 capture (the S1
//                           d6synth room: 4x3 m with a reflective post).
//   --replay <file>         push a capture through the real engine and print
//                           decoded statistics.
//
// It grows into workstream D1's containerized worker CLI (headless Linux
// build running the post pipeline on an uploaded .lscan), which is why it
// links the engine exactly the way an app does — no privileged access, no
// test-only hooks.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "packet_builder.h"
#include "scanengine/core/engine.h"

using namespace scanengine;

namespace {

constexpr double kPi = 3.14159265358979323846;

int usage() {
  std::printf(
      "engine_cli — LidarScan engine command line\n"
      "\n"
      "  engine_cli --selftest [--quiet]\n"
      "  engine_cli --synth <out.bin> [seconds] [--noise]\n"
      "  engine_cli --replay <capture.bin> [--chunk N]\n"
      "  engine_cli --version\n");
  return 2;
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
  return usage();
}
