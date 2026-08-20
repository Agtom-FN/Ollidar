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
// ROUND 8 (additive): the RESOLVED world-frame cloud, i.e. StreamId::kSlamMap.
// Its own file rather than lidar.bin, for one reason that matters: replaying
// kD6Raw means walking lidar.bin, and mixing 16-byte vertices into the raw
// UART stream would make that walk read (and CRC) tens of megabytes of points
// it then discards. Nothing wrote kSlamMap chunks before ROUND 8, so giving
// the stream its own filename breaks no existing recording.
inline constexpr const char* kMapStreamFile = "streams/map.bin";
// ROUND 9 (additive): the PHONE's IMU, i.e. StreamId::kImuPhone. Its own file
// for the same reason kSlamMap got one, plus a sharper one: `streams/imu.bin`
// is the MID-360's SDK2 datagram stream, and both `slam/post/post_pipeline.cpp`
// and `slam/post/d6_resolve.cpp` (lscan_is_d6_project) treat a non-empty
// StreamId::kImu summary as evidence that a container is a Mid-360 project. A
// phone-IMU sample written into imu.bin would therefore route a pure D6 capture
// to the Mid-360 pipeline, which cannot resolve it. Nothing wrote kPhoneImu
// chunks before ROUND 9, so a new filename breaks no existing recording.
inline constexpr const char* kPhoneImuStreamFile = "streams/imu_phone.bin";

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
  kPhoneImu = 12,        // one decoded PHONE gyro+accel sample (ROUND 9)
  kStl27lRaw = 13,       // raw LDROBOT STL-27L UART bytes (ITEM 119), exactly
                         // as received — the kD6Raw contract for a second
                         // serial lidar, on its own type so an offline reader
                         // never hands 47-byte LD frames to the D6 parser.
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

// --- ROUND 8: the kPoseAr payload ------------------------------------------
//
// `ChunkType::kPoseAr` has been defined since A1 and mapped since A5 with NO
// WRITER — android/NOTES.md ROUND 7 §9 item 1 named that as the single seam
// blocking offline re-assembly of a D6 capture ("a record-always D6 capture
// stores the raw UART bytes but not the trajectory, and the assembled cloud
// exists only in the live PageStore and dies with the session"). This is the
// payload the writer needed.
//
// WHY A POD HERE AND NOT `scanengine::Pose`. record/ is the format module: it
// owns framing, CRC and file layout and depends on core/ only. `Pose` lives in
// poses/pose_source.h and drags timesync/clock.h behind it, so encoding it
// directly would invert the layering for no gain. `Engine::push_pose()` does
// the two-line conversion at the boundary, which is also the only place that
// knows the pose is worth recording at all.
//
// LAYOUT — fixed 68 bytes, little-endian, no padding, no alignment
// requirement (same rules as the chunk header itself). The stamp is NOT in
// here: a chunk already carries `t_mono_ns`, and duplicating it would let the
// two disagree.
//
//   off  size  field
//     0    24  position[3]            f64  metres, session-local frame
//    24    32  orientation[4]         f64  unit quaternion (x, y, z, w)
//    56     4  position_sigma_m       f32
//    60     4  orientation_sigma_deg  f32
//    64     1  source                 u8   StreamId
//    65     1  quality                u8   PoseQuality 0..3
//    66     1  tracking_lost          u8   0/1
//    67     1  reserved               u8   written 0, ignored on read
//
// COST, because "record-always" has to be affordable: ARCore delivers ~30
// poses/s, so 68 B * 30 = 2.0 KB/s — 7.3 MB over a one-hour walk, against the
// ~200 KB/s of raw D6 UART bytes the same session already writes. Under 1 %.
inline constexpr std::size_t kPoseChunkPayloadBytes = 68;

struct PoseChunkRecord {
  double position[3] = {0.0, 0.0, 0.0};
  double orientation[4] = {0.0, 0.0, 0.0, 1.0};  // x, y, z, w
  float position_sigma_m = 0.0f;
  float orientation_sigma_deg = 0.0f;
  std::uint8_t source = 0;         // StreamId
  std::uint8_t quality = 0;        // PoseQuality
  std::uint8_t tracking_lost = 0;
};

