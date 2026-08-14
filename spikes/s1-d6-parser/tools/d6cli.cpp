// d6cli -- COIN-D6 bring-up tool for macOS (also builds on Linux).
//
// Opens a CH340 USB-serial port at 230400 8N1, sends the start command, decodes
// the stream with the portable d6::Parser, prints a once-a-second stats line and
// an ASCII plot, and sends the stop command on Ctrl-C.
//
//   d6cli                                  auto-detect the port, live capture
//   d6cli --port /dev/tty.usbserial-1140
//   d6cli --capture raw.bin                record raw bytes while decoding
//   d6cli --replay  raw.bin                decode a previously captured file
//   d6cli --seconds 10                     stop after 10 s (bench runs)
//   d6cli --plot polar|bars|none
//   d6cli --checksum vendor|spec
//   d6cli --list                           list candidate serial devices
//
// Only this file contains platform code; d6/ stays portable.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "commands.h"
#include "d6_parser.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

// --- serial ---------------------------------------------------------------

std::vector<std::string> list_serial_ports() {
  std::vector<std::string> out;
  DIR* d = ::opendir("/dev");
  if (!d) return out;
  while (struct dirent* e = ::readdir(d)) {
    const std::string n(e->d_name);
    // macOS: cu.usbserial-*, tty.usbserial-*, cu.wchusbserial*
    // Linux : ttyUSB*
    if (n.rfind("cu.usbserial", 0) == 0 || n.rfind("tty.usbserial", 0) == 0 ||
        n.rfind("cu.wchusbserial", 0) == 0 || n.rfind("tty.wchusbserial", 0) == 0 ||
        n.rfind("cu.SLAB_USBtoUART", 0) == 0 || n.rfind("ttyUSB", 0) == 0) {
      out.push_back("/dev/" + n);
    }
  }
  ::closedir(d);
  // Prefer the callout (cu.*) devices on macOS: they do not block on DCD.
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    const bool acu = a.find("/cu.") != std::string::npos;
    const bool bcu = b.find("/cu.") != std::string::npos;
    if (acu != bcu) return acu;
    return a < b;
  });
  return out;
}

int open_serial(const std::string& path, int baud) {
  const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    std::fprintf(stderr, "open(%s): %s\n", path.c_str(), std::strerror(errno));
    return -1;
  }
  struct termios tio {};
  if (::tcgetattr(fd, &tio) != 0) {
    std::fprintf(stderr, "tcgetattr: %s\n", std::strerror(errno));
    ::close(fd);
    return -1;
  }
  ::cfmakeraw(&tio);
  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);    // 1 stop bit
  tio.c_cflag &= ~static_cast<tcflag_t>(PARENB);    // no parity
  tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
  tio.c_cflag |= CS8;                               // 8 data bits
  tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);   // no flow control
  tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (::cfsetispeed(&tio, static_cast<speed_t>(baud)) != 0 ||
      ::cfsetospeed(&tio, static_cast<speed_t>(baud)) != 0) {
    std::fprintf(stderr, "cfsetspeed(%d): %s\n", baud, std::strerror(errno));
    ::close(fd);
    return -1;
  }
  if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
    std::fprintf(stderr, "tcsetattr: %s\n", std::strerror(errno));
    ::close(fd);
    return -1;
  }
  ::tcflush(fd, TCIOFLUSH);
  // The vendor SDK clears DTR after opening (some CH340 boards wire DTR to a
  // reset line). Mirror that.
  int bits = TIOCM_DTR;
  ::ioctl(fd, TIOCMBIC, &bits);
  return fd;
}

