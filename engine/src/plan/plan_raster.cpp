// plan_raster.cpp — ROUND 15. See plan/plan_raster.h.
#include "scanengine/plan/plan_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "plan_internal.h"

namespace scanengine {
namespace plan {
namespace {

// --- PNG plumbing -----------------------------------------------------------

// Table-free CRC-32 (the PNG polynomial). Table-free because a static table
// would be either a mutable global initialized on first use (a data race the
// rest of this library does not have) or 1 KB of literals; the plan PNG is
// written once per export and the loop costs microseconds.
std::uint32_t crc32_of(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0xFFFFFFFFu) {
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

std::uint32_t adler32_of(const std::uint8_t* data, std::size_t n) {
  std::uint32_t a = 1, b = 0;
  for (std::size_t i = 0; i < n; ++i) {
    a = (a + data[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

void put_be32(std::string* s, std::uint32_t v) {
  s->push_back(static_cast<char>((v >> 24) & 0xFF));
  s->push_back(static_cast<char>((v >> 16) & 0xFF));
  s->push_back(static_cast<char>((v >> 8) & 0xFF));
  s->push_back(static_cast<char>(v & 0xFF));
}

void put_chunk(std::string* s, const char type[4], const std::string& payload) {
  put_be32(s, static_cast<std::uint32_t>(payload.size()));
  const std::size_t start = s->size();
  s->append(type, 4);
  s->append(payload);
  const std::uint32_t crc =
      crc32_of(reinterpret_cast<const std::uint8_t*>(s->data() + start), s->size() - start) ^
      0xFFFFFFFFu;
  put_be32(s, crc);
}

// zlib stream whose deflate data is a chain of STORED blocks. Legal deflate
// (RFC 1951 §3.2.4), and every PNG reader in existence handles it because it
// is what a compressor emits for incompressible data.
std::string zlib_stored(const std::vector<std::uint8_t>& raw) {
  std::string z;
  z.push_back(static_cast<char>(0x78));  // CM=8, CINFO=7 (32 KB window)
  z.push_back(static_cast<char>(0x01));  // FCHECK so (0x78<<8|0x01) % 31 == 0
  std::size_t off = 0;
  const std::size_t kMax = 65535;
  if (raw.empty()) {
    z.push_back(static_cast<char>(0x01));
    z.push_back(0);
    z.push_back(0);
    z.push_back(static_cast<char>(0xFF));
    z.push_back(static_cast<char>(0xFF));
  }
  while (off < raw.size()) {
    const std::size_t n = std::min(kMax, raw.size() - off);
    const bool last = (off + n) >= raw.size();
    z.push_back(static_cast<char>(last ? 1 : 0));
    z.push_back(static_cast<char>(n & 0xFF));
    z.push_back(static_cast<char>((n >> 8) & 0xFF));
    const std::uint16_t nlen = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(n));
    z.push_back(static_cast<char>(nlen & 0xFF));
    z.push_back(static_cast<char>((nlen >> 8) & 0xFF));
    z.append(reinterpret_cast<const char*>(raw.data() + off), n);
    off += n;
  }
  const std::uint32_t ad = adler32_of(raw.data(), raw.size());
  z.push_back(static_cast<char>((ad >> 24) & 0xFF));
  z.push_back(static_cast<char>((ad >> 16) & 0xFF));
  z.push_back(static_cast<char>((ad >> 8) & 0xFF));
  z.push_back(static_cast<char>(ad & 0xFF));
  return z;
}

// --- a canvas ---------------------------------------------------------------

struct Rgb {
  std::uint8_t r = 0, g = 0, b = 0;
};

class Canvas {
 public:
  Canvas(std::uint32_t w, std::uint32_t h, Rgb bg) : w_(w), h_(h), px_(std::size_t(w) * h * 3) {
    for (std::size_t i = 0; i < px_.size(); i += 3) {
      px_[i] = bg.r;
      px_[i + 1] = bg.g;
      px_[i + 2] = bg.b;
    }
  }

  std::uint32_t width() const { return w_; }
  std::uint32_t height() const { return h_; }
  const std::uint8_t* data() const { return px_.data(); }

  void set(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(w_) || y >= static_cast<int>(h_)) return;
    const std::size_t i = (static_cast<std::size_t>(y) * w_ + static_cast<std::size_t>(x)) * 3;
    px_[i] = c.r;
    px_[i + 1] = c.g;
    px_[i + 2] = c.b;
  }

  // Integer alpha in [0,255]. Deterministic: fixed-point, no floats.
  void blend(int x, int y, Rgb c, std::uint32_t a) {
    if (a == 0) return;
    if (x < 0 || y < 0 || x >= static_cast<int>(w_) || y >= static_cast<int>(h_)) return;
    if (a >= 255) {
      set(x, y, c);
      return;
    }
    const std::size_t i = (static_cast<std::size_t>(y) * w_ + static_cast<std::size_t>(x)) * 3;
    const std::uint32_t ia = 255u - a;
    px_[i] = static_cast<std::uint8_t>((c.r * a + px_[i] * ia) / 255u);
    px_[i + 1] = static_cast<std::uint8_t>((c.g * a + px_[i + 1] * ia) / 255u);
    px_[i + 2] = static_cast<std::uint8_t>((c.b * a + px_[i + 2] * ia) / 255u);
  }

  void fill_rect(int x0, int y0, int x1, int y1, Rgb c) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) set(x, y, c);
    }
  }

  // A disc-capped thick line. Radius rather than a stroke width because that
  // is what makes the joins between wall segments close without a separate
  // join rule.
  void line(double x0, double y0, double x1, double y1, double radius, Rgb c) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    const int steps = static_cast<int>(std::floor(len)) + 1;
    for (int s = 0; s <= steps; ++s) {
      const double u = steps == 0 ? 0.0 : static_cast<double>(s) / steps;
      disc(x0 + dx * u, y0 + dy * u, radius, c);
    }
  }

