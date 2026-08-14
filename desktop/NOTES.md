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
      CaptureWindow.{h,cpp}   device setup (QSerialPort D6 / Mid-360 IP), start/stop
    render/
      NativeSurface.h         per-OS swapchain-handle shim (the S3 recommendation)
      NativeSurface_mac.mm    CAMetalLayer path — the proven one
      NativeSurface_win.cpp   HWND, UNVERIFIED, with S3 §8 inline
      NativeSurface_linux.cpp X11/XCB/Wayland analysis, UNVERIFIED, S3 §8 inline
      DisplayLink.h           render clock interface (replaces S3's spin loop)
      DisplayLink_mac.mm      CADisplayLink (macOS 14+) → CVDisplayLink → timer
      DisplayLink_generic.cpp Windows/Linux: the timer fallback, with the real
      DisplayLinkFallback.*   per-platform replacements documented
      ViewportWindow.{h,cpp}  QWindow + Filament engine/view/renderer, resize, screenshots
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
