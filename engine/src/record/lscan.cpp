#include "scanengine/record/lscan.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <iomanip>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#define SCANENGINE_FILENO _fileno
#else
#include <unistd.h>
#define SCANENGINE_FILENO fileno
#endif

#include "scanengine/core/log.h"

namespace scanengine {
namespace lscan {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMod = "record";

struct Crc32Table {
  std::uint32_t t[256];
  Crc32Table() {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
  }
};

const Crc32Table& crc_table() {
  static const Crc32Table t;
  return t;
}

inline void put_u16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}
inline void put_i64(std::uint8_t* p, std::int64_t v) {
  const std::uint64_t u = static_cast<std::uint64_t>(v);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF);
}
inline std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
inline std::uint32_t get_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::int64_t get_i64(const std::uint8_t* p) {
  std::uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  return static_cast<std::int64_t>(u);
}

// ROUND 8: f32/f64 as little-endian bytes. Written through the same
// shift-and-mask route the integers take rather than a memcpy of the native
// representation, so the file is byte-identical on a big-endian host — the
// format contract says "all little-endian", and a chunk that only happened to
// be right because x86 and arm64 are little-endian is not a format.
// IEEE-754 layout itself is assumed (and asserted below), byte ORDER is not.
static_assert(sizeof(float) == 4, "lscan: f32 payloads assume IEEE-754 binary32");
static_assert(sizeof(double) == 8, "lscan: f64 payloads assume IEEE-754 binary64");

inline void put_f32(std::uint8_t* p, float v) {
  std::uint32_t u = 0;
  std::memcpy(&u, &v, 4);
  put_u32(p, u);
}
inline float get_f32(const std::uint8_t* p) {
  const std::uint32_t u = get_u32(p);
  float v = 0.f;
  std::memcpy(&v, &u, 4);
  return v;
}
inline void put_f64(std::uint8_t* p, double v) {
  std::uint64_t u = 0;
  std::memcpy(&u, &v, 8);
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF);
}
inline double get_f64(const std::uint8_t* p) {
  std::uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  double v = 0.0;
  std::memcpy(&v, &u, 8);
  return v;
}

}  // namespace