  void disc(double cx, double cy, double radius, Rgb c) {
    const int r = static_cast<int>(std::ceil(radius));
    const int ix = static_cast<int>(std::floor(cx));
    const int iy = static_cast<int>(std::floor(cy));
    for (int y = iy - r; y <= iy + r; ++y) {
      for (int x = ix - r; x <= ix + r; ++x) {
        const double ddx = (x + 0.5) - cx;
        const double ddy = (y + 0.5) - cy;
        if (ddx * ddx + ddy * ddy <= radius * radius) set(x, y, c);
      }
    }
  }

  // Even-odd scanline fill of a closed polygon given in pixel coordinates.
  void fill_polygon(const std::vector<double>& xs, const std::vector<double>& ys, Rgb c,
                    std::uint32_t alpha) {
    const std::size_t n = xs.size();
    if (n < 3 || ys.size() != n) return;
    double ymin = ys[0], ymax = ys[0];
    for (std::size_t i = 1; i < n; ++i) {
      ymin = std::min(ymin, ys[i]);
      ymax = std::max(ymax, ys[i]);
    }
    const int y0 = std::max(0, static_cast<int>(std::floor(ymin)));
    const int y1 = std::min(static_cast<int>(h_) - 1, static_cast<int>(std::ceil(ymax)));
    std::vector<double> xsect;
    for (int y = y0; y <= y1; ++y) {
      const double yc = y + 0.5;
      xsect.clear();
      for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        const double ya = ys[i], yb = ys[j];
        if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
          const double t = (yc - ya) / (yb - ya);
          xsect.push_back(xs[i] + t * (xs[j] - xs[i]));
        }
      }
      std::sort(xsect.begin(), xsect.end());
      for (std::size_t k = 0; k + 1 < xsect.size(); k += 2) {
        const int xa = std::max(0, static_cast<int>(std::floor(xsect[k])));
        const int xb = std::min(static_cast<int>(w_) - 1, static_cast<int>(std::ceil(xsect[k + 1])));
        for (int x = xa; x <= xb; ++x) blend(x, y, c, alpha);
      }
    }
  }

 private:
  std::uint32_t w_, h_;
  std::vector<std::uint8_t> px_;
};

// --- a 5x7 stroke-free bitmap font -----------------------------------------
//
// Enough of ASCII to write a scale bar, a mode banner and a room label.
// Anything absent renders blank, which is why the caller is told the font is
// ASCII-only rather than being given a silent transliteration.
struct Glyph {
  char ch;
  std::uint8_t rows[7];  // low 5 bits, MSB-of-5 = leftmost column
};

const Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'#', {0x0A, 0x1F, 0x0A, 0x0A, 0x0A, 0x1F, 0x0A}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
};

