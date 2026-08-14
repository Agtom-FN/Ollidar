// rtcm3.h — RTCM 10403.x TRANSPORT framing only (Tech Spec §3.4).
//
// Scope, stated up front because the boundary is deliberate: this validates
// the frame envelope
//
//     [0xD3][6 reserved bits | 10-bit length][payload 0..1023][CRC-24Q, 3 B]
//
// and reads the 12-bit message number (DF002) out of the first payload bits.
// It does NOT decode message CONTENT — no MSM, no ephemeris, no station
// coordinates. Two reasons:
//
//  1. **Nothing in this product needs the content.** The engine's job is to
//     move corrections from the caster to the rover unchanged (§2.3: "NTRIP
//     corrections fetched by the app and forwarded as RTCM3"). The rover's
//     RTK engine is what consumes them. A decoder here would be a second,
//     divergent implementation of somebody else's job.
//  2. **It could not be tested.** S5's REPORT is explicit: the simulator's
//     payloads are transport-valid and semantically meaningless, so a
//     content decoder validated against them would be validated against
//     nothing. Real captured RTCM is the prerequisite (PUBLIC_CASTERS.md),
//     and that is a Phase-2 decision, not a silent one.
//
// What the framing DOES buy, and why it is not just a passthrough:
//
//   * **Corrections age.** §3.4 requires it surfaced. Age is time since the
//     last CRC-VALID frame — counting bytes would keep the age at zero while
//     a caster streams garbage.
//   * **Integrity.** A Bluetooth SPP link to the rover is the least reliable
//     hop in the chain. If frames arrive at the engine already corrupt, the
//     rover will reject them and the fix will quietly stay Float; the CRC
//     counter here is what tells the difference between "bad corrections"
//     and "bad sky".
//   * **Frame-aligned forwarding.** Handing the rover whole frames rather
//     than arbitrary TCP segments means a dropped connection cannot leave a
//     half-frame in the rover's parser.
//
// Owner: A10.
#ifndef SCANENGINE_GNSS_RTCM3_H
#define SCANENGINE_GNSS_RTCM3_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "scanengine/core/span.h"

namespace scanengine {
namespace rtcm3 {

inline constexpr std::uint8_t kPreamble = 0xD3;
inline constexpr std::size_t kHeaderBytes = 3;
inline constexpr std::size_t kCrcBytes = 3;
inline constexpr std::size_t kMaxPayloadBytes = 1023;  // 10-bit length field
inline constexpr std::size_t kMaxFrameBytes = kHeaderBytes + kMaxPayloadBytes + kCrcBytes;

// CRC-24Q (RTCM SC-104 / "Qualcomm"): generator
// x^24+x^23+x^18+x^17+x^14+x^11+x^10+x^7+x^6+x^5+x^4+x^3+x+1 == 0x1864CFB,
// MSB-first, initial value 0, no reflection, no final XOR. Same convention
// (and the same constant) the S5 spike's rtcm_tool.crc24q re-derived from the
// standard, so the two implementations cross-check each other in the
// sim-labelled tests.
inline constexpr std::uint32_t kCrc24qPoly = 0x01864CFBu;

std::uint32_t crc24q(const std::uint8_t* data, std::size_t n,
                     std::uint32_t crc = 0) noexcept;
inline std::uint32_t crc24q(ByteSpan b, std::uint32_t crc = 0) noexcept {
  return crc24q(b.data(), b.size(), crc);
}

struct FrameInfo {
  std::size_t offset = 0;        // byte offset of the 0xD3 in the scanned buffer
  std::size_t payload_len = 0;
  std::size_t total_len = 0;     // 3 + payload_len + 3
  std::uint16_t message_type = 0;  // DF002; 0 when payload_len < 2
  bool crc_ok = false;
};

// Whole-frame check over exactly one candidate frame starting at `data[0]`.
// Returns false when the buffer is too short for the declared length — that
// is "wait for more bytes", not "corrupt", and the framer treats it that way.
bool validate_frame(const std::uint8_t* data, std::size_t n, FrameInfo* out) noexcept;

// Build a frame around `payload` (tests and the GGA/RTCM loopback fixtures).
// Returns 0 when payload_len > kMaxPayloadBytes; otherwise the frame length
// written to `out`, which must have room for kMaxFrameBytes.
std::size_t build_frame(const std::uint8_t* payload, std::size_t payload_len,
                        std::uint8_t* out) noexcept;

// A frame's message number, for the histogram. Fixed-size because it lives
// inside a stats struct that gets copied under a lock.
inline constexpr std::size_t kMessageTypeSlots = 16;

struct MessageTypeCount {
  std::uint16_t type = 0;
  std::uint32_t count = 0;
};

struct Rtcm3Stats {
  std::uint64_t bytes_in = 0;
  std::uint64_t frames_ok = 0;
  std::uint64_t frames_crc_failed = 0;
  std::uint64_t resync_bytes = 0;    // bytes skipped hunting for a preamble
  std::uint64_t payload_bytes = 0;   // CRC-valid payload only
  std::int64_t t_first_frame_ns = 0;
  std::int64_t t_last_frame_ns = 0;
  std::uint16_t last_message_type = 0;

