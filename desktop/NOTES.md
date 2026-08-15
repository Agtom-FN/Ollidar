# C1 — LidarScan desktop app scaffold

**Status: done and running on macOS.** `desktop/` is a Qt 6 application that links
`libscanengine`'s C++ API directly, embeds a productionized version of the S3 spike's
Filament renderer, opens `.lscan` projects through the engine's `record/` module, and
replays a recorded capture back through the engine into the viewport live.

Spec references are to `docs/LidarScan Tech Spec.md` v1.2.1, `engine/DESIGN.md` (A1),
`engine/docs/A5-lscan.md`, `engine/include/scanengine/cloud/display_params.h` (A14) and
`spikes/s3-render/REPORT.md` (S3).

```
desktop/
  CMakeLists.txt              Qt 6 + Filament + add_subdirectory(../engine)
                               + C8's universal/.app-bundle path
  tools/                      C8 build+packaging scripts (§13):
    fetch_filament.sh          harvested from S3; pins v1.75.0 (arm64 prebuilt)
    build_filament_x86_64.sh   the x86_64 slice the release tarball omits
    make_universal_filament.sh lipo the two into filament-universal/
    fetch_qt_universal.sh      official universal Qt (brew's is arm64-only)
    make_icon.sh               .icns/.png/.ico, stdlib-only PNG writer
    package_macos.sh           universal .app + macdeployqt + codesign + DMG
  packaging/                  installer inputs, per OS — see packaging/README.md
    PACKAGING.md               the procedure + real signing/notarization
    Info.plist.in              C7 wrote it, C8 wired it into the bundle
    macos/ windows/ linux/ ci/
  materials/points.mat        harvested from S3, extended to A14's uniform contract
  scripts/verify.sh           the evidence run in §6, end to end
  scripts/display-params-intensity.json
  src/
    main.cpp                  app entry + the CLI that drives verify.sh
    app/
      MainWindow.{h,cpp}      shell: projects library, viewport, docks, status bar, menus
      EngineHost.{h,cpp}      the single owner of scanengine::Engine; event pump; health
      Project.{h,cpp}         .lscan read / create / import, all through record/
      ReplayController.{h,cpp} lscan::ReplaySource on a worker thread
      DisplayParamsDock.{h,cpp} §3.9 panel bound to A14's DisplayParamsController
      CaptureWindow.{h,cpp}   guided self-test → Record/Pause/Stop (C2, §8.1-8.3)
      MeasureDock.{h,cpp}     C3 §3.13 measure panel: mode toggle, units, segment list (§8.4)
      ExportDialog.{h,cpp}    C3 §3.13 export center: PLY/LAS/PCD, progress + cancel (§8.5)
      ProcessingDock.{h,cpp}  C4 §3.8 processing queue: job list + Post-process/
                               Colorize/Export/Transfer/Cloud submit (§9)
      PlanDock.{h,cpp}        C5 §3.6 floor-plan workspace: extraction + 2D
                               QGraphicsView + editor controls + DXF/PDF (§10)
      QtHttpTransport.{h,cpp} C4: a real scanengine::jobs::HttpTransport over
                               QNetworkAccessManager (§9.2)
      SyntheticMid360.{h,cpp} C4 evidence fixture: a real .lscan with real
                               kMid360Points/kMid360Imu chunks, no hardware (§9.4)
      SyntheticBuilding.{h,cpp} C5 evidence fixture: A12's own synthetic
                               two-room-plus-corridor test building (§10.5)
      MergeDock.{h,cpp}       C6 §3.10 merge workbench: session list, coarse
                               align (georef/3-point/yaw), ICP refine + a
                               QPainter residual chart, build + colour-by-
                               session, export (§11)
      MergeFixture.{h,cpp}    C6 evidence fixture: the 3-session overlapping
                               building engine/tests/test_merge.cpp measures
                               A13 against, ported like SyntheticMid360/
                               SyntheticBuilding were (§11.2)
      MergeSessionLoader.{h,cpp} C6: a real .lscan directory into a merge
                               session's cloud via a private Engine + unpaced
                               ReplaySource (§11.1)
      TransferDialog.{h,cpp}  C7 §3.8/§3.13 transfer bundle: Export/Import
                               dialogs over A5's zip_export()/zip_import(),
                               real progress + cancel (INT-34 hooks) (§12)
    render/
      NativeSurface.h         per-OS swapchain-handle shim (the S3 recommendation)
      NativeSurface_mac.mm    CAMetalLayer path — the proven one
      NativeSurface_win.cpp   HWND, UNVERIFIED, with S3 §8 inline
      NativeSurface_linux.cpp X11/XCB/Wayland analysis, UNVERIFIED, S3 §8 inline
      DisplayLink.h           render clock interface (replaces S3's spin loop)
      DisplayLink_mac.mm      CADisplayLink (macOS 14+) → CVDisplayLink → timer
      DisplayLink_generic.cpp Windows/Linux: the timer fallback, with the real
      DisplayLinkFallback.*   per-platform replacements documented
      ViewportWindow.{h,cpp}  QWindow + Filament engine/view/renderer, resize, screenshots,
                               C3 measure-tool picking + marker rendering (§8.4)
      PagedCloudRenderer.*    GPU mirror of scanengine::PageStore
```

~5,000 lines including comments.

---

## 1. Architecture decisions

### 1.1 Qt Widgets, not QML — and why that is a deviation worth taking

Tech Spec §1 says "Qt 6 / QML". **This app is Qt Widgets** with the 3D viewport as a native
child `QWindow` embedded through `QWidget::createWindowContainer()`. The reason is the
viewport, and it is the same reason S3 built its spike that way:

* Filament owns a **`CAMetalLayer`** (macOS) or a `VkSurfaceKHR` (elsewhere) and drives its
  own swapchain, its own command submission and its own present. Qt Quick's scene graph
  wants to own exactly those things for the window it renders into. Putting Filament inside
  a `QQuickWindow` means either a `QSGRenderNode` sharing a Metal command queue and
  render-pass state with Qt's renderer, or a second native surface manually positioned over
  the Quick scene — the first is a version-coupled integration against Qt's private
  graphics abstraction (QRhi), the second reintroduces every problem an embedded native
  window has *plus* Quick's compositing on top.
* The thing S3 actually proved — 138 fps at 10M points, 1,105 swapchain recreates without
  an artefact — was proved through `createWindowContainer()`. Choosing QML would have meant
  throwing away the one integration with measured evidence behind it and re-derisking it.
* Everything else the desktop needs (dock widgets, a menu bar, native file dialogs, a
  status bar, `QTreeWidget`) is what Widgets is for. §3.13's desktop surface is a
  workstation, not an animated touch UI.

