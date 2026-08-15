# INT-34 — closing A11's and A15's seam lists

**Scope:** `engine/src/core/**`, `engine/include/scanengine/core/**`, `engine/capi/**`,
`engine/src/cloud/page_store.*` + its include mirror, `engine/src/record/zip.*` + include,
`engine/include/scanengine/color/{colorize,colorizer}.h` (the additive abstract-seam hooks
only) and the `src/color/` alignment they force, `engine/src/jobs/**` + two new
`include/scanengine/jobs/` headers, `engine/tools/engine_cli.cpp`,
`engine/tests/{test_engine,test_capi,test_headers,test_jobs,test_job_runner,test_cloud}.cpp`
+ `capi_smoke.c`, `engine/DESIGN.md`, `engine/CMakeLists.txt`.

**One documented exception, called out again in §7:** `engine/src/record/lscan.cpp`'s
manifest serialization (and the one setter that feeds it in `record/lscan.h`). That file
is A5's; this is the only edit INT-34 made in it, it is purely additive, and it exists
because A11 §8.4 has nowhere else to live.

**Reads:** `docs/A11-color.md` §8 and `docs/A15-jobs.md` §7 — the two seam lists this task
exists to close — plus `docs/INT29-wiring.md` (the ABI conventions every addition follows)
and `engine/DESIGN.md` §2/§4/§5/§6.

Nothing in `merge/`, `slam/`, `gnss/`, `plan/`, `export/` or `drivers/` was touched, and
neither was `android/` or `desktop/`.

---

## 1. The seam lists, and where each one went

| Seam | Asked by | Closed by |
| --- | --- | --- |
| `page_data_mutable()` on the PageStore | A11 §8.1 | `cloud/page_store.h` (§2) |
| "a way to tell the renderer the colours changed" | A11 §8.1 | `notify_recoloured()` + `PageUpdate::kind` (§2) |
| `scan_engine_record_keyframe` + `scan_keyframe` | A11 §8.2 | ABI 4, over `Engine::record_keyframe()` (§3) |
| `scan_colorizer_*` | A11 §8.2 | ABI 4 (§3) |
| `scan_clock_sweep_estimate` | A11 §8.2 | ABI 4 (§3) |
| A4 → A11 (`sync_quality`, the angular-rate fn) | A11 §8.3 | `jobs/colorize_wiring.h` (§5) |
| A7 → A11 (the pose fn) | A11 §8.3 | `jobs/colorize_wiring.h` (§5) |
| the clock offset in `manifest.json` | A11 §8.4 | `FileRecordWriter::add_clock_offset()` (§7) |
| `color/` headers in `test_headers.cpp`, `color/` implemented in DESIGN §1 | A11 §8.5 | done |
| `kJobProgress` has no C union case | A15 §7.1 | ABI 4 (§3) |
| DESIGN's `(A15) job workers` row | A15 §7.2 | DESIGN §2 (§6) |
| no `JobRunner` adapter | A15 §7.3 | `jobs/job_runner_adapter.h` (§4) |
| `zip_export()`/`zip_import()` have no progress or cancel hook | A15 §7.4 | `record/zip.h` (§7) |
| `engine_cli` has no job-queue-driven CLI | A15 §7.5 | `--post` (§6) |
| the `PointColorizer` fast path is `dynamic_cast`-detected | A15 §7.6 | hooks on the abstract `Colorizer` (§5) |

A8 §6's `ExtrinsicsSolver` wrapper (A11 §8.3's third bullet) is **not** closed — see §9.

---

## 2. The PageStore's mutable seam

`Colorizer::colorize(PageStore*)` has to rewrite points that already exist, and the store
only ever handed out a `const PointVertex*` because its entire traffic was
append-then-render. `src/color/colorizer.cpp` therefore `const_cast`'d the view's pointer.
That cast was *defined* and *narrow* — the buffer is non-const inside the store, never
reallocates, and only r/g/b/a change — but it was implicit where it had to be explicit.

Two additions, and the `const_cast` is gone:

```cpp
PointVertex* page_data_mutable(PageId id);                 // nullptr if not found
Status notify_recoloured(PageId page, uint32_t first, uint32_t count);
```

**The contract the accessor states and the cast could not.** A writer may change r/g/b/a
of any live point and must not touch x/y/z: the page's bounding box is maintained
*incrementally at append time* and is not recomputed here, so moving a point silently
invalidates the frustum-cull box every renderer and exporter reads. That sentence is the
whole reason the accessor is better than the cast — it has somewhere to live.

