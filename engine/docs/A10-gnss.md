# A10 — GNSS/RTK: NMEA, NTRIP, RTCM3, georeferencing, CRS

**Scope:** `engine/include/scanengine/gnss/`, `engine/src/gnss/`,
`engine/tests/test_gnss.cpp`.
**Spec:** §2.3 (RTK rover), §3.4 (GNSS/RTK + pose fusion), §3.2 (time sync).
**Contracts:** `engine/DESIGN.md` §2 (threading), §3 (errors), §6 (how to add a
module).
**Reads:** `spikes/s5-rtk-sim/REPORT.md` — the simulation infrastructure this
task was built and tested against. `engine/docs/A4-timesync.md` §7 (the clock
mapping GNSS epochs go through). `engine/docs/A8-pushbroom.md` §2 (the
"hand-roll it, and say why" precedent this task follows twice) and its
`ExternalPoseSource` pattern. `engine/docs/A9-export.md` "CRS seam".

Nothing outside `gnss/` was touched. §9 lists the C-ABI and wiring additions
the orchestrator has to place in files A10 does not own — including the one
CMake block A10 could not add itself, because A7 owns `CMakeLists.txt` this
wave.

---

## 1. What landed

| File | What |
| --- | --- |
| `gnss/gnss.h` | A1's seam, filled in. `FixType`, an extended `GnssFix`, `GnssReceiver`, `NtripConfig` (+13 behaviour fields, all defaulted), `NtripClient`, `CrsConfig`. `fix_at_least()` and `default_sigma_for_fix()` are new and are the one place the §3.4 quality ladder is expressed. |
| `gnss/nmea.h/.cpp` | NMEA 0183: checksum, sentence framing, GGA/RMC/GST/GSA/VTG decoding, fix-state mapping, UTC→Unix, a streaming `NmeaFramer`, and a GGA **builder** for the NTRIP upload. |
| `gnss/rtcm3.h/.cpp` | RTCM 10403.x transport framing: CRC-24Q, `validate_frame`, `build_frame`, a resyncing `Rtcm3Framer` with per-message-type counts and corrections age. No message decoding — §3 says why. |
| `gnss/crs.h/.cpp` | WGS84 ↔ ECEF ↔ local ENU, Transverse Mercator (Krüger 6th order), UTM zone selection incl. the Norway/Svalbard irregularities, EPSG numbering, OGC WKT1 and PROJ strings, a `GeoidModel` seam. |
| `gnss/gnss_source.h/.cpp` | `GnssSource` — `GnssReceiver` + `PoseSource` + `PoseInterpolator`. Epoch assembly, GST sigmas, A4 time correlation, ENU origin anchoring, gated SE(3) interpolation. |
| `gnss/ntrip_client.h/.cpp` | `TcpNtripClient` — NTRIP 1.0/2.0, sourcetable, basic auth, RTCM3 passthrough, GGA upload, stall detection, reconnect with jittered exponential backoff. One engine-owned thread. |
| `gnss/georef.h/.cpp` | `GeorefEstimator` (the seam A7's factor graph implements), `WeightedSimilarityEstimator` (the hand-rolled default), `GeorefFusion` (pairing, CRS, `to_global*`). |
| `src/gnss/socket_compat.h` | Local BSD/Winsock TCP wrapper. Internal to `src/gnss/`. |
| `tests/test_gnss.cpp` | 36 offline cases / 2 231 assertions in **0.01 s**, plus 2 `doctest::skip()` cases that drive the S5 Python simulators live. |

The whole engine suite after A10: **336 cases, 2 254 382 assertions, 0
failures**; `ctest` 5/5 including the 204 s A3 Mid-360 sim soak.

---

## 2. Two decisions that shaped everything else

### 2.1 No GTSAM (§3.4 names it)

Spec §3.4: *"Fusion: factor graph (GTSAM) with LIO/VIO odometry factors +
GNSS position factors weighted by fix quality; continuously estimates the
local↔global similarity transform."*

That sentence describes **two** problems, and A10 owns the second:

1. **Smoothing the trajectory** with odometry + GNSS factors. That is a
   large sparse problem, it changes the poses themselves, and it is A7's
   pose-graph task.
2. **Aligning the local frame to the globe.** Given a trajectory and a stream
   of fixes, find the transform that carries one onto the other.

Problem 2 has **four parameters**: yaw about gravity, a 3-translation, and a
scale locked to 1 for a metric SLAM system. Its weighted least-squares
solution is closed form (Umeyama restricted to a rotation about a known
axis); the normal equations are 4×4 whether the session has 30 fixes or
30 000; the fix-quality weighting §3.4 asks for is one scalar per
observation; and the robustness that actually matters is an IRLS/Huber loop,
not a sparse linear solver.

`vcpkg.json`'s `$dependency-onboarding-order` is explicit about the cost of
the port — *"BEFORE adding it, verify each required transitive Boost
component's vcpkg portfile is CMake-based: autotools/configure ports … FAIL
under cmake/triplets/universal-osx.cmake"* — across five CI legs including
the Android NDK. **A7 had not added gtsam when A10 landed** (`vcpkg.json`
still lists eigen3 alone), so taking that dependency on A7's behalf, to solve
a four-parameter problem, was not A10's call to make. Same conclusion A8
reached about Ceres for the same kind of reason (docs/A8-pushbroom.md §2).

**The estimator is therefore behind an interface**, and that is the deliverable
half of this decision. `GeorefEstimator` takes weighted correspondences and
returns a `GeorefSolution` + its quality.
`GeorefFusion::set_estimator(std::unique_ptr<GeorefEstimator>)` swaps in a
different one with no change to any consumer — the same move
`TimeSync::set_estimator()` made possible for A4.
`gnss/georef/estimator_is_a_swappable_seam` proves the swap end to end
against a stub. When A7 lands a factor graph, it implements `add`/`solve` and
gets the CRS, the outlier policy's replacement, `to_global_points()` and
B9's status reporting for free.

### 2.2 No PROJ

A9's `ExportOptions::crs_wkt` needs a real CRS string; §3.4 needs an EPSG
picker and UTM defaults. PROJ would supply both — and drag SQLite plus a
~10 MB `proj.db` grid database that would have to ship inside the Android
APK and build on the universal-osx overlay triplet.

What is actually needed is four closed-form transforms and one Krüger series.
`gnss/crs.h` is 250 lines of header and 450 of implementation with no
dependency at all, and §4 states the measured accuracy of every one of them
against references that are not this code. The bound at which this decision
has to be revisited is also in §4: a non-WGS84 datum, or a national grid.

---

## 3. RTCM3: framing only, deliberately

`gnss/rtcm3.h` validates the envelope — `0xD3`, the 10-bit length, CRC-24Q —
and reads DF002 for the message-type histogram. It decodes **no message
content**. Two reasons, and both are boundaries rather than shortcuts:

* **Nothing in this product needs the content.** §2.3: *"NTRIP corrections
  fetched by the app and forwarded as RTCM3."* The rover's RTK engine is what
  consumes them. A decoder here would be a second, divergent implementation
  of somebody else's job.
* **It could not be tested.** S5's REPORT is explicit that the simulator's
  payloads are transport-valid and semantically meaningless: *"A10's RTCM3
  message decoder … cannot be validated against this stream."* A decoder
  validated against nothing is worse than no decoder.

What the framing does buy is not decorative:

| | why |
| --- | --- |
| **Corrections age** (§3.4 requires it surfaced) | age is time since the last **CRC-valid** frame. Counting bytes would report 0 s while a caster streams garbage. `age_s()` returns **−1**, not 0, before the first frame: "unknown" and "fresh" are different claims and only one of them is reassuring. |
| **Integrity** | the Bluetooth SPP hop to the rover is the least reliable link in the chain. If corrections arrive at the engine already corrupt, the rover silently stays Float. `frames_crc_failed` is what separates "bad corrections" from "bad sky". |
| **Frame-aligned forwarding** | the rover gets whole frames, so a dropped TCP connection cannot leave a half-frame in its parser. |

Two corrections-age numbers exist and they are not the same: the **engine's**
(`Rtcm3Framer::age_s` / `NtripClient::correction_age_s`, time since the last
valid frame off the caster) and the **rover's** (`GnssFix::correction_age_s`,
GGA field 13, the age of the corrections the rover's RTK engine actually
applied). The rover's is normally larger — it includes the caster→engine
latency, the engine→rover Bluetooth hop and the rover's own processing. A UI
that shows one should say which.

