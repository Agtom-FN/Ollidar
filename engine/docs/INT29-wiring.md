# INT-29 — wiring A10 (GNSS/RTK) into the engine core, the events and the C ABI

**Scope:** `engine/src/core/**`, `engine/include/scanengine/core/**`, `engine/capi/**`,
`engine/CMakeLists.txt`, `engine/DESIGN.md`,
`engine/tests/{test_engine,test_capi}.cpp`, `engine/tests/capi_smoke.c`.

**Reads:** `docs/A10-gnss.md` §9 (the seam list this task exists to close),
`docs/INT24-wiring.md` (the ABI v2 conventions every addition here follows),
`engine/DESIGN.md` §2/§4/§6, `docs/A9-export.md` "CRS seam".

Nothing in `gnss/`, `slam/`, `color/`, `plan/`, `jobs/`, `export/`, `record/` or
`drivers/` was touched, and neither was `android/` or `desktop/`. A10's 36 offline
`gnss/*` cases are untouched and still green.

---

## 1. What the C ABI gained (ABI 2 → 3)

`SCAN_ABI_VERSION` and `scanengine::kEngineAbiVersion` moved together, as DESIGN §6
item 9 requires. `scan_event`'s union and `scan_session_config` both changed layout, so
this is a real break for a JNI binary built against ABI 2 — which is what the version
check at library load exists to catch.

| Addition | Mirrors |
| --- | --- |
| `scan_engine_push_nmea` | `Engine::push_serial_bytes` on a rover device |
| `scan_engine_last_fix` / `scan_gnss_fix` | `GnssSource::last_fix` / `GnssFix` |
| `scan_engine_gnss_stats` / `scan_gnss_stats` | `GnssSource::stats` + the ENU origin |
| `scan_engine_georef_solution` / `scan_georef_solution` | `GeorefFusion::solution` |
| `scan_engine_crs_wkt` / `scan_engine_crs_epsg` | the A9 export seam |
| `scan_ntrip` handle: `_create` / `_destroy` / `_connect` / `_disconnect` / `_get_state` / `_get_stats` / `_set_rtcm_callback` | `TcpNtripClient` |
| `scan_ntrip_fetch_sourcetable` / `scan_ntrip_source` | the mountpoint picker |
| `scan_ntrip_config`, `scan_ntrip_stats` | `NtripConfig`, `NtripStats` (+ `Rtcm3Stats`) |
| `SCAN_FIX_*` (5), `SCAN_NTRIP_*` (6), `SCAN_TRAJECTORY_*` (2) | `FixType`, `NtripState`, `TrajectorySource` |
| `SCAN_EVENT_NTRIP_STATE` (51), `SCAN_EVENT_GEOREF_CONVERGED` (52) | `EventType` |
| `scan_event.payload.gnss` / `.ntrip` / `.georef` / `.health` | the four payloads |
| `scan_session_config.trajectory` | `SessionConfig::trajectory` |

Every new enum value is `static_assert`ed against its C++ original. `FixType` earns
that guard as much as A8's pose gates did: §3.4's capture gate is an **ordered**
comparison ("at or above RTK Float"), so inserting a state in the middle without moving
the mirror would silently change which captures the app allows.

Five contract decisions, all documented in the header:

1. **`scan_engine_push_nmea` refuses a non-rover device** (`SCAN_ERR_INVALID_ARGUMENT`).
   Feeding NMEA to a D6 parser is not an error anywhere — it reads as a stream of
   malformed packets and degrades the wrong device's health row.
2. **`scan_ntrip_create(engine, …)` BORROWS the engine's client**; passing `NULL` for the
   engine creates a standalone one. The borrowed one is the one whose GGA upload is the
   rover's own last sentence and whose forwarded frames are recorded, and it is what an
   app wants; the standalone one is for a mountpoint picker with no engine. `destroy()`
   frees only what it owns, and clears the engine's rover sink on the way out so a
   destroyed handle cannot leave a dangling callback on a client that outlives it.
3. **`scan_ntrip_set_rtcm_callback` on a borrowed handle routes through
   `Engine::set_rtcm_sink`, not through `NtripClient::set_rtcm_callback`** — the latter
   would replace the Engine's own handler, which is what records the frames. A
   trampoline converts `ByteSpan` to `(ptr, len)`; never a function-pointer cast
   (DESIGN §4).
