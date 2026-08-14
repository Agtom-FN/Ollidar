// wire_selftest — checks the simulator's wire encoder against the SDK's own
// definitions before any UDP socket is involved. Failures here would otherwise
// only show up as a silent "device never connects".

#include <cstdio>
#include <cstring>
#include <vector>

#include "livox_wire.h"

// The SDK's own control-frame view, included directly from sdk_core.
#include "comm/sdk_protocol.h"

using namespace s2sim;

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      ++g_fail;                                             \
    } else {                                                \
      std::printf("ok  : %s\n", msg);                       \
    }                                                       \
  } while (0)

int main() {
  // ---- struct layout agreement with the SDK's public/internal headers ----
  CHECK(sizeof(CmdHeader) == sizeof(livox::lidar::SdkPreamble),
        "control header size == SDK SdkPreamble");
  CHECK(sizeof(DataHeader) == offsetof(LivoxLidarEthernetPacket, data),
        "data header size == SDK LivoxLidarEthernetPacket prefix");
  CHECK(sizeof(CartesianHigh) == sizeof(LivoxLidarCartesianHighRawPoint),
        "cartesian point size == SDK LivoxLidarCartesianHighRawPoint");
  CHECK(sizeof(ImuSample) == sizeof(LivoxLidarImuRawPoint),
        "IMU sample size == SDK LivoxLidarImuRawPoint");
  CHECK(kPointPacketBytes == 1380, "point packet is 1380 bytes (96 pts, data_type 1)");
  CHECK(kImuPacketBytes == 60, "IMU packet is 60 bytes");

  // Field offsets of the data header must match the official protocol table.
  CHECK(offsetof(DataHeader, length) == 1, "data header: length @1");
  CHECK(offsetof(DataHeader, time_interval) == 3, "data header: time_interval @3");
  CHECK(offsetof(DataHeader, dot_num) == 5, "data header: dot_num @5");
  CHECK(offsetof(DataHeader, udp_cnt) == 7, "data header: udp_cnt @7");
  CHECK(offsetof(DataHeader, frame_cnt) == 9, "data header: frame_cnt @9");
  CHECK(offsetof(DataHeader, data_type) == 10, "data header: data_type @10");
  CHECK(offsetof(DataHeader, time_type) == 11, "data header: time_type @11");
  CHECK(offsetof(DataHeader, crc32) == 24, "data header: crc32 @24");
  CHECK(offsetof(DataHeader, timestamp) == 28, "data header: timestamp @28");

  // ---- the SDK must accept a frame we build ----
  DetectionAck d{};
  d.ret_code = 0;
  d.dev_type = kDevTypeMid360;
  std::strncpy(d.sn, "3GGDJ6K00100001", sizeof(d.sn) - 1);
  d.lidar_ip[0] = 127; d.lidar_ip[1] = 0; d.lidar_ip[2] = 0; d.lidar_ip[3] = 1;
  d.cmd_port = kLidarCmdPort;

  std::vector<uint8_t> frame;
  BuildCmdFrame(frame, 4242, kCmdIdSearch, kCmdTypeAck, kSenderLidar,
                reinterpret_cast<uint8_t*>(&d), sizeof(d));

  livox::lidar::SdkProtocol proto;
  CHECK(proto.CheckPreamble(frame.data(), frame.size()),
        "SDK SdkProtocol::CheckPreamble accepts our detection ACK (crc16 + crc32)");

  livox::lidar::CommPacket pkt{};
  CHECK(proto.ParsePacket(frame.data(), frame.size(), &pkt), "SDK parses our frame");
  CHECK(pkt.cmd_id == kCmdIdSearch, "parsed cmd_id == 0x0000");
  CHECK(pkt.cmd_type == kCmdTypeAck, "parsed cmd_type == ACK");
  CHECK(pkt.sender_type == kSenderLidar, "parsed sender_type == lidar");
  CHECK(pkt.seq_num == 4242, "parsed seq_num round-trips");
  CHECK(pkt.data_len == sizeof(DetectionAck), "parsed payload length == 24");

  // A corrupted payload must be rejected -- proves the CRC is really checked.
  frame[kCmdHeaderLen + 3] ^= 0xFF;
  CHECK(!proto.CheckPreamble(frame.data(), frame.size()),
        "SDK rejects our frame after a payload bit flip");

  // ---- KV round-trip ----
  KvWriter kv;
  kv.AddScalar<uint8_t>(kKeyFwType, 1);
  HostIpInfoValue hv{};
  hv.host_ip[0] = 127; hv.host_ip[3] = 1;
  hv.host_port = 56301;
  hv.lidar_port = kLidarPointPort;
  kv.Add(kKeyLidarPointDataHostIpCfg, &hv, sizeof(hv));
  const std::vector<uint8_t>& body = kv.Finish();

  std::vector<KvItem> items;
  CHECK(ParseKvList(body.data(), static_cast<uint16_t>(body.size()), items), "KV list parses");
  CHECK(items.size() == 2, "KV list has 2 entries");
  CHECK(items[0].key == kKeyFwType && items[0].len == 1 && items[0].value[0] == 1,
        "KV[0] is fw_type = 1");
  CHECK(items[1].key == kKeyLidarPointDataHostIpCfg && items[1].len == 8, "KV[1] is host point cfg");
  HostIpInfoValue back{};
  std::memcpy(&back, items[1].value, sizeof(back));
  CHECK(back.host_port == 56301 && back.lidar_port == kLidarPointPort,
        "host/lidar ports survive the round trip");

  std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
              g_fail == 1 ? "" : "s");
  return g_fail == 0 ? 0 : 1;
}
