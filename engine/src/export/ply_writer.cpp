#include "ply_writer.h"

#include <cstdint>
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
  return Status(set_last_error(ScanError::kIoError, "ply export: %s (%s)", why, path.c_str()));
}
}  // namespace

Status write_ply(const PageStore& store, Span<const StreamId> streams, const std::string& path,
                  const ExportOptions& opts, ExportProgressCallback progress_cb,
                  void* progress_user_data, ExportCancelToken* cancel_token) {
  if (!host_is_little_endian()) {
    return Status(set_last_error(ScanError::kNotSupported,
                                  "ply export: big-endian host not supported"));
  }

  const std::vector<PageView> pages = select_pages(store, streams);
  const std::uint64_t total = count_selected(pages, opts);

  const bool with_color = opts.include_color;
  const bool with_intensity = opts.include_intensity;

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return fail_io(path, "could not open output file");

  std::ostringstream hdr;
  hdr << "ply\n"
      << "format binary_little_endian 1.0\n"
      << "comment LidarScan A9 export (engine local metric frame)\n"
      << "element vertex " << total << "\n"
      << "property float x\n"
      << "property float y\n"
      << "property float z\n";
  if (with_color) {
    hdr << "property uchar red\n"
        << "property uchar green\n"
        << "property uchar blue\n";
  }
  if (with_intensity) {
    hdr << "property uchar intensity\n";
  }
  hdr << "end_header\n";
  const std::string hdr_str = hdr.str();
  out.write(hdr_str.data(), static_cast<std::streamsize>(hdr_str.size()));
  if (!out.good()) return fail_io(path, "failed writing header");

  // 3 floats always; up to 3 uchar color + 1 uchar intensity.
  std::uint8_t rec[3 * 4 + 4];
  const std::size_t rec_len = 12 + (with_color ? 3 : 0) + (with_intensity ? 1 : 0);

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
          rec[off++] = p.r;
          rec[off++] = p.g;
          rec[off++] = p.b;
        }
        if (with_intensity) {
          rec[off++] = luminance8(p);
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