bool write_all(int fd, const uint8_t* p, size_t n) {
  size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

// --- plots ----------------------------------------------------------------

constexpr int kSectors = 72;   // one per 5 degrees
constexpr int kRows    = 24;

struct SectorMap {
  float nearest_mm[kSectors];
  SectorMap() { clear(); }
  void clear() {
    for (int i = 0; i < kSectors; ++i) nearest_mm[i] = 0.f;
  }
  void add(float angle_deg, uint16_t d) {
    if (d == 0) return;
    int s = static_cast<int>(angle_deg / (360.f / kSectors));
    if (s < 0) s = 0;
    if (s >= kSectors) s = kSectors - 1;
    if (nearest_mm[s] == 0.f || static_cast<float>(d) < nearest_mm[s])
      nearest_mm[s] = static_cast<float>(d);
  }
};

// 72x24 top-down polar view: one glyph per 5-degree sector placed at its
// nearest return, scaled so `max_mm` reaches the edge.
void draw_polar(const SectorMap& m, float max_mm) {
  std::vector<std::string> canvas(kRows, std::string(kSectors, ' '));
  const float cx = (kSectors - 1) / 2.f;
  const float cy = (kRows - 1) / 2.f;
  canvas[static_cast<size_t>(cy)][static_cast<size_t>(cx)] = '+';
  for (int s = 0; s < kSectors; ++s) {
    const float d = m.nearest_mm[s];
    if (d <= 0.f) continue;
    const float r = std::fmin(d / max_mm, 1.f);
    const float a = (static_cast<float>(s) + 0.5f) * (360.f / kSectors) *
                    3.14159265f / 180.f;
    // 0 deg up, angle increasing clockwise (datasheet: CW rotation).
    const int x = static_cast<int>(std::lround(cx + std::sin(a) * r * cx));
    const int y = static_cast<int>(std::lround(cy - std::cos(a) * r * cy));
    if (x < 0 || x >= kSectors || y < 0 || y >= kRows) continue;
    const char glyph = d < max_mm * 0.33f ? '#' : (d < max_mm * 0.66f ? 'o' : '.');
    canvas[static_cast<size_t>(y)][static_cast<size_t>(x)] = glyph;
  }
  std::printf("+%s+\n", std::string(kSectors, '-').c_str());
  for (const auto& row : canvas) std::printf("|%s|\n", row.c_str());
  std::printf("+%s+  top-down, up = 0 deg, edge = %.1f m ('#'<1/3 'o'<2/3 '.')\n",
              std::string(kSectors, '-').c_str(), max_mm / 1000.f);
}

// 72x24 bar chart: column = 5-degree sector, bar height = nearest distance.
void draw_bars(const SectorMap& m, float max_mm) {
  std::vector<std::string> canvas(kRows, std::string(kSectors, ' '));
  for (int s = 0; s < kSectors; ++s) {
    const float d = m.nearest_mm[s];
    if (d <= 0.f) continue;
    int h = static_cast<int>(std::lround(std::fmin(d / max_mm, 1.f) * kRows));
    if (h < 1) h = 1;
    for (int r = 0; r < h; ++r)
      canvas[static_cast<size_t>(kRows - 1 - r)][static_cast<size_t>(s)] =
          (r == h - 1) ? '#' : '|';
  }
  std::printf("+%s+ %.1f m\n", std::string(kSectors, '-').c_str(), max_mm / 1000.f);
  for (const auto& row : canvas) std::printf("|%s|\n", row.c_str());
  std::printf("+%s+ 0 m\n", std::string(kSectors, '-').c_str());
  std::printf(" 0deg%sangle%s355deg\n", std::string(28, ' ').c_str(),
              std::string(28, ' ').c_str());
}

// --- options ---------------------------------------------------------------

struct Options {
  std::string port;
  std::string replay;
  std::string capture;
  std::string plot = "polar";
  double seconds = 0.0;          // 0 = until Ctrl-C
  double replay_duration = 0.0;  // real seconds the capture covers (0 = wire time)
  float max_range_mm = 6000.f;
  bool list_only = false;
  bool quiet = false;
  bool no_start_cmd = false;
  d6::ChecksumVariant checksum = d6::ChecksumVariant::kVendorSdk;
};

void usage() {
  std::printf(
      "usage: d6cli [--port DEV] [--replay FILE] [--capture FILE]\n"
      "             [--seconds N] [--replay-duration S] [--plot polar|bars|none]\n"
      "             [--max-range M] [--checksum vendor|spec] [--no-start]\n"
      "             [--list] [--quiet]\n"
      "\n"
      "A raw capture carries no timestamps, so --replay derives its clock from\n"
      "the wire rate (23040 B/s at 230400 8N1). The device idles between\n"
      "packets, so replayed pts/s and rotation Hz run fast; pass\n"
      "--replay-duration with the real capture length for true rates.\n");
}

bool parse_args(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--port") o->port = next("--port");
    else if (a == "--replay") o->replay = next("--replay");
    else if (a == "--capture") o->capture = next("--capture");
    else if (a == "--plot") o->plot = next("--plot");
    else if (a == "--seconds") o->seconds = std::atof(next("--seconds").c_str());
    else if (a == "--replay-duration")
      o->replay_duration = std::atof(next("--replay-duration").c_str());
    else if (a == "--max-range") o->max_range_mm = std::atof(next("--max-range").c_str()) * 1000.f;
    else if (a == "--checksum") {
      const std::string v = next("--checksum");
      o->checksum = (v == "spec") ? d6::ChecksumVariant::kSpecLiteral
                                  : d6::ChecksumVariant::kVendorSdk;
    }
    else if (a == "--no-start") o->no_start_cmd = true;
    else if (a == "--list") o->list_only = true;
    else if (a == "--quiet") o->quiet = true;
    else if (a == "-h" || a == "--help") { usage(); std::exit(0); }
    else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return false; }
  }
  return true;
}

