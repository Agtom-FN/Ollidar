// lvx2_replay -- streams a real Livox .lvx2 recording (see DATASETS.md) as
// real-time-paced UDP packets in the SAME wire format mid360_sim emits, so
// the real SDK2 client (and later the engine's A3 driver) can be developed
// against the REAL Risley-prism scan pattern and REAL point statistics
// instead of mid360_sim's synthetic sweep scan.
//
// The control plane (discovery / handshake / heartbeat) is copied from
// mid360_sim.cpp nearly verbatim -- it is synthetic either way (a .lvx2 file
// carries no control-channel traffic at all), so there is nothing to replay
// there. Only the data plane differs: PointThread there draws from a
// procedural scene; here it draws from Lvx2Reader.
//
// What is REAL (replayed byte-for-byte from the recording) vs SYNTHESIZED
// (recomputed because the .lvx2 container does not store it) -- see also
// FOLLOWUP_NOTES.md:
//   REAL        : point x/y/z/reflectivity/tag payload, udp_cnt, frame_cnt,
//                 data_type, per-package device timestamp deltas (i.e. the
//                 actual inter-packet pacing / jitter as captured)
//   SYNTHESIZED : the 36-byte on-wire DataHeader wrapper itself (length,
//                 time_interval, crc32) -- .lvx2 does not store these,
//                 CRC32 is recomputed over the real payload so the SDK
//                 accepts the packet; time_interval assumes constant
//                 200,000 pts/s spacing (matches the measured rate, see
//                 DATASETS.md); device timestamps are REBASED to start at 0
//                 (real relative deltas, relabeled origin)
//   NOT REPLAYED: heartbeat/push-state (0x0102) content is synthetic
//                 (mid360_sim's placeholder KV set) -- .lvx2 has no control
//                 channel; IMU is emitted only if the source file actually
//                 contains IMU packages (Livox's own Indoor/Outdoor samples
//                 do not -- see DATASETS.md -- so replaying them produces
//                 ZERO IMU packets, which is the correct, honest behaviour,
//                 not a bug)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>

#include "livox_wire.h"
#include "lvx2_reader.h"

using namespace s2sim;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_run{true};
void OnSignal(int) { g_run.store(false); }

struct Options {
  std::string input_path;
  std::string lidar_ip = "127.0.0.1";
  std::string host_ip = "127.0.0.1";
  std::string sn = "3GGDJ6K00100001";
  double speed = 1.0;      // playback speed multiplier (2.0 = twice as fast)
  bool loop = false;
  int duration_s = 0;      // 0 = play until EOF (once) or forever (with --loop)
  int stats_period_s = 5;
};

struct LidarState {
  std::atomic<bool> streaming{false};
  std::atomic<bool> imu_enabled{true};
  std::atomic<uint8_t> pcl_data_type{1};

  std::mutex mtx;
  sockaddr_in state_dst{};
  sockaddr_in point_dst{};
  sockaddr_in imu_dst{};
  bool have_state_dst = false;
  bool have_point_dst = false;
  bool have_imu_dst = false;

  std::atomic<uint64_t> point_packets_sent{0};
  std::atomic<uint64_t> point_pts_sent{0};
  std::atomic<uint64_t> imu_packets_sent{0};
  std::atomic<uint64_t> loops{0};
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
    std::fprintf(stderr, "[replay] bind %s:%u failed: %s\n", reuse_any ? "0.0.0.0" : ip.c_str(),
                 port, std::strerror(errno));
    ::close(s);
    return -1;
  }
  return s;
}

std::string IpStr(const sockaddr_in& a) {
  char b[INET_ADDRSTRLEN] = {0};
  ::inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b));
  return b;
}