4. **`scan_ntrip_fetch_sourcetable` always writes the caster's TRUE total** to
   `*out_count`, even when the array was too small, and returns
   `SCAN_ERR_CAPACITY_EXCEEDED`. A caller then sizes a second array from the answer
   instead of guessing. `scan_ntrip_source`'s strings are fixed-size arrays, not
   pointers, so the record honours convention 3 by value.
5. **`scan_engine_crs_wkt` / `_crs_epsg` return `""`, never `NULL`**, so a JNI
   `NewStringUTF()` on the result is always safe, and the buffer is `thread_local`,
   which is what makes convention 4's "valid until the next engine call on the same
   thread" true when two threads ask at once.

### The event union's missing cases (B2's gap)

`convert_event()` had a `default:` that memcpy'd the raw payload for anything it did not
mirror. Four types now have real cases: the three new GNSS ones, and
**`kDeviceHealth`**, which had a C++ payload, a C `SCAN_EVENT_DEVICE_HEALTH` constant and
no union member at all — a consumer reading it was reading a C++ struct's padding through
a C `uint8_t[64]`. `kPoseUpdate` was already done by INT-24. Only `kJobProgress` (A15)
still travels as raw bytes, and the comment now says so by name.

---

## 2. Engine ownership

```
        app rover-reader thread                    NTRIP receive thread
                  │                                        │
   push_serial_bytes(kRtkRover)                     recv → Rtcm3Framer
                  │                                        │ whole CRC-valid frames
      recorder(kGnssNmea)  ← record-always                  ├─▶ recorder(kGnssRtcm)
                  │                                        └─▶ set_rtcm_sink → app → rover
            GnssSource  ── PoseInterpolator ──▶ D6PushbroomAssembler  (trajectory = kGnss)
                  │  fix callback                                  ▲
                  ├──────────▶ EventBus  kGnssFix                  │ D6 decode thread
                  │                                                │
                  └──▶ GeorefFusion.add_fix() ──▶ kGeorefConverged │
                              ▲                                    │
                              │ sample_at()                        │
                       ExternalPoseSource ◀── push_pose() ─────────┘
                       (the LOCAL trajectory)
                              │
                     crs_wkt() / crs_epsg() ──▶ A9 ExportOptions
```

* **One `GnssSource`, one `TcpNtripClient` and one `GeorefFusion` per Engine**, all alive
  for the Engine's whole lifetime rather than per session. That is the same call INT-24
  made for the pose source and for the same reason: an operator pairs the rover, joins a
  caster and waits for RTK Fixed *before* pressing record, and §3.4's capture gate is
  exactly that pre-session decision. They are configured through **`EngineConfig::gnss`
  and `EngineConfig::georef`**, not through `DeviceConfig` — see §2.2.
* **`DeviceKind::kRtkRover` is a thin `Driver` adapter** (`RtkRoverDriver`, internal to
  `src/core/engine.cpp`) that owns no transport and no decoder. It exists to give the
  rover a `DeviceId`, a `DeviceState` and a health row, because that is what the rest of
  the engine and B9's device list are built around. Its health is the **NMEA link**
  stats, not the fix state: this row answers "is the link working", which is a different
  question from "is the sky working". A rover under a bridge streams perfect NMEA with
  fix 0, and that is `kStreaming` with a health row full of good checksums — the fix
  quality reaches the UI through `kGnssFix` and `GnssStats::by_fix` instead.
* **Record-always covers both GNSS legs.** `push_serial_bytes()` writes the pushed buffer
  as a `kGnssNmea` chunk **before** it is parsed, exactly as it already did for `kD6Raw`,
  and the NTRIP handler writes every forwarded frame as `kGnssRtcm`. Both go through the
  same `record_m` the Mid-360 raw shim uses, because `FileRecordWriter` is not internally
  synchronized and the rover reader, the D6 reader and the NTRIP thread can now all
  record concurrently.

### 2.1 Two decisions worth stating

**The recorded NMEA chunk is the pushed BUFFER, not one sentence.** `ChunkType::kGnssNmea`
is documented "one NMEA sentence", and the Engine deliberately does not honour that
literally: Bluetooth SPP hands the app 20–990-byte MTU fragments, and re-framing them
into sentences here would mean **parsing before recording**, which is the one thing
record-always forbids. `GnssSource::push_nmea` frames arbitrary chunks (A10 tests it at
1/3/9/27/81-byte boundaries), so a replay that feeds the chunks back byte for byte
reproduces the capture. If A5 wants per-sentence chunks it can split during replay, where
a parse failure costs nothing.