**This is not a one-way door.** `QQuickWidget` drops a QML scene into a Widgets layout, so
any individual panel (the merge workbench's alignment UI, a floor-plan editor) can be QML
without touching the viewport. If the decision is to be reversed wholesale, the isolated
pieces are `MainWindow` and `DisplayParamsDock`; `ViewportWindow`, `PagedCloudRenderer`,
`NativeSurface` and `DisplayLink` contain no widget code at all. Flag it if the QML wording
in §1 was a hard requirement rather than a "one codebase, three OSes" requirement — the
latter is fully satisfied here.

### 1.2 Engine linkage

`add_subdirectory(../engine)` and `target_link_libraries(lidarscan PRIVATE scanengine)`.
The desktop uses the **C++ API only** — `capi/scanengine_c.h` is for JNI and is not
included anywhere here, per DESIGN.md §4.

**One integration finding that every future Qt-plus-engine target inherits:**
`QT_NO_KEYWORDS` is **mandatory**, not stylistic. `engine/include/scanengine/drivers/d6/d6_parser.h`
declares `void emit(const Point&)` — the S1 parser kept byte-identical on purpose — and
Qt's `emit` macro turns that declaration into a syntax error in any translation unit that
includes both. Every desktop source therefore uses `Q_EMIT` / `Q_SIGNALS` / `Q_SLOTS`. It is
set in `CMakeLists.txt` with that explanation next to it.

### 1.3 Threading

| Thread | What runs on it |
| --- | --- |
| GUI thread | Qt, **all** Filament calls, the display-link tick, the PageStore→GPU sync, `EngineHost`'s 30 Hz event drain, and (for the D6) `QSerialPort::readyRead` → `Engine::push_serial_bytes()` |
| Replay thread | `lscan::ReplaySource::run()` → `Engine::push_serial_bytes()` |
| Engine-owned | whatever the engine's own modules create (A3's SDK2 receive threads, A6's odometry) — the app never sees them |

Filament's `Engine` is created, used and destroyed on one thread for the process lifetime,
so there is no swapchain/resize race by construction. The D6 decode running synchronously
on the GUI thread is deliberate and bounded: DESIGN.md §2 makes the whole decode path run
on the pushing thread, and 4,000 pts/s is ~23 kB/s — measured CPU cost per render tick,
*including* the PageStore sync, is **0.05–0.24 ms p95**. A Mid-360 at 200k pts/s is *not*
pumped this way; its bytes never touch the app (the driver owns its own receive threads).

### 1.4 How points get from the engine to the GPU

`PagedCloudRenderer` **polls** `PageStore` every frame: `page_ids()` → `page_view(id)` →
upload `[uploaded, count)` → remember `count`. It does not follow `kPointsAvailable` events.

That is a deliberate reading of DESIGN.md §5: a subscriber that sees `kEventsDropped` "must
stop trusting incremental ranges and re-read the pages". Re-reading the pages is the only
path that is correct under backpressure, so this renderer *always* takes it — one code
path, rather than a drop-recovery branch that only ever executes under load and therefore
only ever breaks in the field. It is lock-free and legal because `PageView::data` never
moves and `count` is published with a release store. The event stream drives UI only
(device state, session state, rotations, errors).

Productionization deltas from S3's `PagedCloud`:

* **one shared identity `IndexBuffer`** for all pages instead of one per page. Filament
  requires an index buffer even for `POINTS`; at 1 M points that is 4 MB each, so sharing
  saves 4 MB per page (36 MB at S3's 10M run).
* **real per-page bounding boxes** from `PageView::bounds_*`, so frustum culling culls. S3
  deliberately gave every page the whole room to measure the worst case.
* **A14's `lod_point_budget`** applied as a soft throttle: pages past the budget stay
  resident but leave the scene. Walking pages in `PageId` order makes that deterministic
  and chronological rather than a frame-to-frame flicker between arbitrary pages. This is
  **not** §3.12's coarse-to-fine LOD — see §7.

### 1.5 Render loop

S3's loop was a spinning `QTimer(0)` that burned ~83% of a core retrying `beginFrame()`;
REPORT §7 lists replacing it as C1 work. `DisplayLink` does that, preferring in order:

1. **`CADisplayLink` from the `NSView`** (macOS 14+) — Apple's current API, fires on the
   main run loop, follows the display the view is on including ProMotion. This is what runs
   on this machine. (`CVDisplayLink` is deprecated as of macOS 15.)
2. **`CVDisplayLink`** — callback on the CV thread, forwarded to the GUI thread by a queued
   invocation behind a **coalescing** flag: if the GUI thread has not consumed the previous
   tick, the new one is counted and dropped instead of queued, so a slow frame cannot build
   a backlog. It also re-targets `CVDisplayLinkSetCurrentCGDisplay()` when the window moves
   between monitors.
3. **A timer at the display's refresh interval** — the current Windows/Linux path.

Result: **59.2–60.0 fps vsync-locked** with **0.05–0.24 ms p95 CPU per frame**.

### 1.6 Resize

`resizeEvent` only sets a flag. The render tick applies at most **one** swapchain
destroy/recreate per frame, because S3 measured ~8–9 ms of CPU per recreate and REPORT §5
explicitly says a production app must coalesce. `QWindow::screenChanged` forces a full
reconfigure (backing scale, drawable size, display-link target) — the multi-monitor/DPI
path S3 called out as its "main residual macOS risk" is at least *written*; it is still not
*tested*, because this machine has one display (§7).

### 1.7 Display parameters (A14)

A14 landed its header during this task and its implementation shortly after; the desktop
binds to **the real thing** — `DisplayParams`, `DisplayParamsController`, `to_uniforms()`,
`colormap_lut()`, `profile_defaults()`, `to_json()`/`from_json()`. There is no mirror
struct and no parallel display model on this side. (A temporary local implementation of
A14's function bodies existed for about an hour while only the header was present; it was
deleted the moment `engine/src/cloud/display_params.cpp` appeared. `CMakeLists.txt` now
hard-fails if it finds the header without the implementation, so a half-landed A14 gives a
readable error instead of undefined symbols.)

Two details worth knowing:

* **The shader does not reimplement A14's colormaps.** A14 exposes `colormap_lut()` as a
  256-entry table precisely so a shader can sample it; the app uploads all three colormaps
  as one 256×3 RGBA8 texture and `points.mat` samples row `colormap`. The shader therefore
  agrees with `evaluate_point_color()` **by construction** instead of by two copies of the
  same maths being kept in sync by hand.
* **Colour management.** Everything upstream — `PointVertex`'s RGBA8, the colormap LUTs,
  `evaluate_point_color()`'s output — is authored in **display (sRGB)** space, but Filament
  treats `material.baseColor` as **linear** and encodes to sRGB at the end. The material
  converts sRGB→linear before writing `baseColor`, and the skybox background gets the same
  conversion in C++. Without it every cloud renders washed out and the panel's numbers stop
  meaning anything.

`auto_range` is the renderer's job per A14's header ("the caller (renderer) is expected to
refresh manual_min/manual_max every frame ... from the actual data range"). Height uses
`PageView::bounds_min[2]/bounds_max[2]`. Intensity has no equivalent in `PageView`, so
`PagedCloudRenderer` accumulates a 256-bin luminance histogram during the upload memcpy and
reports the **1st/99th percentile**, not min/max — a lidar scan routinely contains a handful
of saturated returns off retroreflective tape (the engine's own synthetic room has a band at
intensity 255 among points at ~25), and stretching the ramp over min..max pushes every real
return into the bottom few percent of the ramp, i.e. renders the whole cloud black. See §7
for the case where 1%/99% is still not robust enough.

---

## 2. What the app does today

**Projects library.** Open / create / import `.lscan` directories, recents persisted via
`QSettings`. Everything goes through `lscan::FileRecordReader` / `FileRecordWriter`, so the
desktop cannot drift from the container the Android app and the cloud worker write. The
project panel shows per-stream chunk counts, payload bytes and time spans from
`stream_summaries()`, the reader's `warnings()` (truncated tails, CRC mismatches, unreadable
streams), and whether the manifest is **sealed** — A5 §2 makes an unsealed manifest a
*positive* crash signal, so the UI says "NOT SEALED (session ended abnormally)" rather than
hiding it.

**Import raw D6 → project.** A raw byte capture (`engine_cli --synth`, or a logged port) is
pushed through a live `Engine` session with `push_serial_bytes()`, so the recorded chunks
are byte-identical to a real capture's. Arrival stamps are synthesised at the D6's wire rate
(230400 8N1 = 23,040 B/s) rather than stamped "now" — the file is consumed in milliseconds
and stamping now would collapse the whole capture into one instant, making a 1× replay
meaningless.

**Replay.** `lscan::ReplaySource` on a worker thread, into a session with an *empty*
`lscan_dir` (a live preview that records nothing — recording a replay into the project being
replayed would append the same bytes back into the file being read). Speed 1.0 reproduces
the capture's pacing; 0 is unpaced. Points appear in the viewport as they decode.

**Viewport.** Orbit / pan / zoom, auto-orbit, auto-fit on first data, `F` to refit, vsync
toggle, screenshots.

**Display-parameter dock.** Every §3.9 control, bound to the A14 controller. EDL and the
trajectory/pose-graph overlays are present, persisted, and **disabled with a tooltip saying
why** rather than pretending to work (§7).

**Capture window.** D6: `QSerialPortInfo` enumeration with vendor/product ids, 230400 8N1,
a real `QSerialPort` whose `readyRead` feeds `push_serial_bytes()` and whose write path is
installed as `D6Config::serial.write_fn` so the driver can send its own `AA 55 F0 0F` /
`AA 55 F5 0A` frames. Mid-360: host/lidar IP + ports into `Mid360Config`, with the S2
finding (an explicit lidar IP is *required* on macOS — broadcast discovery fails with
EADDRNOTAVAIL) stated in the panel and enforced. Live / Record-only toggle. Per-OS driver
guidance text compiled per platform. **Untested against hardware — none is present** (§7).

**Status bar.** Engine state, session state, per-device state + points + checksum pass rate,
PageStore totals and dropped-point count, and the renderer's fps / CPU p95 / GPU p95 /
drawable size / DPR / swapchain-recreate count.

---

## 3. Per-OS seams (what C8 packaging has to close)

### 3.1 Rendering

| | macOS | Windows | Linux |
| --- | --- | --- | --- |
| Backend | Metal — **proven** (S3 + this task) | Vulkan — **unproven** | Vulkan — **unproven** |
| Surface type | `QSurface::MetalSurface` | `QSurface::VulkanSurface` | `QSurface::VulkanSurface` |
| Swapchain handle | `CAMetalLayer` found under `winId()`'s `NSView` (Filament's docs say `NSView`; that aborts — S3 §3) | `HWND` from `winId()` — documented and *believed* fine | X11 `Window` from `winId()`; XCB needs `SwapChain::CONFIG_ENABLE_XCB`; **Wayland is refused with an explanatory error** because it needs a `wl_surface` + explicit size |
| Resize | destroy + recreate swapchain (`needsSwapChainRecreateOnResize() == true`) | assumed handled internally — **first thing to test** | same assumption |
| Render clock | `CADisplayLink` / `CVDisplayLink` | timer at refresh rate; real fix is DXGI waitable object / `VK_KHR_present_wait` | timer; real fix is `wp_presentation_feedback` or `VK_KHR_present_wait` |
| `gl_PointSize` | works | needs `largePoints`; may clamp to 1.0 | historically weak on Intel/AMD; may clamp |

`NativeSurface_{win,linux}.cpp` carry S3 §8's analysis inline, verbatim where it matters, so
whoever finishes them inherits the reasoning rather than a citation. The app shows the
renderer as unverified (`isVerifiedPlatform()`) on those platforms rather than implying S3
covered them. If `gl_PointSize` fails on Vulkan, the fallback is billboard-quad expansion in
the vertex shader (4–6× the vertex work) — that is exactly what A14's
`PointSizeMode::kWorldSize` exists to express, and the change touches only `points.mat` plus
`PagedCloudRenderer`'s primitive type.

### 3.2 Filament distribution

`tools/fetch_filament.sh` pins **v1.75.0** and puts the release under `third_party/`
(gitignored, 131 MB unpacked). The pin is load-bearing: S3 found Filament's own docs wrong
in two places in the same release (the `NSView` swapchain handle, and a README link list
missing `libzstd`, which `libfilament` now hard-needs).

**A real C8 blocker found here:** the `filament-v1.75.0-mac.tgz` release ships **`lib/arm64`
only** — there is no `lib/x86_64` in the macOS tarball. §3.13 requires a *universal* DMG
(Intel + Apple Silicon), and the engine already builds universal via its
`macos-universal` preset. So C8 must either obtain/build x86_64 Filament static libs and
`lipo` them together, or ship two DMGs, or drop Intel. This is not a packaging detail that
can be discovered late — flag it now.

> **RESOLVED by C8 — see §13.1.** Owner decision: build v1.75.0 x86_64 from source
> (`tools/build_filament_x86_64.sh`, 2m 00s) and `lipo` it against the prebuilt arm64
> set (`tools/make_universal_filament.sh`). One universal DMG, Intel kept.
> C8 found the same "Filament's own layout is not what you'd guess" trap twice more:
> the ninja target is `filament-abseil`, not `abseil`, and `geometry`/`abseil` ship as
> `lib<name>_combined.a` — the plain target output for `filament-abseil` is an **empty
> 656-byte archive**.

### 3.3 Packaging (C8)

**Done — §13, and `packaging/PACKAGING.md` is the procedure.** The list below is C1's
original statement of the work; each row now says where it landed.

* **macOS** — the executable is currently a plain binary, not a `.app` bundle. C8 needs
  `MACOSX_BUNDLE`, `Info.plist`, `macdeployqt`, codesign + notarize, and must place
  `points.filamat` inside `Contents/Resources` (the app resolves it via
  `QCoreApplication::applicationDirPath()`, which is the one line that changes).
  → **all done and verified** (§13.4, §13.5) except notarization, which needs an Apple
  Developer ID that does not exist in this environment; the DMG is ad-hoc signed and
  `spctl` says `rejected`, recorded rather than hidden.
* **Windows** — NSIS/MSIX, `windeployqt`, plus the CH340 driver pointer the capture window
  already names. → **NSIS script + PowerShell build script + driver page staged**, syntax-
  validated by `makensis` only; never built or run (§13.6).
* **Linux** — AppImage + `.deb`, `linuxdeployqt`, and the udev rule for `/dev/ttyUSB*` that
  the capture window's hint text already tells the user about. → **staged** (§13.6), with
  `linuxdeploy` rather than `linuxdeployqt` (the latter refuses to run on a modern CI image).
* Qt is used under **LGPLv3 with dynamic linking** (§1), which every packaging path must
  preserve — no static Qt. → **preserved**: Qt frameworks live in `Contents/Frameworks`.
  Note the new constraint C8 hit: **Homebrew Qt is arm64-only**, so a universal build has
  to use the Qt Company's own (universal since 6.2) binaries — §13.2.

---

## 4. Where the other tasks plug in

| Task | Seam |
| --- | --- |
| **C2** capture flows | `CaptureWindow` — wizards, self-tests, reconnect UX, per-OS driver install guidance. The engine wiring (`addD6`/`addMid360`/`startSession`, the serial read+write paths) is done; C2 owns the UX around it. Also: move the D6 read loop to its own thread if a future device is faster than the D6. |
| **C3** review workspace | `ViewportWindow` + `PagedCloudRenderer`. Measure tools attach to the viewport; the EDL post-process pass, real coarse-to-fine LOD and the trajectory/pose-graph overlays are the three renderer features C1 left as documented gaps. Export UI hangs off `export/exporter.h`. |
| **C4** processing queue | **Done, §9.** `ProcessingDock` — driven by `Engine::jobs()` (INT-34, landed mid-task), polled rather than subscribed to `kJobProgress` (still falls through `describeEvent()`'s default case; not needed since `list()` already has every update). |
| **C5** floor plan | **Done, §10.** `PlanDock` — the height-clip band in `DisplayParams` is a *display* clip (viewport-only); the plan's own slice band is a completely separate `plan::PlanEditState`, per §10.1. |
| **C6** merge workbench | **Done, §11.** `MergeDock` over A13's `MergeProject`; colour-by-session is done by overwriting `MergeResult::cloud`'s RGBA per run-table range before `publish()` — no renderer redesign needed, exactly as this row predicted. |
| **C7** transfer import | **Done, §12.** `TransferExportDialog`/`TransferImportDialog` over `lscan::zip_export`/`zip_import` + `Project.cpp`'s `readProject`; file association (`QEvent::FileOpen`) and drag-drop are on `MainWindow`. |
| **C8** packaging | **Done, §13.** macOS universal DMG is real (built, mounted, launched on both slices); Windows/Linux are staged scripts. Procedure: `packaging/PACKAGING.md`. |
| **B10** Android params panel | Same `DisplayParamsController`; `points.mat`'s fragment shader is the reference implementation of the uniform contract (B10 consumes the same fields as a std140 UBO). |
| **A3/A10** | The moment `Engine` grows a push entry point for Mid-360/GNSS raw streams, `ReplayController` replays those too — `ReplayConfig::chunk_type` is already the only thing that changes. |
| **A14** | `lod_point_budget` currently throttles by dropping whole pages; a real LOD policy in the engine would replace `PagedCloudRenderer::sync()`'s budget branch. |

---

## 5. Build

```sh
brew install qt cmake ninja
cd desktop
./tools/fetch_filament.sh v1.75.0
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/lidarscan
```

`--help` lists the CLI. `scripts/verify.sh` runs the whole evidence sequence.

That is the **development build**: arm64-only, Homebrew Qt, a plain binary. To build the
**shippable universal `.app` + DMG** instead (C8, §13):

```sh
cd desktop
./tools/fetch_filament.sh v1.75.0          # arm64 Filament   (prebuilt)
./tools/build_filament_x86_64.sh v1.75.0   # x86_64 Filament  (from source, ~7 min)
./tools/make_universal_filament.sh         # lipo -> third_party/filament-universal/
./tools/fetch_qt_universal.sh 6.11.1       # official UNIVERSAL Qt (brew's is arm64-only)
./tools/make_icon.sh
./tools/package_macos.sh                   # -> dist/LidarScan-0.1.0-universal.dmg
```

Full procedure, including real signing/notarization: `packaging/PACKAGING.md`.

---

## 6. Verification

Host: **Apple M4**, macOS 26.5.1, single 1920×1080 @ 60 Hz display, Qt **6.11.1**,
CMake 4.4.2, Ninja 1.13.2, AppleClang 21, Filament **v1.75.0** (Metal, arm64),
`scanengine 0.1.0 (clock: mach_absolute_time)`.

Everything below is reproducible with `./scripts/verify.sh`; its raw output is
`evidence/verify.log`. Screenshots are `Renderer::readPixels()` off the swapchain Qt's
`QMetalLayer` owns — the literal pixels Filament put in the Qt window. That is S3's
mechanism (REPORT §6) and it is kept because it has better provenance than an OS screen
capture *and* it works in a shell with no Screen Recording permission.

**Build:** clean configure + build from scratch, engine included: **exit 0, and zero
warnings from any file under `desktop/src`** (`-Wall -Wextra`). The 239 warnings the build
does emit are all from the vendored Livox SDK2 under `engine/third_party/`, which this task
does not own.

**Engine → project:** `engine_cli --synth` wrote 30 s / 394,287 bytes of synthetic COIN-D6
wire traffic (including the `0xFE/0xFF` speed-adjustment and garbage bytes `--noise`
injects). The app imported it through `Engine::push_serial_bytes()` into
`evidence/synth.lscan`: **120,300 points decoded**, 193 chunks, 385.0 KB in
`streams/lidar.bin`, 17.07 s span, `manifest.json` `"sealed": true`, reader reports **no
warnings**.

**Replay → viewport:**

| Run | Points in viewport | fps | CPU p95 | GPU p95 | Drawable | Swapchain recreates |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| 01 unpaced replay, complete | 120,300 | **59.2** | 0.23 ms | 1.97 ms | 918×913 @ dpr 1.0 | 0 |
| 02 4× replay, caught **mid-stream** | **75,589** | 58.7 | 0.05 ms | 0.84 ms | 918×913 | 0 |
| 03 different `DisplayParams` (A14 `from_json`) | 120,300 | 59.5 | 0.24 ms | 2.05 ms | 918×913 | 0 |
| 04 during/after a 10 s resize storm | 120,300 | 40.6 | 0.21 ms | 1.94 ms | 918×913 | **250** |
| 05 HiDPI, `QT_SCALE_FACTOR=2` | 120,300 | **59.7** | 0.24 ms | 1.93 ms | **960×1412 @ dpr 2.0** | 0 |

* Run 02 is the one that proves the viewport is **streaming**: the same project, caught at
  75,589 of 120,300 points because the replay was paced. A preloaded cloud cannot do that.
* Run 03 is the same cloud under a `DisplayParams` document loaded through A14's own
  `from_json()` — intensity colour mode, thermal colormap, explicit 0.04–0.15 range,
  different background, `lod_point_budget` 20 M. The dock shows every value it parsed and
  the cloud is visibly, completely different. That is the proof the
  `DisplayParams → DisplayParamsUniforms → Filament MaterialInstance` binding is live.
* Run 04 reruns S3's stability stress on the productionized path: continuous resize for
  10 s → **373 resize events coalesced into 250 swapchain destroy/recreate cycles, no
  crash, no validation error, no artefact**, and the app returns to 60 fps afterwards.
  Frame rate during the storm fell to ~38–40 fps while **render-tick CPU stayed at 0.21 ms
  p95** — i.e. the cost is Qt re-laying-out a full dock-laden `QMainWindow`, not the
  renderer. (S3 held 60 fps through its storm with a bare window and no docks; that is the
  difference.)
* Run 05 exercises the DPI path: `devicePixelRatio` 2.0, a 960×1412 physical drawable, Qt
  chrome at 2×, Filament's viewport matching, 59.7 fps. No blur, no half-size image.

**Screenshots** (`evidence/`): `NN-*.png` is Filament's own framebuffer; `NN-*-window.png`
is that composited with `QWidget::grab()` of the main window, i.e. what the user sees. The
central area of the raw Qt grab is **empty**, which is itself the proof the 3D content is a
native child window and not something Qt painted.

**Engine health surfaced correctly, including a failure:** after an unpaced replay ends, the
D6 driver's stall watchdog fires (`streaming -> degraded [device not responding]`) because
no more bytes arrive. That is A2's watchdog behaving exactly as specified, and the status
bar and log show it. It is left visible rather than suppressed.

### Not verified

* **No hardware.** No COIN-D6 and no Mid-360 were connected. `QSerialPortInfo` enumeration
  runs (this machine has no serial adapters, so it reports none), the `QSerialPort` open /
  read / write path is real code on a real Qt class, and the engine wiring is exercised by
  the replay path — but **"a real D6 on a serial port would work" is an expectation, not a
  measurement**. Same for the Mid-360 tab: it configures `Mid360Config` correctly and calls
  `add_device`, and nothing beyond that has been observed.
* **Windows and Linux are not built, let alone run.** The two `NativeSurface` files compile
  only on their own platforms and have never been compiled by anyone. S3's exit criterion
  ("macOS **+** Windows or Linux") remains half-met, exactly as REPORT §8 left it.
* **Multi-monitor / DPI change** is written (`screenChanged` → full reconfigure,
  `CVDisplayLinkSetCurrentCGDisplay`) but not tested: one display on this machine. This is
  the same residual risk S3 flagged and it is still open.
* **Scale.** The largest cloud rendered here is 120,300 points — the whole 30 s synthetic
  D6 capture. S3 measured 10 M points at 138 fps with the same buffer layout and material
  family, so the headroom is known from there, not from this task.
* **No long soak.** Longest continuous run is ~15 s.
* **EDL is not implemented** and no LOD beyond the page-dropping throttle.

---

## 7. Known gaps, deliberately

| Gap | Why / what it needs |
| --- | --- |
| **EDL post-effect** (§3.12) | A full-screen post-process, not a point-material effect; S3 §7 left its cost unmeasured. The parameter is stored, persisted and shown **disabled with a tooltip**. C3 + a measurement. |
| **Coarse-to-fine LOD** (§3.12) | `lod_point_budget` currently drops whole pages out of the scene past the budget. Real LOD needs a spatial structure (octree/voxel) that belongs in the engine's `cloud/`, not in the renderer. |
| **Trajectory / pose-graph overlays** | Nothing produces a trajectory yet (A8/A7). Checkboxes are stored, persisted, disabled with a tooltip. |
| **Intensity auto-range robustness** | The 1st/99th-percentile range is much better than min/max, but the engine's own synthetic room has a saturated band that is **1.1%** of all points — just wide enough to land on the 99th percentile. A percentile pair is a heuristic; a proper answer is a histogram-mode-based range, and it arguably belongs in A14 next to `evaluate_point_color()` so both apps get it. |
| **Viewport-side auto-range does not write back to the dock** | The viewport keeps its own `DisplayParams` copy and retargets `manual_min/max` there, so the panel shows the stored (usually 0..1) range while the render uses the data range. Correct output, confusing UI. The fix is for the viewport to push the retargeted range back into the `DisplayParamsController`, which needs a "don't recurse" guard. |
| ~~**`points.filamat` is loaded from `applicationDirPath()`**~~ | ~~Fine for a dev build, wrong for a bundle — one line, and it is C8's to change.~~ **Closed by C8 (§13.4):** `ViewportWindow.cpp` searches `applicationDirPath()` then `../Resources`, so one binary works in both layouts. |
| **No unit tests on the desktop side** | The verification here is an end-to-end run. A headless test of `Project.cpp` (read/create/import round trip) needs no GPU and is the obvious first one. |
| **Record-only mode** | The toggle exists and is documented; what it does today is stop the viewport mirroring the PageStore. The engine still fills the PageStore, which for a long capture is the wrong behaviour — A14's eviction policy is the real fix. |

---

## 8. C2 (capture flows) + C3 (review workspace)

New/changed files:

```
src/app/CaptureWindow.{h,cpp}   rewritten: guided self-test, Test/Record/Pause/Stop
                                 state machine, auto-refreshing CH340-hinted port
                                 picker, structured health line, per-project Mid-360
                                 settings, session summary
src/app/MeasureDock.{h,cpp}     new: C3 measure panel (mode toggle, units, segment
                                 list + delete/clear, per DisplayParamsDock's shape)
src/app/ExportDialog.{h,cpp}    new: C3 export center (PLY/LAS/PCD, options, progress
                                 + cancel, open-containing-folder)
src/render/ViewportWindow.{h,cpp}
                                 + measure tool: CPU pick against PageStore, ESC
                                 clears the pending pick, marker/segment rendering
                                 reusing points.mat through a second MaterialInstance
src/app/MainWindow.{h,cpp}      wires MeasureDock + Export…, profile-default-on-open,
                                 auto-persist-on-close, captureWindow() accessor
src/main.cpp                    CLI evidence hooks: --export, --measure-selftest,
                                 --mid360-selftest, --mid360-record-into, --new-project
scripts/verify_c2c3.sh          the evidence run in §8.5, end to end
scripts/check_ply.py            from-scratch PLY reader (§8.5.2) — deliberately does
                                 not reuse A9's writer or its own test-file reader
```

### 8.1 Capture flow: self-test before Record, not Start-means-Record

The C1 skeleton had one `Start capture` button that opened the device and began
recording in the same click. C2's guided flow is a small state machine
(`CaptureWindow::Phase`): `kIdle -> kTesting -> kReady -> kRecording <-> kPaused`.

* **Test device** starts a **live-preview session** (`EngineHost::startSession`
  with an empty `lscan_dir`, `record=false` — the exact pattern
  `ReplayController` already used), then opens the device (D6 serial /
  Mid-360 `Mid360Config`). Because the session is already active,
  `Engine::add_device()` auto-starts the driver immediately
  (`core/engine.cpp`'s documented "a device added mid-session starts
  immediately" rule) — no separate "start" call exists on the public API to
  drive.
* The self-test window polls `Engine::device_health()` on the existing 300 ms
  timer. D6: pass if the observed rate over a 3 s window is **≥ 3,000 pts/s**
  (a margin below the ~4,000 pts/s nominal rate the task's guided-step text
  names, to tolerate rotation-speed variance). Mid-360: pass on **first
  data**, fail on **no packet within the window** (8 s in the UI, matching
  the self-test's own timeout — A3's own `connect_timeout_ms` default is
  10 s for the *engine's* watchdog, which is a different, longer-horizon
  timer).
* **Record** stops the preview session and starts a real one at the chosen
  `.lscan` directory (`record=true`). The device is **not** removed and
  re-added — `Engine::start_session()` restarts every still-registered
  device, so the serial port / UDP configuration is never redone between
  phases. The same restart pattern is Pause (stop recording session, start a
  new preview session — device keeps streaming to the viewport, nothing more
  hits disk) and Resume (stop preview, start recording again at the **same**
  directory). `record/lscan.h`'s writer appends on every `open()` and never
  rewrites a completed chunk, so a paused capture's stream files simply grow
  across each resume — the manifest's `sealed` flag toggling false/true each
  time is A5's crash-signal contract working exactly as designed, not a bug
  in this flow.

### 8.2 Session summary: what it can and cannot honestly report

`Engine::recorder().stats()` (`RecordStats`) is reset to zero by
`FileRecordWriter::open()` (`src/record/lscan.cpp`), so a paused/resumed
capture's stats do not accumulate across the pause boundary by themselves.
`CaptureWindow::accumulateRecorderStats()` snapshots `stats()` immediately
before each session switch and adds it into a running total
(`cum_bytes_written_`/`cum_chunks_written_`), so the final summary is a true
sum across every recording segment, not just the last one.

**What "points" in the summary means, precisely.** The generic
`DeviceHealth::points_out` counter is the driver's lifetime count, not
"points written to disk" — it keeps incrementing while paused (the device
keeps streaming to the viewport during a pause; it just is not being
recorded). There is no per-driver "points actually recorded" counter on the
public `Engine`/`Driver` API. The summary label says this explicitly
("points decoded since Record ... device counters also include any paused
time") rather than presenting a number with more precision than the API can
back up.

### 8.3 The generic `DeviceHealth` ceiling, found while wiring the health panel

The task asked the capture window to poll "state, pts/s, rotation Hz,
checksum rate, stall/restart counters" from the engine. The first four are
on `core/types.h`'s `DeviceHealth` and are exactly what
`CaptureWindow::updateHealth()` renders. **The fifth is not reachable.**
`D6HealthSnapshot` (restart attempts, stall kind, checksum-variant verdict)
and `Mid360Stats` (link state, watchdog trips, forced re-inits) are real,
richer structs `D6Driver::snapshot()` / `Mid360Driver::stats()` expose —
but `Engine` stores every device behind the base `Driver*` interface
(`impl_->devices` in `core/engine.cpp`) with no accessor for the concrete
driver type, and no C++ API method forwards those richer structs. So the
desktop's health panel can show `DeviceState::kDegraded` plus
`DeviceHealth::last_error` (which *is* enough to say "stalled" and give a
reason) and `DeviceHealth::drops`, but cannot show a restart-attempt counter
or `Mid360LinkState` by name. `CaptureWindow::updateHealth()`'s DEGRADED line
says so inline rather than silently under-delivering. **This is a real,
small seam for core/A2/A3 to close** — e.g. an
`Engine::device_extended_health(id) -> variant<D6HealthSnapshot,
Mid360Stats>` or two kind-specific accessors — not something C2 can add from
`desktop/` (outside this task's ownership; `engine/` is being edited
concurrently by two other agents this pass).

### 8.4 Measure tool

`ViewportWindow` gained the picking/state (`setMeasureMode`,
`measurements()`, `removeMeasurement()`, `clearMeasurements()`); `MeasureDock`
is a thin panel over it, the same split `DisplayParamsDock` uses against
`DisplayParamsController`.

* **Picking** is a plain O(N) nearest-point-to-ray scan over every resident
  page (`ViewportWindow::pickPoint()`) — the ray is built by hand from
  `azimuth_/elevation_/distance_/target_` and the vertical FOV (the same
  numbers `updateCamera()` uses), not by extracting Filament's camera
  matrices, so it has no dependency on which Filament APIs happen to expose
  view/projection. Tolerance is a screen-pixel radius (10 px) converted to a
  world-space radius at each candidate point's own depth. No spatial index:
  fine at the scale this was verified against (120,300 points, sub-frame);
  flagged in the header as the thing to revisit for a multi-million-point
  cloud.
* **Rendering** reuses `points.mat` through a **second `MaterialInstance`**
  forced to `ColorMode::kRgb` + `PointSizeMode::kFixedPixels` (9 px)
  regardless of the dock's live display parameters — built via a local
  `DisplayParams` + `to_uniforms()`, not a hand-rolled parallel uniform list,
  so it can never drift from what `points.mat` actually expects. A pending
  pick is one yellow point; a completed segment is 32 cyan points linearly
  interpolated between its endpoints (i.e. a point-sampled "line" — no new
  material/shader, no `LINES` topology). No new `.filamat` was needed.
* **ESC** (`ViewportWindow::keyPressEvent`) clears only the pending pick; the
  completed segment list is `MeasureDock`'s delete/clear-all buttons' job, as
  the task specified.
* Units (m/ft) are a `QSettings` preference (`measure/imperial`) — there is
  no engine or per-project concept of a display unit; `DisplayParams` and
  `PointVertex` are metric-only by design.

### 8.5 Export dialog

`ExportDialog` drives `scanengine::export_points()` directly (the same entry
point `Exporter`/`make_exporter()` delegate to internally, per
`docs/A9-export.md`) on its own `std::thread`, polling an `atomic<float>`
progress from a `QTimer` — the same shape `ReplayController` already uses
for its worker thread, chosen for the same reason: the export thread must
never touch a `QWidget`. Cancel is `ExportCancelToken::request_cancel()`,
polled by the writer every 4096 raw points per A9's own doc.

"Bounds from current clipping" snapshots the `DisplayParams` the dialog was
constructed with (not a live binding to the dock — stated in the header and
enforced by `MainWindow::onExport()` always constructing a **fresh**
dialog rather than reusing a cached one, so a second "Export…" always
reflects whatever is clipped *right now*). Box clipping maps straight onto
`ExportOptions::bounds_filter`; height-only clipping maps onto a box with an
effectively-unbounded X/Y extent and the height band on Z. The checkbox is
disabled (with a tooltip explaining why) when neither clip is active.

"Open containing folder" is per-OS: `open -R` (macOS, reveals + selects),
`explorer.exe /select,` (Windows), `QDesktopServices::openUrl` on the parent
directory (Linux — no reveal-and-select primitive there).

### 8.6 Display parameters: profile default on open, auto-persist on close

Both pieces already existed as manual actions in C1 (`loadFromProject`/
`saveToProject`, and a File-menu "Save display parameters to project" action)
— C2/C3's job was making them automatic:

* `MainWindow::openProject()` now tries `params_dock_->loadFromProject()`
  first; **only if that fails** (no `processed/display_params.json` yet —
  i.e. a project that has never had its display parameters saved) does it
  fall back to `scanengine::profile_defaults()` for whatever workflow
  profile the manifest declares (`ProjectInfo::profile`, matched
  case-insensitively against `to_string(DisplayProfile)`, exactly the
  pattern `main.cpp`'s existing `--display-profile` flag already used),
  defaulting to `kQuickScan` if the manifest names none/an unknown one.
* `MainWindow::persistDisplayParamsIfProjectOpen()` is now called from
  `closeProject()`, from `openProject()` (before swapping the model out from
  under the *previous* project), and from a new `closeEvent()` override (so
  quitting the app with a project open does not silently lose whatever the
  dock ended up at).

**Verified precisely** (§8.7.4): a fresh `floorplan`-profile project opened
with no saved file gets **exactly** A14's documented Floor-plan defaults
(fixed 1.5 px, 8 M budget, height/thermal colormap, height clip 1.0–1.5 m
enabled, EDL on at 0.7, both overlays off) and that file is written on quit
with no explicit save action; hand-editing one field and reopening proves
`loadFromProject()` wins over the profile fallback once a file exists, and
that the auto-save on the next quit does not clobber it back to defaults.

### 8.7 Verification (2026-08-15, same host as §6: Apple M4, macOS 26.5.1)

`scripts/verify_c2c3.sh` reproduces all of this; raw output is
`evidence/verify_c2c3.log`. Two real bugs were caught and fixed by this run
before it went green — noted here because they are exactly the kind of thing
a headless CLI hook is for:

* `scripts/check_ply.py` double-advanced its read offset (added `stride`
  *and* the trailing property sizes again) — fixed; the corrected reader now
  agrees with the writer's own accounting exactly.
* `--measure-selftest`'s first version clicked two fixed screen pixels; a D6
  capture at this stage is a thin room-*outline* trace (pushbroom assembly
  is A8's job), so a fixed guess landed on empty space more often than not.
  Replaced with an 8×8 screen-space grid walk that stops at the first two
  hits — same `pickPoint()` code path, just not betting on one guess.

**Build:** clean configure + build from scratch, zero warnings from any file
under `desktop/src` (`-Wall -Wextra`); the 239 warnings the build does emit
are all vendored (`engine/third_party/Livox-SDK2`), same as §6's baseline.

**Engine tree churn during this task (report, not fix, per ownership rules):**
`engine/` is being edited concurrently by two other agents this pass
(core/capi, and A7's `slam/post/` + A10's `gnss/`). A clean rebuild from
scratch hit three different transient breakages outside `desktop/` over the
course of this task (`capi/scanengine_c.cpp`'s `SCAN_ABI_VERSION`
mismatched mid-edit; `slam/post/post_pipeline.cpp` referencing a
not-yet-added `corrected_pose_lookup_ok`; `gnss/ntrip_client.cpp` with a
syntactically invalid placeholder alias declaration) — each resolved itself
within the next few minutes of that agent's own work, and the final clean
build above is green. None of these three files are under this task's
ownership and none were touched to fix them.

#### 8.7.1 Measure tool

`--measure-selftest` sends two real `QMouseEvent`s through
`ViewportWindow`'s actual event path (not a private-method call) against the
C1 synthetic D6 capture (120,300 points, a ~4×3 m room outline at z=0):

```
[lidarscan] measure-selftest: segment (1.500,-1.500,0.000) -> (2.000,0.903,0.000) = 2.4541 m
```

`evidence/06-measure.png` (below) shows the resulting cyan point-sampled
segment against the (very dim — raw D6 intensity at RGB colour mode, same
reason §6's own screenshots don't try to make this cloud bright) room
outline.

#### 8.7.2 Export round trip

The C1 synthetic capture, replayed (`--replay=0`) into the live PageStore
and exported three ways via `--export FORMAT:PATH` (the exact
`export_points()` call `ExportDialog` makes):

| Format | Bytes | Notes |
| --- | ---: | --- |
| PLY (binary) | 1,925,061 | re-imported below |
| LAS 1.4 | 3,128,653 | format 2 (RGB, no GPS time — `las_gps_time=false`, no real time source yet per A9-export.md) |
| PCD (binary) | 2,406,200 | |

`scripts/check_ply.py` (a reader written from the PLY spec, independent of
A9's writer and of `test_export.cpp`'s own from-scratch reader — the same
"two independent implementations must agree" principle one level up) against
the PLY export:

```
OK: evidence/export-c2c3.ply: 120300 points, properties=['x', 'y', 'z', 'red', 'green', 'blue', 'intensity'],
    bounds=(-2.001,-1.500,0.000)..(2.001,1.500,0.000)
```

120,300 exactly matches the import's own "120300 points decoded" report
(§6); the body byte count matches the header's declared count and stride
exactly (no truncation, no extra bytes); every x/y/z is finite and inside
the capture's own bounds. `--expect-points 120300` was also passed and
matched.

#### 8.7.3 Mid-360 self-test + capture, against the S2 simulator on loopback

Built `spikes/s2-mid360-sim`'s `mid360_sim` (already present in this tree —
see engine/docs/A3-mid360-driver.md §7, which this run reproduces from the
desktop side rather than through `ctest -L sim`). Loopback config per
`engine/tests/test_mid360_driver.cpp`'s documented quirk: `lidar_ip =
127.0.0.1`, `host_ip = 127.000.000.001` (numerically identical to
`127.0.0.1`, a different *string*, which is what slips past the SDK's
self-IP filter on loopback — **not for production config**).

`--mid360-selftest 127.000.000.001:127.0.0.1` drives `CaptureWindow`'s real
guided self-test (`engine add_device` + `start`, auto-started because the
preview session is already active, first-data-or-timeout):

```
[scanengine][info][mid360] device 1: connected (sn=3GGDJ6K00100001 ip=127.0.0.1)
[scanengine][info][mid360] handle 16777343: configured (sn=3GGDJ6K00100001 ip=127.0.0.1)
[scanengine][info][mid360] device 1: starting -> streaming
[lidarscan] mid360-selftest PASSED — first packet after 1.63 s
```

1.63 s matches A3's own measured loopback handshake time (§7: "Handshake to
first packet: 1.45 s") to the same order of magnitude. Two runs, 1.59 s and
1.63 s.

**`--mid360-record-into` then drove the full guided flow — Test → Record →
Stop — and found a real engine gap.** The session summary correctly reported
what actually happened:

```
[lidarscan] mid360-selftest: recorded project — 0 chunks, 0 bytes, 0.00 s span, sealed=true
```

**This is not a desktop bug.** `Engine::push_serial_bytes()`
(`core/engine.cpp`) is the *only* place anything calls
`recorder().write_chunk()`, and it is D6-only
(`lscan::ChunkType::kD6Raw`, unconditionally). The Mid-360 driver owns its
own sockets and never goes through `push_serial_bytes()` — its point/IMU
packets reach `PageStore` (confirmed live: `device_health().points_out`
climbed during the self-test, which is how the 1.6 s pass time was measured
at all) and the IMU sink, but nothing anywhere calls
`recorder().write_chunk(ChunkType::kMid360Points, ...)` or
`kMid360Imu`. `record/lscan.h` already reserves both chunk types and
documents that ".lscan Mid-360 chunks are stored as unmodified datagrams"
(`mid360_driver.h`'s own `kInject` backend doc), so the format side is
ready — the wiring from driver packet callback (or a `PageStore`/event
subscriber, the same shape the existing IMU shim in `core/engine.cpp` uses
for `on_mid360_imu`) into `Engine::recorder()` is simply not there yet.

**Net effect today:** a Mid-360 capture through this desktop app **streams
live to the viewport correctly** (verified) but **does not persist raw data
to the `.lscan`** (verified — a real, reproducible finding, not a guess).
The D6 path has no such gap: `push_serial_bytes()` writes the chunk before
parsing it, every time. **This is a core/A3 seam, flagged here rather than
fixed** — `desktop/` cannot add engine-side recording wiring, and `engine/`
is owned by other concurrently-running agents this pass. Whoever picks it up
needs one of: a `PageStore` subscriber on `StreamId::kLidarMid360` that also
writes raw chunks (loses "unmodified datagram" fidelity, since PageStore
holds decoded `PointVertex`, not raw packets), or a driver-level raw-packet
sink parallel to the existing `imu_sink` (matches `mid360_driver.h`'s own
`kInject`-backend framing exactly, and is the natural fit for "unmodified
datagrams").

`evidence/mid360-selftest.log`, `evidence/mid360-sim-stdout.log`.

**Known nondeterminism hazard, not a code bug:** `mid360_sim` and the SDK2
backend bind fixed loopback ports (56100–56501). With five agents working
this repo concurrently, another agent's own `ctest -L sim` /
`lvx2_replay` run competing for the same ports produced transient `bind
failed` on two attempts during this task; `scripts/verify_c2c3.sh` waits (up
to ~7 minutes, polling `lsof`) rather than killing another agent's process.
Real hardware would not have this problem — it is purely a same-machine,
concurrent-simulator artefact.

#### 8.7.4 Display-parameter profile-default-on-open / auto-persist-on-close

`--new-project floorplan --project DIR` (a small CLI hook added for exactly
this test) then opening `DIR` fresh (no saved `display_params.json` yet) and
quitting:

```json
"pointSize": {"mode": "fixedPixels", "fixedPx": 1.5, ...},
"lodPointBudget": 8000000,
"colorMode": "height",
"height": {..., "manualMin": 1, "manualMax": 1.5, "colormap": "thermal", ...},
"edl": {"enabled": true, "strength": 0.7},
"clipHeight": {"enabled": true, "min": 1, "max": 1.5},
"overlays": {"trajectory": false, "poseGraph": false}
```

Matches `docs/A14-display.md` §5's Floor-plan row exactly (fixed 1.5 px,
8 M, height/thermal, EDL on/0.7, height-clipped 1.0–1.5 m, no overlays) —
this is `applyDisplayProfile()`'s fallback firing correctly on first open,
and the file exists at all only because `closeEvent()` auto-saved it with no
explicit user action. Hand-editing `lodPointBudget` to `12345678` and
reopening + quitting again left it at `12345678` — proof
`loadFromProject()` wins over the profile fallback once a file exists, and
that auto-persist-on-close does not stomp a loaded (as opposed to
freshly-defaulted) document back to profile defaults.

### 8.8 Not verified / known gaps from this task

* **D6 self-test/health panel against real hardware.** No COIN-D6 is present
  (same as §6). The state machine, port auto-refresh, CH340 hint and the
  self-test's D6-specific pts/s branch are real code exercised by the
  self-test's *shared* lifecycle (`startPreviewSession`/
  `startRecordingSession`/pause/resume/stop are identical code for both
  tabs, and that lifecycle **was** proven end to end against the Mid-360
  simulator, §8.7.3) — but "a real D6 self-test would pass at ~4k pts/s" is
  still an expectation, not a measurement. No serial-loopback tool (`socat`)
  was available on this machine to fake one.
* **§8.3's `DeviceHealth` ceiling** — stall/restart counters are not on the
  path the task asked for them to come from; see §8.3 for exactly what is
  missing and where it would need to land.
* **§8.7.3's Mid-360 recording gap** — live view works, `.lscan` persistence
  does not yet, for this sensor only.
* **CH340 hinting is untested against a real CH340 adapter** — the
  vendor-ID match (`0x1A86`) is correct per WCH's own datasheet, but no
  adapter was plugged in to confirm `QSerialPortInfo` actually reports that
  VID on every OS this app targets.
* **No 3D line rendering for measure segments** — a point-sampled line (32
  points) reads clearly in a screenshot at this cloud's scale but is not a
  continuous line primitive; a real `LINES`-topology renderable would need
  its own tiny unlit `.mat` (deliberately not built — see §8.4, reuse over
  new-material-surface-area).
* **Export dialog's own UI thread path is not screenshotted** — `evidence/`
  has the CLI (`--export`) path's output because that is what proves
  `export_points()` end to end without a GPU-dependent interactive session;
  the dialog's progress bar / cancel button were exercised by inspection and
  by this task's author driving the real GUI, not captured as evidence
  (`ViewportWindow::captureScreenshot()` is the viewport's readPixels path
  and does not apply to a `QDialog`).

---

## 9. C4 (processing queue)

New files: `src/app/ProcessingDock.{h,cpp}` (the dock), `src/app/QtHttpTransport.{h,cpp}`
(a real `HttpTransport`), `src/app/SyntheticMid360.{h,cpp}` (the evidence fixture builder).
`src/main.cpp` gained `--build-synth-mid360`, `--post-e2e`, `--cloud-submit-selftest`.
`MainWindow` gained a `processingDock()` accessor and wires `ProcessingDock::loadResultRequested`
into the viewport + Plan dock. `CMakeLists.txt` gained the `Qt6::Network` component (§9.2).

### 9.1 `ProcessingDock` does not own a `JobQueue` — it uses `Engine::jobs()`

This is the one place this task's "code against what exists when you start" instruction
actually bit, in the good direction. At task start `engine/include/scanengine/jobs/job_queue.h`
had no accessor anywhere on `Engine`; `jobs/job_queue.h`'s own doc calls `JobQueue` "a standalone
concrete class". So `ProcessingDock` was built to construct and own its own
`scanengine::jobs::JobQueue`, seeded with `&host_->engine()->events()` so `EventType::kJobProgress`
still republished onto the real event bus.

**Mid-task, `Engine::jobs()` landed** (INT-34, `core/engine.h`: "lazily owned... the first call to
`jobs()` creates it and every later call returns the same one; an Engine that is never asked owns
no thread... destroyed (and its worker joined) at the very start of `~Engine`"). This is exactly
the shape `docs/A15-jobs.md` §7 item 3 asked for, and it removes a real problem the
dock-owns-its-own-queue design had: a second, independent `JobQueue` means a second worker thread
and, if anything else in the app (a future capture-window "process on stop" button, say) ever
wanted to submit a job, it would have needed its own third queue or a reference threaded through
from `ProcessingDock`. **Adopted.** `ProcessingDock::queue()` is now one line,
`return host_->engine()->jobs();` — no member, no constructor wiring, no destructor `stop()` call
(the engine's own shutdown order — jobs before recorder before page store before bus — already
handles that). Re-verified end to end after the switch (§9.4); no behavioural difference, less
code.

**Also landed mid-task and *not* adopted:** `jobs/job_runner_adapter.h` (INT-34's other half,
closing §7 item 3's `JobRunner` translation) and `jobs/colorize_wiring.h` (a `ColorizeWiring`
struct + `wire_colorizer()` that would wire a live `TimeSync`/`ImuIngest`/`PoseInterpolator` into
a `PointColorizer`). Neither fits this dock: the adapter's own header explains why the *richer*
`JobQueue`/`JobSpec` API — chaining, priority, per-kind options — is what an app-facing UI should
code against directly rather than through the lossy `JobRequest{mode, lscan_dir, output_dir,
pipeline}` seam, which is exactly what `ProcessingDock` already does. `colorize_wiring.h` needs a
live `TimeSync`/`ImuIngest` — real objects that exist only inside a *live capture* session — but
Colorize here runs against an already-recorded (and usually already-imported-from-a-phone)
project with no capture session open, so there is nothing of that shape to wire. Flagged for
whoever adds capture-time colorization or an in-session "process what I just captured" flow: that
caller *does* have a live `TimeSync` and should use `colorize_wiring.h` instead of the manual
sync-quality dropdown described in §9.3.

### 9.2 `QtHttpTransport` — a real socket attempt, per the task's own instruction

`docs/A15-jobs.md` §5 and `jobs/cloud_submit.h` are explicit that `CloudSubmitClient` is written
and tested **only** against a scripted fake `HttpTransport`; the real one is "D3's". This task's
brief is equally explicit the other way: *"the fake-transport path is not for UI"*. So
`QtHttpTransport` is a real, if small, adapter: one `QNetworkAccessManager` (`thread_local`, since
`JobQueue`'s worker thread — not the GUI thread — is what calls `request()`), a `QEventLoop` spun
per call to turn `QNetworkReply`'s async callback into `HttpTransport::request()`'s synchronous
contract, and a timeout timer that aborts the reply so an unreachable/black-holed host cannot wedge
a job forever. `HttpResponse::transport_ok` is `false` exactly when no real HTTP status line ever
came back (`status_code == 0`) — connection refused, DNS failure, or this transport's own timeout
— which is precisely the flag `CloudSubmitClient`'s retry/resume logic keys off. `Qt6::Network`
was added to `CMakeLists.txt`'s `find_package`/`target_link_libraries` for this.

### 9.3 The dock: one queue, five actions, a polling table

`ProcessingDock` is a `QDockWidget` holding a `QTableWidget` (ID / Kind / State / Progress bar /
Stage / Message / Cancel+Load-result buttons) refreshed every 200 ms from `queue().list()` — the
same "poll an atomic/a snapshot from a `QTimer`" shape `ExportDialog`/`ReplayController` already
established for their own worker threads, chosen again here because `list()` already reflects
every update the instant it lands and a second `EventBus` subscription would just be a second,
redundant path to the same information.

* **Post-process…** — a small dialog: pick (or browse to) a `.lscan` directory, or click "Build a
  synthetic Mid-360 test project…" (§9.4) to get one with no hardware. Submits `kPostProcess` with
  a "fast" `PostConfig` preset (`desktopPostConfig()`, ported from `test_post.cpp`'s own
  `fast_synth_config()` — smaller keyframes/voxels, tuned for an interactive UI run rather than a
  30-minute capture) rather than A7's full-density production default, so the dock stays
  responsive against the desktop's own synthetic fixture. `store = null`: the job gets its own
  fresh `PageStore`, not the engine's live one — see §9.4 for why "Load result" is the mechanism
  that gets it into the viewport rather than the pipeline publishing straight into what is already
  on screen.
* **Colorize…** — needs a project with `streams/frames/frames.idx` (Android-only capture, §3.5;
  desktop has no camera). Source is either the currently-loaded viewport/engine cloud (wrapped as
  a non-owning `shared_ptr<PageStore>` — same "wrap, do not copy" pattern `docs/A11-color.md` §8.1
  names for the store's own mutable-access seam) or `chain_from` a finished
  Post-process/Colorize job. A `color::PointColorizer` is constructed per submission and kept
  alive in `colorizers_` (keyed by job id, since `ColorizeParams::colorizer` is a non-owning raw
  pointer that must outlive the queued run — job_types.h says so explicitly) until `refresh()`
  observes the job settled. Sync quality is a dropdown (Good/Gated) rather than measured, because
  there is no live `TimeSync` for an already-recorded project to measure from — see §9.1's note on
  `colorize_wiring.h` for where a real answer would come from.
* **Export…** — chains from a finished Post-process/Colorize job id (`kExportPoints`,
  PLY/LAS/PCD). This is deliberately a *second* export path alongside C3's `ExportDialog`: that
  one exports whatever is live in the viewport right now, synchronously-ish on its own
  `std::thread`; this one exports a specific finished job's own `PageStore` through the queue, so
  exporting job #3's result does not depend on job #3 having been "Load result"-ed into the
  viewport first.
* **Transfer bundle…** — `kTransferExport`: project dir + destination `.zip` +
  `include_results` checkbox, straight onto `TransferExportParams`.
* **Submit to cloud…** — the config dialog the task calls for: server URL + token, persisted to
  `QSettings` (`cloud/baseUrl`, `cloud/token`) so a re-open remembers them. Bundle source is either
  a browsed `.lscan.zip` path or `chain_from` a finished Transfer job. Every field maps directly
  onto `CloudSubmitParams`; `spec.cloud.transport` is `cloud_transport_.get()`, one
  `QtHttpTransport` owned by the dock for its whole lifetime. The dialog states plainly that no
  server exists in this environment and the attempt will fail — see §9.5 for the measured
  failure.

**"Load result" and why it is not "a fresh engine/replay of processed/".** The task phrases C4's
completion flow as loading the final cloud "via a fresh engine/replay of the processed output —
use whatever A7's pipeline writes to processed/". `docs/A7-post.md` §7 says, in its own words, the
opposite is true today: *"The final cloud is **not** written back into the `.lscan` as a
`kPointsXyzRgba` chunk in `processed/`"* — `stream_of(ChunkType::kPointsXyzRgba)` maps to
`kUnknown`, so there is nothing under `processed/` to replay. What A7's pipeline *does* produce is
a live, in-memory `PageStore` on `StreamId::kSlamMap` (`docs/A7-post.md` §7,
`PostSlamPipeline::out_store()`), which `JobQueue::produced_store(job_id)` hands back as a
`shared_ptr<PageStore>` once the job reaches `kDone`. So "Load result" is
`ProcessingDock::loadResultRequested(shared_ptr<PageStore>, jobId)` →
`viewport_->setPointStore(store.get())` + `viewport_->fitView()` + `plan_dock_->setPointStore(...)`
— the *same* kSlamMap pages a replay would eventually publish, reached directly rather than through
a round trip to disk and back that the engine does not currently support. `MainWindow` keeps the
`shared_ptr` alive (`loaded_result_store_`) for as long as it might be on screen. If A7/A5 close
the §7 gap and start writing a processed cloud back into the `.lscan`, this is the one place that
would change — an actual replay/reader call in place of `produced_store()`.

### 9.4 A real post-process run from the UI, with no hardware

`app/SyntheticMid360.{h,cpp}` is a line-for-line port of `engine/tests/test_post.cpp`'s own
`write_synthetic_lscan()` and its helpers (`Room`, `LoopTraj`, `imu_truth`, `mid360_ray`,
`put_header`, the small quaternion routines) — a ray-cast loop through a 24×18×3.5 m synthetic
hall, written through `lscan::FileRecordWriter` as **real** `kMid360Points`/`kMid360Imu` chunks in
the Mid-360 wire format (`mid360::DataHeader`/`CartesianHigh`/`ImuRaw`), exactly the shape A7's own
pipeline reads. Ported rather than shared because `engine/` is read-only for this task and that
helper is file-local to a test binary; `SyntheticMid360.h` says so and points at the source. This
is *not* the S2 sim-inject path (that needs `mid360_sim` running and a live capture session) and
*not* A7's `transcode_livoxdump()` path (that needs a fixture file this task cannot rely on being
present) — it needs nothing but the engine's own public headers, so `--build-synth-mid360 DIR`
works standalone, headless, on any machine.

`--build-synth-mid360 evidence/c4-synth-mid360.lscan` produced **1,667 point packets (127,425
points), 1,601 IMU packets, 8.0 s** — matching `docs/A7-post.md` §6.2's own numbers for the
identical recipe almost exactly (that run reports 127,425 points decoded from the same generator).

`--post-e2e DIR` submits a real `kPostProcess` job through `ProcessingDock`'s queue (i.e.
`Engine::jobs()`, §9.1), polls `queue().status(jobId)` on a `QTimer` until terminal, and on success
calls the exact same `setPointStore`/`fitView`/`setPointStore`-on-the-plan-dock sequence "Load
result" does:

```
[lidarscan] post-e2e: job #1 submitted for evidence/c4-synth-mid360.lscan
[lidarscan] post-e2e: job #1 running — 0% (opening recording)
[lidarscan] post-e2e: job #1 running — 35% (full-density odometry)
[lidarscan] post-e2e: job #1 running — 75% (pose-graph optimization)
[lidarscan] post-e2e: job #1 done — 100% (done)
[lidarscan] post-e2e: DONE — 83228 points in the result cloud
[lidarscan] post-e2e: result loaded into the viewport
```

`evidence/09-post-process*.png` (`--shot`, 6 s after `--post-e2e`) shows the reconstructed loop
hall in the viewport — 83,228 points, the whole run (submit → odometry → loop detection →
optimization → re-integration → filtering → publish) finished well inside the screenshot delay.
`evidence/verify_c4c5.log` has the full run.

### 9.5 Submit-to-cloud fails gracefully, measured

`--cloud-submit-selftest URL` submits a real `kCloudSubmit` job against a URL nothing listens on
(`https://127.0.0.1:1/v1` — a real socket connect attempt, refused), through the same
`QtHttpTransport` the dialog uses, and polls to a terminal state:

```
[lidarscan] cloud-submit-selftest: job #1 submitted against https://127.0.0.1:1/v1
[lidarscan] cloud-submit-selftest: job #1 settled as failed (error=network error, message=network error) — the app is still running, which is the point
```

**Measured, not assumed: this takes ~28–35 s**, not an instant failure. `POST /jobs`'s own retry
policy (`docs/A15-jobs.md` §5: up to `max_retries` = 5 attempts with exponential backoff 500 ms ×
2, capped at 30 s, each attempt itself subject to `QtHttpTransport`'s own connect timeout) runs to
exhaustion before the client gives up and the job settles `kFailed` / a network `ScanError`. The
app stays fully responsive throughout (verified: the GUI event loop keeps running, `--quit-after`
still fires on schedule) — nothing blocks the GUI thread, because this all happens on
`Engine::jobs()`'s own worker thread. `evidence/cloud-submit-selftest.log` has the full run. This
is the graded, non-crashing failure the task asked for; a UI operator would see the row sit in
`running`/`cancelling` for that same half-minute before turning red, which is a real UX cost of
A15's own retry policy against total silence, not a bug introduced here — `Cancel` on that row
works throughout (it polls the same cooperative flag `CloudSubmitClient::send_with_retry` checks
between attempts).

### 9.6 A real, pre-existing crash-on-exit, found and fixed while verifying this task

Running `scripts/verify_c2c3.sh` as a regression check (§9.1's engine-side rewiring touched
`MainWindow`, so its own §8 evidence was re-run to make sure nothing broke) hit a **SIGSEGV on
exit** after the Mid-360 self-test step, every time. Symbolicating the crash report
(`~/Library/Logs/DiagnosticReports/lidarscan-*.ips` via `atos`) gave the stack:

```
main -> CaptureWindow::~CaptureWindow() -> CaptureWindow::onStop() ->
EngineHost::stopSession(QString*) -> EngineHost::logLine(QString const&) ->
MainWindow's `connect(host_, &EngineHost::logLine, ...)` lambda  [SIGSEGV]
```

**This predates C4/C5 entirely** — a second crash report from earlier the same day (05:17,
before this task's own binary existed) symbolicates to the identical stack shape, confirming it
is a real bug in C1/C2's own code (`CaptureWindow.cpp`, `EngineHost.cpp`, `MainWindow.cpp`), not
something introduced here. Root cause: `CaptureWindow` is created **lazily**
(`MainWindow::captureWindow()`, first touched from the Capture menu or `--mid360-selftest`), so
it always becomes a *later* `QObject` child of `MainWindow` than the docks `buildUi()` creates up
front — including the "Projects" dock that owns `log_`. Qt destroys a `QObject`'s children
front-to-back (construction order, not reverse), so on exit the Projects dock (and `log_` with
it) was torn down **before** `capture_`. `CaptureWindow`'s own destructor calls `onStop()` (to
cleanly stop any in-flight capture), which calls `EngineHost::stopSession()`, which emits
`logLine()`, which invokes `MainWindow`'s own lambda — dereferencing the by-then-dangling `log_`.

Since this is squarely inside `desktop/` (this task's ownership, unlike the `engine/` cross-
boundary breakages this doc reports rather than fixes — §"OWNERSHIP" at the top of this file),
and a crash-on-exit undermines the "app launches" verification bar for every task that touches
`MainWindow`, it was fixed here rather than only reported: `MainWindow::~MainWindow()` now
explicitly `delete capture_;` before the implicit base-class/child destruction runs, sidestepping
the ordering question entirely instead of relying on child-list order. Re-verified:
`scripts/verify_c2c3.sh`'s Mid-360 self-test step (§8.7.3) now exits clean, no crash report
produced, same PASSED-at-2.17s self-test result as before.

### 9.7 Not verified / known gaps

* **Colorize was not run against a real `frames.idx`.** No Android capture with camera frames
  exists in this environment (§3.5: capture is Android-only) and no `.lscan` with
  `streams/frames/frames.idx` was available to import. The wiring (`ColorizeParams`,
  `PointColorizer` construction, `chain_from`/`store` source selection, the `colorizers_` lifetime
  map) is real code exercised by inspection and by the "no camera" refusal path
  (`load_keyframes()` returning `kNotFound`, which `run_colorize()` treats as a non-failure per
  `docs/A15-jobs.md` §4) against the synthetic Mid-360 project, which has no `frames/` directory —
  but "colorizing a real phone capture would work" is an expectation, not a measurement here.
* **Transfer bundle was exercised by code review and the dialog's own field wiring, not captured
  as CLI evidence** — `jobs::run_transfer_export()` is the same `zip_export()` A5 already ships and
  C7 (transfer import) will need to read the far end of; no new risk here beyond A15's own
  documented cancellation gap (`docs/A15-jobs.md` §3: `zip_export()` has no cancel hook, so a
  Transfer job can only be cancelled either side of the blocking zip call).
* **The processing table's Cancel button was exercised interactively, not scripted** — cancelling
  a `kPostProcess` job mid-run is a real, tested path in `jobs::JobQueue` itself
  (`docs/A15-jobs.md` §6's `queue/cancel_running`), and `ProcessingDock::onCancelJob` is a
  three-line call to `queue().cancel(jobId)`; no new engine-facing risk, just not screenshotted.

---

## 10. C5 (floor plan workspace)

New files: `src/app/PlanDock.{h,cpp}` (the dock + `PlanGraphicsView`),
`src/app/SyntheticBuilding.{h,cpp}` (the evidence fixture builder). `src/main.cpp` gained
`--plan-fixture`, `--plan-extract`, `--plan-export-dxf`, `--plan-export-pdf`, `--plan-shot`,
`--plan-delay`. `MainWindow` gained `planDock()` and `loadSyntheticBuildingFixture()`.

### 10.1 The recompute split, exactly as `plan_editor.h` specifies it

`PlanDock` holds one `plan::PlanEditState state_` plus the two cached `OccupancyGrid`s
`recompute_grids()` last produced. Every control maps to exactly one of two paths:

| Control | Mutator | Path |
| --- | --- | --- |
| Slice slider (release) / band-width spinbox | `with_slice_band` | slow: `recompute_grids()` + `recompute_walls()` |
| Window sill check toggle | `with_sill_check` | slow (a second grid on the same lattice has to be built or dropped) |
| Include/exclude region drawn or deleted | `with_include_region`/`with_exclude_region`/`without_region`/`with_regions_cleared` | slow (changes which points land in the grid) |
| Orthogonality snap toggle | `with_orthogonality` | **fast**: `recompute_walls()` only, against the cached grids |

This is `docs/A12-plan.md` §5's own cost model ("moving the slice band or a rectangle changes
which points land in the grid... changing snapping... does NOT touch the cloud") wired up exactly
as specified, not approximated: the slider's live drag only repaints a label
(`onSliderMoved`→`updateSliceLabel()`); the actual re-slice happens once, on `sliderReleased`, so
dragging across the full 6 m range does not fire dozens of full grid rebuilds.

`state_`/`main_grid_`/`sill_grid_`/`model_` are plain members, not a `std::vector<PlanEditState>`
undo stack — the task asked for the recompute split and the editor controls, not undo/redo, and
`plan_editor.h`'s own doc frames the `vector<PlanEditState>` history as something "the Qt desktop
and the Android viewer" *can* keep for free because every mutator returns a new value; nothing
here stops a future pass from adding a history list on top of the same state shape.

### 10.2 Rendering: metres as scene units, and the bug that came from not doing that consistently

`PlanGraphicsView`'s `QGraphicsScene` is built directly in **metres** (1 scene unit = 1 m, y
negated so north renders up), and `QGraphicsView::fitInView()` supplies the pixel scale — no
hand-rolled meters-to-pixels transform. Walls are `QGraphicsLineItem`s with `pen.setWidthF(wall
thickness)` (a scene-unit width, so it scales with zoom like a real wall would); openings are a
second, coloured line per gap (door = brown, window = cyan) laid across the gap; rooms are
`QGraphicsPolygonItem`s filled translucent blue with a `QGraphicsTextItem` label (name + area) at
the centroid, flagged `ItemIgnoresTransformations` so the label text stays a constant *pixel* size
regardless of zoom.

**That flag is exactly what broke the first version of `fitInView()`.** `ItemIgnoresTransformations`
means the label's `boundingRect()` — sized in local (pixel-ish) coordinates by the font metrics —
gets folded straight into `QGraphicsScene::itemsBoundingRect()` as if it were scene-unit (metre)
geometry, which silently inflated the "whole plan" bounding box to tens of "metres" and zoomed the
real ~8 m building down to a speck in one corner of the view (caught by inspecting the first
`--plan-shot` screenshot, which showed exactly that). The fix — `PlanDock::renderModel()` computes
the fit rectangle from **`model_.bounds`** (`PlanBounds`, real metres, no items in it) rather than
from `itemsBoundingRect()`, and `PlanGraphicsView::fitToRect()`/`resizeEvent()` re-fit to that
stored rectangle on every resize (needed because this dock is constructed, and does its first
`fitInView()`, before `MainWindow` has ever been shown or laid out to its final size — a `QTimer`
delay or a dock tab that starts hidden both hit this). `evidence/10-plan.png` is the corrected
render; the underlying bug and fix are recorded here because it is exactly the kind of thing a
"screenshot ⇒ inspect it ⇒ notice the plan is 2 px in a corner" step catches that a compile-only
pass never would.

### 10.3 Include/exclude region drawing

`PlanGraphicsView` in "draw region" mode (a checkable toolbutton) drags out a dashed yellow
rubber-band `QGraphicsRectItem` and emits its rectangle **in scene coordinates**; `PlanDock`
converts that back to world coordinates (`worldY = -sceneY`, the same negation §10.2 render uses)
and calls `with_include_region`/`with_exclude_region` depending on an Include/Exclude combo box.
The region list (`QListWidget`) shows every region as `include/exclude (minx,miny)-(maxx,maxy)`;
"Delete selected"/"Clear all" map straight onto `without_region`/`with_regions_cleared`. Existing
regions are also drawn as dashed overlays on the plan itself (green = include, red = exclude) so
the effect of a region is visible without cross-referencing the list.

### 10.4 DXF / PDF export, with open-in-Finder

`onExportDxf`/`onExportPdf` prompt for a save path (defaulting into the project's `exports/`
directory) and call `plan::write_dxf`/`plan::write_pdf` with default `DxfOptions`/`PdfOptions`,
then reveal the file the same per-OS way `ExportDialog::onOpenContaining()` already does
(`open -R` / `explorer.exe /select,` / `QDesktopServices::openUrl`) — reimplemented locally rather
than shared, since `ExportDialog`'s version is private to that class. Both are also reachable
headlessly (`exportDxfForCli()`/`exportPdfForCli()`, no `QFileDialog`) for `main.cpp`'s
`--plan-export-dxf`/`--plan-export-pdf` evidence hooks, which the interactive buttons now delegate
to after the file dialog returns a path — one code path, not two.

### 10.5 Verified against A12's own synthetic building fixture

`app/SyntheticBuilding.{h,cpp}` ports `engine/tests/test_plan.cpp`'s `make_building()` /
`emit_face()` / `emit_box_sides()` (default `BuildOpts`: clutter on, no shelf-against-wall, no
diagonal wall, no rotation) — the same two-room-plus-corridor building `docs/A12-plan.md` §6
documents, chosen for the same reason `SyntheticMid360` was ported rather than shared (§9.4):
`engine/` is read-only and the generator is file-local to a test binary.

`--plan-fixture` appends the fixture directly into the engine's own `PageStore`
(`MainWindow::loadSyntheticBuildingFixture()`, `StreamId::kSlamMap`) and points the
viewport/Plan dock at it. `--plan-extract` then drives `PlanDock::runExtractionForCli()` — the
exact same `recompute_grids()`+`recompute_walls()` the "Extract floor plan from current cloud"
button calls:

```
[lidarscan] plan-fixture: loaded
[lidarscan] plan-extract: 6 walls, 3 openings, 3 rooms, 38.28 m2 total area, slice 1.00..1.50 m
[lidarscan] plan-extract:   room R1: 11.1963 m2
[lidarscan] plan-extract:   room R2: 13.2843 m2
[lidarscan] plan-extract:   room R3: 13.7979 m2
```

**These numbers match `docs/A12-plan.md` §6's own reported figures for the identical fixture
exactly** (11.1963 / 13.2843 / 13.7979 m², against true areas 11.2000 / 13.2825 / 13.8000 m² —
A12's own doc's own table, to four decimal places), which is as strong a confirmation as this task
can get that the desktop's editor/recompute path reaches the same extractor with the same default
`PlanOptions` A12 itself verified against, not a divergent copy.

`evidence/10-plan.png` (`--plan-shot`, the Plan dock raised so its `QGraphicsView` has a real
layout to render into) shows the three labelled rooms, walls and the door/window openings.
`evidence/floorplan.dxf` passes `scripts/check_dxf.py` — a from-scratch DXF R12 reader independent
of A12's own writer/tests (the same "two independent implementations must agree" posture
`scripts/check_ply.py` already established for A9's PLY writer in §8.5.2), asserting `$ACADVER =
AC1009`, `HEADER`/`ENTITIES` sections present, every `POLYLINE` closed by a `SEQEND`, and all four
expected layers (`WALLS`/`OPENINGS`/`ROOMS`/`DIMENSIONS`) present:

```
OK: evidence/floorplan.dxf: AC1009, 4 sections, 15 POLYLINE/SEQEND pairs, 28 entities, layers=['DIMENSIONS', 'OPENINGS', 'ROOMS', 'WALLS']
```

`evidence/floorplan.pdf` was opened through **macOS Quick Look**
(`qlmanage -t -s 1000 -o evidence evidence/floorplan.pdf`, the same verification style
`docs/A12-plan.md` §9 itself uses — "No CAD package is installed on this machine, so the DXF's
only verification is the from-scratch reader" applies equally here), producing
`evidence/floorplan.pdf.png`: poché-filled wall footprints, dashed threshold lines across both
doors and the window, three labelled rooms with areas, a scale bar at 1:50, a north arrow marked
`N (ASSUMED)` (§7's documented placeholder — this session has no georeferencing), and a title
block with scale/sheet/slice-band/grid-resolution/wall-opening-room counts — rendering correctly
end to end with no viewer-side errors.

### 10.6 Not verified / known gaps

* **Non-rectangular / rotated regions.** The include/exclude tool only draws axis-aligned
  rectangles (`PlanRegion`'s own shape, `plan_model.h`) — this matches the engine API exactly, not
  a desktop limitation.
* **No undo/redo stack.** `plan_editor.h`'s pure-function `PlanEditState` makes one trivial to add
  (`std::vector<PlanEditState>`, §10.1), but the task asked for the recompute split and the editor
  controls, not undo — not implemented.
* **The slice slider's live-drag preview is the label text only, not a live-updating plan.** A
  full live re-slice on every `valueChanged` tick (rather than once on `sliderReleased`) was
  measured as unnecessary cost for this task's fixture (§10.1) and not attempted at a larger scale;
  A12's own doc (§8 item 10) already flags "live slider on Android" as marginal even for the
  *fast* (walls-only) path on a 40×40 m building, so a full re-grid on every tick would be worse.
* **A real Mid-360/post-process cloud was not run through the Plan dock** — §10.5's fixture is
  A12's own synthetic building, not the C4 §9.4 post-process result. Nothing in `PlanInput`/
  `PlanDock` distinguishes the two (`store_` is `const PageStore*` either way — the same pointer
  `ProcessingDock::loadResultRequested` feeds it), but "extracting a plan from a real post-process
  run's cloud would work" is inference from the shared code path, not a separate measurement.
* **Non-`kZ` up-axis was not exercised.** `UpAxis::kZ` is the only value used anywhere in this
  task; `plan::UpAxis::kY`/`kX` are real engine options `PlanDock` never sets.

---

## 11. C6 (merge workbench)

New files: `src/app/MergeDock.{h,cpp}` (the dock + `MergeIcpChart`, a raw-`QPainter`
residual chart — no charting library), `src/app/MergeFixture.{h,cpp}` (the evidence
fixture), `src/app/MergeSessionLoader.{h,cpp}` (real `.lscan` → merge session). `MainWindow`
gained `mergeDock()`, `setOpenProjectDir()` wiring, and an `exportMergedRequested()` → a fresh
`ExportDialog` over the merged store (the same "always construct a fresh dialog" rule C3's
`onExport()` established). `src/main.cpp` gained `--merge-fixture-evidence`, `--merge-dock-shot`
(+ `-delay`). `src/render/ViewportWindow.{h,cpp}` gained `debugPickWorld()`, a read-only wrapper
around the existing (private) `pickPoint()` for evidence tooling — see §11.4.

### 11.1 Three session sources, one code path

`MergeDock::addSession()` is the only place that calls `MergeProject::add_session()`; every
UI action and every CLI hook funnels through it:

* **"Build synthetic fixture (evidence)"** — `MergeFixture` (§11.2) hands back owning
  `std::vector<PointVertex>` per session; `addSession()` appends each into a freshly
  allocated `scanengine::PageStore` it now owns (`SessionRow::ownStore`).
* **"Add from open project"** (prefilled from `MainWindow::setOpenProjectDir()`, the same
  posture `ProcessingDock`/`PlanDock` already use) and **"Import .lscan project…"** (a
  `QFileDialog::getExistingDirectory`) are the *same* function, `addFromProject()` — they
  differ only in which directory arrives already filled in.

`addFromProject()` cannot use `SessionMerger::add_session(lscan_dir)` — engine/docs/A13-merge.md
§2 is explicit that seam is declared, not implemented, because nothing writes a processed cloud
into a `.lscan` yet. What *is* real and already proven end to end (C1 §"replay == capture") is
replaying a project's raw D6 bytes through a live `Engine`. `MergeSessionLoader` is exactly
`ReplayController`'s shape — a live-preview session (`lscan_dir` empty, so nothing re-records),
one D6 device, `lscan::ReplaySource` — with two deliberate differences: **its own private
`Engine`** rather than sharing `EngineHost`'s (a merge project holds several sessions' clouds
alive *simultaneously*, and `SessionCloud` is a list of non-owning spans into whatever produced
them — the `Engine`, and the `PageStore` inside it, must outlive the `MergeProject`, so each
loaded session gets its own), and **`speed = 0` (unpaced) run synchronously to completion**
rather than a polled worker thread, because alignment cannot start on a half-loaded cloud. A
project with no D6 raw chunks (Mid-360-only) is refused with a message naming exactly why
(`record/replay.h` only forwards `ChunkType::kD6Raw` today) — the same gate
`MainWindow::startReplay()` already enforces, reused via `ProjectInfo::has_d6_raw`.

Whichever source, `addSession()` then does exactly one thing every time:
`scanengine::merge::collect_pages(*row.store(), stream, &in.cloud)` — `kLidarD6` for a
replay-loaded session (where the D6 driver actually publishes: `src/drivers/d6/d6_driver.cpp`
appends at `StreamId::kLidarD6`, confirmed by reading the engine source since nothing in its
own docs states it), `kSlamMap` for a synthetic-fixture session — followed by
`project_->add_session(in)`. One function, three doorways.

### 11.2 The evidence fixture, ported the way SyntheticMid360/SyntheticBuilding were

`MergeFixture` is a line-for-line port of `engine/tests/test_merge.cpp`'s own `Rng`
(xorshift64 + Box-Muller, deliberately not `<random>` — the standard does not fix its
distributions' output, so this build would disagree with the test binary's own numbers
otherwise), `sample_plane`/`building_surfaces`/`session_cloud`/`Fixture`, and the `kPickWorld`
manual-pick fixture. Ported rather than shared for the same reason SyntheticMid360.h/
SyntheticBuilding.h were (C4 §9.4, C5 §10.5): the generator is file-local to a test binary and
`engine/` is read-only for this task.

The building: 30×12×3 m, two interior partitions. Three sessions cover x ∈ [0,13], [9,22],
[18,30] — sessions 0-1 and 1-2 overlap 4 m each, 0-2 share nothing (the workbench's
"these two do not see the same place" case comes free from the same fixture, exactly as
`docs/A13-merge.md` describes it). Session 0 is the anchor. Each session is independently
re-sampled (its own RNG stream, 5 mm range noise) and additionally carries a `SessionGeoref`
anchored at its own ENU origin — three different days' first-fix locations, 200-400 m apart, one
shared CRS — reproducing `docs/A13-merge.md` §3's "shared CRS is not shared frame" trap on
purpose.

### 11.3 3-point manual picking reuses the C3 measure tool, exactly as asked

The task's own hint — "reuse the C3 measure picking machinery" — is implemented literally
rather than approximated. `MeasureSegment`'s two-click shape (`ViewportWindow::
setMeasureMode`/`measurementsChanged`/`measurements()`) already IS "click A, click B, get a
pair"; a `PointCorrespondence` needs nothing more than that pair labelled *which* cloud each
click landed in. So:

1. **"3-point manual…"** picks the session to align from `align_session_combo_`, calls
   `beginPickPhase()`: swaps the viewport's point store to the SOURCE session's own store
   (`viewport_->setPointStore(...)`), turns on measure mode, and labels the status line "click a
   feature in SOURCE session '…'".
2. `onViewportMeasurementsChanged()` — connected once in the constructor, a no-op whenever
   `picking_` is false — watches for the FIRST click (`hasPendingMeasurePoint()` flips true).
   Crucially, `ViewportWindow`'s pending point is **pure geometry**, no store reference, so
   swapping the displayed store to the TARGET (anchor) session mid-pair does not disturb it —
   the pending yellow marker from the source cloud simply hangs there while the anchor's cloud
   is what's now on screen for the second click.
3. The second click completes a `MeasureSegment` the normal way; `MergeDock` reads it back
   (`seg.a` = source-local point, `seg.b` = target-local point) as one `PointCorrespondence`,
   clears the viewport's measurements, swaps the store back to SOURCE, and repeats — three
   times, each pair labelled "pick N of 3".
4. On the third pair, `finalizePicks()` calls `align_from_correspondences()` for real and shows
   the `CorrespondenceSolution` (rms, max residual, spread — including the collinearity
   diagnostic) or the refusal (`sol.blocker`) verbatim.

No new picking code exists anywhere in this dock or the viewport; `ViewportWindow` gained
exactly one thing for C6, and it is a read-only wrapper, not a new interaction (§11.4).

### 11.4 CLI evidence drives the SAME buffer, not a parallel path

`alignManualForCli()` (and `--merge-fixture-evidence`) does not synthesize screen clicks the
way `--measure-selftest` (C2/C3) grid-walks real `QMouseEvent`s. It calls `recordPick()` — the
exact private method `onViewportMeasurementsChanged()` calls on a real second click — with
`MergeFixture::pick()`'s known, exact 3D coordinates, then the identical
`align_from_correspondences()` finalize path. This mirrors what `engine/tests/test_merge.cpp`
itself does for its own "exact picks"/"noisy picks" cases: hand-specified 3D points, not
simulated mouse events, because the thing under test is the solver, not Qt's event queue. The
interactive path (§11.3) is real, separately implemented code, exercised by hand and by
`ViewportWindow::debugPickWorld()` being available for anyone who *does* want to script a real
click later (a thin `const`-safe query wrapper around the existing private `pickPoint()`, added
but not used by the evidence hook above — grid-searching screen space for the pixel whose pick
lands nearest a known world point is straightforward with it, just not needed once `recordPick()`
was available as a lower-friction, equally-real seam).

### 11.5 Colour-by-session needs no renderer redesign — confirmed, not just predicted

NOTES.md's own §4 table predicted this before C6 existed: `PagedCloudRenderer` is per-`PageId`
and uploads whatever `PointVertex.rgba` it finds, so "distinct colour per session" is a
**pre-publish edit of `MergeResult::cloud`**, not a shader or uniform change. `buildAndPublishForCli()`
walks `MergeResult::ranges` (the run table — one contiguous span per session, in priority
order, exactly what `docs/A13-merge.md` §6 describes) and overwrites r/g/b (keeping a=255) from
an 8-colour deterministic palette before calling `publish()`. §11.7's screenshot is the proof:
three sharply bounded colour regions with visibly interleaved dots exactly at the two 4 m
overlap seams, where dedup left survivors from both sessions' independent samplings side by
side.

### 11.6 The residual chart reproduces `refine()`'s own trace, not a re-guess

`MergeReport` deliberately does not carry `PairIcpResult::trace` (`docs/A13-merge.md` §7 says
so explicitly — a workbench that wants it calls `refine_pair()` itself). So
`MergeDock::refineForCli()` snapshots every pair's relative transform (`se3::mat4_inverse_rigid`
+ `mat4_mul` over `MergeSession::world_from_session`) **immediately before** calling
`project_->refine()`, keyed by `(session_a, session_b)`. Selecting a pair row afterward calls
`scanengine::merge::refine_pair(session_a.cloud, session_b.cloud, THAT snapshot, default
MergeIcpConfig)` — the exact same inputs `refine()` used internally for that pair — and plots
the returned `trace`. `MergeIcpChart` (raw `QPainter`, no library) draws one polyline of
`rms_m` per iteration in millimetres, with a dashed vertical marker at every point the
correspondence gate changes (`docs/A13-merge.md` §5: "the step down at the stage boundary is
visible in the residual and must not look like a bug" — the chart makes that visible on
purpose rather than smoothing over it).

### 11.7 Verification (2026-08-15, same host as §6: Apple M4, macOS 26.5.1)

`scripts/verify_c6c7.sh` reproduces this section and §12's; raw output is
`evidence/verify_c6c7.log`. Driven end to end via `--merge-fixture-evidence` (real
`MergeProject` calls, not a script that only prints numbers) plus `--shot`/`--merge-dock-shot`
for screenshots. `--display-profile research` is used for the viewport shot only because A14's
Quick-scan default renders intensity-grayscale, which would make three RGB-tinted sessions
indistinguishable in a screenshot — a real, if minor, finding: **the merge workbench's own
colour-by-session preview is invisible under the app's own default profile**, worth a
follow-up (either the dock forcing `ColorMode::kRgb` while its own build is on screen, or
just operator awareness) that this task did not implement, to keep the renderer/display-params
seam untouched as the task's ownership implies.

**Build:** clean configure + build from scratch (deleted `build/`), exit 0, **zero warnings
from any file under `desktop/src`** (`-Wall -Wextra`); the 239 warnings the build does emit are
the same vendored-Livox-SDK2 baseline every prior task in this file reports.

**Georeferenced auto-align** (3 sessions, 3 ENU origins 200-400 m apart, per §11.2):

```
merge-fixture-evidence: georeferenced auto-align OK — aligned=3 skipped=0 max ENU-origin separation=331.57 m
merge-fixture-evidence: georef session 0 vs ground truth: 0.0000 mm / 0.000000 deg (align=georeferenced)
merge-fixture-evidence: georef session 1 vs ground truth: 0.0000 mm / 0.000000 deg (align=georeferenced)
merge-fixture-evidence: georef session 2 vs ground truth: 0.0000 mm / 0.000000 deg (align=georeferenced)
```

Sub-micrometre against ground truth at 4 decimal places — matching `docs/A13-merge.md` §3's own
measured 6.2×10⁻¹⁰ mm to the precision this log prints at. This is A13's own claim ("the
composition is exact") reproduced from the desktop UI's own code path, not re-derived.

**Manual 3-point** (session 1 vs the anchor, exact picks — the "click 3 point pairs" path,
§11.3/§11.4, run through the identical `recordPick()`/`align_from_correspondences()` finalize
sequence the real 3-click UI flow uses):

```
merge-fixture-evidence: 3-point manual align (session 1, exact picks) OK — rms=0.0001 mm max_residual=0.0001 mm implied_scale=1.000000
merge-fixture-evidence: manual-3pt session 1 vs ground truth: 0.0001 mm / 0.000000 deg (align=manual)
```

Matches `docs/A13-merge.md` §4's own "exact picks recover the transform to < 10⁻³ mm" claim.

**Refine (ICP)** — session 1 was left at its exact-manual placement (already sub-mm, so ICP's
job here is mainly to prove the pipeline, not fix a bad coarse guess); session 2 is still at
`AlignSource::kNone` going in, only reachable through the global relaxation once session 1's
pairwise edge to it exists:

```
pair 0<->1: rms 119.21 mm -> 9.91 mm, overlap 33.8%/35.1%, 3 iterations (2 rolled back), converged=yes
pair 0<->2: rms 0.00 mm -> 0.00 mm, overlap 0.0%/0.0%, 0 iterations, low_overlap=yes (0-2 share no geometry, by construction — §11.2)
pair 1<->2: rms 107.43 mm -> 9.95 mm, overlap 35.6%/36.3%, 6 iterations (1 rolled back), converged=yes
global relaxation: chi2 0.00375188 -> 0.000164201 in 3 iterations
post-refine session 1 vs ground truth: 0.0923 mm / 0.002279 deg (align=relaxed)
post-refine session 2 vs ground truth: 0.1172 mm / 0.006926 deg (align=relaxed)
```

The **9.91 mm and 9.95 mm final residuals match `docs/A13-merge.md` §4's own reported 9.6 mm**
for the identical 0.5 m correspondence gate (this task's fixture reproduces A13's own building,
noise level and gate configuration exactly enough that the numbers land in the same place), and
**the chi² trajectory (3.75×10⁻³ → 1.64×10⁻⁴ in 3 iterations) matches `docs/A13-merge.md` §5's
own three-session relaxation table to 3 significant figures** — strong, independent confirmation
that this desktop task's fixture and code path reach the same engine machinery A13's own tests
did, not a divergent copy. Session 2 — never explicitly aligned by any UI action in this run —
ends up at 0.12 mm through the pose graph alone, which is the payoff §3.10 promises for "optional
global relaxation for >2 sessions".

**Build + publish, colour by session:**

```
merge-fixture-evidence: build OK — 34949 input -> 34706 merged points (63 dedup-dropped, 180 priority-dropped), 1 pages (2 shared)
```

`evidence/11-merge-fixture.png` (screenshot below, described) shows the merged cloud with three
sharply distinct colours (green/blue/red) and visible interleaving exactly at the two 4 m
overlap seams. `evidence/12-merge-dock.png` shows the workbench itself: the session table
(provenance, point counts, `GEOREF ±0.020 m` badges, align source, anchor flag, kept/dropped —
12,068/0, 11,428/91, 11,210/89), the pairs table (the exact numbers above), and the residual
chart for pair 0↔1 (a declining polyline from ~129 mm toward the converged value, §11.6).

### 11.8 Not verified / known gaps

* **Yaw search** (`align_yaw_search()`) is wired end to end (`onYawSearch()`/`yawSearchForCli()`)
  and surfaces `ambiguous`/`ok`/margin/runner-up honestly per the task's explicit ask, but was
  not run against a case engineered to trigger the ambiguous-symmetric-room or
  wrong-but-confident-corridor failure modes `docs/A13-merge.md` §4 documents — this task's
  fixture building is not symmetric and the yaw-search path was exercised by inspection and a
  short interactive session, not captured as scripted evidence with those specific failure
  geometries.
* **"Remove selected" is a message box, not a real removal.** `merge/merge.h`'s `MergeProject`
  has no `remove_session()` — sessions can only grow. Told to the operator rather than faked.
  A future desktop pass that wants this needs an engine-side seam.
* **Colour-by-session under Quick-scan's default profile is invisible** — §11.7's finding;
  the workbench does not currently force RGB colour mode while its own build is on screen.
* **All numbers are from the synthetic fixture.** `docs/A13-merge.md` §11 already states A13
  itself has no two real captures of one place; this task inherits the same limitation and does
  not add one.
* **`MergeSessionLoader`'s real-`.lscan` path is verified against one real capture, not two
  overlapping ones.** `--merge-add-project evidence/synth.lscan:real-project-test` (real
  `Engine` + unpaced `ReplaySource`, the exact "Add from open project"/"Import .lscan
  project…" code path) decoded **120,300 points** — matching §6's own "120,300 points
  decoded" for that same capture exactly — and added it as a real `MergeProject` session
  (`session 0 'real-project-test' — 120300 points`). What is NOT verified is aligning/
  refining/merging two sessions loaded this way against each other, because no two real
  captures of one overlapping place exist anywhere in this repo (the same gap
  `docs/A13-merge.md` §11 names for A13 itself). `--merge-fixture-evidence`'s quantitative
  numbers (§11.7) are all from the in-memory `SessionInput` path instead, which is the one
  `docs/A13-merge.md` §2 says is actually supported end to end today — the real-`.lscan`
  loader and the alignment/refine/build pipeline are each independently real and tested, just
  not together on two overlapping real captures.
* **No spatial index for the pick-mode picking** — inherits C3's own documented limitation
  (`ViewportWindow::pickPoint()` is O(N) over resident points); fine at this fixture's scale
  (~12k points/session), flagged for a multi-million-point session same as C3's own note.

---

## 12. C7 (transfer import/export)

New file: `src/app/TransferDialog.{h,cpp}` (`TransferExportDialog` + `TransferImportDialog`).
`MainWindow` gained drag-drop (`dragEnterEvent`/`dropEvent`, `setAcceptDrops(true)`), a
`QEvent::FileOpen` handler (`eventFilter()`, installed on `qApp` in the constructor and removed
in the destructor for the same "don't touch a half-destroyed `this`" reason §9.6 fixed a real
crash for), two File-menu actions, and `importTransferBundle()` as the one funnel every entry
point calls. `src/main.cpp` gained `--transfer-export`, `--transfer-import`,
`--transfer-export-dialog-shot`, `--transfer-import-dialog-shot`. New directory:
`packaging/Info.plist.in` (§12.4).

### 12.1 Two dialogs, direct on A5's `zip_export`/`zip_import` — not through `jobs::`

`docs/A15-jobs.md`/`docs/INT34-wiring.md` already give C4's "Transfer bundle…" a job-queue path
(`jobs::run_transfer_export()`, chained/cancellable through `Engine::jobs()`). C7's asks are
project-context, modal-ish actions — "Export transfer bundle…" off a specific open project,
drag-drop/file-association landing a bundle straight into the library — the same relationship
C3's `ExportDialog` already has to C4's queued export: two real, independent paths to the same
underlying primitive, not a wrapper around one another. So both new dialogs call
`scanengine::lscan::zip_export()`/`zip_import()` **directly**, on their own `std::thread`,
polling an `atomic<float>` from a `QTimer` — the exact shape `ExportDialog` established in C3
and `ReplayController` before that. This is also the most direct exercise available of the
progress/cancel hooks INT-34 added to `record/zip.h` (`ZipProgressFn`/`ZipCancelToken`,
`docs/INT34-wiring.md` §7): a real progress bar over payload bytes, and Cancel wired to
`ZipCancelToken::request_cancel()`.

**The two cancel semantics are asymmetric on purpose, and both dialogs say so in their own
status text**, not just in a comment: cancelling an export **removes** the half-written zip
(`record/zip.h`: "a partial archive on disk is worse than none, because it looks openable and
its central directory is missing"); cancelling an import **leaves** whatever had already been
extracted (`dest_dir` is the caller's directory and may have had contents before the call, so
deleting it is not this function's call to make).

### 12.2 Import: a real manifest sanity report, not a synthetic one

`TransferImportDialog::poll()` on success calls `readProject(dest_dir_)` — the **exact same
function** `MainWindow::openProject()` uses to populate the Projects panel — and
`showReport()` renders it into the same shape: a `QTreeWidget` of `StreamInfo` (chunks/payload/
span per stream) plus a warnings line (`truncated_tail_chunks`/`crc_mismatch_chunks`/
`unreadable_streams`, "no reader warnings — bundle looks sane" when clean). This is
deliberately `Project.cpp`'s reader, not a hand-rolled second summary or `jobs::
ImportValidationReport` (`jobs/transfer.h`) — using the app's own single source of truth for
"what does this .lscan directory contain" means a bundle that imports here is guaranteed to open
identically through "Open project…" afterward, because it *is* the same code path. "Open in
library" (`projectReady` signal) hands the resulting directory straight to
`MainWindow::openProject()`.

### 12.3 Landing in the library: drag-drop, File menu, and the macOS file-association seam

* **Drag-drop** — `MainWindow::dragEnterEvent`/`dropEvent` accept any dropped URL ending in
  `.lscan.zip` (or bare `.zip`, since a bundle need not always carry the doubled extension) and
  call `importTransferBundle(path)`. This works today, with **no packaging change needed** — Qt's
  drag-and-drop is bundle-independent, unlike file-open events (next bullet).
* **File menu** — "Export transfer bundle…" (enabled whenever a project is open) and "Import
  transfer bundle (.lscan.zip)…" open the two dialogs directly.
* **`QEvent::FileOpen`** — what macOS LaunchServices sends a running (or launching) app when the
  user double-clicks / "Open With"s a registered document type. Qt delivers it to the
  **application** object, not any widget, so `MainWindow` installs itself as an event filter on
  `qApp` (removed in `~MainWindow()`) and checks `event->type() == QEvent::FileOpen` there. This
  code is real and would fire correctly **if macOS ever routed a `.lscan.zip` open request to
  this process** — but today's executable is a plain Mach-O binary (§3.3's standing note), and
  LaunchServices only resolves document-type handlers through a real `.app` bundle's
  `Info.plist`. So this piece is written and inert until C8 packages a bundle — §12.4 is exactly
  the config that closes the gap, and this section says so rather than claiming an
  end-to-end-verified OS integration that does not exist yet on this build.

### 12.4 `packaging/Info.plist.in` — documented now, wired by C8

`desktop/packaging/Info.plist.in` declares `CFBundleDocumentTypes` for `.lscan.zip` (an
exported UTI, `com.lidarscan.transfer-bundle`, conforming to `public.zip-archive`) and,
optionally, `.lscan` project directories as an `LSTypeIsPackage` document type (the
GarageBand/Xcode-project trick for a double-clickable directory) so `.lscan` can feel native in
Finder too if C8 wants that. Not wired into `CMakeLists.txt` — no `MACOSX_BUNDLE`, no
`configure_file()`, no `macdeployqt` call exist yet, per §3.3's standing note that the app is
still a plain binary. The file's own header comment states exactly what it is for and exactly
what §12.3's `QEvent::FileOpen` handler is waiting on. `${LIDARSCAN_VERSION}` is left as a
CMake-style placeholder for whichever `project(... VERSION ...)` C8 lands.

### 12.5 Verification (2026-08-15, same host as §11.7)

`scripts/verify_c6c7.sh` reproduces this section too; raw output is `evidence/verify_c6c7.log`.

**Round trip, headless** (`--transfer-export`/`--transfer-import`, A5's functions called
directly with no dialog/thread — the fastest, most direct exercise of the primitives), against
the C1 evidence capture (`evidence/synth.lscan`, 193 D6 chunks / 394,287 bytes, §6):

```
transfer-export: evidence/synth.lscan -> evidence/transfer-roundtrip.lscan.zip: OK (398724 bytes)
transfer-import: evidence/transfer-roundtrip.lscan.zip -> evidence/transfer-roundtrip-imported.lscan: OK
transfer-import: manifest ok, profile quickscan, 193 chunks, 394287 bytes, 17.07 s span, 0 truncated-tail, 0 crc-mismatch, 0 unreadable streams
transfer-import:   stream lidar (COIN-D6 raw): 193 chunks, 394287 bytes
```

**193 chunks / 394,287 bytes on the way out exactly match 193 chunks / 394,287 bytes read back**
(the 398,724-byte zip is 394,287 bytes of payload plus stored-ZIP local/central-directory
overhead — record/zip.h's documented uncompressed format, §6 of `docs/A5-lscan.md`), 0 warnings
either side — the round trip the task asks for, verified via `FileRecordReader`'s own summary on
both ends, not a byte-for-byte diff of the directories (which would also have passed, but the
manifest/stream-summary comparison is what an operator actually sees).

**The real dialogs, screenshotted** (`--transfer-export-dialog-shot`/
`--transfer-import-dialog-shot`, which construct the actual `QDialog` and call
`triggerExportForCli()`/`triggerImportForCli()` — the same `onExport()`/`onImport()` the
buttons call, not a re-implementation): `evidence/14-transfer-export-dialog.png` shows a
completed export ("Export complete: evidence/synth.lscan/exports/synth.lscan.zip (389.4 KB)"),
progress bar at 100%, "Open containing folder" enabled. `evidence/13-transfer-import-report.png`
shows the import report described in §12.2 — manifest ok, 193 chunks, 385.0 KB, 17.07 s, the
stream table, "no reader warnings — bundle looks sane" — with "Open in library" enabled.

**A real bug found and fixed while building this evidence:** `TransferExportDialog`'s
constructor originally defaulted the output path to `<project_dir>/<name>.lscan.zip` — straight
into the project directory being zipped, not its `exports/` subfolder. `zip_export()` walks
"every regular file under `lscan_dir`" (`docs/A5-lscan.md` §6), so a second export would have
recursively included the first export's own zip inside itself, growing without bound across
repeated exports and corrupting the transfer bundle's own contents. Caught by inspecting the
first `--transfer-export-dialog-shot` screenshot (`evidence/synth.lscan/synth.lscan.zip` in the
path field, not `evidence/synth.lscan/exports/synth.lscan.zip`) — the same "screenshot ⇒ inspect
it ⇒ notice something is wrong" step §10.2 credits for catching a real bug in C5. Fixed:
the default path now always goes through `exports/`.

**Build:** clean configure + build from scratch, exit 0, zero warnings from any file under
`desktop/src`; same 239-warning vendored-Livox-SDK2 baseline.

### 12.6 Not verified / known gaps

* **`QEvent::FileOpen` / real macOS document-type registration is unverified against a real
  `.app` bundle** — §12.3 explains exactly why (no bundle exists yet) and §12.4 is the
  config C8 needs. Drag-drop, which needs no bundle, **is** verified (real `QDropEvent` path,
  same `importTransferBundle()` funnel).
* **No cross-machine transfer was tested** — both ends of every round trip in this task ran on
  the same host. The zip format itself is a real, standards-conformant archive (openable by
  Finder/`unzip`/Explorer per `docs/A5-lscan.md` §6), so this is an inference from the format's
  own conformance, not a separate measurement.
* **Large-bundle cancel timing was not measured** — `evidence/synth.lscan` zips to 389 KB, fast
  enough that a manually-triggered Cancel click is hard to land mid-transfer reliably in a
  scripted run. The cancel code path itself (`ZipCancelToken::request_cancel()`, polled "between
  entries and every 64 KiB inside one" per `record/zip.h`) is real and shared with C4's own
  Transfer-bundle job, which `docs/A15-jobs.md` already exercises at the unit-test level
  (`transfer/*` cases, `docs/INT34-wiring.md` §8's zip progress/cancel test).
* **`.lscan.zip` vs bare `.zip` extension handling** — both are accepted by drag-drop/the
  import dialog's file filter; only `.lscan.zip` is wired to `QEvent::FileOpen` (macOS
  file-association is normally one canonical extension per document type, and `docs/A5-lscan.md`
  itself calls the bundle a `.lscan.zip`).

---

## 13. C8 (desktop packaging)

**Status: the macOS universal DMG is real — built, mounted, launched on both
architectures. Windows and Linux are staged scripts, validated only as far as
their own tooling allows.**

Owner decision taken at the start of this task, against `NOTES.md` §3.2's
blocker: **build Filament v1.75.0 x86_64 from source and ship one TRUE universal
DMG.** Not two DMGs, not Intel dropped.

New/changed files:

```
tools/build_filament_x86_64.sh   the missing x86_64 Filament slice, from source
tools/make_universal_filament.sh lipo it against the arm64 prebuilt set
tools/fetch_qt_universal.sh      official universal Qt via aqtinstall
tools/make_icon.sh               .icns/.png/.ico from a stdlib-only PNG writer
tools/package_macos.sh           universal .app + macdeployqt + codesign + DMG
CMakeLists.txt                   universal-Filament selection, MACOSX_BUNDLE,
                                  Info.plist configure, Resources placement
src/render/ViewportWindow.cpp    points.filamat: also search ../Resources (§13.4)
packaging/PACKAGING.md           the full procedure incl. real signing/notarization
packaging/README.md              the map of packaging/
packaging/Info.plist.in          CFBundleExecutable now comes from CMake (§13.4)
packaging/macos/entitlements.plist
packaging/windows/lidarscan.nsi, build_installer.ps1, CH340-driver.html
packaging/linux/build_appimage.sh, build_deb.sh, debian-control.in,
                 debian-postinst, debian-postrm, lidarscan.desktop,
                 lidarscan.xml, 99-lidarscan.rules, README-udev.txt
packaging/ci/{macos-dmg,windows-installer,linux-packages}.yml.snippet + README.md
.gitignore                       the new third_party/ and dist/ trees
```

`.github/` is owned by another agent this wave, so the three CI jobs are staged
in `packaging/ci/` as `.yml.snippet` files for that owner to merge. All three
parse as valid YAML.

### 13.1 Building Filament x86_64 — what it actually took

`filament-v1.75.0-mac.tgz` ships `lib/arm64` and nothing else. Cross-compiling
is nominally just `-DCMAKE_OSX_ARCHITECTURES=x86_64`, and that part was true.
Four things were not obvious:

**1. The build runs tools it just built.** `matc` compiles Filament's ~40
built-in materials and `resgen` turns the results into assembly that goes
straight into `libfilament.a`. Under an x86_64 configure those tools are x86_64
too — the build wants to execute Intel binaries on an Apple-Silicon host.
**Rosetta 2 handles it transparently** (present on this machine and on GitHub's
`macos-14` images). The documented alternative is Filament's own
`-DIMPORT_EXECUTABLES_DIR=<arm64 build dir>` escape hatch, which the script
accepts from the environment — but it needs a *complete prior arm64 CMake build
tree of the same revision* because it consumes that build's generated
`ImportExecutables-Release.cmake`, and the release tarball is not such a tree.
So that route means building Filament twice; Rosetta means building it once.

**2. `ninja abseil` does not exist.** The target is **`filament-abseil`** —
Filament prefixes its vendored abseil so it cannot collide with a system one,
and renames it to `libabseil.a` on the way into the release tarball. First run
died here.

**3. `geometry` and `abseil` ship as *combined* archives, not the target
outputs — and getting this wrong is silent.** Both use Filament's own
`combine_static_libs()` helper (`CMakeLists.txt:777` →
`build/linux/combine-static-libs.sh`), a POST_BUILD step that merges the target
plus all of its static dependencies into `lib<name>_combined.a`; an
`install(FILES … RENAME)` rule is what ships it under the plain name. The plain
ninja output for `filament-abseil` is an **empty 656-byte archive** (the tnt
target aggregates dependencies and has no sources of its own). Copy that and
everything configures, links, and then fails on undefined `absl::` symbols. The
collector now prefers `*_combined.a` and has a "no archive under 4 KB"
tripwire so the general class of this bug cannot recur silently.

| lib | arm64 (prebuilt) | x86_64 (this build) |
| --- | ---: | ---: |
| `libabseil.a` **before the fix** | 2,013,864 | **656** ← the bug |
| `libabseil.a` after | 2,013,864 | 1,860,632 |
| `libgeometry.a` before | 291,736 | 172,896 (target output, not the combined one) |
| `libgeometry.a` after | 291,736 | 324,944 |

**4. Only build what the app links.** `-DFILAMENT_SKIP_SAMPLES=ON
-DFILAMENT_SKIP_SDL2=ON` plus passing the 12 needed libraries to ninja as
explicit targets skips gltfio, viewer, matdbg, samples, image/imageio, assimp,
the tests and the Java bindings. **1,047 ninja edges, 2m 00s on 10 jobs** — not
the 30–60 minutes budgeted. (Full clone ≈ 3 min for 1.7 GB, configure ≈ 1.5 min.)

Exact provenance is written to `third_party/filament-x86_64/BUILDINFO.txt` by
the script itself:

```
filament version   : v1.75.0
filament commit    : 0e58877c09afb1aacd09ff640f74d2adcd2a7e80
build time         : 2m 0s on 10 jobs
host               : arm64 macOS 26.5.1 (25F80)
compiler           : Apple clang version 21.0.0 (clang-2100.1.1.101)
macOS SDK          : 26.5 @ /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
cmake 4.4.2 / ninja 1.13.2 / deployment target 11.0
host-tool strategy : Rosetta 2 (x86_64 matc/resgen translated)
```

`make_universal_filament.sh` then `lipo`s each of the 12 archives against the
prebuilt arm64 set. It deliberately does **not** rebuild arm64 from source: the
prebuilt arm64 slice is the one S3 measured 138 fps and 1,105 swapchain
recreates against, and replacing those bytes with our own rebuild would throw
that provenance away for no gain. `lipo` keeps them byte-for-byte and only
*adds* Intel. All 12 outputs verify as `x86_64 arm64`; a single-arch result is a
hard error, not a warning, because that is precisely the failure that produces a
"universal" DMG which will not launch on an Intel Mac.

**`matc` is not built for x86_64 and does not need to be.** It is a build-time
tool that runs on the developer's/CI's own machine; the arm64 one from the
prebuilt release compiles `materials/points.mat` for either target.

### 13.2 Qt: Homebrew cannot do this, aqtinstall can

```
$ lipo -info /opt/homebrew/opt/qt/lib/QtCore.framework/QtCore
Non-fat file: ... is architecture: arm64
```

Homebrew builds per-architecture bottles by design; there is no universal Qt
bottle and there will not be one. So a universal `LidarScan.app` **cannot be
linked against Homebrew Qt at all** — its x86_64 slice would have no Qt. This is
the Qt-side twin of the Filament blocker and it is not written down anywhere in
C1–C7, because until now nothing needed two architectures.

Options weighed:

| | Verdict |
| --- | --- |
| Qt online installer | Universal and official, but an interactive GUI needing a Qt account login. Unusable headless, unusable in CI. |
| **aqtinstall** | Downloads **the same official artifacts** from the same Qt CDN, non-interactively, no account. **Taken.** |
| Build Qt from source universal | Hours, to reproduce binaries the vendor already publishes. |

The Qt Company's macOS desktop builds have been universal since Qt 6.2, and the
archive names say so out loud:
`qtbase-MacOS-MacOS_15-Clang-MacOS-MacOS_15-X86_64-ARM64.7z`. Verified:

```
QtCore         x86_64 arm64
QtGui          x86_64 arm64
QtWidgets      x86_64 arm64
QtNetwork      x86_64 arm64
QtSerialPort   x86_64 arm64
macdeployqt    x86_64 arm64
```

**6.11.1 deliberately** — the exact version S3 and C1–C7 built and measured
against (§6), so switching from Homebrew to the official binaries changes the
*architecture* and nothing else. Qt stays **dynamically linked** (frameworks in
`Contents/Frameworks`), which Tech Spec §1's "LGPLv3 with dynamic linking" makes
mandatory: no static Qt, ever, without a commercial licence.

### 13.3 The engine needed nothing

`add_subdirectory(../engine)` inherits `CMAKE_OSX_ARCHITECTURES`, and the
universal build — including the vendored Livox SDK2 — went through first time,
with the same 239-warning vendored baseline §6 and §8.7 record and zero warnings
from `desktop/src`. The `macos-universal` preset had already proved this on the
engine's own; this task confirms it holds when the engine is consumed as a
subdirectory.

### 13.4 Bundle changes

`-DLIDARSCAN_MACOS_BUNDLE=ON` (default **OFF**). Default-off is deliberate:
`scripts/verify{,_c2c3,_c4c5,_c6c7}.sh` — all of C1–C7's evidence — invoke
`build/lidarscan`, and moving the executable to
`build/LidarScan.app/Contents/MacOS/LidarScan` would silently break all four.
`tools/package_macos.sh` turns it on for its own build tree.

* **`points.filamat` → `Contents/Resources`.** `ViewportWindow.cpp` now searches
  `applicationDirPath()` first and `applicationDirPath()/../Resources` second,
  so one binary works in both the dev-build and the bundle layout. This is the
  one line §7 listed as "fine for a dev build, wrong for a bundle, and it is
  C8's to change".
* **`CFBundleExecutable` is filled from CMake.** C7's `Info.plist.in` hard-coded
  `lidarscan`, but the bundle wants to be `LidarScan.app` with a matching
  `Contents/MacOS/LidarScan` (the name Finder, the Dock and the Applications
  folder show). Rather than let a hand-written plist and a CMake `OUTPUT_NAME`
  drift — a mismatch there is a bundle macOS refuses to launch outright — both
  now come from the same `LIDARSCAN_EXECUTABLE` variable.
* **CMakeLists hard-fails on a half-built universal tree.** Asking for both
  architectures makes `third_party/filament-universal/` *required*, with an
  error naming the three scripts to run. The alternative — falling back to the
  arm64-only prebuilt — links with warnings and produces an app whose Intel
  slice is missing every Filament symbol, i.e. exactly the silent failure this
  whole task exists to prevent.

### 13.5 Verification (2026-08-15, same host as §6: Apple M4, macOS 26.5.1)

**The DMG is real.** `dist/LidarScan-0.1.0-universal.dmg`, 30 MB (66 MB app),
UDZO/zlib, with an `/Applications` symlink.

```
$ lipo -info build-universal/LidarScan.app/Contents/MacOS/LidarScan
Architectures in the fat file: .../LidarScan are: x86_64 arm64
```

Every deployed Qt framework too — `macdeployqt` from the universal Qt copied fat
frameworks, it did not thin them:

```
QtCore  QtDBus  QtGui  QtNetwork  QtSerialPort  QtSvg  QtWidgets   →  all "x86_64 arm64"
```

**Mounted and launched from the mounted image** (not from the build tree):

```
$ hdiutil attach dist/LidarScan-0.1.0-universal.dmg -readonly
/dev/disk5s1   ...   /Volumes/LidarScan 0.1.0

$ lipo -info "/Volumes/LidarScan 0.1.0/LidarScan.app/Contents/MacOS/LidarScan"
Architectures in the fat file: ... are: x86_64 arm64

$ "/Volumes/LidarScan 0.1.0/LidarScan.app/Contents/MacOS/LidarScan" \
    --project=evidence/synth.lscan --replay=0 --shot=evidence/15-dmg-launch.png ...
[lidarscan] screenshot OK -> evidence/15-dmg-launch.png | 120300 pts / 1 pages |
            51.6 fps | cpu p95 0.22 ms | gpu p95 1.98 ms | 758x946 px dpr 1.00 | swapchains 0
```

That is the full C1 pipeline — the 120,300-point synthetic D6 capture replayed
through the engine into the Filament viewport — running out of a read-only
mounted DMG, with `points.filamat` resolved from `Contents/Resources` and Qt
resolved from `Contents/Frameworks`. `FEngine resolved backend: Metal`,
`Selected physical device 'Apple M4'`.

**And the Intel slice was actually executed, not merely counted.** Running the
same binary through Rosetta forces the x86_64 slice:

```
$ arch -x86_64 "/Volumes/LidarScan 0.1.0/LidarScan.app/Contents/MacOS/LidarScan" \
    --project=evidence/synth.lscan --replay=0 --shot=evidence/16-dmg-launch-x86_64.png ...
[lidarscan] screenshot OK -> evidence/16-dmg-launch-x86_64.png | 120300 pts / 1 pages |
            55.5 fps | cpu p95 0.32 ms | gpu p95 1.34 ms | 758x946 px dpr 1.00 | swapchains 0
```

Same 120,300 points, same Metal backend, and the two screenshots are
**byte-identical** (`sha256 9708459017…`), which is a stronger result than "it
launched": the x86_64 Filament we built from source, the x86_64 Qt and the
x86_64 engine produce bit-for-bit the same framebuffer as the arm64 ones.

**A Finder-style launch works too** — `open "/Volumes/LidarScan 0.1.0/LidarScan.app"`
gave a running process from the mounted image, visible to System Events as
`LidarScan`.

**C7's file association is now verified end to end for the first time.** §12.6
listed macOS document-type registration as unverifiable without a bundle.
LaunchServices registered the app *from the mounted DMG*:

```
$ lsregister -dump | grep -A2 com.lidarscan
identifier:   com.lidarscan.desktop
claimed UTIs: com.lidarscan.project, com.lidarscan.transfer-bundle
type id:      com.lidarscan.transfer-bundle   flags: exported
type id:      com.lidarscan.project
```

**Signing, honestly.** Ad-hoc (`codesign -s -`), inside-out, and it validates:
`valid on disk` + `satisfies its Designated Requirement`, `Format=app bundle
with Mach-O universal (x86_64 arm64)`. But:

```
$ spctl -a -vvv -t exec build-universal/LidarScan.app
build-universal/LidarScan.app: rejected
```

That is recorded, not hidden. On any *other* Mac this app is quarantined and
refused until it is notarized. `packaging/PACKAGING.md` §1.4 has the complete
Developer ID + `notarytool` + `stapler` procedure, the four entitlements the
hardened runtime needs and why each one is needed, the four things deliberately
*not* requested and why, and the four notarization rejections this build is
already shaped to avoid.

Evidence: `evidence/15-dmg-launch.png` (arm64),
`evidence/16-dmg-launch-x86_64.png` (x86_64, byte-identical),
`evidence/nsis-syntax-check.log`.

### 13.6 Windows and Linux: what was actually validated

Both are **staged, never executed**, and the reason is not laziness — §3.1 marks
both renderers UNVERIFIED and neither `NativeSurface_win.cpp` nor
`NativeSurface_linux.cpp` has ever been compiled by anyone. A working installer
for an app that does not build is groundwork, not a deliverable. What *was*
checked, with the tooling that exists on macOS:

| Check | Result |
| --- | --- |
| `makensis -V3` on `lidarscan.nsi` (Homebrew NSIS 3.12, stub staging tree) | **compiles clean, emits a real 146 KB installer** — 6 pages, 5 sections, 760 instructions, LZMA. `evidence/nsis-syntax-check.log` |
| `desktop-file-validate lidarscan.desktop` | clean (one `Categories` warning found and fixed) |
| `xmllint --noout lidarscan.xml` | well-formed |
| `plutil -lint` on `entitlements.plist` + the generated `Info.plist` | OK |
| `shellcheck -S warning` across all 9 new shell scripts | clean |
| `yaml.safe_load` on the 3 CI snippets | valid |

Three design decisions worth carrying forward:

* **NSIS, not MSIX.** MSIX cannot be installed at all without a code-signing
  certificate (even for a local test), forces the packaged-app identity model
  onto an app that opens raw COM ports, and would still need a separate
  unpackaged build for CI smoke tests.
* **The CH340 driver is a pointer page, not a bundled binary.** WCH's
  `CH341SER.EXE` redistribution terms are not something this task can clear, the
  file churns, and a stale bundled driver can *downgrade* a working in-box one —
  Windows 10/11 both ship a CH340 driver that works for most units. So the
  installer ships `drivers/CH340-driver.html`: try plugging it in first, how to
  check Device Manager, then the vendor link. Same message
  `CaptureWindow`'s per-OS guidance already gives.
* **The Windows installer does not claim `.zip`.** Windows has no `.lscan.zip`
  extension — only `.zip`, owned by the shell's zip handler — so claiming it
  would hijack every archive on the machine. `.lscan` gets a ProgID outright;
  `.lscan.zip` is exposed through `OpenWithProgids` as an "Open with →
  LidarScan" entry. §12.6's note that only `.lscan.zip` is wired to
  `QEvent::FileOpen` has this as its Windows counterpart.

On Linux both artifacts are produced because they do different jobs: the
AppImage bundles Qt and needs no root but **structurally cannot** install the
udev rule or register the MIME types; the `.deb` depends on the distro's Qt (so
security updates arrive via apt) and does both in its postinst.
`99-lidarscan.rules` uses `TAG+="uaccess"` rather than requiring
`usermod -aG dialout` + a re-login, which is a poor first-run experience for a
field tool. And `lidarscan.desktop` forces `QT_QPA_PLATFORM=xcb` because
`NativeSurface_linux.cpp` **refuses Wayland outright** — on a default Ubuntu
22.04+/Fedora session the viewport would not come up at all without XWayland.

### 13.7 Not verified

* **No real Intel Mac.** The x86_64 slice was executed under **Rosetta 2 on an
  M4**, which proves the code is correct x86_64 and that every x86_64 library in
  the bundle links and runs. It does **not** prove behaviour on genuine Intel
  hardware, where the GPU is an Intel Iris / AMD Radeon with a different Metal
  feature level — Filament's Metal backend reported `MTLGPUFamilyApple7` here.
  Renderer behaviour on real Intel silicon is an expectation, not a measurement.
* **Not notarized, and not signable here** — no Apple Developer ID exists in
  this environment. `spctl` says `rejected`, as it should.
* **`create-dmg` was not used** (not installed); the DMG came from `hdiutil`.
  The `create-dmg` branch of `package_macos.sh` — the one that sets icon
  positions and window size — has never run.
* **Windows and Linux: nothing beyond syntax.** No compile, no install, no
  launch, on either.
* **No cross-machine install test.** The DMG was mounted on the machine that
  built it, so the quarantine/Gatekeeper path a downloaded DMG takes was never
  exercised.
* **The icon is a generated placeholder**, not designed artwork —
  `tools/make_icon.sh` draws it with a stdlib-only PNG writer so that swapping in
  a real mark later is "edit the script, re-run, commit" rather than hunting for
  whoever produced a binary blob.
