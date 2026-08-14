#include "pcd_writer.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "byte_io.h"
#include "point_source.h"
#include "scanengine/core/error.h"

namespace scanengine::exportimpl {

namespace {
namespace fs = std::filesystem;

Status fail_io(const std::string& path, const char* why) {
  std::error_code ec;
  fs::remove(path, ec);
  return Status(set_last_error(ScanError::kIoError, "pcd export: %s (%s)", why, path.c_str()));
}

// PCL convention: pack 8-bit R/G/B into the low 24 bits of a uint32, then
// reinterpret those 32 bits as a float (NOT a numeric cast) — the "rgb"
// field is FLOAT32 but its bit pattern, not its value, is meaningful.
float pack_rgb_float(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  const std::uint32_t packed = (static_cast<std::uint32_t>(r) << 16) |
                                (static_cast<std::uint32_t>(g) << 8) |
                                static_cast<std::uint32_t>(b);
  float f;
  std::memcpy(&f, &packed, sizeof(f));
  return f;
}
}  // namespace

Status write_pcd(const PageStore& store, Span<const StreamId> streams, const std::string& path,
                  const ExportOptions& opts, ExportProgressCallback progress_cb,
                  void* progress_user_data, ExportCancelToken* cancel_token) {
  if (!host_is_little_endian()) {
    return Status(set_last_error(ScanError::kNotSupported,
                                  "pcd export: big-endian host not supported"));
  }

  const std::vector<PageView> pages = select_pages(store, streams);
  const std::uint64_t total = count_selected(pages, opts);

  const bool with_color = opts.include_color;
  const bool with_intensity = opts.include_intensity;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return fail_io(path, "could not open output file");

  std::ostringstream fields, sizes, types, counts;
  fields << "x y z";
  sizes << "4 4 4";
  types << "F F F";
  counts << "1 1 1";
  if (with_color) {
    fields << " rgb";
    sizes << " 4";
    types << " F";
    counts << " 1";
  }
  if (with_intensity) {
    fields << " intensity";
    sizes << " 4";
    types << " F";
    counts << " 1";
  }

  std::ostringstream hdr;
  hdr << "# .PCD v0.7 - Point Cloud Data file format\n"
      << "VERSION 0.7\n"
      << "FIELDS " << fields.str() << "\n"
      << "SIZE " << sizes.str() << "\n"
      << "TYPE " << types.str() << "\n"
      << "COUNT " << counts.str() << "\n"
      << "WIDTH " << total << "\n"
      << "HEIGHT 1\n"
      << "VIEWPOINT 0 0 0 1 0 0 0\n"
      << "POINTS " << total << "\n"
      << "DATA binary\n";
  const std::string hdr_str = hdr.str();
  out.write(hdr_str.data(), static_cast<std::streamsize>(hdr_str.size()));
  if (!out.good()) return fail_io(path, "failed writing header");

  std::uint8_t rec[5 * 4];
  const std::size_t rec_len = 12 + (with_color ? 4 : 0) + (with_intensity ? 4 : 0);

  const Status st = for_each_selected(
      pages, opts, total, progress_cb, progress_user_data, cancel_token,
      [&](const PointVertex& p, const PageView&, std::uint32_t, std::uint64_t) {
        std::size_t off = 0;
        put_f32le(rec + off, p.x);
        off += 4;
        put_f32le(rec + off, p.y);
        off += 4;
        put_f32le(rec + off, p.z);
        off += 4;
        if (with_color) {
          put_f32le(rec + off, pack_rgb_float(p.r, p.g, p.b));
          off += 4;
        }
        if (with_intensity) {
          put_f32le(rec + off, static_cast<float>(luminance8(p)));
          off += 4;
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
