// frames_idx.h — `streams/frames/frames.idx`, the keyframe index (A11).
//
// THIS IS THE FORMAT B8 WRITES. Tech Spec §3.11 reserves
// `streams/frames/  keyframe JPEGs + frames.idx (pose, intrinsics, t)`;
// A1 fixed the framing and the chunk-type number; A5 built the writer/reader
// around them and left the PAYLOAD to A11 (docs/A5-lscan.md §3). This header
// is that payload.
//
// --- the seam, exactly -----------------------------------------------------
//
// frames.idx is an ORDINARY .lscan stream file. It is not a special case:
//
//   frames.idx = [StreamFileHeader (32 B)][chunk][chunk]...
//   chunk      = [payload_len u32][type u16][flags u16][t_mono_ns i64]
//                [payload][crc32 u32]
//   type       = ChunkType::kCameraFrameIndex (7)
//   stream     = StreamId::kCameraFrames (6)   — lscan::stream_file_of() already
//                maps it to "streams/frames/frames.idx"
//
// So the capture side does not need a new writer at all:
//
//   std::vector<std::uint8_t> rec;
//   encode_keyframe_record(kf, &rec);
//   recorder.write_chunk(lscan::ChunkType::kCameraFrameIndex, kf.t_mono_ns,
//                        ByteSpan(rec.data(), rec.size()));
//
// and gets crash safety, the truncated-tail rule and the CRC for free.
// `KeyframeIndexWriter` below writes the byte-identical thing standalone, for
// tools, tests and the desktop importer; `tests/test_color.cpp` asserts the
// two agree byte for byte.
//
// One record per keyframe, in capture order, `t_mono_ns` non-decreasing.
//
// --- the record ------------------------------------------------------------
//
// Little-endian, packed, no alignment requirement — the same rules as every
// other .lscan structure. A fixed 160-byte part followed by the image file
// name:
//
//   off  size  field                         notes
//     0     2  record_version u16            kKeyframeRecordVersion
//     2     2  fixed_bytes    u16            160 today; a READER SKIPS ANY
//                                            EXCESS so a future version can
//                                            append fields without a bump
//     4     4  flags          u32            kKeyframeFlag* (colorize.h)
//     8     8  t_engine_ns    i64            engine clock, exposure of ROW 0
//    16     8  exposure_ns    i64            exposure duration
//    24    24  position       f64[3]         world_from_camera translation
//    48    32  orientation    f64[4]         world_from_camera quaternion x,y,z,w
//    80    16  fx, fy, cx, cy f32[4]         pixels
//    96    20  distortion     f32[5]         k1, k2, p1, p2, k3 (OpenCV order)
//   116     4  width          u32
//   120     4  height         u32
//   124     4  row_time_ns    f32            rolling shutter; 0 = global
//   128     4  position_sigma f32            metres
//   132     4  orient_sigma   f32            degrees
//   136     1  pose_quality   u8             PoseQuality
//   137     1  tracking_lost  u8
//   138     1  pose_source    u8             StreamId
//   139     1  reserved       u8             0
//   140     4  angular_rate   f32            rad/s at capture (kMotionValid)
//   144     4  linear_speed   f32            m/s at capture   (kMotionValid)
//   148     4  iso            f32            (kExposureValid)
//   152     4  image_bytes    u32            JPEG size on disk, 0 = unknown
//   156     4  name_len       u32            bytes of the name that follows
//   160  name_len  image name  UTF-8         RELATIVE TO streams/frames/,
//                                            forward slashes, no "..", not
//                                            absolute
//
// The name is relative to `streams/frames/` (normally a bare
// "kf_000123.jpg"), because that is the directory the app writes into and it
// keeps the record short. `Keyframe::image_path` — which colorize.h defines
// as relative to the .lscan ROOT — is composed on read as
// "streams/frames/" + name, and decomposed on write.
//
// t_mono_ns appears twice: in the chunk header (so A5's chronological merge
// and `seek()` work without parsing payloads) and in the record (so a record
// extracted on its own is still self-describing). A reader that finds them
// disagreeing trusts the record and counts it.
//
// Owner: A11.
#ifndef SCANENGINE_COLOR_FRAMES_IDX_H
#define SCANENGINE_COLOR_FRAMES_IDX_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scanengine/color/colorize.h"
#include "scanengine/core/error.h"
#include "scanengine/core/span.h"

