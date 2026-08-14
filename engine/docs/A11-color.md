# A11 — camera colorization

**Scope:** `engine/include/scanengine/color/`, `engine/src/color/`,
`engine/tests/test_color.cpp`.
**Spec:** §3.5 (camera colorization), §3.11 (`.lscan` container).
**Contracts:** `engine/DESIGN.md` §2 (threading), §3 (errors), §5 (PageStore),
§6 (how to add a module).
**Reads:** `spikes/s6-calibration/REPORT.md` §6–§7 and `WIZARD.md` §3–§4 — the
error budget this module is written against; `engine/docs/A4-timesync.md` §3–§4
and §7 (the sync gate, and why a constant offset survives filtering);
`engine/docs/A5-lscan.md` §3 (the `frames.idx` seam this task closes);
`engine/docs/A8-pushbroom.md` §3.1 and §6 (frames, extrinsics, and the
"wrap, do not reimplement" instruction for the wizard solver);
`engine/docs/A7-post.md` (the cancel/progress posture and where the cloud
comes from).

Nothing outside those directories was touched. §8 lists what the orchestrator
has to place in files A11 does not own.

---

## 1. What landed

| File | What |
| --- | --- |
| `color/colorize.h` | The shared vocabulary — `CameraIntrinsics`, `Keyframe`, and A1's two seams. Extended, not rewritten: `Keyframe` gained the capture metadata the S6 mitigations need (per-frame angular rate, exposure, flags). |
| `color/frames_idx.h/.cpp` | **THE `streams/frames/frames.idx` format** (§3): record codec, validation, a standalone writer, and a truncation-tolerant reader. |
| `color/image_source.h/.cpp` | `DecodedImage` (+ bilinear sampler), JPEG/PNG decoding via vendored **stb_image**, the `ImageSource` seam and its file-backed implementation. |
| `color/clock_sweep.h/.cpp` | The wizard's **8-second clock-offset estimator** (S6 §7.1 item 1) and `policy_for(SyncQuality)` — the go/no-go and the motion-gate thresholds. |
| `color/colorizer.h/.cpp` | `PointColorizer`: best-view selection (incidence × distance × motion), z-buffer occlusion, **rolling-shutter projection**, bilinear RGB into `PointVertex`, coverage flags, progress/cancel. |
| `src/color/stb_image.h` | Vendored v2.30, public domain / MIT dual (§4). |
| `tests/test_color.cpp` | 25 cases, 6,862 assertions, **0.10 s**. |

The three mitigations S6 made mandatory are all here and all measured:
the wizard sweep (§7.4), the rolling-shutter per-row model (§7.2), and the
motion gate (§7.3).

---

## 2. Where this runs, and what it refuses to do

Colorization is a **post-processing stage on any platform**, driven from a
`.lscan`. Capture is Android-only (§3.5) but nothing here is: keyframes come
off disk through `frames_idx.h`, pixels through an `ImageSource`, points from
a `PageStore` or a plain span. That is the same posture as A7's post pipeline,
which is where the cloud it colours comes from.

It does **not** decide whether a session may be coloured at all. That is
`policy_for(SyncQuality)`, driven by A4's `TimeSync::quality()`, and the
caller passes the answer in as `ColorizeConfig::sync_quality`.

| `SyncQuality` | S6 evidence | policy |
| --- | --- | --- |
| `kGood` (≤ 5 ms) | 15.3 px at 30 °/s | colorize; gate 30 °/s, reject 90 °/s |
| `kGated` (≤ 15 ms) | 22.1 px ungated → **16.2 px below 15 °/s** | colorize; gate 15 °/s, reject 60 °/s |
| `kPoor` (> 15 ms) | 36 px ungated; **18.7 px below 10 °/s** at 30 ms | **refuse**, unless `allow_poor_sync` — then gate 10 °/s |
| `kUnknown` | not converged | **refuse**, always |

The default of `ColorizeConfig::sync_quality` is `kUnknown`, so a caller who
forgets to wire A4 gets a refusal (`kNotSupported`) rather than a silently
mis-registered cloud. A4 §4 is explicit that jitter is meaningless before
convergence, and §7 that consumers gate on `sync_quality()`, **never on
`jitter_ns`** — neither this module nor its tests ever reads `jitter_ns`.

The refusal happens **before any work**: no image is decoded, no point is
touched (`gate/an_unsynchronised_session_is_refused_before_any_work`).

---

## 3. `frames.idx` — the format B8 writes

### 3.1 It is an ordinary `.lscan` stream file

A1 fixed the framing and the chunk-type number; A5 built the writer and reader
around them and left the payload to A11 (A5 §3). So there is nothing special
about `frames.idx`:

```
streams/frames/frames.idx = [StreamFileHeader 32 B][chunk][chunk]...
chunk  = [payload_len u32][type u16][flags u16][t_mono_ns i64][payload][crc32 u32]
type   = ChunkType::kCameraFrameIndex (7)
stream = StreamId::kCameraFrames (6)     // lscan::stream_file_of() already
                                         // maps it to this path
```