std::uint32_t crc32(ByteSpan data, std::uint32_t seed) {
  const Crc32Table& tab = crc_table();
  std::uint32_t c = seed ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < data.size(); ++i) {
    c = tab.t[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

void encode_chunk_header(const ChunkHeader& h, std::uint8_t* out) {
  put_u32(out + 0, h.payload_len);
  put_u16(out + 4, static_cast<std::uint16_t>(h.type));
  put_u16(out + 6, h.flags);
  put_i64(out + 8, h.t_mono_ns);
}

bool decode_chunk_header(ByteSpan in, ChunkHeader* out) {
  if (in.size() < kChunkHeaderBytes || out == nullptr) return false;
  const std::uint8_t* p = in.data();
  out->payload_len = get_u32(p + 0);
  out->type = static_cast<ChunkType>(get_u16(p + 4));
  out->flags = get_u16(p + 6);
  out->t_mono_ns = get_i64(p + 8);
  return out->payload_len <= kMaxChunkPayload;
}

void encode_stream_header(const StreamFileHeader& h, std::uint8_t* out) {
  std::memcpy(out, kMagic, 4);
  put_u16(out + 4, h.format_version);
  put_u16(out + 6, static_cast<std::uint16_t>(h.stream));
  put_i64(out + 8, h.t_start_mono_ns);
  put_i64(out + 16, h.t_start_utc_ns);
  std::memset(out + 24, 0, kStreamHeaderBytes - 24);  // reserved
}

bool decode_stream_header(ByteSpan in, StreamFileHeader* out) {
  if (in.size() < kStreamHeaderBytes || out == nullptr) return false;
  const std::uint8_t* p = in.data();
  if (std::memcmp(p, kMagic, 4) != 0) return false;
  out->format_version = get_u16(p + 4);
  out->stream = static_cast<StreamId>(get_u16(p + 6));
  out->t_start_mono_ns = get_i64(p + 8);
  out->t_start_utc_ns = get_i64(p + 16);
  return true;
}

std::uint32_t chunk_crc(const ChunkHeader& h, ByteSpan payload) {
  std::uint8_t hdr[kChunkHeaderBytes];
  encode_chunk_header(h, hdr);
  const std::uint32_t c = crc32(ByteSpan(hdr, kChunkHeaderBytes));
  // Continue the same CRC over the payload: seed carries state, so the
  // result equals crc32 over the concatenation.
  const Crc32Table& tab = crc_table();
  std::uint32_t acc = c ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < payload.size(); ++i) {
    acc = tab.t[(acc ^ payload[i]) & 0xFFu] ^ (acc >> 8);
  }
  return acc ^ 0xFFFFFFFFu;
}

// --- ROUND 8: kPoseAr payload codec -----------------------------------------

void encode_pose_chunk(const PoseChunkRecord& p, std::uint8_t* out) {
  for (int i = 0; i < 3; ++i) put_f64(out + 0 + 8 * i, p.position[i]);
  for (int i = 0; i < 4; ++i) put_f64(out + 24 + 8 * i, p.orientation[i]);
  put_f32(out + 56, p.position_sigma_m);
  put_f32(out + 60, p.orientation_sigma_deg);
  out[64] = p.source;
  out[65] = p.quality;
  out[66] = p.tracking_lost;
  out[67] = 0;  // reserved
}

bool decode_pose_chunk(ByteSpan in, PoseChunkRecord* out) {
  if (out == nullptr || in.size() < kPoseChunkPayloadBytes) return false;
  const std::uint8_t* p = in.data();
  for (int i = 0; i < 3; ++i) out->position[i] = get_f64(p + 0 + 8 * i);
  for (int i = 0; i < 4; ++i) out->orientation[i] = get_f64(p + 24 + 8 * i);
  out->position_sigma_m = get_f32(p + 56);
  out->orientation_sigma_deg = get_f32(p + 60);
  out->source = p[64];
  out->quality = p[65];
  out->tracking_lost = p[66];
  return true;
}

// --- ROUND 9: kPhoneImu payload codec ---------------------------------------

void encode_phone_imu_chunk(const PhoneImuChunkRecord& s, std::uint8_t* out) {
  for (int i = 0; i < 3; ++i) put_f32(out + 0 + 4 * i, s.gyro_rad_s[i]);
  for (int i = 0; i < 3; ++i) put_f32(out + 12 + 4 * i, s.accel_m_s2[i]);
}

bool decode_phone_imu_chunk(ByteSpan in, PhoneImuChunkRecord* out) {
  if (out == nullptr || in.size() < kPhoneImuChunkPayloadBytes) return false;
  const std::uint8_t* p = in.data();
  for (int i = 0; i < 3; ++i) out->gyro_rad_s[i] = get_f32(p + 0 + 4 * i);
  for (int i = 0; i < 3; ++i) out->accel_m_s2[i] = get_f32(p + 12 + 4 * i);
  return true;
}

StreamId stream_of(ChunkType t) {
  switch (t) {
    case ChunkType::kD6Raw: return StreamId::kLidarD6;
    case ChunkType::kMid360Points: return StreamId::kLidarMid360;
    case ChunkType::kMid360Imu: return StreamId::kImu;
    case ChunkType::kPoseAr: return StreamId::kPoseAr;
    case ChunkType::kGnssNmea:
    case ChunkType::kGnssRtcm: return StreamId::kGnss;
    case ChunkType::kCameraFrameIndex: return StreamId::kCameraFrames;
    // ROUND 8: the resolved world-frame cloud now HAS a stream. A7-post.md §8
    // item 2 listed `stream_of(kPointsXyzRgba) -> kUnknown` as the reason "the
    // post-processed cloud is never written back to disk"; ROUND 7 §9 item 3
    // hit the same wall from the D6 side. Mapping it to kSlamMap — the stream
    // id both A6's registered map and A8's pushbroom output already publish on
    // — is the whole fix, and it is additive: nothing wrote this chunk type
    // before, so no existing container changes meaning.
    case ChunkType::kPointsXyzRgba: return StreamId::kSlamMap;
    // ROUND 9: the phone's own IMU. NOT StreamId::kImu — see the note over
    // kPhoneImuStreamFile in lscan.h: kImu means "this is a Mid-360 project" to
    // two offline pipelines, and a phone sample must not claim that.
    case ChunkType::kPhoneImu: return StreamId::kImuPhone;
    case ChunkType::kDeviceInfo:
    case ChunkType::kMarker:
    case ChunkType::kSessionNote:
    case ChunkType::kNone: return StreamId::kUnknown;
  }
  return StreamId::kUnknown;
}

const char* stream_file_of(StreamId s) {
  switch (s) {
    case StreamId::kLidarD6:
    case StreamId::kLidarMid360: return kLidarStreamFile;
    case StreamId::kImu: return kImuStreamFile;
    case StreamId::kPoseAr:
    case StreamId::kPoseFused: return kPoseArStreamFile;
    case StreamId::kGnss: return kGnssStreamFile;
    case StreamId::kCameraFrames: return kFrameIndexFile;
    // ROUND 8: the SLAM map gets its OWN file. It used to share lidar.bin on
    // the reasoning that an engine-internal stream should land beside what it
    // derives from — fine while nothing recorded it, wrong the moment
    // something does: ReplaySource walks lidar.bin looking for kD6Raw, and
    // interleaving a 40k-points/s vertex stream into it would make every
    // replay read and CRC tens of megabytes it immediately discards.
    case StreamId::kSlamMap: return kMapStreamFile;
    case StreamId::kPoseLio: return kPoseArStreamFile;
    // ROUND 9: its own file, not imu.bin (the Mid-360's) and not poses_ar.bin
    // (which a kPoseAr replay walks chunk by chunk — a 400 Hz stream folded in
    // there would make every pose replay read and CRC 13x its own volume).
    case StreamId::kImuPhone: return kPhoneImuStreamFile;
    case StreamId::kUnknown: return kLidarStreamFile;
  }
  return kLidarStreamFile;
}

// --- NullRecordWriter -------------------------------------------------------

Status NullRecordWriter::open(const std::string& lscan_dir) {
  if (open_) return set_last_error(ScanError::kInvalidState, "record writer already open");
  path_ = lscan_dir;
  stats_ = RecordStats{};
  open_ = true;
  SCAN_LOG_INFO(kMod, "null writer open at '%s' (A5 replaces this with the real container)",
                path_.c_str());
  return kOkStatus;
}

Status NullRecordWriter::write_chunk(ChunkType type, std::int64_t t_mono_ns, ByteSpan payload,
                                     std::uint16_t flags) {
  if (!open_) return set_last_error(ScanError::kInvalidState, "record writer not open");
  if (payload.size() > kMaxChunkPayload) {
    return set_last_error(ScanError::kInvalidArgument, "chunk payload %zu exceeds %u bytes",
                          payload.size(), kMaxChunkPayload);
  }
  if (type == ChunkType::kNone) {
    return set_last_error(ScanError::kInvalidArgument, "chunk type kNone is not writable");
  }
  (void)flags;
  ++stats_.chunks_written;
  stats_.bytes_written += payload.size() + kChunkOverheadBytes;
  if (stats_.t_first_ns == 0) stats_.t_first_ns = t_mono_ns;
  stats_.t_last_ns = t_mono_ns;
  return kOkStatus;
}

Status NullRecordWriter::flush() {
  if (!open_) return set_last_error(ScanError::kInvalidState, "record writer not open");
  ++stats_.flushes;
  return kOkStatus;
}

Status NullRecordWriter::close() {
  open_ = false;
  return kOkStatus;
}

// =============================================================================
// FileRecordWriter / FileRecordReader (A5)
// =============================================================================

namespace {

std::int64_t wall_clock_ns_now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void fsync_file(std::FILE* fp) {
  if (fp == nullptr) return;
  std::fflush(fp);
#if defined(_WIN32)
  _commit(SCANENGINE_FILENO(fp));
#else
  fsync(SCANENGINE_FILENO(fp));
#endif
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// A structural check, not a schema validator: are the braces/brackets
// balanced and every string terminated? That is exactly what distinguishes
// "empty file", "truncated mid-write JSON" and "garbage" from "well-formed
// enough to read" without pulling in a JSON library (Tech Spec workstream A
// stays dependency-light -- see vcpkg.json's onboarding-order note).
bool looks_like_valid_json_object(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= s.size() || s[i] != '{') return false;
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (in_string) {
      if (escape) { escape = false; continue; }
      if (c == '\\') { escape = true; continue; }
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') { in_string = true; continue; }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (depth < 0) return false;
    }
  }
  return depth == 0 && !in_string && s.find("\"schemaVersion\"") != std::string::npos;
}

enum class ChunkReadStatus { kOk, kCleanEnd, kTruncatedTail, kCrcMismatch };

// Reads one framed chunk from `fp` at its current position. Never throws,
// never reads past what fread() actually returns -- a short read anywhere in
// the frame (header, payload or trailer) is reported as kTruncatedTail, not
// as a partially-filled ChunkHeader/payload.
ChunkReadStatus read_one_chunk(std::FILE* fp, ChunkHeader* out_h,
                                std::vector<std::uint8_t>* out_payload) {
  std::uint8_t hdrbuf[kChunkHeaderBytes];
  const std::size_t hn = std::fread(hdrbuf, 1, kChunkHeaderBytes, fp);
  if (hn == 0) return ChunkReadStatus::kCleanEnd;
  if (hn < kChunkHeaderBytes) return ChunkReadStatus::kTruncatedTail;

  ChunkHeader h;
  if (!decode_chunk_header(ByteSpan(hdrbuf, kChunkHeaderBytes), &h)) {
    // Oversized/garbled length field: framing broke here exactly like a
    // truncated tail does -- there is no safe resync point past it.
    return ChunkReadStatus::kTruncatedTail;
  }

  std::vector<std::uint8_t> payload(h.payload_len);
  if (h.payload_len > 0) {
    const std::size_t pn = std::fread(payload.data(), 1, h.payload_len, fp);
    if (pn != h.payload_len) return ChunkReadStatus::kTruncatedTail;
  }

  std::uint8_t trailer[kChunkTrailerBytes];
  const std::size_t tn = std::fread(trailer, 1, kChunkTrailerBytes, fp);
  if (tn != kChunkTrailerBytes) return ChunkReadStatus::kTruncatedTail;

  const std::uint32_t stored_crc = get_u32(trailer);
  const std::uint32_t computed_crc = chunk_crc(h, ByteSpan(payload.data(), payload.size()));
  if (stored_crc != computed_crc) return ChunkReadStatus::kCrcMismatch;

  *out_h = h;
  *out_payload = std::move(payload);
  return ChunkReadStatus::kOk;
}

}  // namespace

