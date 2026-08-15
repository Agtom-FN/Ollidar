# libscanengine — engine design (task A1)

**Status:** A1 complete. This document is the contract A2–A15, B-workstream (JNI) and
C-workstream (Qt) build against. Where a decision here conflicts with a later task's
convenience, the decision wins unless it is renegotiated in this file.

Spec references are to `docs/LidarScan Tech Spec.md` v1.2.1.

---

## 1. Module map

```
engine/
  CMakeLists.txt        one library (scanengine) + engine_cli + tests
  CMakePresets.json     5 presets, migrated from the S7 spike
  vcpkg.json            eigen3 now; dependency onboarding order documented inline
  cmake/triplets/       universal-osx overlay triplet (macOS arm64 + x86_64)
  capi/                 scanengine_c.h / .cpp   — the flat C ABI for JNI
  include/scanengine/   public headers (this is the API surface)
  src/                  implementations, mirroring include/ 1:1
  tests/                doctest suite + the S1 packet builder + a C smoke test
  tools/engine_cli.cpp  headless CLI: --selftest, --synth, --replay, --post
                        (--post is workstream D1's cloud-worker entry point)
  third_party/doctest/  vendored single-header test framework
```

| Module | Files | Owner(s) | State after A1 |
| --- | --- | --- | --- |
| `core/` | `engine.h` `error.h` `event.h` `event_bus.h` `log.h` `span.h` `types.h` | A1 | **complete** — lifecycle, error model, event bus, logging facade |
| `transport/` | `byte_source.h` `usb_serial_source.h` `udp_source.h` | A1 / A2 / A3 | `UsbSerialSource` complete; `UdpSource` is a stub returning `kUnimplemented` |
| `drivers/d6/` | `d6_parser.h/.cpp` `commands.h` `d6_driver.h/.cpp` | A1 / A2 / A8 | parser = the finished S1 artefact, copied in; driver integrates it end to end |
| `drivers/mid360/` | `mid360_driver.h/.cpp` | **A3** | interface + stub; `start()` returns `kUnimplemented` |
| `timesync/` | `clock.h` `offset_estimator.h/.cpp` | A1 / A4 | clock complete (S7 backends); `TimeSync` registry + passthrough estimator |
| `record/` | `lscan.h/.cpp` `replay.h/.cpp` `zip.h/.cpp` | A1 / A5 | **implemented (A5)** — format constants, CRC32, framing, the on-disk writer/reader, replay, and the `.lscan.zip` transfer bundle (INT-34 added the zip's optional progress callback + cancel token, and the manifest's `clockOffsets` key) |
| `cloud/` | `point_page.h` `page_store.h/.cpp` `display_params.h/.cpp` | A1 / A14 | complete — the render-facing contract. INT-34 added `page_data_mutable()` + `notify_recoloured()` and `PageUpdate::kind`: colorization is the one producer that rewrites points that already exist, and a renderer has to be able to tell a recolour from an append (docs/A11-color.md §8.1) |
| `poses/` | `pose_source.h` `pose_interpolator.h` `se3.h` `external_pose_source.h/.cpp` | A1 / **A8** / A10 | **implemented (A8)** — SE(3) vocabulary, gated interpolation, the ARCore/RTK/replay ingestion ring |
| `slam/pushbroom/` | `pushbroom_assembler.h/.cpp` `mount_calibration.h/.cpp` | **A8** | **implemented** — D6 profiles × trajectory × mount extrinsic → world points; the S6 planar-checkerboard solver with the split-half gate |
| `slam/` (live) | `lio.h` `eskf.h` `ivox.h` + `src/slam/*.cpp` | **A6** | **implemented** — ESKF lidar-inertial odometry, incremental voxel map, `LioPoseSource` |
| `gnss/` | `gnss.h` `nmea.h` `rtcm3.h` `ntrip_client.h` `gnss_source.h` `georef.h` `crs.h` + `src/gnss/*.cpp` | **A10** | **implemented** — NMEA/RTCM3 parsing, the NTRIP client, the fix timeline, the local→global fusion and the CRS seam (wired INT-29) |
| `slam/post/` | `post_pipeline.h` `pose_graph.h` `loop_closure.h` `scan_context.h` `cloud_filter.h` `progress.h` + `src/slam/post/*.cpp` | **A7** | **implemented** — the cancellable, progress-reported offline pipeline |
| `export/` | `exporter.h` `las_constants.h` + `src/export/*.cpp` | **A9** | **implemented** — PLY / LAS 1.4 / PCD |
| `color/` | `colorize.h` `frames_idx.h` `image_source.h` `clock_sweep.h` `colorizer.h` + `src/color/*.cpp` | **A11** | **implemented** — the `frames.idx` format, JPEG/PNG decode (vendored stb_image), the wizard's clock sweep, and `PointColorizer` (best-view selection, z-buffer occlusion, rolling shutter, the motion gate). INT-34 added `set_cancel_token()`/`set_progress_fn()` to the **abstract** `Colorizer` seam (docs/A15-jobs.md §7.6) |
| `plan/` | `floor_plan.h` `occupancy.h` `plan_model.h` `plan_editor.h` `plan_writers.h` + `src/plan/*.cpp` | **A12** | **implemented** — occupancy, wall extraction, rooms, DXF/PDF |
| `jobs/` | `job.h` (A1 seam) `job_types.h` `job_queue.h` `local_runner.h` `transfer.h` `cloud_submit.h` `http_transport.h` + `src/jobs/*.cpp` | **A15** / INT-34 | **implemented** — the five job kinds, one worker thread with priorities and cancellation, the cloud REST client and the transfer bundle. INT-34 added `job_runner_adapter.h` (A1's `JobRunner` seam over the queue, docs/A15-jobs.md §7.3) and `colorize_wiring.h` (docs/A11-color.md §8.3's A4/A7 → A11 one-liners) |
| `merge/` | `merge.h` (+ `src/merge/` in flight) | A13 | **A13 was mid-flight while INT-34 landed** — this row is A13's to write, not INT-34's. |

