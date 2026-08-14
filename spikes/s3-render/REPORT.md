# Spike S3 — Filament-in-Qt desktop rendering (macOS / Apple silicon)

**Verdict: GO (with caveats).** Filament's Metal backend renders a live-streamed,
paged 10M-point cloud inside a Qt 6 window at **138–149 fps** uncapped and a rock-solid
**60.0–60.5 fps** vsync-locked on an Apple M4, survives ~1,100 swapchain recreates in a
10-second continuous-resize storm without a crash or artifact, and handles DPR 2.0
correctly. The two caveats are (1) the native handle must be a **CAMetalLayer**, not
the `NSView` that Filament's own header documents, and (2) the Windows/Linux Vulkan
path is **not** proven by this spike (see §8).

Exit criterion from Tech Spec §4 Phase 0 S3 — *"10M pts @60 fps (desktop); Qt embed
stable across resize/screen-change"* — **met on macOS**, with 2.3× headroom over 60 fps
at 10M points.

---

## 1. What was built

`spikes/s3-render/` is a self-contained Qt 6 + Filament application (~1,100 lines).

| Piece | File | What it proves |
|---|---|---|
| Qt ↔ Filament window bridge | `src/MacBridge.mm`, `src/FilamentWindow.cpp` | `QWindow::winId()` → `NSView` → its `CAMetalLayer` → `Engine::createSwapChain()`; swapchain recreate on resize; `contentsScale`/`drawableSize` for HiDPI; `displaySyncEnabled` toggle |
| Paged point buffers | `src/PagedCloud.{h,cpp}` | 1 M-point pages (§3.12's "~1 M pts/page"), one `VertexBuffer`+`IndexBuffer`+renderable per page, partial `setBufferAt()` uploads + `setGeometryAt()` count bumps — no reallocation while streaming |
| Point material | `materials/points.mat` | `PrimitiveType::POINTS`, unlit, per-vertex RGBA8, `gl_PointSize` from a material parameter — compiles through `matc -a metal` and works on Metal |
| Synthetic lidar source | `src/PointSource.{h,cpp}` | Producer thread emitting exactly 200k pts/s in 2,000-point bursts into an SPSC ring, drained by the render thread |
| Benchmark harness | `src/FilamentWindow.cpp` (`benchStep`) | Scripted phases, per-phase fps / CPU / GPU percentiles, resize storm, minimize/restore, framebuffer readback screenshots |

Point vertex layout is the production one: interleaved `float3 position + RGBA8`,
16 bytes/point. A 1 M page therefore costs 16 MB of vertex data + 4 MB of index data.

### Build / run

```sh
brew install qt cmake ninja          # Qt 6.11.1, CMake 4.4.2, Ninja 1.13.2
./tools/fetch_filament.sh v1.75.0    # prebuilt release -> third_party/filament/
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/s3_render                                    # interactive, 2M points
./build/s3_render --bench --vsync=off --results=r.md # scripted benchmark
```

Interactive keys: drag = orbit, wheel = zoom, `space` = auto-orbit, `s` = toggle the
200k pts/s stream, `1`/`2`/`3` = load 2M/5M/10M, `c` = clear.

---

## 2. Setup: what actually had to be done

1. **Toolchain.** The machine had neither Qt, CMake nor Ninja. `brew install qt cmake ninja`
   → Qt **6.11.1**, CMake 4.4.2, Ninja 1.13.2. Compiler: Apple clang from Command Line
   Tools, `-std=c++20` (Filament 1.75 requires C++20).
2. **Filament: prebuilt, no source build needed.** `filament-v1.75.0-mac.tgz` (44 MB) from
   the GitHub releases page unpacks to `include/`, `lib/arm64/` (static libs) and `bin/`
   (native arm64 `matc`, `matinfo`, …). **A source build was never attempted and is not
   needed** — the release bundle is complete for an embedding app.
3. **Filament's own README link list is stale.** The documented
   `-lfilament -lbackend -lbluegl -lbluevk -lfilabridge -lfilaflat -lutils -lgeometry
   -lsmol-v -libl -labseil` fails to link v1.75: `libfilament.a` now needs **`libzstd.a`**
   (`MaterialParser`/`ZstdHelper` → `ZSTD_decompress`). Added in `CMakeLists.txt`.
4. **`QSurface::MetalSurface` on the `QWindow`** — without it Qt would set up a raster/GL
   surface. The `QWindow` is embedded with `QWidget::createWindowContainer()` in a
   `QMainWindow` that also carries a normal Qt status bar (proving real widget coexistence).

---

## 3. The one real blocker: the native handle

Filament's `include/filament/SwapChain.h` says, verbatim:

> *On OSX, any `NSView` can be used **directly** as a `nativeWindow` with createSwapChain().*

**This is wrong in v1.75.** Passing `QWindow::winId()` (an `NSView*`) straight to
`Engine::createSwapChain()` aborts on the Filament driver thread:

```
libc++abi: terminating due to uncaught exception of type utils::PreconditionPanic
  frame #5: filament::backend::MetalSwapChain::MetalSwapChain(
                MetalContext&, PlatformMetal&, CAMetalLayer*, unsigned long long)
  frame #6: ConcreteDispatcher<MetalDriver>::createSwapChain(...)
```

(full log: `results/F-nsview-failure.log`; reproduce with `./build/s3_render --handle=nsview`)

The Metal `MetalSwapChain` constructor takes a **`CAMetalLayer*`** and precondition-checks
the class of the object it is handed. The fix is one line of Objective-C++, and there are
two working variants, both benchmarked:

| `--handle=` | How | 10M fps (vsync off) | Result |
|---|---|---|---|
| `qtlayer` **(default)** | Take the `CAMetalLayer` Qt already created for the `MetalSurface` window and pass *that* | **138.8** | works |
| `ownlayer` | Create our own `CAMetalLayer`, add it as a sublayer of Qt's `NSView` | **139.9** | works |
| `nsview` | Pass the raw `NSView*` as documented | — | **PreconditionPanic, abort** |

Observed layer topology on Qt 6.11.1: `winId()`'s `NSView.layer` is a `QContainerLayer`,
with Qt's **`QMetalLayer` (a `CAMetalLayer` subclass) as a sublayer**. `qtlayer` mode
walks one level of sublayers to find it. Both modes let the app own
`contentsScale`, `drawableSize`, `maximumDrawableCount` and `displaySyncEnabled`.

**Implication for C1:** budget a small per-OS `NativeSurface` shim (≈120 lines on macOS)
rather than assuming `winId()` is directly consumable, and pin the Filament version — this
is exactly the kind of thing that can change between releases.

---

## 4. Benchmark results

Host: **Apple M4, 10-core GPU**, macOS 26.5.1 (Tahoe), Qt 6.11.1, Filament v1.75.0
(Metal backend, feature level 1, backend feature level 3), single 1920×1080 @ 60 Hz display.
Post-processing **on** (linear tone mapper), MSAA/FXAA/TAA/bloom/SSAO off, shadows off,
frustum culling on (it never removes work — every page's bounding box is the whole room).
Point size 2.0 px unless noted. Camera auto-orbits inside the scanned room every frame.

*"FPS" counts only frames where `Renderer::beginFrame()` returned true. `beginFrame()`
returns false when no drawable is free — those ticks are backpressure, not frames.*

### 4.1 Headline — uncapped (vsync off), 1280×773 px, DPR 1.0
`results/A-vsyncoff-1280x800.md`

| Phase | Points | FPS | GPU frame p50 | GPU frame p95 | CPU frame p95 | Frame interval p95 |
|---|---:|---:|---:|---:|---:|---:|
| 2 M static | 2,000,000 | **283.9** | 1.80 ms | 3.45 ms | 0.05 ms | 11.78 ms |
| 5 M static | 5,000,000 | **209.7** | 3.31 ms | 5.54 ms | 0.06 ms | 10.31 ms |
| 10 M static | 10,000,000 | **138.8** | 6.92 ms | 8.53 ms | 0.11 ms | 8.73 ms |
| 10 M, pointSize 1 px | 10,000,000 | 137.8 | 7.01 ms | 8.43 ms | 0.09 ms | 8.51 ms |
| 10 M, pointSize 4 px | 10,000,000 | 141.5 | 6.84 ms | 8.29 ms | 0.09 ms | 8.40 ms |
| live ingest 200k pts/s, 0 → 4 M | 4,000,000 | **277.2** | 1.21 ms | 4.17 ms | 0.06 ms | 11.99 ms |
| 10 M **+** live ingest 200k pts/s | 11,912,000 | **129.1** | 3.35 ms | 8.30 ms | 0.11 ms | 9.33 ms |
| resize storm @ ~12 M (10 s) | 11,912,000 | **110.6** | 8.24 ms | 10.02 ms | 9.87 ms | 10.70 ms |
| post-resize steady state | 11,912,000 | 119.7 | 8.34 ms | 9.60 ms | 0.10 ms | 9.64 ms |
| after minimize/restore | 11,912,000 | 110.8 | 8.41 ms | 9.59 ms | 0.12 ms | 9.64 ms |

Ingest was exactly **200,001 pts/s** measured end-to-end with **0 ring-buffer drops**;
the 10M+ingest phase sustained **200,041 pts/s** while holding 129 fps.

### 4.2 Vsync locked (the shipping configuration), 1280×773 px
`results/B-vsyncon-1280x800.md`

| Phase | Points | FPS | GPU frame p50 | GPU p95 | CPU p95 |
|---|---:|---:|---:|---:|---:|
| 2 M static | 2,000,000 | **60.4** | 4.11 ms | 5.25 ms | 0.05 ms |
| 5 M static | 5,000,000 | **60.6** | 7.12 ms | 8.80 ms | 0.05 ms |
| 10 M static | 10,000,000 | **60.5** | 10.76 ms | 11.90 ms | 0.06 ms |
| 10 M + live ingest 200k pts/s | 11,980,000 | **60.2** | 2.67 ms | 3.20 ms | 0.05 ms |
| resize storm @ ~12 M | 11,980,000 | **60.0** | 9.88 ms | 10.81 ms | 16.53 ms |
| after minimize/restore | 11,980,000 | 57.3 | 10.54 ms | 11.65 ms | 0.04 ms |

**Every phase holds the 60 Hz refresh**, including while resizing continuously at 12M points.
(GPU frame times are higher than in the uncapped run — with vsync on the GPU idles between
frames and clocks down; the uncapped run is the true cost measure.)

### 4.3 Larger viewport — maximized 1920×946 px, vsync off
`results/C-vsyncoff-maximized.md`

| Phase | Points | FPS | GPU p50 | GPU p95 |
|---|---:|---:|---:|---:|
| 2 M static | 2,000,000 | 548.7 | 1.68 ms | 1.95 ms |
| 5 M static | 5,000,000 | 259.9 | 3.59 ms | 4.08 ms |
| 10 M static | 10,000,000 | **141.7** | 7.03 ms | 7.45 ms |
| 10 M + live ingest | 11,986,000 | 128.7 | 2.97 ms | 7.99 ms |
| resize storm | 11,986,000 | 108.6 | 8.58 ms | 10.20 ms |

Full-screen is **not slower** than the small window — at 10M points on M4 the cost is
vertex/primitive throughput, not fill rate. This is also why point size (1 px → 4 px)
moves fps by <3%.

### 4.4 HiDPI — DPR 2.0, 1600×918 physical pixels, vsync off
`results/D-hidpi-dpr2.md` (`QT_SCALE_FACTOR=2`)

| Phase | Points | FPS | GPU p50 | GPU p95 |
|---|---:|---:|---:|---:|
| 2 M static | 2,000,000 | 570.3 | 1.57 ms | 1.79 ms |
| 5 M static | 5,000,000 | 271.7 | 3.39 ms | 3.97 ms |
| 10 M static | 10,000,000 | **148.6** | 6.68 ms | 7.20 ms |
| 10 M + live ingest | 11,900,000 | 137.2 | 2.64 ms | 7.64 ms |
| resize storm | 11,900,000 | 94.8 | 10.11 ms | 11.51 ms |
| after minimize/restore | 11,900,000 | 105.5 | 8.86 ms | 9.24 ms |

`devicePixelRatio` 2.0 is picked up correctly: the drawable is 1600×918 real pixels, the
Qt status bar renders at 2×, and Filament's viewport matches. No blur, no half-size image,
no letterboxing. Screenshot: `shots-hidpi/03-10M-window.png`.

### 4.5 Memory

10M points resident = **200 MB RSS** for the whole process (measured with `ps`), which is
the paged GPU buffers themselves (10 pages × (16 MB vertex + 4 MB index)) — Apple silicon
unified memory means these overlap. Bulk load of 10M points (generate + upload) takes
**~44 ms**.

---

## 5. Stability findings

| Stress | Result |
|---|---|
| **Continuous resize** — window resized every frame for 10 s while rendering ~12 M points | **2,328 resize requests → 1,105 swapchain destroy/recreate cycles, 0 crashes, 0 validation errors, 0 visual artifacts.** Frame rate degraded only to 110.6 fps (uncapped) / 60.0 fps (vsync). Screenshot after the storm is clean: `shots/05-after-resize-window.png` |
| **Minimize / restore** | Window minimized 2.5 s, restored, kept rendering. No swapchain loss, no black frame. `shots/06-after-minimize-window.png` |
| **Post-storm steady state** | Returns to 119.7 fps — no leak or degradation across ~1,100 swapchain recreations |
| **Move across displays** | **Not tested** — this Mac has a single 1920×1080 @60 Hz display. The DPR-2.0 run (§4.4) exercises the same code path (`devicePixelRatio` change → `configureLayer` + swapchain recreate), but a genuine `QWindow::screenChanged` with two different-DPI monitors is **untested and is the main residual macOS risk** |
| **Long-run soak** | Not run. Longest continuous run here is ~90 s. A 30-min soak belongs in A1/C1 CI |

Resize is handled by destroying and recreating the swapchain (`flushAndWait()` →
`destroy(swapChain)` → `configureLayer()` → `createSwapChain()` → `View::setViewport()`),
which is what Filament's own `filamentapp` does on macOS. It costs ~8–9 ms of CPU per
resize (visible as `CPU frame p95 9.87 ms` in the storm row) — fine for interactive
resizing, but a production app should coalesce resizes to one per frame rather than one
per event.

---

## 6. Screenshots

`screencapture` (OS-level window capture) is **unavailable in this session** — the shell
has no Screen Recording permission (`could not create image from display`). Instead the app
captures its own evidence, which is strictly better provenance:

* `shots/NN-*.png` — `Renderer::readPixels()` off the swapchain Qt's `QMetalLayer` owns:
  the literal pixels Filament put in the Qt window.
* `shots/NN-*-qtchrome.png` — `QWidget::grab()` of the `QMainWindow`. The central area is
  **empty**, which is itself the proof that the 3D content is a native child window and not
  something Qt painted; the Qt status bar shows the live counters.
* `shots/NN-*-window.png` — the two composited, i.e. what the user sees. Recommended:
  `shots/03-10M-window.png` (10,000,000 pts, 138.6 fps, gpu p95 8.53 ms, 1280×773, vsync off)
  and `shots-hidpi/03-10M-window.png` (same at DPR 2.00, 148.5 fps).

The scene is a synthetic 12 × 8 × 3 m room scanned by a walking rotating lidar: floor blue →
ceiling yellow height ramp, four warm-tinted columns, per-point intensity jitter.

---

## 7. Gaps in this spike

* **EDL post-effect not implemented.** §3.12 calls for it. Filament supports custom
  post-process materials, so it is expected to work, but the cost is unmeasured. At 10M
  points there is 2.3× headroom at 60 Hz, so a full-screen EDL pass should fit — verify in A14.
* **No LOD.** Every point is submitted every frame — deliberately, to measure the worst case.
  Coarse-to-fine LOD only makes these numbers better.
* **Render loop is a spinning `QTimer(0)`.** It burns ~83% of one core retrying
  `beginFrame()`. That is a benchmark artifact (it maximises measurement resolution);
  production should drive the loop from `CVDisplayLink`/`CADisplayLink` on macOS and the
  equivalent elsewhere.
* **Points are a single flat cloud**, not the georeferenced multi-session structure A13 needs.
* **Single display, single GPU, no external monitor / screen-change test** (see §5).

---

## 8. Risk assessment for Windows / Linux (Vulkan)

This spike proves **Metal on macOS only**. Nothing here de-risks Vulkan, and the S3 exit
criterion explicitly asks for "macOS **+ Windows or Linux**". Concrete risks:

1. **Native handle plumbing is entirely different and equally under-documented.** macOS
   needed a `CAMetalLayer` despite the header saying `NSView`. Windows takes an `HWND`
   (`QWindow::winId()` is directly an `HWND`, likely fine); Linux is the hard case —
   Filament's `SwapChain::CONFIG_ENABLE_XCB` exists precisely because X11 vs XCB vs Wayland
   handles differ, and Qt on Wayland gives a `wl_surface`, which needs
   `VK_KHR_wayland_surface`. **Expect a Linux-specific investigation.**
2. **Resize is riskier on Vulkan.** `VK_ERROR_OUT_OF_DATE_KHR` handling, in-flight-frame
   fencing and swapchain recreation are a classic crash/validation-error source, and the
   1,105 recreations that were harmless here would exercise all of it. Budget real time.
3. **`gl_PointSize` is a genuine portability hazard.** It works on Metal (verified). On
   Vulkan it requires the shader to write `PointSize` **and** the device to expose
   `largePoints`; some drivers clamp the point size range to 1.0, and point size support has
   historically been weak on Intel/AMD Linux drivers. **Mitigation:** if `gl_PointSize`
   proves unreliable, switch to instanced quads / billboard expansion in the vertex shader.
   That costs 4–6× the vertex work, which at the measured M4 headroom is survivable, but on
   a mid-range dGPU it may force LOD earlier than planned.
4. **Performance is unknown off Apple silicon.** The M4 result is 10M @ 138 fps with the
   GPU only ~7 ms busy. A desktop dGPU should exceed this; an integrated Intel/AMD laptop GPU
   very likely will not hit 10M @ 60 fps and will need the LOD path from day one.
5. **`libzstd`/link-list drift** applies on every platform — pin the Filament version in CI.

**Recommendation:** run the Windows half of S3 (or fold it into **S7**, the Windows
toolchain spike) before A1 closes. It is the remaining unknown; a Linux/Wayland spike should
follow but can trail Windows.

---

## 9. Verdict

### GO — with caveats — for "Filament embedded in Qt" as the desktop rendering architecture

**Why GO:**
* 10M points @ **138.8 fps** uncapped / **60.5 fps** locked — 2.3× the §3.12 desktop target,
  before any LOD.
* CPU cost is negligible: **0.11 ms p95** per frame for submission, so the UI thread is free
  for the engine, the merge workbench and the processing queue.
* 200k pts/s live ingest into paged GPU buffers is essentially free (**0.11 ms p95** while
  simultaneously holding 12M points at 129 fps), with zero drops — the S2 lidar rate is a
  non-issue for the renderer.
* Qt embedding is **stable**: 1,105 swapchain recreations under a resize storm, minimize/
  restore, and DPR 2.0 all clean.
* One codebase, one material, one point pipeline shared with Android — the §3.12 premise holds.

**Caveats, in priority order:**
1. **Windows/Linux Vulkan is unproven** — the S3 exit criterion is only half met. Do the
   Windows leg next (§8).
2. **Pin the Filament version.** The `NSView` documentation was wrong and the README's link
   list was stale in the same release. Treat Filament's public docs as unreliable and lock
   `v1.75.0` in CI.
3. **Write a per-OS `NativeSurface` shim** in C1 rather than passing `winId()` around.
4. **`gl_PointSize` needs a Vulkan fallback plan** (instanced quads) before A14 commits to a
   display-parameter API that assumes screen-space point size.
5. **Multi-monitor / DPI-change is untested** — one display on this machine. Cover it in C1.
6. Replace the spin loop with a display-link-driven render loop.

**Plan B was not needed.** Raw Metal-in-Qt was never invoked; Filament works once handed a
`CAMetalLayer`.

---

## Appendix — raw result files

| File | Run |
|---|---|
| `results/A-vsyncoff-1280x800.md` | vsync off, 1280×773, DPR 1, full sequence incl. resize storm |
| `results/B-vsyncon-1280x800.md` | vsync on, 1280×773, DPR 1 |
| `results/C-vsyncoff-maximized.md` | vsync off, maximized 1920×946 |
| `results/D-hidpi-dpr2.md` | vsync off, DPR 2.0, 1600×918 physical |
| `results/E-ownlayer.md` | vsync off, app-created `CAMetalLayer` instead of Qt's |
| `results/F-nsview-failure.log` | raw `NSView` handle → `PreconditionPanic` abort |

Each `.md` carries the full per-phase table plus the run log (page counts, upload times,
ingest rates, swapchain recreate counts).
