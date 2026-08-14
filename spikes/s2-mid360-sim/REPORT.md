# S2-sim — Livox Mid-360 software path de-risked without hardware

**Spike:** S2 (spec §4 Phase 0), executed in *simulator* mode because the physical
Mid-360 is at another location.
**Host:** Apple M4, macOS 26.5.1 (build 25F80), arm64. Apple clang 21.0.0, CMake 4.4.2.
**Date:** 2026-08-15.
**Covered:** everything in the Mid-360 path that is *software* — SDK2 buildability on
macOS, the wire protocol, discovery/handshake/heartbeat, sustained 200 k pts/s point +
200 Hz IMU streaming, loss accounting, memory/CPU stability.
**Not covered:** anything that depends on the physical device or a real NIC. Listed
exhaustively in §8.

---

## 1. Headline results

| S2 exit criterion (spec §4) | Result |
| --- | --- |
| 200 k pts/s sustained 10 min | ✅ **200,000 pts/s mean over 600.0 s** (120,008,736 points) |
| No packet-loss growth | ✅ **0 lost, 0 duplicated** over 1,250,091 packets |
| IMU @ 200 Hz | ✅ **200.00 Hz** (120,009 packets) |
| *(added)* memory stability | ✅ client RSS **2.3 → 2.2 MB**, simulator 1.49 → 1.54 MB. No growth. |
| *(added)* CPU headroom | ✅ client **2.4 %** of one core, simulator 7.7 % |
| *(added)* SDK2 builds/runs on macOS arm64 | ⚠️ **only with 3 patches** — unpatched SDK2 **cannot start at all** on macOS. See §3. |

Governing caveat: this is loopback against a simulator written from the protocol spec.
It proves the *software* path. It cannot prove the device. §8 is the honest list of what
is still hardware-only.

---

## 2. Protocol sources and cross-checks

Three independent sources; a field was only trusted when at least two agreed.

**[S] SDK2 source** — `third_party/Livox-SDK2` (master @ 2026-08-15, SDK version 1.4.3
per `include/livox_lidar_def.h`). Ground truth for what the client actually *does*:
`comm/sdk_protocol.{h,cpp}` (control framing + CRC placement), `comm/define.h`
(`DetectionData`, command IDs, ports), `include/livox_lidar_def.h`
(`LivoxLidarEthernetPacket`, point/IMU structs, param keys), `device_manager.cpp`
(socket binding, source-port dispatch, discovery loop),
`command_handler/{general,mid360}_command_handler.cpp` (the handshake state machine).

**[D] Official Livox documentation** — "Livox LiDAR Communication Protocol — Mid360",
<https://livox-wiki-en.readthedocs.io/en/latest/tutorials/new_product/mid360/livox_eth_protocol_mid360.html>
— normative byte tables for the control frame, point-cloud header, Cartesian type 1/2,
IMU, discovery ACK, port table and `time_type` semantics.

**[R] A real Mid-360 recording** — Livox's own `Indoor_sampledata.lvx2` (212 MB,
downloaded and parsed byte-by-byte; see `DATASETS.md`). The only source reflecting actual
firmware behaviour.

### Cross-check outcome

| Field | [S] | [D] | [R] | Verdict |
| --- | --- | --- | --- | --- |
| Control header 24 B; crc16 over bytes [0,18); crc32 over payload | ✓ | ✓ | n/a | agreed |
| Data header 36 B; `crc32`@24, `timestamp`@28 | ✓ | ✓ | ✓ | agreed |
| Cartesian type 1 = 14 B (`int32 x,y,z` mm + refl + tag) | ✓ | ✓ | ✓ | agreed |
| 96 points per UDP packet, packet = 1380 B | — | ✓ | ✓ **exactly 96** | agreed |
| 200,000 pts/s | — | — | ✓ **inter-packet Δt = 480.000 µs exactly** | agreed |
| IMU = 6×float, gyro rad/s, acc **g** | ✓ | ✓ | n/a | agreed |
| Ports 56000/56100/56200/56300/56400/56500 | ✓ | ✓ | n/a | agreed |
| **`udp_cnt` resets at frame start; `frame_cnt` increments** | — | ✓ | ✗ **contradicted** | **[D] is wrong for shipping firmware** |

