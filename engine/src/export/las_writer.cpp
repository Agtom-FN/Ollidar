#include "las_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "byte_io.h"
#include "point_source.h"
#include "scanengine/core/error.h"
#include "scanengine/export/las_constants.h"

namespace scanengine::exportimpl {

namespace {
namespace fs = std::filesystem;

Status fail_io(const std::string& path, const char* why) {
  std::error_code ec;
  fs::remove(path, ec);
  return Status(set_last_error(ScanError::kIoError, "las export: %s (%s)", why, path.c_str()));
}

struct Quant {
  double scale[3];
  double offset[3];
};

// Offset: rounded to the nearest whole metre at the bounds' midpoint (a
// conventional, human-legible LAS offset). Scale: 1 mm by default, widened
// by decades only if the data's extent from that offset would overflow a
// signed 32-bit integer -- PointVertex positions are already in the
// session's local metric frame (point_page.h), so this only matters for
// pathological synthetic inputs, not real captures.
Quant choose_quantization(const float mn[3], const float mx[3]) {
  Quant q;
  for (int a = 0; a < 3; ++a) {
    const double off = std::floor((static_cast<double>(mn[a]) + static_cast<double>(mx[a])) * 0.5);
    const double half_range =
        std::max(std::fabs(static_cast<double>(mx[a]) - off), std::fabs(static_cast<double>(mn[a]) - off));
    double scale = 0.001;
    while (half_range / scale > 2000000000.0) scale *= 10.0;
    q.scale[a] = scale;
    q.offset[a] = off;
  }
  return q;
}

std::int32_t quantize(double raw, double offset, double scale) {
  double v = std::round((raw - offset) / scale);
  v = std::min(v, 2147483647.0);
  v = std::max(v, -2147483648.0);
  return static_cast<std::int32_t>(v);
}

void creation_day_year(std::uint16_t* day_of_year, std::uint16_t* year) {
  const std::time_t now = std::time(nullptr);
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &now);
#else
  gmtime_r(&now, &tm_utc);
#endif
  *day_of_year = static_cast<std::uint16_t>(tm_utc.tm_yday + 1);  // LAS: 1-based
  *year = static_cast<std::uint16_t>(tm_utc.tm_year + 1900);
}

struct HeaderFields {
  std::uint8_t point_format = kLasPointFormat2;
  std::uint16_t point_record_len = kLasPointFormat2RecordLen;
  std::uint16_t global_encoding = 0;
  double scale[3] = {0.001, 0.001, 0.001};
  double offset[3] = {0.0, 0.0, 0.0};
  double max[3] = {0.0, 0.0, 0.0};
  double min[3] = {0.0, 0.0, 0.0};
  std::uint64_t point_count = 0;
  std::uint32_t vlr_count = 0;
  std::uint32_t offset_to_point_data = 0;
};

std::vector<std::uint8_t> build_header(const HeaderFields& f) {
  std::vector<std::uint8_t> h(kLas14HeaderSize, 0);
  std::uint8_t* d = h.data();

  put_bytes(d + 0, "LASF", 4);
  put_u16le(d + 4, 0);  // file source id
  put_u16le(d + 6, f.global_encoding);
  put_u32le(d + 8, 0);   // project GUID data 1
  put_u16le(d + 12, 0);  // project GUID data 2
  put_u16le(d + 14, 0);  // project GUID data 3
  std::memset(d + 16, 0, 8);  // project GUID data 4
  d[24] = 1;                  // version major
  d[25] = 4;                  // version minor
  put_fixed_str(d + 26, 32, "LidarScan");
  put_fixed_str(d + 58, 32, "scanengine A9 export");
  std::uint16_t day = 0, year = 0;
  creation_day_year(&day, &year);
  put_u16le(d + 90, day);
  put_u16le(d + 92, year);
  put_u16le(d + 94, static_cast<std::uint16_t>(kLas14HeaderSize));
  put_u32le(d + 96, f.offset_to_point_data);
  put_u32le(d + 100, f.vlr_count);
  d[104] = f.point_format;
  put_u16le(d + 105, f.point_record_len);

  // Legacy 32-bit count fields: populated for backward compatibility with
  // pre-1.4 readers ONLY for non-extended point formats (< 6) whose count
  // fits in 32 bits; zero otherwise, per the LAS 1.4 spec (formats 6-10
  // MUST leave them zero — the extended, 64-bit fields at 247/255 are
  // authoritative there).
  const bool extended_format = f.point_format >= 6;
  const bool legacy_fits = !extended_format && f.point_count <= 0xFFFFFFFFull;
  put_u32le(d + 107, legacy_fits ? static_cast<std::uint32_t>(f.point_count) : 0u);
  for (int r = 0; r < 5; ++r) {
    const std::uint32_t v =
        (legacy_fits && r == 0) ? static_cast<std::uint32_t>(f.point_count) : 0u;
    put_u32le(d + 111 + r * 4, v);
  }

  put_f64le(d + 131, f.scale[0]);
  put_f64le(d + 139, f.scale[1]);
  put_f64le(d + 147, f.scale[2]);
  put_f64le(d + 155, f.offset[0]);
  put_f64le(d + 163, f.offset[1]);
  put_f64le(d + 171, f.offset[2]);
  put_f64le(d + 179, f.max[0]);
  put_f64le(d + 187, f.min[0]);
  put_f64le(d + 195, f.max[1]);
  put_f64le(d + 203, f.min[1]);
  put_f64le(d + 211, f.max[2]);
  put_f64le(d + 219, f.min[2]);

  put_u64le(d + 227, 0);  // start of waveform data packet record
  put_u64le(d + 235, 0);  // start of first EVLR
  put_u32le(d + 243, 0);  // number of EVLRs
  put_u64le(d + 247, f.point_count);
  // Single-return-only data model (no multi-return support in this engine
  // yet): every point counts against return 1, all higher returns are 0.
  for (int r = 0; r < 15; ++r) {
    put_u64le(d + 255 + r * 8, (r == 0) ? f.point_count : 0ull);
  }

  return h;
}

std::vector<std::uint8_t> build_wkt_vlr(const std::string& wkt, const std::string& description) {
  // The LAS spec does not require the WKT payload to be NUL-terminated, and
  // record_length_after_header is defined as the payload's exact byte
  // length -- so the payload is exactly `wkt`, no added terminator, and a
  // reader that trusts record_length_after_header (as it must) recovers the
  // string exactly.
  std::vector<std::uint8_t> v(54 + wkt.size(), 0);
  std::uint8_t* d = v.data();
  put_u16le(d + 0, 0);  // reserved
  put_fixed_str(d + 2, 16, kLasWktVlrUserId);
  put_u16le(d + 18, kLasWktVlrRecordId);
  put_u16le(d + 20, static_cast<std::uint16_t>(wkt.size()));
  put_fixed_str(d + 22, 32, description.c_str());
  std::memcpy(d + 54, wkt.data(), wkt.size());
  return v;
}

}  // namespace

