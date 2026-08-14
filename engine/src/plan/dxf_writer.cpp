// dxf_writer.cpp — hand-rolled ASCII DXF R12 (AC1009).
//
// THE FORMAT, in one paragraph, because that is all it is. A DXF file is a
// flat sequence of (group code, value) pairs, each on its own line: the code
// line, then the value line. Structure comes entirely from sentinel pairs —
// (0, SECTION) opens a section, (2, NAME) names it, (0, ENDSEC) closes it,
// (0, EOF) ends the file. Group codes carry fixed meanings: 0 = entity type,
// 1 = primary text, 2 = name, 6 = linetype, 7 = text style, 8 = layer,
// 10/20/30 = first point x/y/z, 11/21/31 = second point, 40 = a size,
// 62 = colour, 66 = "vertices follow", 70 = flags, 72 = justification.
//
// WHY R12. It is the last revision every CAD, GIS and drafting tool reads
// without argument. It has no LWPOLYLINE (so a polyline is a POLYLINE entity
// followed by VERTEX entities and a SEQEND), no OBJECTS section, no CLASSES
// section, no required handles, and no encoding declaration. A floor plan of
// straight walls, gap markers and area labels needs nothing newer, and every
// feature we would gain costs a reader somewhere.
//
// FOUR LAYERS.
//   WALLS       one closed 4-vertex polyline per wall = its footprint
//               (centreline +- half thickness), optionally the centreline too
//   OPENINGS    a 2-vertex polyline across each gap, plus its label
//   ROOMS       one closed polyline per room, on the interior face
//   DIMENSIONS  room name + area text, and the overall bounding dimensions
//
// LINE ENDINGS are "\n", not CRLF. Both are accepted by every reader (the
// spec's own parser strips whitespace), and one of them is byte-identical on
// every platform this engine builds for, which is what the determinism test
// requires.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "plan_internal.h"
#include "scanengine/plan/plan_writers.h"

namespace scanengine {
namespace plan {
namespace {

// R12 layer/style names: uppercase, no spaces, <= 31 characters. Anything
// else is silently rewritten rather than written out and rejected by the
// reader on the other end.
std::string sanitize_name(const std::string& in, const char* fallback) {
  std::string out;
  for (char c : in) {
    if (out.size() >= 31) break;
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '-' || u == '$') {
      out.push_back(c);
    } else if (u >= 'a' && u <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back('_');
    }
  }
  if (out.empty()) out = fallback;
  return out;
}

// TEXT values are single-line and ASCII. A newline would break the group-code
// framing outright (the reader would take the tail as the next code), so it
// is removed, not escaped.
std::string sanitize_text(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) continue;
    if (u > 0x7e) {
      out.push_back('?');
      continue;
    }
    out.push_back(c);
  }
  return out;
}

class DxfBuilder {
 public:
  explicit DxfBuilder(const DxfOptions& o) {
    walls_ = sanitize_name(o.layer_walls, "WALLS");
    openings_ = sanitize_name(o.layer_openings, "OPENINGS");
    rooms_ = sanitize_name(o.layer_rooms, "ROOMS");
    dims_ = sanitize_name(o.layer_dimensions, "DIMENSIONS");
    dec_ = std::min(12, std::max(0, o.decimals));
  }

  void pair(int code, const std::string& value) {
    s_ += fmt_int(code);
    s_ += '\n';
    s_ += value;
    s_ += '\n';
  }
  void pair(int code, int value) { pair(code, fmt_int(value)); }
  void real(int code, double value) { pair(code, fmt_fixed(value, dec_)); }

  void point(int base, Vec2 p, double z = 0.0) {
    real(base, p.x);
    real(base + 10, p.y);
    real(base + 20, z);
  }

  void polyline(const std::string& layer, const std::vector<Vec2>& pts, bool closed) {
    if (pts.size() < 2) return;
    pair(0, "POLYLINE");
    pair(8, layer);
    pair(66, 1);  // vertices follow
    pair(70, closed ? 1 : 0);
    // The POLYLINE header's own 10/20/30 is unused for a 2D polyline but is
    // required to be present; the spec says write zeroes.
    real(10, 0.0);
    real(20, 0.0);
    real(30, 0.0);
    for (const Vec2& p : pts) {
      pair(0, "VERTEX");
      pair(8, layer);
      point(10, p);
    }
    pair(0, "SEQEND");
    pair(8, layer);
  }