`page_data_mutable()` deliberately does **not** return a count. The count a writer works
against must be the one it read (`page_view(id).count`, an acquire load), not a second one
sampled a moment later.

**Why an update KIND and not a new event type.** A11 §8.1 offered both ("re-publishing
`PageUpdate{page, 0, count}` or a new `EventType::kPointsRecoloured`"). `PageUpdate` gained
a field instead:

```cpp
enum class PageUpdateKind : uint8_t { kAppended = 0, kRecoloured = 1 };
```

Three reasons. Every existing subscriber keeps compiling and keeps its single code path.
Both kinds carry the same `[first, first+count)` range, so a subscriber that ignores
`kind` entirely *still does the right thing* — it re-uploads the range. And a consumer that
caches geometry separately from colour (A14's territory) can now skip the position upload,
which a second event type would have given it too but at the cost of a second `case` in
every switch that already handles `kPointsAvailable`.

It crosses to C as `scan_event.payload.points.update_kind`.

**One consequence inside the Engine.** `Engine::Impl::on_page_update` forwards
`kLidarMid360` pages to A6's odometry. A recolour republishes a range that already went
through it once, and re-inserting the same geometry would duplicate it in the voxel map, so
the forwarding is now explicitly append-only. Colorization runs on `kSlamMap` in practice,
but "in practice" is not a guarantee and this is one line.

---

## 3. C ABI 3 → 4

`SCAN_ABI_VERSION` and `scanengine::kEngineAbiVersion` moved together (DESIGN §6 item 9).
The `scan_event` union changed layout, so this is a real break for a JNI binary built
against ABI 3 — which is what the load-time version check exists to catch.

| Addition | Mirrors |
| --- | --- |
| `scan_keyframe` + `scan_engine_record_keyframe` | `color::encode_keyframe_record()` + `recorder.write_chunk()` |
| `scan_colorizer` handle: `_create` / `_destroy` / `_load_keyframes` / `_set_extrinsics` / `_set_progress_callback` / `_run` / `_cancel` / `_progress` / `_stats` | `color::PointColorizer` |
| `scan_colorize_config`, `scan_colorize_stats` | `color::ColorizeConfig`, `color::ColorizeStats` |
| `scan_clock_sweep_estimate` + `scan_rate_sample`, `scan_clock_sweep_result` | `color::estimate_clock_offset()` |
| `SCAN_SYNC_*` (4), `SCAN_COVERAGE_*` (3), `SCAN_KEYFRAME_POSE_*` (2), `SCAN_KEYFRAME_*` flags (4), `SCAN_SWEEP_*` (8) | `SyncQuality`, `ColorCoverage`, `KeyframePoseFrame`, the frames.idx flags, `ClockSweepVerdict` |
| `SCAN_JOB_*` (5) | `jobs::JobState` |
| `SCAN_PAGE_UPDATE_*` (2) | `PageUpdateKind` |
| `scan_event.payload.job` | `JobProgressPayload` |
| `scan_event.payload.points.update_kind` | `PointsAvailablePayload::update_kind` |

Every new enum value is `static_assert`ed against its C++ original. **`SyncQuality` earns
that guard the way `FixType` did in INT-29:** `policy_for()` switches on it and its
*default* (`kUnknown`) is the one that refuses, so a value drifting by one would turn
"not converged, do not colorize" into "colorize" — and the failure is a silently
mis-registered cloud, not a crash.

Five contract decisions, all in the header:

1. **`scan_engine_record_keyframe()` goes through the Engine, not the recorder.** A11 §3.1
   is right that the capture side needs no new writer — it is `encode_keyframe_record()` +
   `write_chunk()` against a `FileRecordWriter` the app already holds. What the app cannot
   do for itself is take the **lock**: `FileRecordWriter` is not internally synchronized
   (`record/` owns no thread), and since the Mid-360 raw shim landed, the D6 serial thread,
   the Mid-360 receive thread and the NTRIP thread can all be recording concurrently. B8's
   CameraX callback is a fourth. So `Engine::record_keyframe()` (new, C++ too) takes the
   same `record_m` every other producer takes, and the C entry point is a field-by-field
   conversion in front of it.
2. **A keyframe is validated before it is written.** Non-unit quaternion, principal point
   outside the image, an absolute or `..`-bearing image name — all `kInvalidArgument`, and
   nothing reaches the disk. A reader's only option for a bad record is to skip it
   silently, which is worse than a refusal the caller can see. (The image-name check is the
   same zip-slip class `zip_import()` defends against; an index is just as
   attacker-supplied as an archive.)
3. **`scan_colorize_config` exposes the choices and not the constants.** `sync_quality`,
   the two motion thresholds, the clock offset, the pose frame, the range window and the
   four booleans. It does *not* expose the 75° incidence cut, the 1/8 depth scale, the
   slope bias or the scoring weights: those are S6-derived measurements, not per-session
   decisions, and putting them in the ABI invites a JNI caller to tune away a measured
   result. **A zero-initialized struct is safe and refuses** — `sync_quality == 0` is
   `SCAN_SYNC_UNKNOWN` — which is the single most important property of the whole surface.
   `min_range_m`/`max_range_m` treat 0 as "keep the default" for the same reason: a
   memset'd config must not silently become "colour nothing".
4. **`scan_colorizer_run()` is blocking, on the caller's thread**, and there is
   **deliberately no C surface for A15's job queue at ABI 4**. An app that wants a queue
   drives `Engine::jobs()` in C++; a JNI consumer subscribes to `kJobProgress`. Adding
   `scan_job_*` before a real JNI consumer exists would fix the wrong half of the API
   (`JobSpec` carries a `Colorizer*`, an `HttpTransport*` and a `PageStore` — none of which
   have C representations yet).
5. **A refused sweep is `SCAN_OK` with `accepted == 0` and a verdict.** "The user did not
   sweep" is something the wizard must *say*; only structurally bad input (unsorted, empty,
   non-finite) is an error. Same call as INT-29's `scan_engine_pose_at()` gate: the
   interesting answer is not the status code.

`tests/capi_smoke.c` went 102 → **127 checked steps**, still C11.

---

## 4. The JobRunner adapter (`jobs/job_runner_adapter.h`)

A15 §7.3 explained exactly why it left this undone, and the explanation is the design:
`JobRequest{mode, lscan_dir, output_dir, pipeline}` cannot express `chain_from`, a
priority, an `ExportFormat`, a `Colorizer*` or a `CloudSubmitConfig`, so a shim inside
`jobs/` would have been *lossy*. What was missing was not the translation but **a place to
put the choices `JobRequest` has no room for**. `JobRunnerOptions` is that place: the app
configures it once — it already knows its format, its transport and its colorizer — and
from then on the UI submits plain `JobRequest`s.

```
mode = kLocal, pipeline ""/"post"  →  kPostProcess
mode = kLocal, pipeline "colorize" →  kPostProcess → kColorize      (chained)
mode = kLocal, pipeline "export"   →  kPostProcess → kExportPoints  (chained)
mode = kLocal, pipeline "plan"/"merge" → kUnimplemented
mode = kExtractForTransfer         →  kTransferExport
mode = kCloud                      →  kTransferExport → kCloudSubmit (chained)
```

**The chains are the point.** A `JobRequest` names a *directory*, never a `PageStore`, so
"colorize this session" and "export this session" are not single jobs — a `.lscan` has to
become a `PageStore` first. `submit()` returns the tail's id, and `status()` reports the
whole chain as ONE job with ONE monotone progress
(`(finished stages + current.progress) / stages`), which is what a UI wanted from
`JobRequest` in the first place. A failure anywhere is reported with its **cause**, not
with the `kInvalidState` a downstream chained job produces when its source did not finish.

Three details worth stating:

* **A half-built chain is unwound.** If the second spec is rejected (no colorizer, no
  transport, no `output_dir`), the first is cancelled before returning. Otherwise the head
  would run, produce a store nobody consumes, and report `kDone` for a request that never
  completed.
* **`cancel()` cancels the whole chain, tail first.** Cancelling the head first can let the
  queue start the tail before the tail's own cancel lands, and the tail would then fail
  with `kInvalidState` instead of `kCancelled`.
* **The two `JobState` enums come back apart here.** A15 has five states and folds
  cancellation into `kFailed` + `ScanError::kCancelled` (its §2 explains why). A1's seam
  enum has seven and keeps them separate. The adapter maps `kFailed`+`kCancelled` →
  `kCancelled`, and recovers `kUploading`/`kDownloading` — which A15 deliberately does not
  have — from a `kCloudSubmit` job's own stage label. A UI that shows "failed" for a
  user-pressed cancel is a bug report waiting to happen.

`tests/test_job_runner.cpp` is a **separate file** for a concrete reason, not tidiness:
`tests/test_jobs.cpp` opens both `scanengine` and `scanengine::jobs` with using-directives,
so every unqualified `JobState` in it means A15's — and including `jobs/job.h` there makes
hundreds of pre-existing lines ambiguous. In the new file the seam enum is the unqualified
one, which is also the right emphasis for a file about translating between them.

---

## 5. Colorization wiring, and the abstract seam

### 5.1 A15 §7.6 — cancel and progress on `Colorizer` itself

`jobs/local_runner.cpp` used to `dynamic_cast` to `color::PointColorizer` to get
cancellation and fine-grained progress, and **any other `Colorizer` had a real gap**: a
blocking `colorize()` that could not be interrupted and exactly two progress ticks. Two
methods on the interface close it:

```cpp
virtual void set_cancel_token(post::CancelToken* token) { (void)token; }
virtual void set_progress_fn(ColorizeProgressFn cb)     { (void)cb; }
```

Both **additive and defaulted to no-ops**, which is what makes this safe: every existing
implementation and every test double compiles unchanged and behaves exactly as before. An
implementation that overrides them gets real cancellation for free, with no cast and no
second vocabulary. `PointColorizer::set_cancel_token()` already had that exact signature,
so it simply became an `override`; `set_progress_fn()` is the coarse form of its richer
`set_progress_callback(ColorProgressFn)` and is implemented in terms of it.

The token is A7's `post::CancelToken` rather than a colorization-specific one for the
reason A11 §5.7 already gave: A15 owns one token per job and hands out a pointer.

**One `dynamic_cast` survives in `run_colorize()`, and it is a convenience rather than a
capability**: `PointColorizer` knows how to load its own keyframes (and install a
`FileImageSource`) from a `.lscan` directory. The abstract seam has no such notion — a
second implementation would source keyframes its own way — so making it a virtual would
add a method nobody else could answer.

### 5.2 A11 §8.3 — `jobs/colorize_wiring.h`

A11's decoupling is right: the sync gate arrives as a value, the motion gate and the
trajectory as callables. But that left four connections that "are one line each and neither
is wired by anything today". They now live in one place instead of being retyped in the
CLI, in B6's Android service and in the Qt panel:

```cpp
cfg.sync_quality           ← TimeSync::quality(stream)          (A4)
cfg.camera_clock_offset_ns ← the sweep / the manifest           (A11)
set_angular_rate_fn(...)   ← ImuIngest::angular_rate_at(t, w)   (A4)
set_pose_fn(...)           ← a PoseInterpolator                 (A7)
```

**Failing closed is preserved on purpose.** A null `timesync` leaves `sync_quality` at
whatever the base config says — which defaults to `kUnknown`, which refuses. A wiring
helper that quietly assumed `kGood` would undo the one property A11 §2 built the module
around.

**The pose function accepts a FLAGGED pose** (`has_pose`, not `ok()`). A flagged sample is
still real geometry, and refusing it would drop the rolling-shutter correction back to the
constant-velocity fallback for exactly the frames captured while the tracker was
struggling — the frames that need it most. Tracking-lost *keyframes* are still refused
outright by the colorizer itself; this is a per-*row* pose ~20 ms inside an
already-accepted keyframe.

`tests/test_jobs.cpp` asserts the gate end to end: a keyframe recorded at 0.05 rad/s, an
`ImuIngest` reporting 3 rad/s at that instant, and `keyframes_rejected_motion == 1` — with
the un-wired run as the control that proves it was the IMU doing it.

---

## 6. `engine_cli --post`, and the Engine's job queue

`Engine::jobs()` returns the one `JobQueue` an app should drive, **created lazily** and
constructed with the Engine's own `EventBus` (so `kJobProgress` lands on the same
subscription as everything else). Lazy matters: the queue starts a worker thread in its
constructor, and the overwhelming majority of `Engine` instances — every unit test, every
live capture that never processes anything — must not pay for one. `~Engine` destroys it
**first**, before the recorder, the page store and the bus, because a running job touches
all three.