`tests/wire_selftest.cpp` (run by `ctest`) machine-checks the [S]↔spike half: every
struct size and field offset, a simulator-built frame fed through the SDK's own
`SdkProtocol::CheckPreamble` / `ParsePacket`, and a negative test where one flipped
payload bit must be rejected. 28 assertions, all pass.

### Finding: `udp_cnt` is free-running and `frame_cnt` is always 0

Measured over 4,224 consecutive packages of `Indoor_sampledata.lvx2`:

```
udp_cnt   : 17473, 17474, 17475, ... 21696   monotonically increasing, 0 resets
frame_cnt : 0 for every package (1 distinct value across the whole sample)
```

The published table says `udp_cnt` "resets at frame start" and `frame_cnt` is
"incremented per scan". The shipping Mid-360 does neither.

**This matters for A3.** A loss detector written from the documentation resets its
expectation at every frame boundary — creating a blind spot exactly where bursty loss
appears — and on a real device it would never see a reset, so it would mis-handle the
counter wrap instead. The correct rule, which this spike's client implements and which
works against *both* models:

```
gap = (uint16)(udp_cnt - prev_udp_cnt)
gap == 0        -> duplicate / stalled sender
1 < gap < 1024  -> gap - 1 packets lost
gap >= 1024     -> counter reset (documented model only); not attributable
```

The simulator defaults to `--frame-model real` and reproduces the documented behaviour
with `--frame-model doc`, so A3's detector can be regression-tested against both. Both
were soaked for 10 minutes; see §6.

---

## 3. SDK2 on macOS arm64 — three patches, one a hard blocker

`patches/` holds three unified diffs; `scripts/fetch_sdk2.sh` fetches the pinned tarball
and applies them. Verified: a clean fetch + patch reproduces the working tree
byte-for-byte (`diff -rq` clean).

### `0001-cmake-minimum-required-3.10.patch` — build blocker
All 11 `CMakeLists.txt` use `cmake_minimum_required(VERSION 3.0)`. CMake ≥ 4.0 removed
compatibility below 3.5, so the SDK does not **configure** with current CMake (4.4.2).