// --- FileRecordWriter --------------------------------------------------------

struct FileRecordWriter::Impl {
  struct StreamFile {
    std::FILE* fp = nullptr;
    StreamId stream = StreamId::kUnknown;
    std::size_t bytes_since_flush = 0;
    std::int64_t last_flush_mono_ns = 0;
  };
  struct SensorInfo {
    std::string id, kind, model;
  };
  // INT-34, additive: see FileRecordWriter::add_clock_offset() in lscan.h.
  struct ClockOffsetInfo {
    std::string bracket;
    std::int64_t camera_to_engine_ns = 0;
    double sigma_ns = 0.0;
  };

  bool open_ = false;
  std::string path_;
  RecordStats stats_{};
  std::string profile_ = "quickscan";
  std::vector<SensorInfo> sensors_;
  std::vector<ClockOffsetInfo> clock_offsets_;  // INT-34
  // ROUND 8: row-major phone_from_lidar, or "not set" — see
  // FileRecordWriter::set_mount_calibration().
  bool have_mount_ = false;
  double mount_phone_from_lidar_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  // ROUND 9: see FileRecordWriter::set_imu_calibration().
  bool have_imu_calib_ = false;
  double imu_camera_from_imu_[4] = {0, 0, 0, 1};
  std::int64_t created_at_utc_ns_ = 0;
  std::map<StreamId, StreamFile> streams_;