**The capture side therefore needs no new writer.** It already holds an open
`FileRecordWriter`:

```cpp
std::vector<std::uint8_t> rec;
SCAN_TRY(color::encode_keyframe_record(kf, &rec));
SCAN_TRY(recorder.write_chunk(lscan::ChunkType::kCameraFrameIndex, kf.t_mono_ns,
                              ByteSpan(rec.data(), rec.size())));
```

and gets crash safety, the flush policy, the truncated-tail rule and the CRC
for free. `KeyframeIndexWriter` writes the byte-identical thing standalone for
the tools that have no recorder (the desktop importer, tests, offline
re-indexing); `fidx/is_byte_identical_to_what_A5s_recorder_writes` asserts the
two agree byte for byte and that A5's `FileRecordReader` hands the chunks back
as `kCameraFrameIndex` on `StreamId::kCameraFrames`.

Both create the file **lazily, on the first record**, which is what makes the
stream header's `t_start_mono_ns` the first keyframe's stamp in both — and
what makes "a session with no camera has no `frames.idx`" true, so
`read_frame_index()` can return `kNotFound` and mean it (§3.5's "gracefully
unavailable").

### 3.2 The record

Little-endian, packed, no alignment requirement — the same rules as every
other `.lscan` structure. **160 fixed bytes + the image name.**

| off | size | field | notes |
| ---: | ---: | --- | --- |
| 0 | 2 | `record_version` u16 | `kKeyframeRecordVersion` = 1 |
| 2 | 2 | `fixed_bytes` u16 | 160 today; a reader **skips any excess**, so fields may be appended without a version bump |
| 4 | 4 | `flags` u32 | `kKeyframeFlag*` (`colorize.h`) |
| 8 | 8 | `t_engine_ns` i64 | engine clock (A4's domain), exposure of **row 0** |
| 16 | 8 | `exposure_ns` i64 | exposure duration |
| 24 | 24 | `position` f64[3] | `world_from_camera` translation |
| 48 | 32 | `orientation` f64[4] | `world_from_camera` quaternion, **x, y, z, w** |
| 80 | 16 | `fx, fy, cx, cy` f32[4] | pixels |
| 96 | 20 | `distortion` f32[5] | k1, k2, p1, p2, k3 (OpenCV/ARCore order) |
| 116 | 4 | `width` u32 | |
| 120 | 4 | `height` u32 | |
| 124 | 4 | `row_time_ns` f32 | rolling shutter; **0 = global shutter** |
| 128 | 4 | `position_sigma_m` f32 | |
| 132 | 4 | `orientation_sigma_deg` f32 | |
| 136 | 1 | `pose_quality` u8 | `PoseQuality` |
| 137 | 1 | `tracking_lost` u8 | |
| 138 | 1 | `pose_source` u8 | `StreamId` |
| 139 | 1 | reserved u8 | 0 |
| 140 | 4 | `angular_rate_rad_s` f32 | rig rate at capture (`kKeyframeFlagMotionValid`) |
| 144 | 4 | `linear_speed_m_s` f32 | (`kKeyframeFlagMotionValid`) |
| 148 | 4 | `iso` f32 | (`kKeyframeFlagExposureValid`) |
| 152 | 4 | `image_bytes` u32 | JPEG size on disk; 0 = unknown |
| 156 | 4 | `name_len` u32 | |
| 160 | `name_len` | image name, UTF-8 | **relative to `streams/frames/`**, forward slashes, no `..`, not absolute |

Flags: `kKeyframeFlagMotionValid` (1), `kKeyframeFlagExposureValid` (2),
`kKeyframeFlagTrackingLost` (4), `kKeyframeFlagAutoExposureLocked` (8).

### 3.3 Six decisions inside those 160 bytes

1. **`t_engine_ns` is the exposure time of ROW 0**, not of the frame centre.
   That makes the rolling-shutter model need no second reference point: row
   `r` is exposed at `t + r · row_time`. It is also the convention the
   uncorrected error in §7.2 is measured against — see the note there about
   S6's mid-frame reference.
2. **The pose is `world_from_camera`.** A8 §3.1 established that the phone
   frame and the camera frame are one frame, so this is the same trajectory
   the pushbroom assembler and the LIO put points into, and no extrinsic is
   needed to project. (A caller whose poses are the *lidar body's* sets
   `pose_frame = kLidarBody` and the colorizer composes the mount extrinsic
   itself — §5.4.)
3. **`t_mono_ns` appears twice**, in the chunk header and in the record. The
   header copy is what lets A5's chronological merge and `seek()` work without
   parsing payloads; the record copy is what makes a record extracted on its
   own self-describing. A reader that finds them disagreeing trusts the record
   and counts it (`header_time_mismatches`).
4. **`angular_rate_rad_s` is recorded per keyframe.** S6's motion gate needs
   the rig's turn rate at capture; recording it means colorization works from
   a `.lscan` alone, on a desktop, with no IMU stream to re-integrate. A
   caller who *has* the IMU passes an `AngularRateFn` and that wins.
5. **The name is relative to `streams/frames/`**, not to the container root:
   it is the directory the app writes into, and it keeps the record short.
   `Keyframe::image_path` — which `colorize.h` defines as relative to the
   root — is composed on read and decomposed on write.
6. **`fixed_bytes` is in the record**, so a future field is appended and old
   readers skip it. Only a `record_version` bump is a hard refusal
   (`kVersionMismatch`), and it is reserved for a change that would make an
   old reader wrong rather than merely incomplete.

### 3.4 Validation, on both sides

`validate_keyframe()` runs at encode *and* at read: unit quaternion (1e-6),
positive `fx`/`fy`, principal point inside the image, non-zero dimensions,
non-negative row time and exposure, finite everything, and a **relative image
name with no `..` component, no leading `/` and no drive letter** — the same
zip-slip class A5's `zip_import()` defends against, because the name in an
index is just as much an attacker-supplied path. An invalid record cannot be
written; one found on disk is counted in `rejected_records` and skipped.

Read failures are counted, never fatal: `truncated_tail_chunks`,
`crc_mismatch_chunks`, `foreign_chunks` (an unknown chunk type in the same
file, skipped by length), `malformed_records`, `rejected_records`,
`out_of_order_records`, `header_time_mismatches`.

---

## 4. JPEG: vendored stb_image, and why

`engine/src/color/stb_image.h` — **stb_image v2.30 by Sean Barrett**,
dual-licensed **public domain (Unlicense) / MIT** at the user's choice. Both
licence texts are reproduced in full at the bottom of the vendored file; the
public-domain option requires no attribution, the MIT option requires the
copyright notice, which the file carries.

* Compiled in **one translation unit only** (`src/color/image_source.cpp`),
  with `STBI_ONLY_JPEG`, `STBI_ONLY_PNG`, `STBI_NO_STDIO`, `STBI_NO_LINEAR`,
  `STBI_NO_HDR` — the other nine decoders never reach the binary. No public
  header mentions it, exactly as `mid360_sdk2.cpp` is the only file that sees
  the Livox SDK.
* **Not a vcpkg port**, deliberately: `vcpkg.json`'s
  `$dependency-onboarding-order` asks every new dependency to prove itself on
  all five CI legs, and a single header with no build system does that by
  construction. libjpeg-turbo would be a real port, for a stage that decodes a
  few hundred frames per session off the hot path.
* **`STBI_NO_STDIO`**: the file is read by the engine, so a missing file is
  `kFileError` with a real message and the same entry point serves an
  in-memory buffer — which is what the tests use, and what a future Android
  path holding a CameraX buffer would use.
* A **pixel cap** (400 MPix) rejects a decompression bomb before allocating.
* `FileImageSource` cross-checks the decoded size against
  `CameraIntrinsics::width/height` and refuses a mismatch. An
  intrinsics/image mismatch smears colour across an entire cloud; it costs
  nothing to catch per keyframe.

Sampling is **bilinear in pixel-centre convention** (pixel *i*'s centre at
*i* + 0.5), clamped at the border, with round-half-up quantisation — truncation
would bias every sampled colour half a level dark, which shows up as a global
cast on a whole cloud.

---

## 5. The colorization pass

### 5.1 Shape of the loop

```
for each keyframe k (in index order):
    frustum-reject against the cloud's bounding sphere
    decode k's image                       (one image resident at a time)
    render a depth buffer from the CLOUD at reduced resolution
    for each point p:
        project p into k (rolling-shutter fixed point)
        reject: behind · off-image/edge margin · out of range · incidence · occluded
        score = incidence^w1 · distance^w2 · motion^w3
        if score > best[p]:  sample RGB bilinearly, remember it
write best colours into the points; flag the rest
```

Keyframe-outer, point-inner. That ordering is what keeps **one decoded image
resident at a time** (a 4032×3024 RGB frame is 36 MB; a hundred of them is
not an option on a phone) and what makes the depth buffer a per-keyframe
temporary rather than a per-keyframe cache.

The cost is two passes over the points per keyframe, i.e. `O(K·N)` with a very
small constant — measured 2 ms for 4,000 points × 8 keyframes, and dominated
in any real session by JPEG decoding, which is `O(K)`.

Ties are broken by the **lower keyframe index** (`>` not `>=`), and the whole
pass is single-threaded with a fixed iteration order, so the output is
bit-identical run to run (`gate/colorize_is_deterministic` compares the raw
bytes of the point array).

### 5.2 The score

```
s_incidence = |cos θ|            θ = angle between the surface normal and the
                                     ray back to the camera; rejected past
                                     max_incidence_deg (75°)
s_distance  = ref / (ref + range)   smooth, monotone, never zero
s_motion    = 1                     below the gate
            = gate / rate           above it   (and rejected past the reject
                                                threshold)
score       = s_inc^1 · s_dist^1 · s_motion^2
```

`w_motion = 2` by default because S6 §6.1 found sync × turn-rate to be the
dominant term in the budget — the weighting says so.

**Normals have to be estimated**, because `PointVertex` carries no normal
(16 bytes, S3-proven, not renegotiable). One PCA pass over a voxel-hash
neighbourhood, radius `normal_radius_m` (15 cm), eigenvector of the smallest
eigenvalue by cyclic Jacobi — not the closed-form cubic, which loses its
digits exactly where a point cloud lives, on a near-perfect plane where two
eigenvalues are equal and the third is ~1e-9 of them. Cost is one sort plus 27
cell lookups per point, ~12 bytes per point resident. With
`estimate_normals = false` the incidence term is neutral and selection falls
back to distance × motion.

The sign of a cloud's normal is arbitrary, so incidence uses `|cos θ|`.

### 5.3 Occlusion: a z-buffer rendered from the cloud

A point cloud has no surfaces, so a full-resolution depth buffer would be
mostly holes and would occlude nothing. The buffer is rendered at
`depth_scale` (1/8) with a splat of `splat_radius_px` (1 → 3×3), which at 1/8
of a 4032×3024 frame is 504×378 — a 5 cm-spaced cloud at 3 m covers every
pixel of it.

Visibility is `depth ≤ zbuf + tolerance`, and the tolerance has three terms:

```
tol = depth_tolerance_m                    (0.05 m — the splat's own footprint)
    + depth_relative_tolerance · depth     (1 % — depth error scales with range)
    + depth_slope_bias · cell_m · tan θ    (the slope-scaled term)
```

**The slope-scaled term is not optional, and it was measured into existence.**
One depth cell covers a patch of world whose *depth spread* grows with the
surface's obliquity: seen at 60°, a cell 20 cm wide spans 35 cm of depth, so
the far half of a perfectly visible wall sits "behind" the near half and a
fixed tolerance rejects it. Before this term existed, the synthetic room lost
**10 % of its points in alternating stripes** at the ends of every wall — the
columns where the view is grazing — and after it, **zero**. It is the same
slope-scaled bias shadow mapping uses, for the same reason, and it is zero on
a surface facing the camera.

**The consequence a caller must know:** the depth buffer must be coarse enough
that the cloud is *dense in it*. A sparse occluder in a fine buffer is a
sieve, and the surface behind it will be coloured with the occluder's pixels.
The failure is one-sided and the fix is to make `depth_scale` smaller (a
coarser buffer occludes more conservatively), which is why the default is 1/8
rather than 1/2. The occlusion test in §7.1 samples its panel at 2 cm against
the wall's 5 cm for exactly this reason.

### 5.4 Frames and the extrinsic

`Keyframe::pose` is `world_from_camera` (§3.3 item 2), so projection is
`camera_from_world = inverse(pose)` and **the mount extrinsic is not used**.
It is still validated when set — `mat4_is_rigid`, A8 §4.4's
column-major-across-JNI trap, which otherwise produces a plausible-looking but
mirrored result nobody notices until export.

With `pose_frame = kLidarBody` the poses are the *lidar body's* (what A6/A7
produce) and the colorizer composes
`world_from_camera = world_from_body · inverse(camera_from_lidar)` itself.
`color/refuses_a_non_rigid_extrinsic_and_composes_a_lidar_body_trajectory`
asserts the two paths produce **identical colours on 100 %** of the points
through an S6 mount-(b)-like extrinsic (110 mm baseline, 15° tilt).

### 5.5 Rolling shutter

Row `r` is exposed at `t + r · row_time`, so the pose that projects a point
depends on the row the point lands on, which depends on the pose. It is a
fixed point, and it converges immediately because a 20 ms readout moves a row
by a fraction of the image:

```
v ← project(p, pose(t))
repeat up to `rolling_shutter_iterations` (3):
    v ← project(p, pose(t + v · row_time))
    stop when v moves less than 1/100 of a row
```

`pose(t)` comes from one of two places:

* **A `PoseAtFn`** — in production A7's optimized trajectory
  (`PostSlamPipeline::trajectory()` is a `PoseSource`). Exact.
* **Nothing at all**, in which case a constant-velocity model is differenced
  from the *neighbouring keyframes*: `v = Δposition/Δt` and
  `ω = log(R_next · R_prevᵀ)/Δt`. This needs only `frames.idx`, which matters
  because the desktop importer and the cloud worker may have the frames and
  not the trajectory.

§7.2 measures both. They come out the same, to 1 %, over a 20 ms readout —
which is unsurprising, since 20 ms is short enough that a keyframe-to-keyframe
constant-velocity model is an excellent local approximation, and is the reason
the fallback is worth having rather than being a token.

### 5.6 Output, coverage, and what is never touched

Only `r`, `g`, `b` are written. Positions are untouched;
**alpha is untouched by default**, because it is A8's flag channel and A14's
LOD/selection channel — `low_confidence_alpha` and `uncovered_alpha` are
opt-in, mirroring `PushbroomConfig::flagged_alpha`.

`ColorCoverage`, one byte per point, parallel to the input:

| value | meaning |
| --- | --- |
| `kNone` | no acceptable view; the point **keeps** its intensity-derived colour, and a UI can draw the coverage gap |
| `kColorized` | coloured from a keyframe inside the motion gate |
| `kLowConfidence` | coloured, but only fast-turn frames were available — S6 §6.3's explicit instruction, the same treatment §3.3 gives ARCore tracking-loss points |

### 5.7 Progress, cancel, threading

`colorize()` is blocking and single-threaded; `cancel()`, `progress()`,
`stage()` and `stats()` are safe from another thread while it runs. The cancel
token is **A7's `post::CancelToken`**, not a second one: A15 owns one token
per job and hands out a pointer, and there is no reason for colorization to
invent a parallel vocabulary. Progress is a plain callback for the same reason
A7's is (`slam/post/progress.h`'s header comment): the stage must be runnable
in a unit test and by the cloud worker's CLI with no `Engine` in sight.
Cancellation is polled per keyframe and every `progress_point_interval`
points, and unwinds with `kCancelled` leaving the points as they were.

