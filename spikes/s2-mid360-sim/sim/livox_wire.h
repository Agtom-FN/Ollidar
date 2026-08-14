// S2-sim — Livox Mid-360 wire-format helpers (lidar side).
//
// Layouts here are cross-checked against two independent sources:
//   [1] Livox-SDK2 source (third_party/Livox-SDK2):
//         sdk_core/comm/sdk_protocol.h      -> SdkPreamble / SdkPacket (control frame)
//         sdk_core/comm/define.h            -> DetectionData, command IDs, port constants
//         include/livox_lidar_def.h         -> LivoxLidarEthernetPacket, point/IMU structs
//   [2] Official Livox wiki, "Livox LiDAR Communication Protocol -- Mid360":
//         https://livox-wiki-en.readthedocs.io/en/latest/tutorials/new_product/mid360/
//                livox_eth_protocol_mid360.html
//
// CRCs are computed with the SDK's own FastCRC implementation so the bytes are
// bit-identical to what the SDK verifies (crc16 = CCITT-FALSE over the first 18
// header bytes, crc32 = CRC-32/ISO-HDLC over the payload).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "livox_lidar_def.h"

#include "FastCRC/FastCRC.h"

namespace s2sim {

// ---------------------------------------------------------------------------
// Control-frame (command) protocol -- 24-byte header, little endian.
// Ref [1] SdkPacket, Ref [2] table "control command frame format".
// ---------------------------------------------------------------------------
constexpr uint8_t kSof = 0xAA;
constexpr uint8_t kProtoVersion = 0;
constexpr uint32_t kCmdHeaderLen = 24;

// cmd_type
constexpr uint8_t kCmdTypeReq = 0;
constexpr uint8_t kCmdTypeAck = 1;
// sender_type
constexpr uint8_t kSenderHost = 0;
constexpr uint8_t kSenderLidar = 1;

// Command IDs (Ref [1] sdk_core/comm/define.h LidarCommandID)
constexpr uint16_t kCmdIdSearch = 0x0000;
constexpr uint16_t kCmdIdWorkModeControl = 0x0100;
constexpr uint16_t kCmdIdGetInternalInfo = 0x0101;
constexpr uint16_t kCmdIdPushMsg = 0x0102;

// Default Mid-360 UDP ports (Ref [1] define.h, Ref [2] port table)
constexpr uint16_t kDetectionPort = 56000;
constexpr uint16_t kLidarCmdPort = 56100;
constexpr uint16_t kLidarPushPort = 56200;
constexpr uint16_t kLidarPointPort = 56300;
constexpr uint16_t kLidarImuPort = 56400;
constexpr uint16_t kLidarLogPort = 56500;

constexpr uint8_t kDevTypeMid360 = 9;  // kLivoxLidarTypeMid360

#pragma pack(push, 1)
struct CmdHeader {
  uint8_t sof;
  uint8_t version;
  uint16_t length;  // whole frame, header + payload
  uint32_t seq_num;
  uint16_t cmd_id;
  uint8_t cmd_type;
  uint8_t sender_type;
  uint8_t rsvd[6];
  uint16_t crc16_h;  // CCITT-FALSE over bytes [0,18)
  uint32_t crc32_d;  // CRC-32 over payload (0 when payload empty)
};
static_assert(sizeof(CmdHeader) == kCmdHeaderLen, "control frame header must be 24 bytes");

// Discovery (cmd 0x0000) ACK payload. Ref [1] define.h DetectionData.
struct DetectionAck {
  uint8_t ret_code;
  uint8_t dev_type;
  char sn[16];
  uint8_t lidar_ip[4];
  uint16_t cmd_port;
};
static_assert(sizeof(DetectionAck) == 24, "detection ack payload must be 24 bytes");

// Point-cloud / IMU UDP data packet header.
// Ref [1] LivoxLidarEthernetPacket, Ref [2] table "point cloud data packet".
struct DataHeader {
  uint8_t version;        // 0
  uint16_t length;        // whole UDP datagram length
  uint16_t time_interval; // 0.1 us, last point time - first point time
  uint16_t dot_num;
  uint16_t udp_cnt;       // packet counter inside the frame
  uint8_t frame_cnt;
  uint8_t data_type;      // 0 = IMU, 1 = cartesian 32-bit, 2 = cartesian 16-bit
  uint8_t time_type;      // 0 = lidar-local (power-on) time
  uint8_t rsvd[12];
  uint32_t crc32;         // over timestamp + data
  uint64_t timestamp;     // ns
};
static_assert(sizeof(DataHeader) == 36, "data packet header must be 36 bytes");

struct CartesianHigh {  // data_type == 1
  int32_t x, y, z;      // mm
  uint8_t reflectivity;
  uint8_t tag;
};
static_assert(sizeof(CartesianHigh) == 14, "cartesian-high point must be 14 bytes");

struct ImuSample {  // data_type == 0
  float gyro_x, gyro_y, gyro_z;  // rad/s
  float acc_x, acc_y, acc_z;     // g
};
static_assert(sizeof(ImuSample) == 24, "IMU sample must be 24 bytes");
#pragma pack(pop)

// The canonical Mid-360 point packet: 36 + 96*14 = 1380 bytes.
constexpr uint16_t kPointsPerPacket = 96;
constexpr size_t kPointPacketBytes = sizeof(DataHeader) + kPointsPerPacket * sizeof(CartesianHigh);
static_assert(kPointPacketBytes == 1380, "Mid-360 point packet is 1380 bytes");
constexpr size_t kImuPacketBytes = sizeof(DataHeader) + sizeof(ImuSample);

inline uint16_t Crc16Header(const uint8_t* buf) {
  FastCRC16 c;
  return c.ccitt(buf, 18);
}

inline uint32_t Crc32(const uint8_t* buf, size_t len) {
  if (len == 0) return 0;
  FastCRC32 c;
  return c.crc32(buf, len);
}

// Build a complete control frame into `out`.
inline void BuildCmdFrame(std::vector<uint8_t>& out, uint32_t seq_num, uint16_t cmd_id,
                          uint8_t cmd_type, uint8_t sender_type, const uint8_t* payload,
                          uint16_t payload_len) {
  out.assign(kCmdHeaderLen + payload_len, 0);
  auto* h = reinterpret_cast<CmdHeader*>(out.data());
  h->sof = kSof;
  h->version = kProtoVersion;
  h->length = static_cast<uint16_t>(kCmdHeaderLen + payload_len);
  h->seq_num = seq_num;
  h->cmd_id = cmd_id;
  h->cmd_type = cmd_type;
  h->sender_type = sender_type;
  if (payload_len) std::memcpy(out.data() + kCmdHeaderLen, payload, payload_len);
  h->crc32_d = Crc32(out.data() + kCmdHeaderLen, payload_len);
  h->crc16_h = Crc16Header(out.data());
}

// Validate and split an incoming control frame. Returns false on any check failure.
inline bool ParseCmdFrame(const uint8_t* buf, size_t len, CmdHeader& hdr, const uint8_t*& payload,
                          uint16_t& payload_len) {
  if (len < kCmdHeaderLen) return false;
  std::memcpy(&hdr, buf, sizeof(hdr));
  if (hdr.sof != kSof || hdr.version != kProtoVersion) return false;
  if (hdr.length < kCmdHeaderLen || hdr.length > len) return false;
  if (hdr.crc16_h != Crc16Header(buf)) return false;
  payload_len = static_cast<uint16_t>(hdr.length - kCmdHeaderLen);
  payload = buf + kCmdHeaderLen;
  if (hdr.crc32_d != Crc32(payload, payload_len)) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Key/value parameter list helpers.
// Request/response body layout: [key_num u16][rsvd u16] then N * (key u16, len u16, value[len]).
// For a *query* (cmd 0x0101 request) only the bare u16 keys follow the header.
// ---------------------------------------------------------------------------
class KvWriter {
 public:
  KvWriter() { buf_.resize(4, 0); }
  void Add(uint16_t key, const void* value, uint16_t len) {
    size_t off = buf_.size();
    buf_.resize(off + 4 + len);
    std::memcpy(buf_.data() + off, &key, 2);
    std::memcpy(buf_.data() + off + 2, &len, 2);
    if (len) std::memcpy(buf_.data() + off + 4, value, len);
    ++count_;
  }
  template <typename T>
  void AddScalar(uint16_t key, T v) {
    Add(key, &v, static_cast<uint16_t>(sizeof(T)));
  }
  void AddString(uint16_t key, const std::string& s, uint16_t field_len) {
    std::vector<char> tmp(field_len, 0);
    std::memcpy(tmp.data(), s.data(), std::min<size_t>(s.size(), field_len - 1));
    Add(key, tmp.data(), field_len);
  }
  // Finish with the leading [key_num][rsvd] header filled in.
  const std::vector<uint8_t>& Finish() {
    std::memcpy(buf_.data(), &count_, 2);
    return buf_;
  }
  uint16_t count() const { return count_; }

 private:
  std::vector<uint8_t> buf_;
  uint16_t count_ = 0;
};

struct KvItem {
  uint16_t key;
  uint16_t len;
  const uint8_t* value;
};

// Parse a KV *list* body (key_num, rsvd, then key/len/value triples).
inline bool ParseKvList(const uint8_t* body, uint16_t body_len, std::vector<KvItem>& out) {
  if (body_len < 4) return false;
  uint16_t n = 0;
  std::memcpy(&n, body, 2);
  uint16_t off = 4;
  for (uint16_t i = 0; i < n; ++i) {
    if (off + 4u > body_len) return false;
    KvItem it{};
    std::memcpy(&it.key, body + off, 2);
    std::memcpy(&it.len, body + off + 2, 2);
    it.value = body + off + 4;
    if (off + 4u + it.len > body_len) return false;
    out.push_back(it);
    off = static_cast<uint16_t>(off + 4 + it.len);
  }
  return true;
}

// Parse a query body (key_num, rsvd, then bare u16 keys).
inline bool ParseKeyQuery(const uint8_t* body, uint16_t body_len, std::vector<uint16_t>& out) {
  if (body_len < 4) return false;
  uint16_t n = 0;
  std::memcpy(&n, body, 2);
  uint16_t off = 4;
  for (uint16_t i = 0; i < n; ++i) {
    if (off + 2u > body_len) return false;
    uint16_t k = 0;
    std::memcpy(&k, body + off, 2);
    out.push_back(k);
    off += 2;
  }
  return true;
}

// The host-IP config value carried by keys 0x0005/0x0006/0x0007.
// Ref [1] define.h HostIpInfoValue.
#pragma pack(push, 1)
struct HostIpInfoValue {
  uint8_t host_ip[4];
  uint16_t host_port;
  uint16_t lidar_port;
};
#pragma pack(pop)
static_assert(sizeof(HostIpInfoValue) == 8, "host ip cfg value must be 8 bytes");

}  // namespace s2sim