Rule: **`src/<module>/x.cpp` implements `include/scanengine/<module>/x.h`.** Nothing in
`src/` is included across module boundaries; modules talk through public headers only.

Namespaces: everything is `scanengine::`, except the D6 wire parser, which keeps its
original `d6::` namespace so the S1 files stay byte-identical to the spike.

---

## 2. Threading model

**The engine owns no threads of its own.** That is a deliberate starting point, not an
oversight: every thread that will exist gets introduced by the task that needs it, and
must be documented here when it lands.

The `Engine` can now *cause* three threads to exist, and drives none of them: A6's
`LioOdometry` (only with `LioConfig::internal_thread`), A10's `TcpNtripClient` receive
thread (only between `connect()` and `disconnect()`), and — since INT-34 — A15's
`jobs::JobQueue` worker, created **lazily on the first `Engine::jobs()` call**, so an
Engine that never processes anything still owns nothing.

| Thread | Owner | What runs on it |
| --- | --- | --- |
| App serial-reader thread | app (Android JNI reader / Qt `readyRead`) | `push_serial_bytes()` → `UsbSerialSource::push` → D6 parser → point conversion → `PageStore::append` → `EventBus::publish`. The **entire decode path is synchronous on the caller's thread.** |
| App control thread | app | `create/start_session/add_device/stop_session`. Serialized by one engine mutex. |
| App consumer thread | app | `poll_event` / `wait_event` / `drain`, plus reading point pages for GPU upload. |
| SDK2 receive threads *(A3, landed)* | Livox SDK2 | point + IMU callbacks → decode → `PageStore` / IMU ring. Reaches the rest of the engine only through `PageStore` and `EventBus`. |
| Driver supervisor *(A3, landed)* | `Mid360Driver` | 20 Hz `tick()`: watchdog, reconnect, per-second health. One per driver. `internal_supervisor_thread = false` + manual `tick()` in unit tests. |
| `UdpSource` receive *(A3, landed)* | `UdpSource` | raw-UDP backend only; one per bound port; blocking `recvfrom` with 100 ms timeout so `stop()` is prompt. Lock order `flush_m_` → `m_`; `PageStore::append` publishes with **no** driver lock held (see docs/A3-mid360-driver.md §6). |
| LIO odometry *(A6, landed)* | `LioOdometry` (only when `LioConfig::internal_thread`) | Drains the IMU/point queues, propagates, undistorts, runs the iterated update, inserts into the voxel map, appends to the `PageStore`, publishes the pose. One per instance; ~10 Hz scan-to-map, ≤ 2 big cores on Android. Lock order `proc_m` → `in_m`; `LioPoseSource` callbacks fire **outside** its lock, so a subscriber cannot deadlock the odometry from one. With `internal_thread = false` (the default, and what every test uses) the pipeline runs inline on the caller's thread — the engine's default posture. |
| NTRIP receive *(A10, landed; wired INT-29)* | `TcpNtripClient` | blocking `recv` with a 1 s socket timeout so `disconnect()` is prompt; RTCM3 framing; the rover-write callback; periodic GGA upload; stall detection; reconnect with jittered exponential backoff. One per client, and the Engine owns exactly one client — but the thread exists only between `ntrip().connect()` and `disconnect()`, not for the Engine's lifetime. The rover callback runs **on this thread**, with **no client lock held**, so a slow Bluetooth write cannot deadlock the client; it must still be quick and must not re-enter the client (the general callback rule). |
| Job worker *(A15, landed; owned by the Engine since INT-34)* | `jobs::JobQueue` | **One** thread per queue, started in the constructor and joined in the destructor. Runs **post-processing, colorization, point export, extract-for-transfer and cloud submit** — every job kind, strictly one at a time, highest `JobSpec::priority` first and FIFO within a level. Cancellation is cooperative per kind (`post::CancelToken`, `ExportCancelToken`, `lscan::ZipCancelToken` since INT-34, an atomic between upload chunks) and the job still finalizes from this thread once its `run()` unwinds. Progress republishes as `EventType::kJobProgress` on the queue's `EventBus`. Completion callbacks fire **on this thread** — quick, no re-entry, the same rule `EventBus` callbacks and `PageStore` subscribers follow. `Engine::jobs()` creates the one queue an app should use, **lazily**, so an Engine that never processes anything owns no thread; `~Engine` destroys it first, before the recorder, the page store and the bus. See `docs/A15-jobs.md` §3 and `docs/INT34-wiring.md`. |