Status write_las14(const PageStore& store, Span<const StreamId> streams, const std::string& path,
                    const ExportOptions& opts, ExportProgressCallback progress_cb,
                    void* progress_user_data, ExportCancelToken* cancel_token) {
  if (!host_is_little_endian()) {
    return Status(set_last_error(ScanError::kNotSupported,
                                  "las export: big-endian host not supported"));
  }

  const std::vector<PageView> pages = select_pages(store, streams);
  const SelectionStats stats = scan_selected(pages, opts);

  const std::string wkt = !opts.crs_wkt.empty() ? opts.crs_wkt : kLasLocalFramePlaceholderWkt;
  const std::string description = (opts.crs_wkt.empty() && !opts.crs_epsg.empty())
                                       ? ("Placeholder; requested " + opts.crs_epsg)
                                       : std::string("OGC Coordinate System WKT");
  const std::vector<std::uint8_t> vlr = build_wkt_vlr(wkt, description);

  const Quant q = choose_quantization(stats.min, stats.max);

  HeaderFields hf;
  hf.point_format = opts.las_gps_time ? kLasPointFormat7 : kLasPointFormat2;
  hf.point_record_len = opts.las_gps_time ? kLasPointFormat7RecordLen : kLasPointFormat2RecordLen;
  hf.global_encoding = kLasGlobalEncodingWktBit;
  hf.scale[0] = q.scale[0];
  hf.scale[1] = q.scale[1];
  hf.scale[2] = q.scale[2];
  hf.offset[0] = q.offset[0];
  hf.offset[1] = q.offset[1];
  hf.offset[2] = q.offset[2];
  hf.max[0] = stats.max[0];
  hf.max[1] = stats.max[1];
  hf.max[2] = stats.max[2];
  hf.min[0] = stats.min[0];
  hf.min[1] = stats.min[1];
  hf.min[2] = stats.min[2];
  hf.point_count = stats.count;
  hf.vlr_count = 1;
  hf.offset_to_point_data =
      static_cast<std::uint32_t>(kLas14HeaderSize + vlr.size());

  const std::vector<std::uint8_t> header = build_header(hf);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return fail_io(path, "could not open output file");

  out.write(reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
  out.write(reinterpret_cast<const char*>(vlr.data()), static_cast<std::streamsize>(vlr.size()));
  if (!out.good()) return fail_io(path, "failed writing header/VLR");

  const bool with_color = opts.include_color;
  const bool with_intensity = opts.include_intensity;
  const bool fmt7 = opts.las_gps_time;

  std::uint8_t rec[kLasPointFormat7RecordLen];
  const std::size_t rec_len = fmt7 ? kLasPointFormat7RecordLen : kLasPointFormat2RecordLen;

  const Status st = for_each_selected(
      pages, opts, stats.count, progress_cb, progress_user_data, cancel_token,
      [&](const PointVertex& p, const PageView& pv, std::uint32_t idx_in_page, std::uint64_t) {
        const std::int32_t xi = quantize(p.x, hf.offset[0], hf.scale[0]);
        const std::int32_t yi = quantize(p.y, hf.offset[1], hf.scale[1]);
        const std::int32_t zi = quantize(p.z, hf.offset[2], hf.scale[2]);
        const std::uint16_t intensity = with_intensity ? luminance16(p) : 0;
        const std::uint16_t red = with_color ? widen8to16(p.r) : 0;
        const std::uint16_t green = with_color ? widen8to16(p.g) : 0;
        const std::uint16_t blue = with_color ? widen8to16(p.b) : 0;

        if (!fmt7) {
          put_i32le(rec + 0, xi);
          put_i32le(rec + 4, yi);
          put_i32le(rec + 8, zi);
          put_u16le(rec + 12, intensity);
          rec[14] = 0x09;  // return_number=1 (bits0-2) | number_of_returns=1 (bits3-5)
          rec[15] = 0;     // classification
          rec[16] = 0;     // scan angle rank
          rec[17] = 0;     // user data
          put_u16le(rec + 18, 0);  // point source id
          put_u16le(rec + 20, red);
          put_u16le(rec + 22, green);
          put_u16le(rec + 24, blue);
        } else {
          // Approximate per-point GPS time: linear interpolation across the
          // page's [t_first_ns, t_last_ns] by in-page index. See
          // ExportOptions::las_gps_time's doc comment for why this is not a
          // real GPS time base.
          double t_seconds;
          if (pv.count > 1) {
            const double frac = static_cast<double>(idx_in_page) / static_cast<double>(pv.count - 1);
            t_seconds = (static_cast<double>(pv.t_first_ns) +
                         frac * static_cast<double>(pv.t_last_ns - pv.t_first_ns)) *
                        1e-9;
          } else {
            t_seconds = static_cast<double>(pv.t_first_ns) * 1e-9;
          }

          put_i32le(rec + 0, xi);
          put_i32le(rec + 4, yi);
          put_i32le(rec + 8, zi);
          put_u16le(rec + 12, intensity);
          rec[14] = 0x11;  // return_number=1 (bits0-3) | number_of_returns=1 (bits4-7)
          rec[15] = 0;     // classification flags | scanner channel | dir | edge
          rec[16] = 0;     // classification
          rec[17] = 0;     // user data
          put_i16le(rec + 18, 0);  // scan angle
          put_u16le(rec + 20, 0);  // point source id
          put_f64le(rec + 22, t_seconds);
          put_u16le(rec + 30, red);
          put_u16le(rec + 32, green);
          put_u16le(rec + 34, blue);
        }
        out.write(reinterpret_cast<const char*>(rec), static_cast<std::streamsize>(rec_len));
      });

  if (!st.ok()) {
    out.close();
    std::error_code ec;
    fs::remove(path, ec);
    return st;
  }
  if (!out.good()) return fail_io(path, "failed writing point data");

  out.close();
  if (out.fail()) return fail_io(path, "failed closing output file");
  return Status(ScanError::kOk);
}

}  // namespace scanengine::exportimpl
