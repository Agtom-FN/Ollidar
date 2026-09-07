// The Livox-SDK v1 backend for the Mid-70.
//
// ALWAYS compiled; only the body is conditional. Without
// ENGINE_WITH_LIVOX_SDK1 (the default until third_party/fetch_sdk1.sh has
// run) the factory returns a failure that names the fetch script, and the
// raw-UDP / inject backends still exercise every other line of the driver.
#include <cstdio>
#include <memory>
#include <string>

#include "mid70_backend.h"
#include "scanengine/core/log.h"

#if defined(SCANENGINE_HAVE_LIVOX_SDK1)
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "livox_def.h"
#include "livox_sdk.h"
#endif

namespace scanengine {

#if !defined(SCANENGINE_HAVE_LIVOX_SDK1)

std::unique_ptr<Mid70BackendImpl> make_sdk1_backend(Mid70Driver&, DeviceId id,
                                                    const Mid70Config&) {
  (void)set_last_error(
      ScanError::kNotSupported,
      "mid70 device %u: this engine was built without Livox-SDK v1. Run "
      "engine/third_party/fetch_sdk1.sh (fetches the pinned SDK and applies the three "
      "build patches) and re-run cmake. To capture from an already-streaming device, or "
      "to replay, use Mid70Backend::kRawUdp / kInject instead.",
      id);
  return nullptr;
}

Status mid70_sdk1_set_rmc_sync_time(const char*, std::uint32_t) {
  return set_last_error(ScanError::kNotSupported,
                        "mid70: LidarSetRmcSyncTime needs Livox-SDK v1; this engine was "
                        "built without it (engine/third_party/fetch_sdk1.sh)");
}

#else  // SCANENGINE_HAVE_LIVOX_SDK1

namespace {

constexpr const char* kMod = "mid70";

// SDK v1 IS A PROCESS-WIDE SINGLETON, and more so than SDK2: Init(), Start(),
// Uninit(), SetBroadcastCallback() and SetDeviceStateUpdateCallback() are
// free functions over one global DeviceManager, and the two callbacks carry
// NO user pointer at all. So exactly one driver instance may own the SDK at a
// time, and the callbacks have to find their driver through a global. Two
// Mid-70s on one wire would be two handles inside this same SDK instance —
// a real feature, and not A17's (the hardware to test it does not exist here).
std::mutex g_sdk_owner_mutex;
bool g_sdk_claimed = false;

// Callback dispatch, identical in shape and reason to mid360_sdk2.cpp:
// callbacks arrive on SDK threads, and close() must guarantee that no
// callback is in flight before Uninit() joins those threads.
//   callback: shared_lock, read g_owner, use it, release
//   close():  unique_lock, clear g_owner, release, THEN Uninit
// After close()'s unique_lock is released no callback body can observe a
// non-null owner, so none can touch a driver that is about to die.
std::shared_mutex g_cb_mutex;
Mid70Driver* g_owner = nullptr;

// --- handshake bookkeeping -------------------------------------------------
//
// Guarded by g_state_mutex, which is ALWAYS taken *after* g_cb_mutex (every
// callback shared-locks g_cb_mutex first), so the two can never deadlock.
std::mutex g_state_mutex;

// Which lidar we want. Empty = the first Mid-70 that broadcasts.
std::string g_want_code;
bool g_dual_return = false;

// One connected device, by design (see the singleton note above).
bool g_have_handle = false;
std::uint8_t g_handle = 0;

// Where the device is in the bring-up. The ROS driver's connect_state under
// another name, and it exists for the reason A17's manual §4 gives: a cold
// Mid-70 answers the handshake while still self-heating and reports
// kLidarStateInit for up to three minutes. Configuring it then is refused by
// the device, so we wait for the kEventStateChange that says Normal.
enum class Phase : std::uint8_t {
  kIdle = 0,        // no device, or disconnected
  kConnected = 1,   // handshake done, not yet configured
  kConfiguring = 2, // coordinate + return-mode commands outstanding
  kSampling = 3,    // LidarStartSampling acked
};
Phase g_phase = Phase::kIdle;

// Outstanding config acks. LidarStartSampling only goes out once BOTH have
// come back — the order the livox_ros_driver used against real Mid-70
// firmware. Starting sampling first and configuring after produces a stream
// in whatever mode the device happened to keep from its last session.
constexpr std::uint32_t kCfgCoordinate = 1u << 0;
constexpr std::uint32_t kCfgReturnMode = 1u << 1;
std::uint32_t g_pending_cfg = 0;

// A device that refuses a config command usually keeps refusing it. The ROS
// driver retried forever, which on a device in a bad mode is a callback loop
// at command-timeout rate. Three tries, then leave it to the driver's
// watchdog: a forced re-init tears the SDK down and re-handshakes, which is
// the only recovery that has ever worked anyway.
constexpr std::uint32_t kMaxConfigRetries = 3;
std::uint32_t g_cfg_retries = 0;

// Last err_code seen from the 1 Hz error message, so a steady state is not
// re-logged once a second.
std::uint32_t g_last_err_code = 0;
bool g_have_err_code = false;

// One warning per open() for a data type we cannot size (see PointDataCallback).
std::atomic<bool> g_warned_unknown_type{false};

void reset_handshake_state() {
  g_have_handle = false;
  g_handle = 0;
  g_phase = Phase::kIdle;
  g_pending_cfg = 0;
  g_cfg_retries = 0;
  g_last_err_code = 0;
  g_have_err_code = false;
}

// --- datagram sizing -------------------------------------------------------
//
// The DataCallback hands us `data_num` POINTS, not bytes, and the driver
// wants the whole datagram exactly as it arrived (that is what the raw sink
// records and what mid70_packets.h parses). The SDK derived data_num from
// the received size with
//     data_num = (size - 18) / sizeof(<point type for data->data_type>)
// (sdk_core/src/data_handler/data_handler.cpp, kPrefixDataSize = 18), so
// inverting it recovers the exact byte count — modulo any trailing bytes the
// SDK's integer division already discarded, which by construction is what
// the SDK itself considers the packet to be.
//
// Sizes come from the SDK's own structs rather than mid70_packets.h's
// mirrored ones, because it is the SDK's sizeof that produced data_num.
// The two agree; the static_asserts below hold that agreement to account.
std::size_t per_point_bytes(std::uint8_t data_type) {
  switch (data_type) {
    case kCartesian:             return sizeof(LivoxRawPoint);              // 13
    case kSpherical:             return sizeof(LivoxSpherPoint);            // 9
    case kExtendCartesian:       return sizeof(LivoxExtendRawPoint);        // 14
    case kExtendSpherical:       return sizeof(LivoxExtendSpherPoint);      // 10
    case kDualExtendCartesian:   return sizeof(LivoxDualExtendRawPoint);    // 28
    case kDualExtendSpherical:   return sizeof(LivoxDualExtendSpherPoint);  // 16
    case kImu:                   return sizeof(LivoxImuPoint);              // 24
    case kTripleExtendCartesian: return sizeof(LivoxTripleExtendRawPoint);  // 42
    case kTripleExtendSpherical: return sizeof(LivoxTripleExtendSpherPoint);// 22
    default:                     return 0;
  }
}

static_assert(sizeof(LivoxRawPoint) == sizeof(mid70::RawPoint),
              "SDK v1 cartesian point and our mirror disagree");
static_assert(sizeof(LivoxExtendRawPoint) == sizeof(mid70::ExtendRawPoint),
              "SDK v1 extended cartesian point and our mirror disagree");
static_assert(sizeof(LivoxDualExtendRawPoint) == sizeof(mid70::DualExtendRawPoint),
              "SDK v1 dual extended cartesian point and our mirror disagree");
static_assert(sizeof(LivoxImuPoint) == sizeof(mid70::ImuPoint),
              "SDK v1 IMU sample and our mirror disagree");
// The 18-byte header prefix is OUR constant: sizeof(LivoxEthPacket) is 19,
// because the struct declares a one-byte `data[1]` tail.
static_assert(sizeof(mid70::EthHeader) == 18, "Mid-70 datagram prefix is 18 bytes");

std::string firmware_string(const std::uint8_t v[4]) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
  return std::string(buf);
}

// --- config steps ----------------------------------------------------------

struct ConfigStep {
  std::uint32_t bit;
  const char* label;
  void (*send)(std::uint8_t handle);  // re-sends this step on a failed ack
};

void SendCoordinate(std::uint8_t handle);
void SendReturnMode(std::uint8_t handle);

const ConfigStep kStepCoordinate{kCfgCoordinate, "set-cartesian-coordinate", &SendCoordinate};
const ConfigStep kStepReturnMode{kCfgReturnMode, "set-point-cloud-return-mode", &SendReturnMode};

void ConfigAck(livox_status status, std::uint8_t handle, std::uint8_t response,
               void* client_data);
void StartSampleAck(livox_status status, std::uint8_t handle, std::uint8_t response, void*);
void RmcSyncAck(livox_status status, std::uint8_t handle, std::uint8_t response, void*);

void SendCoordinate(std::uint8_t handle) {
  // Cartesian, millimetres (data_type 0 or 2). A6 wants the resolution, and
  // the spherical types would only mean converting back.
  const livox_status s = SetCartesianCoordinate(handle, ConfigAck,
                                                const_cast<ConfigStep*>(&kStepCoordinate));
  if (s != kStatusSuccess) {
    SCAN_LOG_WARN(kMod, "handle %u: SetCartesianCoordinate returned %d", handle,
                  static_cast<int>(s));
  }
}

void SendReturnMode(std::uint8_t handle) {
  // kFirstReturn, not kStrongestReturn: first return is what the FAST-LIO
  // work this port descends from ran, and it is what the driver's decimation
  // budget (live_points_per_sec against 100k pts/s) is sized for. Dual return
  // doubles the rate to 200k pts/s and switches the wire type to
  // kDualExtendCartesian, which mid70_packets.h decodes but which nothing has
  // yet been tuned against.
  const PointCloudReturnMode mode = g_dual_return ? kDualReturn : kFirstReturn;
  const livox_status s = LidarSetPointCloudReturnMode(handle, mode, ConfigAck,
                                                      const_cast<ConfigStep*>(&kStepReturnMode));
  if (s != kStatusSuccess) {
    SCAN_LOG_WARN(kMod, "handle %u: LidarSetPointCloudReturnMode returned %d", handle,
                  static_cast<int>(s));
  }
}

// Caller holds g_state_mutex.
void begin_configure_locked(std::uint8_t handle) {
  g_phase = Phase::kConfiguring;
  g_pending_cfg = kCfgCoordinate | kCfgReturnMode;
  g_cfg_retries = 0;
  SCAN_LOG_INFO(kMod, "handle %u: configuring (cartesian mm, %s return)", handle,
                g_dual_return ? "dual" : "first");
  SendCoordinate(handle);
  SendReturnMode(handle);
}

void ConfigAck(livox_status status, std::uint8_t handle, std::uint8_t response,
               void* client_data) {
  const ConfigStep* step = static_cast<const ConfigStep*>(client_data);
  if (step == nullptr) return;

  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;

  std::lock_guard<std::mutex> state(g_state_mutex);
  if (!g_have_handle || handle != g_handle || g_phase != Phase::kConfiguring) return;

  if (status != kStatusSuccess || response != 0) {
    if (g_cfg_retries < kMaxConfigRetries) {
      ++g_cfg_retries;
      SCAN_LOG_WARN(kMod, "handle %u: %s failed (status=%d response=%u), retry %u/%u", handle,
                    step->label, static_cast<int>(status), static_cast<unsigned>(response),
                    g_cfg_retries, kMaxConfigRetries);
      step->send(handle);
    } else {
      SCAN_LOG_ERROR(kMod,
                     "handle %u: %s failed %u times (status=%d response=%u); back to "
                     "connected — the next kLidarStateNormal state change re-configures, "
                     "and the driver's watchdog re-inits if none comes",
                     handle, step->label, kMaxConfigRetries, static_cast<int>(status),
                     static_cast<unsigned>(response));
      // Fable review: kConfiguring with nothing outstanding was a dead end —
      // StateUpdateCallback only configures from kConnected.
      g_phase = Phase::kConnected;
      g_pending_cfg = 0;
      g_cfg_retries = 0;
    }
    return;
  }

  SCAN_LOG_DEBUG(kMod, "handle %u: %s ok", handle, step->label);
  g_pending_cfg &= ~step->bit;
  if (g_pending_cfg != 0) return;

  const livox_status s = LidarStartSampling(handle, StartSampleAck, nullptr);
  if (s != kStatusSuccess) {
    SCAN_LOG_WARN(kMod, "handle %u: LidarStartSampling returned %d", handle,
                  static_cast<int>(s));
  }
}

void StartSampleAck(livox_status status, std::uint8_t handle, std::uint8_t response, void*) {
  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;

  std::lock_guard<std::mutex> state(g_state_mutex);
  if (!g_have_handle || handle != g_handle) return;

  if (status != kStatusSuccess || response != 0) {
    // No retry here. A refused start is either a device that is not Normal
    // yet or one in a bad mode, and both are the watchdog's problem — it has
    // the wall clock this callback does not.
    SCAN_LOG_ERROR(kMod, "handle %u: LidarStartSampling failed (status=%d response=%u)",
                   handle, static_cast<int>(status), static_cast<unsigned>(response));
    // Same dead-end fix as the config acks: roll back so the next Normal
    // state change runs the whole configure + start again.
    g_phase = Phase::kConnected;
    g_pending_cfg = 0;
    g_cfg_retries = 0;
    return;
  }
  g_phase = Phase::kSampling;
  SCAN_LOG_INFO(kMod, "handle %u: sampling", handle);
}

// Ack for the optional PPS+GPS passthrough below. Logged, not propagated:
// the caller's Status said only that the command went out.
void RmcSyncAck(livox_status status, std::uint8_t handle, std::uint8_t response, void*) {
  if (status != kStatusSuccess || response != 0) {
    SCAN_LOG_WARN(kMod, "handle %u: LidarSetRmcSyncTime rejected (status=%d response=%u)",
                  handle, static_cast<int>(status), static_cast<unsigned>(response));
  } else {
    SCAN_LOG_INFO(kMod, "handle %u: RMC sync time accepted", handle);
  }
}

// --- SDK callbacks ---------------------------------------------------------

void PointDataCallback(std::uint8_t handle, LivoxEthPacket* data, std::uint32_t data_num,
                       void*) {
  if (data == nullptr || data_num == 0) return;

  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;

  const std::size_t point_bytes = per_point_bytes(data->data_type);
  if (point_bytes == 0) {
    // A data type this build does not know the size of: we cannot say where
    // the datagram ends, so passing it on would hand the driver — and the
    // raw recorder — a made-up length. Drop it, once loudly.
    if (!g_warned_unknown_type.exchange(true)) {
      SCAN_LOG_WARN(kMod, "handle %u: dropping datagrams with unknown data_type %u", handle,
                    static_cast<unsigned>(data->data_type));
    }
    return;
  }

  const std::size_t len = sizeof(mid70::EthHeader) + static_cast<std::size_t>(data_num) * point_bytes;
  g_owner->on_point_packet(reinterpret_cast<const std::uint8_t*>(data), len, SteadyClock::now());
}

// ~1 Hz per connected device. The Mid-70 reports its clock health here AND in
// every datagram's err_code; the driver reads the datagram copy (that is the
// one tied to the points), so this callback only logs — and only on a change,
// because a stuck warning printed once a second is a warning nobody reads.
void ErrorMessageCallback(livox_status status, std::uint8_t handle, ErrorMessage* message) {
  if (message == nullptr) return;

  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;

  std::lock_guard<std::mutex> state(g_state_mutex);
  const std::uint32_t code = message->error_code;
  if (g_have_err_code && code == g_last_err_code) return;
  g_have_err_code = true;
  g_last_err_code = code;

  const LidarErrorCode e = message->lidar_error_code;
  const char* level = (e.system_status == 0) ? "ok" : (e.system_status == 1 ? "warning" : "error");
  SCAN_LOG_INFO(kMod,
                "handle %u: device status %s (err_code=0x%08x status=%d temp=%u volt=%u "
                "motor=%u dirty=%u pps=%u time_sync=%u)",
                handle, level, code, static_cast<int>(status),
                static_cast<unsigned>(e.temp_status), static_cast<unsigned>(e.volt_status),
                static_cast<unsigned>(e.motor_status), static_cast<unsigned>(e.dirty_warn),
                static_cast<unsigned>(e.pps_status), static_cast<unsigned>(e.time_sync_status));
}

void DeviceInformationCallback(livox_status status, std::uint8_t handle,
                               DeviceInformationResponse* response, void*) {
  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;
  if (status != kStatusSuccess || response == nullptr || response->ret_code != 0) {
    SCAN_LOG_WARN(kMod, "handle %u: QueryDeviceInformation failed (status=%d)", handle,
                  static_cast<int>(status));
    return;
  }
  // Only the firmware field: on_device_connected() leaves a null argument
  // alone, so this refines the row the connect event already published.
  const std::string fw = firmware_string(response->firmware_version);
  g_owner->on_device_connected(nullptr, nullptr, fw.c_str());
}

// The device broadcasts on UDP 55000 once a second until someone answers.
// This is where we decide whether it is ours.
void BroadcastCallback(const BroadcastDeviceInfo* info) {
  if (info == nullptr) return;

  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;

  std::lock_guard<std::mutex> state(g_state_mutex);

  if (info->dev_type != kDeviceTypeLidarMid70) {
    // A Horizon, an Avia or a Hub on the same wire. Not an error — say so
    // once per device type rather than staying silent, because "the app does
    // not see my lidar" is otherwise unexplainable from the log.
    SCAN_LOG_DEBUG(kMod, "ignoring broadcast from %s: dev_type %u is not a Mid-70",
                   info->broadcast_code, static_cast<unsigned>(info->dev_type));
    return;
  }
  if (!g_want_code.empty() && g_want_code != info->broadcast_code) {
    SCAN_LOG_DEBUG(kMod, "ignoring Mid-70 %s: configured broadcast_code is %s",
                   info->broadcast_code, g_want_code.c_str());
    return;
  }
  if (g_have_handle) return;  // already connected to one; see the singleton note

  std::uint8_t handle = 0;
  const livox_status s = AddLidarToConnect(info->broadcast_code, &handle);
  if (s != kStatusSuccess || handle >= kMaxLidarCount) {
    SCAN_LOG_WARN(kMod, "AddLidarToConnect(%s) failed (status=%d handle=%u)",
                  info->broadcast_code, static_cast<int>(s), static_cast<unsigned>(handle));
    return;
  }

  // Both callbacks are registered BEFORE the connection completes, per the
  // SDK's own note on SetDataCallback ("set the point cloud data callback
  // before beginning sampling").
  SetDataCallback(handle, PointDataCallback, nullptr);
  (void)SetErrorMessageCallback(handle, ErrorMessageCallback);

  g_have_handle = true;
  g_handle = handle;
  g_phase = Phase::kIdle;
  SCAN_LOG_INFO(kMod, "connecting to Mid-70 %s at %s (handle %u)", info->broadcast_code,
                info->ip, static_cast<unsigned>(handle));
}

void StateUpdateCallback(const DeviceInfo* device, DeviceEvent type) {
  if (device == nullptr) return;

  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) return;