  ~Impl() {
    for (auto& kv : streams_) {
      if (kv.second.fp != nullptr) std::fclose(kv.second.fp);
    }
  }

  Status ensure_stream_open(StreamId stream, std::int64_t t_mono_ns) {
    if (streams_.find(stream) != streams_.end()) return kOkStatus;

    const std::string file_path = path_ + "/" + stream_file_of(stream);
    std::FILE* fp = std::fopen(file_path.c_str(), "wb");
    if (fp == nullptr) {
      return set_last_error(ScanError::kFileError, "record: cannot create stream file '%s'",
                            file_path.c_str());
    }
    StreamFileHeader h;
    h.format_version = kFormatVersion;
    h.stream = stream;
    h.t_start_mono_ns = t_mono_ns;
    h.t_start_utc_ns = wall_clock_ns_now();
    std::uint8_t buf[kStreamHeaderBytes];
    encode_stream_header(h, buf);
    if (std::fwrite(buf, 1, kStreamHeaderBytes, fp) != kStreamHeaderBytes) {
      std::fclose(fp);
      return set_last_error(ScanError::kFileError, "record: short write of stream header '%s'",
                            file_path.c_str());
    }

    StreamFile sf;
    sf.fp = fp;
    sf.stream = stream;
    sf.last_flush_mono_ns = t_mono_ns;
    streams_.emplace(stream, sf);
    SCAN_LOG_INFO(kMod, "opened stream file '%s' (stream=%s)", file_path.c_str(),
                  to_string(stream));
    return kOkStatus;
  }

  Status maybe_auto_flush(StreamFile& sf, std::int64_t t_mono_ns) {
    const bool by_bytes = sf.bytes_since_flush >= FileRecordWriter::kAutoFlushBytes;
    const bool by_time =
        (t_mono_ns - sf.last_flush_mono_ns) >= FileRecordWriter::kAutoFlushIntervalNs;
    if (!by_bytes && !by_time) return kOkStatus;
    fsync_file(sf.fp);
    sf.bytes_since_flush = 0;
    sf.last_flush_mono_ns = t_mono_ns;
    ++stats_.flushes;
    return kOkStatus;
  }

