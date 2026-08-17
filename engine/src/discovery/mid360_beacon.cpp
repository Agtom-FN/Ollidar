// mid360_beacon.cpp — decode the Mid-360's 1 Hz broadcast heartbeat.
//
// GROUND TRUTH is captures/mid360_real_30s.livoxdump, port 56201: thirty
// 430-byte datagrams from SN ARMCP7K0034759 on 2026-08-17. Two of them are
// committed verbatim as tests/integration/data/mid360_beacon.bin, and every
// offset, key id and CRC convention below was read off those bytes rather
// than off a datasheet. Where the two would disagree, the bytes win.
//
// FRAME LAYOUT (SDK2 control frame, 24-byte header + key-value payload)
//
//   off  size  field
//     0     1  sof            0xAA
//     1     1  version        0
//     2     2  length         WHOLE frame, LE (430 in the field capture)
//     4     4  seq_num        increments once per heartbeat
//     8     2  cmd_id         0x0102 observed; NOT required (see below)
//    10     1  cmd_type
//    11     1  sender_type    1 = lidar
//    12     6  reserved
//    18     2  crc16          CRC16-CCITT-FALSE over bytes 0..17
//    20     4  crc32          CRC32 (ISO-HDLC / zlib) over bytes 24..length
//    24     2  key_num        number of key-value pairs (31 in the capture)
//    26     2  reserved
//    28     …  key_num × { u16 key, u16 len, u8 value[len] }
//
// Both CRCs were BRUTE-FORCED against the capture rather than assumed:
// CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) reproduces 0x5BC6 and
// the zlib CRC32 reproduces 0x127CB7CB for the first record. That is why they
// are checked here at all — an unverified checksum is worse than none,
// because it rejects good frames.
//
// Owner: A16.
#include "scanengine/discovery/discovery.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "scanengine/core/log.h"
#include "scanengine/core/types.h"

namespace scanengine {
namespace discovery {
namespace {

constexpr const char* kMod = "discovery";

constexpr std::uint8_t kSof = 0xAA;
constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kCrc16Span = 18;   // bytes 0..17 feed the header CRC
constexpr std::size_t kPayloadKvStart = 28;

// The keys this module reads. Every other key in the frame is skipped by
// length, which is what makes a firmware that adds keys a non-event.
constexpr std::uint16_t kKeyLidarIpCfg = 0x0004;        // ip + mask + gateway
constexpr std::uint16_t kKeyStateInfoHostCfg = 0x0005;  // where the beacon goes
constexpr std::uint16_t kKeyPointHostCfg = 0x0006;      // the PERSISTED host
constexpr std::uint16_t kKeyImuHostCfg = 0x0007;
constexpr std::uint16_t kKeySn = 0x8000;
constexpr std::uint16_t kKeyProductInfo = 0x8001;
constexpr std::uint16_t kKeyVersionApp = 0x8002;
constexpr std::uint16_t kKeyMac = 0x8005;

std::uint16_t rd_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t rd_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// CRC16-CCITT-FALSE. Bit-serial: this runs once per 430-byte datagram at
// 1 Hz, so a table would be 512 bytes of cache for nothing.
std::uint16_t crc16_ccitt_false(const std::uint8_t* p, std::size_t n) {
  std::uint16_t crc = 0xFFFF;
  for (std::size_t i = 0; i < n; ++i) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(p[i]) << 8));
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<std::uint16_t>(crc << 1);
    }
  }
  return crc;
}

// CRC32 ISO-HDLC (the zlib/PNG one). The Mid-360 POINT path already has a
// crc32 in drivers/mid360; this file does not reach into another workstream's
// internals for eight lines of arithmetic.
std::uint32_t crc32_iso(const std::uint8_t* p, std::size_t n) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

std::string ipv4_from_bytes(const std::uint8_t* p) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
  return buf;
}

// A NUL-padded fixed-length field, trimmed of trailing NULs and whitespace
// and stripped of anything unprintable — a garbled SN must not become a
// control-character injection into a log line or a Qt label.
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

