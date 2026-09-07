// juxi_frames.h — JuxiTech ICM-42670-P module UART framing (A18).
//
// Ported from the vendor's Arduino reference (imu_uart_driver.cpp,
// IMU_UART_Process/_parse_frame_data/IMU_UART_SendCommand), not guessed. The
// wire facts this file encodes, all of them from that reference:
//
//   • 115200 8N1. Every frame is
//         7E 23 <len> <func> <payload…> <sum8>
//     where `len` is the TOTAL number of bytes in the frame, i.e.
//         len = 4 + payload_len + 1        (2 header + len + func + checksum)
//     and sum8 = (0x7E + 0x23 + len + func + Σ payload) & 0xFF.
//     The vendor's receiver computes `data_length = len - 4` and treats that
//     as "payload + checksum", which is the same statement.
//
//   • func 0x04 is the only frame carrying motion: 18 payload bytes =
//     nine int16 little-endian values, ax ay az gx gy gz mx my mz, scaled
//         accel × (16 / 32767)                 → g
//         gyro  × (2000 / 32767) × π/180       → rad/s
//         mag   × (800 / 32767)                → µT
//     (the vendor folds the deg→rad conversion into its GYRO_RATIO, so its
//     get_gyroscope_data() is already rad/s; we match that.)
//
//   • func 0x16 (quaternion, 16 B), 0x26 (Euler, 12 B), 0x32 (barometer,
//     16 B), 0x01 (version, 3 B) and 0x81 (return-state, 2 B) are recognised,
//     COUNTED and otherwise ignored. The module emits all of them every cycle
//     and there is no command that turns them off, so "unexpected frame type"
//     is not an error here — but a stream that suddenly stops carrying 0x04
//     while still carrying 0x16 is a real, visible condition, which is why
//     they are counted separately rather than lumped into "unknown".
//
//   • The rate command is 7E 23 07 60 <hz> 5F <sum8>, hz 10..100. The module
//     PERSISTS it in flash, so it survives a power cycle and re-sending it is
//     idempotent.
//
// The module has NO timestamp and NO sample counter in any frame: nothing
// here can tell you how many samples were lost across a hole. That is why
// timing lives in imu_stamper.h and blackouts are counted in the driver from
// arrival times, not reconstructed from the wire.
//
// FrameParser is a byte-stream state machine in the shape of d6::Parser: it
// reassembles frames across arbitrary chunk boundaries (the app hands the
// engine whatever the OS gave it — DESIGN.md / transport/byte_source.h) and
// never assumes a chunk starts on a frame boundary.
//
// Owner: A18 (serial IMU).
#ifndef SCANENGINE_DRIVERS_IMU_SERIAL_JUXI_FRAMES_H
#define SCANENGINE_DRIVERS_IMU_SERIAL_JUXI_FRAMES_H

#include <cstddef>
#include <cstdint>
#include <functional>

namespace scanengine {
namespace juxi {

// --- framing constants (vendor reference) ---------------------------------
constexpr std::uint8_t kHead1 = 0x7E;
constexpr std::uint8_t kHead2 = 0x23;

constexpr std::uint8_t kFuncVersion = 0x01;      // 3-byte payload
constexpr std::uint8_t kFuncRawImu = 0x04;       // 18-byte payload (the one we use)
constexpr std::uint8_t kFuncQuaternion = 0x16;   // 16-byte payload
constexpr std::uint8_t kFuncEuler = 0x26;        // 12-byte payload
constexpr std::uint8_t kFuncBarometer = 0x32;    // 16-byte payload
constexpr std::uint8_t kFuncRequestData = 0x80;  // host → module
constexpr std::uint8_t kFuncReturnState = 0x81;  // 2-byte payload
constexpr std::uint8_t kFuncSetRate = 0x60;      // host → module, the rate command
constexpr std::uint8_t kSetRateTrailer = 0x5F;   // second param byte of the rate command

// Bytes of a frame that are not payload: head1, head2, len, func, checksum.
constexpr std::size_t kFrameOverheadBytes = 5;
constexpr std::size_t kRawImuPayloadBytes = 18;
constexpr std::size_t kRawImuFrameBytes = kRawImuPayloadBytes + kFrameOverheadBytes;  // 23

// The vendor's receiver buffers `len - 4` bytes into a 64-byte array, so a
// frame longer than this cannot come from this module and is a framing fault.
constexpr std::size_t kMaxPayloadPlusChecksum = 64;

// Rate command bounds. The module ignores values outside them.
constexpr std::uint8_t kMinRateHz = 10;
constexpr std::uint8_t kMaxRateHz = 100;
constexpr std::size_t kRateCommandBytes = 7;

// Scale factors, exactly the vendor's constants.
constexpr float kAccelScaleG = 16.0f / 32767.0f;
constexpr float kGyroScaleRadS = (2000.0f / 32767.0f) * 3.14159265358979323846f / 180.0f;
constexpr float kMagScaleUt = 800.0f / 32767.0f;

// One decoded func-0x04 frame. Magnetometer is decoded because the frame
// carries it and dropping it silently would make the parser's output
// disagree with the wire; the driver does not publish it (a hand-carried rig
// near a laptop has no usable heading, and LIO does not consume mag).
struct RawImuFrame {
  float accel_g[3] = {0.f, 0.f, 0.f};
  float gyro_rad_s[3] = {0.f, 0.f, 0.f};
  float mag_ut[3] = {0.f, 0.f, 0.f};
};

// Per-type counters. `raw_frames` is the health signal; the rest exist so a
// stalled 0x04 report inside an otherwise healthy stream is visible.
struct FrameStats {
  std::uint64_t bytes_in = 0;