  {
    std::lock_guard<std::mutex> state(g_state_mutex);
    if (!g_have_handle || device->handle != g_handle) return;

    if (type == kEventDisconnect) {
      SCAN_LOG_INFO(kMod, "handle %u: SDK reports disconnect", device->handle);
      // Keep the handle: SDK v1 re-discovers the device on its own when it
      // starts broadcasting again and fires kEventConnect on the same handle.
      // Only the bring-up state is rolled back, so the config runs again —
      // a device that rebooted has forgotten what we told it.
      g_phase = Phase::kIdle;
      g_pending_cfg = 0;
      g_cfg_retries = 0;
      g_have_err_code = false;
    } else if (type == kEventConnect) {
      if (g_phase == Phase::kIdle) {
        g_phase = Phase::kConnected;
        // Firmware is in DeviceInfo already; QueryDeviceInformation below is
        // the SDK's own answer to the same question and refines it if the
        // pushed copy is stale.
        const std::string fw = firmware_string(device->firmware_version);
        g_owner->on_device_connected(device->broadcast_code, device->ip, fw.c_str());
        SCAN_LOG_INFO(kMod, "handle %u: connected (code=%s ip=%s fw=%s state=%d)",
                      device->handle, device->broadcast_code, device->ip, fw.c_str(),
                      static_cast<int>(device->state));
        (void)QueryDeviceInformation(device->handle, DeviceInformationCallback, nullptr);
      }
    } else if (type == kEventStateChange) {
      SCAN_LOG_INFO(kMod, "handle %u: state %d (status_code=0x%08x)", device->handle,
                    static_cast<int>(device->state), device->status.status_code.error_code);
      // A device that leaves Normal mid-configure or mid-sampling (back into
      // Init after a reboot, or into an error state) has to be configured
      // again when it comes back. Without this a stale "Normal" in the SDK's
      // DeviceInfo — it survives kEventDisconnect (device_manager.cpp:86-98)
      // — could start a configure on a unit that is really still heating,
      // whose acks then time out, and nothing would re-arm it.
      if (device->state != kLidarStateNormal &&
          (g_phase == Phase::kConfiguring || g_phase == Phase::kSampling)) {
        SCAN_LOG_WARN(kMod, "handle %u: left Normal while %s; will re-configure on Normal",
                      device->handle, g_phase == Phase::kSampling ? "sampling" : "configuring");
        g_phase = Phase::kConnected;
        g_pending_cfg = 0;
        g_cfg_retries = 0;
      }
    }

    // Configure as soon as — and only once — the device says it is Normal.
    // A Mid-70 that is still self-heating sits in kLidarStateInit and refuses
    // the commands; the kEventStateChange that lifts it is the trigger.
    if (g_phase == Phase::kConnected && device->state == kLidarStateNormal) {
      begin_configure_locked(device->handle);
    }
  }