  Status write_manifest(bool sealed) const {
    std::ostringstream j;
    j << "{\n";
    j << "  \"schemaVersion\": 1,\n";
    j << "  \"formatVersion\": " << kFormatVersion << ",\n";
    j << "  \"engineVersion\": \"" << json_escape(SCANENGINE_VERSION) << "\",\n";
    j << "  \"createdAtUtcNs\": " << created_at_utc_ns_ << ",\n";
    j << "  \"sealed\": " << (sealed ? "true" : "false") << ",\n";
    if (sealed) j << "  \"sealedAtUtcNs\": " << wall_clock_ns_now() << ",\n";
    j << "  \"profile\": \"" << json_escape(profile_) << "\",\n";
    j << "  \"sensors\": [";
    for (std::size_t i = 0; i < sensors_.size(); ++i) {
      if (i != 0) j << ", ";
      const auto& s = sensors_[i];
      j << "{\"id\": \"" << json_escape(s.id) << "\", \"kind\": \"" << json_escape(s.kind)
        << "\", \"model\": \"" << json_escape(s.model) << "\"}";
    }
    j << "],\n";
    // A5 reserved these keys so consumers could rely on their presence from
    // day one. ROUND 8 fills `mountCalibration` in (see
    // FileRecordWriter::set_mount_calibration() — without it a D6 capture is
    // not self-contained and cannot be re-resolved off this directory alone);
    // A10's CRS is still a placeholder.
    if (have_mount_) {
      j << "  \"mountCalibration\": {\"phoneFromLidar\": [";
      for (int i = 0; i < 16; ++i) {
        if (i != 0) j << ", ";
        // Round-trip precision: a 4x4 that decodes to a slightly different
        // matrix than the one that was recorded resolves the cloud into a
        // slightly different room, and "slightly" is what this whole round is
        // about.
        j << std::setprecision(17) << mount_phone_from_lidar_[i];
      }
      j << "]},\n";
    } else {
      j << "  \"mountCalibration\": null,\n";
    }
    // ROUND 9: the phone-IMU extrinsic, for the same self-containment reason
    // as mountCalibration above — a kPhoneImu stream without it re-resolves
    // into a quietly distorted trajectory rather than failing loudly.
    if (have_imu_calib_) {
      j << "  \"imuCalibration\": {\"cameraFromImu\": [";
      for (int i = 0; i < 4; ++i) {
        if (i != 0) j << ", ";
        j << std::setprecision(17) << imu_camera_from_imu_[i];
      }
      j << "]},\n";
    } else {
      j << "  \"imuCalibration\": null,\n";
    }
    j << "  \"crs\": null,\n";
    // --- INT-34, additive: A11 §8.4's per-bracket camera clock offset ------
    // t_engine_ns = t_camera_ns + cameraToEngineNs. Always emitted, `{}` when
    // nothing was set, so a consumer can rely on the key. See
    // FileRecordWriter::add_clock_offset() in record/lscan.h for the whole
    // rationale and the note about this being INT-34's one edit in A5's file.
    j << "  \"clockOffsets\": {";
    for (std::size_t i = 0; i < clock_offsets_.size(); ++i) {
      if (i != 0) j << ", ";
      const auto& co = clock_offsets_[i];
      j << "\"" << json_escape(co.bracket) << "\": {\"cameraToEngineNs\": "
        << co.camera_to_engine_ns << ", \"sigmaNs\": " << co.sigma_ns << "}";
    }
    j << "},\n";
    j << "  \"streams\": {";
    bool first = true;
    for (const auto& kv : streams_) {
      if (!first) j << ", ";
      first = false;
      j << "\"" << json_escape(stream_file_of(kv.first)) << "\": {\"stream\": "
        << static_cast<int>(kv.first) << "}";
    }
    j << "}\n";
    j << "}\n";

    const std::string final_path = path_ + "/" + kManifestFile;
    const std::string tmp_path = final_path + ".tmp";
    {
      std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
      if (!out) {
        return set_last_error(ScanError::kFileError, "record: cannot write manifest '%s'",
                              tmp_path.c_str());
      }
      const std::string text = j.str();
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      out.flush();
      if (!out) {
        return set_last_error(ScanError::kFileError, "record: short write of manifest '%s'",
                              tmp_path.c_str());
      }
    }
    std::error_code ec;
    fs::rename(tmp_path, final_path, ec);
    if (ec) {
      // Cross-filesystem or Windows-existing-destination edge case: fall
      // back to remove+rename. Best-effort -- the manifest is metadata, not
      // the crash-safety-critical path (the chunk streams are).
      fs::remove(final_path, ec);
      fs::rename(tmp_path, final_path, ec);
      if (ec) {
        return set_last_error(ScanError::kFileError, "record: cannot install manifest '%s': %s",
                              final_path.c_str(), ec.message().c_str());
      }
    }
    return kOkStatus;
  }
};

FileRecordWriter::FileRecordWriter() : impl_(std::make_unique<Impl>()) {}
FileRecordWriter::~FileRecordWriter() {
  if (impl_ && impl_->open_) (void)close();
}

Status FileRecordWriter::open(const std::string& lscan_dir) {
  if (impl_->open_) return set_last_error(ScanError::kInvalidState, "record writer already open");

  std::error_code ec;
  fs::create_directories(lscan_dir, ec);
  if (ec) {
    return set_last_error(ScanError::kFileError, "record: cannot create '%s': %s",
                          lscan_dir.c_str(), ec.message().c_str());
  }
  for (const char* sub : {kStreamsDir, kFramesDir, kProcessedDir, kMergedDir, kExportsDir}) {
    fs::create_directories(lscan_dir + "/" + sub, ec);
    if (ec) {
      return set_last_error(ScanError::kFileError, "record: cannot create '%s/%s': %s",
                            lscan_dir.c_str(), sub, ec.message().c_str());
    }
  }

  impl_->path_ = lscan_dir;
  impl_->stats_ = RecordStats{};
  impl_->streams_.clear();
  impl_->created_at_utc_ns_ = wall_clock_ns_now();
  impl_->open_ = true;

  const Status ms = impl_->write_manifest(/*sealed=*/false);
  if (!ms.ok()) {
    impl_->open_ = false;
    return ms;
  }

  SCAN_LOG_INFO(kMod, "file writer open at '%s'", lscan_dir.c_str());
  return kOkStatus;
}

