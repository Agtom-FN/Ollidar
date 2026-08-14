#include "scanengine/color/frames_idx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "scanengine/core/log.h"
#include "scanengine/record/lscan.h"

namespace scanengine {
namespace color {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMod = "color";

// --- little-endian primitives ----------------------------------------------
//
// Hand-rolled rather than memcpy'd structs, for the same reason
// src/record/lscan.cpp does it: the format is defined in bytes, and a
// big-endian or differently-padding target must produce the identical file.

inline void put_u8(std::uint8_t* p, std::uint8_t v) { p[0] = v; }
inline void put_u16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}
inline void put_i64(std::uint8_t* p, std::int64_t v) {
  const std::uint64_t u = static_cast<std::uint64_t>(v);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF);
}
inline void put_f32(std::uint8_t* p, float v) {
  std::uint32_t bits;
  static_assert(sizeof(bits) == sizeof(v), "float is not 32 bits");
  std::memcpy(&bits, &v, 4);
  put_u32(p, bits);
}
inline void put_f64(std::uint8_t* p, double v) {
  std::uint64_t bits;
  static_assert(sizeof(bits) == sizeof(v), "double is not 64 bits");
  std::memcpy(&bits, &v, 8);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
}

inline std::uint8_t get_u8(const std::uint8_t* p) { return p[0]; }
inline std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
inline std::uint32_t get_u32(const std::uint8_t* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  return v;
}
inline std::int64_t get_i64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return static_cast<std::int64_t>(v);
}
inline float get_f32(const std::uint8_t* p) {
  const std::uint32_t bits = get_u32(p);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
inline double get_f64(const std::uint8_t* p) {
  std::uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) bits |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  double v;
  std::memcpy(&v, &bits, 8);
  return v;
}

const char* kFramesPrefix = "streams/frames/";

bool name_is_safe(const std::string& name) {
  if (name.empty() || name.size() > kKeyframeNameMaxBytes) return false;
  if (name[0] == '/' || name[0] == '\\') return false;
  if (name.size() >= 2 && name[1] == ':') return false;  // C:\...
  if (name.find('\\') != std::string::npos) return false;  // forward slashes only
  // Any ".." path component, not merely the substring — "a..b.jpg" is fine.
  std::size_t start = 0;
  while (start <= name.size()) {
    const std::size_t slash = name.find('/', start);
    const std::string part =
        name.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20) return false;
  }
  return true;
}

}  // namespace

// --- names ------------------------------------------------------------------

std::string keyframe_image_name(const Keyframe& kf) {
  const std::string& p = kf.image_path;
  const std::size_t n = std::strlen(kFramesPrefix);
  if (p.size() > n && p.compare(0, n, kFramesPrefix) == 0) return p.substr(n);
  return p;
}

std::string keyframe_image_path(const std::string& lscan_dir, const Keyframe& kf) {
  const std::string rel =
      kf.image_path.rfind("streams/", 0) == 0 ? kf.image_path : (kFramesPrefix + keyframe_image_name(kf));
  if (lscan_dir.empty()) return rel;
  return lscan_dir + "/" + rel;
}

// --- validation -------------------------------------------------------------

