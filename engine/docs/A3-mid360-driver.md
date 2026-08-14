# A3 — Livox Mid-360 driver

Status: implemented and verified in simulation. Not yet run against physical
hardware; §8 lists exactly what that still owes.

This document is the operational companion to
`include/scanengine/drivers/mid360/mid360_driver.h`. The *why* behind each
decision lives in the headers; this is how to build it, run it, read its
numbers, and what its numbers were on 2026-08-15.

Everything here descends from the S2 spike
(`spikes/s2-mid360-sim/REPORT.md` + `FOLLOWUP_NOTES.md`). Where a claim is
marked **[S2]** it was measured there, not assumed.

---

## 1. What A3 delivers

| Piece | File | Needs the SDK? |
| --- | --- | --- |
| Wire layout, loss model, point filter | `include/…/mid360/mid360_packets.h`, `src/…/mid360_packets.cpp` | no |
| Driver: lifecycle, batching, IMU, health, watchdog, reconnect | `include/…/mid360/mid360_driver.h`, `src/…/mid360_driver.cpp` | no |
| SDK2 backend (discovery, handshake, config push, callbacks) | `src/…/mid360_sdk2.cpp` | **yes** |
| Raw-UDP and inject backends | `src/…/mid360_raw_udp.cpp` | no |
| UDP transport (sockets, SO_RCVBUF, Android pre-bound fd) | `include/scanengine/transport/udp_source.h`, `src/transport/udp_source.cpp` | no |
| SDK2 vendoring | `third_party/fetch_sdk2.sh`, `third_party/patches/*.patch`, `third_party/.gitignore` | — |
| Tests (unit + simulator end-to-end) | `tests/test_mid360_driver.cpp` | unit: no · sim: yes |

Only `mid360_sdk2.cpp` includes a Livox header. Nothing in the public API,
the C ABI, or any other module knows the SDK exists.

---

## 2. Building

```sh
engine/third_party/fetch_sdk2.sh          # pinned tarball + the 3 patches
cmake --preset macos-universal
cmake --build --preset macos-universal
ctest --preset macos-universal
```

`ENGINE_WITH_LIVOX_SDK2` is `AUTO`: present ⇒ the SDK2 backend is compiled
and `livox_lidar_sdk_static` is linked; absent ⇒ everything else still
builds and `start()` on the SDK backend fails with `kNotSupported` and a
message naming the fetch script. `ON` turns a missing checkout into a
configure error; `OFF` forces the SDK out.

**Why vendored-and-patched rather than a vcpkg port [S2 REPORT.md §3].**
Stock Livox-SDK2:

1. does not *configure* — all 11 `CMakeLists.txt` say
   `cmake_minimum_required(VERSION 3.0)`, and CMake ≥ 4.0 dropped
   compatibility below 3.5;
2. does not *compile* — `sdk_core` builds with `-Werror`, and Apple clang 21
   raises 11 diagnostics inside the SDK's own vendored rapidjson / fmt /
   FastCRC;
3. **does not run on macOS at all** — `DeviceManager::CreateDetectionChannel()`
   and `CreateCommandChannel()` `bind()` the literal address
   `255.255.255.255`. Linux allows it; Darwin returns `EADDRNOTAVAIL`, so
   `util::CreateSocket()` returns −1 and `LivoxLidarSdkInit()` fails outright.

The three diffs in `third_party/patches/` are the minimal fix for each. The
SDK tree is gitignored: the committed artefacts are the script (pinned
tarball URL) and the patches, which makes "what did we change and why" four
screens of review instead of a 9 MB import. A fresh fetch+patch reproduces
the S2 spike's tree byte-for-byte (`diff -rq` clean, re-verified for this
task).

The spec's claim that SDK2 "supports Windows/Linux/macOS hosts" is not true
of the stock SDK. Carrying the patch is a maintenance tax and the Darwin fix
is worth an upstream PR.

---

## 3. Configuration

