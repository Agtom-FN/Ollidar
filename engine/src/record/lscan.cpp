#include "scanengine/record/lscan.h"

#include <cstring>

#include "scanengine/core/log.h"

namespace scanengine {
namespace lscan {
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

StreamId stream_of(ChunkType t) {
  switch (t) {
    case ChunkType::kD6Raw: return StreamId::kLidarD6;
    case ChunkType::kMid360Points: return StreamId::kLidarMid360;
    case ChunkType::kMid360Imu: return StreamId::kImu;
    case ChunkType::kPoseAr: return StreamId::kPoseAr;
    case ChunkType::kGnssNmea:
    case ChunkType::kGnssRtcm: return StreamId::kGnss;
    case ChunkType::kCameraFrameIndex: return StreamId::kCameraFrames;
    case ChunkType::kDeviceInfo:
    case ChunkType::kMarker:
    case ChunkType::kSessionNote:
    case ChunkType::kPointsXyzRgba:
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

}  // namespace lscan
}  // namespace scanengine
