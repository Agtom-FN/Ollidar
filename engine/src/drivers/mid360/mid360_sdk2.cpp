// The Livox-SDK2 backend.
//
// This file is ALWAYS compiled; only its body is conditional. When the
// engine is built without ENGINE_WITH_LIVOX_SDK2 (the default, because
// third_party/Livox-SDK2 is fetched rather than committed) the factory
// returns a failure that names the fetch script, and the rest of the driver
// — decode, filter, loss, watchdog, reconnect — still builds and still runs
// under the raw-UDP and inject backends. That is deliberate: a CI leg
// without the SDK must still test the logic that matters.
#include <cstdio>
#include <memory>
#include <string>

#include "mid360_backend.h"
#include "scanengine/core/log.h"

#if defined(SCANENGINE_HAVE_LIVOX_SDK2)
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <system_error>

#include "livox_lidar_api.h"
#include "livox_lidar_def.h"
#endif

namespace scanengine {

#if !defined(SCANENGINE_HAVE_LIVOX_SDK2)

std::unique_ptr<Mid360BackendImpl> make_sdk2_backend(Mid360Driver&, DeviceId id,
                                                     const Mid360Config&) {
  (void)set_last_error(
      ScanError::kNotSupported,
      "mid360 device %u: this engine was built without Livox-SDK2. Run "
      "engine/third_party/fetch_sdk2.sh (fetches the pinned SDK and applies the three "
      "portability patches — stock SDK2 cannot even bind a socket on macOS) and "
      "re-run cmake. To capture from an already-configured device, or to replay, use "
      "Mid360Backend::kRawUdp / kInject instead.",
      id);
  return nullptr;
}

#else  // SCANENGINE_HAVE_LIVOX_SDK2

namespace {

constexpr const char* kMod = "mid360";

// SDK2 IS A PROCESS-WIDE SINGLETON. LivoxLidarSdkInit/Uninit, the callback
// registrations and DeviceManager are all global, so exactly one driver
// instance may own it at a time. A second Mid-360 has to be handled as a
// second handle inside the same SDK instance (the config file takes a list
// of lidar IPs) — that is a real feature, and it is not A3's: multi-unit
// SN/IP-conflict behaviour is on S2's hardware-only list.
std::mutex g_sdk_owner_mutex;

// Callback dispatch. Callbacks arrive on SDK threads; close() must be able
// to guarantee that no callback is in flight before it calls Uninit(), which
// joins those threads. A shared_mutex does exactly that with no cost on the
// hot path beyond an uncontended shared lock:
//   callback: shared_lock, read owner, use it, release
//   close():  unique_lock, clear owner, release, THEN Uninit
// After close()'s unique_lock is released, no callback body can observe a
// non-null owner, so none can touch a driver that is about to die.
std::shared_mutex g_cb_mutex;
Mid360Driver* g_owner = nullptr;
bool g_sdk_claimed = false;

void PointCallback(uint32_t, const uint8_t, LivoxLidarEthernetPacket* data, void*) {
  if (data == nullptr) return;
  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;
  // `data` points at the received datagram: a 36-byte header followed by the
  // payload, exactly the bytes mid360_packets.h parses. `length` is the
  // whole datagram, which is what the driver validates against.
  g_owner->on_point_packet(reinterpret_cast<const std::uint8_t*>(data), data->length,
                           SteadyClock::now());
}

void ImuCallback(uint32_t, const uint8_t, LivoxLidarEthernetPacket* data, void*) {
  if (data == nullptr) return;
  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;
  g_owner->on_imu_packet(reinterpret_cast<const std::uint8_t*>(data), data->length,
                         SteadyClock::now());
}

void AsyncControlAck(livox_status status, uint32_t handle, LivoxLidarAsyncControlResponse* r,
                     void* what) {
  const char* label = static_cast<const char*>(what);
  if (status != kLivoxLidarStatusSuccess || r == nullptr || r->ret_code != 0) {
    SCAN_LOG_WARN(kMod, "handle %u: %s failed (status=%d ret=%u err_key=%u)", handle,
                  label ? label : "control", static_cast<int>(status), r ? r->ret_code : 255u,
                  r ? r->error_key : 0xFFFFu);
  } else {
    SCAN_LOG_DEBUG(kMod, "handle %u: %s ok", handle, label ? label : "control");
  }
}

// Fires once per device once the handshake completes. THIS is where the
// device is told what to send — and it is the callback S2 proved never
// re-fires after a power-cycle, which is why the driver's watchdog exists.
void InfoChangeCallback(const uint32_t handle, const LivoxLidarInfo* info, void*) {
  if (info == nullptr) return;
  {
    std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
    if (g_owner != nullptr) g_owner->on_device_connected(info->sn, info->lidar_ip);
  }
  // Millimetre Cartesian: A6 wants the resolution, and the centimetre type
  // saves bandwidth we are not short of on a dedicated link.
  SetLivoxLidarPclDataType(handle, kLivoxLidarCartesianCoordinateHighData, AsyncControlAck,
                           const_cast<char*>("set-pcl-data-type"));
  EnableLivoxLidarImuData(handle, AsyncControlAck, const_cast<char*>("enable-imu"));
  SetLivoxLidarWorkMode(handle, kLivoxLidarNormal, AsyncControlAck,
                        const_cast<char*>("set-work-mode-normal"));
  SCAN_LOG_INFO(kMod, "handle %u: configured (sn=%s ip=%s)", handle, info->sn, info->lidar_ip);
}

// The 1 Hz push-state heartbeat. Recorded as Mid360Stats::t_last_heartbeat_ns
// — a second, independent outage signal (S2 saw it stall 1.2 s after the
// point stream did). The driver does not yet DEMOTE on heartbeat-only
// silence: a device that heartbeats but does not stream is a real condition
// we have never observed, and inventing a policy for it before a real device
// shows us one would be guessing. The timestamp is exposed so the health
// panel can show it and so that policy has data to be written against.
void InfoPushCallback(const uint32_t, const uint8_t, const char*, void*) {
  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner != nullptr) g_owner->on_heartbeat(SteadyClock::now());
}

std::string sdk_config_json(const UdpConfig& u) {
  // SDK2's entry point takes a config FILE, so we write one. The shape is
  // Livox's own (see the SDK samples and spikes/s2-mid360-sim/config/): a
  // per-model block naming the device ports, plus a host_net_info array
  // saying where the device should stream. The device is TOLD; it does not
  // discover us.
  char buf[2048];
  std::snprintf(buf, sizeof(buf),
                "{\n"
                "  \"MID360\": {\n"
                "    \"lidar_net_info\": {\n"
                "      \"cmd_data_port\": %u,\n"
                "      \"push_msg_port\": %u,\n"
                "      \"point_data_port\": %u,\n"
                "      \"imu_data_port\": %u,\n"
                "      \"log_data_port\": %u\n"
                "    },\n"
                "    \"host_net_info\": [\n"
                "      {\n"
                "        \"lidar_ip\": [\"%s\"],\n"
                "        \"host_ip\": \"%s\",\n"
                "        \"multicast_ip\": \"\",\n"
                "        \"cmd_data_port\": %u,\n"
                "        \"push_msg_port\": %u,\n"
                "        \"point_data_port\": %u,\n"
                "        \"imu_data_port\": %u,\n"
                "        \"log_data_port\": %u\n"
                "      }\n"
                "    ]\n"
                "  }\n"
                "}\n",
                static_cast<unsigned>(u.cmd_port), static_cast<unsigned>(u.push_port),
                static_cast<unsigned>(u.point_port), static_cast<unsigned>(u.imu_port),
                static_cast<unsigned>(u.log_port), u.lidar_ip.c_str(), u.host_ip.c_str(),
                static_cast<unsigned>(u.host_cmd_port), static_cast<unsigned>(u.host_push_port),
                static_cast<unsigned>(u.host_point_port), static_cast<unsigned>(u.host_imu_port),
                static_cast<unsigned>(u.host_log_port));
  return std::string(buf);
}

class Sdk2Backend final : public Mid360BackendImpl {
 public:
  Sdk2Backend(Mid360Driver& driver, DeviceId id, const Mid360Config& cfg)
      : driver_(driver), id_(id), cfg_(cfg) {}

