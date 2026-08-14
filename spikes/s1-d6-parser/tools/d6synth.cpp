// d6synth -- write a synthetic COIN-D6 raw byte stream, so the replay path and
// the CLI plots can be exercised (and regression-checked) without hardware.
//
//   d6synth out.bin [seconds] [--noise]
//
// Produces 10 Hz revolutions of 400 points (4,000 pts/s, the datasheet rate) in
// 10 packets of 40 samples plus a start packet, describing a 4 m x 3 m room with
// a reflective post. --noise sprinkles garbage and speed-adjustment bytes
// between revolutions to exercise the resync path.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "packet_builder.h"

namespace {

// Ray-cast a rectangular room with a small reflective post.
uint16_t range_at(double deg) {
  const double a = deg * M_PI / 180.0;
  const double dx = std::sin(a), dy = std::cos(a);   // 0 deg = +y
  const double hx = 2.0, hy = 1.5;                   // 4 m x 3 m room, centred
  double t = 1e9;
  if (std::fabs(dx) > 1e-9) {
    t = std::fmin(t, std::fmax((hx) / dx, (-hx) / dx));
  }
  if (std::fabs(dy) > 1e-9) {
    t = std::fmin(t, std::fmax((hy) / dy, (-hy) / dy));
  }
  // Post at (0.8, 0.6), radius 0.15
  const double px = 0.8, py = 0.6, pr = 0.15;
  const double b = dx * px + dy * py;
  const double c = px * px + py * py - pr * pr;
  const double disc = b * b - c;
  if (disc > 0) {
    const double th = b - std::sqrt(disc);
    if (th > 0.05 && th < t) t = th;
  }
  return static_cast<uint16_t>(std::lround(t * 1000.0));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: d6synth out.bin [seconds] [--noise]\n");
    return 2;
  }
  const char* path = argv[1];
  double seconds = (argc > 2 && argv[2][0] != '-') ? std::atof(argv[2]) : 2.0;
  bool noise = false;
  for (int i = 2; i < argc; ++i)
    if (std::strcmp(argv[i], "--noise") == 0) noise = true;

  const int revolutions = static_cast<int>(seconds * 10.0);
  const int packets = 10, per_packet = 40;
  const double step = 360.0 / (packets * per_packet);

  std::vector<uint8_t> out;
  unsigned rng = 12345;
  auto rand8 = [&]() {
    rng = rng * 1103515245u + 12345u;
    return static_cast<uint8_t>((rng >> 16) & 0xFF);
  };

  for (int r = 0; r < revolutions; ++r) {
    if (noise && (r % 7) == 3) {
      const uint8_t sp[4] = {0xFE, 0xFE, 0xFF, 0xFF};
      out.insert(out.end(), sp, sp + 4);
      for (int i = 0; i < 5; ++i) out.push_back(rand8());
    }
    {
      d6test::PacketSpec s;
      s.start_packet = true;
      s.scan_freq = 10;
      s.first_angle_deg = 0.0;
      s.last_angle_deg = 0.0;
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
        const uint16_t d = range_at(a);
        const bool hi = (a > 100.0 && a < 104.0);
        const uint8_t inten = static_cast<uint8_t>(
            hi ? 255 : std::fmin(250.0, 40000.0 / static_cast<double>(d ? d : 1)));
        s.samples.push_back(d6test::Sample{d, inten, hi});
      }
      d6test::append(&out, d6test::build(s));
    }
  }

  FILE* f = std::fopen(path, "wb");
  if (!f) { std::perror("fopen"); return 1; }
  std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  std::printf("wrote %zu bytes (%d revolutions%s) to %s\n", out.size(),
              revolutions, noise ? ", with noise" : "", path);
  return 0;
}