// `out` must have room for kPoseChunkPayloadBytes.
void encode_pose_chunk(const PoseChunkRecord& p, std::uint8_t* out);
// False (leaving `*out` untouched) when `in` is not exactly the expected size.
// A LONGER payload is accepted and its tail ignored, so a future engine that
// appends a field stays readable here — the same forward-compatibility rule
// the chunk framing itself follows.
bool decode_pose_chunk(ByteSpan in, PoseChunkRecord* out);

// --- ROUND 8: the kPointsXyzRgba payload -----------------------------------
//
// A `PointVertex` array (cloud/point_page.h), verbatim: 12 bytes of
// little-endian f32 x/y/z then 4 bytes of r/g/b/a. Declared here as a byte
// count rather than as the struct so record/ keeps its core/-only dependency
// set; the two are pinned to each other by a static_assert in lscan.cpp.
inline constexpr std::size_t kPointVertexBytes = 16;

// --- ROUND 9: the kPhoneImu payload -----------------------------------------
//
// ROUND 9 item 35 made the phone's gyro a real input to the geometry:
// `ImuDensifiedPoseSource` replaces the orientation of every interpolated pose
// with a gyro-integrated path between the same two ARCore knots, which on the
// owner's own 12 Hz-jitter fixture took the wall's plane-fit RMS from 0.739 cm
// to 0.021 cm. That makes the phone IMU part of how the cloud is SHAPED — and
// record-always (Tech Spec §3 key rule 2) says a stream the geometry depends on
// must be on disk, or the container stops being self-contained and the offline
// re-resolve silently produces a different (worse) room than the live pass. It
// is exactly the argument ROUND 8 made for kPoseAr, one sensor further down.
//
// WHY A DECODED POD AND NOT AN OPAQUE DATAGRAM. kMid360Imu records the SDK2
// datagram verbatim because that is what arrives — a wire format with its own
// framing that the driver must parse anyway. A phone IMU sample arrives as an
// Android `SensorEvent`: already decoded, already in SI units, already in
// CLOCK_BOOTTIME. There is no wire format to preserve, so this follows the
// kPoseAr precedent (a fixed little-endian POD) rather than the kMid360Imu one.
//
// LAYOUT — fixed 24 bytes, little-endian, no padding, no alignment requirement.
// The stamp is NOT in here, for the same reason it is not in the pose record: a
// chunk already carries `t_mono_ns` and duplicating it would let the two
// disagree.
//
//   off  size  field
//     0    12  gyro_rad_s[3]    f32  rad/s, IMU frame (Android SENSOR_TYPE_GYROSCOPE)
//    12    12  accel_m_s2[3]    f32  m/s^2, IMU frame (SENSOR_TYPE_ACCELEROMETER)
//
// f32 and not f64 on purpose: an Android SensorEvent delivers `float`, so f64
// would store nothing but the widening. `ImuDensifiedPoseSource::PhoneImuSample`
// carries the same two float triples, which makes the conversion a memcpy's
// worth of work at the boundary and leaves no room for a units mistake.
//
// COST, because record-always has to be affordable: 24 B at 400 Hz = 9.6 KB/s,
// 34 MB over a one-hour walk. Against the ~200 KB/s of raw D6 UART bytes the
// same session already writes, under 5 % — and it is the cheapest 5 % in the
// container, because it is what makes the offline resolve reproduce the live
// geometry instead of approximating it.
inline constexpr std::size_t kPhoneImuChunkPayloadBytes = 24;

struct PhoneImuChunkRecord {
  float gyro_rad_s[3] = {0.0f, 0.0f, 0.0f};
  float accel_m_s2[3] = {0.0f, 0.0f, 0.0f};
};

