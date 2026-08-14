// COIN-D6 host->device commands and device->host ACK / info frames.
//
// Source of truth: "COIN-D6 LiDAR Data Format Standard Specification V1.0"
// sections 1 and 2.1, cross-checked against the vendor ROS2 SDK
// (sdk/lidar_information.h  -> start_lidar/end_lidar/...,
//  sdk/lidar_data_processing.cpp -> Lidar_Data_Processing::wait_start_reply).
//
// Header-only, C++17, no dependencies outside the standard library.

#ifndef D6_COMMANDS_H
#define D6_COMMANDS_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace d6 {

// ---------------------------------------------------------------------------
// Host -> device commands (spec §1; vendor lidar_information.h)
// ---------------------------------------------------------------------------

inline constexpr uint8_t kCmdStart[4]        = {0xAA, 0x55, 0xF0, 0x0F};
inline constexpr uint8_t kCmdStop[4]         = {0xAA, 0x55, 0xF5, 0x0A};
// Not documented in the D6 spec text, present in the vendor SDK. Kept for
// bring-up experiments only -- do not use in the shipping driver without
// vendor confirmation for the D6 specifically.
inline constexpr uint8_t kCmdLowExposure[4]  = {0xAA, 0x55, 0xF1, 0x0E};
inline constexpr uint8_t kCmdHighExposure[4] = {0xAA, 0x55, 0xF2, 0x0D};
inline constexpr uint8_t kCmdGetVersion[4]   = {0xAA, 0x55, 0xFC, 0x03};

// ---------------------------------------------------------------------------
// Device -> host ACK frames (spec §1)
//
//   start OK : A5 5A 50 07 00 00 00 00 00 00 00 A8
//   stop  OK : A5 5A 55 07 00 00 00 00 00 00 00 AD
//   error    : A5 5A 55 07 00 00 00 00 00 00 00 E9
//
// The trailing byte is the XOR of the preceding 11 bytes for both OK frames
// (verified: A5^5A^50^07 = 0xA8, A5^5A^55^07 = 0xAD -- the vendor's
// wait_start_reply() implements exactly this XOR-and-compare). The error frame
// deliberately carries a *wrong* XOR (0xE9), which is how it is told apart from
// the stop-OK frame: both have the same first 11 bytes.
// ---------------------------------------------------------------------------

inline constexpr size_t  kAckLen        = 12;
inline constexpr uint8_t kAckSync0      = 0xA5;
inline constexpr uint8_t kAckSync1      = 0x5A;
inline constexpr uint8_t kAckTypeStart  = 0x50;  // byte 2 of the start-OK frame
inline constexpr uint8_t kAckTypeStop   = 0x55;  // byte 2 of the stop-OK / error frame
inline constexpr uint8_t kAckLenField   = 0x07;  // byte 3
inline constexpr uint8_t kAckErrorXor   = 0xE9;  // trailing byte of the error frame

enum class Ack {
  kNone = 0,   // no complete ACK frame found
  kStartOk,    // scanning started
  kStopOk,     // scanning stopped
  kError,      // device reported an error for the last command
  kUnknown,    // A5 5A framed, right length, but neither of the above
};

// XOR of the first 11 bytes of a 12-byte ACK frame.
inline uint8_t ack_xor(const uint8_t* p) {
  uint8_t x = 0;
  for (size_t i = 0; i + 1 < kAckLen; ++i) x ^= p[i];
  return x;
}

// Classify exactly one 12-byte candidate frame at `p`.
inline Ack classify_ack(const uint8_t* p, size_t n) {
  if (n < kAckLen) return Ack::kNone;
  if (p[0] != kAckSync0 || p[1] != kAckSync1) return Ack::kNone;
  if (p[3] != kAckLenField) return Ack::kUnknown;
  const uint8_t trailer = p[kAckLen - 1];
  if (trailer == kAckErrorXor) return Ack::kError;
  if (trailer != ack_xor(p)) return Ack::kUnknown;
  if (p[2] == kAckTypeStart) return Ack::kStartOk;
  if (p[2] == kAckTypeStop) return Ack::kStopOk;
  return Ack::kUnknown;
}

