// sdk_client_demo — drives the *real* Livox SDK2 client exactly the way engine
// task A3 (mid360 driver) will: init from a config JSON, wait for the
// info-change callback, start the device, and consume point + IMU callbacks.
//
// It measures what the S2 exit criteria need:
//   * sustained point rate (pts/s) and packet rate
//   * IMU rate (Hz)
//   * packet loss, inferred from the protocol's own udp_cnt sequence
//   * device timestamp continuity and callback-to-device time skew
//   * RSS growth over the run (memory stability)
//
// Usage: sdk_client_demo <config.json> [--duration S] [--report-period S] [--csv FILE]

#include "livox_lidar_api.h"
#include "livox_lidar_def.h"

#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_run{true};
void OnSignal(int) { g_run.store(false); }

struct Stats {
  std::atomic<uint64_t> point_packets{0};
  std::atomic<uint64_t> points{0};
  std::atomic<uint64_t> zero_points{0};   // no-return samples
  std::atomic<uint64_t> imu_packets{0};
  std::atomic<uint64_t> lost_packets{0};  // inferred from udp_cnt gaps
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> ts_backwards{0};
  std::atomic<uint64_t> bad_dot_num{0};
  std::atomic<uint64_t> bad_length{0};
  std::atomic<uint64_t> dup_packets{0};

  // udp_cnt / frame_cnt sequence tracking (guarded by mtx)
  std::mutex mtx;
  bool have_prev = false;
  uint16_t prev_udp_cnt = 0;
  uint8_t prev_frame_cnt = 0;
  uint64_t prev_ts_ns = 0;
  uint64_t first_ts_ns = 0;
  uint64_t last_ts_ns = 0;

  std::atomic<uint32_t> handle{0};
  std::atomic<bool> connected{false};

  // IMU sanity: mean |acc| should sit at ~1 g
  std::atomic<uint64_t> imu_acc_n{0};
  double imu_acc_sum = 0.0;
  std::mutex imu_mtx;
};

Stats g_st;
FILE* g_csv = nullptr;

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

