#include "scanengine/drivers/imu_serial/juxi_frames.h"

namespace scanengine {
namespace juxi {
namespace {

// Little-endian int16, exactly the vendor's to_int16().
std::int16_t to_i16(const std::uint8_t* b) {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[0]) |
                                   (static_cast<std::uint16_t>(b[1]) << 8));
}

}  // namespace

std::uint8_t checksum8(const std::uint8_t* frame, std::size_t n_without_checksum) {
  std::uint8_t sum = 0;
  for (std::size_t i = 0; i < n_without_checksum; ++i) sum = static_cast<std::uint8_t>(sum + frame[i]);
  return sum;
}

bool decode_raw_imu(const std::uint8_t* payload, std::size_t n, RawImuFrame* out) {
  if (payload == nullptr || out == nullptr || n != kRawImuPayloadBytes) return false;
  for (int i = 0; i < 3; ++i) {
    out->accel_g[i] = static_cast<float>(to_i16(payload + 2 * i)) * kAccelScaleG;
    out->gyro_rad_s[i] = static_cast<float>(to_i16(payload + 6 + 2 * i)) * kGyroScaleRadS;
    out->mag_ut[i] = static_cast<float>(to_i16(payload + 12 + 2 * i)) * kMagScaleUt;
  }
  return true;
}

std::size_t encode_rate_command(std::uint8_t hz, std::uint8_t out[kRateCommandBytes]) {
  if (out == nullptr || hz < kMinRateHz || hz > kMaxRateHz) return 0;
  out[0] = kHead1;
  out[1] = kHead2;
  out[2] = static_cast<std::uint8_t>(kRateCommandBytes);  // len counts the whole frame
  out[3] = kFuncSetRate;
  out[4] = hz;
  out[5] = kSetRateTrailer;
  out[6] = checksum8(out, kRateCommandBytes - 1);
  return kRateCommandBytes;
}

const char* func_name(std::uint8_t func) noexcept {
  switch (func) {
    case kFuncVersion: return "version";
    case kFuncRawImu: return "raw-imu";
    case kFuncQuaternion: return "quaternion";
    case kFuncEuler: return "euler";
    case kFuncBarometer: return "barometer";
    case kFuncRequestData: return "request-data";
    case kFuncReturnState: return "return-state";
    case kFuncSetRate: return "set-rate";
    default: return "unknown";
  }
}

void FrameParser::reset() {
  state_ = State::kHead1;
  len_ = 0;
  func_ = 0;
  body_len_ = 0;
  body_index_ = 0;
  stats_ = FrameStats{};
}

void FrameParser::dispatch(const RawFrameCallback& on_raw) {
  // body_ holds payload + checksum. Verify sum8 over head1, head2, len, func
  // and the payload — the vendor's calculation, byte for byte.
  const std::size_t payload_len = body_len_ - 1;
  std::uint8_t sum = static_cast<std::uint8_t>(kHead1 + kHead2 + len_ + func_);
  for (std::size_t i = 0; i < payload_len; ++i) sum = static_cast<std::uint8_t>(sum + body_[i]);

  if (sum != body_[payload_len]) {
    ++stats_.checksum_failures;
    return;
  }

  switch (func_) {
    case kFuncRawImu: {
      RawImuFrame f;
      if (!decode_raw_imu(body_, payload_len, &f)) {
        // Right func, wrong length: a truncated or extended 0x04 is not a
        // sample and must not be published as one.
        ++stats_.unknown_frames;
        return;
      }
      ++stats_.raw_frames;
      if (on_raw) on_raw(f);
      break;
    }
    case kFuncQuaternion: ++stats_.quaternion_frames; break;
    case kFuncEuler: ++stats_.euler_frames; break;
    case kFuncBarometer: ++stats_.barometer_frames; break;
    case kFuncVersion: ++stats_.version_frames; break;
    case kFuncReturnState: ++stats_.state_frames; break;
    default: ++stats_.unknown_frames; break;
  }
}

void FrameParser::push(const std::uint8_t* bytes, std::size_t n, const RawFrameCallback& on_raw) {
  if (bytes == nullptr) return;
  stats_.bytes_in += n;

  for (std::size_t i = 0; i < n; ++i) {
    const std::uint8_t b = bytes[i];
    switch (state_) {
      case State::kHead1:
        if (b == kHead1) state_ = State::kHead2;
        // else: hunting. Not counted as a resync — a byte that is not 0x7E
        // while we are looking for one is the normal shape of garbage, and
        // counting each of them would drown the counter that matters.
        break;

      case State::kHead2:
        if (b == kHead2) {
          state_ = State::kLength;
        } else {
          ++stats_.resyncs;
          // 0x7E 0x7E is a legal start of a frame whose first header byte was
          // the previous one; re-test this byte as head1 rather than dropping it.
          state_ = (b == kHead1) ? State::kHead2 : State::kHead1;
        }
        break;

      case State::kLength:
        len_ = b;
        // len counts the whole frame, so anything under 5 has no room for a
        // payload byte plus a checksum. The vendor rejects the same range
        // (its data_length == 0 test) and, like it, we reject anything that
        // would not fit its 64-byte body buffer.
        if (len_ < kFrameOverheadBytes ||
            static_cast<std::size_t>(len_) - 4u > kMaxPayloadPlusChecksum) {
          ++stats_.resyncs;
          state_ = State::kHead1;
          break;
        }
        body_len_ = static_cast<std::size_t>(len_) - 4u;  // payload + checksum
        state_ = State::kFunction;
        break;

      case State::kFunction:
        func_ = b;
        body_index_ = 0;
        state_ = State::kBody;
        break;

      case State::kBody:
        body_[body_index_++] = b;
        if (body_index_ >= body_len_) {
          dispatch(on_raw);
          state_ = State::kHead1;
        }
        break;
    }
  }
}

}  // namespace juxi
}  // namespace scanengine