  void line(const std::string& layer, Vec2 a, Vec2 b) {
    pair(0, "LINE");
    pair(8, layer);
    point(10, a);
    point(11, b);
  }

  void text(const std::string& layer, Vec2 at, double height, const std::string& value,
            bool centered) {
    pair(0, "TEXT");
    pair(8, layer);
    point(10, at);
    real(40, height);
    pair(1, sanitize_text(value));
    pair(7, "STANDARD");
    if (centered) {
      pair(72, 1);       // horizontally centred
      point(11, at);     // ... about this alignment point
    }
  }

  const std::string& str() const { return s_; }
  std::string& str() { return s_; }
  const std::string& layer_walls() const { return walls_; }
  const std::string& layer_openings() const { return openings_; }
  const std::string& layer_rooms() const { return rooms_; }
  const std::string& layer_dims() const { return dims_; }

 private:
  std::string s_;
  std::string walls_, openings_, rooms_, dims_;
  int dec_ = 5;
};

PlanBounds drawn_bounds(const PlanModel& m) {
  PlanBounds b;
  for (const auto& w : m.walls) {
    for (const auto& fp : wall_footprints(w, m.openings)) {
      for (const Vec2& p : fp) b.expand(p);
    }
  }
  for (const auto& r : m.rooms) {
    for (const Vec2& p : r.polygon) b.expand(p);
  }
  for (const auto& op : m.openings) {
    b.expand(op.a);
    b.expand(op.b);
  }
  if (!b.valid) {
    b.expand(Vec2{0.0, 0.0});
    b.expand(Vec2{1.0, 1.0});
  }
  return b;
}

std::string opening_label(const Opening& op) {
  std::string s = to_string(op.kind);
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s + " " + fmt_fixed(op.width_m, 2);
}

}  // namespace

