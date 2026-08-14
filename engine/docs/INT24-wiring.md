# INT-24 — wiring A8 (pushbroom) and A6 (live SLAM) into the engine core and the C ABI

**Scope:** `engine/src/core/**`, `engine/include/scanengine/core/**`, `engine/capi/**`,
`engine/tests/{test_engine,test_capi,test_headers}.cpp`, `engine/tests/capi_smoke.c`,
plus the two additive seams A8 and A6 asked other owners for:
`D6Config::profile_sink` and the Mid-360 driver's shared time estimator.

**Reads:** `engine/DESIGN.md` §2/§4/§5/§6, `docs/A8-pushbroom.md` §7,
`docs/A6-lio.md` §2/§5/§9, `docs/A4-timesync.md`, `docs/A3-mid360-driver.md` §4.

Nothing in `slam/`, `post/`, `gnss/`, `export/`, `record/`, `cloud/` or `CMakeLists.txt`
was touched. Every new `.cpp` and `test_*.cpp` is picked up by the existing globs.

---

## 1. What the C ABI gained (ABI 1 → 2)

`SCAN_ABI_VERSION` and `scanengine::kEngineAbiVersion` moved together, as DESIGN §6
item 9 requires. Both struct layouts and the function table changed, so this is a real
break for a JNI binary built against ABI 1 — which is what the version check at library
load exists to catch.

| Addition | Mirrors |
| --- | --- |
| `scan_pose` | `scanengine::Pose` |
| `scan_engine_push_pose` / `scan_engine_pose_at` | `Engine::push_pose` / `Engine::pose_at` |
| `scan_engine_set_mount_extrinsics` / `_pushbroom_enable` / `_pushbroom_flush` / `_pushbroom_stats` | `Engine`'s `D6PushbroomAssembler` |
| `scan_pushbroom_stats` | `PushbroomStats` |
| `scan_mount_calib` handle: `_create` / `_add_observation` / `_solve` / `_destroy` | `MountCalibrationSolver` |
| `scan_mount_calib_result` | `MountCalibResult` |
| `SCAN_POSE_QUALITY_*` (0..3) | `PoseQuality` |
| `SCAN_POSE_GATE_*` (0..6) | `PoseGate` |
| `SCAN_CALIB_GATE_*` (0..3) | `CalibGate` |
| `SCAN_STREAM_SLAM_MAP` (8), `SCAN_STREAM_POSE_LIO` (9) | `StreamId` |
| `scan_event.payload.pose` | `PoseUpdatePayload` |
| `scan_session_config.live_slam`, `.pushbroom` | `SessionConfig` |

Every one of those enum values is `static_assert`ed against its C++ original in
`capi/scanengine_c.cpp`, including the seven `StreamId` values that were previously
only spot-checked. The drift guard earns more here than usual: `PoseGate` decides
§3.3's "flagged and excluded by default" and `CalibGate` decides whether a user is
allowed to ship a calibration, so a silent renumbering would turn a rejected capture
into a good one.

Three deliberate contract decisions, all documented in the header:

1. **`scan_engine_pose_at` always writes `*out_gate`, even when it returns an error**,
   and returns `SCAN_OK` whenever a pose could be interpolated at all — including the
   three flagged gates. The geometry is real; what the gate says is how much to trust
   it. `SCAN_ERR_AGAIN` means "newer than my newest pose, buffer and retry";
   `SCAN_ERR_NOT_FOUND` means "predates the stream, never resolvable".
2. **`confidence < 0` means "derive it"** (`pose_confidence()`), because ARCore has a
   `TrackingState`, not a scalar. An RTK or filter-backed caller passes a real number.
3. **`scan_mount_calib_add_observation` aliases `scan_point_vertex` onto `PointVertex`
   rather than converting field by field.** That is the one exception DESIGN §4 already
   carves out, and the 16-byte `static_assert` is what licenses it.

