// lscan.h — the `.lscan` container format (Tech Spec §3.11).
//
// This header is the FORMAT CONTRACT. A1 fixes the framing, the magic, the
// version and the chunk-type numbering — because the Android app, the Qt
// app, the cloud worker and the replay harness must all agree on them
// before any of those exist. A5 implements the crash-safe writer and the
// reader/replay harness against exactly these constants.
//
//   MyScan.lscan/
//     manifest.json                  sensor, profile, mount calib, CRS, versions
//     streams/
//       lidar.bin imu.bin poses_ar.bin gnss.bin
//       frames/   keyframe JPEGs + frames.idx
//     processed/  live_preview.lod · final.cloud · plan.dxf/pdf
//     merged/     merge graphs + results
//     exports/    user exports
//
// Stream files are: [StreamFileHeader][chunk][chunk]...  with
//   chunk = [payload_len u32][type u16][flags u16][t_mono_ns i64][payload][crc32 u32]
// all little-endian, no padding, no alignment requirement.
//
// Crash safety (the property that makes "record-always" true): the writer
// only ever appends, and a chunk is complete the instant its CRC lands. A
// reader stops at the first chunk whose length runs past EOF or whose CRC
// fails — the truncated tail of a killed capture is simply not there. No
// index is required to read a stream; the frames/ index is an optimisation,
// not a dependency.
//
// Forward compatibility: an unknown chunk type is skipped using its length,
// so a newer engine's extra streams never break an older reader; a
// StreamFileHeader with a higher `format_version` than kFormatVersion is a
// hard kVersionMismatch.
//
// Owner: A1 (format constants + CRC) / A5 (writer, reader, replay harness,
// camera-frame and GNSS streams).
#ifndef SCANENGINE_RECORD_LSCAN_H
#define SCANENGINE_RECORD_LSCAN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "scanengine/core/error.h"
#include "scanengine/core/span.h"
#include "scanengine/core/types.h"

