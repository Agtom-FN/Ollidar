// pdf_writer.cpp — hand-rolled minimal PDF 1.4: a scaled floor-plan sheet.
//
// THE FORMAT, in one paragraph. A PDF is a header line, a sequence of
// numbered indirect objects ("<n> 0 obj ... endobj"), a cross-reference table
// listing the BYTE OFFSET of every object, and a trailer that says how many
// objects there are and which one is the document catalog. A reader seeks to
// the end, reads `startxref` to find the table, and from then on addresses
// objects by offset — which is why a wrong offset is not a cosmetic bug but a
// file that will not open, and why the test recomputes every offset from the
// bytes rather than trusting the writer's own bookkeeping.
//
// Six objects, always the same six: catalog, page tree, page, content stream,
// Helvetica, Helvetica-Bold. Helvetica is one of the 14 standard Type 1 fonts
// every conforming reader must supply, so nothing is embedded and the file
// stays a few kilobytes. No compression: /FlateDecode would need zlib (a
// dependency this engine does not have and does not want, see A9), and a
// floor plan's content stream is small enough that it does not matter.
//
// THE SHEET. A4 or A3, portrait or landscape, at a scale off the standard
// architectural ladder. The model is in metres, the page is in PostScript
// points (1/72 inch), and 1 m at 1:D is 1000/D mm = (1000/D) * 72/25.4 pt.
// The plan is centred in the drawing area, which is the sheet minus margins
// minus whatever the title block reserves.
//
// NOTHING IS DERIVED FROM THE CLOCK. No /CreationDate, no /ModDate, no
// std::time() in the title block. A file that stamps itself cannot be tested
// for determinism, and a survey drawing's date belongs to the survey.
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

constexpr double kPtPerMm = 72.0 / 25.4;
constexpr double kA4WidthPt = 210.0 * kPtPerMm;   // 595.28
constexpr double kA4HeightPt = 297.0 * kPtPerMm;  // 841.89
constexpr double kA3WidthPt = 297.0 * kPtPerMm;
constexpr double kA3HeightPt = 420.0 * kPtPerMm;

// The standard architectural/engineering ladder, coarsest last. `auto` walks
// it in order and stops at the first entry the plan fits at, so it always
// picks the LARGEST drawing that fits — which is what a drafter would do.
const int kScaleLadder[] = {20, 25, 50, 100, 200, 500, 1000, 2000, 5000};

double pt_per_metre(int denom) {
  if (denom <= 0) denom = 100;
  return (1000.0 / static_cast<double>(denom)) * kPtPerMm;
}

// A PDF literal string: only \, ( and ) have to be escaped. Anything outside
// printable ASCII is replaced rather than encoded, because doing it properly
// means a /ToUnicode CMap and an embedded font, which is a different feature.
std::string pdf_string(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  out.push_back('(');
  for (char c : in) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '\\' || c == '(' || c == ')') {
      out.push_back('\\');
      out.push_back(c);
    } else if (u < 0x20 || u > 0x7e) {
      out.push_back('?');
    } else {
      out.push_back(c);
    }
  }
  out.push_back(')');
  return out;
}

std::string num(double v) { return fmt_fixed(v, 3); }

// Model metres -> page points.
struct Xform {
  double s = 1.0;   // points per metre
  double ox = 0.0;  // page point of model x = 0
  double oy = 0.0;
  Vec2 operator()(Vec2 p) const { return Vec2{ox + p.x * s, oy + p.y * s}; }
};

