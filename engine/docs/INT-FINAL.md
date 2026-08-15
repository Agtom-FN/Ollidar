# INT-FINAL — the Phase-1 integration sweep

**Scope:** `engine/**` only. Additive everywhere it could be; the one place it
could not be is `scan_device_config`, whose layout changed, which is what makes
this **ABI 4 → 5**.

**Sources this task implements, rather than re-derives:**

| Source | What it asked for |
| --- | --- |
| `android/NOTES.md` §8 findings 1, 2, 4 | the Android capture seam through the C ABI, a second pre-bound descriptor, `sdk_config_path` |
| `android/NOTES.md` §8 finding 5 · `desktop/NOTES.md` §8.3 | `Mid360Stats` unreachable from an app |
| `desktop/NOTES.md` §11.8 | "'Remove selected' is a message box … a future desktop pass needs an engine-side seam" |
| `docs/INT29-wiring.md` §7 item 5 | "a C consumer needing a national grid will want a `scan_engine_set_crs()`" |
| `docs/A8-pushbroom.md` §6 · `docs/A11-color.md` §8.3 · `docs/INT34-wiring.md` §9 item 1 | wrap `MountCalibrationSolver` behind `ExtrinsicsSolver` |
| `docs/INT34-wiring.md` §9 item 6 | "`--post` does not colorize … one flag plus a `--sync-quality` argument" |

---

## 1. ABI 4 → 5: the whole new surface

`SCAN_ABI_VERSION` and `scanengine::kEngineAbiVersion` moved together, as
DESIGN §6 item 9 requires. **Every ABI-4 function signature, struct layout and
enum value is unchanged.** One struct grew (`scan_device_config`), two structs
are new, three functions are new, two enum families are new.

### 1.1 `scan_device_config`, the Mid-360 half

Before ABI 5 this was `lidar_ip` + `host_ip` and nothing else, which is why
android/NOTES §8 finding 1 called it "the one worth fixing first": the field
`transport/udp_source.h` documents **as** the Android seam was unreachable, and
so were the backend selector, all ten ports, `recv_buffer_bytes`, the filter
and `live_points_per_sec`. Added:

```c
int32_t  mid360_backend;              /* SCAN_MID360_BACKEND_{SDK2,RAW_UDP,INJECT} */
int32_t  mid360_prebound_fd;          /* the points socket   — the Android seam */
int32_t  mid360_prebound_imu_fd;      /* the IMU socket      — finding 2        */
uint16_t mid360_{point,imu,cmd,push,log}_port;
uint16_t mid360_host_{cmd,push,point,imu,log}_port;
int32_t  mid360_recv_buffer_bytes;
uint8_t  mid360_live_points_per_sec_set;  uint32_t mid360_live_points_per_sec;
uint8_t  mid360_publish_imu_set;          uint8_t  mid360_publish_imu;
uint8_t  mid360_filter_set;   /* + drop_no_return, tag_reject_mask,
                                 min_reflectivity, min_range_m, max_range_m */
uint8_t  mid360_verify_crc_set;           uint8_t  mid360_verify_crc;
const char* mid360_sdk_config_path;   /* finding 4 */
```

**`memset(0)` is exactly the ABI-4 device** — SDK2, engine-created sockets, the
A3 default ports, the A3 default filter, 40k pts/s of decimation. That is the
property the whole design is arranged around, and it is asserted in both
`intfinal/abi5_device_config_is_additive_and_zero_means_ABI_4` and
`capi_smoke.c` step 132. Three fields carry an explicit `_set` flag rather than
using 0-as-default, because for them **0 is a value a caller may mean**:
`live_points_per_sec` (0 = no decimation, which is what a replay wants),
`publish_imu` (0 = point-only) and `verify_crc`. This is the convention
`scan_ntrip_config::allow_v1_fallback_set` already established.

Two things are refused at `scan_engine_add_device()` rather than at `start()`,
because add_device is where the app can still do something about it:

* a `mid360_backend` outside 0..2 (it is cast to a C++ `enum class`, so a
  blind cast would select a backend nobody asked for);
* a pre-bound descriptor with `SCAN_MID360_BACKEND_SDK2`. SDK2 creates its own
  sockets inside `util::CreateSocket` in the vendored SDK (android/NOTES §8
  finding 3), so a descriptor bound to a `Network` there would be silently
  ignored — which on a bench reads as "the seam does not work".

### 1.2 `scan_mid360_stats` + `scan_engine_mid360_stats()`

Finding 5, and `desktop/NOTES.md` §8.3's identical complaint: `Engine` exposed
no concrete-driver accessor, so link state, watchdog trips, **forced re-inits**,
window/lifetime loss and the device SN were unreachable from an app *even in
C++*. Both halves now exist:

```cpp
Result<Mid360Stats> Engine::mid360_stats(DeviceId id) const;   // C++ (desktop)
```
```c
scan_error_t scan_engine_mid360_stats(scan_engine*, uint32_t, scan_mid360_stats*);
```

`dynamic_cast` inside the Engine rather than a new virtual on `Driver`: these
counters are the Mid-360's own vocabulary (a forced SDK re-init has no D6
analogue), and a virtual returning a per-driver struct would be a `Driver`
interface that knows about every driver. A non-Mid-360 device is
`kInvalidArgument`, not a zeroed struct. `SCAN_MID360_LINK_*` mirrors
`Mid360LinkState` with the usual `static_assert` guard.

The header states the thing a gauge gets wrong: `loss_pct_window` and
`loss_pct_total` are different numbers, and only the first snaps back after a
transient burst.

### 1.3 `scan_engine_set_crs()`

```c
scan_error_t scan_engine_set_crs(scan_engine*, const char* epsg, const char* wkt);
```
```cpp
Status      Engine::set_crs(const std::string& epsg, const std::string& wkt);
std::string Engine::configured_crs_epsg() const;
std::string Engine::configured_crs_wkt() const;
```

`crs::wkt1_for_epsg()` knows WGS 84 and the two UTM ranges and nothing else —
deliberately, because the engine ships no PROJ and no `proj.db` (`gnss/crs.h`'s
header carries that trade). So a national grid (HK1980 EPSG:2326, OSGB36
EPSG:27700, RD New EPSG:28992) is only expressible if the caller supplies the
WKT its own geodetic authority publishes. Both strings are copied.

**Validated, and the validation is the feature.** The engine cannot check that
a WKT describes the grid it claims to; what it can refuse is the class of
mistake that actually happens across an FFI, since a LAS writer embeds this
string verbatim:

* an EPSG string that does not parse (`EPSG:<n>` and a bare `<n>` both parse
  and are normalised to `EPSG:<n>`, so a consumer never sees two spellings);
* a WKT that is not one — a PROJ.4 string, a bare code, a JSON blob (checked
  against the OGC top-level keywords, WKT1 and WKT2 spellings both);
* a truncated paste (brackets/quotes must balance outside quotes) or one with
  no quoted CRS name;
* **an EPSG the engine cannot render, supplied with no WKT.** That combination
  silently produces an export with an empty CRS field, which looks exactly like
  a local-frame cloud — the failure this call exists to prevent. A `wkt` with
  no `epsg` is fine: some grids have no code.

**It does not defeat the convergence gate.** `crs_wkt()`/`crs_epsg()` stay empty
until the georef transform converges, override or not. An override decides
*what* label a georeferenced cloud gets, never *whether* an ungeoreferenced one
may be labelled — INT-29 §2.1's reasoning is unchanged, and tagging a
local-frame cloud with a national grid is exactly as wrong as tagging it with a
UTM zone. `configured_crs_*()` is the ungated read, so a UI can still show what
the operator picked.

---