const Glyph* glyph_for(char c) {
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  for (const Glyph& g : kFont) {
    if (g.ch == c) return &g;
  }
  return nullptr;
}

// Returns the advance in pixels.
int draw_text(Canvas* cv, int x, int y, const std::string& s, int scale, Rgb c) {
  int cx = x;
  for (char ch : s) {
    const Glyph* g = glyph_for(ch);
    if (g != nullptr) {
      for (int r = 0; r < 7; ++r) {
        for (int col = 0; col < 5; ++col) {
          if ((g->rows[r] >> (4 - col)) & 1u) {
            cv->fill_rect(cx + col * scale, y + r * scale, cx + col * scale + scale - 1,
                          y + r * scale + scale - 1, c);
          }
        }
      }
    }
    cx += 6 * scale;
  }
  return cx - x;
}

// --- palette ---------------------------------------------------------------

constexpr Rgb kPaper{0xFA, 0xFA, 0xF7};
constexpr Rgb kInk{0x1A, 0x1A, 0x1A};
constexpr Rgb kWallFill{0x2B, 0x2B, 0x2B};
constexpr Rgb kWallSingle{0x55, 0x5A, 0x62};
constexpr Rgb kDensityInk{0x33, 0x5C, 0x81};
constexpr Rgb kRoomTint{0x7E, 0xA9, 0xD8};
constexpr Rgb kOpeningDoor{0xC2, 0x62, 0x1F};
constexpr Rgb kOpeningWindow{0x1F, 0x8A, 0x6D};
constexpr Rgb kTrail{0x1E, 0x8C, 0x7F};       // ROUND 16 item 59: the walk
constexpr Rgb kTrailStart{0x2E, 0xC4, 0xB6};  // ...where it began
constexpr Rgb kTrailEnd{0xE8, 0x6A, 0x2B};    // ...and where it ended
constexpr Rgb kFrame{0x99, 0x99, 0x93};

// The drawing extent, with the tails trimmed.
//
// A handful of returns through a doorway or off a window puts the far wall of
// the NEXT room into the grid, and an untrimmed fit then renders the room the
// operator actually walked at a third of the page. Percentile rather than a
// standard deviation because the distribution is not remotely normal: it is a
// room plus a few spurs. 0.5 % of occupied cells is at most a couple of dozen
// cells on a real capture, and the trim is reported (it never silently drops
// a WALL — model bounds are unioned back in afterwards).
PlanBounds trimmed_extent(const OccupancyGrid& g, double keep_frac) {
  PlanBounds b;
  if (!g.valid()) return b;
  std::vector<double> xs, ys;
  for (std::uint32_t j = 0; j < g.h; ++j) {
    for (std::uint32_t i = 0; i < g.w; ++i) {
      if (!g.occupied(i, j)) continue;
      xs.push_back(g.cell_center_x(i));
      ys.push_back(g.cell_center_y(j));
    }
  }
  if (xs.size() < 20) return g.extent();
  std::sort(xs.begin(), xs.end());
  std::sort(ys.begin(), ys.end());
  const double drop = (1.0 - keep_frac) * 0.5;
  const std::size_t lo = static_cast<std::size_t>(drop * static_cast<double>(xs.size()));
  const std::size_t hi = xs.size() - 1 - lo;
  b.expand(Vec2{xs[lo], ys[lo]});
  b.expand(Vec2{xs[hi], ys[hi]});
  return b;
}

// The scale bar length: the largest 1/2/5 x 10^n metres that fits in a third
// of the drawing width. Deterministic, and it never prints "3.7 m".
double nice_bar_metres(double span_m) {
  const double target = span_m / 3.0;
  if (!(target > 0.0)) return 1.0;
  double mag = 1.0;
  while (mag * 10.0 <= target) mag *= 10.0;
  while (mag > target && mag > 1e-6) mag /= 10.0;
  const double candidates[3] = {mag, mag * 2.0, mag * 5.0};
  double best = candidates[0];
  for (double c : candidates) {
    if (c <= target) best = c;
  }
  return best;
}

}  // namespace

const char* to_string(PlanRenderMode m) noexcept {
  switch (m) {
    case PlanRenderMode::kWalls: return "walls";
    case PlanRenderMode::kDensity: return "density";
  }
  return "?";
}