The CRC-24Q implementation and the NMEA checksum are both **cross-validated
against the S5 spike's independent Python implementations**: `rtcm_tool.crc24q`
re-derived the polynomial from the standard's generator and gives `0xEDEDD6`
for the same frame this table-driven version does; `nmea_sim.nmea_checksum`
gives `0x64` for the same body.

---

## 4. CRS accuracy — measured, against references that are not this code

Every number here is regenerated by `test_gnss.cpp` on every run.

| transform | reference | measured error |
| --- | --- | --- |
| geodetic → ECEF at (0,0,0), (0,90,0), (90,0,0) | the ellipsoid's *definition* (a, a, b) | exact to 1e-15 relative |
| geodetic ↔ ECEF round trip, −89…89° × −180…180° × −400…9000 m | itself | **1.6 nm** horizontal, **1.9 nm** vertical |
| ENU ↔ WGS84 round trip over a 4 km box | itself | **2.5 nm** |
| meridian arc, 0…84° | Simpson integration of M(φ) = a(1−e²)/(1−e²sin²φ)^1.5, 20 000 panels, computed in the test | **3.2 × 10⁻⁸ mm** |
| Transverse Mercator forward | Snyder, *Map Projections — A Working Manual*, USGS PP 1395, pp. 269–270 (Clarke 1866, k₀ = 0.9996, CM 75°W, 40°30′N 73°30′W) → x = 127 106.5, y = 4 484 124.4, k = 0.9997989 | x **127 106.467**, y **4 484 124.434**, k **0.9997989** — agreement at Snyder's stated 0.1 m / 7-digit precision |
| UTM round trip, whole of zone 50, lat −80…84 | itself | **0.006 µm** |
| UTM grid distance ÷ point scale, 1.4 km baseline | the ECEF chord (shares no code with the projection) | **1411.54 m vs 1411.54 m**, < 1 cm |
| point scale on the central meridian | k₀ = 0.9996 exactly, easting = 500 000 exactly, convergence = 0 | exact to 1e-12 |

**Sources of the reference values, stated because the brief asked:** the
ellipsoid-definition points and the round trips are self-referential by
construction (no external data can be wrong); the meridian arc is checked
against a numerical integration performed *inside the test*, which is
independent of the Krüger series under test; the projection is checked
against a **published printed worked example** (Snyder PP 1395). Note that
Snyder's book prints the meridian arc for that example as 4 484 837.0 m
while the numerical integration — and this code — give 4 484 837.671 m; the
integration is the arbiter, and the test asserts against it.