Status validate_keyframe(const Keyframe& kf) {
  const CameraIntrinsics& in = kf.intrinsics;
  if (in.width == 0 || in.height == 0) {
    return set_last_error(ScanError::kInvalidArgument, "color: keyframe has zero image size");
  }
  if (!(in.fx > 0.f) || !(in.fy > 0.f) || !std::isfinite(in.fx) || !std::isfinite(in.fy)) {
    return set_last_error(ScanError::kInvalidArgument, "color: keyframe fx/fy must be positive (%g, %g)",
                          static_cast<double>(in.fx), static_cast<double>(in.fy));
  }
  if (!std::isfinite(in.cx) || !std::isfinite(in.cy) || in.cx < 0.f || in.cy < 0.f ||
      in.cx > static_cast<float>(in.width) || in.cy > static_cast<float>(in.height)) {
    return set_last_error(ScanError::kInvalidArgument,
                          "color: keyframe principal point (%g, %g) outside %ux%u",
                          static_cast<double>(in.cx), static_cast<double>(in.cy), in.width, in.height);
  }
  for (int i = 0; i < 5; ++i) {
    if (!std::isfinite(in.distortion[i])) {
      return set_last_error(ScanError::kInvalidArgument, "color: keyframe distortion[%d] not finite", i);
    }
  }
  if (!std::isfinite(in.rolling_shutter_row_time_ns) || in.rolling_shutter_row_time_ns < 0.f) {
    return set_last_error(ScanError::kInvalidArgument,
                          "color: keyframe rolling_shutter_row_time_ns must be >= 0 (%g)",
                          static_cast<double>(in.rolling_shutter_row_time_ns));
  }
  double qn = 0.0;
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(kf.pose.orientation[i])) {
      return set_last_error(ScanError::kInvalidArgument, "color: keyframe orientation not finite");
    }
    qn += kf.pose.orientation[i] * kf.pose.orientation[i];
  }
  if (std::fabs(std::sqrt(qn) - 1.0) > 1e-6) {
    return set_last_error(ScanError::kInvalidArgument,
                          "color: keyframe orientation is not a unit quaternion (|q| = %g)",
                          std::sqrt(qn));
  }
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(kf.pose.position[i])) {
      return set_last_error(ScanError::kInvalidArgument, "color: keyframe position not finite");
    }
  }
  if (kf.exposure_duration_ns < 0) {
    return set_last_error(ScanError::kInvalidArgument, "color: keyframe exposure_duration_ns < 0");
  }
  if (!name_is_safe(keyframe_image_name(kf))) {
    return set_last_error(ScanError::kInvalidArgument, "color: unsafe keyframe image name '%s'",
                          kf.image_path.c_str());
  }
  return kOkStatus;
}

// --- codec ------------------------------------------------------------------

Status encode_keyframe_record(const Keyframe& kf, std::vector<std::uint8_t>* out) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: encode_keyframe_record(out == null)");
  }
  SCAN_TRY(validate_keyframe(kf));
  const std::string name = keyframe_image_name(kf);
  out->assign(kKeyframeRecordFixedBytes + name.size(), 0);
  std::uint8_t* p = out->data();

  put_u16(p + 0, kKeyframeRecordVersion);
  put_u16(p + 2, static_cast<std::uint16_t>(kKeyframeRecordFixedBytes));
  put_u32(p + 4, kf.flags | (kf.pose.tracking_lost != 0 ? kKeyframeFlagTrackingLost : 0u));
  put_i64(p + 8, kf.t_mono_ns);
  put_i64(p + 16, kf.exposure_duration_ns);
  for (int i = 0; i < 3; ++i) put_f64(p + 24 + 8 * i, kf.pose.position[i]);
  for (int i = 0; i < 4; ++i) put_f64(p + 48 + 8 * i, kf.pose.orientation[i]);
  put_f32(p + 80, kf.intrinsics.fx);
  put_f32(p + 84, kf.intrinsics.fy);
  put_f32(p + 88, kf.intrinsics.cx);
  put_f32(p + 92, kf.intrinsics.cy);
  for (int i = 0; i < 5; ++i) put_f32(p + 96 + 4 * i, kf.intrinsics.distortion[i]);
  put_u32(p + 116, kf.intrinsics.width);
  put_u32(p + 120, kf.intrinsics.height);
  put_f32(p + 124, kf.intrinsics.rolling_shutter_row_time_ns);
  put_f32(p + 128, kf.pose.position_sigma_m);
  put_f32(p + 132, kf.pose.orientation_sigma_deg);
  put_u8(p + 136, static_cast<std::uint8_t>(kf.pose.quality));
  put_u8(p + 137, kf.pose.tracking_lost);
  put_u8(p + 138, static_cast<std::uint8_t>(kf.pose.source));
  put_u8(p + 139, 0);
  put_f32(p + 140, kf.angular_rate_rad_s);
  put_f32(p + 144, kf.linear_speed_m_s);
  put_f32(p + 148, kf.iso);
  put_u32(p + 152, kf.image_bytes);
  put_u32(p + 156, static_cast<std::uint32_t>(name.size()));
  if (!name.empty()) {
    std::memcpy(p + kKeyframeRecordFixedBytes, name.data(), name.size());
  }
  return kOkStatus;
}