class Content {
 public:
  void raw(const std::string& s) { s_ += s; }
  void op(const std::string& s) {
    s_ += s;
    s_ += '\n';
  }
  void gsave() { op("q"); }
  void grestore() { op("Q"); }
  void width(double w) { op(num(w) + " w"); }
  void stroke_gray(double g) { op(num(g) + " G"); }
  void fill_gray(double g) { op(num(g) + " g"); }
  void dash(double on, double off) {
    op("[" + num(on) + " " + num(off) + "] 0 d");
  }
  void dash_off() { op("[] 0 d"); }
  void move(Vec2 p) { op(num(p.x) + " " + num(p.y) + " m"); }
  void line(Vec2 p) { op(num(p.x) + " " + num(p.y) + " l"); }
  void close() { op("h"); }
  void stroke() { op("S"); }
  void fill() { op("f"); }
  void fill_stroke() { op("B"); }
  void rect(double x, double y, double w, double h) {
    op(num(x) + " " + num(y) + " " + num(w) + " " + num(h) + " re");
  }
  void clip() { op("W n"); }
  void segment(Vec2 a, Vec2 b) {
    move(a);
    line(b);
    stroke();
  }
  void polygon(const std::vector<Vec2>& pts, bool do_fill, bool do_stroke) {
    if (pts.size() < 2) return;
    move(pts[0]);
    for (std::size_t k = 1; k < pts.size(); ++k) line(pts[k]);
    close();
    if (do_fill && do_stroke) {
      fill_stroke();
    } else if (do_fill) {
      fill();
    } else {
      stroke();
    }
  }
  void text(Vec2 at, double size, const std::string& value, bool bold = false) {
    op("BT");
    op(std::string("/") + (bold ? "F2" : "F1") + " " + num(size) + " Tf");
    op(num(at.x) + " " + num(at.y) + " Td");
    op(pdf_string(value) + " Tj");
    op("ET");
  }
  // Helvetica's average advance is ~0.5 em over mixed case; good enough to
  // centre a label, and it costs no font metrics table.
  void text_centered(Vec2 at, double size, const std::string& value, bool bold = false) {
    const double wpt = 0.5 * size * static_cast<double>(value.size());
    text(Vec2{at.x - wpt * 0.5, at.y}, size, value, bold);
  }
  const std::string& str() const { return s_; }

 private:
  std::string s_;
};

PlanBounds model_extent(const PlanModel& m) {
  PlanBounds b;
  for (const auto& w : m.walls) {
    for (const auto& fp : wall_footprints(w, m.openings)) {
      for (const Vec2& p : fp) b.expand(p);
    }
  }
  for (const auto& r : m.rooms) {
    for (const Vec2& p : r.polygon) b.expand(p);
  }
  if (!b.valid) {
    b.expand(Vec2{0.0, 0.0});
    b.expand(Vec2{1.0, 1.0});
  }
  return b;
}

struct Layout {
  double page_w = 0.0;
  double page_h = 0.0;
  double draw_x = 0.0;
  double draw_y = 0.0;
  double draw_w = 0.0;
  double draw_h = 0.0;
  double block_x = 0.0;
  double block_y = 0.0;
  double block_w = 0.0;
  double block_h = 0.0;
};

Layout compute_layout(const PdfOptions& o) {
  Layout L;
  sheet_size_pt(o, &L.page_w, &L.page_h);
  const double m = std::max(0.0, o.margin_mm) * kPtPerMm;
  L.draw_x = m;
  L.draw_y = m;
  L.draw_w = L.page_w - 2.0 * m;
  L.draw_h = L.page_h - 2.0 * m;
  if (o.title_block) {
    L.block_w = std::min(220.0, L.draw_w);
    L.block_h = 76.0;
    L.block_x = L.draw_x + L.draw_w - L.block_w;
    L.block_y = L.draw_y;
    // The plan gets the sheet above the title block strip.
    L.draw_y += L.block_h + 8.0;
    L.draw_h -= L.block_h + 8.0;
  }
  if (L.draw_w < 10.0) L.draw_w = 10.0;
  if (L.draw_h < 10.0) L.draw_h = 10.0;
  return L;
}

std::string scale_label(int denom) { return "1:" + fmt_int(denom); }

// A round bar length whose paper length lands in a comfortable band.
double pick_bar_metres(double pt_per_m) {
  const double ladder[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0};
  double best = ladder[0];
  for (double L : ladder) {
    const double w = L * pt_per_m;
    if (w >= 45.0 && w <= 150.0) return L;
    if (w < 150.0) best = L;
  }
  return best;
}

}  // namespace

const char* to_string(SheetSize s) noexcept {
  switch (s) {
    case SheetSize::kA4: return "A4";
    case SheetSize::kA3: return "A3";
  }
  return "?";
}

void sheet_size_pt(const PdfOptions& opts, double* width_pt, double* height_pt) {
  double w = kA4WidthPt, h = kA4HeightPt;
  if (opts.sheet == SheetSize::kA3) {
    w = kA3WidthPt;
    h = kA3HeightPt;
  }
  if (opts.orientation == SheetOrientation::kLandscape) std::swap(w, h);
  if (width_pt) *width_pt = w;
  if (height_pt) *height_pt = h;
}

int auto_scale_denominator(const PlanBounds& bounds, const PdfOptions& opts) {
  const Layout L = compute_layout(opts);
  const double w_m = std::max(0.001, bounds.width());
  const double h_m = std::max(0.001, bounds.height());
  for (int denom : kScaleLadder) {
    const double s = pt_per_metre(denom);
    if (w_m * s <= L.draw_w && h_m * s <= L.draw_h) return denom;
  }
  return kScaleLadder[sizeof(kScaleLadder) / sizeof(kScaleLadder[0]) - 1];
}