### Where the Engine's own consumers run (INT-24)

Neither A8's assembler nor A6's odometry gets a thread from the Engine. Both hang off
paths that already exist:

* **`ExternalPoseSource`** (one per Engine, `StreamId::kPoseAr`) is internally
  synchronized. `Engine::push_pose()` is called from the app's ARCore/JNI thread while
  points are decoded elsewhere; that is the case it was built for.
* **`D6PushbroomAssembler`** (one per Engine) is *not* internally synchronized — A8's
  contract is "one D6 decode thread drives it". It is fed from `D6Config::profile_sink`,
  i.e. on the app's serial-reader thread inside `push_serial_bytes()`, and the Engine
  additionally holds one mutex around every touch of it, because `pushbroom_flush()` /
  `pushbroom_stats()` arrive from the control thread.
* **`LioOdometry`** is fed by two engine-owned sinks: `PageStore` updates on
  `StreamId::kLidarMid360` (points) and `Mid360Config::imu_sink` → the Engine's single
  `ImuIngest` (IMU). Both run on the Mid-360 receive thread. With
  `internal_thread = true` — what the C ABI selects for a live capture —
  `push_points()`/`push_imu()` only enqueue, so the receive thread never runs scan-to-map;
  with `false` the odometry runs inline there, which is what makes the unit tests
  deterministic.
* **Re-entrancy is bounded and terminates.** The LIO appends its registered map back into
  the same `PageStore` that is notifying it, so `Engine::Impl::on_page_update` re-enters
  once, with `StreamId::kSlamMap`, and stops (only `kLidarMid360` is forwarded).
  `PageStore::append()` notifies with no lock held, which is what makes that legal.
* **Session teardown is ordered:** `stop_session()` stops every device *first*, so no
  producer can be inside either consumer, then flushes the assembler, then flushes, stops
  and releases the odometry — joining A6's thread — and only then closes the recorder.
  Stopping the `kRtkRover` device is what **flushes the pending GNSS epoch**, which is the
  only moment a 1 Hz receiver's last fix would otherwise be lost.

### Where A10's GNSS stack runs (INT-29)

* **`GnssSource`, `TcpNtripClient` and `GeorefFusion` are one each per Engine**, alive for
  the Engine's whole lifetime rather than per session — the operator pairs the rover,
  joins a caster and waits for RTK Fixed *before* pressing record, and §3.4's capture gate
  is that pre-session decision. All three are internally synchronized.
* **The fix path runs on the app's rover-reader thread**, inside `push_serial_bytes()`:
  NMEA framing → epoch assembly → `GnssSource`'s fix callback → `GeorefFusion::add_fix()`
  (which interpolates the local trajectory) → `kGnssFix` on the bus. The callback fires
  with **no `GnssSource` lock held**, which is precisely what lets `add_fix()` turn around
  and call `sample_at()` without deadlocking.
