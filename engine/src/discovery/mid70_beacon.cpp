// mid70_beacon.cpp — decode the Livox Mid-70's 1 Hz SDK **v1** broadcast.
//
// This is NOT mid360_beacon.cpp with different offsets. The Mid-360 speaks
// SDK2 and broadcasts a ~430-byte key-value control frame to 56201; the
// Mid-70 speaks SDK v1 and broadcasts a fixed 50-byte command frame to 55000.
// Different port, different framing, different CRC parameters. The one thing
// they share is the 0xAA start byte, which is why a frame that arrives on the
// wrong port must be rejected by its LENGTH and its CRCs rather than waved
// through on its sof.
//
// GROUND TRUTH is the SDK source itself, vendored at
// spikes/s8-mid70-sdk1/Livox-SDK, cross-read against the second, independent
// copy inside livox_ros_driver (common/comm/sdk_protocol.cpp). Both agree
// byte for byte. What has NOT yet happened is a check against a real
// broadcast off real hardware — the Phase 0 fixture
// tests/integration/data/mid70_broadcast.bin is still owed, because the
// lidar is unplugged. Until it lands, "confirmed" here means "confirmed
// against two independent implementations of the encoder", not "confirmed
// against a wire". Say so out loud rather than let a reader assume the
// stronger claim.
//
// FRAME LAYOUT (SdkPacket, sdk_core/src/comm/sdk_protocol.h, #pragma pack(1))
//
//   off  size  field
//     0     1  sof            0xAA (kSdkProtocolSof)
//     1     1  version        1 — the enum is {kSdkVerNone=0, kSdkVer0=1,
//                             kSdkVer1=2} and Pack() writes kSdkVer0
//     2     2  length         WHOLE frame, LE
//     4     1  packet_type    kRequestPack 0 / kAckPack 1 / kMsgPack 2
//     5     2  seq_num        LE
//     7     2  preamble_crc   LE, over bytes 0..6 (GetPreambleLen()-2 == 7)
//     9     1  cmd_set        0x00 kCommandSetGeneral
//    10     1  cmd_id         0x00 kCommandIDGeneralBroadcast
//    11     …  payload        length - GetPacketWrapperLen() == length - 15
//   n-4     4  crc32          LE, over bytes 0 .. length-5
//
// and for a broadcast the payload is BroadcastDeviceInfo (livox_def.h, inside
// the file's #pragma pack(1) region), 16 + 1 + 2 + 16 = 35 bytes, making the
// whole frame 50.
//
// THE TWO CRCs — read out of the code, not guessed, and NEITHER is the stock
// algorithm its name suggests:
//
//   comm_port.cpp:37   protocol_ = new SdkProtocol(0x4c49, 0x564f580a);
//
//   preamble  FastCRC16::mcrf4xx_calc, i.e. CRC-16/MCRF4XX — reflected poly
//             0x8408 (0x1021 reversed), refin/refout true, xorout 0x0000 —
//             but seeded with Livox's 0x4C49 in place of MCRF4XX's standard
//             0xFFFF init. Call it CRC-16/MCRF4XX-with-init-0x4C49. A stock
//             CCITT-FALSE (or a stock MCRF4XX) rejects every real frame.
//
//   frame     FastCRC32::crc32_calc, which starts its register at
//             `seed ^ 0xFFFFFFFF` and finishes with `^ 0xFFFFFFFF` over the
//             standard 0xEDB88320 reflected table. With seed 0 that is
//             exactly CRC-32/ISO-HDLC (zlib); Livox passes 0x564F580A, so
//             the register starts at 0xA9B0A7F5 instead of 0xFFFFFFFF.
//             Call it CRC-32/ISO-HDLC-with-init-0x564F580A.
//
// Because the inits differ from the Mid-360's, the helpers in
// mid360_beacon.cpp are NOT shared — they are the same two polynomials with
// the wrong starting values, and a shared function with an init parameter
// would put two protocols' invariants in one place for no gain. Both are
// implemented below and both are NAMED.
//
// Owner: A16 (Phase 5 of the Mid-70 plan).
#include "scanengine/discovery/discovery.h"

#include <cstdio>
#include <cstring>

#include "scanengine/core/log.h"

