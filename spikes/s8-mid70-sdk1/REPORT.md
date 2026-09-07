# S8 — Livox-SDK v1 on macOS, for the Mid-70 (task A17)

Status: SDK **builds** on this Mac (Apple Silicon, macOS, CMake 4.4.2, Apple
clang from the Command Line Tools) for arm64 and for the engine's
`arm64;x86_64` universal slice, after two patches. **Not yet run against a
lidar** — §5 lists what that still owes. Every claim marked **[S8]** was
measured here on 2026-09-07; nothing below is copied from a README.

The S2 spike did this exercise for SDK2 and the Mid-360; this is the same
exercise for SDK **v1**, which is what a Mid-70 speaks. They are different
protocols with different discovery, different datagram layouts and different
failure modes, and nothing from S2's patches applies as-is.

---

## 1. What was measured

| Question | Answer | Evidence |
| --- | --- | --- |
| Does stock SDK v1 configure with CMake ≥ 4? | **No.** `cmake_minimum_required(VERSION 3.0)` in both `CMakeLists.txt` and `sdk_core/CMakeLists.txt`; CMake 4 refuses compatibility below 3.5. | `build-stock/cmake.log` **[S8]** |
| With `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, does `sdk_core` compile? | **No.** `sdk_core/CMakeLists.txt:29-32` passes `-Wall -Werror` for GNU / AppleClang / Clang, and the SDK's *vendored* spdlog/fmt headers trip `-Wdeprecated-literal-operator` (`identifier '_u' preceded by whitespace`) and `-Wdeprecated-declarations` (`char_traits<T>` for non-char T) under current clang. No error originates in Livox's own code. | `build-policy/build.log` **[S8]** |
| With those demoted, does it link? | **Yes.** `libProject_static.a`, 1,397,872 bytes, arm64. No remaining warnings of any kind. | `build-fix/build.log` **[S8]** |
| Universal (`arm64;x86_64`)? | **Yes.** `lipo -info`: `x86_64 arm64`. | `build-uni/` **[S8]** |
| Any Darwin-specific network problem like S2's `bind(255.255.255.255)`? | **Not at build time.** SDK v1 has a kqueue I/O backend (`sdk_core/src/base/multiple_io/multiple_io_kqueue.cpp`) and a shared unix `network_util.cpp` with no `__APPLE__` branches. Whether `Init()`/`Start()` bind cleanly on Darwin is a **runtime** question — §5. | source tree **[S8]** |
| Dependencies beyond a C++11 compiler? | **None.** FastCRC, spdlog and cmdline are vendored under `sdk_core/include/third_party`; the only system dependency is pthreads. No APR, no Boost, nothing from vcpkg. | `sdk_core/CMakeLists.txt` **[S8]** |

## 2. The two patches

Both are minimal and both mirror what `engine/third_party/patches/` already
does for SDK2:

1. **`cmake_minimum_required` 3.0 → 3.5** in the root and `sdk_core`
   CMakeLists (the spike measured 3.5 as sufficient; the shipped patch
   `sdk1-0001` uses 3.10, the same floor SDK2's patch chose, because CMake 4
   also deprecation-warns below 3.10). The alternative — `CMAKE_POLICY_VERSION_MINIMUM` on the
   command line — works for a standalone build but does not propagate into
   an `add_subdirectory()` from the engine, so the patch is the correct fix.
2. **Drop `-Werror` in `sdk_core/CMakeLists.txt`** (keep `-Wall`). The
   warnings are in third-party headers the SDK vendors; promoting them to
   errors was never a statement about Livox's code, and the engine's own
   `ENGINE_WARNINGS_AS_ERRORS` leg applies only to `scanengine` sources.

The static library target is named `${PROJECT_NAME}_static` inside
`sdk_core/CMakeLists.txt`. When `sdk_core` is added as a subdirectory of the
engine project, `${PROJECT_NAME}` is *the engine's* name — Phase 2 either
patches the target name to a fixed `livox_sdk_static` or wraps the
subdirectory; `engine/third_party/fetch_sdk1.sh` and
`patches/sdk1-*.patch` are the authority on which was chosen.

## 3. What the SDK gives the driver

From `sdk_core/include/livox_sdk.h` / `livox_def.h` (SDK version 2.3.0):

- Lifecycle: `Init()`, `Start()`, `Uninit()` — **process-wide singletons**,
  exactly like SDK2, so one driver instance owns the SDK at a time and
  `close()` must guarantee no callback is in flight before `Uninit()`.
- Discovery: the **device broadcasts** to UDP 55000 about once a second
  while unconnected; `SetBroadcastCallback()` delivers
  `BroadcastDeviceInfo { broadcast_code[16]; dev_type; reserved; ip[16] }`.
  `dev_type == kDeviceTypeLidarMid70 (6)`. The host never has to send a
  broadcast — which is why S2's Darwin `bind(255.255.255.255)` failure
  cannot recur here in the same form.
- Connect: `AddLidarToConnect(broadcast_code, &handle)`,
  `SetDataCallback(handle, cb, user)`, `SetErrorMessageCallback(handle, cb)`;
  `SetDeviceStateUpdateCallback()` reports connect / disconnect / state
  change; on connect: `SetCartesianCoordinate()`, then
  `LidarStartSampling()`. `LidarSetRmcSyncTime()` exists for GPS-mode UTC.
- Data: the data callback receives `LivoxEthPacket*` — an 18-byte header
  (`version, slot, id, rsvd, err_code u32, timestamp_type, data_type,
  timestamp[8]`) followed by `data_num` points of `data_type`. **No length
  field, no sequence counter.** `err_code` carries `LidarErrorCode`, which
  includes `pps_status` and `time_sync_status` — the device's own report of
  whether its clock is synchronised, in every datagram.
- A Mid-70 has **no IMU**; `kImu` datagrams are never sent.

## 4. What the engine build does with this

`engine/third_party/fetch_sdk1.sh` fetches the pinned tarball and applies the
patches; `ENGINE_WITH_LIVOX_SDK1` (AUTO / ON / OFF) mirrors the SDK2 option;
`src/drivers/mid70/mid70_sdk1.cpp` is always compiled and its body is
conditional on `SCANENGINE_HAVE_LIVOX_SDK1`, so every CI leg (none of which
fetch an SDK) still builds and tests the driver through the raw-UDP and
inject backends. See `engine/docs/A17-mid70-driver.md`.

## 5. Owed to hardware (cannot be measured on this Mac today)

The Mid-70 and its USB IMU were unplugged from the Windows host during this
work, and the live-test Mac mini (kc_m4) does not yet have them.

1. `Init()` + `Start()` on Darwin: confirm the broadcast listener binds
   (0.0.0.0:55000) and that a real Mid-70 broadcast reaches the callback.
2. The handshake sequence on real firmware (`SetCartesianCoordinate` →
   `LidarStartSampling`) and which `data_type` current firmware sends after
   it (the ROS driver observed **2, kExtendCartesian, 96 points/datagram**).
3. `timestamp_type` on a free-running device (expected 4, PPS-only, with the
   stamp being nanoseconds since power-on — measured 25,566 s and 65,069 s
   of uptime on two days in the ROS work).
4. Fixtures: `tests/integration/data/mid70_broadcast.bin` (≥ 2 raw
   broadcast frames), `captures/mid70_real_30s.livoxdump` (raw datagrams
   with arrival stamps), `tests/integration/data/juxi_imu_60s.bin` (raw
   UART bytes spanning at least one of the module's ~35 s blackouts).
   `tools/remote-capture/capture_mid70.py` and `capture_serial_imu.py` are
   written for this; run them on whichever host the rig is plugged into.
5. Power-cycle behaviour: does SDK v1 re-handshake a unit it has seen
   before (the S2 finding for SDK2 was that it never does)? The driver keeps
   the forced-re-init path regardless.