* **The corrections path runs on the NTRIP receive thread**: whole CRC-valid frames →
  the Engine's handler (record as a `kGnssRtcm` chunk under `record_m`) → the app's
  `set_rtcm_sink()` callback, invoked **outside every engine lock**.
* **`~Engine` joins the NTRIP thread first.** `Impl`'s members are destroyed in reverse
  declaration order, which would otherwise free the recorder while that thread is still
  writing to it.

Why the decode runs on the caller's thread: it keeps the engine free of platform serial
code (§3.1's per-OS matrix lives entirely in the apps), it makes replay bit-identical to
capture, and it gives the app control over priority and core affinity — which matters
directly for the §3.3 "≤ 2 big cores" budget and the thermal risk in §5.

Thread-safety rules:

* `EventBus` and `PageStore` are the only cross-thread hubs. Both are safe from any
  number of producers and consumers.
* `Engine` public methods are safe from any thread.
* A **single `Driver` instance must be pushed from one thread at a time.** Two threads
  pushing bytes into the same D6 device is undefined; two threads pushing into two
  different devices is fine.
* Callbacks (event-bus callback mode, page-store subscribers, the serial write
  function) run **on the caller's/publisher's thread**. They must be quick and must not
  re-enter the engine.

### Event-bus semantics

* **Ordering:** `publish()` assigns a globally increasing `sequence` under one mutex, so
  events are **totally ordered across all producers**, and every subscriber sees that
  order, FIFO, without reordering.
* **Two delivery modes, chosen per subscription:**
  * *Queued* (default) — copied into that subscriber's bounded ring; the consumer drains
    on its own thread with `poll` / `wait` / `drain`. This is what JNI and Qt use.
  * *Callback* — invoked inline on the publishing thread with the bus lock held. For
    in-engine consumers only (record, live SLAM). Non-blocking, no re-entry.
* **Backpressure:** bounded ring, default 1024 events, policies `kDropOldest` (default),
  `kDropNewest`, `kBlock` (offline replay only — it stalls a producer thread).
* **Drops are never silent.** The next event handed to a subscriber that lost events is a
  synthesized `kEventsDropped` carrying the count since last delivery and the total. A
  renderer that sees one must stop trusting incremental ranges and re-read the pages.
* **Lifetime:** `unsubscribe()` blocks until any in-flight callback has returned, so the
  caller's `user_data` is safe to free afterwards. `close()` wakes every waiter.
* **Bulk data never travels in an event.** Points go to the `PageStore` and the event
  carries `(page, first, count)`; raw bytes go to `record/`. That is what keeps `Event`
  an 80-byte trivially copyable POD that can be dropped under load without leaking.

---

## 3. Error philosophy

1. **One flat enum, `ScanError`,** with stable, append-only numeric values. It crosses the
   C ABI as an `int32_t`. No `std::error_code`, no per-module error domains — an Android
   app holding an older header must still interpret a new build's codes.
2. **`Status` / `Result<T>` are `[[nodiscard]]`.** A failure cannot be dropped silently.
   `SCAN_TRY(expr)` propagates.
3. **The enum carries the class of failure; the detail is out of band.** A thread-local
   last-error string (errno/`GetLastError` model) holds a formatted message; the C ABI
   exposes it as `scan_engine_last_error()`. This keeps the enum from growing a value
   every time a message needs a parameter.
4. **No exceptions cross a public boundary.** The engine may throw internally
   (`std::bad_alloc`); the C ABI wraps every entry point in a catch-all.
5. **Failure is graded, not binary.** A device that loses checksums goes `kDegraded`, not
   `kFault`: data is still usable and the S1 exit criterion (>99.5% pass rate) is the
   threshold. A device that fails to start does **not** abort a session — a two-sensor
   capture must survive one sensor being unplugged.
6. **`kAgain` is not an error.** An empty event queue returns it; a caller must not log it.

Adding an error value: append at the end of the enum, add it to `error_str()`, add the
`SCAN_ERR_*` mirror to `capi/scanengine_c.h`. The `static_assert`s in
`capi/scanengine_c.cpp` turn any drift into a build failure.

---

## 4. C-ABI conventions (`capi/scanengine_c.h`)

Used by Android/JNI (§3 key rule 1). The Qt desktop links the C++ API directly and must
not use this header.

* Every fallible function returns `scan_error_t`; out-parameters come last.
* Handles are opaque (`scan_engine*`); the caller frees only with the matching destroy.
* No struct holds a pointer the callee keeps. Input buffers are consumed before return.
* Strings out are UTF-8, engine-owned, valid until the next engine call **on the same
  thread**. Copy them.
* Enum values mirror the C++ enums exactly and are `static_assert`ed. Structs are
  converted **field by field**, never `reinterpret_cast`ed, so the two layouts may
  diverge freely — except `scan_point_vertex`, whose 16-byte identity with `PointVertex`
  is asserted because a GPU upload depends on it.
* `SCAN_ABI_VERSION` + `scan_engine_abi_version()`: JNI must compare these once at
  library load.
* Function pointers with different return types (the C write callback returns `int32_t`,
  the C++ one returns `ScanError`) are bridged with a trampoline, never a cast.

Surface: create/destroy · start/stop/state · add_device/remove_device/device_health ·
push_serial_bytes · poll_event/wait_event/set_event_callback ·
page_count/page_id_at/get_point_page/total_points · last_error/error_str/version/abi ·
set_log_callback · **push_pose/pose_at · set_mount_extrinsics/pushbroom_enable/
pushbroom_flush/pushbroom_stats · scan_mount_calib_create/add_observation/solve/destroy**
(ABI 2, INT-24) · **push_nmea/last_fix/gnss_stats/georef_solution/crs_wkt/crs_epsg ·
scan_ntrip_\*** (ABI 3, INT-29) · **scan_engine_record_keyframe · scan_colorizer_\*
(create/destroy/load_keyframes/set_extrinsics/set_progress_callback/run/cancel/progress/stats)
· scan_clock_sweep_estimate** (ABI 4, INT-34).

ABI 4 also filled two payload gaps rather than adding a call: `scan_event.payload.job`
gives `SCAN_EVENT_JOB_PROGRESS` a real union case (it had been crossing as zeroed opaque
bytes since A1 declared the type — `docs/A15-jobs.md` §7.1), and
`scan_event.payload.points.update_kind` distinguishes a recolour from an append
(`docs/A11-color.md` §8.1). **There is deliberately no C surface for the job queue at
ABI 4:** an app drives it through `Engine::jobs()` in C++, and the `kJobProgress` event
is what a JNI consumer subscribes to. See `docs/INT34-wiring.md`.

The A8 additions follow the same rules and add one convention of their own: **a function
that reports a gate always writes the gate, including when it returns an error.**
`scan_engine_pose_at()` returns `SCAN_OK` whenever a pose could be interpolated *at all*
— the flagged gates (`STALE` / `TRACKING_LOST` / `LOW_CONFIDENCE`) carry real geometry
that the caller must decide about — `SCAN_ERR_AGAIN` for a time newer than the newest
pose, and `SCAN_ERR_NOT_FOUND` for one that predates the stream. Collapsing those into a
single error is exactly what `poses/pose_interpolator.h` exists to avoid.

`tests/capi_smoke.c` is compiled **as C11** and is the reference call sequence for JNI.

---

## 5. The render-facing contract (`cloud/`)

Proven in S3: Filament rendered a paged 10 M-point cloud at 138–149 fps on an M4 with
exactly this layout.

* `PointVertex` = interleaved `float3` position + `RGBA8`, **16 bytes**, `static_assert`ed.
* Pages hold ~1 M points (`kDefaultPageCapacity`), allocated once and **never
  reallocated** — so `PageView::data` is stable and a renderer can keep it.
* `count` only ever grows and is published with a release store; the
  `kPointsAvailable` event (or an acquire read of the count) is the happens-before edge
  that makes lock-free reads of `[0, count)` legal.
* Pages are **single-stream**: provenance survives into merged exports (A13), and page
  ids are **never reused**, so a stale `PageView` or a queued event resolves to
  "not found" rather than to a different page.
* Backpressure: bounded by `max_pages`; a full store appends nothing and returns
  `kCapacityExceeded` with a `dropped_points` counter. A14 replaces the cap with an
  LOD/eviction policy — safe only because A5's recording keeps the raw data on disk.
* One place turns page updates into events: the Engine subscribes to the PageStore, so
  every producer (D6, Mid-360, A6's SLAM map, A8's assembled pushbroom cloud;
  colorization later) gets identical semantics. That one subscriber is also where live
  SLAM is fed from — see §2's INT-24 note — which is what makes the odometry consume
  exactly the points the renderer sees.
* **World-frame results go out on `StreamId::kSlamMap`**, raw sensor-frame previews on
  their device's own stream. Pages are single-stream, so this separates the two clouds
  into different pages, different events and different provenance in an export without
  needing a second store — and a second store is not an option, because `PageId`s are
  allocated per store and would collide across the C ABI's by-id page lookup.

---

## 6. How to add a module (A2–A15)

1. **Add the header first**, under `include/scanengine/<module>/`. Interfaces are pure
   virtual; config is a plain struct with defaults. Put the *why* — and the spike finding
   you must honour — in the header comment, not in a wiki.
2. **Implement in `src/<module>/`** and add the `.cpp` to the `add_library(scanengine ...)`
   list in `CMakeLists.txt`, in the existing commented grouping.
3. **Reach the rest of the engine only through `DriverContext` / `EventBus` /
   `PageStore` / `TimeSync`.** Do not add a back-pointer to `Engine`.
4. **Document your threads** in §2 of this file if you create any.
5. **Errors:** return `Status`/`Result<T>`; call `set_last_error(code, "...")` with a
   message that names the module and the parameters. Never `printf` a failure.
6. **Events:** if you need a new event type, append to `EventType`, add its payload POD,
   extend `category_of()`, mirror both in `capi/scanengine_c.h`, and add a `case` to
   `convert_event()` — otherwise the payload crosses the ABI as opaque bytes.
7. **Tests:** add `tests/test_<module>.cpp` to the `scanengine_tests` target. Use the S1
   packet builder (`tests/packet_builder.h`) for synthetic D6 data; do not add new test
   frameworks.
8. **Dependencies:** a new vcpkg port must build on all five CI legs. Read the
   `$dependency-onboarding-order` notes in `vcpkg.json` before adding one — in
   particular, autotools-based ports fail under the macOS universal overlay triplet.
9. **Never break the ABI silently.** Changing a struct in `scanengine_c.h`, or the meaning
   of an enum value, means bumping `SCAN_ABI_VERSION` and `kEngineAbiVersion` together.

### Notes for the immediately-next tasks

* **A2 (D6 hardening).** The parser is untouched S1 code — keep it that way and put
  reconnect/health/fault policy in `D6Driver`. The health model today is: first decoded
  packet promotes `kStarting → kStreaming`; checksum pass rate below 0.995 over ≥200
  packets demotes to `kDegraded`; a device error ACK sets `kFault`. There is no
  stall/timeout detection and no reconnect yet — that is yours. `scan_for_acks()` only
  looks for ACKs while starting/stopping; the device-info frame (`parse_device_info`) is
  parsed by nothing yet and should feed a `kDeviceInfo` .lscan chunk.
* **A3 (Mid-360).** `UdpSource` and `Mid360Driver` are the seams; both return
  `kUnimplemented` today, and `Engine::add_device` already routes to them. The S2-sim
  findings you must implement rather than rediscover are written into
  `transport/udp_source.h` and `drivers/mid360/mid360_driver.h` — vendored/patched SDK2,
  explicit lidar IP on macOS, free-running `udp_cnt` loss model. IMU is `StreamId::kImu`
  and must not go into the PageStore. Your receive thread is the first engine-owned
  thread: document it in §2.
* **A4 (timesync).** `OffsetEstimator` is the interface; `PassthroughOffsetEstimator` is
  the placeholder. `TimeSync::set_estimator()` swaps yours in per stream with no changes
  upstream. The D6 has no device clock — leave it on arrival stamps. Jitter is the number
  A11 budgets against (S6: 15 ms jitter eats 83% of the reprojection budget), so
  `OffsetEstimate::jitter_ns` must become meaningful, not decorative.
* **A5 (.lscan).** The **format is already fixed and tested**: magic, version, chunk
  framing `[len u32][type u16][flags u16][t_mono i64][payload][crc32 u32]`, the CRC32
  polynomial, the chunk-type numbering, the directory layout, and the truncated-tail
  rule. `tests/test_record.cpp` must stay green. Implement `RecordWriter` (replacing
  `NullRecordWriter` in `Engine::Impl`) and `RecordReader`; the engine already calls
  `write_chunk(kD6Raw, …)` with the raw bytes **before** parsing them, so record-always
  is wired — you only have to make it hit the disk.

---

## 7. Verification appendix (A1 exit evidence)

Host: macOS 15 (Darwin 25.5.0), Apple silicon, CMake 4.4.2, Ninja, AppleClang.
No `VCPKG_ROOT` was set, so this run exercised the **FetchContent fallback** for Eigen;
CI exercises the vcpkg manifest path with the pinned baseline.

```
$ cmake --preset macos-universal
-- scanengine: Eigen3 not installed; fetching 3.4.0
-- Configuring done (5.6s) / Generating done — build/macos-universal

$ cmake --build --preset macos-universal
[29/29] ... zero warnings, zero errors
  (a separate -DENGINE_WARNINGS_AS_ERRORS=ON build is also clean;
   tests/capi_smoke.c also compiles clean under `cc -std=c11 -Wall -Wextra -pedantic`)

$ ctest --preset macos-universal
    Start 1: scanengine_tests ......... Passed
    Start 2: scanengine_capi_smoke .... Passed
    Start 3: engine_cli_selftest ...... Passed
    Start 4: engine_cli_version ....... Passed
100% tests passed, 0 tests failed out of 4

$ ./build/macos-universal/scanengine_tests
[doctest] test cases:   83 |   83 passed | 0 failed | 0 skipped
[doctest] assertions: 8935 | 8935 passed | 0 failed |
```

Test-case breakdown (83): 33 D6 parser cases ported verbatim from S1 · 9 core
(error / log / clock / timesync) · 11 event bus · 8 page store · 7 .lscan format · 9 engine
lifecycle + D6 end-to-end · 5 C ABI (one of which drives the pure-C `scan_capi_smoke_run`
sequence through its 38 checked steps) · 1 header self-containment.

Every public header was additionally checked to compile **alone**, as the first include of
an otherwise empty translation unit (28/28 clean) — the seam headers have no .cpp yet, so
a missing include in one of them would otherwise surface only when A6–A15 opened it.

Universal (fat) binaries:

```
$ lipo -info libscanengine.a scanengine_tests engine_cli scanengine_capi_smoke
Architectures in the fat file: libscanengine.a are: x86_64 arm64
Architectures in the fat file: scanengine_tests are: x86_64 arm64
Architectures in the fat file: engine_cli are: x86_64 arm64
Architectures in the fat file: scanengine_capi_smoke are: x86_64 arm64
```

CLI, end to end:

```
$ ./engine_cli --version
scanengine 0.1.0 (clock: mach_absolute_time) (ABI 1)

$ ./engine_cli --selftest --quiet
selftest: 2 s synthetic COIN-D6 capture through the full engine path
  bytes in            : 26287
  packets ok / bad    : 220 / 0
  checksum pass rate  : 1.0000
  points decoded      : 8020
  pages / dropped     : 1 / 0
  device state        : streaming
  events pts/rot/dev  : 52 / 19 / 2
  page bounds (m)     : [-2.001 -1.500 0.000] .. [2.001 1.500 0.000]
selftest: PASS

$ ./engine_cli --synth /tmp/s1_synth.bin 3 --noise
wrote 39426 bytes (3.0 s, with noise) to /tmp/s1_synth.bin

$ ./engine_cli --replay /tmp/s1_synth.bin --chunk 64
replay: /tmp/s1_synth.bin (39426 bytes, 64-byte chunks)
  bytes in            : 39426
  packets ok / bad    : 330 / 0
  checksum pass rate  : 1.0000
  points decoded      : 12030
  pages / dropped     : 1 / 0
  device state        : streaming
  events pts/rot/dev  : 306 / 29 / 2
  page bounds (m)     : [-2.001 -1.500 0.000] .. [2.001 1.500 0.000]
```

The replayed bounds are the synthetic room's true geometry (4.0 × 3.0 m, centred), decoded
through the real engine path — transport → parser → point conversion → page store →
events — at 64-byte chunk boundaries that tear packets arbitrarily. The `--noise` stream
injects speed-adjustment (`0xFE/0xFF`) and garbage bytes between revolutions; they are
dropped, not misparsed (checksum pass rate stays 1.0000).

**Not verified locally:** the Windows/MSVC, Windows/clang-cl, Linux and Android legs —
those run in `.github/workflows/engine-ci.yml`, which A1 repointed from the S7 stub to
`engine/`. E1 owns keeping the matrix green.
