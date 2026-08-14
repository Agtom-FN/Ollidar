// mid360_sim — a protocol-faithful Livox Mid-360 simulator that speaks the real
// Livox SDK2 wire protocol on loopback, so the whole Mid-360 software path can be
// exercised without the physical sensor.
//
// It plays the *lidar* side of the link:
//   * discovery  (cmd 0x0000 ACK, announced at 1 Hz from UDP source port 56000)
//   * control    (cmd 0x0101 get-internal-info, 0x0100 work-mode/host-IP config)
//                 on lidar cmd port 56100
//   * push/state (cmd 0x0102 pushed at 1 Hz from lidar push port 56200) -- heartbeat
//   * point data (data_type 1, 96 pts/packet, 1380 B) from lidar port 56300
//   * IMU data   (data_type 0, 1 sample/packet, 60 B) at 200 Hz from lidar port 56400
//
// Host IP + host ports for the three data/state streams are taken from the
// 0x0100 request the SDK sends, exactly as the real device does.
//
// See REPORT.md for what is faithful vs simplified.

#include <arpa/inet.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "livox_wire.h"
#include "scene.h"

using namespace s2sim;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_run{true};
bool g_verbose = false;
void OnSignal(int) { g_run.store(false); }

struct Options {
  std::string lidar_ip = "127.0.0.1";
  std::string host_ip = "127.0.0.1";  // fallback until the host tells us
  double point_rate = 200000.0;       // points/s
  double imu_rate = 200.0;            // Hz
  double loss_pct = 0.0;              // injected packet loss, %
  double jitter_ms = 0.0;             // injected send jitter, ms (uniform 0..jitter)
  double range_noise_m = 0.02;        // 1-sigma range noise
  std::string sn = "3GGDJ6K00100001";
  int stats_period_s = 30;
  int duration_s = 0;  // 0 = run until signalled
  // Packet-counter model. "real" reproduces what an actual Mid-360 emits
  // (verified against Livox's own Indoor_sampledata.lvx2, see DATASETS.md):
  // udp_cnt is a free-running uint16 that does NOT reset, and frame_cnt is
  // always 0. "doc" reproduces the behaviour the published protocol table
  // describes (udp_cnt resets at frame start, frame_cnt increments).
  bool doc_frame_model = false;
};

// ---------------------------------------------------------------------------
// Runtime state shared between the command thread and the streaming threads.
// ---------------------------------------------------------------------------
struct LidarState {
  std::atomic<bool> streaming{false};   // work_mode == kLivoxLidarNormal
  std::atomic<bool> imu_enabled{true};
  std::atomic<uint8_t> pcl_data_type{1};

  std::mutex mtx;
  // Destinations learned from the host's 0x0100 configuration request.
  sockaddr_in state_dst{};
  sockaddr_in point_dst{};
  sockaddr_in imu_dst{};
  bool have_state_dst = false;
  bool have_point_dst = false;
  bool have_imu_dst = false;

  std::atomic<uint64_t> point_packets_sent{0};
  std::atomic<uint64_t> point_packets_dropped{0};
  std::atomic<uint64_t> imu_packets_sent{0};
  std::atomic<uint64_t> imu_packets_dropped{0};
  std::atomic<uint64_t> cmds_handled{0};
  std::atomic<uint64_t> cmd_datagrams{0};
};

int MakeUdpSocket(const std::string& ip, uint16_t port, bool reuse_any = false) {
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return -1;
  int on = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  int sndbuf = 4 * 1024 * 1024;
  ::setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int rcvbuf = 4 * 1024 * 1024;
  ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = reuse_any ? INADDR_ANY : ::inet_addr(ip.c_str());
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    std::fprintf(stderr, "[sim] bind %s:%u failed: %s\n", reuse_any ? "0.0.0.0" : ip.c_str(), port,
                 std::strerror(errno));
    ::close(s);
    return -1;
  }
  return s;
}

size_t ResidentBytes() {
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS) {
    return 0;
  }
  return info.resident_size;
}