**Accuracy bounds of the hand-rolled transforms, and when to stop trusting
them:**

* **geodetic ↔ ECEF ↔ ENU: exact.** A rotation and a translation. No
  approximation to bound. Bowring's method plus two Newton refinements is at
  the double-precision floor everywhere including the polar axis.
* **UTM: < 1 mm within a zone** (±3° of the central meridian), and the
  round-trip measurement above says nanometres for the range this product is
  used in. The Krüger series is 6th order in the third flattening; its
  truncation error grows as roughly (Δλ)⁷ and becomes visible past ~±10° from
  the central meridian, which UTM never asks for. **Do not** use `tm_forward`
  for a wide-zone projection (a national Transverse Mercator with a 4° or
  wider half-width is fine; a hemisphere-wide one is not).
* **Datum: WGS84 only.** There is no datum transformation here — no
  Helmert 7-parameter, no NTv2 grid shift. A project whose CRS is a national
  datum (HK80, OSGB36, NAD83, GDA2020) will be **out by metres to tens of
  metres** if it treats these WGS84 coordinates as that datum's. That is the
  hard boundary of the no-PROJ decision, and `wkt1_for_epsg()` returns an
  **empty string** for anything that is not 4326/4979 or a WGS84 UTM zone
  precisely so a caller cannot silently mislabel one. The survey-profile
  escape hatch is `CrsConfig::epsg` plus a caller-supplied WKT.
* **Vertical: see §6.**

### WKT dialect

`utm_wkt1()` emits **OGC WKT1**, not WKT2. The LAS 1.4 OGC-WKT VLR predates
WKT2, and CloudCompare / QGIS / LAStools / PDAL all read WKT1 without
argument while WKT2 support in the older readers is patchy. A9's *local*
placeholder is deliberately WKT2 `ENGCRS` so that a tool trying to reproject
it **fails loudly**; a real CRS has the opposite requirement, so it gets the
conservative dialect. The test asserts the exact registry text for the
spheroid, the projection parameters and the `AUTHORITY` node, and that the
brackets balance — a malformed WKT in a VLR makes a LAS file unopenable, and
that is exactly the kind of thing a template edit breaks silently.

---

## 5. Georeferencing: accuracy vs fix quality

The scenario is a 40 × 25 m walking loop at 1.2 m/s for 240 s, sampled at
1 Hz, with the local frame rotated 37° and translated (123, −45, 7.5) m from
the global one, and per-fix noise at the §3.4 magnitudes (Fixed 2 cm, Float
30 cm, Single 2 m). *Actual* error is the worst of the four loop corners
after applying the recovered transform — i.e. where a point on the far side
of the site really lands, not the parameter error.

| fix mix | yaw error | worst corner error | reported σ_h | reported CEP95 | residual RMS |
| --- | --- | --- | --- | --- | --- |
| all RTK Fixed | 0.0009° | **2.0 mm** | 0.020 m | 0.049 m | 0.028 m |
| Fixed 3 : Float 1 | 0.0041° | **2.6 mm** | 0.021 m | 0.050 m | 0.205 m |
| all RTK Float | 0.0129° | **29 mm** | 0.301 m | 0.737 m | 0.415 m |
| Float 3 : Single 1 | 0.0579° | **38 mm** | 0.314 m | 0.769 m | 1.402 m |
| all Single | 0.0860° | **195 mm** | 2.009 m | 4.916 m | 2.769 m |

End-to-end through the real path (NMEA bytes → `GnssSource` → `GeorefFusion`,
199 RTK-Fixed fixes): yaw **36.994°** (truth 37), translation
**(11.996, −7.999, 1.250) m** (truth (12, −8, 1.25)), **CEP95 4.9 cm**.

Three properties the table is asserted on:

1. **Both columns are monotone in fix quality.** The actual error degrades and
   the reported uncertainty degrades with it. A mix that is mostly Fixed
   reports better than pure Float, and worse than pure Fixed.
2. **CEP95 bounds the actual error in every row** — by 25× for Fixed, 25× for
   Float, 25× for Single. It is a *conservative* bound, and §5.1 says why
   that is the right kind of wrong.
3. **Reported σ_h grows 100× from Fixed to Single** while the actual error
   grows 100×. Degradation is visible in the number a UI shows, not hidden
   behind a fit residual that averaged it away.

### 5.1 Why the reported uncertainty is conservative on purpose

`GeorefSolution::horizontal_sigma_m` is three terms in quadrature:

```
σ_h² = σ_translation²  +  (σ_yaw · lever_arm)²  +  σ_fix²
       └ averages down ┘   └ averages down ┘        └ does NOT ┘
```

The first two are the transform's own parameter uncertainty from
(JᵀWJ)⁻¹ — they shrink as √N, and after 200 RTK-Fixed samples they are
millimetres. The third is the fixes' own accuracy, included **at full
strength**, treating it as fully correlated across the session.

That is deliberate. A session's fixes share a base station's coordinate
error, an antenna phase-centre model and an atmospheric bias; none of that
averages out. Treating σ_fix as independent — which is what leaving it out
amounts to — would report 1.4 mm for an RTK-Fixed session, and a cloud
georeferenced from 2 cm fixes is not accurate to 1.4 mm. The truth is between
the two, and the pessimistic end is the one a survey report should print.