  ~Sdk2Backend() override {
    close();
    std::lock_guard<std::mutex> lock(g_sdk_owner_mutex);
    if (claimed_) {
      g_sdk_claimed = false;
      claimed_ = false;
    }
  }

  const char* backend_name() const override { return "sdk2"; }

  Status open() override {
    {
      std::lock_guard<std::mutex> lock(g_sdk_owner_mutex);
      if (!claimed_) {
        if (g_sdk_claimed) {
          return set_last_error(
              ScanError::kBusy,
              "mid360 device %u: Livox-SDK2 is a process-wide singleton and another "
              "Mid-360 driver already owns it. A second unit belongs in the same SDK "
              "instance as a second lidar_ip, which is not implemented (S2 lists "
              "multi-unit behaviour as hardware-only).",
              id_);
        }
        g_sdk_claimed = true;
        claimed_ = true;
      }
    }

    SCAN_TRY(write_config());

    if (!cfg_.sdk_console_log) DisableLivoxSdkConsoleLogger();

    if (!LivoxLidarSdkInit(config_path_.c_str())) {
      // The overwhelmingly likely cause on a fresh machine is a host_ip that
      // is not actually on this box, so name it.
      return set_last_error(ScanError::kIoError,
                            "mid360 device %u: LivoxLidarSdkInit('%s') failed. Check that "
                            "host_ip '%s' is a local address and that ports %u/%u/%u are free. "
                            "(On macOS this also requires the PATCHED SDK from "
                            "engine/third_party/fetch_sdk2.sh — stock SDK2 fails its "
                            "broadcast bind here.)",
                            id_, config_path_.c_str(), cfg_.udp.host_ip.c_str(),
                            static_cast<unsigned>(cfg_.udp.host_cmd_port),
                            static_cast<unsigned>(cfg_.udp.host_point_port),
                            static_cast<unsigned>(cfg_.udp.host_imu_port));
    }
    inited_ = true;

    SetLivoxLidarPointCloudCallBack(PointCallback, nullptr);
    SetLivoxLidarImuDataCallback(ImuCallback, nullptr);
    SetLivoxLidarInfoChangeCallback(InfoChangeCallback, nullptr);
    SetLivoxLidarInfoCallback(InfoPushCallback, nullptr);

    {
      std::unique_lock<std::shared_mutex> lock(g_cb_mutex);
      g_owner = &driver_;
    }

    if (!LivoxLidarSdkStart()) {
      close();
      return set_last_error(ScanError::kIoError, "mid360 device %u: LivoxLidarSdkStart() failed",
                            id_);
    }

    SCAN_LOG_INFO(kMod, "device %u: SDK2 up (config %s), waiting for %s", id_,
                  config_path_.c_str(), cfg_.udp.lidar_ip.c_str());
    return kOkStatus;
  }