Status decode_keyframe_record(ByteSpan in, Keyframe* out, std::size_t* consumed) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: decode_keyframe_record(out == null)");
  }
  if (in.size() < 8) {
    return set_last_error(ScanError::kCorruptData, "color: keyframe record too short (%zu bytes)",
                          in.size());
  }
  const std::uint8_t* p = in.data();
  const std::uint16_t version = get_u16(p + 0);
  if (version > kKeyframeRecordVersion) {
    return set_last_error(ScanError::kVersionMismatch,
                          "color: frames.idx record version %u > supported %u", version,
                          kKeyframeRecordVersion);
  }
  const std::size_t fixed = get_u16(p + 2);
  if (fixed < kKeyframeRecordFixedBytes) {
    return set_last_error(ScanError::kCorruptData,
                          "color: keyframe record declares fixed_bytes %zu < %zu", fixed,
                          kKeyframeRecordFixedBytes);
  }
  if (in.size() < fixed) {
    return set_last_error(ScanError::kCorruptData,
                          "color: keyframe record truncated (%zu of %zu fixed bytes)", in.size(),
                          fixed);
  }
  const std::uint32_t name_len = get_u32(p + 156);
  if (name_len > kKeyframeNameMaxBytes) {
    return set_last_error(ScanError::kCorruptData, "color: keyframe image name %u bytes (max %u)",
                          name_len, kKeyframeNameMaxBytes);
  }
  if (in.size() < fixed + name_len) {
    return set_last_error(ScanError::kCorruptData,
                          "color: keyframe record name truncated (%zu bytes, need %zu)", in.size(),
                          fixed + name_len);
  }

  Keyframe kf;
  kf.flags = get_u32(p + 4);
  kf.t_mono_ns = get_i64(p + 8);
  kf.exposure_duration_ns = get_i64(p + 16);
  for (int i = 0; i < 3; ++i) kf.pose.position[i] = get_f64(p + 24 + 8 * i);
  for (int i = 0; i < 4; ++i) kf.pose.orientation[i] = get_f64(p + 48 + 8 * i);
  kf.intrinsics.fx = get_f32(p + 80);
  kf.intrinsics.fy = get_f32(p + 84);
  kf.intrinsics.cx = get_f32(p + 88);
  kf.intrinsics.cy = get_f32(p + 92);
  for (int i = 0; i < 5; ++i) kf.intrinsics.distortion[i] = get_f32(p + 96 + 4 * i);
  kf.intrinsics.width = get_u32(p + 116);
  kf.intrinsics.height = get_u32(p + 120);
  kf.intrinsics.rolling_shutter_row_time_ns = get_f32(p + 124);
  kf.pose.position_sigma_m = get_f32(p + 128);
  kf.pose.orientation_sigma_deg = get_f32(p + 132);
  kf.pose.quality = static_cast<PoseQuality>(get_u8(p + 136));
  kf.pose.tracking_lost = get_u8(p + 137);
  kf.pose.source = static_cast<StreamId>(get_u8(p + 138));
  kf.angular_rate_rad_s = get_f32(p + 140);
  kf.linear_speed_m_s = get_f32(p + 144);
  kf.iso = get_f32(p + 148);
  kf.image_bytes = get_u32(p + 152);
  kf.pose.t_mono_ns = kf.t_mono_ns;
  if (name_len > 0) {
    kf.image_path = kFramesPrefix + std::string(reinterpret_cast<const char*>(p + fixed), name_len);
  }
  *out = std::move(kf);
  if (consumed != nullptr) *consumed = fixed + name_len;
  return kOkStatus;
}

// --- writer -----------------------------------------------------------------

struct KeyframeIndexWriter::Impl {
  std::FILE* fp = nullptr;
  std::string path;
  bool opened = false;
  bool have_t_start = false;
  std::int64_t t_start_ns = 0;
  std::uint32_t records = 0;
  std::vector<std::uint8_t> scratch;

  ~Impl() {
    if (fp != nullptr) std::fclose(fp);
  }