  if (type == kEventDisconnect) g_owner->on_device_disconnected();
}

class Sdk1Backend final : public Mid70BackendImpl {
 public:
  Sdk1Backend(Mid70Driver& driver, DeviceId id, const Mid70Config& cfg)
      : driver_(driver), id_(id), cfg_(cfg) {}

  ~Sdk1Backend() override {
    close();
    std::lock_guard<std::mutex> lock(g_sdk_owner_mutex);
    if (claimed_) {
      g_sdk_claimed = false;
      claimed_ = false;
    }
  }

  const char* backend_name() const override { return "sdk1"; }

  Status open() override {
    {
      std::lock_guard<std::mutex> lock(g_sdk_owner_mutex);
      if (!claimed_) {
        if (g_sdk_claimed) {
          return set_last_error(
              ScanError::kBusy,
              "mid70 device %u: Livox-SDK v1 is a process-wide singleton (Init/Start/Uninit "
              "and the broadcast callback are global, with no user pointer) and another "
              "driver already owns it. A second Mid-70 belongs on a second handle inside "
              "this same SDK instance, which is not implemented.",
              id_);
        }
        g_sdk_claimed = true;
        claimed_ = true;
      }
    }

    // Before Init(): the SDK installs its console sink during initialisation.
    if (!cfg_.sdk_console_log) DisableConsoleLogger();

    if (!Init()) {
      close();
      return set_last_error(
          ScanError::kIoError,
          "mid70 device %u: Livox-SDK v1 Init() failed. It binds UDP %u for device "
          "broadcasts and a command socket toward the lidar's %u; check that no other "
          "Livox process (this engine's own Mid-360 path included) holds them and that "
          "the host firewall allows inbound UDP.",
          id_, static_cast<unsigned>(mid70::kBroadcastPort),
          static_cast<unsigned>(mid70::kLidarCmdPort));
    }
    inited_ = true;

    {
      std::lock_guard<std::mutex> state(g_state_mutex);
      reset_handshake_state();
      g_want_code = cfg_.broadcast_code;
      g_dual_return = cfg_.dual_return;
    }
    g_warned_unknown_type.store(false);

    SetBroadcastCallback(BroadcastCallback);
    SetDeviceStateUpdateCallback(StateUpdateCallback);

    // Owner before Start(): Start() spins up the discovery thread, and a
    // broadcast can land on the very next tick.
    {
      std::unique_lock<std::shared_mutex> lock(g_cb_mutex);
      g_owner = &driver_;
    }

    if (!Start()) {
      close();
      return set_last_error(ScanError::kIoError,
                            "mid70 device %u: Livox-SDK v1 Start() failed — on macOS that is "
                            "almost always UDP 55000 already bound (a discovery pass still "
                            "running, Livox Viewer, or a livox_ros_driver node)",
                            id_);
    }

    LivoxSdkVersion v{};
    GetLivoxSdkVersion(&v);
    if (cfg_.broadcast_code.empty()) {
      // Say this out loud: unlike the Mid-360's SDK2 config file, v1 has no
      // place to put a lidar IP. Selection is by broadcast code only, so a
      // configured udp.lidar_ip does nothing on this backend.
      SCAN_LOG_INFO(kMod,
                    "device %u: SDK v1 %d.%d.%d up, waiting for any Mid-70 to broadcast on "
                    "UDP %u%s",
                    id_, v.major, v.minor, v.patch, static_cast<unsigned>(mid70::kBroadcastPort),
                    cfg_.udp.lidar_ip.empty()
                        ? ""
                        : " (udp.lidar_ip is ignored here: v1 selects by broadcast code)");
    } else {
      SCAN_LOG_INFO(kMod, "device %u: SDK v1 %d.%d.%d up, waiting for Mid-70 %s on UDP %u", id_,
                    v.major, v.minor, v.patch, cfg_.broadcast_code.c_str(),
                    static_cast<unsigned>(mid70::kBroadcastPort));
    }
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
      // No LidarStopSampling first, deliberately: it is an async command
      // whose ack would arrive on a thread Uninit() is about to join, so
      // "did the device get it?" is unanswerable. The device stops on its own
      // when the connection drops, and the next open() re-handshakes it into
      // a known mode regardless — which is the same contract as the Mid-360.
      Uninit();
      inited_ = false;
      std::lock_guard<std::mutex> state(g_state_mutex);
      reset_handshake_state();
      SCAN_LOG_INFO(kMod, "device %u: SDK v1 torn down", id_);
    }
  }

 private:
  Mid70Driver& driver_;
  DeviceId id_;
  Mid70Config cfg_;
  bool inited_ = false;
  bool claimed_ = false;
};

}  // namespace