  // Top message types by first-seen order; overflow is counted, not dropped
  // silently, so "we saw more than 16 kinds" is visible.
  MessageTypeCount types[kMessageTypeSlots]{};
  std::size_t type_slots_used = 0;
  std::uint32_t types_overflow = 0;

  double crc_pass_rate() const {
    const std::uint64_t n = frames_ok + frames_crc_failed;
    return n ? static_cast<double>(frames_ok) / static_cast<double>(n) : 0.0;
  }
  std::uint32_t count_of(std::uint16_t type) const {
    for (std::size_t i = 0; i < type_slots_used; ++i) {
      if (types[i].type == type) return types[i].count;
    }
    return 0;
  }
};

// Stream framer. Resyncs a byte at a time on anything that is not a valid
// in-bounds frame, exactly like the S5 spike's `iter_frames` and like every
// real RTCM reader has to: an NTRIP caster may prepend an HTTP body boundary,
// and a serial rover link may lose bytes mid-frame.
//
// Not thread-safe. The NTRIP client owns one on its receive thread.
class Rtcm3Framer {
 public:
  using FrameHandler =
      std::function<void(ByteSpan frame, const FrameInfo& info, std::int64_t t_ns)>;

  Rtcm3Framer();

  // Whole, CRC-valid frames only. `frame` includes preamble, length, payload
  // and CRC — i.e. exactly the bytes to hand the rover.
  void set_handler(FrameHandler h) { handler_ = std::move(h); }

  // Optional: CRC-failed frames, for a diagnostic log. The bytes are NOT
  // forwarded to the rover.
  void set_bad_frame_handler(FrameHandler h) { bad_ = std::move(h); }

  void push(ByteSpan bytes, std::int64_t t_ns);

  // Buffer AND stats.
  void reset();

  // Buffer only. This is what a reconnect wants: the half-frame left over
  // from the dead connection must not be spliced onto the first bytes of the
  // new one, but "how many corrections did this SESSION receive" is a
  // question whose answer must survive the reconnect.
  void clear_buffer();

  const Rtcm3Stats& stats() const { return stats_; }

  // Seconds since the last CRC-valid frame; -1 before the first one. This is
  // the number §3.4 calls "corrections age surfaced" as seen by the ENGINE.
  // The rover's own view of it (GGA field 13, "age of differential data") is
  // separate and usually larger — see docs/A10-gnss.md §4.
  double age_s(std::int64_t now_ns) const;

  std::size_t buffered() const { return buf_.size(); }

 private:
  void note_type_(std::uint16_t type);

  std::vector<std::uint8_t> buf_;
  FrameHandler handler_;
  FrameHandler bad_;
  Rtcm3Stats stats_{};
};

}  // namespace rtcm3
}  // namespace scanengine

#endif  // SCANENGINE_GNSS_RTCM3_H