Status FileRecordWriter::write_chunk(ChunkType type, std::int64_t t_mono_ns, ByteSpan payload,
                                     std::uint16_t flags) {
  if (!impl_->open_) return set_last_error(ScanError::kInvalidState, "record writer not open");
  if (payload.size() > kMaxChunkPayload) {
    return set_last_error(ScanError::kInvalidArgument, "chunk payload %zu exceeds %u bytes",
                          payload.size(), kMaxChunkPayload);
  }
  if (type == ChunkType::kNone) {
    return set_last_error(ScanError::kInvalidArgument, "chunk type kNone is not writable");
  }

  const StreamId stream = stream_of(type);
  SCAN_TRY(impl_->ensure_stream_open(stream, t_mono_ns));
  auto& sf = impl_->streams_.at(stream);

  ChunkHeader h;
  h.payload_len = static_cast<std::uint32_t>(payload.size());
  h.type = type;
  h.flags = flags;
  h.t_mono_ns = t_mono_ns;

  std::uint8_t hdr[kChunkHeaderBytes];
  encode_chunk_header(h, hdr);
  const std::uint32_t crc = chunk_crc(h, payload);
  std::uint8_t trailer[kChunkTrailerBytes];
  put_u32(trailer, crc);

  std::size_t written = std::fwrite(hdr, 1, kChunkHeaderBytes, sf.fp);
  if (payload.size() > 0) written += std::fwrite(payload.data(), 1, payload.size(), sf.fp);
  written += std::fwrite(trailer, 1, kChunkTrailerBytes, sf.fp);

  const std::size_t expected = kChunkHeaderBytes + payload.size() + kChunkTrailerBytes;
  if (written != expected || std::ferror(sf.fp)) {
    return set_last_error(ScanError::kFileError, "record: short write on stream '%s'",
                          to_string(stream));
  }

  sf.bytes_since_flush += expected;
  ++impl_->stats_.chunks_written;
  impl_->stats_.bytes_written += expected;
  if (impl_->stats_.t_first_ns == 0) impl_->stats_.t_first_ns = t_mono_ns;
  impl_->stats_.t_last_ns = t_mono_ns;

  return impl_->maybe_auto_flush(sf, t_mono_ns);
}

Status FileRecordWriter::flush() {
  if (!impl_->open_) return set_last_error(ScanError::kInvalidState, "record writer not open");
  for (auto& kv : impl_->streams_) {
    fsync_file(kv.second.fp);
    kv.second.bytes_since_flush = 0;
  }
  ++impl_->stats_.flushes;
  return kOkStatus;
}

Status FileRecordWriter::close() {
  if (!impl_->open_) return kOkStatus;  // idempotent, matches NullRecordWriter
  for (auto& kv : impl_->streams_) {
    fsync_file(kv.second.fp);
    if (kv.second.fp != nullptr) std::fclose(kv.second.fp);
    kv.second.fp = nullptr;
  }
  const Status ms = impl_->write_manifest(/*sealed=*/true);
  impl_->open_ = false;
  if (!ms.ok()) return ms;
  return kOkStatus;
}

bool FileRecordWriter::is_open() const { return impl_->open_; }
RecordStats FileRecordWriter::stats() const { return impl_->stats_; }
const std::string& FileRecordWriter::path() const { return impl_->path_; }

void FileRecordWriter::set_profile(const std::string& profile) { impl_->profile_ = profile; }

// ROUND 14 — see lscan.h. Everything here is a property of ONE container.
void FileRecordWriter::reset_metadata() {
  impl_->profile_ = "quickscan";
  impl_->sensors_.clear();
  impl_->clock_offsets_.clear();
  impl_->have_mount_ = false;
  impl_->have_imu_calib_ = false;
}

void FileRecordWriter::add_sensor(const std::string& id, const std::string& kind,
                                  const std::string& model) {
  impl_->sensors_.push_back(Impl::SensorInfo{id, kind, model});
}

// INT-34, additive — the only entry point added to A5's writer. See lscan.h.
void FileRecordWriter::add_clock_offset(const std::string& bracket,
                                        std::int64_t camera_to_engine_ns, double sigma_ns) {
  const std::string key = bracket.empty() ? std::string("default") : bracket;
  for (auto& co : impl_->clock_offsets_) {
    if (co.bracket == key) {
      co.camera_to_engine_ns = camera_to_engine_ns;
      co.sigma_ns = sigma_ns;
      return;
    }
  }
  impl_->clock_offsets_.push_back(Impl::ClockOffsetInfo{key, camera_to_engine_ns, sigma_ns});
}

// ROUND 8, additive: see FileRecordWriter::set_mount_calibration() in lscan.h.
void FileRecordWriter::set_mount_calibration(const double phone_from_lidar[16]) {
  if (phone_from_lidar == nullptr) return;
  for (int i = 0; i < 16; ++i) impl_->mount_phone_from_lidar_[i] = phone_from_lidar[i];
  impl_->have_mount_ = true;
}

// ROUND 9, additive: see FileRecordWriter::set_imu_calibration() in lscan.h.
void FileRecordWriter::set_imu_calibration(const double camera_from_imu[4]) {
  if (camera_from_imu == nullptr) return;
  for (int i = 0; i < 4; ++i) impl_->imu_camera_from_imu_[i] = camera_from_imu[i];
  impl_->have_imu_calib_ = true;
}

// --- FileRecordReader ---------------------------------------------------------

