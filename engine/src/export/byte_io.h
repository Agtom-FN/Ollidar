// byte_io.h — tiny little-endian byte-packing helpers shared by every A9
// writer (PLY/LAS/PCD are all little-endian binary formats on the wire).
//
// These write raw bytes via memcpy, which is only correct-LE on a
// little-endian host. Every writer calls host_is_little_endian() once up
// front and fails with kNotSupported rather than silently emitting
// byte-swapped files — acceptable because all five engine-ci legs (Windows
// x64, macOS universal, Linux x86_64, Android arm64) are little-endian; see
// docs/A9-export.md.
//
// Private to export/ — not part of the public API surface.
#ifndef SCANENGINE_SRC_EXPORT_BYTE_IO_H
#define SCANENGINE_SRC_EXPORT_BYTE_IO_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace scanengine::exportimpl {

inline bool host_is_little_endian() {
  const std::uint16_t v = 1;
  std::uint8_t b = 0;
  std::memcpy(&b, &v, 1);
  return b == 1;
}

inline void put_u16le(std::uint8_t* out, std::uint16_t v) { std::memcpy(out, &v, 2); }
inline void put_u32le(std::uint8_t* out, std::uint32_t v) { std::memcpy(out, &v, 4); }
inline void put_u64le(std::uint8_t* out, std::uint64_t v) { std::memcpy(out, &v, 8); }
inline void put_i16le(std::uint8_t* out, std::int16_t v) { std::memcpy(out, &v, 2); }
inline void put_i32le(std::uint8_t* out, std::int32_t v) { std::memcpy(out, &v, 4); }
inline void put_f32le(std::uint8_t* out, float v) { std::memcpy(out, &v, 4); }
inline void put_f64le(std::uint8_t* out, double v) { std::memcpy(out, &v, 8); }
inline void put_bytes(std::uint8_t* out, const void* src, std::size_t n) {
  std::memcpy(out, src, n);
}

// Writes `s` into a fixed-width field, truncated if too long, zero-padded if
// short. Used for LAS's System Identifier / Generating Software / VLR
// User ID / VLR Description fields.
inline void put_fixed_str(std::uint8_t* out, std::size_t field_len, const char* s) {
  std::size_t n = std::strlen(s);
  if (n > field_len) n = field_len;
  std::memset(out, 0, field_len);
  std::memcpy(out, s, n);
}

}  // namespace scanengine::exportimpl

#endif  // SCANENGINE_SRC_EXPORT_BYTE_IO_H
