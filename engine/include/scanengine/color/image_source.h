// image_source.h — keyframe pixels (A11).
//
// Colorization needs one thing from an image: RGB at a sub-pixel coordinate.
// This header is the seam that supplies it, so the projector can be tested
// against analytically rendered images with no JPEG anywhere, and so a future
// caller can hand over pixels it already has decoded (the Android capture
// path holds the CameraX buffer; re-encoding and re-decoding it would be
// silly).
//
// The default implementation decodes JPEG (and PNG) with **stb_image**,
// vendored at `engine/src/color/stb_image.h`:
//
//   stb_image v2.30 by Sean Barrett — dual-licensed PUBLIC DOMAIN
//   (Unlicense) / MIT, at the user's choice. Both licences are reproduced in
//   full at the bottom of the vendored file. No attribution is required under
//   the public-domain option; the MIT option requires the copyright notice,
//   which the vendored file carries.
//
// Why vendored rather than a vcpkg port: a single header with no build system
// clears all five CI legs by construction, which `vcpkg.json`'s
// `$dependency-onboarding-order` note asks every new dependency to prove; and
// libjpeg-turbo would be a real port for a feature that decodes a few hundred
// 4032×3024 JPEGs per session, off the hot path.
//
// Owner: A11.
#ifndef SCANENGINE_COLOR_IMAGE_SOURCE_H
#define SCANENGINE_COLOR_IMAGE_SOURCE_H

#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/color/colorize.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"

namespace scanengine {
namespace color {

// 8-bit RGB, row-major, tightly packed (stride = width * 3). Alpha is dropped
// at decode: a keyframe has none, and carrying it would cost a third more
// memory for a 12 MPix image.
struct DecodedImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;

  bool valid() const {
    return width > 0 && height > 0 &&
           rgb.size() == static_cast<std::size_t>(width) * height * 3u;
  }
  void clear() {
    width = height = 0;
    rgb.clear();
    rgb.shrink_to_fit();
  }
  // Nearest-pixel fetch, clamped at the border.
  void at(std::int32_t x, std::int32_t y, std::uint8_t out_rgb[3]) const;
  // Bilinear sample in pixel-centre convention: pixel (i, j)'s centre is at
  // (i + 0.5, j + 0.5). Clamped at the border.
  void sample_bilinear(double u, double v, std::uint8_t out_rgb[3]) const;
};

// Decodes an encoded image (JPEG/PNG) from memory. kCorruptData on a decode
// failure, with stb's reason in `last_error()`.
Status decode_image(ByteSpan encoded, DecodedImage* out);

// Decodes a file. kFileError if it cannot be read.
Status decode_image_file(const std::string& path, DecodedImage* out);

// Where a colorizer gets its pixels. Called once per keyframe, in keyframe
// order, on the colorizing thread; the colorizer holds at most one decoded
// image at a time, so an implementation may reuse a buffer.
class ImageSource {
 public:
  virtual ~ImageSource() = default;
  // kNotFound is not fatal to a run: the keyframe is skipped and counted.
  virtual Status load(const Keyframe& kf, DecodedImage* out) = 0;
};

// Reads `<lscan_dir>/<kf.image_path>` and decodes it. Verifies
// `kf.image_bytes` when the record carries one (0 = unknown) and verifies
// the decoded size against `kf.intrinsics.width/height`, because an
// intrinsics/image mismatch silently smears colour across the whole cloud —
// it is exactly the failure a per-keyframe check catches for free.
class FileImageSource final : public ImageSource {
 public:
  explicit FileImageSource(std::string lscan_dir);
  Status load(const Keyframe& kf, DecodedImage* out) override;

  // Off by default for `image_bytes` only (a re-packed .lscan may legitimately
  // differ); the dimension check is always on.
  void set_verify_size(bool on) { verify_bytes_ = on; }

 private:
  std::string root_;
  bool verify_bytes_ = false;
};

}  // namespace color
}  // namespace scanengine

#endif  // SCANENGINE_COLOR_IMAGE_SOURCE_H
