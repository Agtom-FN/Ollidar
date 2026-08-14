// lvx2_reader.h -- minimal reader for Livox .lvx2 container files.
//
// Format cross-checked against DATASETS.md (written for this spike from a
// byte-by-byte probe of Livox's own Indoor_sampledata.lvx2 /
// Outdoor_sampledata.lvx2) and re-verified here:
//
//   0x00  public header   : signature[16]="livox_tech", version[4]={2,0,0,0},
//                           magic u32=0xAC0EA767
//   0x18  private header  : frame_duration u32 (ms), device_count u8
//   0x1D  device info     : 63 bytes per device (lidar_sn[16] first)
//         frame blocks    : frame header {current_offset u64, next_offset u64,
//                           frame_index u64} then N packages, each
//                           {version u8, lidar_id u32, lidar_type u8,
//                            timestamp_type u8, timestamp u64, udp_cnt u16,
//                            data_type u8, length u32, frame_cnt u8,
//                            reserve[4]} = 27 B header + `length` bytes of
//                           payload.
//
// `current_offset` equals the frame's own byte offset (used to locate the
// first frame block); `next_offset == 0` marks the last frame block in the
// file, whose packages run until EOF.
//
// The payload following each package header is the point/IMU data with NO
// on-wire DataHeader (36 B) in front of it -- for data_type==1 it is exactly
// `dot_num * 14` bytes of raw CartesianHigh points (see livox_wire.h), i.e.
// the lvx2 container stores the decoded per-package metadata in its own
// 27-byte header and keeps only the point/IMU payload verbatim. This is what
// lvx2_replay.cpp re-wraps into a live-format 36-byte DataHeader + payload
// UDP datagram.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace s2sim {

constexpr uint32_t kLvx2Magic = 0xAC0EA767;
constexpr size_t kLvx2DeviceInfoLen = 63;

#pragma pack(push, 1)
struct Lvx2PublicHeader {
  char signature[16];
  uint8_t version[4];
  uint32_t magic;
};
static_assert(sizeof(Lvx2PublicHeader) == 24, "lvx2 public header must be 24 bytes");

struct Lvx2PrivateHeader {
  uint32_t frame_duration_ms;
  uint8_t device_count;
};
static_assert(sizeof(Lvx2PrivateHeader) == 5, "lvx2 private header must be 5 bytes");

struct Lvx2FrameHeader {
  uint64_t current_offset;
  uint64_t next_offset;  // 0 => this is the last frame block in the file
  uint64_t frame_index;
};
static_assert(sizeof(Lvx2FrameHeader) == 24, "lvx2 frame header must be 24 bytes");

struct Lvx2PackageHeader {
  uint8_t version;
  uint32_t lidar_id;
  uint8_t lidar_type;
  uint8_t timestamp_type;
  uint64_t timestamp;  // ns, device clock (same clock domain as the live time_type=0 stream)
  uint16_t udp_cnt;
  uint8_t data_type;  // 0 = IMU, 1 = cartesian 32-bit (same encoding as the live wire)
  uint32_t length;    // payload bytes that follow
  uint8_t frame_cnt;
  uint8_t reserve[4];
};
static_assert(sizeof(Lvx2PackageHeader) == 27, "lvx2 package header must be 27 bytes");
#pragma pack(pop)

// One decoded package. `payload` points into the reader's internal buffer
// and is only valid until the next call to Next().
struct Lvx2Package {
  uint32_t lidar_id = 0;
  uint8_t data_type = 0;
  uint8_t timestamp_type = 0;
  uint16_t udp_cnt = 0;
  uint8_t frame_cnt = 0;
  uint64_t timestamp_ns = 0;
  const uint8_t* payload = nullptr;
  uint32_t payload_len = 0;
};