double CpuSeconds() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec +
         ru.ru_stime.tv_usec * 1e-6;
}

std::string IpStr(const sockaddr_in& a) {
  char b[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b));
  return b;
}

// ---------------------------------------------------------------------------
// Command / discovery / push threads
// ---------------------------------------------------------------------------

// Answer 0x0101 "get internal info" with the keys the host asked for.
std::vector<uint8_t> BuildInternalInfoAck(const std::vector<uint16_t>& keys, const Options& opt,
                                          const LidarState& st) {
  KvWriter kv;
  for (uint16_t k : keys) {
    switch (k) {
      case kKeyFwType:
        // 1 = application firmware (0 would mean the device is in loader mode and
        // the SDK would never proceed to configuration).
        kv.AddScalar<uint8_t>(k, 1);
        break;
      case kKeyPclDataType:
        kv.AddScalar<uint8_t>(k, st.pcl_data_type.load());
        break;
      case kKeyPatternMode:
        kv.AddScalar<uint8_t>(k, 0);
        break;
      case kKeyWorkMode:
        kv.AddScalar<uint8_t>(k, st.streaming.load() ? 0x01 : 0x02);
        break;
      case kKeyImuDataEn:
        kv.AddScalar<uint8_t>(k, st.imu_enabled.load() ? 1 : 0);
        break;
      case kKeySn:
        kv.AddString(k, opt.sn, 16);
        break;
      case kKeyProductInfo:
        kv.AddString(k, "Mid360-SIM (S2 spike)", 64);
        break;
      case kKeyVersionApp:
      case kKeyVersionLoader:
      case kKeyVersionHardware: {
        uint8_t v[4] = {13, 12, 0, 1};
        kv.Add(k, v, 4);
        break;
      }
      case kKeyMac: {
        uint8_t mac[6] = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x01};
        kv.Add(k, mac, 6);
        break;
      }
      case kKeyCurWorkState:
        kv.AddScalar<uint8_t>(k, st.streaming.load() ? 0x01 : 0x02);
        break;
      case kKeyCoreTemp:
        kv.AddScalar<int32_t>(k, 4210);  // 0.01 degC
        break;
      case kKeyPowerUpCnt:
        kv.AddScalar<uint32_t>(k, 7);
        break;
      case kKeyLocalTimeNow:
      case kKeyLastSyncTime:
        kv.AddScalar<uint64_t>(k, 0);
        break;
      case kKeyTimeOffset:
        kv.AddScalar<int64_t>(k, 0);
        break;
      case kKeyTimeSyncType:
        kv.AddScalar<uint8_t>(k, 0);
        break;
      case kKeyLidarDiagStatus:
        kv.AddScalar<uint16_t>(k, 0);
        break;
      case kKeyHmsCode: {
        uint32_t hms[8] = {0};
        kv.Add(k, hms, sizeof(hms));
        break;
      }
      case kKeyLidarIpCfg: {
        uint8_t v[12] = {0};
        ::inet_pton(AF_INET, opt.lidar_ip.c_str(), v);
        v[4] = 255; v[5] = 0; v[6] = 0; v[7] = 0;
        kv.Add(k, v, sizeof(v));
        break;
      }
      case kKeyStateInfoHostIpCfg:
      case kKeyLidarPointDataHostIpCfg:
      case kKeyLidarImuHostIpCfg: {
        HostIpInfoValue v{};
        const sockaddr_in* d = (k == kKeyStateInfoHostIpCfg)   ? &st.state_dst
                               : (k == kKeyLidarPointDataHostIpCfg) ? &st.point_dst
                                                                    : &st.imu_dst;
        std::memcpy(v.host_ip, &d->sin_addr, 4);
        v.host_port = ntohs(d->sin_port);
        v.lidar_port = (k == kKeyStateInfoHostIpCfg)         ? kLidarPushPort
                       : (k == kKeyLidarPointDataHostIpCfg)  ? kLidarPointPort
                                                             : kLidarImuPort;
        kv.Add(k, &v, sizeof(v));
        break;
      }
      default:
        // Unknown key: answer with a single zero byte so the host's KV walk stays
        // in step rather than desynchronising.
        kv.AddScalar<uint8_t>(k, 0);
        break;
    }
  }
  const std::vector<uint8_t>& body = kv.Finish();
  // LivoxLidarDiagInternalInfoResponse = { ret_code u8, param_num u16, data[] }
  std::vector<uint8_t> out(3 + (body.size() - 4));
  out[0] = 0;
  uint16_t n = kv.count();
  std::memcpy(out.data() + 1, &n, 2);
  std::memcpy(out.data() + 3, body.data() + 4, body.size() - 4);
  return out;
}