namespace scanengine {
namespace discovery {
namespace {

constexpr std::uint8_t kSof = 0xAA;

// GetPreambleLen() == sizeof(SdkPreamble) == 9; the CRC covers all but its
// own two bytes.
constexpr std::size_t kPreambleBytes = 9;
constexpr std::size_t kPreambleCrcSpan = 7;
// The header up to and including cmd_id.
constexpr std::size_t kHeaderBytes = 11;
constexpr std::size_t kCrc32Bytes = 4;
// GetPacketWrapperLen() == sizeof(SdkPacket) - 1 + 4 == 15.
constexpr std::size_t kWrapperBytes = kHeaderBytes + kCrc32Bytes;

// sizeof(BroadcastDeviceInfo) under #pragma pack(1) is 35 — but that is the
// SDK's IN-MEMORY struct, not the wire. The SDK's own receiver
// (sdk_core/src/device_discovery.cpp:148) copies only
// `sizeof(BroadcastDeviceInfo) - sizeof(ip)` = 19 bytes out of the datagram
// (code[16] + dev_type + reserved[2]) and fills `ip` from the UDP source
// address (:152-155). So a real broadcast is 11 + 19 + 4 = 34 bytes and
// carries NO ip text. (Fable review, 2026-09-07.) The 35-byte form is still
// accepted in case a firmware ever does send the struct whole.
constexpr std::size_t kBroadcastInfoWireBytes = 19;
constexpr std::size_t kBroadcastInfoBytes = 35;
constexpr std::size_t kBroadcastCodeBytes = 16;  // kBroadcastCodeSize
constexpr std::size_t kIpTextBytes = 16;

constexpr std::uint8_t kCommandSetGeneral = 0x00;
constexpr std::uint8_t kCommandIdGeneralBroadcast = 0x00;

// The Livox seeds, straight from comm_port.cpp.
constexpr std::uint16_t kLivoxCrc16Init = 0x4C49;
constexpr std::uint32_t kLivoxCrc32Seed = 0x564F580Au;

std::uint16_t rd_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t rd_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// CRC-16/MCRF4XX with Livox's non-standard 0x4C49 init.
//
// FastCRC16::mcrf4xx_calc is a table walk of
//   crc = (crc >> 8) ^ table[(crc & 0xFF) ^ byte]
// over crc_table_mcrf4xx, whose entry 1 is 0x1189 — the signature of the
// reflected 0x8408 polynomial. Bit-serial here for the same reason
// mid360_beacon.cpp is: this runs once per 50-byte datagram at 1 Hz.
std::uint16_t crc16_mcrf4xx_livox(const std::uint8_t* p, std::size_t n) {
  std::uint16_t crc = kLivoxCrc16Init;
  for (std::size_t i = 0; i < n; ++i) {
    crc = static_cast<std::uint16_t>(crc ^ p[i]);
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1u) ? static_cast<std::uint16_t>((crc >> 1) ^ 0x8408u)
                       : static_cast<std::uint16_t>(crc >> 1);
    }
  }
  return crc;
}

// CRC-32/ISO-HDLC with Livox's non-standard init. `seed ^ 0xFFFFFFFF` is
// FastCRC32's own expression; with the SDK's 0x564F580A that is 0xA9B0A7F5.
std::uint32_t crc32_iso_livox(const std::uint8_t* p, std::size_t n) {
  std::uint32_t crc = kLivoxCrc32Seed ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

// livox_def.h DeviceType. Every value is NAMED rather than filtered: the
// point of discovery is to tell an operator what is on the wire, and
// "Horizon at 192.168.1.12" is a far better answer than silence when they
// have plugged in the wrong lidar.
const char* device_type_name(std::uint8_t t) {
  switch (t) {
    case 0: return "Hub";
    case 1: return "Mid-40";
    case 2: return "Tele";
    case 3: return "Horizon";
    case 6: return "Mid-70";
    case 7: return "Avia";
    default: return nullptr;
  }
}

// A NUL-padded fixed-length ASCII field, stripped of anything unprintable —
// a garbled broadcast code must not become a control-character injection
// into a log line or a Qt label. Same policy, same reasons, as
// mid360_beacon.cpp's printable_field().
std::string printable_field(const std::uint8_t* p, std::size_t n) {
  std::string s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const char c = static_cast<char>(p[i]);
    if (c == '\0') break;
    if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) s += c;
  }
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

}  // namespace

// --- CRCs (public: a diagnostic wants to know WHICH half failed) -----------

bool Mid70PreambleCrcOk(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len < kPreambleBytes) return false;
  return crc16_mcrf4xx_livox(data, kPreambleCrcSpan) == rd_u16(data + kPreambleCrcSpan);
  // Equivalent, and the form the SDK's CheckPreamble() uses:
  //   crc16_mcrf4xx_livox(data, kPreambleBytes) == 0
  // (a reflected CRC with xorout 0 leaves zero when run over message+CRC).
  // Spelled out as a comparison here so a failure can be logged with both
  // numbers.
}