  // The file is created on the FIRST record, not at open(), so that
  //   * a session with no keyframes leaves no frames.idx at all — which is
  //     what makes "no camera ⇒ kNotFound" mean what it says, and
  //   * `StreamFileHeader::t_start_mono_ns` is the first keyframe's stamp,
  //     byte-for-byte what lscan::FileRecordWriter would have written.
  Status ensure_open(std::int64_t t_mono_ns) {
    if (fp != nullptr) return kOkStatus;
    if (!opened) {
      return set_last_error(ScanError::kInvalidState, "color: KeyframeIndexWriter::add before open");
    }
    fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
      return set_last_error(ScanError::kFileError, "color: cannot create '%s'", path.c_str());
    }
    lscan::StreamFileHeader h;
    h.format_version = lscan::kFormatVersion;
    h.stream = StreamId::kCameraFrames;
    h.t_start_mono_ns = have_t_start ? t_start_ns : t_mono_ns;
    h.t_start_utc_ns = 0;
    std::uint8_t buf[lscan::kStreamHeaderBytes];
    lscan::encode_stream_header(h, buf);
    if (std::fwrite(buf, 1, sizeof(buf), fp) != sizeof(buf)) {
      std::fclose(fp);
      fp = nullptr;
      return set_last_error(ScanError::kFileError, "color: short write of frames.idx header '%s'",
                            path.c_str());
    }
    return kOkStatus;
  }
};

KeyframeIndexWriter::KeyframeIndexWriter() : impl_(new Impl) {}
KeyframeIndexWriter::~KeyframeIndexWriter() = default;

Status KeyframeIndexWriter::open(const std::string& lscan_dir) {
  std::error_code ec;
  fs::create_directories(fs::path(lscan_dir) / "streams" / "frames", ec);
  if (ec) {
    return set_last_error(ScanError::kFileError, "color: cannot create '%s/%s' (%s)",
                          lscan_dir.c_str(), lscan::kFramesDir, ec.message().c_str());
  }
  return open_file(lscan_dir + "/" + lscan::kFrameIndexFile, 0);
}

Status KeyframeIndexWriter::open_file(const std::string& idx_path, std::int64_t t_start_mono_ns) {
  SCAN_TRY(close());
  impl_->path = idx_path;
  impl_->opened = true;
  impl_->have_t_start = t_start_mono_ns != 0;
  impl_->t_start_ns = t_start_mono_ns;
  impl_->records = 0;
  return kOkStatus;
}

Status KeyframeIndexWriter::add(const Keyframe& kf) {
  SCAN_TRY(encode_keyframe_record(kf, &impl_->scratch));
  SCAN_TRY(impl_->ensure_open(kf.t_mono_ns));

  lscan::ChunkHeader h;
  h.payload_len = static_cast<std::uint32_t>(impl_->scratch.size());
  h.type = lscan::ChunkType::kCameraFrameIndex;
  h.flags = lscan::kFlagNone;
  h.t_mono_ns = kf.t_mono_ns;
  std::uint8_t hdr[lscan::kChunkHeaderBytes];
  lscan::encode_chunk_header(h, hdr);
  const std::uint32_t crc =
      lscan::chunk_crc(h, ByteSpan(impl_->scratch.data(), impl_->scratch.size()));
  std::uint8_t crc_bytes[4];
  put_u32(crc_bytes, crc);

  if (std::fwrite(hdr, 1, sizeof(hdr), impl_->fp) != sizeof(hdr) ||
      std::fwrite(impl_->scratch.data(), 1, impl_->scratch.size(), impl_->fp) !=
          impl_->scratch.size() ||
      std::fwrite(crc_bytes, 1, 4, impl_->fp) != 4) {
    return set_last_error(ScanError::kFileError, "color: short write of a frames.idx record");
  }
  ++impl_->records;
  return kOkStatus;
}

Status KeyframeIndexWriter::flush() {
  if (impl_->fp != nullptr && std::fflush(impl_->fp) != 0) {
    return set_last_error(ScanError::kFileError, "color: frames.idx flush failed");
  }
  return kOkStatus;
}

Status KeyframeIndexWriter::close() {
  impl_->opened = false;
  if (impl_->fp == nullptr) return kOkStatus;
  const int rc = std::fclose(impl_->fp);
  impl_->fp = nullptr;
  if (rc != 0) return set_last_error(ScanError::kFileError, "color: frames.idx close failed");
  return kOkStatus;
}

bool KeyframeIndexWriter::is_open() const { return impl_->opened; }
std::uint32_t KeyframeIndexWriter::records() const { return impl_->records; }

// --- reader -----------------------------------------------------------------