```cpp
Mid360Config cfg;
cfg.udp.lidar_ip = "192.168.1.100";   // REQUIRED
cfg.udp.host_ip  = "192.168.1.5";     // REQUIRED, must be a local address
DeviceConfig dc; dc.kind = DeviceKind::kMid360; dc.mid360 = cfg;
engine.add_device(dc);
```

### Explicit IP is mandatory, everywhere

`start()` refuses to run without both `lidar_ip` and `host_ip`. On macOS
this is forced: patch 0003 removes the broadcast bind, and the broadcast
*send* fails there anyway, so zero-configuration discovery of an unknown
lidar cannot work **[S2 REPORT.md §3 finding 2]**. It is the default on
every platform because the connect wizard (Tech Spec §3.1) asks for the IP
regardless, and because "which of the two units on this switch did I just
connect to" is not a question we want the network answering for us.

`host_ip` is equally required and equally non-negotiable: a Mid-360 does not
discover its host, it is *told* where to stream, via the
`kKeyLidarPointDataHostIpCfg` / `kKeyLidarImuHostIpCfg` KVs in the SDK's
`0x0100` configuration push. Get it wrong and the device streams into the
void, silently.

### Backends

| `Mid360Backend` | Owns sockets | Can bring a device up | Used for |
| --- | --- | --- | --- |
| `kSdk2` *(default)* | SDK2 | yes | live capture |
| `kRawUdp` | `UdpSource` ×2 | no | an already-configured device; injection harnesses |
| `kInject` | nothing | no | replay through `push_bytes()`; deterministic tests |

`kSdk2` is a **process-wide singleton** — `LivoxLidarSdkInit/Uninit` and the
callback registrations are global. A second `kSdk2` driver gets `kBusy` with
a message saying so. Multi-unit capture belongs in one SDK instance with
several lidar IPs; that is not implemented, and S2 lists multi-unit /
IP-conflict behaviour as hardware-only.

### Ports

Device 56100/56200/56300/56400/56500, host 56101/56201/56301/56401/56501 —
the +1 convention from Livox's own samples, which also keeps the two sides
distinguishable in a packet capture. All ten are configurable in `UdpConfig`.

### The SDK config file

`LivoxLidarSdkInit()` takes a JSON *path*, so the driver writes one from
`UdpConfig` into the temp directory and deletes it on stop. Set
`sdk_config_path` to keep it somewhere inspectable, or point it at a
hand-written file with `generate_sdk_config = false`.

---

## 4. The point path

```
SDK receive thread
  → parse_packet()            36-byte header, length/dot_num validation
  → LossTracker::observe()    free-running udp_cnt
  → point_passes()            no-return / tag / range / reflectivity
  → decimation                deterministic, every Nth survivor
  → mm → metres, reflectivity → greyscale
  → batch (8192 points)
  → PageStore::append(StreamId::kLidarMid360)   ← no driver lock held
  → Engine's page subscriber → EventType::kPointsAvailable
```

Points land in the device's own frame, in metres. The driver does **no**
geometry: A8 applies the trajectory, A6 the lidar↔IMU extrinsic.

### Loss detection: the free-running `udp_cnt` model

```
gap = (uint16)(udp_cnt − prev)
gap == 0          duplicate / stalled sender
gap == 1          in sequence
1 < gap < 1024    gap − 1 packets lost
gap >= 1024       counter reset — NOT attributable to loss
```

The published protocol table says `udp_cnt` resets at frame start and
`frame_cnt` increments per scan. Measured over 4,224 consecutive packages of
Livox's own recording: `udp_cnt` increases monotonically with **zero**
resets, and `frame_cnt` is 0 for every package **[S2 REPORT.md §2]**. A
detector written from the documentation resets its expectation at every
frame boundary — a blind spot exactly where bursty loss lives — and S2
quantified the cost: 1.917% detected against 2.0% injected under the
documented model, versus 1.9612% against 1.961% under the rule above.

The `uint16` arithmetic is load-bearing: it is what turns the counter's wrap
at 65535 → 0 into a gap of 1 instead of a 65,534-packet fiction.

### The filter, and why its defaults are what they are