### `0002-no-werror-on-clang.patch` — build blocker
`sdk_core/CMakeLists.txt` compiles with `-Werror` under (Apple)Clang. Apple clang 21
raises 11 diagnostics, all in *vendored* third-party headers plus `comm/define.h`:
`-Wunknown-warning-option` (rapidjson's malformed pragma strings, e.g. `-Wc++ 98 - compat`),
`-Wdeprecated-literal-operator` (bundled fmt), `-Wnon-c-typedef-for-linkage`
(`define.h:141`), `-Wunused-private-field` (FastCRC). None are real defects — `-Werror`
simply predates the compiler. Warnings stay enabled, just non-fatal.

### `0003-darwin-no-broadcast-bind.patch` — **runtime blocker: SDK2 does not start on macOS**

The significant finding. `DeviceManager::CreateDetectionChannel()` and
`CreateCommandChannel()` create a UDP socket bound to the literal address
`255.255.255.255`. Linux permits this; **Darwin returns `EADDRNOTAVAIL`**, so
`util::CreateSocket()` returns −1 and `DeviceManager::Init()` fails outright. Measured
on the unpatched build:

```
bind failed
[error] Create detection broadcast socket failed.  [device_manager.cpp] [CreateDetectionChannel] [266]
[error] Create detection channel failed.           [device_manager.cpp] [CreateChannel] [242]
[error] Create channel failed.                     [device_manager.cpp] [Init] [169]
Livox Init Failed
```

So spec §2.2's claim that Livox SDK2 "supports Windows/Linux/macOS hosts" is **not true
of the stock SDK** — macOS needs a source patch. Both call sites are already
`#ifdef WIN32`-guarded, so the minimal fix is to treat Darwin like Win32.

**Consequences for A3 — real ones:**
1. The engine must **vendor a patched SDK2**. Add these patches to A1's dependency
   pinning; the same `-Werror` issue will bite S7 on clang-cl.
2. On macOS the broadcast **send** also fails (`sendto` to 255.255.255.255 from a
   host-IP-bound socket logs "Detection lidars failed" once per second — non-fatal).
   Combined with (1), **zero-configuration discovery of an unknown lidar IP does not
   work on macOS**. A3 must require an explicit lidar IP in the config there. Spec §3.1's
   connect wizard already does manual IP entry, so this costs nothing — but it becomes a
   documented requirement rather than an accident.
3. Worth an upstream issue/PR; carrying the patch indefinitely is a maintenance tax.

### Result
`liblivox_lidar_sdk_static.a`, `liblivox_lidar_sdk_shared.dylib` and all 7 upstream
samples build clean on macOS arm64 with the three patches. Nothing else was needed — no
toolchain hacks, no dependency substitution. The `kqueue` I/O backend the SDK already
ships is what gets selected on Darwin.

---

## 4. The loopback problem, solved without root

**(a) The SDK drops any packet whose source IP equals the host IP.**
`device_manager.cpp:472`:
```cpp
if (lidar_ip == detection_host_ip_) {   // string compare against the configured host_ip
  return;
}
```
On loopback the simulator and client necessarily share `127.0.0.1`, so everything would
be discarded. The textbook fix is a second loopback address
(`ifconfig lo0 alias 127.0.0.2`) — but that needs `sudo`, unavailable non-interactively
here, and it would make the spike unreproducible in CI.

Solution: that comparison is a **string** compare against the literal from the config
file, while every *bind* goes through `inet_addr()`. So the config uses a non-canonical
but numerically identical spelling:

```json
"host_ip": "127.000.000.001",     "lidar_ip": ["127.0.0.1"]
```
`inet_addr("127.000.000.001") == inet_addr("127.0.0.1") == 0x0100007F`, so sockets bind
correctly and `custom_lidars_cfg_map_` keys match, while
`inet_ntoa(source) == "127.0.0.1" != "127.000.000.001"` so the self-IP filter never
fires. `BuildRequest::IpToU8()` splits on `.` and uses `std::stoi`, so the zero-padded
4-part form still yields `{127,0,0,1}` in the host-IP config sent to the device. No
patch, no root, CI-friendly. It is a deliberate exploit of a loose comparison and must
**not** leak into A3's production config.

**(b) Port 56000 is wanted by both sides.** The SDK binds `<host_ip>:56000` for
discovery; the simulated lidar must *send* discovery ACKs from source port 56000 (the SDK
dispatches discovery purely on source port). Resolved without a patch: the simulator
binds `0.0.0.0:56000` with `SO_REUSEADDR` while the SDK binds `127.0.0.1:56000`. Verified
that both binds succeed in either order and that BSD delivers unicast to the more
specific bind, so the simulator steals nothing from the SDK.

**(c) No broadcast on loopback.** macOS does not carry `255.255.255.255` to `lo0`, so the
simulator never sees the SDK's 1 Hz search request. It instead **announces** unprompted at
the same 1 Hz cadence to the host's control port. A real device answers the search; the
simulator volunteers. Invisible to everything downstream of discovery.

---

## 5. What the simulator implements

`sim/mid360_sim.cpp` (+ `sim/livox_wire.h`, `sim/scene.h`). It deliberately does **not**
link the SDK — it re-implements the wire format from [S]+[D] so a mistake in the SDK's
view of the protocol cannot be masked by sharing its code. The only borrowed component is
the SDK's `FastCRC`, so CRC bytes are bit-identical to what the SDK verifies.

### Faithfully implemented

| Area | Detail |
| --- | --- |
| Control framing | 24-B header, `sof=0xAA`, `version=0`, CRC-16/CCITT-FALSE over bytes [0,18), CRC-32 over payload. Round-trips through the SDK's own parser; a 1-bit payload flip is rejected. |
| Discovery | cmd `0x0000` ACK carrying `DetectionData{ret_code, dev_type=9, sn[16], lidar_ip[4], cmd_port}`, from source port 56000 at 1 Hz; also answers a real search request if one arrives. |
| Handshake | cmd `0x0101` get-internal-info backed by a real key→value table (fw type, SN, product, versions, MAC, work state, core temp, host-IP configs, HMS, …); cmd `0x0100` work-mode/host-IP configuration answered with `LivoxLidarAsyncControlResponse{ret_code=0, error_key=0}`. This is exactly the sequence that drives the SDK from "detected" to `LidarInfoChangeCallback`. |
| Host-directed streaming | Point/IMU/state destinations come from the `kKeyLidarPointDataHostIpCfg` / `kKeyLidarImuHostIpCfg` / `kKeyStateInfoHostIpCfg` KVs in the host's `0x0100` request — the device is *told* where to stream, as on real hardware. Not hardcoded. |
| Runtime control | `kKeyWorkMode` (0x01 starts sampling, otherwise stops), `kKeyPclDataType`, `kKeyImuDataEn` are honoured and reflected in queries and in the state push. |
| Heartbeat | cmd `0x0102` state push at 1 Hz from lidar port 56200 as a KV list. Verified end-to-end: the SDK parses it and hands the client JSON with the right SN, product string, work state and core temperature. |
| Point packets | 1380 B = 36-B header + 96 × 14-B Cartesian-32. `data_type=1`, `time_type=0`, `dot_num=96`, `length=1380`, `time_interval=4750` (95 × 5 µs in 0.1 µs units), `crc32` over timestamp+payload, `timestamp` in ns. |
| Point cadence | Absolute schedule, not sleep accumulation: packet *k* due at `k·96/rate`. Measured drift over 600 s: **0.000 s** at 1 ms reporting resolution. Matches the real device's exact 480.000 µs spacing. |
| IMU packets | 60 B = 36-B header + 24-B `{gyro xyz rad/s, acc xyz g}`, `dot_num=1`, `data_type=0`, 200 Hz on the same absolute schedule. |
| Counter model | `--frame-model real` (default): free-running `udp_cnt`, `frame_cnt=0`, as measured on real hardware. `--frame-model doc`: the documented per-frame reset. |
| Scene | 12 × 8 × 3 m room shell + 2 pillars + table slab + cabinet, per-point ray cast (slab method), 2 cm 1σ range noise, reflectivity from albedo × incidence × range falloff. |
| Motion | Analytic C² trajectory: Lissajous translation (±3.6 / ±2.4 / ±0.1 m), one yaw revolution per 4 min, small roll/pitch wobble. |
| IMU consistency | Gyro = exact body-frame angular rate of that trajectory (ZYX Euler-rate → body-rate transform); accelerometer = `Rᵀ(a_world − g)/9.80665` in g. **Independently verified by the client: mean \|acc\| = 1.0000 g across all 120,009 IMU packets** — the IMU and the point cloud describe one consistent rigid-body motion, which is what A6's ESKF needs. |
| Fault injection | `--loss PCT` (true drop: counters advance, bytes never leave the process), `--jitter MS` (uniform per-burst delay), plus `--rate`, `--imu-rate`, `--noise`. |

### Deliberately simplified, and why it is acceptable

| Simplification | Consequence |
| --- | --- |
| **Scan pattern** is a two-incommensurate-frequency az/el sweep over the documented 360° × [−7°, +52°] FOV, not Livox's Risley-prism pattern (constants unpublished). | Point *density distribution* is wrong; point *rate, ordering and packetisation* are right. Fine for throughput/plumbing (A3); **not** a substitute for real data when tuning A6's voxel map or S3's LOD. |
| **One pose per packet** (480 µs of motion collapsed to one origin) rather than per-point. | 480 µs at walking speed is < 1 mm — below the sensor's own noise. Irrelevant for A3; A6 must still implement per-point de-skew and test it on real data. |
| **Almost all rays return** — 2,038 no-returns in 120 M points (0.0017 %), against **35.9 %** zero points in the real indoor sample, which also shows a non-trivial `tag` distribution (`{0: 29839, 1: 187, 2: 54, 4: 105, 8: 14, 16: 129, 32: 6, …}`). Simulator always emits `tag=0`. | A3's zero-point and tag filtering is **not** exercised here. *Closable in software now* by replaying `Indoor_sampledata.lvx2` — recommended, and worth folding into E2. |
| **Reflectivity** is a smooth analytic function; real data is a broad multi-modal histogram. | Intensity-based colouring/filtering unvalidated. Cosmetic for S2. |
| **No log stream (56500), no debug point cloud (60301), no firmware upgrade path.** | The SDK only opens the log socket when logging is enabled; not on A3's path. |
| **Discovery announced, not solicited** (§4c). | Invisible downstream. |
| **No device faults**: no HMS codes, no thermal throttle, no link drop/reconnect. | Reconnect and health handling — an explicit A3 deliverable — is **untested**. This is the largest gap that *could* have been closed in software and was not; natural follow-up is to kill/restart the simulator mid-soak. |

---

## 6. Soak results

`scripts/run_soak.sh` runs `sdk_client_demo` — the real SDK2 client, driven exactly as A3
will drive it (config JSON → `LivoxLidarSdkInit` → info-change callback →
`SetLivoxLidarPclDataType` + `EnableLivoxLidarImuData` + `SetLivoxLidarWorkMode(Normal)` →
point and IMU callbacks) — against `mid360_sim` on loopback, both processes on the same
M4. Raw logs, per-minute CSVs and independent `ps` sampling are in `results/`.

### Run A — 10 min, clean link, real-device counter model *(the exit test)*

`results/soak-realmodel-20260815-012836-*`

| Metric | Measured | Target |
| --- | --- | --- |
| Duration (measurement window) | 600.0 s | 600 s |
| Point packets | 1,250,091 | — |
| Points | 120,008,736 | — |
| **Mean point rate** | **200,000 pts/s** | 200,000 |
| Mean packet rate | 2,083.3 pkt/s | 2,083.3 |
| **IMU rate** | **200.00 Hz** (120,009 packets) | 200 |
| IMU mean \|acc\| | 1.0000 g | 1.000 |
| **Packets lost** (from `udp_cnt`) | **0 (0.0000 %)** | no growth |
| Duplicate / stalled `udp_cnt` | 0 | 0 |
| Timestamp regressions | 0 | 0 |
| Device-clock vs host-clock skew over 600.044 s | **−0.000 s** | — |
| Packets with `dot_num ≠ 96` | 0 | 0 |
| Packets with wrong `length` | 0 | 0 |
| No-return points | 2,038 (0.0017 %) | — |
| `frame_cnt` transitions observed | 0 | 0 (confirms the real model is in effect) |
| **Client RSS** first / last / max | **2.3 / 2.2 / 2.2 MB** | no growth |
| Client CPU | 14.4 s = **2.4 %** of one core | — |
| Simulator RSS | 1.3 MB | — |
| Simulator CPU | 46.4 s = 7.7 % of one core | — |
| Simulator packets sent / dropped | 1,251,372 / 0 | — |

Per-minute client samples (`results/soak-realmodel-*-demo.csv`) — flat throughout, no
drift, no sawtooth, no loss onset:

```
t_s     pts_per_s  pkt_per_s  imu_hz  lost_total  rss_bytes  cpu_pct
 60.2   200001     2083.3     200.0   0           2228224    2.27
120.4   200000     2083.3     200.0   0           2310144    2.34
180.4   200000     2083.3     200.0   0           2310144    2.46
240.6   200000     2083.3     200.0   0           2310144    2.27
300.7   200000     2083.3     200.0   0           2310144    2.80
360.9   199999     2083.3     200.0   0           2310144    2.53
421.0   200000     2083.3     200.0   0           2310144    2.48
481.1   200001     2083.3     200.0   0           2310144    2.13
541.2   200000     2083.3     200.0   0           2310144    2.33
```

RSS is byte-identical (2,310,144) for the last eight samples. Independently sampled with
`ps` every 15 s (41 samples, `results/soak-realmodel-*-ps.log`) rather than trusting only
self-reporting: client mean CPU 2.3 %, max RSS 2,496 KB; simulator mean CPU 7.8 %,
RSS 1,488 → 1,536 KB. **No leak in either process.**

*(The simulator's 1,251,372 sent vs the client's 1,250,091 received differ by 1,281
packets ≈ 0.6 s of streaming — the simulator keeps sampling after the client's
measurement window closes and before it is signalled. It is a window-boundary effect, not
loss: the in-window `udp_cnt` sequence has zero gaps.)*

### Run B — documented counter model (`--frame-model doc`), 10 min

`results/soak-docmodel-20260815-011744-*`

| Metric | Measured |
| --- | --- |
| Duration | 600.1 s |
| Points / packets | 120,015,264 / 1,250,159 |
| Mean point rate | **200,000 pts/s** |
| IMU | 120,015 packets, **200.00 Hz**, \|acc\| 1.0000 g |
| Packets lost | **0 (0.0000 %)** |
| `frame_cnt` transitions | 6,000 (= 600 s × 10 Hz ✓, confirms the doc model) |
| Skew / regressions | −0.000 s / 0 |
| Client RSS first / last / max | 2.3 / 2.0 / 2.6 MB |
| Client CPU / simulator CPU | 2.1 % / 6.8 % |

Identical throughput and stability under both counter models — which is the property A3
needs, since the shipping firmware and the published documentation disagree.

### Run C — fault injection: 2 % loss + 2 ms jitter, 60 s, real model

`results/fault-loss2-jitter2-*`

| Metric | Measured |
| --- | --- |
| Packets generated / dropped at the sender | 125,728 / 2,465 = **1.961 % injected** |
| **Loss detected by the client from `udp_cnt`** | **2,452 = 1.9612 %** |
| Detection error | **0.0002 pp** (13 packets, at the window boundaries) |
| Point rate under loss | 196,071 pts/s (= 200,000 × 0.980 ✓) |
| IMU rate under loss | 196.02 Hz (= 200 × 0.980 ✓, 240 IMU packets dropped) |
| Timestamp regressions under jitter | 0 |
| Skew under jitter | 0.002 s |
| Duplicates | 0 |

Loss accounting is essentially exact under the free-running counter. The same test under
the **documented** model detected 1.917 % against 2.0 % injected — the ~0.08 pp shortfall
is the frame-boundary blind spot described in §2, quantified. That is the concrete cost
of getting the counter model wrong.

*Note on the jitter knob:* the first implementation slept per packet, which silently
turned it into a rate limiter (2 ms jitter → 144 k pts/s instead of 200 k). It now delays
the whole due burst, which is what network jitter looks like at a receiver and leaves mean
throughput intact. Worth remembering when reading any jitter-based benchmark.

### Bug found and fixed in the simulator (recorded because the lesson transfers)

The first end-to-end attempt reached configuration then stalled: the SDK sent `0x0100`
twice and then timed out forever. The cause was mine, not the SDK's — the point and IMU
threads called `sleep_for(20 ms)` *while holding* the shared state mutex in their
not-yet-streaming branch, so between them they held it ~100 % of the time and starved the
command thread inside `ApplyConfigKvs`. The command socket stopped being drained and the
handshake died. Generic lesson for A3's driver threads: **never sleep under the lock the
control path needs.**

---

## 7. Reproducing

```sh
cd spikes/s2-mid360-sim
scripts/fetch_sdk2.sh                                # tarball fetch + 3 patches (no git, no sudo)
cmake -S . -B build/spike -DCMAKE_BUILD_TYPE=Release
cmake --build build/spike -j
ctest --test-dir build/spike                         # wire_selftest: 28 assertions vs the SDK's own parser
scripts/run_soak.sh 600                              # Run A, the exit test
scripts/run_soak.sh 600 --frame-model doc            # Run B
scripts/run_soak.sh 60 --loss 2 --jitter 2           # Run C
```
`mid360_sim --help` lists all knobs (`--rate`, `--imu-rate`, `--loss`, `--jitter`,
`--noise`, `--frame-model`, `--lidar-ip`, `--host-ip`, `--host-cmd-port`, `--sn`).

---

## 8. What remains hardware-only

None of the following can be closed by more simulation. These are the residual S2 risks
and they travel with the Mid-360 to the bench.

**Device and link**
1. **Real NIC behaviour.** Loopback has no MTU, no driver, no interrupt coalescing, no
   ring-buffer overflow. 200 k pts/s is ~23 Mbit/s of 1380-B datagrams (2,083 pps) on a
   100 Mbit link. Receive-buffer sizing (`SO_RCVBUF`), kernel drop counters (`netstat -s`)
   and USB-Ethernet adapter behaviour are all untested. **This is the single biggest
   untested item**, and it is precisely what spec §5 flags as High risk ("OEM
   USB-Ethernet variance"). Loopback proves the *consumer* keeps up; it says nothing
   about the *transport*.
2. **Real loss patterns.** Zero loss on loopback is unsurprising. Bursty loss correlated
   with CPU scheduling and adapter buffering is unknown. The loss *detector* is validated
   to 0.0002 pp; the loss *rate* is not.
3. **Link drop / reconnect / power-cycle.** Untested even in simulation (§5). Should be
   added to the simulator *before* hardware arrives, so bench time is spent on things only
   hardware can answer.
4. **The 100 Mbit constraint** and whether the adapter negotiates full duplex.

**Timing**
5. **Real device timestamps.** The simulator derives a perfect `time_type=0` clock from
   the host's `steady_clock`, so the measured skew is ~0 *by construction*. A real Mid-360
   has its own oscillator with real drift, so A4's time-sync module (drift-tracked median
   filter, spec §3.2) is **completely unexercised**. This also means S6's colorization
   sync-jitter budget (spec §3.5, ≤ 5 ms) is untouched by S2.
6. **PPS / PTP / GPS sync** (`time_type` 1 and 2) — not implemented, not tested.
7. **True per-point timing** within a packet under real firmware, and de-skew correctness.

**Sensor physics — what actually determines whether SLAM works**
8. **The real non-repetitive scan pattern.** The single most important thing S2-sim cannot
   provide. Coverage growth over integration time — and therefore A6/A7 map quality and
   S3's LOD budget — depends on it. *Partially closable in software now* by replaying
   `Indoor_sampledata.lvx2`; recommended before the bench.
9. **Real range noise, drop-outs and `tag` semantics** — 35.9 % no-return indoors in real
   data vs 0.0017 % here; tag bit meanings unvalidated against any filtering code.
10. **Real IMU**: bias, bias instability, scale-factor error, noise density, axis
    misalignment, and the true lidar↔IMU extrinsic. The simulator's IMU is truth + white
    noise; a real ESKF must survive bias walk.
11. **Reflectivity / intensity calibration.**
12. **Thermal behaviour** over long captures (core temperature is synthetic here) and its
    effect on range and rate.

**Per-device / per-firmware quirks**
13. **Actual firmware responses** to `0x0101` — real key sets and value encodings,
    including keys the simulator answers with a placeholder. A real device may return keys
    we mishandle.
14. **HMS / diagnostic codes** and `kKeyLidarDiagStatus` under real fault conditions — the
    input to A3's health panel, entirely fabricated here.
15. **Discovery of a device whose IP is unknown**, which on macOS is blocked by §3
    finding (2) regardless.
16. **Multi-unit SN / IP-conflict handling.** Note `Outdoor_sampledata.lvx2` is a genuine
    3-device capture and can pre-test some multi-device plumbing in software.

**Recommended before the Mid-360 reaches the bench** (all software, all doable now):
add link-drop/reconnect to the simulator; build an `.lvx2` replay source so A6 can be
developed against the *real* scan pattern; fold the real indoor sample into E2 as a golden
dataset for zero-point and tag handling.

---

## 9. Verdict

**GO for A3 on the software path.** SDK2 works on macOS arm64 once patched, the handshake
and both data streams are understood at byte level, and the real SDK2 client sustains the
full 200 k pts/s + 200 Hz IMU load for 10 minutes at 2.4 % of one core with flat memory
and zero packet loss. The API surface A3 needs is exercised end-to-end.

**Three things this spike changes about the plan:**
1. The engine must **vendor a patched SDK2** — stock SDK2 will not start on macOS. Add the
   three patches to A1's dependency pinning and open an upstream issue.
2. A3's packet-loss detection must be written against the **free-running `udp_cnt`**, not
   the documented per-frame reset. The published protocol table is wrong here, and Run C
   quantifies the cost of believing it.
3. macOS (and therefore the desktop app) must **require an explicit lidar IP** — spec §3.1's
   wizard already does this, but it is now a hard requirement rather than a convenience.

**S2 is not closed.** The exit criteria are met in simulation; §8 items 1, 5 and 8 — real
NIC transport, real device clock, real scan pattern — must be re-run on the physical
Mid-360 before M1. `scripts/run_soak.sh` is written so the identical soak can be pointed
at the real device by swapping the config JSON, which is how S2 should be finally signed
off.
