# LidarScan — Technical Specification

**Working title:** LidarScan (placeholder — rename before release)
**Version:** 1.2.1 · 2026-08-15 (v1.2 approved 2026-08-14; .1 folds S6 spike findings into §3.3/§3.5/§5)
**Status:** ✅ Approved — Phase 0 in execution
**Scope of this spec:** Phase 0 (spikes) + Phase 1 (**Android app + Desktop app for macOS/Windows/Linux**)

---

## 1. Summary

A capture + SLAM system built around a shared C++ engine, shipping as two apps on four OS targets:

- **Android app** (Kotlin/Compose) — the handheld capture device: live SLAM preview, AR overlay, camera colorization capture, RTK rover connectivity.
- **Desktop app** (one **Qt 6 / QML** codebase) — capture *and* workstation on **macOS (universal: Intel + Apple Silicon), Windows 10/11 x64 (.exe + installer), and Linux x86_64 (AppImage + .deb)**. All three OSes speak USB serial (D6, CH340 drivers exist everywhere — the vendor's own tooling is CH340-based) and Ethernet (Mid-360) natively, so any desktop can capture; the desktop is also the primary home for heavy processing, review, floor plans, and multi-session merge.

> **UI-stack note (supersedes the earlier "SwiftUI for desktop" decision):** with Windows and Linux added, per-OS native UIs would mean three desktop codebases. Qt 6 gives one C++/QML codebase for all three, binds to the engine directly (same language, no FFI layer), and deploys native installers per OS. Qt is used under LGPLv3 with dynamic linking (no commercial license needed). Alternative considered: Compose Multiplatform (shares Kotlin skills with the Android app) — rejected because embedding a native Filament/Vulkan surface in it is fragile, and desktop rendering is this app's heaviest UI job.

Two supported sensors:

- **COIN-D6** — 2D lidar (360°, 4,000 pts/s, UART), mounted **vertically** → pushbroom profile scanning; 3D assembled from an external trajectory (ARCore VIO indoors, RTK/GNSS outdoors).
- **Livox Mid-360** — 3D lidar (200,000 pts/s, Ethernet, built-in IMU), mounted **horizontally or tilted** → lidar-inertial SLAM with live map preview.

### Product decisions (locked)

| Decision | Choice |
| --- | --- |
| Platforms (Phase 1) | **Android + Desktop (macOS universal / Windows x64 / Linux x86_64)**. iOS dropped from the roadmap. |
| Connectivity | Phase 1: **USB** (OTG serial / USB-C Ethernet on Android; native serial + Ethernet on all desktops). Phase 2: WiFi bridges (ESP32 for D6; travel-router bridge for Mid-360 — ESP32 confirmed **not viable** for Mid-360 throughput). |
| Processing | **Hybrid** live+post, plus user-selectable **"Record now, process later"**, plus a **processing-mode chooser: Local / Cloud / Extract-for-transfer** (§3.8). |
| UI stack | **Kotlin + Jetpack Compose** (Android) · **Qt 6 / QML** (desktop, one codebase, three OSes) · shared **C++17/20 engine** underneath both. |
| Renderer | **Filament** everywhere (Vulkan on Android/Windows/Linux, Metal on macOS) — one point-cloud pipeline, embedded in Qt via native window handle. |
| Audience | All four segments via **workflow profiles**: Survey · Floor plan · Research · Quick scan. |

### Phase 1 features (owner directive 2026-08-14)

1. Capture + hybrid SLAM for both sensors
2. **Camera colorization** (Android capture; processing on any platform)
3. **Floor plan extraction** (DXF/PDF)
4. **AR display overlay** (Android — live cloud anchored in camera view)
5. **NTRIP + RTK** (rover over Bluetooth/serial, NTRIP corrections, georeferenced output)
6. **Display parameter adjustment** (point size, density/LOD, color mode, EDL shading, clipping, background)
7. **Processing modes: local / cloud / extract-for-transfer**
8. **Multi-session merge**

*Interpretations to confirm:* "display detail parameters adjustment" = the render/display controls in item 6; "extract for transfer processing" = export the raw `.lscan` bundle for processing on another machine (typically a desktop). Flag if either reading is wrong.

### Phase 1 non-goals

iOS (dropped) · WiFi bridges (Phase 2) · PPK (Phase 2) · bridge firmware (Phase 2) · volumes/cross-sections (Phase 3) · store-release polish.

---

## 2. Hardware & sensor facts

### 2.1 COIN-D6 (protocol per translated vendor spec, `english coind6/`)

| Property | Value |
| --- | --- |
| Interface | UART 230400 baud, 8N1, LVTTL 3.3 V (CH340 USB-serial in vendor kit) |
| Data rate | 4,000 pts/s · 10 Hz rotation · 0.9° resolution · 0.05–12 m |
| Start / stop | `AA 55 F0 0F` / `AA 55 F5 0A` (ACK frames per spec §1) |
| Data packet | Header `0x55AA` LE · M&T byte (freq + start-packet flag) · LSN · FSA/LSA angles · 2-byte XOR checksum · 3-byte samples |
| Sample decode | `Distance = Si_H*64 + (Si_2nd>>2)` mm · `Intensity = (Si_2nd&0x03)*64 + (Si_L>>2)` · angles `(FSA>>1)/64` with LSN interpolation |
| Quirks | Speed-adjustment `0xFE/0xFF` bytes until rotation stabilizes; high-reflectivity flag; checksum XOR order is non-sequential (spec §2.2) |

### 2.2 Livox Mid-360

| Property | Value |
| --- | --- |
| Interface | 100 Mbps Ethernet, UDP (Livox SDK2, C++). S2-sim findings: **stock SDK2 does not run on macOS** (broadcast-bind fails with EADDRNOTAVAIL) — engine vendors a 3-patch SDK2 (pinned, `spikes/s2-mid360-sim/patches/`), and macOS requires an **explicit lidar IP** (no broadcast discovery). Lidar on `192.168.1.1xx`, host static on `192.168.1.x`. **Protocol doc conflict:** real devices free-run `udp_cnt` and keep `frame_cnt`=0 (verified against Livox's own .lvx2 sample), contrary to the official table — loss detection in A3 must use the free-running model |
| Data rate | ~200,000 pts/s (~22 Mbps sustained), 96-pt UDP packets |
| IMU | Built-in 6-axis @ 200 Hz |
| Power | 9–27 V, ~6.5 W — external battery required |
| FOV | 360° × 59° vertical; tilted mount recommended for floor + ceiling |

### 2.3 RTK rover (Phase 1)

Target: u-blox ZED-F9P class boards and Emlid Reach RX/RS — **NMEA 0183 over Bluetooth** (SPP on Android; BT serial profile on desktops; USB-serial fallback on desktops). NTRIP corrections fetched by the app and forwarded as RTCM3. Fix states surfaced: RTK Fixed / Float / DGPS / Single / none.

### 2.4 Phase 1 test kit

Android phone (USB-C OTG, Vulkan, ARCore — Pixel 7+/Galaxy S22+ class) · Apple-silicon Mac **and** one Intel Mac (or CI-only x86_64 build verification) · Windows 10/11 x64 PC · Linux x86_64 box (Ubuntu 22.04+ reference) · CH340 USB-serial adapter · USB-C Ethernet adapter (AX88179/RTL8153) · powered USB-C hub · 12 V battery + barrel connector for Mid-360 · RTK rover + antenna + NTRIP account · rigid phone+lidar mount bracket (D6 vertical / Mid-360 tilted) with camera view unobstructed.

---

## 3. System architecture

```
┌──────────────────────────┐   ┌───────────────────────────────┐
│ Android (Kotlin/Compose) │   │ Desktop (Qt 6 / QML)          │
│ capture · AR overlay ·   │   │ macOS ∪ · Windows · Linux     │
│ camera pipeline · RTK UI │   │ capture · workstation:        │
│                          │   │ review · merge · floor plans  │
├───────── JNI ────────────┤   ├──── direct C++ linkage ───────┤
│                                                              │
│        ENGINE  libscanengine  (C++17/20, CMake)              │
│                                                              │
│  transport/  UsbSerialSource · UdpSource · BtNmeaSource      │
│  drivers/    d6/ · mid360/ (Livox SDK2 wrapper)              │
│  timesync/   monotonic mapping, per-stream offsets           │
│  record/     .lscan container (always-on, crash-safe)        │
│  poses/      PoseSource: ARCore · GNSS/RTK (fused)           │
│  gnss/       NTRIP client · NMEA/RTCM · georef fusion        │
│  slam/       live/ ESKF LIO · post/ full LIO + PGO ·         │
│              pushbroom/ D6 assembler                         │
│  color/      camera frame ingest · extrinsics · projection   │
│  plan/       slice → lines → floor plan → DXF/PDF            │
│  merge/      multi-session registration (georef + ICP)       │
│  cloud/      voxel-hashed store, LOD, display params         │
│  export/     PLY · LAS 1.4 · PCD · DXF · PDF                 │
│  jobs/       processing modes: local · cloud · transfer      │
├──────────────────────────────────────────────────────────────┤
│  RENDERER  Filament (Vulkan / Metal) — one shared pipeline   │
└──────────────────────────────────────────────────────────────┘
          ▲ same engine, headless CLI build (Linux)
          └── CLOUD WORKER: container runs engine CLI on
              uploaded .lscan; minimal job REST service
```

**Key rules**

1. **Engine owns all sensor data.** UIs pass opaque buffers in (serial bytes, camera frames, NMEA) and get render handles + status events out. Android consumes a small **C ABI** over JNI; the Qt app links the engine's C++ API directly (same process, no FFI).
2. **Record-always.** Raw streams (lidar, IMU, camera frames, poses, GNSS) hit the `.lscan` container before any processing; live SLAM is just one consumer. Replay == capture — which is what makes local/cloud/transfer processing the *same pipeline in three places*.
3. **Pose sources are pluggable and fusable.** ARCore VIO, GNSS/RTK, and LIO poses feed one fusion layer (§3.4).
4. **Portable C++.** Engine builds for Android arm64, macOS arm64 **and x86_64** (universal), Windows x64 (MSVC or clang-cl), Linux x86_64 — all in CI from day one; the Linux build doubles as the cloud worker.

### 3.1 Connectivity (Phase 1, USB) — per platform

| | D6 (serial) | Mid-360 (Ethernet) |
| --- | --- | --- |
| Android | `usb-serial-for-android` (CH340) → JNI; permission/attach flow | USB-C Ethernet; `ConnectivityManager` `TRANSPORT_ETHERNET` + `Network.bindSocket`; static-IP wizard + per-OEM guidance + pre-capture self-test |
| macOS | `QSerialPort` — CH340 in-box on modern macOS | native Ethernet/USB-C adapter; manual IP UI + self-test |
| Windows | `QSerialPort` (COM port); CH340 driver install guided in setup wizard (vendor driver ships in `english coind6/4 Windows Host Software/CH340 Driver/`) | native Ethernet; adapter static-IP guidance + self-test |
| Linux | `QSerialPort` (`/dev/ttyUSB*`); udev rule installed by package (vendor ships `sc_mini.rules` precedent) | native Ethernet; NetworkManager static-IP guidance + self-test |

RTK rover: Bluetooth SPP (Android), Qt Bluetooth / OS BT-serial or USB-serial (desktops); NMEA in, RTCM3 out.

### 3.2 Time sync

Single monotonic engine clock (`CLOCK_BOOTTIME` / `mach_absolute_time` / `QueryPerformanceCounter`). Arrival-stamping + per-stream device-timestamp offset with drift-tracked median filter. Camera frames use ARCore timestamps (same clock domain as ARCore poses — why colorization capture is Android-side). GNSS: NMEA time via arrival correlation (sufficient at walking speed; PPK/gPTP deferred).

### 3.3 SLAM pipelines

**Mid-360 live:** ESKF lidar-inertial odometry (Point-LIO/FAST-LIO2 family): IMU propagation @200 Hz, iterated update against incremental voxel map (iVox-style), scan-to-map @10 Hz, input decimated to ~40k pts/s live. Budget ≤ 2 big cores on Android; effectively unconstrained on desktop.

**Mid-360 post:** full-density LIO re-run → Scan Context loop candidates → GTSAM pose-graph optimization → re-integration → voxel dedup/outlier filter. Foreground service (Android) / background task (desktop), cancellable, progress-reported.

**D6 pushbroom:** rigid phone+lidar mount; trajectory from the pose fusion layer (ARCore indoors, RTK outdoors, blended when both). Mount-extrinsics wizard using a **planar checkerboard target** (S6 finding: corner/doorframe capture is geometrically unusable for a 2D scanner — the sampled slice slides along the corner line, so it is not a repeatable world point). Points during ARCore tracking loss are flagged and excluded by default. Desktop D6 capture: no ARCore → RTK-trajectory or fixed-position profile mode only (bench/tripod use), stated in UI.

### 3.4 GNSS/RTK + pose fusion (Phase 1)

- **NTRIP client** in engine: caster connect, mountpoint list, RTCM3 forward to rover; reconnect logic; corrections-age surfaced.
- **Fusion:** factor graph (GTSAM) with LIO/VIO odometry factors + GNSS position factors weighted by fix quality; continuously estimates the local↔global similarity transform. Output: georeferenced trajectory and cloud in the project CRS.
- **CRS support:** EPSG picker (survey profile), WGS84/UTM defaults, coarse EGM96 geoid bundled — survey-grade geoid models Phase 2.
- D6 outdoor mode: RTK **is** the trajectory source; capture UX warns/blocks below fix-quality threshold.

### 3.5 Camera colorization

- **Capture (Android):** CameraX shared with ARCore session; keyframes JPEG-recorded at 2–5 fps with poses + intrinsics into `.lscan/streams/frames/`.
- **Calibration:** camera↔lidar extrinsics from the mount wizard — **planar checkerboard target for both sensors** (A1 0.80×0.60 m minimum, ≥8 poses incl. roll variation; quality gate = split-half agreement, not solver covariance). S6 findings: Mid-360 colorization is GO with ≤5 ms sync jitter (≤15 ms with motion-gated keyframe selection); D6 colorization requires a bench calibration (~45 poses) and measured range noise ≤~10 mm 1σ — verify against S1 hardware measurement. Mitigations pulled into A11: constant clock-offset estimation in the wizard (8 s sweep), rolling-shutter per-row time model, motion-gated keyframe selection. Hardware time-sync stays out of Phase 1 (software mitigations dominate the budget).
- **Processing (any platform):** per point, best-view keyframe selection (angle/distance/occlusion z-buffer), color sample, RGB into final cloud; exports carry RGB in PLY/LAS.
- Desktop-captured sessions have no camera → colorization gracefully unavailable for them.

### 3.6 Floor plan extraction

Gravity-aligned cloud → horizontal slice band (default 1.0–1.5 m, configurable) → 2D occupancy → RANSAC line extraction + merge → optional orthogonality snapping → wall polylines + opening heuristics → **DXF** (layered polylines) and **PDF** (scaled sheet with dimensions). Editor v1: slice-height slider + include/exclude regions; CAD-grade editing is Phase 3. Primary UX on desktop; view + export on Android.

### 3.7 AR display overlay (Android)

Live captured cloud rendered anchored in the ARCore camera view during capture (shared Filament scene, camera-image background) — coverage gaps visible on the spot. Toggle AR view ↔ free-orbit 3D. Reuses the ARCore session already required for D6/colorization.

### 3.8 Processing modes

| Mode | What happens | Where |
| --- | --- | --- |
| **Local** | Post pipeline runs on this device | Android (foreground service) or any desktop |
| **Cloud** | `.lscan` zip (resumable upload) → job service → Linux worker runs engine CLI → results downloaded into project | Minimal REST job service + object storage + containerized worker (§4 workstream D) |
| **Extract for transfer** | Project exported as `.lscan.zip` via share sheet / USB / network share (AirDrop where macOS is the receiver); desktop app imports via drag-drop or file association, processes, exports a results bundle back | No server involved |

Cloud MVP boundaries (Phase 1): single-tenant (owner's account), token auth, no payments/quotas, one worker instance, hard upload-size cap. Productionizing is Phase 2+.

### 3.9 Display parameter adjustment

Render settings panel on all platforms, applied live: point size (px + adaptive), density/LOD budget, color mode (RGB/height/intensity/time/fix-quality), gamma/brightness, EDL toggle + strength, background, height/box clipping, trajectory + pose-graph overlays. Settings persist per project; profiles set defaults.

### 3.10 Multi-session merge

- **Coarse:** automatic when sessions are georeferenced (shared CRS); otherwise manual 3-point / drag alignment.
- **Refine:** voxel-downsampled point-to-plane ICP per pair; optional global relaxation for >2 sessions.
- **Merge:** unified cloud with per-session provenance, voxel dedup, combined export. Primary UX: desktop **merge workbench** (session list, pair alignment, residual report); Android offers georeferenced auto-merge only.

### 3.11 `.lscan` container

```
MyScan.lscan/
  manifest.json                  sensor, profile, mount calib, CRS, versions
  streams/
    lidar.bin  imu.bin  poses_ar.bin  gnss.bin      framed: [len][type][t_mono][payload][crc32]
    frames/    keyframe JPEGs + frames.idx (pose, intrinsics, t)
  processed/   live_preview.lod · final.cloud · plan.dxf/pdf
  merged/      merge graphs + results
  exports/     user exports
```

Crash-safe writer (truncated tails skipped on read); forward-compatible chunk types; the zip of this directory is the cloud-upload and transfer unit.

### 3.12 Rendering

Filament everywhere: Vulkan on Android/Windows/Linux, Metal on macOS; embedded in Qt via native window handle (`QWindow::winId()` → Filament swapchain), Compose `SurfaceView` on Android. One C++ point pipeline: paged streaming buffers (~1 M pts/page), coarse-to-fine LOD, EDL post-effect, camera-feed background (AR mode). Targets: 2 M on-screen pts @60 fps (Pixel 8 class); 10 M+ @60 fps (Apple silicon / desktop dGPU); degrade via LOD, never framerate.

### 3.13 App structure

**Android (capture-first):** Projects · Device setup wizard · Mount calibration · Capture (live 3D / AR overlay toggle, Live-SLAM vs Record-only, RTK status strip) · RTK setup · Processing (mode chooser, queue) · Review (viewer, display params, measure, plan view, export) · Settings/profiles.

**Desktop (Qt, capture + workstation):** Projects library (drag-drop / file-association import) · Capture window (both sensors, same wizards, per-OS driver guidance) · Processing queue (local; cloud submit) · Review workspace (large-cloud viewer, display params, measure) · Floor plan workspace (slice controls, DXF/PDF) · **Merge workbench** · Export center · per-OS packaging: notarized universal DMG (macOS), NSIS/MSIX installer incl. CH340 driver pointer (Windows), AppImage + .deb with udev rule (Linux).

---

## 4. Execution plan — tasks with agent assignments

**Delegation model:** Claude (this session) orchestrates and integrates; **Opus 5** takes architecture-critical / high-uncertainty tasks; **Sonnet 5** takes well-specified implementation against interfaces Opus has fixed; a rolling Opus review gate covers Sonnet engine-adjacent merges.

> **Hardware-absent addendum (2026-08-15):** sensors are at a remote location. S2/S5 were executed as simulator variants (`s2-mid360-sim`: protocol-faithful Mid-360 simulator + patched SDK2 10-min loopback soak at 200k pts/s clean; `s5-rtk-sim`: NMEA/NTRIP/RTCM3 simulation infra, 10/10 self-tests). A remote-capture kit (`tools/remote-capture/lidarscan-capture-kit.zip`) collects real D6/Mid-360/GNSS data at the hardware site; returned captures close the remaining live exit criteria (S1 checksum variant + D6 noise σ, S2 real-transport soak, S5 real fixes). Public datasets (1.3 GB incl. Livox official .lvx2 and a CC-BY Mid-360+IMU set) downloaded for A6/E2 development against real scan patterns.

### Phase 0 — de-risk spikes (gate for Phase 1 build-out)

| ID | Task | Agent | Exit criteria |
| --- | --- | --- | --- |
| S1 | D6 USB bring-up: CH340 read → C++ parser (Android + one desktop OS) | **Opus 5** | Live polar plot; checksum pass >99.5%; start/stop ACKs verified |
| S2 | Mid-360 on NDK: SDK2 arm64, Ethernet-transport binding, point+IMU streams | **Opus 5** | 200k pts/s sustained 10 min, no packet-loss growth, IMU @200 Hz |
| S3 | Render throughput: Filament streaming points — Android, **and Filament-in-Qt embed on macOS + Windows or Linux** | **Opus 5** | 2 M pts @60 fps (Android ref) · 10 M pts @60 fps (desktop); Qt embed stable across resize/screen-change |
| S4 | Bench setup: procurement list, wiring, repeatable rig doc | **Sonnet 5** | Hardware ordered/verified; setup doc with photos |
| S5 | RTK bench validation: BT NMEA from rover, NTRIP loop, fix states | **Sonnet 5** | RTK Fixed on bench; NMEA+RTCM logs captured for tests |
| S6 | Camera↔lidar calibration feasibility: extrinsics solve + reprojection quality on rig | **Opus 5** | Reprojection error quantified; go/no-go on wizard design |
| S7 | **Windows toolchain spike**: engine + GTSAM + Livox SDK2 + Filament building on MSVC/clang-cl | **Sonnet 5** | Green Windows CI build of engine + unit tests |

### Workstream A — Engine (C++)

| ID | Task | Agent | Depends on |
| --- | --- | --- | --- |
| A1 | Engine skeleton: CMake, module layout, C ABI (JNI) + C++ API (Qt), event bus, error model, **4-platform CI targets** | **Opus 5** | S1–S3, S7 |
| A2 | D6 driver hardening: reconnect, health, speed-adjust filtering, fault states | **Sonnet 5** | A1 |
| A3 | Mid-360 driver productionization: SDK2 wrapper, config, health | **Opus 5** | A1 |
| A4 | Time-sync module + IMU ingestion (per-OS clock backends) | **Opus 5** | A1 |
| A5 | `.lscan` container (incl. camera-frame + GNSS streams), crash-safety, replay harness | **Sonnet 5** | A1 |
| A6 | Live LIO (Mid-360): ESKF + iVox, decimation, perf budget | **Opus 5** | A3, A4, A5 |
| A7 | Post pipeline: full LIO re-run, Scan Context + GTSAM PGO, final cloud | **Opus 5** | A6 |
| A8 | D6 pushbroom assembler + mount-calibration solver | **Opus 5** | A2, A4, A5 |
| A9 | Export writers: PLY (binary, RGB), LAS 1.4 (georef), PCD + reference-reader tests | **Sonnet 5** | A5 |
| A10 | **GNSS/RTK**: NTRIP client, NMEA/RTCM, factor-graph georef fusion, CRS/EPSG | **Opus 5** | A4, S5 |
| A11 | **Colorization**: frame ingest, extrinsics calibration (S6 result), best-view projection, RGB write | **Opus 5** | A5, A7, S6 |
| A12 | **Floor plan**: slice, line extraction, snapping (Opus) + DXF/PDF writers (Sonnet, split PR) | **Opus 5 + Sonnet 5** | A7 |
| A13 | **Multi-session merge**: georef coarse align, manual-align API, ICP refine, dedup, residual report | **Opus 5** | A7, A10 |
| A14 | Display-parameter API: LOD budget, EDL, clipping, color modes, persistence | **Sonnet 5** | A1, S3 |
| A15 | Jobs module: local runner, cloud submit client, transfer bundle export/import | **Sonnet 5** | A5, A7 |

### Workstream B — Android app

| ID | Task | Agent | Depends on |
| --- | --- | --- | --- |
| B1 | Scaffold: nav, theming, Projects, `.lscan` management | **Sonnet 5** | — |
| B2 | D6 connect flow: USB permission UX, attach intents, health panel | **Sonnet 5** | A2 |
| B3 | Mid-360 connect flow: Ethernet transport, static-IP wizard, OEM variance, self-test | **Opus 5** | A3 |
| B4 | Capture screen: Filament view, status strip, Live/Record-only toggle, pause/resume | **Sonnet 5** | A1, S3 |
| B5 | Profiles + Settings | **Sonnet 5** | B1 |
| B6 | Processing UI: mode chooser, foreground service, queue, Review screen, export sheet | **Sonnet 5** | A15, A9 |
| B7 | ARCore: session mgmt, mount-calibration wizard, **AR overlay view** | **Opus 5** | A8, A14 |
| B8 | Camera keyframe pipeline (CameraX + ARCore timestamps → `.lscan`) | **Sonnet 5** | B7, A5 |
| B9 | RTK UI: rover BT pairing, NTRIP config, fix-status strip, capture gating | **Sonnet 5** | A10 |
| B10 | Display-parameters panel | **Sonnet 5** | A14 |
| B11 | Measure tool + floor-plan viewer (view/export) | **Sonnet 5** | B6, A12 |
| B12 | Georeferenced auto-merge flow (minimal) | **Sonnet 5** | A13 |

### Workstream C — Desktop app (Qt 6 / QML — macOS universal, Windows, Linux)

| ID | Task | Agent | Depends on |
| --- | --- | --- | --- |
| C1 | App scaffold: Qt project, engine linkage, **Filament-in-Qt render view** (from S3), window/session model | **Opus 5** | A1, S3 |
| C2 | Capture flows: QSerialPort D6 + Ethernet Mid-360, per-OS setup wizards + self-tests, health panels | **Sonnet 5** | C1, A2, A3 |
| C3 | Review workspace: large-cloud viewer, display params, measure, exports | **Sonnet 5** | C1, A9, A14 |
| C4 | Processing queue: local runs, cloud submit, progress | **Sonnet 5** | C1, A15 |
| C5 | Floor plan workspace: slice controls, preview, DXF/PDF export | **Sonnet 5** | A12, C3 |
| C6 | **Merge workbench**: session list, 3-point align UI, ICP refine, residual report, merged export | **Sonnet 5** (UI over A13) | A13, C3 |
| C7 | Transfer import: file association, drag-drop, results-bundle export | **Sonnet 5** | A15, C1 |
| C8 | **Packaging**: notarized universal DMG (Intel + Apple Silicon) · Windows NSIS/MSIX installer + CH340 driver guidance · Linux AppImage + .deb with udev rule | **Sonnet 5** | C1–C7 |

### Workstream D — Cloud processing (MVP)

| ID | Task | Agent | Depends on |
| --- | --- | --- | --- |
| D1 | Engine CLI: headless Linux build, containerized worker image | **Sonnet 5** | A7 |
| D2 | Job service: token auth, resumable upload to object storage, queue, worker orchestration, result download; single-tenant MVP | **Opus 5** | D1 |
| D3 | App-side cloud client (Android + desktop) | **Sonnet 5** | D2, A15 |

### Workstream E — Quality & infra

| ID | Task | Agent | Depends on |
| --- | --- | --- | --- |
| E1 | CI matrix: Android NDK · macOS **arm64 + x86_64** · Windows x64 · Linux x86_64; engine unit tests; clang-tidy/ASan | **Sonnet 5** | A1, S7 |
| E2 | Replay integration tests: golden `.lscan` datasets → deterministic pipeline assertions (SLAM, colorization, plan, merge) | **Sonnet 5** | A5–A13 rolling |
| E3 | Field-test protocol + accuracy report: indoor loop, corridor, outdoor RTK walk vs. known points, merge residuals | **Sonnet 5** | M3 |
| E4 | **Opus review gate**: rolling review of Sonnet engine-adjacent merges (A2, A5, A9, A12b, A14, A15, D1, E2, S7) | **Opus 5** | rolling |

### Milestones

| Milestone | Contents | Definition of done |
| --- | --- | --- |
| **M0** | S1–S7 | All spike exit criteria met on real hardware; Windows engine CI green |
| **M1 Capture** | A1–A5, B1–B4, C1–C2 | Both sensors: connect → capture → `.lscan` → raw live view on Android **and** on macOS/Windows/Linux desktop builds; record-only mode |
| **M2 Live SLAM + AR** | A6, A8, B7, B8 | Mid-360 live map; D6 pushbroom (ARCore); AR overlay during capture; calibration wizards |
| **M3 RTK** | A10, B9, E3 partial | RTK-fused georeferenced capture; fix-quality gating; LAS in chosen CRS opens correctly in CloudCompare/QGIS |
| **M4 Processing suite** | A7, A9, A11–A15, B6, B10–B12, C3–C7, D1–D3 | Post + loop closure; colorized clouds; floor plan DXF/PDF; local/cloud/transfer all functional; merge workbench produces a clean 2-session merge |
| **M5 Beta** | B5, C8, E1–E3, polish | Field protocol passes; ≥30 min crash-free capture; internal beta: APK + notarized DMG + Windows installer + AppImage |

---

## 5. Risks

| Risk | Severity | Mitigation |
| --- | --- | --- |
| **Phase 1 scope** (~3× the v1.0 plan, now 4 OS targets) | **High** | Strict M1→M5 order keeps a working product at every gate; features behind flags; cut-line candidates: D2 cloud service (transfer covers it), B12, Windows/Linux may trail macOS within M1 without blocking M2 |
| Windows toolchain (GTSAM/SDK2/Filament on MSVC) | High | Dedicated spike S7 **before** Phase 1 commit; clang-cl fallback; vendored pinned deps |
| Filament-embedded-in-Qt (S3 result: **GO on macOS** — 10M pts @138 fps on M4, 1,105 swapchain recreates crash-free; requires CAMetalLayer shim, not the NSView Filament documents; pin Filament v1.75.0) | Medium (was Med-High) | Residual: Windows/Linux Vulkan leg unproven — run with S7's first CI push before A1 closes; `gl_PointSize` needs an instanced-quad Vulkan fallback plan; multi-monitor DPI-change untested |
| OEM USB-Ethernet variance (Mid-360 on Android) | High | Opus-assigned B3, self-test wizard, tested-device list; Phase 2 WiFi bridge is the universal fallback |
| Live LIO thermals on mid-range phones | High | Hybrid degrades to Record-only; decimation budget; perf gates S3/A6 |
| **Time-sync jitter** dominating colorization error (S6: at 15 ms jitter, sync alone eats 83% of the reprojection budget; extrinsic calibration itself is solved) | Med-High | A11 software mitigations: wizard clock-offset sweep, rolling-shutter row model, motion-gated keyframes; AR overlay unaffected (GO at all jitter levels) |
| ARCore tracking loss (D6 indoors, overlay) | Medium | Confidence UX, flagged points; RTK takes over outdoors |
| RTK/NMEA device variance | Medium | S5 bench validation; support matrix starts F9P + Emlid only |
| Cloud service scope creep | Medium | §3.8 MVP boundaries are contractual |
| D6 protocol edge cases | Low-Med | S1 against real device; vendor Windows tool as reference |

---

## 6. Phase 2/3 preview

**Phase 2** — WiFi bridges (ESP32-D6 firmware; router-bridge Mid-360 flow), PPK, survey-grade geoid models, cloud productionization (multi-user, quotas). **Phase 3** — volumes/cross-sections, CAD-grade plan editing, coverage-gap guidance, ROS bag export, store releases. iOS: dropped; revisit only on explicit owner decision.

---

## 7. Approval gate

No agents are launched and no code is written until this revision is approved. On approval, execution starts with **Phase 0: S1, S3, S6 (Opus 5) + S4, S7 (Sonnet 5)** immediately; **S2** waits for the Mid-360 + Ethernet adapter on the bench; **S5** waits for the RTK rover hardware.