```
engine_cli --post <lscan-dir> [--out <dir>] [--no-loops] [--no-outlier] [--dedup <m>] [--quiet]
engine_cli --synth-lscan <dir> [seconds]
engine_cli --post-selftest [--quiet]

exit: 0 ok · 1 failed · 2 usage · 3 cancelled
```

`--post` is workstream D1's entry point and it is deliberately **not** a special code path:
it builds a `QueueJobRunner` over `Engine::jobs()` and submits the same `JobRequest` B6's
mode chooser would (`pipeline = "export"` with an `--out`, `"post"` without). If the cloud
worker did not run the same path a phone runs, "one pipeline in three places" (§3.8) would
be a slogan. Progress goes to **stderr**, results to **stdout** — a worker's stdout is its
product and has to stay machine-readable when it is piped.

`--synth-lscan` writes the same tiny stationary Mid-360 capture `tests/test_jobs.cpp`
builds, so `--post` has something to chew on with no rig. `--post-selftest` chains the two
in a temp directory, asserts a real non-trivial `.ply` came out, and removes it; that is
the new `engine_cli_post` ctest, which is why no binary fixture is committed.

`--no-outlier` is not a test hack: A7's statistical outlier filter is tuned against real
point density and the synthetic cloud (96 identical directions on a 3 m shell) is far too
sparse for its k-NN distance distribution to mean anything — it deletes everything. That is
the same reason `tests/test_jobs.cpp` turns it off, and a worker given a short stationary
capture wants the same switch.