  void close() override {
    {
      // Drain in-flight callbacks BEFORE Uninit joins the threads that make
      // them; see the g_cb_mutex note above.
      std::unique_lock<std::shared_mutex> lock(g_cb_mutex);
      if (g_owner == &driver_) g_owner = nullptr;
    }
    if (inited_) {
      LivoxLidarSdkUninit();
      inited_ = false;
      SCAN_LOG_INFO(kMod, "device %u: SDK2 torn down", id_);
    }
    if (owns_config_file_ && !config_path_.empty()) {
      std::remove(config_path_.c_str());
      owns_config_file_ = false;
    }
  }

 private:
  Status write_config() {
    if (!cfg_.generate_sdk_config) {
      if (cfg_.sdk_config_path.empty()) {
        return set_last_error(ScanError::kInvalidArgument,
                              "mid360 device %u: generate_sdk_config is off but "
                              "sdk_config_path is empty",
                              id_);
      }
      config_path_ = cfg_.sdk_config_path;
      owns_config_file_ = false;
      return kOkStatus;
    }

    config_path_ = cfg_.sdk_config_path;
    if (config_path_.empty()) {
      // Unique per open(), because a forced re-init writes a fresh one while
      // the SDK may still be letting go of the last.
      static std::atomic<std::uint64_t> serial{0};
      std::error_code ec;
      std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
      if (ec) dir = std::filesystem::path(".");
      char leaf[96];
      std::snprintf(leaf, sizeof(leaf), "scanengine_mid360_%u_%llu.json", id_,
                    static_cast<unsigned long long>(serial.fetch_add(1)));
      config_path_ = (dir / leaf).string();
      owns_config_file_ = true;
    }

    std::ofstream out(config_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
      return set_last_error(ScanError::kFileError,
                            "mid360 device %u: cannot write SDK config '%s' (%s)", id_,
                            config_path_.c_str(), std::strerror(errno));
    }
    out << sdk_config_json(cfg_.udp);
    out.close();
    return kOkStatus;
  }

  Mid360Driver& driver_;
  DeviceId id_;
  Mid360Config cfg_;
  std::string config_path_;
  bool owns_config_file_ = false;
  bool inited_ = false;
  bool claimed_ = false;
};

}  // namespace

std::unique_ptr<Mid360BackendImpl> make_sdk2_backend(Mid360Driver& driver, DeviceId id,
                                                     const Mid360Config& cfg) {
  return std::make_unique<Sdk2Backend>(driver, id, cfg);
}

#endif  // SCANENGINE_HAVE_LIVOX_SDK2

}  // namespace scanengine