void ApplyConfigKvs(const std::vector<KvItem>& kvs, LidarState& st, const Options& opt) {
  std::lock_guard<std::mutex> lk(st.mtx);
  for (const KvItem& it : kvs) {
    switch (it.key) {
      case kKeyStateInfoHostIpCfg:
      case kKeyLidarPointDataHostIpCfg:
      case kKeyLidarImuHostIpCfg: {
        if (it.len < sizeof(HostIpInfoValue)) break;
        HostIpInfoValue v{};
        std::memcpy(&v, it.value, sizeof(v));
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        std::memcpy(&dst.sin_addr, v.host_ip, 4);
        dst.sin_port = htons(v.host_port);
        if (it.key == kKeyStateInfoHostIpCfg) {
          st.state_dst = dst;
          st.have_state_dst = true;
        } else if (it.key == kKeyLidarPointDataHostIpCfg) {
          st.point_dst = dst;
          st.have_point_dst = true;
        } else {
          st.imu_dst = dst;
          st.have_imu_dst = true;
        }
        std::printf("[sim] host cfg key 0x%04X -> %s:%u (lidar port %u)\n", it.key,
                    IpStr(dst).c_str(), v.host_port, v.lidar_port);
        break;
      }
      case kKeyWorkMode: {
        uint8_t m = it.len ? it.value[0] : 0;
        // kLivoxLidarNormal == 0x01 starts sampling; anything else stops it.
        bool on = (m == 0x01);
        st.streaming.store(on);
        std::printf("[sim] work mode 0x%02X -> %s\n", m, on ? "SAMPLING" : "idle");
        break;
      }
      case kKeyPclDataType:
        if (it.len) {
          st.pcl_data_type.store(it.value[0]);
          std::printf("[sim] pcl data type -> %u\n", it.value[0]);
        }
        break;
      case kKeyImuDataEn:
        if (it.len) {
          st.imu_enabled.store(it.value[0] != 0);
          std::printf("[sim] imu data %s\n", it.value[0] ? "enabled" : "disabled");
        }
        break;
      default:
        break;
    }
  }
  (void)opt;
}

