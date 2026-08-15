# LidarScan

Dual-lidar (COIN-D6 pushbroom + Livox Mid-360 live SLAM) scanner, built as one
shared C++ engine with three consumers: an Android app, a desktop app
(macOS/Windows/Linux), and a cloud processing worker. Sensors also pair with
an RTK GNSS rover for georeferenced capture.

**Spec:** `docs/LidarScan Tech Spec.md` (v1.2.1, approved 2026-08-14) —
architecture, hardware facts, the full execution plan (workstreams A–E) and
milestones M0–M5. Read that first for *why*; this file is *where things live
and how to build them*.

---

## Layout

```
engine/   shared C++20 engine — drivers, SLAM, post-processing, GNSS/RTK,
          merge, export, jobs. C ABI (JNI) + C++ API (Qt/CLI). See below.
android/  Kotlin + Jetpack Compose app, links the engine via JNI/C ABI.
desktop/  Qt 6 Widgets + Filament app (macOS/Windows/Linux), links the
          engine's C++ API directly.
cloud/    D1 worker image (containerized engine_cli) + D2 job service
          (Python/FastAPI-style token-auth REST API, MVP/single-tenant).
spikes/   Phase 0 de-risk spikes (S1-S7) — kept as historical record, several
          (S2, S5, S6) also ship reusable simulators/fixtures the engine's
          own tests run against.
tools/    remote-capture kit (tools/remote-capture/) for collecting real
          sensor data at a hardware site when the dev machine isn't there.
docs/     the spec, bench setup/procurement/smoke-test docs (docs/bench/),
          and field validation protocol + report template (docs/field-test/).
```

---

## The engine (`engine/`)

The shared capture/SLAM/processing core. C++20, CMake + presets, `vcpkg` when
`VCPKG_ROOT` is set (CI), `find_package`/`FetchContent` fallback otherwise (a
laptop with no vcpkg still builds). Doctest for unit tests.

**Modules** (see `engine/docs/A*-*.md` for the one-doc-per-task design write-up
of each, and `engine/DESIGN.md` for the cross-cutting contract — threading,
errors, event bus, C ABI rules):

| Area | What |
| --- | --- |
| `drivers/d6`, `drivers/mid360` | COIN-D6 serial driver, Livox Mid-360 UDP driver (vendored patched SDK2, `AUTO`-detected) |
| `timesync/` | per-stream clock correlation + IMU ingestion |
| `record/` | `.lscan` container (crash-safe writer, replay reader, zip transfer bundles) |
| `slam/` | live LIO (ESKF + iVox, Mid-360), post pipeline (full-density re-run, Scan Context + hand-rolled pose-graph loop closure), D6 pushbroom assembler + mount-calibration solver |
| `gnss/` | NMEA, NTRIP client, RTCM3 framing, WGS84/UTM/CRS, georeferencing fusion |
| `merge/` | multi-session coarse align (georeferenced / manual-pick / yaw-search) + ICP refine + dedup |
| `color/`, `plan/` | camera colorization keyframe pipeline; floor-plan slice/extract/DXF/PDF |
| `export/` | PLY (binary, RGB), LAS 1.4 (georeferenced), PCD |
| `jobs/` | local job runner, cloud submit client, transfer bundle export/import |
| `cloud/`, `poses/`, `core/` | paged point store, pose-source abstractions, engine skeleton / event bus / error model |
| `capi/` | the C ABI (JNI boundary) mirroring the C++ API |
| `tools/engine_cli` | headless CLI — `--selftest`, `--synth`, `--replay`, `--post`, `--post-selftest`; this is also the cloud worker's entry point |

### Build

```sh
cd engine
cmake --preset macos-universal    # or windows-msvc-x64 / windows-clangcl-x64 / linux-x64 / android-arm64
cmake --build --preset macos-universal
ctest --preset macos-universal -LE "sim|sim-rtk"   # fast path, ~480 cases
```

`-LE "sim|sim-rtk"` skips the loopback-port simulator suites (see below) —
they are not safe on a shared/parallel runner and are excluded from every CI
build-matrix leg for that reason. Run them explicitly when you want them:

```sh
ctest --preset macos-universal -L "sim|sim-rtk"    # needs spikes/s2-mid360-sim's
                                                    # simulator built (scripts/fetch_sdk2.sh)
                                                    # and python3 for the S5 RTK sims
```

### Tests

~480 doctest cases across `engine/tests/test_*.cpp` (auto-globbed by
`CMakeLists.txt` — name a new test file `test_*.cpp` and it is picked up with
no build-file edit). Highlights:

- Per-module unit tests (`test_d6_*`, `test_mid360_driver`, `test_lio`,
  `test_post`, `test_gnss`, `test_merge`, `test_pushbroom`, `test_mount_calib`,
  `test_export`, `test_lscan_io`, …), most with a real-hardware-measured
  section against the one committed CC-BY real Mid-360+IMU capture
  (`spikes/s2-mid360-sim/fixtures/outdoor_imu_ccby_6s.livoxdump` — see that
  directory's `FIXTURES.md` for provenance and licensing).
- `test_e2_replay_golden.cpp` (workstream E2) — end-to-end replay/golden
  integration tests: a committed golden COIN-D6 byte stream and a committed
  golden Mid-360 datagram sequence, each decoded through the real driver and
  checked against hardcoded point-count/checksum/coordinate values
  (`engine/tests/integration/data/`, see that directory's `README.md` for
  provenance and how to regenerate); plus a runtime skip-unless-present LIO+
  post determinism check against the CC-BY fixture, asserting the same
  numbers `engine/docs/A6-lio.md` §7.2 / `A7-post.md` §6.3 measured (path
  length ≈ 32.4 m, \|g\| ≈ 9.816 m/s²).
- `mid360_sim_e2e` (label `sim`) / `gnss_rtk_sim_e2e` (label `sim-rtk`) —
  optional, loopback-port, only registered when their simulator/spike trees
  are present; see `engine/CMakeLists.txt`'s `ENGINE_SIM_TESTS` block.

### CI

`.github/workflows/engine-ci.yml` — 8 jobs: the 5-target build matrix
(Windows MSVC, Windows clang-cl, macOS universal, Linux x86_64, Android NDK
arm64 build-only), a dedicated macOS job that runs the `sim`/`sim-rtk`
simulator suites serially, an Android **app** build (`:core:test
:app:assembleDebug`, `continue-on-error` while the toolchain proves itself in
CI), and a desktop app build-only job. `.github/workflows/worker-image.yml`
builds + smoke-tests the cloud worker container image separately, publishing
to GHCR on tag pushes.

---

## Android app (`android/`)

Kotlin + Jetpack Compose, single-activity, Material 3. Links the engine
through the JNI/C ABI. See `android/NOTES.md` for the full toolchain/version-
pinning story (AGP 8.13.2 + Gradle 8.14.5, deliberately not bleeding-edge —
read that file before bumping either).

```sh
cd android
./gradlew :core:test          # plain-JVM unit tests, no SDK/emulator needed
./gradlew :app:assembleDebug  # full app build, needs Android SDK
```

## Desktop app (`desktop/`)

Qt 6 **Widgets** (not QML — see `desktop/NOTES.md` §1.1 for why: the Filament
viewport wants to own its own native surface, and Widgets +
`createWindowContainer()` is the integration S3 actually proved). Links the
engine's C++ API directly via `add_subdirectory`.

```sh
cd desktop
brew install qt ninja                     # macOS; see NOTES.md for Windows/Linux
./tools/fetch_filament.sh v1.75.0         # pinned — do not bump casually, see NOTES.md
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lidarscan
```

## Cloud (`cloud/`)

- `cloud/worker/` — D1: a containerized `engine_cli` (headless Linux build).
  `Dockerfile` + `scripts/validate_image.sh`; built/smoke-tested/published by
  `.github/workflows/worker-image.yml`.
- `cloud/service/` — D2: the job service (token auth, resumable upload,
  queue, worker orchestration), MVP/single-tenant per Tech Spec §3.8's
  contractual boundaries. `cd cloud/service && python3.12 -m venv .venv &&
  .venv/bin/pip install -r requirements.txt` — see that directory's own
  `README.md` for the rest.

---

## Field validation

`docs/field-test/PROTOCOL.md` — step-by-step runs for when hardware/field
access is available (indoor loop closure, corridor pushbroom, outdoor RTK
walk vs. known points, two-session merge), each with a target cited from the
relevant module's synthetic baseline. `docs/field-test/ACCURACY_REPORT_TEMPLATE.md`
is the table skeleton each run's results go into. `docs/bench/TEST_CHECKLIST.md`
(spike S4) is the lighter-weight prerequisite: wiring/enumeration/protocol-
liveness, no app code involved, meant to be re-run before every field session.

## Status

Hardware is at a remote location as of this writing; S2/S5 were de-risked via
protocol-faithful simulators (`spikes/s2-mid360-sim`, `spikes/s5-rtk-sim`) and
a remote-capture kit (`tools/remote-capture/`) collects real data at the
hardware site. See the Tech Spec's "Hardware-absent addendum" (§4) and each
module's own `engine/docs/A*.md` "What is still hardware-only" section for
exactly what remains to close with real sensors — `docs/field-test/` is where
that gets closed.