Defaults come from real Livox recordings, because the simulator cannot
exercise this path at all — it emits 0.0017% no-returns and `tag == 0` for
every point, against 34.5–67.8% no-returns and a non-trivial tag histogram
in real data **[S2 FIXTURES.md]**.

| Knob | Default | Reason |
| --- | --- | --- |
| `drop_no_return` | `true` | A no-return is `x==y==z==0`. 35.24% of a real indoor slice, 67.76% of an outdoor one. Letting them through piles a third to two thirds of every capture onto the sensor origin. |
| `tag_reject_mask` | `0x0C` (spatial-noise bits) | The 0.45% the device itself flags as geometrically suspect — drag points off edges, retro-reflector bloom. |
| — intensity-noise bits `0x30` | **not** rejected | Usually genuine bright targets whose *intensity* is untrustworthy; the geometry is fine. Use `0x3C` to drop both. |
| `min_range_m` | `0.1` | The Mid-360's close-range blind zone: housing and self-hit spray. |
| `max_range_m`, `min_reflectivity` | off | Site-specific. |

Tag bit layout: `1:0` return number, `3:2` spatial-noise confidence, `5:4`
intensity-noise confidence (0 = normal, 1 = high confidence it *is* noise,
2 = moderate, 3 = low), `7:6` reserved.

### Decimation

`live_points_per_sec` (default 40,000, Tech Spec §3.3) thins the surviving
points **deterministically** — every Nth, never randomly — so replaying the
same bytes yields the same cloud, which is what E2's golden datasets need.
0 disables it, which is what post-processing and `.lvx2` replay want.
`Mid360Stats` reports the sensor rate and the store rate separately.

### IMU never enters the PageStore

IMU is not geometry and does not belong in a render buffer. Samples go to a
bounded ring (`drain_imu()`, the pull seam for A4/A5) plus an optional
`imu_sink` function pointer (the push seam), tagged `StreamId::kImu` by
convention. Gyro stays rad/s and accel stays **g**, exactly as the device
reports them — converting is A4/A6's decision, not the driver's.

**Deliberate gap:** there is no `EventType::kImuAvailable`. Adding one means
editing `core/event.h`, `core/event.cpp`, `capi/scanengine_c.h` and
`capi/scanengine_c.cpp` and bumping `SCAN_ABI_VERSION` — a core/ABI change
this task was scoped out of, and one that only matters once someone actually
consumes IMU across the C ABI. Per-sample events at 200 Hz would be the
wrong shape anyway. **Recommendation for A4/A5:** add `kImuAvailable` with a
`(count, first_t_ns, last_t_ns)` payload when the JNI side needs it; the
driver-side ring and sink are already in place and need no change.

---

## 5. Health, the watchdog, and reconnect

### Per-second stats

Every `health_period_ms` (default 1000) the supervisor recomputes the window
and publishes `EventType::kDeviceHealth`: points/s in, points/s to the
store, IMU Hz, window and lifetime loss %, and the link state. `stats()`
returns the full `Mid360Stats`; `health()` returns the generic
`DeviceHealth` the app's one health widget renders — with
`checksum_pass_rate` carrying `1 − loss` so a D6 and a Mid-360 mean the same
thing on the same dial, and `rotation_hz` carrying the IMU rate (a Mid-360
has no revolutions, and the IMU rate is the other thing that must not sag).

Sustained loss above `max_loss_pct` (default 1%, after `loss_min_packets`)
demotes to `kDegraded` with `kNetworkError`.

### The wall-clock data watchdog — and why `udp_cnt` cannot replace it

**A link drop is invisible to the packet counter.** S2 measured **0 counted
losses across a 15-second cable pull, three separate times**, because the
device's ranging ASIC keeps counting while the Ethernet link is down: the
first packet after resume looks like `prev + 1`, or lands past
`kResetThreshold` and is correctly classed as unattributable
**[S2 FOLLOWUP_NOTES.md §1 Scenario A/C]**. The SDK exposes no "link down"
callback either — checked against `livox_lidar_api.h`: there is only the
one-shot info-change, the periodic info-push, and per-command timeouts.

So the primary outage signal is wall-clock silence:

| Knob | Default | Meaning |
| --- | --- | --- |
| `connect_timeout_ms` | 10000 | Grace for the *first* packet (discovery + handshake + config push). Measured 1.45 s on loopback; without this the watchdog would trip on every startup. |
| `data_timeout_ms` | 1000 | No point packet for this long ⇒ `kSilent` + `DeviceState::kDegraded`. ~2,083 missed packets, far outside any jitter S2 saw (2 ms worst case). |
| `reinit_after_silence_ms` | 5000 | Still silent ⇒ forced full SDK teardown + re-init. |
| `reinit_backoff_initial_ms` / `_max_ms` | 1000 / 30000 | Capped exponential backoff between failed attempts. |
| `max_reinits` | 0 (unlimited) | Positive ⇒ give up into `kFault`. |

### Two failure modes, told apart

```
kUp ──silence > data_timeout──▶ kSilent ──silence > reinit_after──▶ kReinitializing
 ▲                                │                                        │
 └────── data returns ────────────┘  ← clean_resumes++ (CABLE)             │
 └───────────────────── data returns ──────────────────────────────────────┘
                                        ← forced_reinits already counted (POWER-CYCLE)
```

* **Cable class.** The wire comes back and the SDK resumes with zero
  application action — S2 confirmed this three times over. Counted as
  `clean_resumes`, no re-init.
* **Power-cycle class.** THE LOAD-BEARING FINDING. A device that lost its
  host configuration is **never** re-configured by the SDK. Once a handle is
  known, `GeneralCommandHandler::HandleDetectionData()`
  (`general_command_handler.cpp:337-397`) returns early forever and re-sends
  the host-IP config only if `is_update_cfg` is still false — i.e. only
  before the first successful configuration ever. S2 watched a
  power-cycled device keep sending discovery ACKs every second, keep being
  received by the SDK, and **never stream again** for the rest of the run.
  Waiting for a self-heal is not a strategy. The driver tears the SDK down
  (`LivoxLidarSdkUninit`) and builds it again (`Init` + callbacks + `Start`),
  counted as `forced_reinits`. §7 shows it recovering full rate.

`Mid360BackendImpl::close()` drains in-flight callbacks under a
`shared_mutex` before `Uninit()` joins the SDK's threads, so a re-init can
never race a callback into a half-destroyed driver.

---

## 6. Threads