void CommandThread(int sock, Options opt, LidarState* st) {
  std::vector<uint8_t> buf(2048);
  while (g_run.load()) {
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    timeval tv{0, 200000};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = ::recvfrom(sock, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from),
                           &flen);
    if (n <= 0) continue;
    st->cmd_datagrams.fetch_add(1);
    if (g_verbose) {
      std::printf("[sim] rx %zd B from %s:%u\n", n, IpStr(from).c_str(), ntohs(from.sin_port));
      std::fflush(stdout);
    }

    CmdHeader h{};
    const uint8_t* body = nullptr;
    uint16_t body_len = 0;
    if (!ParseCmdFrame(buf.data(), static_cast<size_t>(n), h, body, body_len)) {
      std::printf("[sim] dropped malformed control frame (%zd B)\n", n);
      continue;
    }
    if (h.cmd_type != kCmdTypeReq) continue;  // we only answer requests
    st->cmds_handled.fetch_add(1);

    std::vector<uint8_t> ack_payload;
    switch (h.cmd_id) {
      case kCmdIdGetInternalInfo: {
        std::vector<uint16_t> keys;
        if (!ParseKeyQuery(body, body_len, keys)) break;
        ack_payload = BuildInternalInfoAck(keys, opt, *st);
        std::printf("[sim] 0x0101 get-internal-info: %zu key(s) -> ack %zu B\n", keys.size(),
                    ack_payload.size());
        break;
      }
      case kCmdIdWorkModeControl: {
        std::vector<KvItem> kvs;
        if (!ParseKvList(body, body_len, kvs)) break;
        ApplyConfigKvs(kvs, *st, opt);
        // LivoxLidarAsyncControlResponse = { ret_code u8, error_key u16 }
        ack_payload.assign(3, 0);
        break;
      }
      case kCmdIdSearch: {
        DetectionAck d{};
        d.ret_code = 0;
        d.dev_type = kDevTypeMid360;
        std::strncpy(d.sn, opt.sn.c_str(), sizeof(d.sn) - 1);
        ::inet_pton(AF_INET, opt.lidar_ip.c_str(), d.lidar_ip);
        d.cmd_port = kLidarCmdPort;
        ack_payload.assign(reinterpret_cast<uint8_t*>(&d),
                           reinterpret_cast<uint8_t*>(&d) + sizeof(d));
        break;
      }
      default:
        ack_payload.assign(3, 0);  // generic {ret_code=0, error_key=0}
        break;
    }
    if (ack_payload.empty()) continue;

    std::vector<uint8_t> frame;
    BuildCmdFrame(frame, h.seq_num, h.cmd_id, kCmdTypeAck, kSenderLidar, ack_payload.data(),
                  static_cast<uint16_t>(ack_payload.size()));
    ::sendto(sock, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&from), flen);
  }
}