std::unique_ptr<Mid70BackendImpl> make_sdk1_backend(Mid70Driver& driver, DeviceId id,
                                                    const Mid70Config& cfg) {
  return std::make_unique<Sdk1Backend>(driver, id, cfg);
}

Status mid70_sdk1_set_rmc_sync_time(const char* rmc, std::uint32_t len) {
  if (rmc == nullptr || len == 0 || len > 0xFFFFu) {
    return set_last_error(ScanError::kInvalidArgument,
                          "mid70: RMC sentence must be 1..65535 bytes (got %u)", len);
  }
  std::shared_lock<std::shared_mutex> lock(g_cb_mutex);
  if (g_owner == nullptr) {
    return set_last_error(ScanError::kInvalidState,
                          "mid70: no SDK v1 backend is open, so there is no device to send a "
                          "GPRMC sentence to");
  }
  std::lock_guard<std::mutex> state(g_state_mutex);
  if (!g_have_handle) {
    return set_last_error(ScanError::kInvalidState,
                          "mid70: SDK v1 is up but no Mid-70 has been connected yet");
  }
  // The command is asynchronous, so this returns "sent", not "accepted"; the
  // ack lands in RmcSyncAck. Whether the device then actually locks to the
  // sentence is only observable in the datagrams' time_sync_status, which is
  // where the driver reads it.
  const livox_status s = LidarSetRmcSyncTime(g_handle, rmc, static_cast<std::uint16_t>(len),
                                             RmcSyncAck, nullptr);
  if (s != kStatusSuccess) {
    return set_last_error(ScanError::kIoError,
                          "mid70: LidarSetRmcSyncTime(handle %u) returned %d",
                          static_cast<unsigned>(g_handle), static_cast<int>(s));
  }
  return kOkStatus;
}

#endif  // SCANENGINE_HAVE_LIVOX_SDK1

}  // namespace scanengine