// "DevType:Mid-360 FmType:App FmVer:35010108 BuildTime:2025/06/09" → the four
// fields. Tolerant by construction: a missing key leaves its output empty and
// an unknown key is ignored, so firmware that adds a field costs nothing.
void split_product_info(const std::string& info, Mid360Beacon* out) {
  std::size_t i = 0;
  while (i < info.size()) {
    const std::size_t colon = info.find(':', i);
    if (colon == std::string::npos) break;
    const std::string key = info.substr(i, colon - i);
    std::size_t end = info.find(' ', colon + 1);
    if (end == std::string::npos) end = info.size();
    const std::string value = info.substr(colon + 1, end - colon - 1);
    if (key == "DevType") out->dev_type = value;
    else if (key == "FmType") out->fw_type = value;
    else if (key == "FmVer") out->fw_version_text = value;
    else if (key == "BuildTime") out->build_time = value;
    i = end + 1;
  }
}

bool plausible_unicast(const std::uint8_t* p) {
  if (p[0] == 0 || p[0] >= 224) return false;               // 0/8, multicast, broadcast
  if (p[0] == 255 || (p[0] == 127)) return false;           // broadcast, loopback
  if (p[0] == 255 && p[1] == 255 && p[2] == 255) return false;
  return true;
}

// A contiguous IPv4 netmask: 1-bits then 0-bits, and not all-zero.
bool looks_like_netmask(const std::uint8_t* p) {
  const std::uint32_t m = (static_cast<std::uint32_t>(p[0]) << 24) |
                          (static_cast<std::uint32_t>(p[1]) << 16) |
                          (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
  if (m == 0 || m == 0xFFFFFFFFu) return false;
  const std::uint32_t inverted = ~m;
  return (inverted & (inverted + 1u)) == 0u;  // 0...01...1
}

// --- the fallback ---------------------------------------------------------
//
// Entered when the key-value walk cannot make sense of the frame: a firmware
// that reorders or extends the payload, a datagram clipped by a middlebox, a
// vendor that renumbers a key. The heartbeat's VALUE to the wizard (which
// lidar, at which IP, expecting which host) survives all of that because it
// is still present as text and as IPv4-shaped bytes. Anchor on the text,
// scan for the addresses, flag the record `heuristic` so the UI can say so.
bool heuristic_parse(const std::uint8_t* d, std::size_t n, Mid360Beacon* out) {
  const std::string text(reinterpret_cast<const char*>(d), n);

  // 1. The DevType: anchor and the printable run that carries it.
  const std::size_t anchor = text.find("DevType:");
  if (anchor != std::string::npos) {
    std::size_t end = anchor;
    while (end < n && d[end] >= 0x20 && d[end] < 0x7F) ++end;
    out->product_info = text.substr(anchor, end - anchor);
    split_product_info(out->product_info, out);
  }

  // 2. The SN: the last long alphanumeric run BEFORE the anchor. In the field
  //    frame that is exactly the key-0x8000 value, which sits immediately
  //    ahead of the product-info string.
  const std::size_t limit = (anchor == std::string::npos) ? n : anchor;
  std::size_t run_start = 0;
  bool in_run = false;
  for (std::size_t i = 0; i <= limit; ++i) {
    const bool alnum =
        i < limit && ((d[i] >= '0' && d[i] <= '9') || (d[i] >= 'A' && d[i] <= 'Z'));
    if (alnum && !in_run) {
      in_run = true;
      run_start = i;
    } else if (!alnum && in_run) {
      in_run = false;
      if (i - run_start >= 8) {
        out->sn.assign(reinterpret_cast<const char*>(d + run_start), i - run_start);
      }
    }
  }

  // 3. The addresses. The lidar's own IP is the one FOLLOWED by a contiguous
  //    netmask AND a gateway on the same subnet — a triple, not a single
  //    field. That triple is what makes it identifiable without knowing the
  //    key layout, and it is strong enough to survive a frame full of
  //    IPv4-shaped noise: a CRC32 word looks like an address, but the twelve
  //    bytes after it do not look like a mask and a gateway.
  //
  //    The scan starts AFTER the 24-byte header for the same reason — the
  //    sequence number and the two CRCs are exactly the kind of high-entropy
  //    bytes that produce a plausible-looking address.
  std::size_t lidar_at = n;
  for (std::size_t i = (n > kHeaderBytes ? kHeaderBytes : 0); i + 12 <= n; ++i) {
    if (!plausible_unicast(d + i)) continue;
    if (!looks_like_netmask(d + i + 4)) continue;
    const std::string ip = ipv4_from_bytes(d + i);
    const std::string mask = ipv4_from_bytes(d + i + 4);
    const std::string gw = ipv4_from_bytes(d + i + 8);
    // A /8..../30 mask: /0..7 and /31,/32 are legal in principle and are
    // never how a lidar is configured, so they are far more likely to be
    // coincidence than configuration.
    const int prefix = PrefixLen(mask);
    if (prefix < 8 || prefix > 30) continue;
    // The gateway lives on the interface's own subnet. This is the test that
    // rejects the header's CRC bytes.
    if (!SameSubnet(ip, gw, mask)) continue;
    out->lidar_ip = ip;
    out->netmask = mask;
    out->gateway = gw;
    lidar_at = i;
    break;
  }
  if (lidar_at != n) {
    for (std::size_t i = lidar_at + 12; i + 6 <= n; ++i) {
      if (!plausible_unicast(d + i)) continue;
      const std::string cand = ipv4_from_bytes(d + i);
      if (cand == out->lidar_ip || cand == out->gateway) continue;
      if (!SameSubnet(cand, out->lidar_ip, out->netmask)) continue;
      out->persisted_host_ip = cand;
      out->persisted_point_port = rd_u16(d + i + 4);
      break;
    }
  }
  out->heuristic = true;
  // Worth returning only if it recovered something the caller can act on.
  return !out->lidar_ip.empty() || !out->sn.empty();
}

}  // namespace