bool Mid70FrameCrcOk(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len < kWrapperBytes) return false;
  std::size_t declared = rd_u16(data + 2);
  // Unlike the Mid-360's advisory heartbeat, a length that does not fit the
  // datagram is not something to clamp and carry on with: the CRC32 sits AT
  // that offset, so a wrong length means we would be checksumming against
  // four arbitrary bytes. Say no.
  if (declared < kWrapperBytes || declared > len) return false;
  return crc32_iso_livox(data, declared - kCrc32Bytes) == rd_u32(data + declared - kCrc32Bytes);
}

// --- the parser ------------------------------------------------------------

Result<Mid70Beacon> ParseMid70Beacon(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "mid70 beacon: null buffer");
  }
  if (len < kWrapperBytes) {
    return set_last_error(ScanError::kProtocolError, "mid70 beacon: %zu bytes is too short",
                          len);
  }
  if (data[0] != kSof) {
    return set_last_error(ScanError::kProtocolError,
                          "mid70 beacon: sof 0x%02X, expected 0xAA", data[0]);
  }

  const std::size_t declared = rd_u16(data + 2);
  if (declared != len) {
    // A Mid-360 heartbeat arriving on the wrong socket dies here, as does a
    // truncated datagram and anything else 0xAA-shaped: the SDK v1 frame is
    // self-delimiting and a broadcast arrives one-per-datagram, so the
    // declared length and the datagram length must agree EXACTLY.
    return set_last_error(ScanError::kProtocolError,
                          "mid70 beacon: declares %zu bytes, datagram is %zu", declared, len);
  }

  if (!Mid70PreambleCrcOk(data, len)) {
    return set_last_error(ScanError::kChecksumFailed,
                          "mid70 beacon: preamble crc16 0x%04X, computed 0x%04X",
                          static_cast<unsigned>(rd_u16(data + kPreambleCrcSpan)),
                          static_cast<unsigned>(crc16_mcrf4xx_livox(data, kPreambleCrcSpan)));
  }
  if (!Mid70FrameCrcOk(data, len)) {
    return set_last_error(ScanError::kChecksumFailed,
                          "mid70 beacon: frame crc32 0x%08X does not verify",
                          rd_u32(data + len - kCrc32Bytes));
  }

  const std::uint8_t cmd_set = data[9];
  const std::uint8_t cmd_id = data[10];
  if (cmd_set != kCommandSetGeneral || cmd_id != kCommandIdGeneralBroadcast) {
    // A well-formed SDK v1 frame that is not a broadcast — a handshake ACK
    // that leaked onto 55000, say. Correct protocol, wrong message.
    return set_last_error(ScanError::kProtocolError,
                          "mid70 beacon: cmd_set/cmd_id %u/%u, expected 0/0",
                          static_cast<unsigned>(cmd_set), static_cast<unsigned>(cmd_id));
  }

  const std::size_t payload = declared - kWrapperBytes;
  if (payload != kBroadcastInfoWireBytes && payload != kBroadcastInfoBytes) {
    return set_last_error(ScanError::kProtocolError,
                          "mid70 beacon: %zu-byte payload, expected %zu (the wire form: "
                          "code[16] + dev_type + reserved[2]) or %zu (sizeof "
                          "BroadcastDeviceInfo)",
                          payload, kBroadcastInfoWireBytes, kBroadcastInfoBytes);
  }

  const std::uint8_t* info = data + kHeaderBytes;
  Mid70Beacon b;
  b.broadcast_code = printable_field(info, kBroadcastCodeBytes);
  b.dev_type = info[kBroadcastCodeBytes];
  // info + 17..18 is BroadcastDeviceInfo::reserved. The ip text exists only
  // in the 35-byte form; on the real 34-byte wire the lidar's address is the
  // datagram's source address, which DiscoverMid70 records as source_ip.
  if (payload == kBroadcastInfoBytes) {
    b.lidar_ip = printable_field(info + kBroadcastCodeBytes + 3, kIpTextBytes);
  }

  const char* name = device_type_name(b.dev_type);
  if (name != nullptr) {
    b.dev_type_name = name;
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "unknown (%u)", static_cast<unsigned>(b.dev_type));
    b.dev_type_name = buf;
  }
  return b;
}

std::string Mid70Beacon::describe() const {
  std::string s = dev_type_name.empty() ? std::string("Livox") : dev_type_name;
  if (!broadcast_code.empty()) s += " " + broadcast_code;
  if (!lidar_ip.empty()) s += " at " + lidar_ip;
  else if (!source_ip.empty()) s += " from " + source_ip;
  if (dev_type != 6) s += " (not a Mid-70)";
  return s;
}

}  // namespace discovery
}  // namespace scanengine