Both halves are reported separately (`translation_sigma_h_m`,
`yaw_sigma_deg`, `mean_fix_sigma_m`, `residual_rms_h_m`) so a caller that
knows better about its base station can recombine them.

**Covariance inflation.** When the residuals are larger than the weights say
they should be, the excess is real unmodelled error — SLAM drift, an
unmodelled lever arm, a tilted local frame — and the parameter sigmas are
scaled by the reduced chi-square. Measured: injecting a 0.15 %-of-distance
LIO drift into the local trajectory raises the residual RMS from 0.030 m to
0.156 m and the reported σ_h from 0.0201 m to 0.0254 m. The residual is what
*diagnoses* drift; the inflation is what stops the report claiming 2 cm on a
15 cm cloud.

### 5.2 Robustness

20 injected multipath jumps of 5–30 m in 240 samples — **still claiming RTK
Fixed accuracy**, so their 1/σ² weight makes them the heaviest samples in the
fit — are rejected exactly (20 of 20, no false positives), and the worst
corner error goes from 3.5 mm (clean data) to **3.7 mm**. Plain weighted
least squares cannot survive that input; the Huber IRLS plus a hard
rejection pass does.

The Huber threshold is in units of **each observation's own sigma**, so a
2 cm Fixed sample and a 2 m Single sample are held to the same *statistical*
standard. The hard rejection has a 10 cm floor
(`SimilarityEstimatorConfig::reject_floor_m`) so that a run of 2 cm fixes
cannot reject a genuine 10 cm SLAM excursion as an outlier.

### 5.3 Yaw-only, and what is not converged

Roll and pitch are observable from gravity, continuously, by the IMU A6's
ESKF is already running. Solving for them from GNSS *positions* instead means
estimating two parameters that a level walk does not constrain at all. So the
local frame's +Z must already be up (`GeorefConfig::local_is_gravity_aligned`
is a precondition, not a request), and `GeorefSolution::gravity_residual_m` —
the vertical residual's correlation with horizontal radius, expressed as the
vertical error that tilt produces at the working radius — is the evidence
that the precondition held.

Yaw is observable only from a **baseline**. Two fixes a metre apart determine
heading to ±atan(σ/1 m): 1.1° at 2 cm, 63° at 2 m. So a stationary rover gets
`converged == false` with `blocker == "trajectory too short to observe
heading"` (measured: 7.9° yaw sigma over an 8.8 cm baseline from 120
RTK-Fixed samples) rather than a confident heading derived from noise. The
other blockers are `"not enough usable fixes"` and `"heading uncertainty above
threshold"`, and every `to_global*()` returns `kInvalidState` until the
transform converges — `set_allow_unconverged(true)` is the explicit opt-out
for a live preview that would rather show an approximately-placed cloud.

**Scale is locked to 1 by default.** A free scale absorbs GNSS bias into a
systematic stretch of the cloud, which is invisible in the residual and
catastrophic in a measurement. With a 3 % mis-scaled local frame, the locked
solve leaves a 0.22 m residual (visible, diagnosable) and the unlocked solve
recovers 1/1.03 to 0.1 %. Unlocking is a deliberate act, and the header says
what it costs.

---

## 6. Vertical, and the "coarse EGM96 geoid bundled" of §3.4

**A10 ships the `GeoidModel` seam and no grid.** That is a considered
decision:

* **The receiver already carries one.** GGA field 11 is the geoid separation
  the rover's own (usually EGM96) model computed for this exact position.
  `GnssFix` keeps it (`geoid_sep_m`, `has_geoid_sep`) and derives
  `height_ellipsoid_m = alt_m + geoid_sep_m`. Every F9P and every Emlid
  reports it. An engine-side model would add a second opinion, not
  information.
* **A grid coarse enough to embed in source is worse than useless.** EGM96
  undulation spans −105 … +85 m; a 15° lattice interpolates to several metres
  of error, which would silently corrupt the vertical of a survey whose
  horizontal is good to 2 cm.
* **A real 15′×15′ EGM96 grid is a ~2 MB data asset** needing an install rule
  in a `CMakeLists.txt` this task does not own, and an Android asset-packaging
  decision that belongs to B-workstream.

So: `ConstantGeoidModel` for a site with a known local undulation (the honest
configuration for a single-site survey), the rover's own value when present,
and a documented seam for the Phase-2 survey-grade model §3.4 already defers.

`Geodetic::height_m` is **ellipsoidal** throughout `crs.h`; `GnssFix::alt_m`
is **orthometric**, exactly as GGA reports it. Mixing them is the classic
30-metre georeferencing bug, so the field names say which is which and the
conversion happens in exactly one place.

---

## 7. GnssSource: three things worth knowing

**Epoch assembly costs one epoch of latency, and it buys the sigmas.** A
receiver emits a burst per epoch — GGA, RMC, GST, GSA, VTG — sharing one UTC.
The per-axis sigmas live in **GST, which arrives after the GGA that carries
the position**, so publishing on GGA would systematically use the fallback
sigma and never the measured one. The epoch closes when a sentence with a
different UTC arrives, on `flush()`, or after `epoch_timeout_ns` (1.5 s).
The 200 ms (at 5 Hz) of latency lands on the *fix callback* only: poses are
timestamped with the epoch's own UTC-derived engine time, so a
late-*published* pose is still correctly placed in time, and A8's assembler
already buffers points whose pose has not arrived (`PoseGate::kFuture`).

