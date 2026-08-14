// image_source.cpp — keyframe decoding (A11).
//
// This is the ONE translation unit that instantiates stb_image, exactly the
// way src/drivers/mid360/mid360_sdk2.cpp is the one that sees the Livox SDK:
// no public header mentions it, so no consumer of the engine inherits it.
#include "scanengine/color/image_source.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "scanengine/color/frames_idx.h"
#include "scanengine/core/log.h"

// --- vendored stb_image (public domain / MIT dual — see the file's tail) ----
//
// Only what a keyframe can be: JPEG (what CameraX writes) and PNG (what a
// desktop import or a test fixture is likely to hand over). Excluding the
// other nine formats keeps ~4,000 lines of decoder out of the binary.
//
// STBI_NO_STDIO: we read the file ourselves, so file errors come back as
// kFileError with a real message instead of stb's "can't fopen", and the same
// entry point serves an in-memory buffer (which is what the tests, and a
// future Android path holding a CameraX buffer, actually have).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void)0)

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4456 4457 4996)
#endif

#include "stb_image.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace scanengine {
namespace color {

namespace fs = std::filesystem;

namespace {
// A 12 MPix RGB frame is 36 MB; refuse anything that would be a decompression
// bomb rather than a keyframe (400 MPix ≈ 1.2 GB of RGB).
constexpr std::int64_t kMaxPixels = 400ll * 1000 * 1000;
}  // namespace

void DecodedImage::at(std::int32_t x, std::int32_t y, std::uint8_t out_rgb[3]) const {
  if (!valid()) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
    return;
  }
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= static_cast<std::int32_t>(width)) x = static_cast<std::int32_t>(width) - 1;
  if (y >= static_cast<std::int32_t>(height)) y = static_cast<std::int32_t>(height) - 1;
  const std::size_t idx = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 3u;
  out_rgb[0] = rgb[idx + 0];
  out_rgb[1] = rgb[idx + 1];
  out_rgb[2] = rgb[idx + 2];
}

void DecodedImage::sample_bilinear(double u, double v, std::uint8_t out_rgb[3]) const {
  if (!valid()) {
    out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
    return;
  }
  // Pixel centres sit at (i + 0.5): shift into "index space" before flooring,
  // so a sample exactly at a centre reproduces that pixel bit-exactly rather
  // than blending it with its neighbour.
  const double fx = u - 0.5;
  const double fy = v - 0.5;
  const double flx = std::floor(fx);
  const double fly = std::floor(fy);
  const std::int32_t x0 = static_cast<std::int32_t>(flx);
  const std::int32_t y0 = static_cast<std::int32_t>(fly);
  const double ax = fx - flx;
  const double ay = fy - fly;
  std::uint8_t c00[3], c10[3], c01[3], c11[3];
  at(x0, y0, c00);
  at(x0 + 1, y0, c10);
  at(x0, y0 + 1, c01);
  at(x0 + 1, y0 + 1, c11);
  for (int i = 0; i < 3; ++i) {
    const double top = c00[i] + (c10[i] - c00[i]) * ax;
    const double bot = c01[i] + (c11[i] - c01[i]) * ax;
    const double val = top + (bot - top) * ay;
    const double r = val < 0.0 ? 0.0 : (val > 255.0 ? 255.0 : val);
    // +0.5 rounding, not truncation: truncation biases every sampled colour
    // half a level dark, which is visible as a global cast on a whole cloud.
    out_rgb[i] = static_cast<std::uint8_t>(r + 0.5);
  }
}

Status decode_image(ByteSpan encoded, DecodedImage* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: decode_image(out == null)");
  }
  if (encoded.empty()) {
    return set_last_error(ScanError::kInvalidArgument, "color: decode_image on an empty buffer");
  }
  if (encoded.size() > static_cast<std::size_t>(INT32_MAX)) {
    return set_last_error(ScanError::kInvalidArgument, "color: image of %zu bytes is too large",
                          encoded.size());
  }
  int w = 0, h = 0, channels = 0;
  if (stbi_info_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels) &&
      static_cast<std::int64_t>(w) * h > kMaxPixels) {
    return set_last_error(ScanError::kInvalidArgument, "color: image %dx%d exceeds the pixel cap",
                          w, h);
  }
  stbi_uc* px = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h,
                                      &channels, 3);
  if (px == nullptr) {
    const char* why = stbi_failure_reason();
    return set_last_error(ScanError::kCorruptData, "color: image decode failed (%s)",
                          why != nullptr ? why : "unknown");
  }
  out->width = static_cast<std::uint32_t>(w);
  out->height = static_cast<std::uint32_t>(h);
  out->rgb.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
  stbi_image_free(px);
  return kOkStatus;
}

Status decode_image_file(const std::string& path, DecodedImage* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return set_last_error(ScanError::kFileError, "color: cannot open image '%s'", path.c_str());
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  if (f.bad()) {
    return set_last_error(ScanError::kIoError, "color: read error on image '%s'", path.c_str());
  }
  if (bytes.empty()) {
    return set_last_error(ScanError::kCorruptData, "color: image '%s' is empty", path.c_str());
  }
  return decode_image(ByteSpan(bytes.data(), bytes.size()), out);
}

FileImageSource::FileImageSource(std::string lscan_dir) : root_(std::move(lscan_dir)) {}

Status FileImageSource::load(const Keyframe& kf, DecodedImage* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: FileImageSource::load(out == null)");
  }
  const std::string path = keyframe_image_path(root_, kf);
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return set_last_error(ScanError::kNotFound, "color: keyframe image '%s' is missing",
                          path.c_str());
  }
  if (verify_bytes_ && kf.image_bytes != 0) {
    const auto on_disk = fs::file_size(path, ec);
    if (!ec && on_disk != kf.image_bytes) {
      return set_last_error(ScanError::kCorruptData,
                            "color: keyframe image '%s' is %llu bytes, index says %u", path.c_str(),
                            static_cast<unsigned long long>(on_disk), kf.image_bytes);
    }
  }
  SCAN_TRY(decode_image_file(path, out));
  if (kf.intrinsics.width != 0 && kf.intrinsics.height != 0 &&
      (out->width != kf.intrinsics.width || out->height != kf.intrinsics.height)) {
    const std::uint32_t iw = out->width, ih = out->height;
    out->clear();
    return set_last_error(ScanError::kInvalidArgument,
                          "color: keyframe image '%s' is %ux%u, intrinsics say %ux%u", path.c_str(),
                          iw, ih, kf.intrinsics.width, kf.intrinsics.height);
  }
  return kOkStatus;
}

}  // namespace color
}  // namespace scanengine