`tests/capi_smoke.c` calls **every** new function from real C11 (it compiles clean under
`cc -std=c11 -Wall -Wextra -pedantic`, and the standalone `scanengine_capi_smoke` ctest
entry links it as a pure-C executable) and now runs 67 checked steps instead of 38,
including the ARCore push/interpolate sequence, the column-major-matrix trap, and a
six-observation calibration solve that recovers the identity extrinsic and reads
`SCAN_CALIB_GATE_GOOD`.

---

## 2. Engine ownership

```
                    app thread                    D6 decode thread
                        │                                │
   push_pose() ─────────┤                                │
                        ▼                                ▼
             ExternalPoseSource ◀───── sample_at() ─ D6PushbroomAssembler
             (kPoseAr, A4-mapped)                        │  push_profile()
                        │                                ▼
                        └── kPoseUpdate ─▶ EventBus   PageStore(kSlamMap)


        Mid-360 receive thread
                │
    on_point_packet ─▶ PageStore(kLidarMid360) ─┐
                │                               ├─▶ LioOdometry ─▶ PageStore(kSlamMap)
    on_imu_packet ─▶ ImuIngest(kLidarMid360) ───┘        └─▶ poses() : LioPoseSource
```

* **One `ExternalPoseSource` and one `D6PushbroomAssembler` per Engine**, both alive for
  the Engine's whole lifetime rather than per session — an app sets the mount extrinsic
  and pushes ARCore poses while it is still lining the scan up. `start_session()` resets
  the assembler's queue and counters but **keeps the extrinsic**: that is a property of
  the bracket, not of the capture.
* **One `ImuIngest` per Engine, on `StreamId::kLidarMid360`** — A6 §7.2's request,
  verbatim. It is where the device's g-units become m/s² and where its device clock
  becomes engine time.
* **`LioOdometry` is per session**, held by `shared_ptr` because the Mid-360 receive
  thread reaches it through the two sinks while the control thread may be tearing the
  session down; the sinks copy the pointer out under one mutex and then work on their own
  reference.
* **Both driver sinks chain to whatever the app configured**, so installing the Engine's
  own sink never takes a seam away from a caller already using it.

### SessionConfig

```cpp
bool live_slam = false;      // spec §3.1's Live-SLAM vs Record-only toggle
LioConfig lio{};             // A6's knobs; map_store/map_stream are the Engine's
bool pushbroom = false;      // D6 assembly (needs a mount extrinsic first)
PushbroomConfig pushbroom_cfg = engine_pushbroom_defaults();
```

Record-only is the default on both. Live SLAM failing to start does **not** fail the
session: the raw streams are still decoded, recorded and previewed, which is exactly
what a thermally-throttled phone dropping back to Record-only looks like.

### Two decisions worth stating, because they differ from the brief