namespace scanengine {
namespace color {

inline constexpr std::uint16_t kKeyframeRecordVersion = 1;
inline constexpr std::size_t kKeyframeRecordFixedBytes = 160;
// A name longer than this is a bug or an attack, not a file name.
inline constexpr std::uint32_t kKeyframeNameMaxBytes = 512;

// --- codec ------------------------------------------------------------------

// Encodes one record. Fails with kInvalidArgument on anything
// `validate_keyframe()` rejects — an unusable record must not reach the disk,
// because the reader's only alternative is to skip it silently later.
Status encode_keyframe_record(const Keyframe& kf, std::vector<std::uint8_t>* out);

// Decodes one record. `in` may be longer than the record (trailing bytes are
// ignored); `consumed`, when non-null, reports the record's length.
//
//   kCorruptData      truncated, name_len past the end, fixed_bytes too small
//   kVersionMismatch  record_version > kKeyframeRecordVersion
//
// Forward compatibility: a record whose `fixed_bytes` exceeds this build's
// knowledge is read up to what it knows and the excess is skipped.
Status decode_keyframe_record(ByteSpan in, Keyframe* out, std::size_t* consumed = nullptr);

// Structural validation, applied on both sides of the format:
// finite pose, unit quaternion (1e-6), positive fx/fy, principal point inside
// the image, non-zero width/height, non-negative row time and exposure, a
// non-empty relative image name with no "..", no leading '/' and no drive
// letter. Returns kInvalidArgument with a `last_error()` detail naming the
// field.
Status validate_keyframe(const Keyframe& kf);

// "streams/frames/<name>" ⇄ "<name>".
std::string keyframe_image_name(const Keyframe& kf);
std::string keyframe_image_path(const std::string& lscan_dir, const Keyframe& kf);

// --- writer -----------------------------------------------------------------
//
// Writes a standalone, byte-identical frames.idx (32-byte stream header +
// one kCameraFrameIndex chunk per keyframe). The Android capture path should
// use `lscan::FileRecordWriter` instead — it is already open, already
// crash-safe, already flushing on the same policy as every other stream. This
// class exists for the tools that have no recorder: the desktop importer, the
// synthetic fixtures in tests, and any offline re-indexing.
class KeyframeIndexWriter {
 public:
  KeyframeIndexWriter();
  ~KeyframeIndexWriter();
  KeyframeIndexWriter(const KeyframeIndexWriter&) = delete;
  KeyframeIndexWriter& operator=(const KeyframeIndexWriter&) = delete;

  // Creates `<lscan_dir>/streams/frames/` if needed. The index FILE itself is
  // created lazily, on the first `add()` — exactly as `FileRecordWriter`
  // creates its stream files — so a session that captured no keyframes leaves
  // no frames.idx behind, and the stream header's `t_start_mono_ns` is the
  // first keyframe's stamp. Those two properties are what make the output
  // byte-identical to the recorder's.
  Status open(const std::string& lscan_dir);
  // Same, but naming the index file directly (no directory creation).
  // A non-zero `t_start_mono_ns` overrides the first keyframe's stamp in the
  // stream header.
  Status open_file(const std::string& idx_path, std::int64_t t_start_mono_ns = 0);
  Status add(const Keyframe& kf);
  Status flush();
  Status close();
  bool is_open() const;
  std::uint32_t records() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --- reader -----------------------------------------------------------------

// What the read pass found. Mirrors `lscan::ReaderWarnings`' philosophy: a
// truncated tail is a NORMAL outcome for a killed capture, never an error.
struct FrameIndexStats {
  std::uint32_t records = 0;
  std::uint32_t truncated_tail_chunks = 0;  // framing ran past EOF
  std::uint32_t crc_mismatch_chunks = 0;    // header parsed, payload did not verify
  std::uint32_t foreign_chunks = 0;         // not kCameraFrameIndex — skipped by length
  std::uint32_t malformed_records = 0;      // CRC-valid chunk, unreadable payload
  std::uint32_t rejected_records = 0;       // decoded but failed validate_keyframe()
  std::uint32_t out_of_order_records = 0;   // t went backwards (kept, counted)
  std::uint32_t header_time_mismatches = 0; // chunk header t != record t (record wins)
};

// Reads every keyframe from a .lscan directory (or from an explicit index
// file). Stops at the first chunk whose framing runs past EOF or whose CRC
// fails — A1's truncated-tail rule — and reports it in `stats` rather than
// failing. A missing frames.idx is `kNotFound`: a desktop-captured session
// has no camera, and §3.5 says colorization is then "gracefully unavailable".
Status read_frame_index(const std::string& lscan_dir, std::vector<Keyframe>* out,
                        FrameIndexStats* stats = nullptr);
Status read_frame_index_file(const std::string& idx_path, std::vector<Keyframe>* out,
                             FrameIndexStats* stats = nullptr);
// Same, over an in-memory copy of the whole file (what the cloud worker has).
Status parse_frame_index(ByteSpan file_bytes, std::vector<Keyframe>* out,
                         FrameIndexStats* stats = nullptr);

}  // namespace color
}  // namespace scanengine

#endif  // SCANENGINE_COLOR_FRAMES_IDX_H