// `out` must have room for kPhoneImuChunkPayloadBytes.
void encode_phone_imu_chunk(const PhoneImuChunkRecord& s, std::uint8_t* out);
// False (leaving `*out` untouched) when `in` is shorter than the expected size.
// A LONGER payload is accepted and its tail ignored — the same
// forward-compatibility rule decode_pose_chunk() and the chunk framing follow.
bool decode_phone_imu_chunk(ByteSpan in, PhoneImuChunkRecord* out);

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

  // ROUND 14: drop everything the OPTIONAL setters above accumulated, so the
  // next open() describes the next container and nothing else.
  //
  // This writer outlives the containers it writes — the Engine owns one for
  // its whole life and opens a new .lscan per capture — and `add_sensor()`
  // appends. Nothing ever emptied it, so capture 2 of an app run listed
  // capture 1's sensors as well as its own, capture 3 listed all three sets,
  // and the owner's scan-035 shipped with SIX entries for a two-sensor rig.
  // It is not cosmetic: `"sensors"` is how a reader decides what a container
  // HOLDS without decoding it.
  //
  // Not done inside open(): the Engine sets the profile, the sensors and both
  // extrinsics BEFORE open() (they must be in the manifest open() writes), so
  // clearing there would erase the session that is starting. The caller
  // resets, then describes, then opens.
  void reset_metadata();

  // ===== INT-34 ADDITION — the ONE thing outside A5 that touches this file ==
  //
  // A5 owns record/. INT-34 added exactly this setter and the matching
  // `"clockOffsets"` key in write_manifest(); nothing else in lscan.h or
  // lscan.cpp changed, and the change is purely additive (a new optional
  // field, a new always-present manifest key, no format-version bump, no
  // behaviour change for a caller that never calls it). If A5 revisits the
  // manifest, this is the block to reconcile.
  //
  // WHY. docs/A11-color.md §8.4: WIZARD §3 wants the estimated camera↔engine
  // clock offset in manifest.json, keyed per bracket. A11 both PRODUCES it
  // (`color::ClockSweepResult::offset_ns`) and CONSUMES it
  // (`color::ColorizeConfig::camera_clock_offset_ns`) — but nothing persisted
  // it, so a session re-opened on a desktop or in the cloud worker had to
  // re-run the 8-second sweep it can no longer capture. One number in the
  // manifest is the whole fix.
  //
  // SIGN CONVENTION, the same one clock_sweep.h states and asserts:
  //     t_engine_ns = t_camera_ns + camera_to_engine_ns
  //
  // `bracket` is the mount bracket the calibration belongs to (WIZARD §3 keys
  // both the mount calibration and the offset by it); "" is stored as
  // "default". Calling this twice for one bracket replaces that entry.
  // Emitted as:
  //     "clockOffsets": {"<bracket>": {"cameraToEngineNs": N, "sigmaNs": S}}
  // — always present, `{}` when nothing was set, so a consumer can rely on
  // the key from day one exactly as it can on "mountCalibration"/"crs".
  void add_clock_offset(const std::string& bracket, std::int64_t camera_to_engine_ns,
                        double sigma_ns = 0.0);
  // ===== end INT-34 addition ===============================================

  // ===== ROUND 8 ADDITION — the mount extrinsic, so a project re-resolves ==
  //
  // `"mountCalibration"` has been a reserved `null` since A5. ROUND 8 fills it
  // in, because without it a recorded D6 capture is NOT self-contained: the
  // raw returns are in the lidar's own frame and only `phone_from_lidar` says
  // where that frame sits on the rig. The app has the number (its own manifest
  // has carried `mountTrim` since ROUND 6) but a desktop, a cloud worker or
  // `engine_cli` re-resolving the same directory does not, and asking every
  // consumer to also parse the app's private sidecar is the wrong shape.
  //
  // ROW-MAJOR 4x4, the same convention Engine::set_mount_extrinsics() takes
  // and docs/A8-pushbroom.md §3.1 states. Call before open(); unset stays
  // `null`, exactly as before. Emitted as:
  //     "mountCalibration": {"phoneFromLidar": [16 doubles, row-major]}
  void set_mount_calibration(const double phone_from_lidar[16]);
  // ===== end ROUND 8 addition ==============================================

  // ===== ROUND 9 addition ==================================================
  // The phone-IMU extrinsic, for exactly the reason above. A container that
  // carries a `kPhoneImu` stream but not `camera_from_imu` is self-contained
  // only by accident: the gyro samples are in the Android sensor frame, ARCore
  // reports its pose in the camera frame, and on a real phone those differ by
  // the camera's `SENSOR_ORIENTATION` (usually 90 degrees). Re-resolving
  // without it does not fail loudly — it silently distorts the densified path
  // between pose knots, which is precisely the thing the IMU was added to
  // improve. Only the app knows the number, so only the app can record it.
  //
  // Quaternion (x, y, z, w) taking a vector in the IMU frame to the camera
  // frame, matching `ImuDensifyConfig::camera_from_imu`. Call before open();
  // unset stays `null`. Emitted as:
  //     "imuCalibration": {"cameraFromImu": [x, y, z, w]}
  void set_imu_calibration(const double camera_from_imu[4]);
  // ===== end ROUND 9 addition ==============================================

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