// Discovery announcements. A real Mid-360 answers a broadcast search on UDP
// 56000; macOS loopback does not carry 255.255.255.255, so we announce
// unprompted at the same 1 Hz cadence the SDK searches at, from source port
// 56000 (the source port is what the SDK keys discovery on).
void DetectionThread(int sock, Options opt, uint16_t host_cmd_port) {
  DetectionAck d{};
  d.ret_code = 0;
  d.dev_type = kDevTypeMid360;
  std::strncpy(d.sn, opt.sn.c_str(), sizeof(d.sn) - 1);
  ::inet_pton(AF_INET, opt.lidar_ip.c_str(), d.lidar_ip);
  d.cmd_port = kLidarCmdPort;

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = ::inet_addr(opt.host_ip.c_str());
  dst.sin_port = htons(host_cmd_port);

  uint32_t seq = 1;
  while (g_run.load()) {
    std::vector<uint8_t> frame;
    BuildCmdFrame(frame, seq++, kCmdIdSearch, kCmdTypeAck, kSenderLidar,
                  reinterpret_cast<uint8_t*>(&d), sizeof(d));
    ::sendto(sock, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}

// 1 Hz state push (cmd 0x0102) from lidar push port -- the device heartbeat.
void PushMsgThread(int sock, Options opt, LidarState* st) {
  uint32_t seq = 1;
  auto t0 = Clock::now();
  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sockaddr_in dst{};
    {
      std::lock_guard<std::mutex> lk(st->mtx);
      if (!st->have_state_dst) continue;
      dst = st->state_dst;
    }
    double up = std::chrono::duration<double>(Clock::now() - t0).count();
    KvWriter kv;
    kv.AddString(kKeySn, opt.sn, 16);
    kv.AddString(kKeyProductInfo, "Mid360-SIM (S2 spike)", 64);
    kv.AddScalar<uint8_t>(kKeyPclDataType, st->pcl_data_type.load());
    kv.AddScalar<uint8_t>(kKeyWorkMode, st->streaming.load() ? 0x01 : 0x02);
    kv.AddScalar<uint8_t>(kKeyImuDataEn, st->imu_enabled.load() ? 1 : 0);
    kv.AddScalar<uint8_t>(kKeyCurWorkState, st->streaming.load() ? 0x01 : 0x02);
    kv.AddScalar<int32_t>(kKeyCoreTemp, static_cast<int32_t>(4000 + 200 * std::sin(up / 30.0)));
    kv.AddScalar<uint16_t>(kKeyLidarDiagStatus, 0);
    kv.AddScalar<uint8_t>(kKeyFwType, 1);
    const std::vector<uint8_t>& body = kv.Finish();

    std::vector<uint8_t> frame;
    BuildCmdFrame(frame, seq++, kCmdIdPushMsg, kCmdTypeReq, kSenderLidar, body.data(),
                  static_cast<uint16_t>(body.size()));
    ::sendto(sock, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
  }
}

// ---------------------------------------------------------------------------
// Data streams
// ---------------------------------------------------------------------------

void PointThread(int sock, Options opt, LidarState* st) {
  Room room;
  Trajectory traj;
  std::mt19937_64 rng(0xC0FFEE);
  std::normal_distribution<double> range_noise(0.0, opt.range_noise_m);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  std::vector<uint8_t> pkt(kPointPacketBytes);
  auto* hdr = reinterpret_cast<DataHeader*>(pkt.data());
  auto* pts = reinterpret_cast<CartesianHigh*>(pkt.data() + sizeof(DataHeader));

  const double rate = opt.point_rate;
  const double pkt_dt = kPointsPerPacket / rate;  // s
  const uint16_t interval_01us =
      static_cast<uint16_t>((kPointsPerPacket - 1) * 1e7 / rate + 0.5);

  uint64_t packets = 0;
  uint64_t frame_start_packet = 0;
  int64_t cur_frame = -1;
  Clock::time_point t0{};
  bool started = false;

  while (g_run.load()) {
    // NB: never sleep while holding st->mtx -- the command thread needs it to
    // apply configuration, and starving it stalls the whole handshake.
    sockaddr_in dst{};
    bool ready = false;
    if (st->streaming.load()) {
      std::lock_guard<std::mutex> lk(st->mtx);
      if (st->have_point_dst) {
        dst = st->point_dst;
        ready = true;
      }
    }
    if (!ready) {
      started = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (!started) {
      t0 = Clock::now();
      packets = 0;
      frame_start_packet = 0;
      cur_frame = -1;
      started = true;
    }

    double now = std::chrono::duration<double>(Clock::now() - t0).count();
    uint64_t due = static_cast<uint64_t>(now / pkt_dt);
    if (packets >= due) {
      double next_at = (packets + 1) * pkt_dt;
      double sleep_s = next_at - now;
      if (sleep_s > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int64_t>(std::max(60.0, sleep_s * 1e6))));
      continue;
    }

    // Injected jitter delays the whole due burst rather than each packet, so it
    // makes arrival bursty (what network jitter looks like at the receiver)
    // without capping throughput -- a per-packet sleep would cap the rate at
    // 1/jitter packets per second.
    if (opt.jitter_ms > 0.0) {
      std::this_thread::sleep_for(
          std::chrono::microseconds(static_cast<int64_t>(unit(rng) * opt.jitter_ms * 1000.0)));
      now = std::chrono::duration<double>(Clock::now() - t0).count();
      due = static_cast<uint64_t>(now / pkt_dt);
    }

    // Send everything that is due (normally 1-3 packets per wake-up).
    while (packets < due && g_run.load()) {
      uint64_t first_pt = packets * kPointsPerPacket;
      double t_first = first_pt / rate;
      int64_t frame = static_cast<int64_t>(t_first * 10.0);  // 10 Hz frames
      if (frame != cur_frame) {
        cur_frame = frame;
        frame_start_packet = packets;
      }

      std::memset(hdr, 0, sizeof(DataHeader));
      hdr->version = 0;
      hdr->length = static_cast<uint16_t>(kPointPacketBytes);
      hdr->time_interval = interval_01us;
      hdr->dot_num = kPointsPerPacket;
      if (opt.doc_frame_model) {
        hdr->udp_cnt = static_cast<uint16_t>(packets - frame_start_packet);
        hdr->frame_cnt = static_cast<uint8_t>(cur_frame & 0xFF);
      } else {
        hdr->udp_cnt = static_cast<uint16_t>(packets & 0xFFFF);
        hdr->frame_cnt = 0;
      }
      hdr->data_type = 1;  // cartesian, 32-bit
      hdr->time_type = 0;  // lidar-local time since power-on
      hdr->timestamp = static_cast<uint64_t>(t_first * 1e9);

      // One pose per packet (480 us of motion is far below any pose change we
      // could resolve; documented as a simplification).
      Pose q = traj.At(t_first);
      for (uint16_t i = 0; i < kPointsPerPacket; ++i) {
        double az, el;
        ScanDirection(first_pt + i, az, el);
        double ce = std::cos(el);
        Vec3 d_s(ce * std::cos(az), ce * std::sin(az), std::sin(el));
        Vec3 d_w = q.R * d_s;
        double range = 0, shade = 0;
        if (!room.Cast(q.p, d_w, range, shade)) {
          pts[i] = CartesianHigh{0, 0, 0, 0, 0};  // no return
          continue;
        }
        double r = range + range_noise(rng);
        pts[i].x = static_cast<int32_t>(d_s.x * r * 1000.0);
        pts[i].y = static_cast<int32_t>(d_s.y * r * 1000.0);
        pts[i].z = static_cast<int32_t>(d_s.z * r * 1000.0);
        double refl = 255.0 * shade * (6.0 / (r + 6.0));
        pts[i].reflectivity = static_cast<uint8_t>(std::min(255.0, std::max(1.0, refl)));
        pts[i].tag = 0;
      }
      hdr->crc32 = Crc32(reinterpret_cast<const uint8_t*>(&hdr->timestamp),
                         sizeof(uint64_t) + kPointsPerPacket * sizeof(CartesianHigh));

      ++packets;
      if (opt.loss_pct > 0.0 && unit(rng) * 100.0 < opt.loss_pct) {
        st->point_packets_dropped.fetch_add(1);
        continue;  // injected loss: counters advance, bytes never leave
      }
      ::sendto(sock, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
      st->point_packets_sent.fetch_add(1);
    }
  }
}

void ImuThread(int sock, Options opt, LidarState* st) {
  Trajectory traj;
  std::mt19937_64 rng(0xBEEF);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::normal_distribution<double> gyro_noise(0.0, 0.0015);  // rad/s
  std::normal_distribution<double> acc_noise(0.0, 0.0008);   // g

  std::vector<uint8_t> pkt(kImuPacketBytes);
  auto* hdr = reinterpret_cast<DataHeader*>(pkt.data());
  auto* imu = reinterpret_cast<ImuSample*>(pkt.data() + sizeof(DataHeader));

  const double dt = 1.0 / opt.imu_rate;
  uint64_t n = 0;
  uint32_t frame = 0;
  Clock::time_point t0{};
  bool started = false;

  while (g_run.load()) {
    sockaddr_in dst{};
    bool ready = false;
    if (st->streaming.load() && st->imu_enabled.load()) {
      std::lock_guard<std::mutex> lk(st->mtx);
      if (st->have_imu_dst) {
        dst = st->imu_dst;
        ready = true;
      }
    }
    if (!ready) {
      started = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (!started) {
      t0 = Clock::now();
      n = 0;
      started = true;
    }
    double now = std::chrono::duration<double>(Clock::now() - t0).count();
    uint64_t due = static_cast<uint64_t>(now / dt);
    if (n >= due) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      continue;
    }
    while (n < due && g_run.load()) {
      double t = n * dt;
      Pose q = traj.At(t);
      Vec3 f = Trajectory::SpecificForceG(q);

      std::memset(hdr, 0, sizeof(DataHeader));
      hdr->version = 0;
      hdr->length = static_cast<uint16_t>(kImuPacketBytes);
      hdr->time_interval = 0;
      hdr->dot_num = 1;
      hdr->udp_cnt = static_cast<uint16_t>(n & 0xFFFF);
      hdr->frame_cnt = opt.doc_frame_model ? static_cast<uint8_t>((frame++) & 0xFF) : 0;
      hdr->data_type = 0;  // IMU
      hdr->time_type = 0;
      hdr->timestamp = static_cast<uint64_t>(t * 1e9);

      imu->gyro_x = static_cast<float>(q.omega_b.x + gyro_noise(rng));
      imu->gyro_y = static_cast<float>(q.omega_b.y + gyro_noise(rng));
      imu->gyro_z = static_cast<float>(q.omega_b.z + gyro_noise(rng));
      imu->acc_x = static_cast<float>(f.x + acc_noise(rng));
      imu->acc_y = static_cast<float>(f.y + acc_noise(rng));
      imu->acc_z = static_cast<float>(f.z + acc_noise(rng));
      hdr->crc32 = Crc32(reinterpret_cast<const uint8_t*>(&hdr->timestamp),
                         sizeof(uint64_t) + sizeof(ImuSample));

      ++n;
      if (opt.loss_pct > 0.0 && unit(rng) * 100.0 < opt.loss_pct) {
        st->imu_packets_dropped.fetch_add(1);
        continue;
      }
      ::sendto(sock, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
      st->imu_packets_sent.fetch_add(1);
    }
  }
}

void Usage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "  --lidar-ip IP        address the simulated lidar binds/reports (default 127.0.0.1)\n"
      "  --host-ip IP         host address for the 1 Hz discovery announce (default 127.0.0.1)\n"
      "  --host-cmd-port N    host control port to announce into (default 56101)\n"
      "  --rate PTS_PER_S     point rate (default 200000)\n"
      "  --imu-rate HZ        IMU rate (default 200)\n"
      "  --loss PCT           injected packet loss in %% (default 0)\n"
      "  --jitter MS          injected uniform send jitter in ms (default 0)\n"
      "  --noise M            1-sigma range noise in metres (default 0.02)\n"
      "  --sn STRING          reported serial number\n"
      "  --duration S         exit after S seconds (default: run until SIGINT)\n"
      "  --frame-model M      'real' (default, free-running udp_cnt, frame_cnt=0,\n"
      "                       as measured on a real Mid-360) or 'doc' (udp_cnt\n"
      "                       resets per frame, frame_cnt increments)\n"
      "  --stats-period S     stats line period (default 30)\n",
      argv0);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  uint16_t host_cmd_port = 56101;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "--lidar-ip") opt.lidar_ip = next();
    else if (a == "--host-ip") opt.host_ip = next();
    else if (a == "--host-cmd-port") host_cmd_port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--rate") opt.point_rate = std::stod(next());
    else if (a == "--imu-rate") opt.imu_rate = std::stod(next());
    else if (a == "--loss") opt.loss_pct = std::stod(next());
    else if (a == "--jitter") opt.jitter_ms = std::stod(next());
    else if (a == "--noise") opt.range_noise_m = std::stod(next());
    else if (a == "--sn") opt.sn = next();
    else if (a == "--duration") opt.duration_s = std::stoi(next());
    else if (a == "--stats-period") opt.stats_period_s = std::stoi(next());
    else if (a == "--verbose") g_verbose = true;
    else if (a == "--frame-model") opt.doc_frame_model = (next() == "doc");
    else { Usage(argv[0]); return a == "--help" ? 0 : 2; }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  LidarState st;
  // Fallback destinations so a bare `mid360_sim` is still inspectable before the
  // host has configured it.
  st.state_dst.sin_family = st.point_dst.sin_family = st.imu_dst.sin_family = AF_INET;

  int cmd_sock = MakeUdpSocket(opt.lidar_ip, kLidarCmdPort);
  int push_sock = MakeUdpSocket(opt.lidar_ip, kLidarPushPort);
  int point_sock = MakeUdpSocket(opt.lidar_ip, kLidarPointPort);
  int imu_sock = MakeUdpSocket(opt.lidar_ip, kLidarImuPort);
  // Discovery: bound to INADDR_ANY so it can coexist with the SDK's own
  // <host_ip>:56000 detection socket (BSD delivers unicast to the more specific
  // bind, so this steals nothing from the SDK).
  int det_sock = MakeUdpSocket(opt.lidar_ip, kDetectionPort, /*reuse_any=*/true);
  if (cmd_sock < 0 || push_sock < 0 || point_sock < 0 || imu_sock < 0 || det_sock < 0) {
    std::fprintf(stderr, "[sim] socket setup failed\n");
    return 1;
  }

  std::printf(
      "[sim] Livox Mid-360 simulator\n"
      "[sim]   lidar %s  cmd/%u push/%u point/%u imu/%u  discovery->%s:%u\n"
      "[sim]   sn=%s  rate=%.0f pts/s  imu=%.0f Hz  loss=%.2f%%  jitter=%.2f ms  frame-model=%s\n",
      opt.lidar_ip.c_str(), kLidarCmdPort, kLidarPushPort, kLidarPointPort, kLidarImuPort,
      opt.host_ip.c_str(), host_cmd_port, opt.sn.c_str(), opt.point_rate, opt.imu_rate,
      opt.loss_pct, opt.jitter_ms, opt.doc_frame_model ? "doc" : "real");

  std::thread t_cmd(CommandThread, cmd_sock, opt, &st);
  std::thread t_det(DetectionThread, det_sock, opt, host_cmd_port);
  std::thread t_push(PushMsgThread, push_sock, opt, &st);
  std::thread t_point(PointThread, point_sock, opt, &st);
  std::thread t_imu(ImuThread, imu_sock, opt, &st);

  auto start = Clock::now();
  auto last = start;
  uint64_t last_pp = 0, last_ip = 0;
  double last_cpu = CpuSeconds();
  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto now = Clock::now();
    double since = std::chrono::duration<double>(now - last).count();
    if (since >= opt.stats_period_s) {
      uint64_t pp = st.point_packets_sent.load(), ip = st.imu_packets_sent.load();
      double cpu = CpuSeconds();
      std::printf("[sim] t=%6.0fs  points %.0f pts/s (%.1f pkt/s)  imu %.1f Hz  "
                  "dropped pt/imu %" PRIu64 "/%" PRIu64 "  rss=%.1f MB  cpu=%.1f%%\n",
                  std::chrono::duration<double>(now - start).count(),
                  (pp - last_pp) * double(kPointsPerPacket) / since, (pp - last_pp) / since,
                  (ip - last_ip) / since, st.point_packets_dropped.load(),
                  st.imu_packets_dropped.load(), ResidentBytes() / 1048576.0,
                  (cpu - last_cpu) / since * 100.0);
      std::fflush(stdout);
      last = now;
      last_pp = pp;
      last_ip = ip;
      last_cpu = cpu;
    }
    if (opt.duration_s > 0 &&
        std::chrono::duration<double>(now - start).count() >= opt.duration_s) {
      g_run.store(false);
    }
  }

  t_cmd.join();
  t_det.join();
  t_push.join();
  t_point.join();
  t_imu.join();

  double total = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("[sim] final rss=%.1f MB, cpu=%.1fs (%.1f%% of one core)\n",
              ResidentBytes() / 1048576.0, CpuSeconds(),
              total > 0 ? CpuSeconds() / total * 100.0 : 0.0);
  std::printf("[sim] shutdown after %.1fs: point pkts sent=%" PRIu64 " dropped=%" PRIu64
              ", imu pkts sent=%" PRIu64 " dropped=%" PRIu64 ", cmd datagrams=%" PRIu64 ", cmds handled=%" PRIu64 "\n",
              total, st.point_packets_sent.load(), st.point_packets_dropped.load(),
              st.imu_packets_sent.load(), st.imu_packets_dropped.load(), st.cmd_datagrams.load(),
              st.cmds_handled.load());
  ::close(cmd_sock);
  ::close(push_sock);
  ::close(point_sock);
  ::close(imu_sock);
  ::close(det_sock);
  return 0;
}