**A11 introduces no thread.** `DESIGN.md` §2's table needs no new row.

---

## 6. The clock sweep (S6 §7.1 item 1, WIZARD screen 3)

A4 §3 explains why a constant offset survives every filter: a one-way
timestamp stream is observable only up to the *minimum transport delay*, so
every mapped stamp carries `true offset + min latency`. That residue is
systematic — identical for every sample, constant within a session — and no
filter can see it. Two sensors watching the same physical motion can.

`estimate_clock_offset(camera, lidar, cfg, &result)`:

1. Resample both tracks onto a uniform 2 ms grid over their overlap, shrunk by
   the search radius at both ends so a shifted track never runs off the data.
2. Mean-remove; check the capture actually moved (relative std) and actually
   *swept* (zero crossings — one sweep per second for 8 s is ~16).
3. Normalised cross-correlation over ±100 ms.
4. **Parabolic refinement** through the peak and its two neighbours — this is
   what turns a 2 ms grid into a sub-millisecond answer.
5. Refuse on: too short, too few samples, no motion, no sweep, weak
   correlation, a peak at the search boundary, or **an ambiguous peak**.

**Sign convention, asserted by a test rather than a comment:**
`t_engine_ns = t_camera_ns + offset_ns`, equivalently
`camera(t) == lidar(t + offset)`. The colorizer applies it as
`ColorizeConfig::camera_clock_offset_ns`.