namespace scanengine {
namespace lscan {

inline constexpr char kMagic[4] = {'L', 'S', 'C', 'N'};
inline constexpr std::uint16_t kFormatVersion = 1;

// Directory layout (relative to the .lscan root).
inline constexpr const char* kManifestFile = "manifest.json";
inline constexpr const char* kStreamsDir = "streams";
inline constexpr const char* kFramesDir = "streams/frames";
inline constexpr const char* kFrameIndexFile = "streams/frames/frames.idx";
inline constexpr const char* kProcessedDir = "processed";
inline constexpr const char* kMergedDir = "merged";
inline constexpr const char* kExportsDir = "exports";

inline constexpr const char* kLidarStreamFile = "streams/lidar.bin";
inline constexpr const char* kImuStreamFile = "streams/imu.bin";
inline constexpr const char* kPoseArStreamFile = "streams/poses_ar.bin";
inline constexpr const char* kGnssStreamFile = "streams/gnss.bin";

// Chunk payload kinds. STABLE, APPEND-ONLY: a shipped .lscan may contain any
// of these forever.
enum class ChunkType : std::uint16_t {
  kNone = 0,
  kD6Raw = 1,            // raw COIN-D6 UART bytes, exactly as received
  kMid360Points = 2,     // one SDK2 point packet, unmodified
  kMid360Imu = 3,        // one SDK2 IMU sample
  kPoseAr = 4,           // ARCore pose + tracking state
  kGnssNmea = 5,         // one NMEA sentence
  kGnssRtcm = 6,         // one RTCM3 message forwarded to the rover
  kCameraFrameIndex = 7, // keyframe descriptor (path, pose, intrinsics, t)
  kDeviceInfo = 8,       // device model/firmware at connect time
  kMarker = 9,           // user marker / capture annotation
  kSessionNote = 10,     // free text (mode changes, warnings)
  kPointsXyzRgba = 11,   // engine-frame PointVertex array (processed output)
};

// Chunk flags.
inline constexpr std::uint16_t kFlagNone = 0;
inline constexpr std::uint16_t kFlagCompressed = 1u << 0;  // A5 may add zstd
inline constexpr std::uint16_t kFlagKeyChunk = 1u << 1;    // replay seek point

inline constexpr std::size_t kChunkHeaderBytes = 16;
inline constexpr std::size_t kChunkTrailerBytes = 4;  // crc32
inline constexpr std::size_t kChunkOverheadBytes = kChunkHeaderBytes + kChunkTrailerBytes;
inline constexpr std::uint32_t kMaxChunkPayload = 16u * 1024u * 1024u;

inline constexpr std::size_t kStreamHeaderBytes = 32;

struct ChunkHeader {
  std::uint32_t payload_len = 0;
  ChunkType type = ChunkType::kNone;
  std::uint16_t flags = kFlagNone;
  std::int64_t t_mono_ns = 0;
};

struct StreamFileHeader {
  std::uint16_t format_version = kFormatVersion;
  StreamId stream = StreamId::kUnknown;
  std::int64_t t_start_mono_ns = 0;
  std::int64_t t_start_utc_ns = 0;  // wall clock at session start, for UTC correlation only
};

// --- CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) -------------------------
// Fixed here rather than in A5 because the value is part of the format.
// Test vector: crc32("123456789") == 0xCBF43926.
std::uint32_t crc32(ByteSpan data, std::uint32_t seed = 0);

// --- little-endian codecs ---------------------------------------------------
// `out` must have room for kChunkHeaderBytes / kStreamHeaderBytes.
void encode_chunk_header(const ChunkHeader& h, std::uint8_t* out);
bool decode_chunk_header(ByteSpan in, ChunkHeader* out);
void encode_stream_header(const StreamFileHeader& h, std::uint8_t* out);
bool decode_stream_header(ByteSpan in, StreamFileHeader* out);

// CRC over [header][payload], i.e. everything the trailer protects.
std::uint32_t chunk_crc(const ChunkHeader& h, ByteSpan payload);

// Which stream file a chunk type belongs in.
StreamId stream_of(ChunkType t);
const char* stream_file_of(StreamId s);

struct RecordStats {
  std::uint64_t chunks_written = 0;
  std::uint64_t bytes_written = 0;
  std::uint64_t flushes = 0;
  std::int64_t t_first_ns = 0;
  std::int64_t t_last_ns = 0;
};

// --- writer -----------------------------------------------------------------
//
// Record-always (Tech Spec §3 key rule 2): every raw stream hits this before
// any processing, and live SLAM is just another consumer. Replay == capture,
// which is what makes local / cloud / transfer the same pipeline in three
// places.
class RecordWriter {
 public:
  virtual ~RecordWriter() = default;

  virtual Status open(const std::string& lscan_dir) = 0;
  virtual Status write_chunk(ChunkType type, std::int64_t t_mono_ns, ByteSpan payload,
                             std::uint16_t flags = kFlagNone) = 0;
  virtual Status flush() = 0;
  virtual Status close() = 0;
  virtual bool is_open() const = 0;
  virtual RecordStats stats() const = 0;
};

// Counts and validates, writes nothing. It is what the Engine records into
// until A5 lands, so the whole capture path (including the "recording"
// session state and the byte accounting the UI shows) is exercised today.
class NullRecordWriter final : public RecordWriter {
 public:
  Status open(const std::string& lscan_dir) override;
  Status write_chunk(ChunkType type, std::int64_t t_mono_ns, ByteSpan payload,
                     std::uint16_t flags = kFlagNone) override;
  Status flush() override;
  Status close() override;
  bool is_open() const override { return open_; }
  RecordStats stats() const override { return stats_; }
  const std::string& path() const { return path_; }

 private:
  bool open_ = false;
  std::string path_;
  RecordStats stats_{};
};

// --- reader (A5) -------------------------------------------------------------
// Declared so the replay harness and the cloud worker have a name to code
// against. A5 implements it, including the truncated-tail rule above.
class RecordReader {
 public:
  virtual ~RecordReader() = default;
  virtual Status open(const std::string& lscan_dir) = 0;
  virtual Status next_chunk(ChunkHeader* header, std::vector<std::uint8_t>* payload) = 0;
  virtual Status seek(std::int64_t t_mono_ns) = 0;
  virtual Status close() = 0;
};

}  // namespace lscan
}  // namespace scanengine

#endif  // SCANENGINE_RECORD_LSCAN_H