**`Engine::crs_wkt()` is gated on convergence and `GeorefFusion::crs_wkt()` is not.**
That difference is deliberate, and the two answer different questions. The fusion's
version answers *"what CRS is this site in"*, which is known as soon as an ENU origin
exists — the UTM zone is a property of where the rover is standing. The Engine's is the
A9 export seam, and it answers *"what CRS may I LABEL this cloud with"*, which
additionally needs the local→global transform, because until that converges the points
are still in the local frame. Handing A9 a UTM WKT for a local-frame cloud produces a
file that opens in QGIS, lands in the wrong hemisphere, and never says why. Empty is
exactly A9's documented "embed the local-frame placeholder" input, so the conservative
answer costs nothing. `engine/tests/test_engine.cpp`'s
`a_rover_below_the_gate_reports_the_blocker_instead_of_a_transform` pins both halves at
once: `georef().epsg() == 32650` **and** `crs_wkt().empty()`.

### 2.2 Why the GNSS config is on `EngineConfig`, not `DeviceConfig`

`DeviceConfig::gnss` would be the symmetrical choice, and it is the wrong one here. The
`GnssSource` has to exist before any device is added (it is what an app watches to decide
whether to *start* a capture), its callbacks are wired at `create()` time, and a second
rover would need the source keyed by `DeviceId` anyway — which the spec's rigs do not
have. So `add_device({kind: kRtkRover})` takes no configuration at all, and
`EngineConfig::gnss` / `::georef` carry the knobs. `stream` and `timesync` are overwritten
by `create()` (they are wiring, not a choice), exactly as `LioConfig::map_store` is.

### 2.3 The trajectory switch

`SessionConfig::trajectory` (and `Engine::set_trajectory_source()` mid-session) chooses
which `PoseInterpolator` the D6 assembler reads:

```cpp
enum class TrajectorySource : std::uint8_t { kExternal = 0, kGnss = 1 };
```

`kGnss` is Tech Spec §3.3's *"Desktop D6 capture: no ARCore → RTK-trajectory mode only"*,
and it required **no new code path and no change in `slam/`**: `GnssSource` already
implements the same `PoseInterpolator` the assembler consumes for ARCore, which is §3 key
rule 3 working as designed. The georef fusion's *local* source stays the
`ExternalPoseSource` either way — pairing GNSS against GNSS is degenerate by
construction, and `Engine::set_georef_local_source()` is the seam for pointing it at a
replayed or A7-smoothed track instead.

---

## 3. Events

Three types, following DESIGN §6 step 6 in full (append to `EventType`, add the payload
POD, extend `to_string()` and `category_of()`, mirror in `scanengine_c.h`, add a
`convert_event()` case):

| Type | Payload | Published when |
| --- | --- | --- |
| `kGnssFix` (50, already declared by A1) | fix type, satellites, HDOP, **corrections age**, **σ_h**, lat/lon/alt | once per closed NMEA epoch, on the pushing thread |
| `kNtripState` (51) | state, `ScanError`, backoff, bytes, **engine-side** corrections age | every `TcpNtripClient` state transition, on the NTRIP thread |
| `kGeorefConverged` (52) | CEP95, σ_h, inliers, EPSG, **converged 0/1** | on the *transition*, in both directions |

Three things the payloads say that a naive version would not:

* **`GnssFixPayload::fix_type` is `FixType`, not the GGA quality digit.** A1's comment
  said "4 rtk-fixed, 5 rtk-float" — the wire numbering, which is the *opposite order*
  from `FixType`. Anything gating on "at or above" with the wire digit ranks Float above
  Fixed. The comment is corrected and `quality_raw` keeps the wire digit for callers who
  need to tell a PPS fix from a simulator one.
* **`σ_h` is in the payload and the DOP is not the accuracy.** HDOP is geometry; the
  metre number a UI should print comes from GST when the receiver sends it, and from the
  fix-quality fallback table otherwise.
* **There are two corrections ages and they are not the same** (A10 §3).
  `GnssFixPayload::correction_age_s` is the **rover's** (GGA field 13, what its RTK
  engine actually applied); `NtripStatePayload::correction_age_s` is the **engine's**
  (time since the last CRC-valid frame off the caster), and it is **−1**, not 0, before
  the first frame. A UI showing one must say which.
* **`kGeorefConverged` fires on the transition, not once per fix**, and carries
  `converged` so that *losing* convergence is announced too — a UI that already told the
  operator the scan was exportable has to be able to take it back.