**Time goes through A4, and it works.** §3.2: *"GNSS: NMEA time via arrival
correlation."* The sentence's UTC is the epoch the position refers to; the
arrival stamp is late by the receiver's processing plus the SPP link. Both go
into `TimeSync::add_pair(StreamId::kGnss, …)` — for which
`docs/A4-timesync.md` §7 already installs the min-delay estimator — and the
fix's `t_mono_ns` is the mapped result. Measured against a 120 ms link floor
plus 0–60 ms of uniform jitter at 1 Hz for 180 s: raw arrival spacing scatters
by **53 ms**, the mapped spacing by **12.7 ms** worst overall and **5.3 ms**
in the last third, with the mean epoch spacing correct to 4 × 10⁻⁵ s. A4's
own §6 warns that a 1 Hz stream is the engine's slowest-converging case, and
that is visible here as the residual rate error, not hidden.

**Orientation is a direction of travel, and only while travelling.** A
single-antenna receiver measures position, not attitude. Above
`min_speed_for_heading_mps` the pose carries a yaw-only quaternion from
course-over-ground with `orientation_sigma_deg = 5`; below it, identity with
**sigma 180°**, meaning "no information". A consumer that takes a stationary
rover's heading seriously will spin the cloud, so the number says not to. The
convention is spelled out once, in `close_epoch_locked_`: the local frame is
x = East, y = North, so a compass course C is a yaw of (90° − C).

**Fix outages gate the interpolation.** A no-fix epoch between two good ones
means an interpolation across an outage, and `sample_at()` returns
`PoseGate::kTrackingLost` for it rather than a smooth fiction — the same
treatment A8's `ExternalPoseSource` gives ARCore tracking loss, through the
same `PoseSample` fields, which is what lets A8's assembler consume "ARCore
indoors, RTK outdoors" without a code path per source (§3 key rule 3).

`push_pose()` returns **`kNotSupported`**: pushing a pose into a receiver
would create a trajectory with no fix behind it, which is precisely the
confusion §3.4's quality gate exists to prevent. Replay pushes into an
`ExternalPoseSource` instead.

---

## 8. NTRIP client behaviour

### Threading — DESIGN §2 addition

| Thread | Owner | What runs on it |
| --- | --- | --- |
| NTRIP receive *(A10, landed)* | `TcpNtripClient` | blocking `recv` with a 1 s socket timeout so `disconnect()` is prompt; RTCM3 framing; the rover-write callback; periodic GGA upload; stall detection; reconnect/backoff. One per client. |

The rover callback runs **on this thread** and must be quick and must not
re-enter the client — the general DESIGN §2 callback rule, restated because
this is a new place it applies. It is where the app does
`bluetoothSocket.write(bytes)`. It is invoked with **no client lock held**, so
a slow Bluetooth write cannot deadlock the client's own state.

### The connect contract

`connect()` performs the **first handshake synchronously**, then hands the
socket to the worker. A wrong password or a non-existent mountpoint therefore
surfaces as `kPermissionDenied` / `kNotFound` from `connect()` itself, rather
than as an infinite reconnect loop under a UI that says "connecting…"
forever. Verified live against the S5 caster: bad password →
`kPermissionDenied`, bad mountpoint → `kNotFound`, both leaving the client in
`kFailed`. Those two errors are also treated as **permanently fatal by the
reconnect loop** — retrying a rejected password forever is how an account gets
banned from a public caster.