---

## 7. `record/` — the zip hooks, and the one manifest exception

**`zip_export()` / `zip_import()` gained an optional progress callback and cancel token**
(A15 §7.4). Both parameters are defaulted, so every existing call site is unchanged. The
token mirrors A9's `ExportCancelToken` in shape — a poll-based flag, cheap in a tight copy
loop and settable from another thread — but is a *separate type* (`lscan::ZipCancelToken`)
so `record/` keeps depending on nothing but `core/`, which is the module boundary DESIGN §6
item 3 asks for.

The export's denominator comes from directory metadata (`fs::file_size`), not a read pass:
a progress bar must not cost a third traversal of a multi-gigabyte capture. The import's
comes from a pre-pass over the central directory only (46 bytes + a name per entry).
**Cancelling an export REMOVES the half-written archive** — a partial zip looks openable and
is not, because its central directory never gets written — while **cancelling an import
leaves what it extracted**, because `dest_dir` is the caller's directory and may have had
contents before the call. `jobs/transfer.cpp` now passes both through, which is what closes
the gap `jobs/transfer.h` used to document rather than work around.

### The one exception in A5's file

`FileRecordWriter::add_clock_offset(bracket, camera_to_engine_ns, sigma_ns)` and the
matching key in `write_manifest()`:

```json
"clockOffsets": {"bracket-a": {"cameraToEngineNs": 38000000, "sigmaNs": 205000}}
```