// ---------------------------------------------------------------------------
// Control plane -- copied from mid360_sim.cpp. See that file for the
// reasoning behind each response; it is unchanged here.
// ---------------------------------------------------------------------------
std::vector<uint8_t> BuildInternalInfoAck(const std::vector<uint16_t>& keys, const Options& opt,
                                          const LidarState& st) {
  KvWriter kv;
  for (uint16_t k : keys) {
    switch (k) {
      case kKeyFwType: kv.AddScalar<uint8_t>(k, 1); break;
      case kKeyPclDataType: kv.AddScalar<uint8_t>(k, st.pcl_data_type.load()); break;
      case kKeyPatternMode: kv.AddScalar<uint8_t>(k, 0); break;
      case kKeyWorkMode: kv.AddScalar<uint8_t>(k, st.streaming.load() ? 0x01 : 0x02); break;
      case kKeyImuDataEn: kv.AddScalar<uint8_t>(k, st.imu_enabled.load() ? 1 : 0); break;
      case kKeySn: kv.AddString(k, opt.sn, 16); break;
      case kKeyProductInfo: kv.AddString(k, "Mid360-REPLAY (S2 follow-up)", 64); break;
      case kKeyVersionApp:
      case kKeyVersionLoader:
      case kKeyVersionHardware: {
        uint8_t v[4] = {13, 12, 0, 1};
        kv.Add(k, v, 4);
        break;
      }
      case kKeyMac: {
        uint8_t mac[6] = {0x02, 0x00, 0x5E, 0x10, 0x00, 0x02};
        kv.Add(k, mac, 6);
        break;
      }
      case kKeyCurWorkState: kv.AddScalar<uint8_t>(k, st.streaming.load() ? 0x01 : 0x02); break;
      case kKeyCoreTemp: kv.AddScalar<int32_t>(k, 4210); break;
      case kKeyPowerUpCnt: kv.AddScalar<uint32_t>(k, 1); break;
      case kKeyLocalTimeNow:
      case kKeyLastSyncTime: kv.AddScalar<uint64_t>(k, 0); break;
      case kKeyTimeOffset: kv.AddScalar<int64_t>(k, 0); break;
      case kKeyTimeSyncType: kv.AddScalar<uint8_t>(k, 0); break;
      case kKeyLidarDiagStatus: kv.AddScalar<uint16_t>(k, 0); break;
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
        const sockaddr_in* d = (k == kKeyStateInfoHostIpCfg)         ? &st.state_dst
                               : (k == kKeyLidarPointDataHostIpCfg)  ? &st.point_dst
                                                                     : &st.imu_dst;
        std::memcpy(v.host_ip, &d->sin_addr, 4);
        v.host_port = ntohs(d->sin_port);
        v.lidar_port = (k == kKeyStateInfoHostIpCfg)         ? kLidarPushPort
                       : (k == kKeyLidarPointDataHostIpCfg)  ? kLidarPointPort
                                                              : kLidarImuPort;
        kv.Add(k, &v, sizeof(v));
        break;
      }
      default: kv.AddScalar<uint8_t>(k, 0); break;
    }
  }
  const std::vector<uint8_t>& body = kv.Finish();
  std::vector<uint8_t> out(3 + (body.size() - 4));
  out[0] = 0;
  uint16_t n = kv.count();
  std::memcpy(out.data() + 1, &n, 2);
  std::memcpy(out.data() + 3, body.data() + 4, body.size() - 4);
  return out;
}

void ApplyConfigKvs(const std::vector<KvItem>& kvs, LidarState& st) {
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
        if (it.key == kKeyStateInfoHostIpCfg) { st.state_dst = dst; st.have_state_dst = true; }
        else if (it.key == kKeyLidarPointDataHostIpCfg) { st.point_dst = dst; st.have_point_dst = true; }
        else { st.imu_dst = dst; st.have_imu_dst = true; }
        std::printf("[replay] host cfg key 0x%04X -> %s:%u\n", it.key, IpStr(dst).c_str(),
                    v.host_port);
        break;
      }
      case kKeyWorkMode: {
        uint8_t m = it.len ? it.value[0] : 0;
        st.streaming.store(m == 0x01);
        std::printf("[replay] work mode 0x%02X -> %s\n", m, m == 0x01 ? "SAMPLING" : "idle");
        break;
      }
      case kKeyPclDataType: if (it.len) st.pcl_data_type.store(it.value[0]); break;
      case kKeyImuDataEn: if (it.len) st.imu_enabled.store(it.value[0] != 0); break;
      default: break;
    }
  }
}