## 2. `UdpConfig::prebound_imu_fd` — the raw backend is no longer point-only

android/NOTES §8 finding 2, verbatim: "`RawUdpBackend::open()` copies the whole
`UdpConfig` into both its point and its IMU `UdpSource`, so a single pre-bound
descriptor would be `recvfrom`'d by two receive threads that steal each other's
datagrams. B3's pre-bound path therefore runs point-only."

`UdpConfig` gained one field. `UdpSource` does **not** read it — a source owns
exactly one socket, and muddying that rule would be worse than the bug —
`RawUdpBackend` hands `prebound_fd` to the point source and `prebound_imu_fd`
to the IMU source, clearing the other on each. Never-close already held for one
descriptor and now holds for both: whichever source receives a pre-bound fd
leaves it open at `stop()`, because the app closes it *after* the engine tears
down (no receive thread may still be inside `recvfrom`).

The ambiguous case — a pre-bound point fd, `publish_imu` on, no IMU fd — is
**refused** (`kInvalidArgument`, naming the field) rather than quietly creating
an ordinary socket for the IMU. On Android that socket is not bound to the
USB-Ethernet `Network` and receives nothing, which is indistinguishable from a
device that is not sending IMU. A deliberately point-only capture sets
`publish_imu = false` and is still allowed.

Tested on **real loopback sockets** (`intfinal/prebound_*`), not by inspection:
two sockets bound to `127.0.0.1:0` (ephemeral, so nothing can collide with the
`sim`-labelled tests or a second build tree), four point datagrams and four IMU
datagrams sent, both counted, zero bad packets — and both descriptors still
valid (`fcntl(F_GETFD)`) after the driver has stopped.

---

## 3. `MergeProject::remove_session()`

desktop/NOTES §11.8: "'Remove selected' is a message box, not a real removal …
sessions can only grow. Told to the operator rather than faked."

```cpp
Status MergeProject::remove_session(std::uint32_t id);
```

Three consequences, all stated in the header rather than discovered:

* **Ids renumber.** `session(id)` is a direct index and `MergeSession::id` *is*
  that index. The alternative — leaving a hole — makes `session(id)` return a
  tombstone every caller has to test for and `sessions()` a vector with gaps,
  which is a worse contract than "re-read the list after a removal".
  `provenance_id` does not change, so a UI keying its rows on that (and
  re-reading `find()`) needs no remapping at all, which is the recommended way
  to hold a reference across a removal.
* **The pair report is cleared, not left stale.** Every `MergePair` names
  sessions by id, and after a removal at least one of those ids means a
  different session. `refine()`/`survey_overlap()` rebuild it from scratch,
  which they already documented doing.