**This is the only edit INT-34 made in `record/lscan.{h,cpp}`, it is purely additive, and
it is called out in the header itself so A5 can reconcile it.** No format-version bump, no
behaviour change for a caller that never calls it, and the key is *always* emitted (`{}`
when empty) so a consumer can rely on it from day one exactly as it can on
`"mountCalibration"` and `"crs"`.

Why it had to exist somewhere: A11 both **produces** the offset
(`ClockSweepResult::offset_ns`) and **consumes** it
(`ColorizeConfig::camera_clock_offset_ns`), and nothing persisted it — so a session
re-opened on a desktop or in the cloud worker had to re-run an 8-second sweep it can no
longer capture. WIZARD §3 keys it per mount bracket, so the setter does too; re-setting a
bracket replaces its entry. The sign convention is `clock_sweep.h`'s, restated in the
header: `t_engine_ns = t_camera_ns + cameraToEngineNs`.

---

## 8. Verification

macOS 15 (Darwin 25.5.0), Apple silicon, AppleClang, Ninja, CMake 4.4.2, from a **deleted
build directory**, `-DENGINE_WARNINGS_AS_ERRORS=ON`. Zero warnings from engine code.

```
$ cmake -S engine -B build -G Ninja -DENGINE_WARNINGS_AS_ERRORS=ON
$ cmake --build build

$ ctest -LE 'sim|sim-rtk'
  1/5 scanengine_tests ....... Passed  25.91 sec
  2/5 scanengine_capi_smoke .. Passed
  3/5 engine_cli_selftest .... Passed
  4/5 engine_cli_version ..... Passed
  5/5 engine_cli_post ........ Passed          <- new (§6)
  100% tests passed out of 5

$ ./scanengine_tests
[doctest] test cases:     480 |     480 passed | 0 failed | 7 skipped
[doctest] assertions: 2279642 | 2279642 passed | 0 failed |
```