struct FileRecordReader::Impl {
  struct StreamCursor {
    StreamId stream = StreamId::kUnknown;
    std::string file_path;
    std::FILE* fp = nullptr;
    std::uint64_t valid_chunk_count = 0;  // chunks proven good by the validation pass
    std::uint64_t chunks_consumed = 0;    // read (peeked) so far during iteration
    bool have_peek = false;
    ChunkHeader peek_header{};
    std::vector<std::uint8_t> peek_payload;
  };

  bool open_ = false;
  std::string path_;
  bool manifest_present_ = false;
  bool manifest_ok_ = false;
  std::string manifest_raw_;
  std::vector<StreamSummary> summaries_;
  ReaderWarnings warnings_{};
  std::vector<StreamCursor> cursors_;
  StreamId last_stream_ = StreamId::kUnknown;

  ~Impl() { close_files(); }

  void close_files() {
    for (auto& c : cursors_) {
      if (c.fp != nullptr) std::fclose(c.fp);
      c.fp = nullptr;
    }
  }

  // Opens one candidate stream file (if present), reads+checks its 32-byte
  // header, then walks every chunk once to build the StreamSummary and to
  // find how many leading chunks are trustworthy. Returns false (with
  // *hard_fail set) only for a forward-incompatible format_version -- every
  // other problem degrades to a warning, per the crash-safety contract.
  bool load_stream_file(const std::string& file_path, bool* hard_fail) {
    std::FILE* fp = std::fopen(file_path.c_str(), "rb");
    if (fp == nullptr) return true;  // not present: perfectly normal

    std::uint8_t hdrbuf[kStreamHeaderBytes];
    const std::size_t hn = std::fread(hdrbuf, 1, kStreamHeaderBytes, fp);
    StreamFileHeader sh{};
    if (hn != kStreamHeaderBytes || !decode_stream_header(ByteSpan(hdrbuf, hn), &sh)) {
      ++warnings_.unreadable_streams;
      SCAN_LOG_WARN(kMod, "record: '%s' has no valid stream header; skipping", file_path.c_str());
      std::fclose(fp);
      return true;
    }
    if (sh.format_version > kFormatVersion) {
      SCAN_LOG_ERROR(kMod, "record: '%s' is format_version %u, this reader supports up to %u",
                     file_path.c_str(), sh.format_version, kFormatVersion);
      std::fclose(fp);
      *hard_fail = true;
      return false;
    }

    // Validation pass: walk every chunk once, stop at the first bad one.
    StreamSummary summary;
    summary.stream = sh.stream;
    std::uint64_t good = 0;
    for (;;) {
      ChunkHeader h;
      std::vector<std::uint8_t> payload;
      const ChunkReadStatus st = read_one_chunk(fp, &h, &payload);
      if (st == ChunkReadStatus::kCleanEnd) break;
      if (st == ChunkReadStatus::kTruncatedTail) {
        ++warnings_.truncated_tail_chunks;
        SCAN_LOG_WARN(kMod, "record: '%s' has a truncated tail after %llu good chunk(s)",
                      file_path.c_str(), static_cast<unsigned long long>(good));
        break;
      }
      if (st == ChunkReadStatus::kCrcMismatch) {
        ++warnings_.crc_mismatch_chunks;
        SCAN_LOG_WARN(kMod, "record: '%s' failed a chunk CRC after %llu good chunk(s)",
                      file_path.c_str(), static_cast<unsigned long long>(good));
        break;
      }
      ++good;
      ++summary.chunk_count;
      summary.bytes += h.payload_len;
      if (summary.chunk_count == 1) summary.t_first_ns = h.t_mono_ns;
      summary.t_last_ns = h.t_mono_ns;
    }

    if (good > 0) summaries_.push_back(summary);

    // Rewind to the start of chunk data for the iteration pass.
    std::fseek(fp, static_cast<long>(kStreamHeaderBytes), SEEK_SET);

    StreamCursor c;
    c.stream = sh.stream;
    c.file_path = file_path;
    c.fp = fp;
    c.valid_chunk_count = good;
    cursors_.push_back(std::move(c));
    return true;
  }

  void fill_peek(StreamCursor& c) {
    if (c.have_peek) return;
    if (c.chunks_consumed >= c.valid_chunk_count) return;  // exhausted
    ChunkHeader h;
    std::vector<std::uint8_t> payload;
    const ChunkReadStatus st = read_one_chunk(c.fp, &h, &payload);
    if (st != ChunkReadStatus::kOk) {
      // Should not happen: the validation pass already bounded this cursor
      // to `valid_chunk_count` good chunks. Defensive, not load-bearing.
      c.chunks_consumed = c.valid_chunk_count;
      return;
    }
    c.peek_header = h;
    c.peek_payload = std::move(payload);
    c.have_peek = true;
    ++c.chunks_consumed;
  }
};

FileRecordReader::FileRecordReader() : impl_(std::make_unique<Impl>()) {}
FileRecordReader::~FileRecordReader() {
  if (impl_ && impl_->open_) (void)close();
}

