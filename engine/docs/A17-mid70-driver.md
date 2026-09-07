# A17 — Livox Mid-70 driver

Status (2026-09-07): **code-complete and unit-tested; not yet run against a
Mid-70.** Everything below that describes the wire protocol was taken from the
Livox-SDK v1 headers (`livox_def.h`, `livox_sdk.h`) and the S8 spike
(`spikes/s8-mid70-sdk1/REPORT.md`); everything that describes the driver's
behaviour was measured in `tests/test_mid70_driver.cpp` against
protocol-derived datagrams. Nothing in this file has been seen on a real wire.
§8 is the list of what still has to be.

A17 is the Mid-360 driver (`docs/A3-mid360-driver.md`) re-done for a lidar
that speaks Livox SDK **v1**. Read A3 first; this file is written as the diff.

---

## 1. What A17 delivers

| Piece | Where |
| --- | --- |
| Wire layer: `LivoxEthPacket` header + all six point layouts, `parse_packet`, error-code and timestamp decode, tag masks, point filter, timestamp-gap loss tracker | `include/scanengine/drivers/mid70/mid70_packets.h`, `src/drivers/mid70/mid70_packets.cpp` |
| Driver: `Mid70Config`, `Mid70Stats`, `Mid70Driver : Driver`, three backends behind one `open()/close()` seam | `include/scanengine/drivers/mid70/mid70_driver.h`, `src/drivers/mid70/{mid70_driver.cpp,mid70_backend.h,mid70_raw_udp.cpp,mid70_sdk1.cpp}` |
| Vendored, patched SDK v1 | `third_party/fetch_sdk1.sh`, `third_party/patches/sdk1-000{1,2,3}.patch`, `ENGINE_WITH_LIVOX_SDK1` in `CMakeLists.txt` |
| Engine wiring: `DeviceKind::kMid70`, `StreamId::kLidarMid70`, `ChunkType::kMid70Points`, `DeviceConfig::mid70`, record-always raw shim, LIO point feed | `core/types.h`, `record/lscan.h`, `core/engine.{h,cpp}`, `timesync/offset_estimator.cpp` |
| C ABI 13 | `capi/scanengine_c.h` (`SCAN_DEVICE_MID70`, `SCAN_STREAM_LIDAR_MID70`, `SCAN_MID70_BACKEND_*`, the `mid70_*` block of `scan_device_config`) |
| Discovery | `discovery::DiscoverMid70`, `ParseMid70Beacon` (`docs/A16-discovery.md` covers the Mid-360 half; the Mid-70 half is in `discovery.h`'s own comment block) |
| Tools | `engine_cli --replay <capture.livoxdump> --sensor mid70`; `tools/remote-capture/capture_mid70.py`; `verify_capture.py --type mid70` |
| Tests | `tests/test_mid70_driver.cpp` — 30 cases, `mid70/*` |
| Field-test hook | `DiscoverOptions::raw_sink` — every 55000 datagram, pre-parse, so the desktop app writes the `LX70_CAP` broadcast fixture itself during Auto-detect (`desktop/FIELD_TEST_MID70.md`) |

A Mid-70 has **no IMU**. A Mid-70 session is two devices: this one and a
`DeviceKind::kImuSerial` module (`docs/A18-imu-serial.md`). Their clocks are
handled independently by A4 (§4), which is the whole of the time-sync story
and the reason none of the host-epoch-anchor machinery from the ROS
prototype exists here.

## 2. Building

The SDK is optional exactly the way SDK2 is:

```
cd engine/third_party && ./fetch_sdk1.sh        # pinned tarball + 3 patches -> Livox-SDK/
cmake --preset macos-universal                  # ENGINE_WITH_LIVOX_SDK1=AUTO: ON if the tree is there
```

`ENGINE_WITH_LIVOX_SDK1` is AUTO / ON / OFF. With the SDK absent the engine
still builds every file — `mid70_sdk1.cpp` compiles to a stub whose `open()`
returns `kNotSupported` and names the fetch script — and every test runs
through the raw-UDP and inject backends. That is the CI configuration: no CI
leg fetches an SDK.

The three patches (S8 §2, `fetch_sdk1.sh` header) are the whole delta from
upstream: `cmake_minimum_required` 3.0 → 3.10 (CMake 4 refuses below 3.5 and
deprecation-warns below 3.10; same floor SDK2's patch chose), drop
`-Werror` from `sdk_core` (its vendored spdlog/fmt trips two deprecation
warnings on current clang; none of the errors are in Livox's code), and pin
the library target name to `livox_sdk_static` (upstream names it
`${PROJECT_NAME}_static`, which becomes `scanengine_static` inside our tree).
Measured on this Mac: builds arm64 and `arm64;x86_64`; the only system
dependency is pthreads (S8 §1).

## 3. Configuration

`Mid70Config` is `Mid360Config` minus the IMU fields, plus:

| Field | Default | Why |
| --- | --- | --- |
| `backend` | `kSdk1` | The only backend that brings a device up: it runs discovery, the handshake, the heartbeat, sets Cartesian output and starts sampling. |
| `broadcast_code` | `""` = first Mid-70 heard | The 15-character code on the label (`3GGDJ5N00100101`). SDK v1 identifies devices by it, never by IP: `AddLidarToConnect()` takes the code. Set it when two lidars share a switch. |
| `udp.lidar_ip` | `""` | **Optional for `kSdk1`**, where A3 makes it mandatory. The S2 Darwin failure was SDK2 *sending* to `255.255.255.255`; SDK v1's discovery is the *device* broadcasting to UDP 55000 and the host listening, which binds `0.0.0.0` and works. `kRawUdp` still needs `udp.host_ip` and a bind port (`udp.bind_port`, else `udp.host_point_port`). |
| `filter.tag_reject_mask` | spatial-noise ∣ distortion | See §4. |
| `filter.min_range_m` | 0.10 | The Mid-70's blind zone is 0.05 m; 0.10 keeps the near-field returns the ROS work showed a 0.5 m gate throws away in a corridor. |
| `live_points_per_sec` | 40 000 | Of the sensor's 100 k (200 k dual). Deterministic every-Nth, same as A3. |
| `dual_return` | false | SDK1 only. Asks for `kDualExtendCartesian`; off is what the FAST-LIO work ran. |
| `reconnect.connect_timeout_ms` | 10 000 | A cold device answers a broadcast inside ~2 s; one self-heating below 0 °C can take minutes ([M] §4) — reason to keep trying, not to fault. |

C ABI: `scan_device_config` gained the `mid70_*` block at its tail, which is
why the ABI moved 12 → 13 (a size change, unlike ITEM 119's value-only
addition). `lidar_ip`/`host_ip` are reused; a `memset(0)` config means "SDK v1,
first Mid-70 heard, driver defaults".

## 4. The point path

### The datagram

`LivoxEthPacket` (SDK v1) is an **18-byte header with no length field and no
sequence counter**, followed by `N` points of one layout:

| `data_type` | Layout | Bytes/pt | Pts/datagram | Datagram |
| --- | --- | --- | --- | --- |
| 0 `kCartesian` | x y z mm (i32), reflectivity | 13 | 100 | 1318 |
| 1 `kSpherical` | depth mm (u32), theta, phi (u16 0.01°), reflectivity | 9 | 100 | 918 |
| 2 `kExtendCartesian` | + `tag` | 14 | 96 | 1362 |
| 3 `kExtendSpherical` | + `tag` | 10 | 96 | 978 |
| 4 `kDualExtendCartesian` | two returns per sample | 28 | 48 | 1362 |
| 6 `kImu` | never sent by a Mid-70 | 24 | 1 | 42 |

`parse_packet()` accepts a datagram only if `(len − 18) % bytes_per_point == 0`
and the count is ≤ the type's maximum; the ROS driver observed type 2 with 96
points on current firmware, so that is what the default filter assumes, but
the driver decodes all five point layouts (dual → two `Point`s). A `kImu`
datagram is counted in `unexpected_imu_packets`, delivered to the raw sink,
and otherwise ignored — it must not touch the gap tracker, whose interval
would be wrong for it.

Every point is `memcpy`'d out of the payload; nothing casts the buffer.

### Loss detection without a counter

A3 counts loss from `udp_cnt`. There is no such field here, so
`mid70::GapTracker` infers it from the **device timestamp**: the expected
inter-datagram interval is `returns_per_datagram / points_per_second`
(1.00 ms for 100 × 100 k; 0.96 ms for 96; 0.48 ms for dual, which is 96
*returns* over 200 k — using 48 samples would report a clean dual stream as
50 % lost), re-derived per datagram so a firmware that changes `data_type`
mid-stream changes the cadence with it. A gap of N intervals is N − 1 lost;
a stamp inside 1.5 intervals is in sequence; a backwards stamp is a
duplicate; a jump over 2 s is a *reset*, not loss (a re-handshake, or a
power cycle bringing the NoSync counter back to 0). Datagrams whose stamp
does not decode feed neither the tracker nor the estimator; they are only
counted. Same blind spot as A3: a full outage is the wall-clock watchdog's job
(§5).

### Time

`timestamp_type` says what the 8 stamp bytes are: `kTimestampNoSync` (0),
`kTimestampPtp` (1) and `kTimestampPps` (4) are u64 nanoseconds from the
device's own clock (since power-on when free-running — 25 566 s and 65 069 s of
uptime were seen on two days of ROS work); `kTimestampPpsGps` (3) is packed
UTC (year, month, day, hour, µs), which the driver turns into epoch ns with
`timegm`. Either way it is **the device's clock**, so `StreamId::kLidarMid70`
answers `stream_has_device_clock() == true` and gets A4's min-delay estimator;
the driver calls `add_pair(device, arrival)` on every decodable stamp and maps
points through it. A device with no decodable stamp is mapped on arrival
time. The serial IMU has no clock and is passthrough. That is the entire
two-clock story: two streams, two estimators, both landing on engine time
before the LIO sees either.

### Filter, decimation, no IMU

The Mid-70 `tag` byte is laid out as the Mid-360's: spatial noise (bits 0–1),
intensity noise (2–3), return type (4–5), distortion (6–7). The default mask
rejects spatial noise and distortion, keeps intensity noise, drops no-return
points (`|xyz| == 0`), and gates at 0.10 m. **This default is manual-derived,
not fixture-derived** — A3's mask was chosen from a real recording's tag
histogram and the Mid-70 has no such recording yet (§8).

Decimation is A3's: deterministic every-Nth survivor, byte-identical across
driver instances (tested). The Mid-70 never sends IMU data, so nothing here
touches `ImuIngest`; `Engine::add_device(kMid70)` installs only the raw shim.

### Record-always

Every datagram — valid or not — reaches the raw sink before parsing and is
recorded as one `ChunkType::kMid70Points` chunk (in `streams/lidar.bin`, next
to `kMid360Points`; `ReplaySource` filters on chunk type). Replay is
`Mid70Backend::kInject`, one datagram per `push_bytes()`.

## 5. Health, the watchdog, and reconnect

Per-second stats, the wall-clock data watchdog, the silent → reinit ladder
with backoff and `max_reinits`, and the "two failure modes told apart"
argument are A3's, verbatim, with `udp_cnt` wording replaced by the gap
model. One addition: a forced re-init resets the gap tracker, because a
power-cycled device comes back with its counter at 0 and a stale `prev`
would read as one enormous duplicate.

What is new is that **the device reports its own clock status in every
datagram**: `err_code` is a `LidarErrorCode` bitfield whose `pps_status` and
`time_sync_status` bits (plus temperature / voltage / motor / dirty / firmware
/ fan / self-heating / PTP flags) are decoded into `Mid70Stats::err`, reachable
through `Engine::mid70_stats(id)` (the `mid360_stats()` shape). The driver surfaces them; it never infers them. A
desktop status line that says "PPS: no, sync: no" while the lidar is
free-running is telling the truth, and the day a PPS+GPS bridge is wired to
M12 pins 11/12 the same line will say so without a code change.

`DeviceHealth`: `kind = kMid70`, `bytes_in` measured (datagram size varies
with `data_type`, and a malformed datagram still used the wire),
`rotation_hz = 0`, `checksum_pass_rate = 1 − loss_fraction` (there is no
per-datagram CRC in SDK v1 to fail).

## 6. Threads

As A3: the SDK's receive thread (or the raw-UDP source thread, or the
injecting caller) runs `on_point_packet()`; an optional supervisor thread
runs `tick()`; the SDK is a process singleton guarded by a `shared_mutex` so
`close()` cannot race a callback in flight. The Engine's raw shim takes
`record_m` and nothing else on that thread.

## 7. Verification (2026-09-07)

### Unit tests — no SDK linked

`tests/test_mid70_driver.cpp`, 30 cases, 344 assertions, all `mid70/*`:

* wire sizes, offsets and port constants; `parse_packet` accept/reject
  (short, header-only, non-integral payload, unknown type, over-count, IMU);
* NoSync/PTP/PPS raw-ns and a `timegm`-checked PPS+GPS epoch for 2026-03-14
  09:00 UTC, plus malformed-date rejection;
* every `err_code` bit position and cross-field leak; tag accessors;
* filter defaults, `from_spherical` closed form + round trip;
* `GapTracker`: in-sequence jitter, 1 lost, 3 lost, duplicate, backwards,
  both sides of the 2 s boundary, loss fraction, reset;
* driver through `kInject`: mm→m into the PageStore, all data types
  including both dual returns, no-returns dropped, exact 1-in-5 decimation
  byte-identical across two instances, unexpected IMU counted-not-decoded,
  bad packets counted while the raw sink still sees them, device self-report
  fields, `push_bytes` inject-only, health window rates/loss/degrade,
  watchdog, forced re-init, reconnect cap, raw-UDP guard rails vs. SDK1
  needing no addressing.

Full suite: `ctest -LE "sim|sim-rtk"` green with `ENGINE_WITH_LIVOX_SDK1=OFF`
(the CI configuration) and with the SDK vendored.

### Replay

`engine_cli --replay <capture.livoxdump> --sensor mid70` reads the
`LX70_CAP` container `capture_mid70.py` writes, skips the port-55000
broadcast records, pushes every data datagram with its recorded arrival
stamp through `kInject`, and prints datagrams/s, points/s (100 k single
return expected), loss, `err_code` flags and an arrival-gap census. **It has
not yet had a real container to read** (§8).

## 8. What is still hardware-only

The Mid-70 and its IMU module were unplugged from the Windows host during
this work and the live-test Mac mini did not yet have them. Owed, in order:

1. `Init()` + `Start()` on Darwin: confirm the SDK's listener binds
   `0.0.0.0:55000` and a real broadcast reaches the callback (S8 §5.1).
2. The handshake on real firmware (`SetCartesianCoordinate` →
   `LidarStartSampling`) and which `data_type` it then sends (ROS saw 2/96).
3. `timestamp_type` on a free-running unit (expected 4) and whether a real
   device's stamp jitter stays inside the tracker's 1.5-interval band.
4. Fixtures: `tests/integration/data/mid70_broadcast.bin` (≥ 2 raw broadcast
   frames — the discovery CRC parameters are verified against them, not
   assumed), `captures/mid70_real_30s.livoxdump` (for the inject-replay test
   and a real tag histogram to set the filter default from). Both come from
   `tools/remote-capture/capture_mid70.py`, verified with
   `verify_capture.py`.
5. Power-cycle behaviour: does SDK v1 re-handshake a unit it has seen before?
   (SDK2 never did — S2.) The forced-re-init path is kept regardless.
6. PPS + GPS through the M12 sync pins (the P4 bridge design) —
   `LidarSetRmcSyncTime` exists in the SDK and is not wired; `timestamp_type
   == 3` decoding is tested but has never been produced by a device.