**On the concurrent wave.** This ran with A13 editing `merge/` at the same time, and
`src/merge/icp.cpp` + `tests/test_merge.cpp` appeared in the tree mid-run. At one
intermediate point 2 assertions in `merge/yaw_search_places_a_manhattan_session` were
failing — in files INT-34 may not edit and does not touch — and the run above was repeated
from a deleted build directory afterwards, at which point A13's work had settled and the
whole suite including `merge/*` is green. No cases were excluded from the numbers above.

New cases, by file:

| File | Added |
| --- | --- |
| `tests/test_cloud.cpp` | 2 — `page_data_mutable()` identity/immutability of geometry, and the recolour notification's kind, ranges and refusals |
| `tests/test_engine.cpp` | 3 — keyframes recorded through the Engine read back as `frames.idx` (including both refusals); the manifest's per-bracket clock offset (including "replaces, does not append" and the always-present `{}`); the lazily-owned job queue publishing `kJobProgress` on the Engine's own bus |
| `tests/test_capi.cpp` | 5 — null-handle rejection across the ABI-4 surface; keyframes over the ABI into `frames.idx`; the colorizer handle refusing an unsynchronised session **before any work** and then running, with the C progress trampoline and the `SCAN_PAGE_UPDATE_RECOLOURED` event; the wizard's sweep (23.0 ms truth recovered to **22.96 ms**) including a `NO_MOTION` verdict; the `kJobProgress` union case |
| `tests/test_jobs.cpp` | 3 — the abstract seam's cancel/progress on both a plain and an overriding `Colorizer`; zip progress + cancel (pre-, mid-file, and import-side); `colorize_wiring` with the IMU outranking the recorded rate and the un-wired control |
| `tests/test_job_runner.cpp` | 2 (new file) — the request → chain translation, monotone chained progress, `kUnimplemented` for a pipeline with no job kind, the unwind of a half-built chain, and cancellation reporting A1's `kCancelled` rather than A15's `kFailed` |
| `tests/test_headers.cpp` | 1 — A11 §8.5's four `color/` headers plus `record/zip.h` and INT-34's two `jobs/` headers, self-contained and instantiated |
| `tests/capi_smoke.c` | steps 103–127, in actual C11 |

---

## 9. Left undone, and for whom

1. **A8 §6's `ExtrinsicsSolver` wrapper** (A11 §8.3's third bullet) is still a seam, and
   deliberately: it turns a keyframe's checkerboard detection into the plane `(n, d)` A8's
   `MountCalibrationSolver` wants, and the *detection* lives on the app side where the
   image already is. A11 §8.3 says it is worth writing once B7 exists to feed it, and
   nothing here changes that.
2. **No C surface for the job queue** (§3, decision 4). `JobSpec` carries a `Colorizer*`,
   an `HttpTransport*` and a `PageStore` — three things with no C representation — so the
   honest first step is a `scan_job_submit_post(engine, lscan_dir, out_dir)` narrow enough
   to be complete, added when B6 needs it rather than guessed at now.
3. **`scan_colorizer_run()` takes a `scan_engine*`, so it can only colour the ENGINE's
   page store.** A cloud worker colouring a post-processed store has to go through C++.
   The C fix is a `scan_page_store` handle, which is a bigger decision than this task
   (it would also give A9's export a C surface, which nothing has asked for).
4. **The colorizer's progress callback is not called from `scan_colorizer_run()` on the
   caller's behalf between keyframes if the run refuses.** That is correct — a refusal is
   before any work — but a UI that shows a spinner has to key it off the return, not off a
   progress tick that never arrives.
5. **`ImuIngest` is reachable from the Engine (`Engine::imu()`) but `colorize_wiring` is
   not called by anything inside `core/`.** That is deliberate: the Engine does not decide
   to colorize, an app or the CLI does. When B6/C-workstream builds its processing screen,
   `colorize_config_from()` + `wire_colorizer()` + a `kColorize` job is the whole sequence.
6. **`--post` does not colorize.** `QueueJobRunner`'s `"colorize"` pipeline exists and is
   tested, but the CLI has no `--colorize` flag because a cloud worker colouring a session
   needs the keyframe JPEGs in the uploaded bundle and a decision about `sync_quality` that
   only the capture side can make. One flag plus a `--sync-quality` argument, once D1 has
   a bundle format opinion.