  std::uint64_t raw_frames = 0;
  std::uint64_t quaternion_frames = 0;
  std::uint64_t euler_frames = 0;
  std::uint64_t barometer_frames = 0;
  std::uint64_t version_frames = 0;
  std::uint64_t state_frames = 0;
  std::uint64_t unknown_frames = 0;  // valid checksum, func we do not model

  std::uint64_t checksum_failures = 0;
  // Times the state machine threw away a partial frame and went hunting for
  // 0x7E again: a bad length field, or a 0x7E not followed by 0x23. A
  // checksum failure is NOT a resync (the frame was complete, just wrong).
  std::uint64_t resyncs = 0;

  std::uint64_t frames_ok() const {
    return raw_frames + quaternion_frames + euler_frames + barometer_frames + version_frames +
           state_frames + unknown_frames;
  }
  std::uint64_t frames_seen() const { return frames_ok() + checksum_failures; }
  double checksum_pass_rate() const {
    const std::uint64_t total = frames_seen();
    return total == 0 ? 0.0 : static_cast<double>(frames_ok()) / static_cast<double>(total);
  }
};

// Called once per decoded func-0x04 frame, on the pushing thread.
using RawFrameCallback = std::function<void(const RawImuFrame&)>;

class FrameParser {
 public:
  FrameParser() = default;

  // Feed an arbitrary byte chunk. Frames spanning chunk boundaries are
  // reassembled; `on_raw` is invoked (possibly several times, possibly not at
  // all) before this returns.
  void push(const std::uint8_t* bytes, std::size_t n, const RawFrameCallback& on_raw);

  const FrameStats& stats() const { return stats_; }
  void reset();

 private:
  enum class State : std::uint8_t {
    kHead1 = 0,
    kHead2,
    kLength,
    kFunction,
    kBody,  // payload + checksum
  };

  void dispatch(const RawFrameCallback& on_raw);

  State state_ = State::kHead1;
  std::uint8_t len_ = 0;
  std::uint8_t func_ = 0;
  std::size_t body_len_ = 0;  // payload + checksum, i.e. len_ - 4
  std::size_t body_index_ = 0;
  std::uint8_t body_[kMaxPayloadPlusChecksum] = {0};
  FrameStats stats_{};
};

// Decode a func-0x04 payload (18 bytes). Exposed so a replay tool can decode
// without running the state machine. Returns false if `n` is not 18.
bool decode_raw_imu(const std::uint8_t* payload, std::size_t n, RawImuFrame* out);

// Build the report-rate command. Returns the number of bytes written (7), or
// 0 if `hz` is outside 10..100 — a refusal, not a clamp: silently sending a
// different rate than the caller asked for is how a driver ends up lying
// about its own sample rate.
std::size_t encode_rate_command(std::uint8_t hz, std::uint8_t out[kRateCommandBytes]);

// The vendor's sum8 over a whole frame minus its checksum byte.
std::uint8_t checksum8(const std::uint8_t* frame, std::size_t n_without_checksum);

const char* func_name(std::uint8_t func) noexcept;

}  // namespace juxi
}  // namespace scanengine

#endif  // SCANENGINE_DRIVERS_IMU_SERIAL_JUXI_FRAMES_H
