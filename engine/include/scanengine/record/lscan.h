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
#include <memory>
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

// --- file-backed writer (A5) -------------------------------------------------
//
// Directory skeleton created at open() (Tech Spec §3.11): manifest.json plus
// the streams/, streams/frames/, processed/, merged/, exports/ directories.
// Each streams/*.bin file is created lazily, on its first chunk, so e.g. a
// D6-only capture never produces an empty imu.bin.
//
// Flush / fsync policy (the property that bounds the crash-loss window):
//   * every write_chunk() appends framed bytes to the target stream's
//     buffered FILE*; a chunk is never partially "visible" to a reader
//     because RecordReader only trusts a chunk once its CRC verifies -- so a
//     torn OS-buffer write reads back as "not written yet", never as silent
//     corruption.
//   * a stream auto-flushes (fflush + fsync/_commit) when EITHER
//     kAutoFlushBytes have accumulated since its last flush OR
//     kAutoFlushIntervalNs (1s) have elapsed since its last flush for that
//     stream -- checked opportunistically on the next write_chunk() call,
//     because record/ (like every module per DESIGN.md §2) owns no thread
//     of its own.
//   * DOCUMENTED DATA-LOSS WINDOW: under continuous input the window is
//     bounded by kAutoFlushIntervalNs -- at most ~1s of un-fsync'd chunks
//     lost on a hard crash. If input stalls completely with a nonempty
//     buffer pending, that tail is NOT time-bounded until the next
//     write_chunk() or an explicit flush() call -- a caller that must bound
//     data loss through idle periods should call flush() from its own
//     periodic timer (the Android/Qt capture UIs already poll engine state
//     on a timer for other reasons; hooking flush() to the same cadence is
//     the intended integration -- see docs/A5-lscan.md).
//   * close() flushes+fsyncs everything and rewrites manifest.json with
//     "sealed": true. A manifest still "sealed": false after a crash is a
//     positive signal (not merely an absence of one) that the session ended
//     abnormally; RecordReader does not require it to be true, but a UI may
//     surface it.
class FileRecordWriter final : public RecordWriter {
 public:
  static constexpr std::size_t kAutoFlushBytes = 1u * 1024u * 1024u;     // 1 MiB
  static constexpr std::int64_t kAutoFlushIntervalNs = 1'000'000'000LL;  // 1 s

  FileRecordWriter();
  ~FileRecordWriter() override;
  FileRecordWriter(const FileRecordWriter&) = delete;
  FileRecordWriter& operator=(const FileRecordWriter&) = delete;

  Status open(const std::string& lscan_dir) override;
  Status write_chunk(ChunkType type, std::int64_t t_mono_ns, ByteSpan payload,
                     std::uint16_t flags = kFlagNone) override;
  Status flush() override;
  Status close() override;
  bool is_open() const override;
  RecordStats stats() const override;

  // Optional manifest fields; call before open(). Anything left unset is
  // written as a documented placeholder -- mount calibration (A8) and CRS
  // (A10) are always emitted as `null` until those tasks land, per Tech Spec
  // §3.11.
  void set_profile(const std::string& profile);
  void add_sensor(const std::string& id, const std::string& kind, const std::string& model);

  const std::string& path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --- reader (A5) -------------------------------------------------------------
// Declared so the replay harness and the cloud worker have a name to code
// against. A5 implements it, including the truncated-tail rule above.
class RecordReader {
 public:
  virtual ~RecordReader() = default;
  virtual Status open(const std::string& lscan_dir) = 0;
  // kOkStatus with a filled chunk, or ScanError::kAgain once every stream is
  // exhausted (the same "not an error" convention as EventBus::poll() -- see
  // core/error.h).
  virtual Status next_chunk(ChunkHeader* header, std::vector<std::uint8_t>* payload) = 0;
  virtual Status seek(std::int64_t t_mono_ns) = 0;
  virtual Status close() = 0;
};

// One stream file's shape, computed by FileRecordReader::open()'s validation
// pass (i.e. only over chunks that passed CRC and were not truncated).
struct StreamSummary {
  StreamId stream = StreamId::kUnknown;
  std::uint64_t chunk_count = 0;
  std::uint64_t bytes = 0;  // payload bytes only, summed over valid chunks
  std::int64_t t_first_ns = 0;
  std::int64_t t_last_ns = 0;
};

// Crash-safety telemetry from the validation pass. Never a reason to fail
// open() by itself -- see FileRecordReader's class comment.
struct ReaderWarnings {
  std::uint32_t truncated_tail_chunks = 0;  // framing ran past EOF, or an oversized/garbled header
  std::uint32_t crc_mismatch_chunks = 0;    // header parsed; payload/trailer did not verify
  std::uint32_t unreadable_streams = 0;     // a stream file's own 32-byte header was unreadable

  std::uint32_t total_skipped() const { return truncated_tail_chunks + crc_mismatch_chunks; }
};

// Opens a .lscan directory written by FileRecordWriter (or any writer
// honouring the same framing).
//
// Manifest handling: open() reads and structurally validates manifest.json
// but a missing/corrupt manifest does NOT fail open() -- crash safety means
// raw chunk data must outlive a half-written or absent manifest (the writer
// creates manifest.json before any chunk, but a kill at the very first
// syscall can still leave it empty). manifest_present()/manifest_ok() report
// what was found; the data streams are read independently either way.
//
// Chunk iteration: next_chunk() performs a chronological k-way merge across
// every stream file present on disk -- chunks come out in non-decreasing
// t_mono_ns order across the whole container, which is what the replay
// harness (record/replay.h) needs for a multi-sensor session. A stream stops
// -- silently, from next_chunk()'s point of view -- at the first chunk whose
// framing runs past EOF or whose CRC fails, per the format's truncated-tail
// rule; that stop is detected once, during open()'s validation pass, and
// surfaced through warnings().
class FileRecordReader final : public RecordReader {
 public:
  FileRecordReader();
  ~FileRecordReader() override;
  FileRecordReader(const FileRecordReader&) = delete;
  FileRecordReader& operator=(const FileRecordReader&) = delete;

  Status open(const std::string& lscan_dir) override;
  Status next_chunk(ChunkHeader* header, std::vector<std::uint8_t>* payload) override;
  Status seek(std::int64_t t_mono_ns) override;
  Status close() override;

  // Which stream the chunk returned by the most recent next_chunk() came
  // from. Valid only after a next_chunk() that returned kOkStatus.
  StreamId last_stream() const;

  const std::vector<StreamSummary>& stream_summaries() const;
  const ReaderWarnings& warnings() const;

  // Manifest diagnostics -- see the class comment for why a bad manifest
  // does not fail open().
  bool manifest_present() const;
  bool manifest_ok() const;
  const std::string& manifest_raw() const;  // "" if not present

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lscan
}  // namespace scanengine

#endif  // SCANENGINE_RECORD_LSCAN_H