All three are in `EventCategory::kPose`: a consumer that wants the trajectory wants the
fix quality behind it, the corrections link that produced that quality, and the moment
the session became georeferenceable.

---

## 4. CMake

`engine/CMakeLists.txt` gained A10 §9.1's `sim-rtk` block verbatim, immediately after
A3's `sim` block (it reuses that block's `ENGINE_SIM_TESTS` option, so the order
matters). `src/gnss/*.cpp` and `tests/test_gnss.cpp` were already picked up by the
existing globs, so **no CMake change was needed to build or to run A10's 36 offline
cases** — this registers only the two `doctest::skip()`-tagged live ones.

Unlike `mid360_sim_e2e` it is **not** `RUN_SERIAL`: the cases bind per-process ports
(39000/40000 + pid % 900) and write per-process temp files, so two build trees on one
host cannot interfere.

```
$ ctest -LE 'sim|sim-rtk'    # the fast path: everything else
$ ctest -L sim-rtk           # ~40 s: live NMEA over TCP, live NTRIP
```

---

## 5. Tests

`scanengine_tests` gained **6 cases** (3 engine, 3 C ABI) and `capi_smoke.c` went 67 →
**102 checked steps**, still compiling clean as C11.

| New case | What it pins down |
| --- | --- |
| `engine/rtk_rover_session_end_to_end_from_synthetic_nmea` | A 200-epoch RTK session end to end: 600 sentences in, 200 epochs, 200 fixes, 0 checksum failures, GST sigmas on every epoch, 200 `kGnssNmea` chunks recorded **before** parsing, the device row reporting the link, and the fusion recovering yaw **37.000°** and translation **(11.9999, −7.99989, 1.24992)** against truth (37, 12, −8, 1.25) with **CEP95 4.9 cm** over 200 inliers. `crs_epsg() == "EPSG:32650"`, `crs_wkt()` starts with `PROJCS["WGS 84 / UTM zone 50N"`. Exactly one `kGeorefConverged`, carrying 8 samples — the first moment the estimator's floor is met, which is when the capture UI may tell the operator the scan is exportable. |
| `engine/a_rover_below_the_gate_reports_the_blocker_instead_of_a_transform` | The other half of §3.4's gate. 40 Single fixes: a coarse trajectory *is* published (40 poses), all 40 are offered to the fusion and all 40 refused on quality — not down-weighted — the solution stays unconverged, `to_global_point()` is `kInvalidState`, the **site's** EPSG is known and the **export's** WKT is empty. |
| `engine/the_rtk_trajectory_drives_the_pushbroom_with_no_arcore` | §3.3's desktop mode. A rover and a D6 in one session, `trajectory = kGnss`, **nothing ever pushed into the ARCore ring** (`pose_at()` is `kNoData`), and 81 D6 returns become 81 world points on `kSlamMap` with `dropped_no_pose == 0` — cross-checked against `gnss().sample_at(t_scan)`, i.e. the same interpolator the assembler used. Switching back mid-session is one call. |
| `capi/gnss_and_ntrip_entry_points_reject_null_handles` | Every new entry point is an error, never a crash, on a null handle; the two string accessors return `""` rather than `NULL`; `push_nmea` on a D6 device is `SCAN_ERR_INVALID_ARGUMENT` and on an unknown one `SCAN_ERR_NOT_FOUND`; an unconfigured caster is refused **without touching a socket**. |
| `capi/rtk_fixes_and_the_georef_solution_cross_the_abi` | The whole rover surface from C++-side C: "no fix yet" is `SCAN_OK` with `SCAN_FIX_NONE`, the fix converts field by field (including the orthometric/ellipsoidal pair), the timeline and the ENU origin come back in `scan_gnss_stats`, and the `kGnssFix` payload survives `convert_event()`. Two fixes cannot converge a 4-parameter transform, and the export CRS stays empty with it. |
| `capi/an_engine_backed_ntrip_handle_shares_the_engines_client` | Borrowed and standalone handles both work; `correction_age_s` is **−1** before the first frame; `disconnect()` on a client that never connected is a no-op; and destroying a borrowed handle leaves the engine's client usable and callback-free. |
| `capi_smoke.c` steps 68–102 | The same sequence from **actual C11**: rover device, NMEA built with a checksum computed in the file (so a mistyped digit cannot pass silently), fix/stats/georef/CRS reads, the NTRIP handle's whole lifecycle, and the `scan_rtcm_cb` signature being callable from C. |