Three, and A3 introduces the engine's first owned ones. **`DESIGN.md` §2's
thread table should gain these rows** (that file is not A3's to edit):

| Thread | Owner | Runs |
| --- | --- | --- |
| SDK2 receive threads | Livox SDK2 | point + IMU callbacks → `on_point_packet` / `on_imu_packet` → decode → `PageStore` / IMU ring. Reaches the rest of the engine only through `PageStore` and `EventBus`, per DESIGN §2. |
| Driver supervisor | `Mid360Driver` | 20 Hz `tick()`: watchdog, reconnect, per-second health. One per driver. Disable with `internal_supervisor_thread = false` and call `tick()` yourself (what the unit tests do). |
| `UdpSource` receive | `UdpSource` | raw-UDP backend only; one per bound port, blocking `recvfrom` with a 100 ms timeout so `stop()` is prompt. |

Lock order is `flush_m_` → `m_`, and the `PageStore::append` that publishes
`kPointsAvailable` runs with **no** driver lock held — subscribers execute
inline on the calling thread, and holding a driver lock across app code is
precisely how S2's own simulator deadlocked its handshake ("never sleep
under the lock the control path needs", REPORT.md §6).

---

## 7. Verification (2026-08-15)

Host: Apple M4, macOS 26.5.1 (Darwin 25.5.0), arm64. Apple clang 21.0.0,
CMake 4.4.2, Ninja, `RelWithDebInfo`, clean build directory.

### Unit tests — no SDK linked

```
$ ctest
    Start 1: scanengine_tests ......... Passed
    Start 2: scanengine_capi_smoke .... Passed
    Start 3: engine_cli_selftest ...... Passed
    Start 4: engine_cli_version ....... Passed
    Start 5: mid360_sim_e2e ........... Skipped   (label "sim", opt-in)

$ ./scanengine_tests
[doctest] test cases: 148 | 148 passed | 0 failed | 5 skipped
```

25 of those cases are A3's: wire layout and packet validation, CRC-32
coverage, the free-running loss model (including the 65535 wrap and the
≥1024 reset bucket), the filter replayed against the **real** indoor tag
histogram, mm→m conversion into the PageStore, decimation determinism, the
IMU ring/sink/overflow, the watchdog and forced-re-init state machine under
a scripted clock, and a real loopback `UdpSource`.

### End-to-end against the S2 simulator

```
$ SCANENGINE_SIM_SECONDS=60 ctest -L sim
1/1 Test #5: mid360_sim_e2e ....... Passed  204.11 sec
[doctest] test cases: 5 | 5 passed | 0 failed
```

The real, patched SDK2 driving the real driver against `mid360_sim` on
loopback. **Handshake to first packet: 1.45 s.**

**60-second soak, `--frame-model real`** — the exit test:

| Metric | Measured | Target |
| --- | --- | --- |
| Duration | 60.005 s | 60 |
| Point packets | 125,010 | — |
| Points received | 12,000,960 | — |
| **Mean point rate** | **199,999 pts/s** | 200,000 |
| Points into the PageStore | 2,397,115 (**39,949 pts/s**) | 40,000 (live budget) |
| **IMU** | 12,001 packets, **200.00 Hz** | 200 |
| IMU mean \|acc\| | **1.00011 g** | 1.000 |
| **Packets lost** (`udp_cnt`) | **0 (0.0000%)** | no growth |
| Duplicates / counter resets | 0 / **0** | 0 / 0 (confirms the real firmware model) |
| Bad packets | 0 | 0 |
| Watchdog trips / re-inits | 0 / 0 | 0 / 0 |
| Link / state | up / streaming | — |

**2% injected loss, 20 s** (`--loss 2`):

| Metric | Measured |
| --- | --- |
| Packets received | 40,900 |
| **Packets lost, from `udp_cnt`** | **775 = 1.8596%** (2% injected) |
| Duplicates | 0 |
| Point rate under loss | 196,277 pts/s (= 200,000 × 0.981 ✓) |
| Device state | `kDegraded` — sustained loss above `max_loss_pct` says so |

**Cable pull — 6 s outage at t = 10 s** (`--drop-link-after 10 --link-down-for 6`):

| Metric | Measured |
| --- | --- |
| Watchdog trips | **1** |
| Clean resumes | **1** |
| Forced re-inits | **0** — a live link needs no help |
| **Packets lost from `udp_cnt` across the whole outage** | **0** |
| Counter resets | 1 (the outage showed up *here*, unattributed) |
| Mean rate over the 25 s window | 151,607 pts/s (= 200k × 19/25 ✓) |
| Final link / state | up / streaming |

That "0" is the entire argument for the watchdog: the counter is blind to an
outage of any real length, and the wall clock is not.

**Power-cycle — identity reset at t = 10 s** (`--restart-identity`):

| Metric | Measured |
| --- | --- |
| Watchdog trips | 1 |
| **Forced re-inits** | **1** (0 failures) |
| Clean resumes | 0 — correctly *not* classed as cable-class |
| Points after recovery | **6,251 packets in 3 s = 2,083.7 pkt/s**, i.e. full rate |
| Final link / state | up / streaming |

Without the forced re-init this device never returns: S2 measured it dead
for the remaining 35 s of a 60 s run. `LivoxLidarSdkUninit()` →
`LivoxLidarSdkInit()` in the same process works and is now exercised twice
per `ctest -L sim` run (the preflight probe does one too).

**`.lvx2` replay — real Risley-prism scan pattern, 30 s**
(`lvx2_replay datasets/Indoor_sampledata.lvx2`):

| Metric | Measured |
| --- | --- |
| Mean point rate | **200,003 pts/s** |
| Packets lost / duplicates / bad | 0 / 0 / 0 |
| **No-returns dropped** | 2,058,329 = **34.30%** |
| **Tag-rejected** (spatial-noise) | 25,649 = **0.43%** |
| Points into the PageStore | 3,915,482 (130,497 pts/s, decimation off) |
| IMU packets | **0** — correct: Livox's indoor/outdoor samples contain no IMU |

34.30% no-return corroborates `FIXTURES.md`'s 35.24% (5 s slice) / 34.67%
(full file), and 0.43% tag-rejected matches the 0.45% predicted from the
measured histogram. This is the only run that exercises the filter against
real returns at all — and it is why the defaults were chosen from it.

### Running the sim tests yourself

```sh
cd spikes/s2-mid360-sim && scripts/fetch_sdk2.sh \
  && cmake -S . -B build/spike -DCMAKE_BUILD_TYPE=Release && cmake --build build/spike -j
cd engine && ctest -L sim                      # ~200 s
SCANENGINE_SIM_SECONDS=600 ctest -L sim        # the 10-minute soak
```

The `mid360sim/*` cases carry `doctest::skip()`, so `scanengine_tests` and
every other ctest entry never pay for them; the `sim`-labelled entry
re-invokes the binary with `--no-skip`.

That entry is registered **only when the simulator binary actually exists**
at configure time. The spike's build tree is gitignored, so every CI leg and
every fresh clone has nothing extra to run and is completely unaffected. On
a machine where you *have* built the spike, plain `ctest` includes the
200-second run — use `ctest -LE sim` for the fast path, or configure with
`-DENGINE_SIM_TESTS=OFF`. If the pieces are present at configure time but
unusable at run time, the cases print `SKIPPED: <why>` and CTest reports
**Skipped**, not a false pass.

The test is `RUN_SERIAL`: the simulator and the SDK bind the real Mid-360
ports on loopback, so no two runs may overlap — including against a second
build tree on the same host. (A concurrent run from another build directory
is exactly what a stray `bind failed` means.)

**Loopback caveat, do not copy into production.** The sim config spells the
host IP `127.000.000.001`. That is numerically identical to `127.0.0.1` for
`inet_addr()` but a different *string*, which is what slips past the SDK's
`if (lidar_ip == detection_host_ip_) return;` self-IP filter
(`device_manager.cpp:472`) — without it the SDK discards every packet,
because on loopback both sides share an address. It is a deliberate exploit
of a loose comparison, confined to the test.

---

## 8. What is still hardware-only

Unchanged from S2 REPORT.md §8, and none of it is closable by more
simulation:

1. **Real NIC behaviour** — the biggest one. Loopback has no MTU, no driver
   ring, no interrupt coalescing. 200k pts/s is ~23 Mbit/s of 1380-byte
   datagrams (2,083 pps) on a 100 Mbit link. `UdpConfig::recv_buffer_bytes`
   (4 MB, ~1.4 s of slack) and the granted size in `UdpSourceStats` exist so
   this is measurable on day one — but the SDK2 backend sets its own socket
   buffers, so if the bench shows kernel drops (`netstat -s`), the fix is a
   fourth patch to the SDK's `util::CreateSocket()`.
2. **Real loss patterns** — the detector is validated to 0.0002 pp; the
   *rate* on real hardware is unknown.
3. **Real device timestamps.** The simulator derives a perfect clock from
   the host's, so skew is ~0 by construction. A4's time-sync is unexercised
   by anything here.
4. **Real IMU** — bias, bias instability, scale factor, axis misalignment,
   and the true lidar↔IMU extrinsic. The replay files carry no IMU at all.
5. **Firmware key sets** for `0x0101`, and **HMS / `kKeyLidarDiagStatus`**
   under real faults — the input to the health panel, entirely synthetic
   here. The driver currently surfaces the SDK's info-push only as a
   heartbeat timestamp; parsing its JSON into `DeviceHealth` is the obvious
   next increment once a real device says something real.
6. **Multi-unit** SN / IP-conflict handling, and the single-SDK-instance
   restriction in §3.
7. **Thermal behaviour** over long captures.

The first bench session should re-run exactly the soak in §7 with
`Mid360Backend::kSdk2` pointed at the physical unit — the only change is the
two IP strings.