Status encode_png_rgb8(const std::uint8_t* rgb, std::uint32_t w, std::uint32_t h,
                       std::string* out) {
  if (rgb == nullptr || out == nullptr || w == 0 || h == 0) {
    return set_last_error(ScanError::kInvalidArgument, "%s", "encode_png_rgb8: empty image");
  }
  // Filter byte 0 (None) per scanline. None rather than Sub/Up because the
  // deflate below is STORED anyway, so a filter would cost a pass and buy
  // nothing, and because it keeps the raw stream trivially verifiable.
  std::vector<std::uint8_t> raw;
  raw.reserve((static_cast<std::size_t>(w) * 3 + 1) * h);
  for (std::uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    const std::uint8_t* row = rgb + static_cast<std::size_t>(y) * w * 3;
    raw.insert(raw.end(), row, row + static_cast<std::size_t>(w) * 3);
  }

  out->clear();
  const char sig[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
  out->append(sig, 8);

  std::string ihdr;
  put_be32(&ihdr, w);
  put_be32(&ihdr, h);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type: truecolour RGB
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // adaptive filtering
  ihdr.push_back(0);  // no interlace
  put_chunk(out, "IHDR", ihdr);
  put_chunk(out, "IDAT", zlib_stored(raw));
  put_chunk(out, "IEND", std::string());
  return kOkStatus;
}

Status build_plan_png(const PlanModel& model, const OccupancyGrid* density,
                      const PlanRasterOptions& opts, std::string* out_png,
                      PlanRasterInfo* out_info) {
  if (out_png == nullptr) return set_last_error(ScanError::kInvalidArgument, "%s", "build_plan_png: null out");

  // --- 1. what are we drawing, and over what extent ---------------------
  PlanBounds b = model.bounds;
  if (density != nullptr && density->valid()) {
    b.expand(trimmed_extent(*density, opts.extent_keep_fraction));
  }
  // ROUND 16 item 59: the walk is part of the drawing, so it is part of the
  // extent. Without this a plan whose walls were trimmed by
  // `extent_keep_fraction` would clip the path at the edge of the paper, and a
  // path that stops at a margin reads as a scan that stopped there.
  if (opts.draw_trajectory) {
    for (const Vec2& p : opts.trajectory) b.expand(p);
  }
  if (!b.valid || !(b.width() > 0.0) || !(b.height() > 0.0)) {
    return set_last_error(ScanError::kInvalidArgument, "%s", "build_plan_png: nothing to draw (empty bounds)");
  }

  const bool have_walls = opts.draw_walls && !model.walls.empty();
  PlanRasterInfo info;
  info.mode = have_walls ? PlanRenderMode::kWalls : PlanRenderMode::kDensity;

  const std::uint32_t margin = opts.margin_px;
  const std::uint32_t caption_h = opts.caption ? 96u : 0u;
  const std::uint32_t maxdim = std::max<std::uint32_t>(opts.max_dimension_px, 256u);
  const double inner = static_cast<double>(maxdim) - 2.0 * margin;
  if (!(inner > 16.0)) return set_last_error(ScanError::kInvalidArgument, "%s", "build_plan_png: margins too big");

  const double sx = inner / b.width();
  const double sy = inner / b.height();
  const double px_per_m = std::min(sx, sy);
  info.px_per_m = px_per_m;

  const std::uint32_t draw_w =
      static_cast<std::uint32_t>(std::ceil(b.width() * px_per_m)) + 2u * margin;
  const std::uint32_t draw_h =
      static_cast<std::uint32_t>(std::ceil(b.height() * px_per_m)) + 2u * margin + caption_h;
  Canvas cv(std::max<std::uint32_t>(draw_w, 320u), std::max<std::uint32_t>(draw_h, 240u), kPaper);
  info.width_px = cv.width();
  info.height_px = cv.height();

  // Plan y grows up; image y grows down.
  const double ox = margin;
  const double oy = static_cast<double>(cv.height()) - margin - caption_h;
  auto PX = [&](double x) { return ox + (x - b.min_x) * px_per_m; };
  auto PY = [&](double y) { return oy - (y - b.min_y) * px_per_m; };

  // --- 2. the density backdrop ------------------------------------------
  if (opts.density_backdrop && density != nullptr && density->valid()) {
    const double cell_px = density->res_m * px_per_m;
    const int half = std::max(0, static_cast<int>(std::floor(cell_px * 0.5)));
    // In kDensity mode the cells ARE the drawing, so they are drawn at full
    // strength; as a backdrop under fitted walls they are faint enough that a
    // wall reads first and an UNFITTED run of returns still shows.
    const std::uint32_t alpha = have_walls ? 70u : 235u;
    for (std::uint32_t j = 0; j < density->h; ++j) {
      for (std::uint32_t i = 0; i < density->w; ++i) {
        if (!density->occupied(i, j)) continue;
        ++info.density_cells_drawn;
        const double cx = PX(density->cell_center_x(i));
        const double cy = PY(density->cell_center_y(j));
        const int ix = static_cast<int>(std::floor(cx));
        const int iy = static_cast<int>(std::floor(cy));
        for (int dy = -half; dy <= half; ++dy) {
          for (int dx = -half; dx <= half; ++dx) cv.blend(ix + dx, iy + dy, kDensityInk, alpha);
        }
      }
    }
  }

  // --- 3. rooms ----------------------------------------------------------
  if (opts.draw_rooms) {
    for (const Room& r : model.rooms) {
      if (r.polygon.size() < 3) continue;
      std::vector<double> xs, ys;
      xs.reserve(r.polygon.size());
      ys.reserve(r.polygon.size());
      for (const Vec2& v : r.polygon) {
        xs.push_back(PX(v.x));
        ys.push_back(PY(v.y));
      }
      cv.fill_polygon(xs, ys, kRoomTint, 60u);
    }
  }

  // --- 4. walls ----------------------------------------------------------
  if (have_walls) {
    for (const WallSegment& w : model.walls) {
      const bool paired = w.evidence == WallEvidence::kPairedFaces;
      // Capped: A12 allows a 40 cm wall, which at a phone-sized scale is a
      // 40-pixel-wide black bar that swallows the drawing it is part of. The
      // MEASURED thickness is in the DXF and the PDF, where it can be
      // dimensioned; here the wall only has to read as a wall.
      const double half_t = paired ? w.thickness_m * 0.5 * px_per_m : 0.0;
      const double stroke = std::min(6.0, std::max(1.2, half_t));
      cv.line(PX(w.a.x), PY(w.a.y), PX(w.b.x), PY(w.b.y), stroke,
              paired ? kWallFill : kWallSingle);
    }
  }

  // --- 5. openings -------------------------------------------------------
  if (opts.draw_openings) {
    for (const Opening& o : model.openings) {
      Rgb c = kInk;
      switch (o.kind) {
        case OpeningKind::kDoorCandidate: c = kOpeningDoor; break;
        case OpeningKind::kWindowCandidate: c = kOpeningWindow; break;
        default: continue;  // narrow gaps and wide openings are not marked
      }
      cv.line(PX(o.a.x), PY(o.a.y), PX(o.b.x), PY(o.b.y), 2.5, c);
    }
  }

  // --- 5b. ROUND 16 item 59: the walked path -----------------------------
  //
  // Drawn AFTER the walls and the openings and BEFORE the frame: it has to
  // read on top of the room (a path hidden under a wall stroke answers
  // nothing) and under the sheet's own furniture (a path over the scale bar
  // would make the scale unreadable, and the scale is what makes the drawing a
  // measurement).
  //
  // Opaque, at full strength. A translucent path would have to be blended
  // against a density backdrop whose own alpha already varies, and the result
  // would be a line whose colour meant something other than what the legend
  // says.
  if (opts.draw_trajectory && opts.trajectory.size() >= 2) {
    for (std::size_t i = 1; i < opts.trajectory.size(); ++i) {
      const Vec2& a = opts.trajectory[i - 1];
      const Vec2& c = opts.trajectory[i];
      cv.line(PX(a.x), PY(a.y), PX(c.x), PY(c.y),
              std::max(1.0, opts.trajectory_stroke_px), kTrail);
    }
    // The two ends, as discs, because the gap between them is the one number
    // on this sheet the operator can check against the summary card — it IS
    // the loop-end gap, drawn at true scale.
    const Vec2& first = opts.trajectory.front();
    const Vec2& last = opts.trajectory.back();
    cv.disc(PX(first.x), PY(first.y), std::max(3.0, opts.trajectory_stroke_px * 2.0),
            kTrailStart);
    cv.disc(PX(last.x), PY(last.y), std::max(3.0, opts.trajectory_stroke_px * 2.0), kTrailEnd);
    info.trajectory_points_drawn = static_cast<std::uint32_t>(opts.trajectory.size());
  }

  // --- 6. frame, scale bar, caption --------------------------------------
  cv.fill_rect(static_cast<int>(margin) - 8, static_cast<int>(margin) - 8,
               static_cast<int>(cv.width() - margin) + 8, static_cast<int>(margin) - 8, kFrame);
  cv.fill_rect(static_cast<int>(margin) - 8, static_cast<int>(oy) + 8,
               static_cast<int>(cv.width() - margin) + 8, static_cast<int>(oy) + 8, kFrame);
  cv.fill_rect(static_cast<int>(margin) - 8, static_cast<int>(margin) - 8,
               static_cast<int>(margin) - 8, static_cast<int>(oy) + 8, kFrame);
  cv.fill_rect(static_cast<int>(cv.width() - margin) + 8, static_cast<int>(margin) - 8,
               static_cast<int>(cv.width() - margin) + 8, static_cast<int>(oy) + 8, kFrame);

  int cursor_y = static_cast<int>(oy) + 22;
  if (opts.scale_bar) {
    const double bar_m = nice_bar_metres(b.width());
    info.scale_bar_m = bar_m;
    const int bx = static_cast<int>(margin);
    const int bw = static_cast<int>(std::floor(bar_m * px_per_m));
    cv.fill_rect(bx, cursor_y, bx + bw, cursor_y + 5, kInk);
    cv.fill_rect(bx, cursor_y - 4, bx + 1, cursor_y + 9, kInk);
    cv.fill_rect(bx + bw - 1, cursor_y - 4, bx + bw, cursor_y + 9, kInk);
    const std::string lbl = fmt_fixed(bar_m, bar_m < 1.0 ? 2 : 0) + " M";
    draw_text(&cv, bx + bw + 12, cursor_y - 3, lbl, 2, kInk);
    cursor_y += 24;
  }

  if (opts.caption) {
    if (!opts.title.empty()) {
      draw_text(&cv, static_cast<int>(margin), cursor_y, opts.title, 2, kInk);
      cursor_y += 20;
    }
    std::string l1;
    if (info.mode == PlanRenderMode::kWalls) {
      l1 = fmt_int(static_cast<long long>(model.walls.size())) + " WALLS  " +
           fmt_fixed(model.stats.total_wall_length_m, 1) + " M  ";
      l1 += model.rooms.empty()
                ? std::string("NO ROOM CLOSED")
                : fmt_int(static_cast<long long>(model.rooms.size())) + " ROOMS  " +
                      fmt_fixed(model.stats.total_room_area_m2, 1) + " M2";
    } else {
      l1 = "SLICE DENSITY - NO WALLS FITTED";
    }
    draw_text(&cv, static_cast<int>(margin), cursor_y, l1, 2, kInk);
    cursor_y += 20;
    const std::string l2 = "SLICE " + fmt_fixed(model.slice_z_min_m, 2) + "-" +
                           fmt_fixed(model.slice_z_max_m, 2) + " M  GRID " +
                           fmt_fixed(model.grid_res_m, 3) + " M  1 PX = " +
                           fmt_fixed(px_per_m > 0.0 ? 1.0 / px_per_m : 0.0, 4) + " M";
    draw_text(&cv, static_cast<int>(margin), cursor_y, l2, 1, kInk);
  }

  const Status s = encode_png_rgb8(cv.data(), cv.width(), cv.height(), out_png);
  if (!s.ok()) return s;
  if (out_info != nullptr) *out_info = info;
  return kOkStatus;
}

Status write_plan_png(const PlanModel& model, const OccupancyGrid* density,
                      const PlanRasterOptions& opts, const std::string& path,
                      PlanRasterInfo* out_info) {
  if (path.empty()) return set_last_error(ScanError::kInvalidArgument, "%s", "write_plan_png: empty path");
  std::string png;
  const Status s = build_plan_png(model, density, opts, &png, out_info);
  if (!s.ok()) return s;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return set_last_error(ScanError::kFileError, "write_plan_png: cannot open %s", path.c_str());
  const std::size_t n = std::fwrite(png.data(), 1, png.size(), f);
  const int rc = std::fclose(f);
  if (n != png.size() || rc != 0) {
    return set_last_error(ScanError::kFileError, "write_plan_png: short write to %s", path.c_str());
  }
  return kOkStatus;
}

}  // namespace plan
}  // namespace scanengine