Status build_pdf(const PlanModel& model, const PdfOptions& opts, std::string* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "plan: build_pdf(out == null)");
  }
  const Layout L = compute_layout(opts);
  const PlanBounds ext = model_extent(model);
  const int denom =
      opts.scale_denominator > 0 ? opts.scale_denominator : auto_scale_denominator(ext, opts);
  const double s = pt_per_metre(denom);

  Xform T;
  T.s = s;
  T.ox = L.draw_x + (L.draw_w - ext.width() * s) * 0.5 - ext.min_x * s;
  T.oy = L.draw_y + (L.draw_h - ext.height() * s) * 0.5 - ext.min_y * s;

  Content c;
  const double lw = std::max(0.05, opts.line_width_mm) * kPtPerMm;

  // --- the plan, clipped to its own area ----------------------------------
  c.gsave();
  c.rect(L.draw_x, L.draw_y, L.draw_w, L.draw_h);
  c.clip();

  c.width(lw);
  c.stroke_gray(0.0);
  c.fill_gray(0.35);
  for (const auto& w : model.walls) {
    for (const auto& piece : wall_footprints(w, model.openings)) {
      std::vector<Vec2> fp;
      fp.reserve(piece.size());
      for (const Vec2& p : piece) fp.push_back(T(p));
      c.polygon(fp, opts.wall_fill, true);
    }
  }

  if (opts.opening_marks && !model.openings.empty()) {
    c.gsave();
    c.width(lw * 0.6);
    c.stroke_gray(0.45);
    c.dash(3.0, 2.0);
    for (const auto& op : model.openings) c.segment(T(op.a), T(op.b));
    c.dash_off();
    c.grestore();
  }

  if (opts.room_labels) {
    for (const auto& r : model.rooms) {
      const Vec2 at = T(r.centroid);
      c.fill_gray(0.0);
      c.text_centered(Vec2{at.x, at.y + 2.0}, 8.0, r.label, true);
      c.text_centered(Vec2{at.x, at.y - 8.0}, 7.0, fmt_fixed(r.area_m2, 2) + " m2");
    }
  }
  c.grestore();

  // --- scale bar -----------------------------------------------------------
  if (opts.scale_bar) {
    const double bar_m = pick_bar_metres(s);
    const double bar_pt = bar_m * s;
    const double x0 = L.draw_x;
    const double y0 = L.draw_y - 20.0 < 0.0 ? L.draw_y + 6.0 : L.draw_y - 20.0;
    const int divisions = 4;
    c.gsave();
    c.width(0.5);
    c.stroke_gray(0.0);
    for (int k = 0; k < divisions; ++k) {
      const double xk = x0 + bar_pt * static_cast<double>(k) / divisions;
      const double wk = bar_pt / divisions;
      c.fill_gray((k % 2 == 0) ? 0.0 : 1.0);
      c.rect(xk, y0, wk, 4.0);
      c.fill_stroke();
    }
    c.fill_gray(0.0);
    c.text(Vec2{x0, y0 - 9.0}, 6.5, "0");
    c.text(Vec2{x0 + bar_pt - 12.0, y0 - 9.0}, 6.5, fmt_trim(bar_m, 1) + " m");
    c.text(Vec2{x0, y0 + 8.0}, 7.0, scale_label(denom) + " (" + to_string(opts.sheet) + ")");
    c.grestore();
  }

  // --- north arrow (placeholder unless a real bearing was supplied) --------
  if (opts.north_arrow) {
    const double cx = L.draw_x + L.draw_w - 22.0;
    const double cy = L.draw_y + L.draw_h - 26.0;
    const double r = 13.0;
    const double a = -opts.north_angle_deg * 3.14159265358979323846 / 180.0 +
                     3.14159265358979323846 * 0.5;
    const Vec2 tip{cx + r * std::cos(a), cy + r * std::sin(a)};
    const Vec2 lft{cx + r * 0.45 * std::cos(a + 2.45), cy + r * 0.45 * std::sin(a + 2.45)};
    const Vec2 rgt{cx + r * 0.45 * std::cos(a - 2.45), cy + r * 0.45 * std::sin(a - 2.45)};
    c.gsave();
    c.width(0.6);
    c.stroke_gray(0.0);
    c.fill_gray(0.0);
    c.polygon({tip, lft, Vec2{cx, cy}, rgt}, true, true);
    c.text_centered(Vec2{cx, cy - 20.0}, 7.5, opts.north_known ? "N" : "N (ASSUMED)", true);
    c.grestore();
  }

  // --- title block ---------------------------------------------------------
  if (opts.title_block) {
    c.gsave();
    c.width(0.8);
    c.stroke_gray(0.0);
    c.rect(L.block_x, L.block_y, L.block_w, L.block_h);
    c.stroke();
    double y = L.block_y + L.block_h - 13.0;
    c.fill_gray(0.0);
    c.text(Vec2{L.block_x + 6.0, y}, 9.0, opts.title.empty() ? "Floor plan" : opts.title, true);
    y -= 12.0;
    std::vector<std::string> rows;
    if (!opts.project.empty()) rows.push_back("Project: " + opts.project);
    rows.push_back("Scale: " + scale_label(denom) + "  Sheet: " + to_string(opts.sheet));
    rows.push_back("Slice: " + fmt_trim(model.slice_z_min_m, 2) + "-" +
                   fmt_trim(model.slice_z_max_m, 2) + " m  Grid: " +
                   fmt_trim(model.grid_res_m * 1000.0, 0) + " mm");
    rows.push_back("Walls: " + fmt_int(static_cast<long long>(model.walls.size())) +
                   "  Openings: " + fmt_int(static_cast<long long>(model.openings.size())) +
                   "  Rooms: " + fmt_int(static_cast<long long>(model.rooms.size())));
    if (!opts.date.empty()) rows.push_back("Date: " + opts.date);
    if (!opts.drawn_by.empty()) rows.push_back("Drawn: " + opts.drawn_by);
    if (!opts.reference.empty()) rows.push_back("Ref: " + opts.reference);
    for (const auto& row : rows) {
      if (y < L.block_y + 4.0) break;
      c.text(Vec2{L.block_x + 6.0, y}, 6.5, row);
      y -= 9.0;
    }
    c.grestore();
  }

  // --- assemble the file ---------------------------------------------------
  const std::string& stream = c.str();
  std::vector<std::string> objs;
  objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(L.page_w) + " " +
                 num(L.page_h) +
                 "] /Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>");
  objs.push_back("<< /Length " + fmt_int(static_cast<long long>(stream.size())) +
                 " >>\nstream\n" + stream + "endstream");
  objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                 "/WinAnsiEncoding >>");
  objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding "
                 "/WinAnsiEncoding >>");

  std::string pdf;
  pdf.reserve(stream.size() + 2048);
  pdf += "%PDF-1.4\n";
  // The binary comment: four bytes >= 0x80 on line 2 tell any tool that moves
  // the file that it is binary and must not be newline-translated.
  pdf += "%\xE2\xE3\xCF\xD3\n";

  std::vector<std::size_t> offsets;
  offsets.reserve(objs.size());
  for (std::size_t k = 0; k < objs.size(); ++k) {
    offsets.push_back(pdf.size());
    pdf += fmt_int(static_cast<long long>(k + 1));
    pdf += " 0 obj\n";
    pdf += objs[k];
    pdf += "\nendobj\n";
  }

  const std::size_t xref_off = pdf.size();
  pdf += "xref\n0 ";
  pdf += fmt_int(static_cast<long long>(objs.size() + 1));
  pdf += "\n";
  // Every entry is exactly 20 bytes: 10-digit offset, space, 5-digit
  // generation, space, type, space, newline. Readers index into this table
  // arithmetically, so the width is not negotiable.
  pdf += "0000000000 65535 f \n";
  for (std::size_t off : offsets) {
    std::string d = fmt_int(static_cast<long long>(off));
    while (d.size() < 10) d.insert(d.begin(), '0');
    pdf += d;
    pdf += " 00000 n \n";
  }
  pdf += "trailer\n<< /Size ";
  pdf += fmt_int(static_cast<long long>(objs.size() + 1));
  pdf += " /Root 1 0 R >>\nstartxref\n";
  pdf += fmt_int(static_cast<long long>(xref_off));
  pdf += "\n%%EOF\n";

  *out = std::move(pdf);
  return kOkStatus;
}

Status write_pdf(const PlanModel& model, const PdfOptions& opts, const std::string& path) {
  if (path.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "plan: write_pdf with an empty path");
  }
  std::string body;
  SCAN_TRY(build_pdf(model, opts, &body));
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