void print_summary(const d6::Parser& parser, double elapsed_s) {
  const d6::Stats& s = parser.stats();
  std::printf("\n---- summary (%.1f s) ----\n", elapsed_s);
  std::printf("bytes in            : %llu (%.0f B/s)\n",
              (unsigned long long)s.bytes_in,
              elapsed_s > 0 ? s.bytes_in / elapsed_s : 0.0);
  std::printf("packets ok          : %llu\n", (unsigned long long)s.packets_ok);
  std::printf("packets bad cksum   : %llu\n", (unsigned long long)s.packets_bad_checksum);
  std::printf("packets malformed   : %llu\n", (unsigned long long)s.packets_malformed);
  std::printf("checksum pass rate  : %.4f %%   <-- S1 exit criterion: > 99.5 %%\n",
              s.checksum_pass_rate() * 100.0);
  std::printf("  accepted by vendor: %llu\n", (unsigned long long)s.cs_ok_vendor);
  std::printf("  accepted by spec  : %llu\n", (unsigned long long)s.cs_ok_spec);
  std::printf("resyncs             : %llu\n", (unsigned long long)s.resyncs);
  std::printf("garbage bytes       : %llu\n", (unsigned long long)s.bytes_discarded);
  std::printf("speed-adjust bytes  : %llu\n", (unsigned long long)s.speed_adjust_bytes);
  std::printf("start packets       : %llu (scan freq field = %u)\n",
              (unsigned long long)s.start_packets, (unsigned)s.scan_freq_raw);
  std::printf("rotations           : %llu (%.2f Hz avg)\n",
              (unsigned long long)s.rotations,
              elapsed_s > 0 ? s.rotations / elapsed_s : 0.0);
  std::printf("points              : %llu (%.0f pts/s avg, %llu zero-range)\n",
              (unsigned long long)s.points,
              elapsed_s > 0 ? s.points / elapsed_s : 0.0,
              (unsigned long long)s.points_zero_range);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, &opt)) return 2;

  if (opt.list_only) {
    const auto ports = list_serial_ports();
    if (ports.empty()) {
      std::printf("no candidate serial devices found in /dev\n");
    } else {
      for (const auto& p : ports) std::printf("%s\n", p.c_str());
    }
    return 0;
  }

  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);

  d6::Config cfg;
  cfg.checksum = opt.checksum;
  d6::Parser parser(cfg);

  SectorMap live_map, frame_map;
  uint64_t rotations_drawn = 0;
  parser.set_point_callback([&](const d6::Point& pt) {
    if (pt.new_rotation) {
      live_map = frame_map;
      frame_map.clear();
    }
    frame_map.add(pt.angle_deg, pt.distance_mm);
  });

  FILE* cap = nullptr;
  if (!opt.capture.empty()) {
    cap = std::fopen(opt.capture.c_str(), "wb");
    if (!cap) {
      std::fprintf(stderr, "cannot write %s: %s\n", opt.capture.c_str(),
                   std::strerror(errno));
      return 1;
    }
  }

  const uint64_t t0 = d6::now_ns();
  uint64_t next_report = 0;  // armed on the first byte received

  auto tick = [&](uint64_t now) {
    if (opt.quiet) return;
    if (next_report == 0) next_report = now + 1000000000ull;
    if (now < next_report) return;
    next_report = now + 1000000000ull;
    if (opt.plot == "polar" || opt.plot == "bars") {
      std::printf("\033[H\033[2J");  // clear screen
      if (opt.plot == "polar") draw_polar(live_map, opt.max_range_mm);
      else draw_bars(live_map, opt.max_range_mm);
    }
    std::printf("%s\n", parser.stats_line().c_str());
    std::fflush(stdout);
    rotations_drawn = parser.stats().rotations;
    (void)rotations_drawn;
  };

  // ---------------- replay mode ----------------
  if (!opt.replay.empty()) {
    FILE* f = std::fopen(opt.replay.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "cannot read %s: %s\n", opt.replay.c_str(),
                   std::strerror(errno));
      return 1;
    }
    std::vector<uint8_t> buf(4096);
    // 230400 8N1 -> 23040 byte/s; synthesise arrival times so the rate stats
    // and rotation Hz mean the same thing as in a live run.
    uint64_t vt = t0;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0 && !g_stop) {
      vt += static_cast<uint64_t>(n) * 1000000000ull / 23040ull;
      parser.feed(buf.data(), n, vt);
      if (cap) std::fwrite(buf.data(), 1, n, cap);
      tick(vt);
    }
    std::fclose(f);
    if (cap) std::fclose(cap);
    const double wire_s = static_cast<double>(vt - t0) / 1e9;
    const double elapsed = opt.replay_duration > 0 ? opt.replay_duration : wire_s;
    if (opt.replay_duration <= 0)
      std::printf(
          "\n(replay clock = wire time %.2f s at 23040 B/s; the device idles\n"
          " between packets, so rates below read high -- use --replay-duration)\n",
          wire_s);
    print_summary(parser, elapsed);
    return parser.stats().packets_ok > 0 ? 0 : 1;
  }

  // ---------------- live mode ----------------
  std::string port = opt.port;
  if (port.empty()) {
    const auto ports = list_serial_ports();
    if (ports.empty()) {
      std::fprintf(stderr,
                   "no serial device found. Plug the CH340 adapter in, or pass "
                   "--port. (--list shows candidates, --replay FILE works "
                   "without hardware.)\n");
      if (cap) std::fclose(cap);
      return 1;
    }
    port = ports.front();
    std::printf("auto-selected %s\n", port.c_str());
  }

  const int fd = open_serial(port, 230400);
  if (fd < 0) {
    if (cap) std::fclose(cap);
    return 1;
  }
  std::printf("opened %s @ 230400 8N1\n", port.c_str());

  if (!opt.no_start_cmd) {
    if (!write_all(fd, d6::kCmdStart, sizeof(d6::kCmdStart))) {
      std::fprintf(stderr, "failed to send start command: %s\n", std::strerror(errno));
      ::close(fd);
      return 1;
    }
    std::printf("sent start: AA 55 F0 0F\n");
  }

  // Watch the first 200 ms of the stream for an ACK / info frame before the
  // point cloud takes over.
  std::vector<uint8_t> ack_window;
  bool ack_reported = false;

  std::vector<uint8_t> buf(4096);
  uint64_t last_data_ns = d6::now_ns();
  while (!g_stop) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    const uint64_t now = d6::now_ns();
    if (n > 0) {
      last_data_ns = now;
      parser.feed(buf.data(), static_cast<size_t>(n), now);
      if (cap) std::fwrite(buf.data(), 1, static_cast<size_t>(n), cap);
      if (!ack_reported) {
        ack_window.insert(ack_window.end(), buf.begin(), buf.begin() + n);
        size_t off = 0;
        const d6::Ack a = d6::find_ack(ack_window.data(), ack_window.size(), &off);
        if (a != d6::Ack::kNone) {
          std::printf("ACK: %s (at offset %zu)\n", d6::to_string(a), off);
          ack_reported = true;
        } else if (ack_window.size() > 8192) {
          std::printf("ACK: none seen in the first %zu bytes\n", ack_window.size());
          ack_reported = true;
        }
        d6::DeviceInfo info{};
        for (size_t i = 0; !ack_reported && i + d6::kInfoFrameLen <= ack_window.size(); ++i) {
          if (d6::parse_device_info(&ack_window[i], ack_window.size() - i, &info)) {
            std::printf("device info: model=%s dir=%u ver=%u\n", info.model,
                        info.direction, info.version);
            break;
          }
        }
      }
    } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
      std::fprintf(stderr, "read: %s\n", std::strerror(errno));
      break;
    } else {
      ::usleep(2000);
      if (now - last_data_ns > 3000000000ull) {
        std::fprintf(stderr, "no data for 3 s -- wrong port, wrong baud, or the "
                             "device never started\n");
        last_data_ns = now;
      }
    }
    tick(now);
    if (opt.seconds > 0 &&
        static_cast<double>(now - t0) / 1e9 >= opt.seconds) break;
  }

  // Ctrl-C (or the timer) -> stop the motor before leaving.
  if (write_all(fd, d6::kCmdStop, sizeof(d6::kCmdStop))) {
    std::printf("\nsent stop: AA 55 F5 0A\n");
    // Drain briefly and look for the stop ACK.
    std::vector<uint8_t> tail;
    for (int i = 0; i < 100; ++i) {
      const ssize_t n = ::read(fd, buf.data(), buf.size());
      if (n > 0) tail.insert(tail.end(), buf.begin(), buf.begin() + n);
      ::usleep(5000);
    }
    size_t off = 0;
    const d6::Ack a = d6::find_ack(tail.data(), tail.size(), &off);
    std::printf("stop ACK: %s\n", d6::to_string(a));
  }
  ::tcflush(fd, TCIOFLUSH);
  ::close(fd);
  if (cap) std::fclose(cap);

  print_summary(parser, static_cast<double>(d6::now_ns() - t0) / 1e9);
  return 0;
}