void CommandThread(int sock, Options opt, LidarState* st) {
  std::vector<uint8_t> buf(2048);
  while (g_run.load()) {
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    timeval tv{0, 200000};
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = ::recvfrom(sock, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &flen);
    if (n <= 0) continue;

    CmdHeader h{};
    const uint8_t* body = nullptr;
    uint16_t body_len = 0;
    if (!ParseCmdFrame(buf.data(), static_cast<size_t>(n), h, body, body_len)) continue;
    if (h.cmd_type != kCmdTypeReq) continue;

    std::vector<uint8_t> ack_payload;
    switch (h.cmd_id) {
      case kCmdIdGetInternalInfo: {
        std::vector<uint16_t> keys;
        if (!ParseKeyQuery(body, body_len, keys)) break;
        ack_payload = BuildInternalInfoAck(keys, opt, *st);
        break;
      }
      case kCmdIdWorkModeControl: {
        std::vector<KvItem> kvs;
        if (!ParseKvList(body, body_len, kvs)) break;
        ApplyConfigKvs(kvs, *st);
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
        ack_payload.assign(reinterpret_cast<uint8_t*>(&d), reinterpret_cast<uint8_t*>(&d) + sizeof(d));
        break;
      }
      default: ack_payload.assign(3, 0); break;
    }
    if (ack_payload.empty()) continue;
    std::vector<uint8_t> frame;
    BuildCmdFrame(frame, h.seq_num, h.cmd_id, kCmdTypeAck, kSenderLidar, ack_payload.data(),
                  static_cast<uint16_t>(ack_payload.size()));
    ::sendto(sock, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&from), flen);
  }
}

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