Status build_dxf(const PlanModel& model, const DxfOptions& opts, std::string* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "plan: build_dxf(out == null)");
  }
  DxfBuilder b(opts);
  const double th = std::max(0.01, opts.text_height_m);
  const PlanBounds geom = drawn_bounds(model);
  // $EXTMIN/$EXTMAX are what a CAD package's ZOOM EXTENTS uses, so they have
  // to bracket everything the file actually draws — including the dimension
  // lines and their text, which sit OUTSIDE the geometry by construction.
  PlanBounds ext = geom;
  if (opts.overall_dimensions && geom.valid) {
    const double off = th * 3.0;
    ext.expand(Vec2{geom.min_x - off - th * 2.5, geom.min_y - off - th * 2.5});
  }

  // --- HEADER --------------------------------------------------------------
  b.pair(0, "SECTION");
  b.pair(2, "HEADER");
  b.pair(9, "$ACADVER");
  b.pair(1, "AC1009");
  b.pair(9, "$INSBASE");
  b.real(10, 0.0);
  b.real(20, 0.0);
  b.real(30, 0.0);
  b.pair(9, "$EXTMIN");
  b.real(10, ext.min_x);
  b.real(20, ext.min_y);
  b.real(30, 0.0);
  b.pair(9, "$EXTMAX");
  b.real(10, ext.max_x);
  b.real(20, ext.max_y);
  b.real(30, 0.0);
  b.pair(9, "$LTSCALE");
  b.real(40, 1.0);
  b.pair(9, "$INSUNITS");
  b.pair(70, 6);  // 6 = metres. The model is metric and says so.
  b.pair(0, "ENDSEC");

  // --- TABLES --------------------------------------------------------------
  b.pair(0, "SECTION");
  b.pair(2, "TABLES");

  b.pair(0, "TABLE");
  b.pair(2, "LTYPE");
  b.pair(70, 1);
  b.pair(0, "LTYPE");
  b.pair(2, "CONTINUOUS");
  b.pair(70, 0);
  b.pair(3, "Solid line");
  b.pair(72, 65);
  b.pair(73, 0);
  b.real(40, 0.0);
  b.pair(0, "ENDTAB");

  struct LayerDef {
    const std::string* name;
    int color;
  };
  const LayerDef layers[] = {
      {&b.layer_walls(), 7},      // white/black — the drawing's main content
      {&b.layer_openings(), 1},   // red
      {&b.layer_rooms(), 3},      // green
      {&b.layer_dims(), 4},       // cyan
  };
  b.pair(0, "TABLE");
  b.pair(2, "LAYER");
  b.pair(70, static_cast<int>(sizeof(layers) / sizeof(layers[0])));
  for (const auto& l : layers) {
    b.pair(0, "LAYER");
    b.pair(2, *l.name);
    b.pair(70, 0);
    b.pair(62, l.color);
    b.pair(6, "CONTINUOUS");
  }
  b.pair(0, "ENDTAB");

  b.pair(0, "TABLE");
  b.pair(2, "STYLE");
  b.pair(70, 1);
  b.pair(0, "STYLE");
  b.pair(2, "STANDARD");
  b.pair(70, 0);
  b.real(40, 0.0);
  b.real(41, 1.0);
  b.real(50, 0.0);
  b.pair(71, 0);
  b.real(42, th);
  b.pair(3, "txt");
  b.pair(4, "");
  b.pair(0, "ENDTAB");

  b.pair(0, "ENDSEC");

  // --- BLOCKS (empty, but the section must exist for some readers) ---------
  b.pair(0, "SECTION");
  b.pair(2, "BLOCKS");
  b.pair(0, "ENDSEC");

  // --- ENTITIES ------------------------------------------------------------
  b.pair(0, "SECTION");
  b.pair(2, "ENTITIES");

  for (const auto& w : model.walls) {
    if (opts.wall_footprint) {
      for (const auto& fp : wall_footprints(w, model.openings)) {
        b.polyline(b.layer_walls(), fp, true);
      }
    }
    if (opts.wall_centerlines || !opts.wall_footprint) {
      b.polyline(b.layer_walls(), {w.a, w.b}, false);
    }
  }

  for (const auto& op : model.openings) {
    b.polyline(b.layer_openings(), {op.a, op.b}, false);
    if (opts.opening_labels) {
      const Vec2 mid = op.midpoint();
      b.text(b.layer_openings(), Vec2{mid.x, mid.y + th * 0.4}, th * 0.8, opening_label(op),
             true);
    }
  }

  if (opts.room_polygons) {
    for (const auto& r : model.rooms) {
      if (r.polygon.size() >= 3) b.polyline(b.layer_rooms(), r.polygon, true);
    }
  }
  if (opts.room_labels) {
    for (const auto& r : model.rooms) {
      b.text(b.layer_dims(), Vec2{r.centroid.x, r.centroid.y + th * 0.3}, th, r.label, true);
      b.text(b.layer_dims(), Vec2{r.centroid.x, r.centroid.y - th * 1.1}, th,
             fmt_fixed(r.area_m2, 2) + " m2", true);
    }
  }

  if (opts.overall_dimensions && geom.valid) {
    const double off = th * 3.0;
    const Vec2 bl{geom.min_x, geom.min_y - off};
    const Vec2 br{geom.max_x, geom.min_y - off};
    const Vec2 tl{geom.min_x - off, geom.max_y};
    const Vec2 bl2{geom.min_x - off, geom.min_y};
    b.line(b.layer_dims(), bl, br);
    b.line(b.layer_dims(), bl2, tl);
    b.text(b.layer_dims(), Vec2{(bl.x + br.x) * 0.5, bl.y - th * 1.2}, th,
           fmt_fixed(geom.width(), 3), true);
    b.text(b.layer_dims(), Vec2{tl.x - th * 1.2, (bl2.y + tl.y) * 0.5}, th,
           fmt_fixed(geom.height(), 3), true);
  }

  b.pair(0, "ENDSEC");
  b.pair(0, "EOF");

  *out = std::move(b.str());
  return kOkStatus;
}

Status write_dxf(const PlanModel& model, const DxfOptions& opts, const std::string& path) {
  if (path.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "plan: write_dxf with an empty path");
  }
  std::string body;
  SCAN_TRY(build_dxf(model, opts, &body));
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return set_last_error(ScanError::kFileError, "plan: cannot open '%s' for writing",
                          path.c_str());
  }
  f.write(body.data(), static_cast<std::streamsize>(body.size()));
  f.flush();
  if (!f) {
    return set_last_error(ScanError::kFileError, "plan: failed writing %zu bytes to '%s'",
                          body.size(), path.c_str());
  }
  return kOkStatus;
}

}  // namespace plan
}  // namespace scanengine