**The LIO map goes into the Engine's own `PageStore`, not a second one.** The task
sketched "its own map PageStore"; A6 §9 item 2 asked for the opposite ("pass its own
PageStore as `LioConfig::map_store`"), and A6 is right — `PageId`s are allocated per
store, starting at 1, so two stores in one Engine would hand out **colliding page ids**.
`scan_engine_get_point_page(id)` and every queued `kPointsAvailable` event resolve a page
by id alone, and DESIGN §5's "page ids are never reused, so a stale `PageView` resolves
to *not found* rather than to a different page" would stop being true. The map is still
cleanly separated, because **pages are single-stream**: `StreamId::kSlamMap` gets its own
pages, its own provenance in an export, and its own `kPointsAvailable` events, with the
whole existing render path unchanged.

**The pushbroom's assembled cloud also goes out on `StreamId::kSlamMap`**, not on
`kLidarD6`. `PushbroomConfig`'s own default is `kLidarD6`, which would interleave
world-frame points with `D6Driver`'s sensor-frame live preview in the same pages —
the header itself says an app wanting both live views at once should give the assembler
its own stream. `kSlamMap` *is* the registered-world-frame stream, and the pushbroom is
the D6's registration step, so no new `StreamId` was appended for it.

---

## 3. The two seams in files this task does not own

Both are strictly additive; both are no-ops unless someone opts in.

**`D6Config::profile_sink`** (A8 §7.2 item 3) — one callback per decoded return,
`(angle_deg, range_m, intensity, high_reflectivity, t_engine_ns, user_data)`. Polar, not
Cartesian, so the sensor-frame convention is applied in exactly one place (the
assembler) instead of being applied by the driver and un-applied by the assembler. It is
per point rather than per packet because a D6 revolution spans 100 ms — 10 cm of rig
travel at walking pace — and per-packet time smears exactly that much. The live preview
path is byte-for-byte unchanged whether the sink is set or not.

**Mid-360 point stamps now go through the `kLidarMid360` estimator** (A6 §9 item 4 /
§7.2). The driver already fed nothing but arrival stamps; it now also calls
`TimeSync::add_pair(kLidarMid360, device_stamp, arrival)` and stores points under the
mapped time, so the points and the IMU share **one** estimator. A second estimator would
inject a few ms of independent estimation noise *between* the two streams, which is
precisely the quantity motion undistortion is sensitive to. Two details preserved from
A3/A6:

* **Arrival time still drives the watchdog and the health window.** Those are wall-clock
  questions about this host, not about the device's clock, and mapping them would risk a
  spurious stall trip.
* **A packet whose `timestamp == 0`** — the first ~150 datagrams of a cold device — is
  withheld from the estimator but still counted by the `LossTracker`, because loss
  accounting is per datagram and skipping them would read as 150 lost packets.
* `DriverContext::timesync` may be null (unit-test rigs), in which case the behaviour is
  exactly what it was before.

---

## 4. Tests

`ctest -LE sim` is green and `scanengine_tests` went **272 → 280 cases** (775,804
assertions). No pre-existing case was modified except the two that assert the ABI
version, which now read 2.

| New case | What it pins down |
| --- | --- |
| `engine/pushbroom_turns_d6_profiles_into_world_points` | A D6 session with poses pushed produces world points on `kSlamMap`: 401 in, 401 out, none flagged, and the first return lands at `(10, 21, 30.5)` for a `(10, 20, 30)` pose and a +0.5 m z mount — getting the composition order backwards moves z, not y. Also: a sheared extrinsic is rejected, and five `kPoseUpdate` events are published (nobody had ever published that payload). |
| `engine/pushbroom_points_without_a_pose_wait_rather_than_being_dropped` | Points arriving before their pose are PENDING (21 of them), not dropped; they resolve when the bracketing poses arrive. Enabling assembly without an extrinsic is `kInvalidState`. `pose_at()` reports `kBeforeFirst` / `kOk` / `kFuture` at the three interesting times. |
| `engine/live_slam_publishes_a_map_from_a_mid360_session` | A Mid-360 session driven by synthetic datagrams through the `kInject` backend: the ESKF initializes, scans process, `kSlamMap` pages and events appear, `map_points == LioStats::points_mapped`, the trajectory is readable through `live_slam()->poses()`, the IMU lands in the one `ImuIngest` at 200 Hz and **never** in the PageStore, and stopping the session releases the odometry. |
| `engine/record_only_sessions_run_no_odometry` | The other half of the spec's toggle: points and IMU still flow, `live_slam()` is null, and no `kSlamMap` page exists. |
| `capi/pushbroom_world_points_cross_the_abi_on_the_slam_map_stream` | The same pushbroom result reached through the flat C ABI's ordinary page accessors — no new accessor was needed — plus the `kPoseUpdate` payload surviving conversion. |
| `capi/pose_and_pushbroom_entry_points_reject_null_handles` | Every new entry point is an error, never a crash, on a null handle. |
| `capi/mount_calibration_gates_an_undetermined_capture` | Zero and two observations are refused as *undetermined* (six unknowns against 2–3 constraints per pose), not merely reported as ill-conditioned; a non-rigid CAD nominal is refused too. |
| `headers/A6_and_A8_types_are_usable` | A8's five headers and A6's four are now in the self-containment list (A8 §7.3), and the file that used to host the `ImuSample` ODR clash now touches `timesync/imu_ingest.h`'s — the surviving one. |

Verified on macOS 15 (Darwin 25.5.0), Apple silicon, AppleClang, Ninja, from a **deleted
build directory**, universal (arm64 + x86_64) — `lipo` confirms both slices in
`libscanengine.a` and `scanengine_tests`:

```
$ ctest --preset macos-universal -LE sim
  scanengine_tests / scanengine_capi_smoke / engine_cli_selftest / engine_cli_version
  100% tests passed out of 4

$ ./scanengine_tests
[doctest] test cases:    280 |    280 passed | 0 failed | 5 skipped
[doctest] assertions: 775804 | 775804 passed | 0 failed |

$ ./engine_cli --version
scanengine 0.1.0 (clock: mach_absolute_time) (ABI 2)
```

Also clean and 280/280 under `-DENGINE_WITH_EIGEN=OFF -DENGINE_WITH_LIVOX_SDK2=OFF`,
which is the configuration A6 and A8 both promised to keep working.

The `mid360_sim_e2e` ctest entry (label `sim`) is **not** green on this host, and not
because of this change: a stale `lvx2_replay` process from an earlier run still holds
UDP 56000/56100/56200, and `CMakeLists.txt` documents that no two simulator runs may
share a host. The driver under test receives zero datagrams in that state, which is the
observed failure. Every non-`sim` leg passes.

---

## 5. Left undone, and for whom

1. **`.lscan` pose + trajectory streams (A5).** A8 §7.4 and A6 §9 item 5 both want the
   pose stream and the LIO trajectory recorded, so a replayed capture re-assembles (the
   assembler is already replay-clean) and re-registers without re-running the odometry.
   `record/` is not this task's to edit.
2. **The mount calibration in `manifest.json`** (WIZARD.md §3: extrinsic, split-half
   gate, capture distance, bracket ID, sensor serial). A5/A15 own that file. The C ABI
   now hands an app everything it needs to write it.
3. **`slam/slam.h`'s `LiveOdometry` seam is still there**, now describing a class A6
   deliberately does not derive from. Retiring it is A6's or A1's call, not an
   integrator's — but the `ImuSample` collision it documents is already gone.
4. **`ProfilePoint::t_mono_ns` is the carrying packet's arrival stamp**, not a per-return
   interpolation across the packet. The D6 parser exposes one stamp per packet; a real
   per-return time needs a change in `d6_parser.h` (A2's), and at 40 samples per packet
   over ~10 ms it is worth measuring before it is worth building.
5. **Multi-device live SLAM.** One `ImuIngest` and one `LioOdometry` per Engine assumes
   one Mid-360, which is what the spec's capture rigs have. Two units would need both
   keyed by `DeviceId`.
6. **Two pre-existing `-Wswitch` warnings, in files this task may not edit.**
   `StreamId::kSlamMap` (8) and `kPoseLio` (9) were appended to `core/types.h` before
   this task started — A6 §9 item 1 asked for them — but two exhaustive switches over
   `StreamId` were never extended, so a `-DENGINE_WARNINGS_AS_ERRORS=ON` build fails and
   the default build warns twice. Neither is caused by this change (nothing here adds an
   enum value), and both are one line:

   * `src/timesync/offset_estimator.cpp:75` (`stream_has_device_clock`) — both are
     engine-produced, so they belong with `kPoseFused` in the `return false` group.
   * `src/record/lscan.cpp:156` (stream → file name) — A5's call: the registered map is
     points, the LIO track is a pose stream, but which chunk file each lands in is a
     format decision, not an integration one.

   The rest of the tree is warning-free: no diagnostic anywhere else in `src/`, `capi/`,
   `include/`, `tests/` or `tools/` under `-Wall -Wextra -Wshadow`, and the `scanengine`
   target itself builds clean with `-Werror` once those two switches are complete.