Status FileRecordReader::open(const std::string& lscan_dir) {
  if (impl_->open_) return set_last_error(ScanError::kInvalidState, "reader already open");

  std::error_code ec;
  if (!fs::exists(lscan_dir, ec) || !fs::is_directory(lscan_dir, ec)) {
    return set_last_error(ScanError::kFileError, "record: '%s' is not a directory",
                          lscan_dir.c_str());
  }
  impl_->path_ = lscan_dir;
  impl_->manifest_present_ = false;
  impl_->manifest_ok_ = false;
  impl_->summaries_.clear();
  impl_->warnings_ = ReaderWarnings{};
  impl_->cursors_.clear();

  const std::string manifest_path = lscan_dir + "/" + kManifestFile;
  {
    std::ifstream mf(manifest_path, std::ios::binary);
    if (mf) {
      std::ostringstream ss;
      ss << mf.rdbuf();
      impl_->manifest_raw_ = ss.str();
      impl_->manifest_present_ = true;
      impl_->manifest_ok_ = looks_like_valid_json_object(impl_->manifest_raw_);
      if (!impl_->manifest_ok_) {
        SCAN_LOG_WARN(kMod, "record: manifest.json at '%s' is present but not well-formed",
                      manifest_path.c_str());
      }
    } else {
      SCAN_LOG_WARN(kMod, "record: no manifest.json at '%s'; reading streams only",
                    manifest_path.c_str());
    }
  }

  // Distinct on-disk paths only: several ChunkType/StreamId values can share
  // one physical file (e.g. kLidarD6 and kLidarMid360 both live in
  // lidar.bin) -- stream_file_of() already encodes that mapping.
  static const char* const kCandidates[] = {kLidarStreamFile, kImuStreamFile, kPoseArStreamFile,
                                            kGnssStreamFile, kFrameIndexFile,
                                            // ROUND 8: the resolved cloud, StreamId::kSlamMap.
                                            kMapStreamFile,
                                            // ROUND 9: the phone's own IMU,
                                            // StreamId::kImuPhone. A stream that is
                                            // written but not listed here is written
                                            // and never read back.
                                            kPhoneImuStreamFile};
  bool hard_fail = false;
  for (const char* rel : kCandidates) {
    if (!impl_->load_stream_file(lscan_dir + "/" + rel, &hard_fail)) {
      impl_->close_files();
      return set_last_error(ScanError::kVersionMismatch,
                            "record: '%s/%s' is a newer .lscan format than this reader supports",
                            lscan_dir.c_str(), rel);
    }
  }

  impl_->open_ = true;
  return kOkStatus;
}

Status FileRecordReader::next_chunk(ChunkHeader* header, std::vector<std::uint8_t>* payload) {
  if (!impl_->open_) return set_last_error(ScanError::kInvalidState, "reader not open");
  if (header == nullptr || payload == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "record: null out-parameter");
  }

  int best = -1;
  for (std::size_t i = 0; i < impl_->cursors_.size(); ++i) {
    auto& c = impl_->cursors_[i];
    impl_->fill_peek(c);
    if (!c.have_peek) continue;
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    auto& b = impl_->cursors_[static_cast<std::size_t>(best)];
    if (c.peek_header.t_mono_ns < b.peek_header.t_mono_ns ||
        (c.peek_header.t_mono_ns == b.peek_header.t_mono_ns && c.stream < b.stream)) {
      best = static_cast<int>(i);
    }
  }
  if (best < 0) return ScanError::kAgain;

  auto& c = impl_->cursors_[static_cast<std::size_t>(best)];
  *header = c.peek_header;
  *payload = std::move(c.peek_payload);
  impl_->last_stream_ = c.stream;
  c.have_peek = false;
  c.peek_payload.clear();
  return kOkStatus;
}

Status FileRecordReader::seek(std::int64_t t_mono_ns) {
  if (!impl_->open_) return set_last_error(ScanError::kInvalidState, "reader not open");
  for (auto& c : impl_->cursors_) {
    std::fseek(c.fp, static_cast<long>(kStreamHeaderBytes), SEEK_SET);
    c.chunks_consumed = 0;
    c.have_peek = false;
    c.peek_payload.clear();
    impl_->fill_peek(c);
    while (c.have_peek && c.peek_header.t_mono_ns < t_mono_ns) {
      c.have_peek = false;
      c.peek_payload.clear();
      impl_->fill_peek(c);
    }
  }
  return kOkStatus;
}

Status FileRecordReader::close() {
  impl_->close_files();
  impl_->open_ = false;
  return kOkStatus;
}

StreamId FileRecordReader::last_stream() const { return impl_->last_stream_; }
const std::vector<StreamSummary>& FileRecordReader::stream_summaries() const {
  return impl_->summaries_;
}
const ReaderWarnings& FileRecordReader::warnings() const { return impl_->warnings_; }
bool FileRecordReader::manifest_present() const { return impl_->manifest_present_; }
bool FileRecordReader::manifest_ok() const { return impl_->manifest_ok_; }
const std::string& FileRecordReader::manifest_raw() const { return impl_->manifest_raw_; }

}  // namespace lscan
}  // namespace scanengine