* **The anchor survives.** Removing a non-anchor session touches no alignment.
  Removing the anchor moves it to what is then session 0 and rebases every
  remaining alignment onto it (`set_anchor()`'s existing behaviour), so the
  relative geometry the operator established is preserved and only the origin
  moves — asserted numerically in `intfinal/merge_remove_session_*`.

There is no `Engine::remove_session`: `Engine` has `start_session`/
`stop_session` and owns one session at a time. The desktop gap was
`MergeProject`'s, and that is the one closed.

---

## 4. `color::MountExtrinsicsSolver` — the `ExtrinsicsSolver` seam, filled

`color/colorize.h` declared `ExtrinsicsSolver` and A8 §6 said what it must be:
"wrap, do not reimplement … turn the keyframe's checkerboard detection into
`(n, d)` and delegate to `MountCalibrationSolver`". INT-34 §9 item 1 left it as
a seam because nothing produced detections yet. B7 now does
(`calib/CheckerboardDetector.kt`, `calib/TargetPlane.kt`,
`calib/BoardSegmentation.kt`), so only the adapter was missing.

New: `include/scanengine/color/extrinsics_solver.h`,
`src/color/extrinsics_solver.cpp`.

```cpp
struct BoardDetection { int64_t t_engine_ns; double normal[3], d, sigma_m; ... };

class MountExtrinsicsSolver final : public ExtrinsicsSolver {
  Status add_detection(const BoardDetection&);                       // B7's half
  Status add_observation(const Keyframe&, Span<const PointVertex>) override;
  Status add_observation(const Keyframe&, const BoardDetection&,
                         Span<const PointVertex>);                   // pre-paired
  Status solve(double camera_from_lidar[16]) override;
  float  split_half_agreement_px() const override;
  double split_half_mm_at(double range_m) const;                     // WIZARD units
  const MountCalibResult& result() const;  CalibGate gate() const;   // ...
};
```

Given that both observation types already existed, what the adapter actually
adds is three things:

1. **It pairs the two halves by time.** The camera half (a plane) and the lidar
   half (segmented returns) come from different subsystems on different
   threads; the keyframe is what ties them to one instant. An unmatched
   keyframe is `kNotFound`, never a silently dropped pose — "the detector found
   no board in this frame" is something the wizard has to *say*, or the
   operator watches a shot counter that does not move.
2. **It measures the gate in the right camera's pixels.** `MountCalibConfig`
   defaults to S6's model camera (4032×3024, fx 2912), and the split-half gate
   is quoted in **pixels at a range** — so evaluating it against a model camera
   when the keyframes came from another one silently rescales the verdict. The
   solver takes `CalibCamera` from the first keyframe's own intrinsics and
   **refuses a later keyframe from a visibly different camera**: one
   calibration, one camera.
3. **It reports physical units.** WIZARD.md is explicit ("±5 mm at 3 m — Good",
   never pixels); `split_half_mm_at()` is A8 §6's `px · range / fx` so the
   wizard does not re-derive fx.

What it does not do: detect a checkerboard or solve a homography. A8 §4.1's
whole point is that detection lives app-side where the image is.

**The keyframe's POSE is not used at all** — the point-on-plane residual is
between two sensor frames at one instant. A wizard shot taken during ARCore
tracking loss is still a perfectly good calibration observation, and
`extrinsics/the_keyframe_pose_is_not_used_*` asserts the two solves are
bit-identical, because the opposite assumption would quietly throw away half a
bench capture.

**Verified against synthetic detections, not synthetic images** — the wrapper's
contract begins where B7's detector ends. From a known mount and 12
low-discrepancy board configurations at 20 mm range noise, the wrapper recovers
the mount to **0.268° / 1.31 mm** (A8 §5.1's own solver table gives 0.172° /
2.4 mm as the median over 21 sessions at N = 12, so this is the right order).
The bound the test asserts is deliberately loose: what it is really testing is
that the adapter put the plane in the *right frame*, and a transposed rotation
or a sign error in the camera→lidar plane transform lands tens of degrees away,
not fractions of one.

---

## 5. `engine_cli --post --colorize`

INT-34 §9 item 6 named exactly two blockers: "a cloud worker colouring a
session needs the keyframe JPEGs in the uploaded bundle and a decision about
`sync_quality` that only the capture side can make. One flag plus a
`--sync-quality` argument, once D1 has a bundle format opinion."

```
engine_cli --post <lscan-dir> [--out <dir>] ...
           [--colorize --sync-quality good|gated|poor
            [--allow-poor-sync] [--clock-offset <ns>]]
engine_cli --synth-lscan <dir> [seconds] [--frames N]
```

* **The bundle half** is a property of what the worker is handed: `frames/` is
  either in the `.lscan` or it is not, and its absence is Tech Spec §3.5's
  "gracefully unavailable" — the Colorize job already returns `kOk` for it, and
  the CLI now says so instead of printing a row of zeros that reads like a bug.
* **The sync half FAILS CLOSED.** `--sync-quality` is **mandatory** with
  `--colorize` and has no default. Omitting it is a usage error naming the flag
  and the reason; `--sync-quality poor` additionally needs `--allow-poor-sync`.
  The engine refuses too — the CLI check only moves the same answer to where
  the operator can act on it, and the selftest exercises the *engine's* refusal
  rather than the CLI's (below).
* `timesync` is deliberately left null in the `ColorizeWiring`: this process
  never captured anything, so its `TimeSync` would report `kUnknown` for a
  session whose capture side had converged. Everything else goes through
  `jobs/colorize_wiring.h` so the CLI is not a second place that decides how A4
  feeds A11.

**One adapter change made this possible**: `QueueJobRunner` gained a
`"colorize-export"` pipeline (`kPostProcess → kColorize → kExportPoints`).
Two separate requests cannot express it — chaining is by job id, so the second
request would re-run the post pipeline over the same `.lscan`. `"colorize"` and
`"export"` are untouched; the new string is purely additive.

**`--post-selftest` now has a colorize leg**, and it is a refuse-then-run pair:

1. `--synth-lscan` writes **6 synthetic keyframes** — real PNGs plus real
   `frames.idx` records through A11's own `KeyframeIndexWriter`, so the real
   format is exercised and no binary fixture is committed. (PNG rather than
   JPEG because a PNG can be produced exactly in ~40 lines: deflate has a
   STORED block type. The vendored `stb_image` decodes both.)
2. a colorize with `sync_quality = unknown` must **fail** — that is the gate
   failing closed, and it is the assertion that matters most here;
3. a colorize with `sync_quality = good` must colour points: the run reports
   **6/6 keyframes used, 20/96 points coloured** on the synthetic 3 m shell,
   and the selftest fails if either is zero.

---

## 6. Findings and seams: closed vs deferred

### Closed

| Source | Item | Where |
| --- | --- | --- |
| android §8 #1 | `scan_device_config` cannot carry the Android seam | §1.1 |
| android §8 #2 | one fd, two sockets | §2 |
| android §8 #4 | `sdk_config_path` not exposed through the C ABI | §1.1 |
| android §8 #5 | `Mid360Stats` unreachable from an app | §1.2 |
| desktop §11.8 | no `MergeProject::remove_session()` | §3 |
| INT-29 §7 #5 | no `scan_engine_set_crs()` | §1.3 |
| A8 §6 / A11 §8.3 / INT-34 §9 #1 | `ExtrinsicsSolver` unimplemented | §4 |
| INT-34 §9 #6 | `--post` does not colorize | §5 |

Verified already closed by earlier waves while sweeping, so no work was needed:
**android §8 #6** (`SCAN_EVENT_DEVICE_HEALTH`'s payload — INT-29 added the
union case and `convert_event()` handles `kDeviceHealth`; health is pushed, not
only polled), **A4 §8 #1** (`timesync.set_event_bus()` is wired in
`Engine::create`), **A6 §9 #1** (`StreamId::kSlamMap`/`kPoseLio` exist), **A6
§9 #3** (`slam/slam.h`'s duplicate `ImuSample` is gone), **A8 §7.1/§7.2** and
**A11 §8.1/§8.2** (INT-24/INT-34).