// --- CRCs (public: a diagnostic wants to know WHICH half failed) -----------

bool Mid360HeaderCrcOk(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len < kHeaderBytes) return false;
  return crc16_ccitt_false(data, kCrc16Span) == rd_u16(data + 18);
}

bool Mid360PayloadCrcOk(const std::uint8_t* data, std::size_t len) {
  if (data == nullptr || len < kHeaderBytes) return false;
  std::size_t declared = rd_u16(data + 2);
  if (declared < kHeaderBytes || declared > len) declared = len;
  return crc32_iso(data + kHeaderBytes, declared - kHeaderBytes) == rd_u32(data + 20);
}

// --- the parser ------------------------------------------------------------

Result<Mid360Beacon> ParseMid360Beacon(const std::uint8_t* data, std::size_t len,
                                       bool allow_heuristic) {
  if (data == nullptr) {
    return set_last_error(ScanError::kInvalidArgument, "beacon: null buffer");
  }
  if (len < kMid360BeaconMinBytes) {
    return set_last_error(ScanError::kProtocolError, "beacon: %zu bytes is too short", len);
  }
  if (data[0] != kSof) {
    return set_last_error(ScanError::kProtocolError, "beacon: sof 0x%02X, expected 0xAA",
                          data[0]);
  }

  Mid360Beacon b;
  b.crc_ok = Mid360HeaderCrcOk(data, len) && Mid360PayloadCrcOk(data, len);

  // The declared length is advisory: a frame with a wrong length field but
  // sane contents is still worth showing an operator. Clamp, never trust.
  std::size_t frame_len = rd_u16(data + 2);
  if (frame_len < kPayloadKvStart || frame_len > len) frame_len = len;

  const std::uint16_t key_num = rd_u16(data + kHeaderBytes);
  b.key_count = key_num;

  // --- the key-value walk --------------------------------------------------
  bool walk_ok = key_num > 0 && key_num <= 512;
  std::size_t off = kPayloadKvStart;
  for (std::uint16_t k = 0; walk_ok && k < key_num; ++k) {
    if (off + 4 > frame_len) {
      walk_ok = false;
      break;
    }
    const std::uint16_t key = rd_u16(data + off);
    const std::uint16_t vlen = rd_u16(data + off + 2);
    const std::uint8_t* v = data + off + 4;
    if (off + 4 + vlen > frame_len) {
      walk_ok = false;
      break;
    }
    switch (key) {
      case kKeyLidarIpCfg:
        if (vlen >= 12) {
          b.lidar_ip = ipv4_from_bytes(v);
          b.netmask = ipv4_from_bytes(v + 4);
          b.gateway = ipv4_from_bytes(v + 8);
        }
        break;
      case kKeyStateInfoHostCfg:
        // 255.255.255.255:56201 in the field capture — i.e. the lidar telling
        // us where it is broadcasting THIS frame. Recorded as the port to
        // listen on next time, which is how a firmware that moves the beacon
        // stays discoverable after one successful discovery.
        if (vlen >= 6 && b.push_port_seen == 0) b.push_port_seen = rd_u16(v + 4);
        break;
      case kKeyPointHostCfg:
        if (vlen >= 6) {
          b.persisted_host_ip = ipv4_from_bytes(v);
          b.persisted_point_port = rd_u16(v + 4);
        }
        break;
      case kKeyImuHostCfg:
        if (vlen >= 6) {
          b.persisted_imu_host_ip = ipv4_from_bytes(v);
          b.persisted_imu_port = rd_u16(v + 4);
        }
        break;
      case kKeySn:
        b.sn = printable_field(v, vlen);
        break;
      case kKeyProductInfo:
        b.product_info = printable_field(v, vlen);
        split_product_info(b.product_info, &b);
        break;
      case kKeyVersionApp:
        if (vlen >= 4) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
          b.fw_version = buf;
        }
        break;
      case kKeyMac:
        if (vlen >= 6) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", v[0], v[1], v[2],
                        v[3], v[4], v[5]);
          b.mac = buf;
        }
        break;
      default:
        break;  // skipped by length — the whole point of a KV frame
    }
    off += 4u + vlen;
  }

  // A walk that completed but found nothing identifying is as useless as one
  // that fell over; both go to the fallback.
  const bool useful = !b.lidar_ip.empty() || !b.sn.empty();
  if (walk_ok && useful) {
    if (b.fw_version.empty()) b.fw_version = b.fw_version_text;
    return b;
  }

  if (!allow_heuristic) {
    return set_last_error(ScanError::kCorruptData,
                          "beacon: key-value walk failed at offset %zu (%u keys declared)",
                          off, static_cast<unsigned>(key_num));
  }

  Mid360Beacon h;
  h.key_count = key_num;
  h.crc_ok = b.crc_ok;
  if (!heuristic_parse(data, frame_len, &h)) {
    return set_last_error(ScanError::kCorruptData,
                          "beacon: neither the key-value walk nor the anchor scan "
                          "found a lidar in %zu bytes", len);
  }
  if (h.fw_version.empty()) h.fw_version = h.fw_version_text;
  SCAN_LOG_WARN(kMod, "mid-360 heartbeat parsed heuristically (sn '%s', ip '%s')",
                h.sn.c_str(), h.lidar_ip.c_str());
  return h;
}

std::string Mid360Beacon::describe() const {
  std::string s = "Mid-360";
  if (!sn.empty()) s += " " + sn;
  if (!lidar_ip.empty()) s += " at " + lidar_ip;
  if (!fw_version_text.empty()) s += " fw " + fw_version_text;
  else if (!fw_version.empty()) s += " fw " + fw_version;
  if (!persisted_host_ip.empty()) s += ", expects host " + persisted_host_ip;
  if (heuristic) s += " (heuristic)";
  else if (!crc_ok) s += " (crc unverified)";
  return s;
}

}  // namespace discovery
}  // namespace scanengine