class Lvx2Reader {
 public:
  explicit Lvx2Reader(const std::string& path) : path_(path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("lvx2: cannot open " + path);

    Lvx2PublicHeader pub{};
    if (std::fread(&pub, sizeof(pub), 1, f_) != 1) Fail("truncated public header");
    if (pub.magic != kLvx2Magic) {
      Fail("bad magic 0x" + ToHex(pub.magic) + " (expected 0x" + ToHex(kLvx2Magic) + ")");
    }
    signature_.assign(pub.signature, strnlen(pub.signature, sizeof(pub.signature)));
    std::memcpy(version_, pub.version, 4);

    Lvx2PrivateHeader priv{};
    if (std::fread(&priv, sizeof(priv), 1, f_) != 1) Fail("truncated private header");
    frame_duration_ms_ = priv.frame_duration_ms;
    device_count_ = priv.device_count;

    for (int i = 0; i < device_count_; ++i) {
      char dev[kLvx2DeviceInfoLen];
      if (std::fread(dev, 1, sizeof(dev), f_) != sizeof(dev)) Fail("truncated device info");
      devices_.emplace_back(dev, strnlen(dev, 16));
    }
    frame_block_start_ = std::ftell(f_);
    Rewind();
  }

  ~Lvx2Reader() {
    if (f_) std::fclose(f_);
  }

  Lvx2Reader(const Lvx2Reader&) = delete;
  Lvx2Reader& operator=(const Lvx2Reader&) = delete;

  void Rewind() {
    frame_off_ = frame_block_start_;
    have_frame_ = false;
    last_frame_ = false;
    pos_ = 0;
    next_frame_off_ = 0;
  }

  // Advances to and decodes the next package. Returns false at end of file.
  // `out.payload` is valid only until the next Next()/Rewind() call.
  bool Next(Lvx2Package& out) {
    for (;;) {
      if (!have_frame_) {
        std::fseek(f_, frame_off_, SEEK_SET);
        Lvx2FrameHeader fh{};
        if (std::fread(&fh, sizeof(fh), 1, f_) != 1) return false;  // clean EOF
        pos_ = frame_off_ + static_cast<long>(sizeof(fh));
        next_frame_off_ = fh.next_offset;
        last_frame_ = (fh.next_offset == 0);
        have_frame_ = true;
      }

      bool within_frame = last_frame_ || (static_cast<uint64_t>(pos_) < next_frame_off_);
      if (!within_frame) {
        frame_off_ = static_cast<long>(next_frame_off_);
        have_frame_ = false;
        continue;
      }

      std::fseek(f_, pos_, SEEK_SET);
      Lvx2PackageHeader ph{};
      size_t got = std::fread(&ph, 1, sizeof(ph), f_);
      if (got != sizeof(ph)) {
        // Short/failed read: end of this frame's packages. On the last frame
        // this is normal EOF; on an earlier frame it would mean a corrupt
        // next_offset, which we treat the same way (stop cleanly).
        if (last_frame_) return false;
        frame_off_ = static_cast<long>(next_frame_off_);
        have_frame_ = false;
        continue;
      }

      buf_.resize(ph.length);
      if (ph.length > 0 && std::fread(buf_.data(), 1, ph.length, f_) != ph.length) {
        return false;  // truncated file
      }
      pos_ += static_cast<long>(sizeof(ph) + ph.length);

      out.lidar_id = ph.lidar_id;
      out.data_type = ph.data_type;
      out.timestamp_type = ph.timestamp_type;
      out.udp_cnt = ph.udp_cnt;
      out.frame_cnt = ph.frame_cnt;
      out.timestamp_ns = ph.timestamp;
      out.payload = buf_.data();
      out.payload_len = ph.length;
      return true;
    }
  }

  const std::string& signature() const { return signature_; }
  uint32_t frame_duration_ms() const { return frame_duration_ms_; }
  const std::vector<std::string>& devices() const { return devices_; }

 private:
  void Fail(const std::string& why) { throw std::runtime_error("lvx2 " + path_ + ": " + why); }
  static std::string ToHex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
  }

  std::string path_;
  std::FILE* f_ = nullptr;
  std::string signature_;
  uint8_t version_[4] = {0, 0, 0, 0};
  uint32_t frame_duration_ms_ = 0;
  uint8_t device_count_ = 0;
  std::vector<std::string> devices_;
  long frame_block_start_ = 0;

  // Iteration state.
  long frame_off_ = 0;
  long pos_ = 0;
  uint64_t next_frame_off_ = 0;
  bool have_frame_ = false;
  bool last_frame_ = false;
  std::vector<uint8_t> buf_;
};

}  // namespace s2sim
