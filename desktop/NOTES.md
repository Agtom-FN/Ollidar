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
  tools/fetch_filament.sh     harvested from S3; pins v1.75.0
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

### 3.3 Packaging (C8)

* **macOS** — the executable is currently a plain binary, not a `.app` bundle. C8 needs
  `MACOSX_BUNDLE`, `Info.plist`, `macdeployqt`, codesign + notarize, and must place
  `points.filamat` inside `Contents/Resources` (the app resolves it via
  `QCoreApplication::applicationDirPath()`, which is the one line that changes).
* **Windows** — NSIS/MSIX, `windeployqt`, plus the CH340 driver pointer the capture window
  already names.
* **Linux** — AppImage + `.deb`, `linuxdeployqt`, and the udev rule for `/dev/ttyUSB*` that
  the capture window's hint text already tells the user about.
* Qt is used under **LGPLv3 with dynamic linking** (§1), which every packaging path must
  preserve — no static Qt.

---

## 4. Where the other tasks plug in

| Task | Seam |
| --- | --- |
| **C2** capture flows | `CaptureWindow` — wizards, self-tests, reconnect UX, per-OS driver install guidance. The engine wiring (`addD6`/`addMid360`/`startSession`, the serial read+write paths) is done; C2 owns the UX around it. Also: move the D6 read loop to its own thread if a future device is faster than the D6. |
| **C3** review workspace | `ViewportWindow` + `PagedCloudRenderer`. Measure tools attach to the viewport; the EDL post-process pass, real coarse-to-fine LOD and the trajectory/pose-graph overlays are the three renderer features C1 left as documented gaps. Export UI hangs off `export/exporter.h`. |
| **C4** processing queue | New dock; `EngineHost` already owns the Engine and pumps `kJobProgress` events, which currently fall through `describeEvent()`'s default case. |
| **C5** floor plan | New workspace; the height-clip band in `DisplayParams` is already live and is literally §3.6's slice control. |
| **C6** merge workbench | New window over A13; `PagedCloudRenderer` is per-`PageId` and `PageStore` pages are single-stream, so per-session provenance and colour-by-session need no renderer redesign. |
| **C7** transfer import | `lscan::zip_import` + `Project.cpp`'s `readProject`; add the file association and a drag-drop handler on `MainWindow`. |
| **C8** packaging | §3 above. |
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
| **`points.filamat` is loaded from `applicationDirPath()`** | Fine for a dev build, wrong for a bundle — one line, and it is C8's to change. |
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