`engine/unknown_device_kinds_and_ids_are_rejected` is the one pre-existing case that
changed meaning: `kRtkRover` used to assert `kUnimplemented` and now asserts a working
device. The two ABI-version cases now read 3.

### Two things the tests had to be taught, and both are real contracts

* **`TimePoint{0}` means "stamp on arrival."** A first draft used a `t = 0` time base and
  saw 3 fixes published out of 200: epoch 0 got a real `SteadyClock` stamp, every later
  epoch got 1 ns…199 s, and `GnssSource` correctly refused the non-monotone trajectory
  rather than reordering it. The test grid starts at 5 s.
* **A 1 Hz local trajectory cannot be paired with 1 Hz fixes.**
  `ExternalPoseConfig::max_gap_ns` is 200 ms, so interpolating across a 1 s pose gap is
  `PoseGate::kStale` and `GeorefConfig::require_ungated_local_pose` (correctly) drops the
  correspondence — 199 of 200, in the first draft. The test pushes the local track at
  10 Hz, which is what ARCore and A6's LIO actually deliver.

---

## 6. Verification

macOS 15 (Darwin 25.5.0), Apple silicon, AppleClang, Ninja, CMake 4.4.2, from a **deleted
build directory**.

```
$ cmake -S engine -B build -G Ninja -DENGINE_WARNINGS_AS_ERRORS=ON
$ cmake --build build                  # clean: zero warnings from engine code
                                       # (only the vendored Livox SDK2 warns)

$ ./build/scanengine_tests "--test-case=engine/*,capi/*,gnss/*"
[doctest] test cases:   64 |   64 passed | 0 failed | 336 skipped
[doctest] assertions: 5596 | 5596 passed | 0 failed |

$ ctest -LE 'sim|sim-rtk'
  scanengine_capi_smoke / engine_cli_selftest / engine_cli_version ... Passed

$ ctest -L sim-rtk --output-on-failure
  1/1 Test #6: gnss_rtk_sim_e2e ......... Passed  10.60 sec
  100% tests passed out of 1
```

The `sim-rtk` label registers, is excluded by `-LE 'sim|sim-rtk'`, and passes when
invoked explicitly — driving A10's two live cases against the S5 Python simulators.

**One caveat, and it is not this change.** This wave ran with four other agents editing
the tree concurrently. At the moment of the final run, 11 cases were failing in
`tests/test_color.cpp` (A12's colorization work) and `tests/test_jobs.cpp` (the jobs
pipeline) — files INT-29 may not edit and does not touch. Every `engine/*`, `capi/*` and
`gnss/*` case is green, as is the whole suite when built against a tree with those two
modules' in-flight sources excluded:

```
[doctest] test cases:     343 |     343 passed | 0 failed | 7 skipped
[doctest] assertions: 2257213 | 2257213 passed | 0 failed |
```

---

## 7. Left undone, and for whom

1. **A10 §9.6's `.lscan` replay half.** The raw NMEA and RTCM now hit the recorder, but
   nothing reads them back: `record/` owns the replay side, and a periodic
   `GeorefSolution` + ENU-origin snapshot in `manifest.json` (so a replay does not
   re-derive the alignment) is A5/A15's file to edit.
2. **A9 is not yet *calling* `crs_wkt()`.** The seam is filled and tested; wiring it into
   the export options is `export/`'s line, not this task's. It is two lines
   (A10 §9.5 prints them).
3. **A7's factor graph.** `GeorefFusion::set_estimator()` is reachable from the Engine
   through `georef()`, but nothing in `core/` or `capi/` chooses an estimator. When A7
   lands one, the C ABI needs no change at all.
4. **One rover per Engine.** `RtkRoverDriver` routes every `kRtkRover` device to the same
   `GnssSource`, so adding two rovers gives two device rows over one receiver. Two
   antennas would also give a *heading*, which is the interesting reason to do it, and it
   needs the source keyed by `DeviceId`.
5. **`scan_ntrip_config` cannot express a caller-supplied WKT** (`CrsConfig::epsg` plus a
   survey profile's own WKT — A10 §4's escape hatch). It is reachable in C++ through
   `EngineConfig::georef.crs`; a C consumer needing a national grid will want a
   `scan_engine_set_crs()` at ABI 4.
6. **The NTRIP thread has no live coverage in `ctest -LE sim-rtk`.** Everything asserted
   here is state-machine and lifecycle; the connect/stream/reconnect path is A10's
   `gnsssim/*` case, which is exactly what the new `sim-rtk` label runs.