### Deferred, with reasons

1. **android §8 #3 — SDK2's sockets cannot be bound to a `Network`
   per-socket.** Every route out is an *Android* change, not an engine one:
   `bindProcessToNetwork` / `android_setprocnetwork()` are process-wide (they
   would push NTRIP and Play traffic onto a link with no internet), and the
   only per-socket answer is a fourth patch to the vendored SDK's
   `util::CreateSocket`, which is an Android feature patch rather than a
   bionic-compat one. The engine's contribution is to make the *alternative*
   real: with ABI 5 the raw-UDP backend is a complete capture path on
   app-supplied sockets, so a device already configured to stream can be
   captured with no SDK sockets at all. Deciding between the two is a bench
   decision, deliberately left to one.
2. **No C surface for the job queue** (INT-34 §9 #2). `JobSpec` carries a
   `Colorizer*`, an `HttpTransport*` and a `PageStore` — three things with no C
   representation. Unchanged from INT-34's reasoning: the honest first step is
   a narrow `scan_job_submit_post()`, added when B6 needs it rather than
   guessed at now. `--post --colorize` is the cloud worker's route today and it
   needs no C ABI at all.
3. **`scan_colorizer_run()` still takes a `scan_engine*`**, so it can only
   colour the Engine's own page store (INT-34 §9 #3). The fix is a
   `scan_page_store` handle, which is a genuinely bigger decision — it would
   also give A9's export a C surface, and nothing has asked for one. The C++
   and CLI paths cover the post-processed store, which is where a worker
   actually colours.
4. **`--merge` in `engine_cli`.** `remove_session()` closes the desktop gap;
   exposing the whole merge workbench headlessly is a CLI design task with no
   caller asking for it.
5. **Two overlapping real captures** (desktop §11.8, A13 §11). Not something an
   engine change can produce.
6. **`ExtrinsicsSolver` has no C mirror.** B7 already solves through
   `scan_mount_calib_*`, which is A8's solver directly and is the same answer;
   the wrapper's value (pairing, the gate camera, millimetres) is for a C++
   caller — the desktop bench harness — and adding a second C path to one
   solver would invite the two to drift.
7. **The colorize CLI does not read the manifest's `clockOffsets`.**
   `--clock-offset` takes it on the command line. Reading it back per bracket
   means the CLI choosing *which* bracket, which is a capture-side fact it does
   not have; the value crosses in the job the worker was given.

---

## 7. Verification

macOS 15 (Darwin 25.5.0), Apple silicon, AppleClang, Ninja, from a **deleted
build directory**, `-DENGINE_WARNINGS_AS_ERRORS=ON`. Zero warnings from engine
code.

```
$ cmake -S engine -B build -G Ninja -DENGINE_WARNINGS_AS_ERRORS=ON && cmake --build build

$ ctest -LE 'sim|sim-rtk'
  1/5 scanengine_tests ....... Passed  22.4 s
  2/5 scanengine_capi_smoke .. Passed
  3/5 engine_cli_selftest .... Passed
  4/5 engine_cli_version ..... Passed
  5/5 engine_cli_post ........ Passed        <- now includes the colorize leg
  100% tests passed out of 5

$ ctest -L sim         # the S2 Mid-360 simulator, built and present
  mid360_sim_e2e ..... Passed  204.2 s
  gnss_rtk_sim_e2e ... Passed    9.3 s
  100% tests passed out of 2

$ ctest -L sim-rtk
  gnss_rtk_sim_e2e ... Passed    9.4 s
  100% tests passed out of 1

$ ./scanengine_tests
  [doctest] test cases:     499 |     499 passed | 0 failed | 7 skipped
  [doctest] assertions: 2280043 | 2280043 passed | 0 failed |
```

The 7 skipped are the `mid360sim/*` and `gnsssim/*` cases carrying
`doctest::skip()`, which the two labelled entries above run with `--no-skip`.
INT-34 recorded 480 cases; the 19 added since are this task's 15 plus 4 from
the A13 wave in between.

Two pinned assertions moved with the version, which is what pinning them is
for: `tests/test_capi.cpp`'s `SCAN_ABI_VERSION == 4u` and
`tests/test_engine.cpp`'s `kEngineAbiVersion == 4`. Nothing else in the
existing suite needed a change — every other addition is additive.

New
test files: `tests/test_int_final.cpp` (10 cases: the CRS surface in C++ and in
C, the ABI-5 device config, `mid360_stats`, the dual-fd loopback pair,
`remove_session`, the colorize-export chain) and
`tests/test_extrinsics_solver.cpp` (5 cases). `tests/capi_smoke.c` gained steps
128–146, in actual C11. `tests/test_headers.cpp` gained the new `color/` header.