Status parse_frame_index(ByteSpan file_bytes, std::vector<Keyframe>* out, FrameIndexStats* stats) {
  if (out == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "color: parse_frame_index(out == null)");
  }
  FrameIndexStats local;
  FrameIndexStats& st = (stats != nullptr) ? *stats : local;
  st = FrameIndexStats{};
  out->clear();

  if (file_bytes.size() < lscan::kStreamHeaderBytes) {
    return set_last_error(ScanError::kCorruptData, "color: frames.idx shorter than its header");
  }
  lscan::StreamFileHeader sh;
  if (!lscan::decode_stream_header(ByteSpan(file_bytes.data(), lscan::kStreamHeaderBytes), &sh)) {
    return set_last_error(ScanError::kCorruptData, "color: frames.idx has no LSCN magic");
  }
  if (sh.format_version > lscan::kFormatVersion) {
    return set_last_error(ScanError::kVersionMismatch,
                          "color: frames.idx format version %u > supported %u", sh.format_version,
                          lscan::kFormatVersion);
  }

  std::size_t off = lscan::kStreamHeaderBytes;
  std::int64_t prev_t = 0;
  bool have_prev = false;
  while (off + lscan::kChunkOverheadBytes <= file_bytes.size()) {
    lscan::ChunkHeader h;
    if (!lscan::decode_chunk_header(ByteSpan(file_bytes.data() + off, lscan::kChunkHeaderBytes),
                                    &h)) {
      ++st.truncated_tail_chunks;  // oversized/garbled length
      break;
    }
    const std::size_t total = lscan::kChunkOverheadBytes + h.payload_len;
    if (off + total > file_bytes.size()) {
      ++st.truncated_tail_chunks;
      break;
    }
    const std::uint8_t* payload = file_bytes.data() + off + lscan::kChunkHeaderBytes;
    const std::uint32_t want = get_u32(payload + h.payload_len);
    if (lscan::chunk_crc(h, ByteSpan(payload, h.payload_len)) != want) {
      ++st.crc_mismatch_chunks;
      break;  // A1's truncated-tail rule: stop at the first chunk we cannot trust
    }
    off += total;

    if (h.type != lscan::ChunkType::kCameraFrameIndex) {
      ++st.foreign_chunks;  // forward compatibility: skipped by length
      continue;
    }
    Keyframe kf;
    const Status s = decode_keyframe_record(ByteSpan(payload, h.payload_len), &kf);
    if (!s.ok()) {
      ++st.malformed_records;
      continue;
    }
    if (kf.t_mono_ns != h.t_mono_ns) ++st.header_time_mismatches;
    if (!validate_keyframe(kf).ok()) {
      ++st.rejected_records;
      continue;
    }
    if (have_prev && kf.t_mono_ns < prev_t) ++st.out_of_order_records;
    prev_t = kf.t_mono_ns;
    have_prev = true;
    out->push_back(std::move(kf));
    ++st.records;
  }
  if (off < file_bytes.size() && st.truncated_tail_chunks == 0 && st.crc_mismatch_chunks == 0) {
    // Trailing bytes too short to hold even a chunk header.
    ++st.truncated_tail_chunks;
  }
  if (st.truncated_tail_chunks != 0 || st.crc_mismatch_chunks != 0) {
    SCAN_LOG_WARN(kMod, "frames.idx: %u records, %u truncated, %u crc-failed", st.records,
                  st.truncated_tail_chunks, st.crc_mismatch_chunks);
  }
  return kOkStatus;
}

Status read_frame_index_file(const std::string& idx_path, std::vector<Keyframe>* out,
                             FrameIndexStats* stats) {
  std::error_code ec;
  if (!fs::exists(idx_path, ec)) {
    return set_last_error(ScanError::kNotFound, "color: no keyframe index at '%s'",
                          idx_path.c_str());
  }
  std::ifstream f(idx_path, std::ios::binary);
  if (!f) {
    return set_last_error(ScanError::kFileError, "color: cannot open '%s'", idx_path.c_str());
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  if (f.bad()) {
    return set_last_error(ScanError::kIoError, "color: read error on '%s'", idx_path.c_str());
  }
  return parse_frame_index(ByteSpan(bytes.data(), bytes.size()), out, stats);
}

Status read_frame_index(const std::string& lscan_dir, std::vector<Keyframe>* out,
                        FrameIndexStats* stats) {
  return read_frame_index_file(lscan_dir + "/" + lscan::kFrameIndexFile, out, stats);
}

}  // namespace color
}  // namespace scanengine