void PointCloudCallback(uint32_t handle, const uint8_t dev_type, LivoxLidarEthernetPacket* data,
                        void*) {
  (void)handle;
  (void)dev_type;
  if (data == nullptr) return;

  g_st.point_packets.fetch_add(1, std::memory_order_relaxed);

  if (data->dot_num != 96) g_st.bad_dot_num.fetch_add(1, std::memory_order_relaxed);
  const uint16_t expect_len =
      static_cast<uint16_t>(36 + data->dot_num * sizeof(LivoxLidarCartesianHighRawPoint));
  if (data->data_type == kLivoxLidarCartesianCoordinateHighData && data->length != expect_len) {
    g_st.bad_length.fetch_add(1, std::memory_order_relaxed);
  }

  if (data->data_type == kLivoxLidarCartesianCoordinateHighData) {
    auto* p = reinterpret_cast<LivoxLidarCartesianHighRawPoint*>(data->data);
    uint32_t zeros = 0;
    for (uint32_t i = 0; i < data->dot_num; ++i) {
      if (p[i].x == 0 && p[i].y == 0 && p[i].z == 0) ++zeros;
    }
    g_st.zero_points.fetch_add(zeros, std::memory_order_relaxed);
    g_st.points.fetch_add(data->dot_num, std::memory_order_relaxed);
  }

  uint64_t ts = 0;
  std::memcpy(&ts, data->timestamp, sizeof(ts));

  std::lock_guard<std::mutex> lk(g_st.mtx);
  if (!g_st.have_prev) {
    g_st.have_prev = true;
    g_st.first_ts_ns = ts;
  } else {
    // udp_cnt is the protocol's own packet counter. On a real Mid-360 it is
    // free-running and frame_cnt stays 0 (verified against Livox's
    // Indoor_sampledata.lvx2); the published table instead describes a
    // per-frame reset. Handle both: a small positive step is a real gap, a
    // large one is a frame-boundary reset we cannot attribute.
    uint16_t gap = static_cast<uint16_t>(data->udp_cnt - g_st.prev_udp_cnt);
    if (gap == 0) {
      g_st.dup_packets.fetch_add(1, std::memory_order_relaxed);
    } else if (gap > 1 && gap < 1024) {
      g_st.lost_packets.fetch_add(gap - 1, std::memory_order_relaxed);
    }
    if (data->frame_cnt != g_st.prev_frame_cnt) {
      g_st.frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (ts < g_st.prev_ts_ns) g_st.ts_backwards.fetch_add(1, std::memory_order_relaxed);
  }
  g_st.prev_udp_cnt = data->udp_cnt;
  g_st.prev_frame_cnt = data->frame_cnt;
  g_st.prev_ts_ns = ts;
  g_st.last_ts_ns = ts;
}

void ImuDataCallback(uint32_t, const uint8_t, LivoxLidarEthernetPacket* data, void*) {
  if (data == nullptr) return;
  g_st.imu_packets.fetch_add(1, std::memory_order_relaxed);
  if (data->data_type == kLivoxLidarImuData && data->dot_num >= 1) {
    auto* s = reinterpret_cast<LivoxLidarImuRawPoint*>(data->data);
    double mag = std::sqrt(double(s->acc_x) * s->acc_x + double(s->acc_y) * s->acc_y +
                           double(s->acc_z) * s->acc_z);
    std::lock_guard<std::mutex> lk(g_st.imu_mtx);
    g_st.imu_acc_sum += mag;
    g_st.imu_acc_n.fetch_add(1, std::memory_order_relaxed);
  }
}

void WorkModeCallback(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* r,
                      void*) {
  std::printf("[demo] set work mode: status=%d handle=%u ret=%u err_key=%u\n", status, handle,
              r ? r->ret_code : 255, r ? r->error_key : 65535);
}

void PclTypeCallback(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* r,
                     void*) {
  std::printf("[demo] set pcl data type: status=%d handle=%u ret=%u err_key=%u\n", status, handle,
              r ? r->ret_code : 255, r ? r->error_key : 65535);
}

void ImuEnableCallback(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* r,
                       void*) {
  std::printf("[demo] enable IMU: status=%d handle=%u ret=%u err_key=%u\n", status, handle,
              r ? r->ret_code : 255, r ? r->error_key : 65535);
}

void LidarInfoChangeCallback(const uint32_t handle, const LivoxLidarInfo* info, void*) {
  if (info == nullptr) return;
  std::printf("[demo] CONNECTED handle=%u dev_type=%u sn=%s ip=%s\n", handle, info->dev_type,
              info->sn, info->lidar_ip);
  g_st.handle.store(handle);
  g_st.connected.store(true);

  // Exactly what A3 will do on connect.
  SetLivoxLidarPclDataType(handle, kLivoxLidarCartesianCoordinateHighData, PclTypeCallback,
                           nullptr);
  EnableLivoxLidarImuData(handle, ImuEnableCallback, nullptr);
  SetLivoxLidarWorkMode(handle, kLivoxLidarNormal, WorkModeCallback, nullptr);
}

void LidarInfoCallback(const uint32_t handle, const uint8_t dev_type, const char* info, void*) {
  static std::atomic<int> n{0};
  int i = n.fetch_add(1);
  if (i < 2) {  // the push heartbeat carries a JSON blob; log the first couple
    std::printf("[demo] push state handle=%u dev_type=%u info=%.200s\n", handle, dev_type,
                info ? info : "(null)");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <config.json> [--duration S] [--report-period S] [--csv FILE]\n",
                 argv[0]);
    return 2;
  }
  const char* cfg = argv[1];
  int duration_s = 0;
  int report_s = 30;
  const char* csv_path = nullptr;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--duration" && i + 1 < argc) duration_s = std::atoi(argv[++i]);
    else if (a == "--report-period" && i + 1 < argc) report_s = std::atoi(argv[++i]);
    else if (a == "--csv" && i + 1 < argc) csv_path = argv[++i];
  }
  if (csv_path) {
    g_csv = std::fopen(csv_path, "w");
    if (g_csv)
      std::fprintf(g_csv,
                   "t_s,pts_per_s,pkt_per_s,imu_hz,lost_total,rss_bytes,cpu_pct,dev_ts_s\n");
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  if (!LivoxLidarSdkInit(cfg)) {
    std::fprintf(stderr, "[demo] LivoxLidarSdkInit failed (config=%s)\n", cfg);
    LivoxLidarSdkUninit();
    return 1;
  }
  SetLivoxLidarPointCloudCallBack(PointCloudCallback, nullptr);
  SetLivoxLidarImuDataCallback(ImuDataCallback, nullptr);
  SetLivoxLidarInfoChangeCallback(LidarInfoChangeCallback, nullptr);
  SetLivoxLidarInfoCallback(LidarInfoCallback, nullptr);

  if (!LivoxLidarSdkStart()) {
    std::fprintf(stderr, "[demo] LivoxLidarSdkStart failed\n");
    LivoxLidarSdkUninit();
    return 1;
  }
  std::printf("[demo] SDK2 started (config=%s), waiting for device...\n", cfg);

  auto t_start = Clock::now();
  auto t_last = t_start;
  uint64_t last_pts = 0, last_pkts = 0, last_imu = 0;
  double last_cpu = CpuSeconds();
  size_t rss_first = 0, rss_last = 0, rss_max = 0;
  bool measuring = false;
  Clock::time_point t_measure_start{};
  uint64_t base_pts = 0, base_pkts = 0, base_imu = 0, base_lost = 0, base_frames = 0;
  uint64_t base_ts_ns = 0;
  double base_cpu = 0.0;

  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto now = Clock::now();

    if (!measuring && g_st.points.load() > 0) {
      measuring = true;
      t_measure_start = now;
      t_last = now;
      last_pts = g_st.points.load();
      last_pkts = g_st.point_packets.load();
      last_imu = g_st.imu_packets.load();
      last_cpu = CpuSeconds();
      rss_first = ResidentBytes();
      base_pts = last_pts;
      base_pkts = last_pkts;
      base_imu = last_imu;
      base_lost = g_st.lost_packets.load();
      base_frames = g_st.frames.load();
      base_cpu = last_cpu;
      {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        base_ts_ns = g_st.last_ts_ns;
      }
      std::printf("[demo] first points received, measurement window opens\n");
      std::fflush(stdout);
    }

    double since = std::chrono::duration<double>(now - t_last).count();
    if (measuring && since >= report_s) {
      uint64_t pts = g_st.points.load(), pkts = g_st.point_packets.load(),
               imu = g_st.imu_packets.load();
      double cpu = CpuSeconds();
      size_t rss = ResidentBytes();
      rss_last = rss;
      if (rss > rss_max) rss_max = rss;
      double t_rel = std::chrono::duration<double>(now - t_measure_start).count();
      double pps = (pts - last_pts) / since;
      double ppks = (pkts - last_pkts) / since;
      double ihz = (imu - last_imu) / since;
      double cpu_pct = (cpu - last_cpu) / since * 100.0;
      uint64_t dev_ts = 0;
      {
        std::lock_guard<std::mutex> lk(g_st.mtx);
        dev_ts = g_st.last_ts_ns;
      }
      std::printf("[demo] t=%6.0fs  %9.0f pts/s  %7.1f pkt/s  IMU %6.1f Hz  lost=%" PRIu64
                  "  rss=%.1f MB  cpu=%.1f%%  dev_ts=%.1fs\n",
                  t_rel, pps, ppks, ihz, g_st.lost_packets.load(), rss / 1048576.0, cpu_pct,
                  dev_ts / 1e9);
      std::fflush(stdout);
      if (g_csv) {
        std::fprintf(g_csv, "%.1f,%.0f,%.1f,%.1f,%" PRIu64 ",%zu,%.2f,%.3f\n", t_rel, pps, ppks,
                     ihz, g_st.lost_packets.load(), rss, cpu_pct, dev_ts / 1e9);
        std::fflush(g_csv);
      }
      t_last = now;
      last_pts = pts;
      last_pkts = pkts;
      last_imu = imu;
      last_cpu = cpu;
    }

    if (duration_s > 0 && measuring &&
        std::chrono::duration<double>(now - t_measure_start).count() >= duration_s) {
      g_run.store(false);
    }
    // Give up if the device never shows up.
    if (!measuring && std::chrono::duration<double>(now - t_start).count() > 60.0) {
      std::fprintf(stderr, "[demo] no point data within 60 s; aborting\n");
      g_run.store(false);
    }
  }

  double total = measuring ? std::chrono::duration<double>(Clock::now() - t_measure_start).count()
                           : 0.0;
  uint64_t pts = g_st.points.load() - base_pts, pkts = g_st.point_packets.load() - base_pkts,
           imu = g_st.imu_packets.load() - base_imu;
  uint64_t lost = g_st.lost_packets.load() - base_lost;
  uint64_t frames = g_st.frames.load() - base_frames;
  double cpu_used = CpuSeconds() - base_cpu;
  double imu_acc_mean = 0.0;
  {
    std::lock_guard<std::mutex> lk(g_st.imu_mtx);
    if (g_st.imu_acc_n.load()) imu_acc_mean = g_st.imu_acc_sum / g_st.imu_acc_n.load();
  }
  uint64_t first_ts, last_ts;
  {
    std::lock_guard<std::mutex> lk(g_st.mtx);
    first_ts = base_ts_ns ? base_ts_ns : g_st.first_ts_ns;
    last_ts = g_st.last_ts_ns;
  }
  rss_last = ResidentBytes();
  if (rss_last > rss_max) rss_max = rss_last;

  std::printf(
      "\n================ sdk_client_demo summary ================\n"
      "duration (measured)      : %.1f s\n"
      "point packets            : %" PRIu64 "\n"
      "points                   : %" PRIu64 "\n"
      "  of which no-return     : %" PRIu64 "\n"
      "mean point rate          : %.0f pts/s\n"
      "mean packet rate         : %.1f pkt/s\n"
      "IMU packets              : %" PRIu64 "  (%.2f Hz)\n"
      "IMU mean |acc|           : %.4f g\n"
      "packets lost (udp_cnt)   : %" PRIu64 "  (%.4f %%)\n"
      "frames observed          : %" PRIu64 "\n"
      "device timestamp span    : %.3f s (host span %.3f s, skew %.3f s)\n"
      "timestamp regressions    : %" PRIu64 "\n"
      "duplicate/stalled udp_cnt: %" PRIu64 "\n"
      "packets with dot_num!=96 : %" PRIu64 "\n"
      "packets with bad length  : %" PRIu64 "\n"
      "RSS first / last / max   : %.1f / %.1f / %.1f MB\n"
      "process CPU time         : %.1f s (%.1f %% of one core)\n"
      "=========================================================\n",
      total, pkts, pts, g_st.zero_points.load(), total > 0 ? pts / total : 0.0,
      total > 0 ? pkts / total : 0.0, imu, total > 0 ? imu / total : 0.0, imu_acc_mean,
      lost,
      (pkts + lost) ? 100.0 * lost / (pkts + lost) : 0.0,
      frames, (last_ts - first_ts) / 1e9, total,
      total - (last_ts - first_ts) / 1e9, g_st.ts_backwards.load(), g_st.dup_packets.load(),
      g_st.bad_dot_num.load(),
      g_st.bad_length.load(), rss_first / 1048576.0, rss_last / 1048576.0, rss_max / 1048576.0,
      cpu_used, total > 0 ? cpu_used / total * 100.0 : 0.0);

  if (g_csv) std::fclose(g_csv);
  LivoxLidarSdkUninit();
  return 0;
}