**"Ambiguous" needed care.** The obvious rule — "refuse if anything within
±30 ms correlates nearly as well" — refuses *every good capture*, because the
wizard's ~1 Hz sweep still correlates at 0.98 thirty milliseconds off. What
must be refused is a signal periodic enough that a **whole period fits inside
the search window**, where the true lag genuinely cannot be told from lag ± T.
So the estimator walks out from the peak to the first local minimum on each
side and looks for a *rival peak* outside that lobe. A 1 Hz sweep has none; a
12.5 Hz vibration has one every 80 ms and is refused.

The reported `sigma_ns` divides by the number of **independent** samples, not
of grid points — the grid is deliberately finer than either input (2 ms
against ARCore's 33 ms), and counting grid points reports an uncertainty ~4×
too small. Measured against truth it lands within ~2.5× of the actual error
(§7.4), which is the "±2 ms" the wizard needs to display and not a number to
weight a fit with.

The estimator is **signal-agnostic**: two scalar tracks, no units. In the
wizard they are the target's bearing rate in the camera against its bearing
rate in the lidar; on a bench they can be ARCore's gyro against the Mid-360's
IMU. Normalised correlation removes the arbitrary scale — the test's lidar
track is 1.7× the camera's on purpose.

---

## 7. Measured

`tests/test_color.cpp`, 25 cases / 6,862 assertions / **0.10 s**. Every number
below is regenerated on each run. The synthetic scenes are ray-cast from
analytic geometry, so "what colour should this point be" has an answer that
owes nothing to the code under test.

### 7.1 Colour accuracy and occlusion

A 4 × 4 × 2.5 m room, four walls of different solid colours, 4,000 points at
10 cm, eight cameras on a 0.3 m ring looking outward (320×240, f = 150 px →
93.6° × 77.3°), images ray-cast per pixel:

| metric | result |
| --- | ---: |
| interior points (≥ 20 cm from a wall edge) | 3,024 |
| of which covered | **3,024 (100 %)** |
| of those, **exactly** the right colour | **100 %** |
| mean per-channel error | **0.0 levels** |
| whole-cloud coverage | 97 % (the missing 3 % is the 10 cm strip at each wall's end, seen by nothing) |

"Exactly" means all three channels equal to the wall's colour, not within a
tolerance. Points nearer than 20 cm to a wall edge are excluded from the
strict count because at a corner two walls' colours meet *inside one pixel*,
so a bilinear sample there is a blend by construction.

Occlusion, on a purpose-built scene — a red wall, a white panel 1.2 m in front
of it, one camera head-on (which cannot see the wall behind the panel) and two
off to the sides (which can):

| metric | result |
| --- | ---: |
| panel points coloured white | 2,000 / 2,000 |
| **wall points wearing the panel's colour** | **0** |
| wall points inside the head-on camera's shadow | 160 |
| of those, correctly red from a side camera | **160 / 160** |
| same run with `occlusion_test = false` | **960 wall points turn white** |

That last row is the control: the artefact is real, large, and the z-buffer is
what removes it.

### 7.2 The rolling-shutter correction

S6 §7.1 item 2: *"model rolling shutter as a per-row time offset — −6.8 px,
free. Currently unmodelled and silently spending a third of the budget."*

The measurement: a camera yawing at **30 °/s** (S6's budget case) with a
**20 ms readout**, in front of a wall carrying a triangle-wave colour ramp of
1.0 m period — so one colour level is 1.96 mm of wall and *a colour error is a
position error*. The images are rendered **row by row at the pose of that
row's exposure**: a real rolling shutter, not a model of one.

| | mean colour error | as wall displacement |
| --- | ---: | ---: |
| uncorrected | 9.18 levels | **18.0 mm** |
| corrected, trajectory supplied (`PoseAtFn`) | 0.52 levels | **1.02 mm** |
| corrected, constant velocity from the keyframes alone | 0.52 levels | **1.01 mm** |

**A 17.7× reduction, and the residual is at the sampling floor.** The
uncorrected figure matches the closed form exactly: mean row offset ≈ 10 ms ×
30 °/s = 0.30° = 5.2 mrad, times the 3.5 m range = 18.3 mm.

Converting to S6's units — its reference camera is fx = 2912 px — 5.2 mrad is
**15.3 px**, against S6's quoted 6.8 px. The factor of two is a *convention*,
not a disagreement: S6 measures rows' deviation from the frame **centre**
(mean |Δt| = 5 ms over a ±10 ms range), while `frames.idx` stamps **row 0**
(mean |Δt| = 10 ms). Under either convention the correction removes the term
entirely — to 1.15 px on that camera, ~5 % of the uncorrected error, which is
bilinear sampling and 8-bit quantisation rather than geometry.

The keyframe-differenced fallback matching the exact trajectory to 1 % is the
result worth carrying: over a 20 ms readout, constant velocity is an excellent
local model, so **a `.lscan` with no trajectory in it can still be corrected**.

### 7.3 The motion gate

Two keyframes see the same wall from 10 cm apart. The **faster** one (40 °/s)
is the geometrically better view — it is closer. The slower one is 4 °/s. Each
paints a different colour, so the winner is visible in the output.

| run | points from the fast frame | from the slow frame |
| --- | ---: | ---: |
| gate off (`motion_gate = 1000 °/s`) | **1,350** | 50 |
| gate on (`kGated` → S6's 15 °/s) | **0** | **1,400** |

The gate flips the selection completely, and the two rows together are the
evidence that it is the gate doing it and not the geometry.

The other three behaviours S6 asks for:

* a keyframe **above the gate but below the reject threshold** still colours,
  and every point it colours is flagged `kLowConfidence` (1,350 of 1,350) —
  S6 §6.3's "coloured anyway but flagged";
* a keyframe **above the reject threshold** is not used at all
  (`keyframes_rejected_motion = 1`, 0 points coloured, the points keep their
  intensity colour);
* a caller-supplied `AngularRateFn` **outranks** the recorded per-frame rate:
  claiming the fast frame was slow and vice versa swaps the colours. A4's
  `ImuIngest::angular_rate_at()` is that function in production, and the
  colorizer never links against it — the callable is the seam.

### 7.4 The clock sweep

8-second synthetic sweeps, camera at 30 Hz against lidar at 200 Hz, different
amplitude scales, offsets recovered against truth:

| truth | recovered error |
| ---: | ---: |
| +37 ms | ≤ 1 ms |
| −23 ms | ≤ 1 ms |
| +4.5 ms | ≤ 1 ms |
| 0 | ≤ 1 ms |

and against noise (as a fraction of the sweep's amplitude), at a 29 ms truth:

| noise | error | reported σ |
| ---: | ---: | ---: |
| 2 % | **0.50 ms** | 0.21 ms |
| 8 % | **1.84 ms** | 0.72 ms |
| 25 % | **5.43 ms** | 2.11 ms |

The task's ±1 ms target is met on a clean capture and degrades gracefully; the
reported σ tracks the true error within ~2.5× across two decades of noise,
which is what makes it usable as a displayed uncertainty.

Every refusal is exercised: a rig that did not move (`kNoMotion`), a 1.5 s
sweep (`kTooShort`), a 12.5 Hz vibration (`kAmbiguous`), a one-directional
wave (`kNoSweep`), and structurally bad input (unsorted or too-short tracks →
`kInvalidArgument`, an *error* rather than a verdict).

### 7.5 The rest

| case | asserts |
| --- | --- |
| `fidx/record_round_trips_every_field` | every field survives encode→decode; trailing bytes ignored; `consumed` exact |
| `fidx/is_byte_identical_to_what_A5s_recorder_writes` | the two writers agree byte for byte, and A5's reader parses the chunks |
| `fidx/tolerates_a_truncated_tail_and_stops_at_a_bad_crc` | mid-payload truncation keeps 3 of 4 records; a flipped byte stops the read; an exact frame boundary is **not** a warning |
| `fidx/version_and_forward_compatibility` | a newer version is refused; extra `fixed_bytes` are skipped; a short record is `kCorruptData` |
| `fidx/validation_refuses_what_cannot_be_projected` | non-unit quaternion, principal point outside the image, zero `fx`, zero size, negative row time, `..`/absolute image names — and `kf..2.jpg` is fine |
| `fidx/a_session_without_a_camera_reports_not_found` | §3.5's "gracefully unavailable" is `kNotFound`, not corruption |
| `img/decodes_a_real_jpeg` | a 664-byte JFIF fixture decodes to the right pixels (±4 levels at quality 95); garbage and scribbled headers are `kCorruptData`; `FileImageSource` catches an intrinsics/image size mismatch and a missing file |
| `img/bilinear_sampling_is_exact_at_centres_and_midpoints` | pixel centres reproduce exactly; midpoints are means; outside is clamped |
| `gate/policy_reproduces_the_S6_verdicts` | the table in §2, including `kUnknown` failing closed even with `allow_poor` |
| `gate/an_unsynchronised_session_is_refused_before_any_work` | no image decoded, no point written |
| `gate/colorize_is_deterministic` | two runs, `memcmp` on the raw point bytes |
| `gate/progress_is_monotone_and_cancel_unwinds` | monotone to 1.0; an external token cancels mid-run; the internal `cancel()` is sticky |
| `gate/colorizes_a_page_store_in_place` | multi-page store, positions untouched, colours correct, empty/null stores refused |
| `gate/end_to_end_from_a_real_lscan_directory` | `frames.idx` on disk → `load_keyframes()` → `FileImageSource` → JPEG → correct colours; a missing image is skipped, not fatal |
| `color/points_no_camera_saw_keep_their_colour_and_are_flagged` | `kNone`, RGB **and** alpha untouched; opt-in alpha marking works |

---

## 8. Seams this task did NOT take — orchestrator action required

Each is a small change in a file A11 does not own.

### 8.1 `cloud/page_store.h` — a mutable page accessor

`Colorizer::colorize(PageStore*)` must rewrite points that already exist; the
store only hands out `PageView` with a `const PointVertex*`, because its
normal traffic is append-then-render. Today `src/color/colorizer.cpp`
`const_cast`s that pointer, which is *defined* (the buffer is non-const inside
the store and never reallocates) and *narrow* (only r/g/b/a change, so no
bound, count, page id or time range is invalidated) — but it is implicit where
it should be explicit. The clean fix is one accessor:

```cpp
// cloud/page_store.h — for the one producer that rewrites existing points.
PointVertex* page_data_mutable(PageId id);   // nullptr if not found
```

plus, ideally, a way to tell the renderer the colours changed (A14 territory:
either re-publishing `PageUpdate{page, 0, count}` or a new
`EventType::kPointsRecoloured`). Without it a live viewer keeps the old GPU
buffer until something else forces a re-upload.

### 8.2 C ABI (`capi/scanengine_c.h` / `.cpp`) — B8 needs these

Following DESIGN §4 and §6 item 9 (**bump `SCAN_ABI_VERSION` and
`kEngineAbiVersion` together**):

```c
/* --- keyframe index (B8 writes, any platform reads) --------------------- */
typedef struct {
  int64_t  t_engine_ns;
  double   position[3];
  double   orientation[4];      /* x, y, z, w */
  float    fx, fy, cx, cy;
  float    distortion[5];
  uint32_t width, height;
  float    rolling_shutter_row_time_ns;
  float    position_sigma_m, orientation_sigma_deg;
  uint8_t  pose_quality, tracking_lost, pose_source;
  uint32_t flags;
  int64_t  exposure_ns;
  float    iso, angular_rate_rad_s, linear_speed_m_s;
  uint32_t image_bytes;
  const char* image_name;       /* relative to streams/frames/ */
} scan_keyframe;

scan_error_t scan_engine_record_keyframe(scan_engine*, const scan_keyframe*);

/* --- colorization ------------------------------------------------------- */
typedef struct scan_colorizer scan_colorizer;
scan_error_t scan_colorizer_create(scan_colorizer**, const scan_colorize_config*);
void         scan_colorizer_destroy(scan_colorizer*);
scan_error_t scan_colorizer_load_keyframes(scan_colorizer*, const char* lscan_dir);
scan_error_t scan_colorizer_run(scan_colorizer*, scan_engine*);  /* the PageStore */
scan_error_t scan_colorizer_cancel(scan_colorizer*);
scan_error_t scan_colorizer_stats(scan_colorizer*, scan_colorize_stats* out);

/* --- the wizard's sweep (B7) -------------------------------------------- */
scan_error_t scan_clock_sweep_estimate(const scan_rate_sample* cam, uint32_t n_cam,
                                       const scan_rate_sample* lid, uint32_t n_lid,
                                       scan_clock_sweep_result* out);
```

`scan_engine_record_keyframe()` is the one that matters first: it is
`encode_keyframe_record()` + `recorder.write_chunk()`, and it is what makes B8
a capture task rather than a format task. **No new `ScanError` value is
needed** — everything A11 returns is `kInvalidArgument`, `kNotFound`,
`kNotSupported`, `kInvalidState`, `kCancelled`, `kCorruptData`,
`kVersionMismatch`, `kFileError` or `kIoError`, all of which already have C
mirrors.

### 8.3 Wiring, for whoever owns the pipeline

* **A4 → A11.** The session's gate is
  `cfg.sync_quality = ts.quality(StreamId::kLidarMid360)`, and the motion gate
  is
  `c.set_angular_rate_fn([&](std::int64_t t, double* r) { return imu.angular_rate_at(t, 250'000'000, r); })`.
  Both are one line and neither is wired by anything today.
* **A7 → A11.** After a post run,
  `c.set_pose_fn(...)` over `PostSlamPipeline::trajectory()` gives the
  rolling-shutter model the exact trajectory instead of the keyframe-difference
  fallback, and `colorize(&pipeline.out_store())` colours the deliverable in
  place. A15's local runner is the natural place for "post, then colorize".
* **A8 → A11 (the wizard).** `ExtrinsicsSolver` in `colorize.h` is still an
  unimplemented seam, deliberately: A8 §6 says **wrap, do not reimplement**.
  The wrapper turns a keyframe's checkerboard detection into the plane
  `(n, d)` A8's residual wants and delegates to `MountCalibrationSolver`;
  `split_half_agreement_px()` is `MountCalibResult::split_half_px`. The
  checkerboard *detection* lives on the app side, where the image already is
  (A8 §4.1), which is why this wrapper is worth writing only once B7 exists to
  feed it.
* **A5 → manifest.** WIZARD §3 wants the mount calibration **and the estimated
  clock offset** in `manifest.json`, keyed per bracket. A11 produces the
  offset (`ClockSweepResult::offset_ns`) and consumes it
  (`ColorizeConfig::camera_clock_offset_ns`); nothing persists it yet.
* **`tests/test_headers.cpp`** should add `color/frames_idx.h`,
  `color/image_source.h`, `color/clock_sweep.h` and `color/colorizer.h` to its
  include list, and **`DESIGN.md` §1**'s module table should move `color/`
  from "seam" to implemented (A11). §2's thread table needs no change.

---

## 9. Known limitations

* **The depth buffer is a heuristic, not a renderer.** §5.3: a cloud too
  sparse for the chosen `depth_scale` under-occludes. There is no adaptive
  choice of `depth_scale` from the cloud's density; a future version could
  derive it from the median nearest-neighbour distance, which the normal pass
  already has in hand.
* **No exposure or white-balance harmonisation across keyframes.** Adjacent
  points coloured from frames with different auto-exposure settings will show
  a seam. `exposure_ns` and `iso` are recorded in `frames.idx` precisely so
  this can be added without a format change; the obvious first step is to
  prefer frames with `kKeyframeFlagAutoExposureLocked` and to normalise by
  `iso · exposure`.
* **No blending.** Each point takes its colour from exactly one keyframe.
  Blending the best two or three would reduce seams and noise at a cost in
  determinism (and in a second pass over the images); best-view-only is the
  spec's wording and the cheaper thing to be correct about first.
* **Time proximity is not a selection term.** WIZARD §4 notes that ARCore's
  *relative* pose error over a short window is what matters, so a keyframe
  near a point's own capture time is preferable. `PointVertex` carries no
  timestamp, so there is nothing to compare against; if A7 ever hands over a
  parallel per-point time array, this is a one-term addition to the score.
* **Single-threaded.** The keyframe loop is embarrassingly parallel per
  keyframe *if* the reduction into `best_score` is made deterministic (it is,
  with a per-thread best and a fixed merge order). Not done: 2 ms for 4,000
  points × 8 keyframes leaves no case for it yet, and determinism is worth
  more than a constant factor here.
* **`ExtrinsicsSolver` is still a seam** — see §8.3.