NTRIP **v2 by default with automatic v1 fallback**: the client sends
`Ntrip-Version: Ntrip/2.0` and, if the caster answers with something it cannot
classify or with a sourcetable instead of a stream, reconnects and retries the
bare `ICY 200 OK` form. RTK2go and most community casters are v1-only in
practice (see the spike's `PUBLIC_CASTERS.md`), and a client that cannot fall
back looks like a broken client to the user. The `User-Agent` is forced to
start with `NTRIP` because several public casters 400 anything else.

Bytes that arrive **after** the response header are corrections, not noise:
the handshake reader returns them and they go straight into the framer.

### Under drops — measured

Against `ntrip_caster_sim` with `drop_after_frames=25, max_drops=1` and a
50 ms frame cadence:

```
NTRIP: 60 valid frames (0 CRC failures), 9130 bytes, 7 GGA uploads,
       1 disconnects, 1 reconnects, corrections age 0.036 s
caster GGA log: gga=7 drops=1
```

* the forced mid-stream close is detected on the `recv() == 0`;
* the client backs off (1 s → 2 s → … capped at 30 s, ±25 % deterministic
  jitter so a fleet of rovers that all lost LTE in one tunnel does not
  hammer the caster in lockstep), reconnects, and resumes;
* **every frame delivered to the rover callback is whole and CRC-valid** —
  the test re-validates each one independently;
* **session counters survive the reconnect.** `Rtcm3Framer::clear_buffer()`
  drops the half-frame from the dead connection without resetting the stats,
  because "how many corrections did this session receive" must not become 35
  the moment a caster hiccups. (This is why `reset()` and `clear_buffer()` are
  separate methods.)
* the state callback emits `kStreaming → kReconnecting → kStreaming`, which is
  what B9's status strip renders;
* the caster logged all 7 of our GGA uploads.

Three further failure modes are handled and are not exercised by the sim
because it cannot produce them: a **stall** (socket open, no RTCM for
`stall_timeout_ms`, default 30 s) drops the connection and reconnects —
casters do go quiet, and 30 s of silence on a 1 Hz correction stream is a
black hole, not a quiet caster; `max_reconnect_attempts` (0 = forever)
terminates into `kFailed`; and `receiving()` is false until the first valid
frame on the **current** connection, because "connected" and "receiving
corrections" are different claims and a UI must not infer the second from the
first.

### GGA upload

Preference order: the **rover's own last GGA verbatim**
(`GnssSource::last_gga_sentence()` via `set_gga_provider`), so a VRS caster
sees exactly what a stand-alone rover would send; then a synthesized GGA from
`set_position()`, which is also how a session connects to a caster *before*
the rover has a fix (seeded from the project's approximate site). Default
cadence 10 s, the NTRIP 2.0 suggestion and u-center's default. `NtripSource`
exposes the sourcetable's `nmea` flag as `needs_gga` and a `distance_km()`
helper — baseline length is the dominant term in Fixed-vs-Float, so a
mountpoint picker sorts on it.

### Sockets

`src/gnss/socket_compat.h` is a **local** BSD/Winsock wrapper, deliberately
not an extension of `transport/udp_source.cpp`'s: that file is A3's, and A10
may read it but not edit it. The two agree on approach — one process-wide
`WSAStartup`, `SO_RCVTIMEO` for prompt shutdown, the same
`SCAN_INVALID_SOCKET` / `scan_close_socket` spelling — so a later
consolidation into `transport/tcp_source.h` is mechanical. What is genuinely
new: `getaddrinfo` (a caster is a hostname; a lidar is an IP) and a
non-blocking `connect()` + `select()` deadline, so a dead caster fails in
`connect_timeout_ms` rather than the OS's ~75 s SYN timeout. Asserted by
`gnss/ntrip/argument_validation_and_lifecycle`, which connects to a closed
port and requires a prompt error.

---

## 9. Seams this task did NOT take — orchestrator action required

Each needs a change in a file A10 does not own. **None of them blocks
anything**: everything above is tested and green without them.

### 9.1 `engine/CMakeLists.txt` — the `sim-rtk` ctest entry (A7 owns CMake this wave)

`src/gnss/*.cpp` and `tests/test_gnss.cpp` are already picked up by the
existing `GLOB_RECURSE` / `GLOB`, so **no CMake change is needed to build or
to run the 36 offline cases** — they are in `scanengine_tests` today.

The two live cases carry `doctest::skip()` and need one registration, modelled
exactly on the A3 `mid360_sim_e2e` block a few lines above it:

```cmake
    # --- optional: GNSS/RTK end-to-end against the S5 simulators (A10) -----
    #     ctest -L sim-rtk      # ~40 s: live NMEA over TCP, live NTRIP
    #     ctest -LE sim-rtk     # everything else
    set(ENGINE_S5_SIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../spikes/s5-rtk-sim"
        CACHE PATH "S5 RTK simulator spike (for the 'sim-rtk'-labelled ctest)")
    if(ENGINE_SIM_TESTS AND EXISTS "${ENGINE_S5_SIM_DIR}/nmea_sim.py")
        add_test(NAME gnss_rtk_sim_e2e
                 COMMAND scanengine_tests --no-skip "--test-case=gnsssim/*" --success=false)
        set_tests_properties(gnss_rtk_sim_e2e PROPERTIES
            LABELS "sim-rtk"
            TIMEOUT 300
            SKIP_REGULAR_EXPRESSION "SKIPPED:"
            ENVIRONMENT "SCANENGINE_S5_SIM_DIR=${ENGINE_S5_SIM_DIR}")
        message(STATUS "scanengine: GNSS/RTK simulator tests registered (ctest -L sim-rtk)")
    endif()
```

The cases bind **per-process ports** (39000/40000 + pid % 900) and write
per-process temp files, so two build trees on one host cannot interfere —
unlike A3's, they do **not** need `RUN_SERIAL`. They need `python3` and the
spike directory; either missing prints `SKIPPED: …`, which
`SKIP_REGULAR_EXPRESSION` turns into a CTest skip rather than a false pass.
`SCANENGINE_S5_SIM_DIR` is only a hint — the test also searches upwards from
its CWD for `spikes/s5-rtk-sim`.

Verified locally by invoking that exact command line 3× in a row: 2/2 cases,
1 489 assertions, no flakiness.

### 9.2 C ABI (`capi/scanengine_c.h` / `.cpp`) — B9 needs these

Integration #24 owns `core/`+`capi/` concurrently, so A10 declares rather than
writes them. B9's RTK UI (rover BT pairing, NTRIP config, fix-status strip,
capture gating) needs, at minimum:

| C entry point | maps to |
| --- | --- |
| `scan_engine_push_nmea(engine, bytes, len, t_mono_ns)` | `GnssSource::push_nmea` |
| `scan_engine_last_fix(engine, scan_gnss_fix* out)` | `GnssSource::last_fix` |
| `scan_engine_gnss_stats(engine, scan_gnss_stats* out)` | `GnssSource::stats` (the fix-quality timeline the status strip shows) |
| `scan_ntrip_create/destroy/connect/disconnect/state/stats` | `TcpNtripClient` |
| `scan_ntrip_set_rtcm_callback(client, cb, user)` | already a C-shaped signature in `NtripClient` — a trampoline, never a cast (DESIGN §4) |
| `scan_ntrip_fetch_sourcetable(host, port, scan_ntrip_source* out, n, &count)` | `TcpNtripClient::fetch_sourcetable` — the mountpoint picker |
| `scan_engine_georef_solution(engine, scan_georef_solution* out)` | `GeorefFusion::solution` |
| `scan_engine_crs_wkt(engine)` | `GeorefFusion::crs_wkt` (UTF-8, engine-owned, thread-local — the DESIGN §4 string rule) |

New enums to mirror + `static_assert`: `FixType` (5 values) and `NtripState`
(6). New structs to convert **field by field**: `GnssFix`, `NtripStats`,
`NtripSource`, `GeorefSolution`. `SCAN_ABI_VERSION` and `kEngineAbiVersion`
bump together.

### 9.3 `Engine` wiring (`src/core/engine.cpp`, `include/scanengine/core/engine.h`)

1. **`DeviceKind::kRtkRover` already exists** (`core/types.h`, tagged "A10")
   and `Engine::add_device` should route it to a `GnssSource`, exactly as it
   routes `kD6`/`kMid360`.
2. **Own one `GnssSource`, one `TcpNtripClient` and one `GeorefFusion`** in
   `Engine::Impl`, and wire the three of them:
   `gnss.set_fix_callback([&](const GnssFix& f){ georef.add_fix(f); })`,
   `georef.set_local_source(<A6's LIO or A8's ExternalPoseSource>)`,
   `georef.set_enu_frame(gnss.enu_frame())` on the first fix, and
   `ntrip.set_gga_provider([&](std::string* s){ *s = gnss.last_gga_sentence(); return !s->empty(); })`.
3. **Pass `GnssSource*` as the pushbroom assembler's `PoseInterpolator`** for
   §3.3's "Desktop D6 capture: no ARCore → RTK-trajectory mode". No change to
   A8's code: it is the same interface.

### 9.4 Events (`core/event.h`) — needs an `EventType` append

Three are worth having, and all three need the DESIGN §6 step 6 procedure
(append to `EventType`, add the payload POD, extend `category_of()`, mirror in
`scanengine_c.h`, add a `convert_event()` case):

* `kGnssFix` — payload `{FixType, satellites, hdop, correction_age_s,
  sigma_h}`. B9's status strip currently has to poll.
* `kNtripState` — payload `{NtripState, ScanError, backoff_ms}`.
* `kGeorefConverged` — payload `{cep95_m, samples, epsg}`. The moment the
  session becomes exportable in a real CRS.

Until then, everything is reachable by polling `stats()`, and the NTRIP state
callback already exists in C++.

### 9.5 A9 export — nothing to change, just start passing it

`docs/A9-export.md`'s "CRS seam" says A10 *"needs to change nothing — just
start passing a real `crs_wkt` once it has one."* It has one:

```cpp
opts.crs_wkt  = georef.crs_wkt();     // "" until converged -> A9's ENGCRS placeholder
opts.crs_epsg = georef.epsg_string(); // "EPSG:32650"
```

`crs_wkt()` returns empty until the transform converges, which is exactly the
input A9 documents as "embed the local-frame placeholder". The cloud itself
goes through `GeorefFusion::to_global_points(page, out, cap)`, which snapshots
the 4×4 and releases the lock before transforming — a page is a million
points and holding the fusion mutex across that would stall the GNSS thread.

### 9.6 A5 `.lscan`

`StreamId::kGnss` and `kGnssStreamFile` already exist and the recorder already
has a slot for them. Two things would make replay reproduce capture exactly:
the **raw NMEA bytes** written before parsing (the `kD6Raw` pattern), and a
periodic `GeorefSolution` + origin snapshot in the manifest so a replay does
not have to re-derive the alignment.

### 9.7 `DESIGN.md` §2

One row: the NTRIP receive thread (§8 above has it in the table's format).

### 9.8 Known limitations, stated rather than hidden

* **Antenna lever arm is a constant ENU offset**
  (`GnssSourceConfig::antenna_offset_enu`). That is correct for a
  pole-mounted antenna directly above the rig — the §2.3 configuration — and
  wrong for an antenna offset horizontally from a rig that rotates. A
  body-frame lever arm rotated by the fused attitude is the right fix and
  belongs with A11's rig calibration.
* **No PPK, no RINEX.** §3.4 defers PPK to Phase 2; nothing here forecloses
  it (the raw UTC and the arrival stamps are both kept per fix).
* **Leap seconds.** `GnssFix::utc_unix_ns` is UTC. LAS 1.4's "Adjusted
  Standard GPS Time" is GPS time (UTC + 18 s as of 2026-08) minus 1e9. A9's
  GPS-time field is currently engine-monotonic seconds; converting it needs
  the leap-second count, which is a one-line constant with an expiry date and
  should live next to whoever writes the LAS header.
* **The real-hardware pass is still open.** S5's REPORT is explicit that
  cold-start/TTFF, Bluetooth SPP quirks, multipath-driven fix flapping and
  genuine correction-age effects are hardware-only. Before M3 sign-off, run
  this client against RTK2go (protocol plumbing) and then HK SatRef
  (production-relevant) per `spikes/s5-rtk-sim/PUBLIC_CASTERS.md`.

---

## 10. Tests

`engine/tests/test_gnss.cpp` — **36 offline cases / 2 231 assertions in
0.01 s**, plus 2 skip-tagged live cases (1 489 assertions, ~40 s).

| group | cases | what |
| --- | --- | --- |
| `gnss/nmea/*` | 8 | checksum vs the spike's independent implementation; full GGA/RMC/GST/GSA/VTG decode; 9 malformed classes (no `$`, no checksum, bad hex, bad checksum, control byte, oversize, too short, empty, opt-in checksum-less); the no-fix epoch's empty position fields; all 9 GGA quality digits and 8 mode characters; chunked framing at 1/3/9/27/81-byte boundaries; binary UBX/RTCM interleaved between sentences; 1-in-5 corrupted sentences counted and skipped with the rest intact; UTC→Unix against Python `datetime`; `build_gga` round-tripped through the parser |
| `gnss/rtcm3/*` | 2 | CRC-24Q vs the spike's independent value; build/validate over 0…1023-byte payloads; resync through stray bytes and a decoy `0xD3`; a corrupted CRC counted and withheld from the rover; a truncated tail buffered not failed; message-type histogram; `age_s()` = −1 before the first frame; identical results at 4 chunk sizes |
| `gnss/crs/*` | 7 | §4's table, all of it |
| `gnss/source/*` | 5 | epoch assembly and GST vs fallback sigmas; origin anchoring and its refusal to move; the S5 fix-quality timeline reproduced; interpolation gates incl. `kTrackingLost` across a fix outage; A4 correlation vs raw arrival jitter; ring wrap; `push_pose` refusal |
| `gnss/georef/*` | 8 | exact recovery incl. a 173° initial misalignment; §5's quality-mix table with the CEP-bounds-the-error assertion; 20 gross outliers; unobservable heading; SLAM-drift inflation; scale lock/unlock; the full NMEA→fusion→CRS path with `to_global`/`to_global_points`/`to_wgs84`/`to_utm`/`crs_wkt`; the unconverged refusal; the A7 estimator-swap seam |
| `gnss/ntrip/*` | 3 | sourcetable parsing (incl. `CAS;`/`NET;` noise and a fee-bearing mount); argument/lifecycle validation and a fast failure on a closed port; the GGA upload's two sources |
| `gnsssim/*` | 2 | **live**, see below |

### The live cases

`gnsssim/live_nmea_over_tcp_into_the_gnss_source` spawns
`nmea_sim.py --mode tcp` (5 Hz, `--time-scale 25`, the default
`FIXED:60,FLOAT:20,SINGLE:10,FIXED:0` scenario), reads the socket into
`GnssSource`, and asserts:

```
live NMEA: 2955 sentences, 591 epochs, 0 checksum failures,
           fix mix 441/100/50 (Fixed/Float/Single)
trajectory bounds: E [-0.17, 40.49]  N [0, 27.50]   (sim loop is 40 x 25 m)
```

100 % of a standards-conformant stream parsed, the scripted timeline
reproduced, GST present on every epoch with mean sigmas matching the noise
the simulator actually injected (0.02 / 0.30 / 2.00 m ±20 %), the origin
anchored on an RTK-Fixed epoch, and the recovered trajectory the simulator's
real 40 × 25 m loop.

`gnsssim/live_ntrip_caster_stream_gga_upload_and_reconnect` drives
`ntrip_caster_sim` (through a ~20-line driver written to `$TMPDIR` at run
time, because the caster keeps the rover's GGA uploads in memory and prints
nothing — the spike modules themselves are imported **unmodified**) and
asserts the §8 results: sourcetable fetch, 401 and 404 handling, 60 CRC-valid
frames, 7 logged GGA uploads, one injected drop, one reconnect, the state
sequence, and a clean `disconnect()` → `connect()` → `disconnect()` cycle on
the same object.

### Verification

```
$ cmake -S engine -B build -G Ninja -DENGINE_WARNINGS_AS_ERRORS=ON
$ cmake --build build            # zero warnings from engine code
                                 # (only the vendored Livox SDK2 warns)
$ ctest --output-on-failure
  1/5 scanengine_tests ......... Passed   16.86 sec
  2/5 scanengine_capi_smoke .... Passed
  3/5 engine_cli_selftest ...... Passed
  4/5 engine_cli_version ....... Passed
  5/5 mid360_sim_e2e ........... Passed  204.43 sec
  100% tests passed out of 5

$ ./build/scanengine_tests
[doctest] test cases:     336 |     336 passed | 0 failed | 7 skipped
[doctest] assertions: 2254382 | 2254382 passed | 0 failed |

$ SCANENGINE_S5_SIM_DIR=spikes/s5-rtk-sim \
  ./build/scanengine_tests --no-skip "--test-case=gnsssim/*" --success=false
[doctest] test cases:    2 |    2 passed | 0 failed | 341 skipped
[doctest] assertions: 1489 | 1489 passed | 0 failed |     (3 consecutive runs)
```

Host: macOS 15 (Darwin 25.5.0), Apple silicon, CMake 4.4.2, Ninja, AppleClang,
Python 3.9.6. **Not verified locally:** the Windows/MSVC, Windows/clang-cl,
Linux and Android NDK legs. `src/gnss/socket_compat.h` is the only
platform-conditional code A10 added; it mirrors `transport/udp_source.cpp`'s
already-green Winsock handling and adds `getaddrinfo` + non-blocking
`connect()`, both of which are Winsock-clean. The live cases are
`#if !defined(_WIN32)`, matching the A3 precedent.