// Scan a receive buffer for the first complete ACK frame.
// Returns the classification; on a hit `offset` is the frame start index.
// Returns kNone when no complete frame is present (the caller should keep
// buffering; a trailing partial "A5 5A ..." is not consumed).
inline Ack find_ack(const uint8_t* buf, size_t n, size_t* offset) {
  if (n < kAckLen) return Ack::kNone;
  for (size_t i = 0; i + kAckLen <= n; ++i) {
    if (buf[i] != kAckSync0 || buf[i + 1] != kAckSync1) continue;
    const Ack a = classify_ack(buf + i, n - i);
    if (a != Ack::kNone && a != Ack::kUnknown) {
      if (offset) *offset = i;
      return a;
    }
  }
  return Ack::kNone;
}

inline const char* to_string(Ack a) {
  switch (a) {
    case Ack::kNone:    return "none";
    case Ack::kStartOk: return "start-ok";
    case Ack::kStopOk:  return "stop-ok";
    case Ack::kError:   return "error";
    case Ack::kUnknown: return "unknown";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Device information frame (spec §2.1) -- the first thing the device sends
// after power-up.
//
//   A5 5A  14 00  E3 02  01  <20 byte data area>
//   |      |      |      |
//   |      |      |      +-- type 0x01 = information upload (0x81 = data)
//   |      |      +--------- checksum: SUM of every byte except these two
//   |      +---------------- payload length, LE (0x0014 = 20)
//   +----------------------- header
//
// NOTE the checksum here is an additive sum, unlike the XOR used by the ACK
// frames and the *word* XOR used by the point-cloud packets. Verified against
// the worked example in the spec (sum = 0x02E3).
// ---------------------------------------------------------------------------

inline constexpr size_t kInfoHeaderLen  = 7;
inline constexpr size_t kInfoPayloadLen = 20;
inline constexpr size_t kInfoFrameLen   = kInfoHeaderLen + kInfoPayloadLen;  // 27
inline constexpr uint8_t kInfoTypeInfo  = 0x01;
inline constexpr uint8_t kInfoTypeData  = 0x81;

struct DeviceInfo {
  char     model[13];        // bytes 1-12, NUL terminated ("COIN-D6")
  int16_t  zero_offset;      // bytes 13-14, LE; D6 compensates mechanically -> ignore
  uint8_t  direction;        // byte 15, 0x00 = clockwise
  uint8_t  needs_angle_fix;  // byte 16
  uint8_t  version;          // byte 20, software revision
};

// Parse (and checksum-verify) a 27-byte information frame at `p`.
inline bool parse_device_info(const uint8_t* p, size_t n, DeviceInfo* out) {
  if (n < kInfoFrameLen) return false;
  if (p[0] != kAckSync0 || p[1] != kAckSync1) return false;
  const uint16_t len = static_cast<uint16_t>(p[2] | (p[3] << 8));
  if (len != kInfoPayloadLen) return false;
  if (p[6] != kInfoTypeInfo) return false;
  const uint16_t cs = static_cast<uint16_t>(p[4] | (p[5] << 8));
  uint32_t sum = 0;
  for (size_t i = 0; i < kInfoFrameLen; ++i) {
    if (i == 4 || i == 5) continue;  // the checksum field itself
    sum += p[i];
  }
  if (static_cast<uint16_t>(sum) != cs) return false;
  if (out) {
    const uint8_t* d = p + kInfoHeaderLen;
    std::memcpy(out->model, d, 12);
    out->model[12] = '\0';
    out->zero_offset     = static_cast<int16_t>(d[12] | (d[13] << 8));
    out->direction       = d[14];
    out->needs_angle_fix = d[15];
    out->version         = d[19];
  }
  return true;
}

}  // namespace d6

#endif  // D6_COMMANDS_H