void PushMsgThread(int sock, Options opt, LidarState* st) {
  uint32_t seq = 1;
  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    sockaddr_in dst{};
    {
      std::lock_guard<std::mutex> lk(st->mtx);
      if (!st->have_state_dst) continue;
      dst = st->state_dst;
    }
    KvWriter kv;
    kv.AddString(kKeySn, opt.sn, 16);
    kv.AddString(kKeyProductInfo, "Mid360-REPLAY (S2 follow-up)", 64);
    kv.AddScalar<uint8_t>(kKeyPclDataType, st->pcl_data_type.load());
    kv.AddScalar<uint8_t>(kKeyWorkMode, st->streaming.load() ? 0x01 : 0x02);
    kv.AddScalar<uint8_t>(kKeyImuDataEn, st->imu_enabled.load() ? 1 : 0);
    kv.AddScalar<uint8_t>(kKeyCurWorkState, st->streaming.load() ? 0x01 : 0x02);
    kv.AddScalar<int32_t>(kKeyCoreTemp, 4000);
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
// Data plane -- replays the .lvx2 file's real packages, real-time-paced by
// their real recorded timestamp deltas (scaled by --speed).
// ---------------------------------------------------------------------------
void ReplayThread(int point_sock, int imu_sock, Options opt, LidarState* st) {
  bool any_imu_in_file = false;
  uint64_t total_points_in_file = 0;
  {
    // One quick pre-scan so we know whether to ever wait on have_imu_dst,
    // and so the startup banner can tell the operator what is actually in
    // the file before any pacing/streaming starts.
    Lvx2Reader probe(opt.input_path);
    Lvx2Package pkg;
    uint64_t n_pkg = 0;
    while (probe.Next(pkg)) {
      ++n_pkg;
      if (pkg.data_type == 0) any_imu_in_file = true;
      if (pkg.data_type == 1) total_points_in_file += pkg.payload_len / sizeof(CartesianHigh);
    }
    std::printf("[replay] source: %" PRIu64 " packages, %" PRIu64 " points, IMU packages: %s\n",
                n_pkg, total_points_in_file, any_imu_in_file ? "present" : "NONE (nothing to replay on the IMU port)");
  }

  const double interval_01us_const =
      (kPointsPerPacket - 1) * 1e7 / 200000.0;  // matches the recording's measured ~200k pts/s

  int loop_iter = 0;
  auto t_program_start = Clock::now();
  while (g_run.load()) {
    Lvx2Reader reader(opt.input_path);
    Lvx2Package pkg;
    bool first = true;
    uint64_t t0_dev_ns = 0;
    Clock::time_point t0_wall{};
    ++loop_iter;
    if (loop_iter > 1) {
      std::printf("[replay] looping back to start of file (iteration %d)\n", loop_iter);
      st->loops.fetch_add(1);
    }

    while (g_run.load() && reader.Next(pkg)) {
      bool need_imu_dst = (pkg.data_type == 0);
      // Wait for the host to have configured the destination this package needs.
      for (;;) {
        if (!g_run.load()) return;
        bool ready;
        {
          std::lock_guard<std::mutex> lk(st->mtx);
          ready = st->streaming.load() && st->have_point_dst && (!need_imu_dst || st->have_imu_dst);
        }
        if (ready) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // Re-anchor pacing to "now" every time we had to wait, so a slow
        // handshake does not cause an artificial catch-up burst.
        first = true;
      }

      if (first) {
        t0_dev_ns = pkg.timestamp_ns;
        t0_wall = Clock::now();
        first = false;
      }
      double target_s = (static_cast<double>(pkg.timestamp_ns) - static_cast<double>(t0_dev_ns)) /
                        1e9 / std::max(0.001, opt.speed);
      double now_s = std::chrono::duration<double>(Clock::now() - t0_wall).count();
      double sleep_s = target_s - now_s;
      if (sleep_s > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(sleep_s * 1e6)));
      }
      if (!g_run.load()) return;

      uint64_t rebased_ts_ns = pkg.timestamp_ns - t0_dev_ns;  // relabel origin, real deltas kept

      if (pkg.data_type == 1) {
        uint32_t dot_num = pkg.payload_len / sizeof(CartesianHigh);
        if (dot_num == 0) continue;
        std::vector<uint8_t> out(sizeof(DataHeader) + pkg.payload_len);
        auto* hdr = reinterpret_cast<DataHeader*>(out.data());
        std::memset(hdr, 0, sizeof(DataHeader));
        hdr->version = 0;
        hdr->length = static_cast<uint16_t>(out.size());
        hdr->time_interval = static_cast<uint16_t>(
            dot_num > 1 ? interval_01us_const * (dot_num - 1) / (kPointsPerPacket - 1) : 0);
        hdr->dot_num = static_cast<uint16_t>(dot_num);
        hdr->udp_cnt = pkg.udp_cnt;       // REAL, from the recording
        hdr->frame_cnt = pkg.frame_cnt;   // REAL, from the recording
        hdr->data_type = pkg.data_type;   // REAL (1)
        hdr->time_type = pkg.timestamp_type;
        hdr->timestamp = rebased_ts_ns;   // real deltas, rebased origin
        std::memcpy(out.data() + sizeof(DataHeader), pkg.payload, pkg.payload_len);  // REAL points
        hdr->crc32 = Crc32(reinterpret_cast<const uint8_t*>(&hdr->timestamp),
                           sizeof(uint64_t) + pkg.payload_len);  // synthesized (not stored in lvx2)

        sockaddr_in dst;
        { std::lock_guard<std::mutex> lk(st->mtx); dst = st->point_dst; }
        ::sendto(point_sock, out.data(), out.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        st->point_packets_sent.fetch_add(1);
        st->point_pts_sent.fetch_add(dot_num);
      } else if (pkg.data_type == 0 && st->imu_enabled.load()) {
        if (pkg.payload_len < sizeof(ImuSample)) continue;
        std::vector<uint8_t> out(sizeof(DataHeader) + sizeof(ImuSample));
        auto* hdr = reinterpret_cast<DataHeader*>(out.data());
        std::memset(hdr, 0, sizeof(DataHeader));
        hdr->version = 0;
        hdr->length = static_cast<uint16_t>(out.size());
        hdr->time_interval = 0;
        hdr->dot_num = 1;
        hdr->udp_cnt = pkg.udp_cnt;
        hdr->frame_cnt = pkg.frame_cnt;
        hdr->data_type = pkg.data_type;  // 0
        hdr->time_type = pkg.timestamp_type;
        hdr->timestamp = rebased_ts_ns;
        std::memcpy(out.data() + sizeof(DataHeader), pkg.payload, sizeof(ImuSample));  // REAL IMU
        hdr->crc32 = Crc32(reinterpret_cast<const uint8_t*>(&hdr->timestamp),
                           sizeof(uint64_t) + sizeof(ImuSample));

        sockaddr_in dst;
        { std::lock_guard<std::mutex> lk(st->mtx); dst = st->imu_dst; }
        ::sendto(imu_sock, out.data(), out.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        st->imu_packets_sent.fetch_add(1);
      }

      if (opt.duration_s > 0 &&
          std::chrono::duration<double>(Clock::now() - t_program_start).count() >= opt.duration_s) {
        g_run.store(false);
        return;
      }
    }

    if (!opt.loop) {
      std::printf("[replay] end of file reached (no --loop); replay thread exiting\n");
      return;
    }
  }
}

void Usage(const char* argv0) {
  std::printf(
      "usage: %s <file.lvx2> [options]\n"
      "  --speed N            playback speed multiplier (default 1.0 = real time)\n"
      "  --loop               loop back to the start of the file at EOF\n"
      "  --lidar-ip IP        address the simulated lidar binds/reports (default 127.0.0.1)\n"
      "  --host-ip IP         host address for the 1 Hz discovery announce (default 127.0.0.1)\n"
      "  --host-cmd-port N    host control port to announce into (default 56101)\n"
      "  --sn STRING          reported serial number\n"
      "  --duration S         exit after S seconds total (default: play file once / forever with --loop)\n"
      "  --stats-period S     stats line period (default 5)\n",
      argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { Usage(argv[0]); return 2; }
  Options opt;
  opt.input_path = argv[1];
  uint16_t host_cmd_port = 56101;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
    if (a == "--speed") opt.speed = std::stod(next());
    else if (a == "--loop") opt.loop = true;
    else if (a == "--lidar-ip") opt.lidar_ip = next();
    else if (a == "--host-ip") opt.host_ip = next();
    else if (a == "--host-cmd-port") host_cmd_port = static_cast<uint16_t>(std::stoi(next()));
    else if (a == "--sn") opt.sn = next();
    else if (a == "--duration") opt.duration_s = std::stoi(next());
    else if (a == "--stats-period") opt.stats_period_s = std::stoi(next());
    else { Usage(argv[0]); return a == "--help" ? 0 : 2; }
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  LidarState st;
  st.state_dst.sin_family = st.point_dst.sin_family = st.imu_dst.sin_family = AF_INET;

  int cmd_sock = MakeUdpSocket(opt.lidar_ip, kLidarCmdPort);
  int push_sock = MakeUdpSocket(opt.lidar_ip, kLidarPushPort);
  int point_sock = MakeUdpSocket(opt.lidar_ip, kLidarPointPort);
  int imu_sock = MakeUdpSocket(opt.lidar_ip, kLidarImuPort);
  int det_sock = MakeUdpSocket(opt.lidar_ip, kDetectionPort, /*reuse_any=*/true);
  if (cmd_sock < 0 || push_sock < 0 || point_sock < 0 || imu_sock < 0 || det_sock < 0) {
    std::fprintf(stderr, "[replay] socket setup failed\n");
    return 1;
  }

  std::printf(
      "[replay] Livox .lvx2 replay -- %s\n"
      "[replay]   lidar %s  cmd/%u push/%u point/%u imu/%u  discovery->%s:%u\n"
      "[replay]   sn=%s  speed=%.2fx  loop=%s\n",
      opt.input_path.c_str(), opt.lidar_ip.c_str(), kLidarCmdPort, kLidarPushPort, kLidarPointPort,
      kLidarImuPort, opt.host_ip.c_str(), host_cmd_port, opt.sn.c_str(), opt.speed,
      opt.loop ? "yes" : "no");

  std::thread t_cmd(CommandThread, cmd_sock, opt, &st);
  std::thread t_det(DetectionThread, det_sock, opt, host_cmd_port);
  std::thread t_push(PushMsgThread, push_sock, opt, &st);
  std::thread t_replay(ReplayThread, point_sock, imu_sock, opt, &st);

  auto start = Clock::now();
  auto last = start;
  uint64_t last_pts = 0, last_ip = 0;
  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto now = Clock::now();
    double since = std::chrono::duration<double>(now - last).count();
    if (since >= opt.stats_period_s) {
      uint64_t pts = st.point_pts_sent.load(), ip = st.imu_packets_sent.load();
      std::printf("[replay] t=%6.0fs  points %.0f pts/s  imu %.1f Hz  point pkts=%" PRIu64
                  "  imu pkts=%" PRIu64 "  loops=%" PRIu64 "\n",
                  std::chrono::duration<double>(now - start).count(), (pts - last_pts) / since,
                  (ip - last_ip) / since, st.point_packets_sent.load(), st.imu_packets_sent.load(),
                  st.loops.load());
      std::fflush(stdout);
      last = now;
      last_pts = pts;
      last_ip = ip;
    }
    if (opt.duration_s > 0 && std::chrono::duration<double>(now - start).count() >= opt.duration_s) {
      g_run.store(false);
    }
    if (!t_replay.joinable()) break;
  }

  t_cmd.detach();
  t_det.detach();
  t_push.detach();
  if (t_replay.joinable()) t_replay.join();

  std::printf("[replay] shutdown: point pkts=%" PRIu64 " points=%" PRIu64 " imu pkts=%" PRIu64
              " loops=%" PRIu64 "\n",
              st.point_packets_sent.load(), st.point_pts_sent.load(), st.imu_packets_sent.load(),
              st.loops.load());
  ::close(cmd_sock);
  ::close(push_sock);
  ::close(point_sock);
  ::close(imu_sock);
  ::close(det_sock);
  return 0;
}
