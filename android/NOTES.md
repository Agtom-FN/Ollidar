# LidarScan Android — B1 scaffold notes

This is the Android app scaffold (Tech Spec §3.13, §4 workstream B, task B1).
Kotlin + Jetpack Compose, single-activity, Material 3, Navigation Compose,
ViewModel + StateFlow. Everything below is real, clickable Compose UI — no
lorem-ipsum or bare `TODO()` screens — backed by an on-disk `.lscan` project
store and a fake engine seam, per the B1 brief.

## Opening / building

```
cd android
./gradlew :core:test          # plain-JVM unit tests, no SDK/emulator needed
./gradlew :app:assembleDebug  # full app build, needs Android SDK (see below)
```

Or open `android/` as the project root in Android Studio (Ladybug+).

**Toolchain used to build this scaffold** (installed fresh into this
environment — nothing pre-existed):
- JDK 17 (`brew install openjdk@17`)
- Android SDK command-line tools (`brew install --cask android-commandlinetools`),
  with `platform-tools`, `platforms;android-36`, `build-tools;36.1.0` installed
  and licenses accepted via `sdkmanager`
- Gradle 8.14.5 via the wrapper (`./gradlew`, no separate Gradle install needed
  by a future user — `gradle/wrapper/gradle-wrapper.jar` is committed)

`local.properties` (git-ignored) points `sdk.dir` at wherever the SDK lives on
this machine; a fresh clone needs its own `local.properties` or `ANDROID_HOME`
set — Android Studio does this automatically on first open.

## Version pinning — read this before bumping AGP/AndroidX

The brief asked for "AGP current stable." The **true** current stable AGP at
the time of writing is **9.3.1**, but this scaffold pins **AGP 8.13.2**
instead, deliberately. Two real incompatibilities forced that:

1. **AGP 9.0+ removed the need for (and rejects) the
   `org.jetbrains.kotlin.android` Gradle plugin** — Kotlin support is now
   built into AGP itself, via a DSL (`kotl.in/gradle/agp-built-in-kotlin`)
   this scaffold was not ported to. Applying the classic
   `org.jetbrains.kotlin.android` plugin under AGP 9 is a hard error, not a
   warning.
2. **Gradle 9.6+ removed an internal API AGP 8.x depends on**, so AGP 8.x and
   Gradle 9.6+ can't be paired at all (confirmed by trying it — see the
   Gradle docs link in the error message,
   `upgrading_version_9.html#agp_8x_incompatible`).

Given (1) is an unfamiliar, newly-introduced DSL with no prior art to build
against confidently in the time available, and the task explicitly says not
to fabricate a build result, this scaffold took the reliable path: **Gradle
8.14.5 (the newest 8.x Gradle still paired with AGP 8.13.2 without warnings)
+ AGP 8.13.2 + the classic `kotlin-android`/`kotlin.plugin.compose` plugins**.
This combination is verified green (see "Build verification" below).

A second, related consequence: the very latest AndroidX/Compose releases
(e.g. `androidx.core:core-ktx:1.19.0`, Compose UI `1.12.0`, the `2026.08.00`
Compose BOM) have started declaring `compileSdk 37` / `AGP 9.1+` as a hard
requirement in their AAR metadata, which fails fast under AGP 8.13.2 with an
explicit, readable Gradle error (not a mystery failure). So the AndroidX
versions in `gradle/libs.versions.toml` are pinned a few releases back from
absolute-latest (see the comment block at the top of that file) — still a
modern, current stack (Compose Material 3, Kotlin 2.4.10, `compileSdk`/
`targetSdk` 36, the newest **stable** platform — 37 is canary-only right
now), just not the bleeding-edge-of-bleeding-edge patch releases that assume
AGP 9.

**Upgrade path for a later workstream**: bump `agp` to `9.3.1` in
`libs.versions.toml`, remove the `kotlin-android` plugin alias/application
from both modules' `build.gradle.kts`, follow AGP's built-in-Kotlin migration
doc, bump the Gradle wrapper back to 9.7.0, then the AndroidX versions can
move to true-latest again. Worth doing before B2+ land much more code, so the
migration stays small.

## Build verification (what actually ran, and its result)

Environment had **no** JDK, Android SDK, or Gradle installed at task start.
All three were installed fresh (see "Opening / building" above), well within
the ~20 minute tooling time-box.

```
$ ./gradlew clean :core:test :app:assembleDebug
...
BUILD SUCCESSFUL in 2s
45 actionable tasks: 21 executed, 24 from cache
```

- **`:core:test`** — 16/16 tests pass, 0 failures, 0 skipped, plain JVM (no
  emulator/Robolectric):
  - `FileProjectStoreTest` (9): create/list/open/delete, directory layout,
    name-collision handling, corrupt-manifest skip
  - `ProjectManifestSerializationTest` (3): full-field round-trip,
    required-only-field round-trip, forward-compat (unknown-field) decoding
  - `FakeEngineBridgeTest` (4): connect/capture state-machine transitions
- **`:app:assembleDebug`** — succeeds; produces
  `app/build/outputs/apk/debug/app-debug.apk` (~61 MB debug APK, unsigned
  release path untouched).
- **Not run**: no emulator/device was available in this environment, so the
  app was not launched or click-tested at runtime — only compiled and unit
  tested. Nothing in this environment claims otherwise.

## Module map

```
android/
  settings.gradle.kts, build.gradle.kts, gradle.properties
  gradle/libs.versions.toml          version catalog (all deps/plugins)
  core/                               plain Kotlin/JVM, NO Android/Compose deps
    model/   SensorType, WorkflowProfile, ProjectManifest
    store/   Project, ProjectStore (interface), FileProjectStore (impl)
    engine/  EngineBridge (interface), FakeEngineBridge, EngineBridgeProvider
    src/test/...                      the 16 JVM tests above
  app/                                 Android application module
    LidarScanApplication.kt           owns the AppContainer (DI-lite)
    MainActivity.kt                   single Activity, sets Compose content
    di/AppContainer.kt                manual composition root
    data/                             SettingsRepository (DataStore), Units/ThemeMode
    ui/theme/                         Color.kt (brand tokens), Theme.kt, Type.kt
    ui/nav/                           Routes.kt, LidarScanApp.kt (NavHost)
    ui/projects/                      Projects list screen + ViewModel
    ui/newproject/                    New-project flow screen + ViewModel
    ui/detail/                        Project detail screen + ViewModel
    ui/settings/                      Settings screen + ViewModel
    ui/common/                        SensorBadge/ProfileChip, date/point formatting
```

`:core` deliberately has **no** Android or Compose dependency (see its
`build.gradle.kts` — it's `kotlin("jvm")`, not an Android library module).
That's what makes `:core:test` runnable with zero SDK/emulator, and is a
constraint worth preserving as B2–B15 add engine-adjacent logic: if it needs
`Context`, it belongs in `:app`; if it's pure data/domain logic, it belongs
in `:core`.

## `.lscan` project store (Tech Spec §3.11)

`FileProjectStore` (in `:core`) creates project directories shaped exactly
like the spec's container diagram:

```
<slug>-<random6hex>.lscan/
  manifest.json
  streams/
  streams/frames/
  processed/
  merged/
  exports/
```

- The directory **basename is the project id** — no separate UUID to keep in
  sync with the filesystem. Two projects with the same display name get
  distinct directories (random 6-hex-char suffix), so renaming (not in B1
  scope) won't collide.
- `manifest.json` (kotlinx-serialization, pretty-printed) has exactly the
  fields B1 can populate — `schemaVersion`, `name`, `sensor`, `profile`,
  `createdAtEpochMillis`, `appVersion` — plus nullable
  `pointCountEstimate` / `mountCalibrationId` / `crsEpsg` reserved for
  B4/B6 (point count), B7 (mount calibration), and A10 (CRS) to fill in
  later without a breaking schema change. `ignoreUnknownKeys = true` on
  decode, so a future schema addition doesn't break B1's reader.
- Root directory: `context.getExternalFilesDir(null)/Projects` (app-specific
  external storage — no runtime permission needed, survives app updates,
  room for multi-GB captures), computed once in `AppContainer` and shown
  read-only on the Settings screen. A user-facing storage-location picker
  (SAF) is intentionally not in B1 — flagged as future work in the Settings
  screen's own copy.

## EngineBridge contract (what B2/B3/B4 build against)

`com.lidarscan.core.engine.EngineBridge` (in `:core`) is the seam between the
app and `libscanengine` (A1's C ABI, wired over JNI in B2/B3). It is
intentionally minimal — connect/capture/events, exactly what the B1 brief
asked for — not a prediction of the engine's full surface:

```kotlin
interface EngineBridge {
    val connectionState: StateFlow<ConnectionState>   // DISCONNECTED/CONNECTING/CONNECTED/ERROR
    val captureState: StateFlow<CaptureState>         // IDLE/RECORDING/PAUSED/STOPPING
    val events: Flow<EngineEvent>                     // StatusMessage / CaptureStats / Fault

    suspend fun connect(target: EngineTarget): Result<Unit>
    suspend fun disconnect()
    suspend fun startCapture(projectDirectory: String, liveSlam: Boolean): Result<Unit>
    suspend fun pauseCapture(): Result<Unit>
    suspend fun resumeCapture(): Result<Unit>
    suspend fun stopCapture(): Result<Unit>
}
```

`EngineTarget(sensor: SensorType, transportHint: String?)` — `transportHint`
is deliberately a free-form string (serial device path, IP, …) left for
B2/B3's connect flows to define.

`FakeEngineBridge` is a working in-memory implementation (simulated connect
delay, a fake point-rate ticker while "recording", proper state-machine
guards — e.g. `startCapture` fails cleanly if not connected). `Capture` on
the Project Detail screen currently just shows an "Arrives with B4" stub
panel, but wiring a real Capture screen against `FakeEngineBridge` today
(before any hardware/JNI exists) is exactly what B4 can do first.

`EngineBridgeProvider` is the "DI-lite" selector called out in the brief — a
manual, no-Hilt object with `get()` (lazily creates `FakeEngineBridge`) and
`override(bridge)` for B2/B3/A1 to swap in the real JNI-backed
implementation once it exists. Nothing downstream (ViewModels, `AppContainer`)
needs to change when that swap happens, since everything is coded against the
`EngineBridge` interface, not `FakeEngineBridge` directly.

## Where B2–B12 plug in

- **B2 (D6 connect) — done**, see the "B2 — D6 JNI bridge + connect flow"
  section above. `EngineBridgeProvider` now has a real `RealEngineBridge`
  registered (behind availability/dev-toggle checks) alongside `Fake`.
- **B3 (Mid-360 connect) — done**, see the "B3 — Mid-360 over USB-C Ethernet"
  section below. `ENGINE_WITH_LIVOX_SDK2` is now `ON`,
  `RealEngineBridge.connect` handles `SensorType.MID360`, and there is a
  Compose connect wizard with a pre-capture self-test.
- **B4 (Capture screen) — done**, see the "B4 — Filament capture screen +
  live 3D rendering" section below: live 3D view (Filament/`SurfaceView`),
  status strip, Live-SLAM toggle (now genuinely bound to
  `scan_session_config.live_slam`, ABI 2), pause/resume/stop with a session
  summary sheet, and the "Replay synthetic capture" debug acceptance path.
  `pointCountEstimate` on `ProjectManifest` is **still** not populated on
  capture stop (B4 didn't touch this either) — still worth wiring from
  `CaptureViewModel`'s final stats so the Projects list card stops showing
  "No capture yet"; flagging again for whichever task picks up B5/B6.
- **B5 (Profiles + Settings) — done**, see "B5 + B6 + B9 + B10 + B11 + B12 +
  D3" below. `CaptureDefaults` is the table the profile picker now writes into
  the manifest at creation; Settings surfaces it read-only, and the reason it
  is read-only is in §1 of that section.
- **B6 (Processing UI + Review) — done**. The two "Arrives with B6" stub cards
  are gone; every Project Detail card now navigates somewhere.
- **B7 (ARCore/mount calibration) + B8 (camera keyframes) — done**, see the
  "B7 + B8" section below. `mountCalibrationId` is populated and a full
  `mountCalibration` record now sits beside it; `ProjectStore` gained
  `updateManifest`, which is also what B5/B6 need for `pointCountEstimate`.
- **B9/A10 (RTK/CRS) — done**. `crsEpsg` is populated at capture stop, along
  with a full `georef` record (A10 §9.6's requested snapshot), which is what
  makes B12 possible at all.
- **B10/B11 (display params, measure) — done**. `Units` stayed in `:app`;
  `:core`'s measure code carries its own `MeasureUnit` rather than making
  `:core` depend on the UI layer's enum, and the two are converted at the
  ViewModel boundary.
- **B12 (merge) — done**, and it does write into the `merged/` subdirectory
  `FileProjectStore` has created since B1.

## B2 — D6 JNI bridge + connect flow

Task B2 (Tech Spec §3.1 Android row, §3.13, workstream B). Ownership was
strictly `android/**` — `engine/` was read-only (integration #24 was editing
`engine/capi/` concurrently; other agents were touching `engine/src/gnss/`
live, see "A live engine/ bug hit and self-resolved" below). Pinned against
`SCAN_ABI_VERSION` 1 as observed in `engine/capi/scanengine_c.h` at task
start; `scanengine_jni.cpp`'s `nativeCreateEngine` re-checks this at runtime
and refuses to proceed on a mismatch rather than risk mis-marshalling a
struct whose layout moved.

### 1. Build integration: Gradle `externalNativeBuild` (not the engine's own CMake preset)

`android/app/src/main/cpp/CMakeLists.txt` is a **new, small CMake project**
that `add_subdirectory()`s the real `engine/CMakeLists.txt` (unmodified) and
adds one more target, `scanengine_jni` (the JNI shim, SHARED). Wired into
Gradle via `app/build.gradle.kts`'s `externalNativeBuild { cmake { path =
file("src/main/cpp/CMakeLists.txt") } }` — i.e. **AGP drives CMake itself**,
not `engine/CMakePresets.json`'s `android-arm64` preset. Why:

- That preset assumes a vcpkg toolchain (`VCPKG_ROOT`) to cross-compile
  Eigen for `arm64-android`. Bootstrapping vcpkg's Android triplet here would
  have been slow and unnecessary: `engine/CMakeLists.txt` already falls back
  to `FetchContent` for Eigen3 when no vcpkg toolchain is chainloaded (its
  `ENGINE_FETCH_EIGEN` path), and AGP's own NDK toolchain is exactly what a
  real Android Studio user gets automatically — no `ANDROID_NDK_HOME`/
  `android.toolchain.cmake` wiring needed by hand.
- `ANDROID_PLATFORM`: the preset pins `android-24`; this build uses whatever
  AGP derives from the app's `minSdk` (29, already higher/stricter).
- **`externalNativeBuild.cmake.targets = ["scanengine_jni"]` is required**,
  not optional — without it, ninja builds *every* buildable target in the
  configured project, including Eigen's own `FetchContent`'d
  `eigen_blas`/`eigen_lapack` demo libraries and its
  `spsolver`/`spbenchsolver`/`test_sparseLU` targets (Eigen adds these
  unconditionally; `BUILD_TESTING=OFF` doesn't gate them). None of that is
  an actual dependency of `scanengine_jni` — scoping the target list to just
  ours skips it all. (First discovered as a real build failure: without the
  scope, `eigen_lapack`'s complex-SVD instantiation failed to compile under
  the NDK's clang; once scoped, that code is never even touched.)

Toolchain versions used (fresh-installed in this environment, matching
existing pins elsewhere in the repo):

- **NDK r27d** (`sdkmanager` package `ndk;27.3.13750724`) — matches
  `.github/workflows/engine-ci.yml`'s `nttld/setup-ndk@v1` `ndk-version:
  r27d` pin exactly (confirmed: the installed package's own directory names
  itself `android-ndk-r27d`). Set via `android { ndkVersion =
  "27.3.13750724" }` in `app/build.gradle.kts` — AGP resolves it from the
  SDK's `ndk/<version>/` directory, no `ANDROID_NDK_HOME` needed for a
  Gradle build.
- **CMake 3.31.6** (`sdkmanager` package `cmake;3.31.6`), the version pinned
  in `externalNativeBuild.cmake.version`.
- **arm64-v8a only** — `defaultConfig.ndk.abiFilters += "arm64-v8a"` (Tech
  Spec §3's reference hardware, Pixel 7+/Galaxy S22+, is all arm64).
  `armeabi-v7a`/`x86_64` would need their own `libscanengine` cross-compiles
  and are out of this task's scope.
- `ANDROID_STL=c++_shared` (passed as a CMake argument) — `libc++_shared.so`
  ships alongside `libscanengine_jni.so` in the APK.
- `engine/`'s own build flags are forced from the shim's `CMakeLists.txt`:
  `ENGINE_BUILD_TESTS=OFF`, `ENGINE_BUILD_TOOLS=OFF` (irrelevant to an app
  build), **`ENGINE_WITH_LIVOX_SDK2=OFF`** — B2 is D6-only;
  `third_party/Livox-SDK2` happens to already be checked out in this
  workspace (fetched for A3/B3) but building it here would pull in Mid-360
  UDP code this task doesn't need and hasn't verified cross-compiles for
  Android. **B3 flips this back ON** when it wires Mid-360.

A live `engine/` bug was hit and self-resolved mid-task: a build attempt
failed on `engine/src/gnss/ntrip_client.cpp:19` (`using
gnss_net::scan_socket_t_unused_guard_t = void;` — invalid C++ syntax,
confirmed with a bare `clang++ -fsyntax-only`, i.e. broken on every
platform, not Android-specific). The file's mtime was 6 seconds old when
first hit — a concurrent agent (A10/GNSS territory, outside B2's ownership
either way) was mid-edit. Per the ownership rule, B2 did not touch it;
instead the build was retried once the file stopped matching that broken
placeholder text (a few seconds later), which succeeded. No `engine/` file
was modified by this task.

### 2. JNI shim surface (`scanengine_jni.cpp` binding `engine/capi/scanengine_c.h`)

All exposed as `external fun`s on the Kotlin singleton
`com.lidarscan.app.engine.ScanEngineNative`:

- **Engine lifecycle**: `nativeCreateEngine`/`nativeDestroyEngine` (checks
  `scan_engine_abi_version()` against the shim's pinned `SCAN_ABI_VERSION`
  first), `nativeStartSession`/`nativeStopSession`/`nativeEngineState`.
- **Devices**: `nativeAddD6Device` (routes `scan_device_config.serial_write`
  back into a Kotlin `SerialWriter` functional interface — the engine's D6
  start/stop command bytes cross JNI *out* to `D6SerialConnection.write`),
  `nativeRemoveDevice`, `nativeDeviceHealth` (marshals `scan_device_health`
  field-for-field into a Kotlin `NativeDeviceHealth` via a cached JNI
  constructor).
- **`nativePushSerialBytes`**: takes a `java.nio.ByteBuffer` that must be
  direct (`ByteBuffer.allocateDirect`); the native side calls
  `GetDirectBufferAddress` and hands the engine that raw pointer —
  zero-copy *across the JNI boundary*. (`D6SerialConnection`'s reader thread
  still does one `arraycopy` from usb-serial-for-android's `byte[]`-based
  synchronous read API into the reused direct buffer first — that
  library has no direct-buffer read, so this one copy is unavoidable on the
  Android-USB side; the "zero-copy" claim is specifically about the native
  call not pinning/copying a `byte[]`.)
- **Event pump**: `nativeStartEventPump`/`nativeStopEventPump` spawn/join a
  dedicated `std::thread` per engine handle that loops
  `scan_engine_wait_event` (200ms timeout) and delivers each event to a
  Kotlin `EngineEventListener.onEvent(...)` callback — JNI-attaches that
  thread once at start, detaches once at stop, exactly as
  `scanengine_c.h`'s threading contract asks. `serial_write` callbacks,
  separately, run synchronously on whatever thread called into the engine
  (never the pump thread), reusing whatever `JNIEnv` is already attached
  there.
- **Errors**: `nativeLastError`/`nativeErrorStr` passthrough.

### 3. `RealEngineBridge` + provider wiring

`com.lidarscan.app.engine.RealEngineBridge` implements `:core`'s
`EngineBridge` over `ScanEngineNative`. `AppContainer` picks it (over
`FakeEngineBridge`) when `ScanEngineNative.isAvailable` (i.e.
`System.loadLibrary("scanengine_jni")` succeeded) **and** neither
`BuildConfig.FORCE_FAKE_ENGINE` (a build-time default, false today) nor the
persisted Settings → "Use simulated engine" dev toggle says otherwise. The
dev toggle is read once, synchronously, at `AppContainer` construction
(a single blocking DataStore read at app startup) rather than live-swapped —
`engineBridge` is handed out as a `val` to every ViewModel via
`container.engineBridge`, and those references would go stale if the
instance were swapped later, so the Settings screen says the switch "takes
effect after restarting the app" rather than lying about live effect.
JVM tests never construct an `AppContainer` at all, so `:core:test` /
`FakeEngineBridgeTest` / `D6ConnectControllerTest` exercise
`FakeEngineBridge` directly regardless of any of this.

`EngineBridge` (`:core`) gained one addition beyond B1's original shape: a
`deviceHealth: StateFlow<DeviceHealth?>` property (`DeviceHealth` mirrors
`scan_device_health` field-for-field). `FakeEngineBridge` synthesizes
plausible values while "recording" so the health panel has something to
show without hardware. `RealEngineBridge` populates it via a plain coroutine
ticker calling `nativeDeviceHealth` every 500ms — deliberately **not** an
event-driven push, see the C-ABI gap below.

`pauseCapture`/`resumeCapture` have no `scan_engine_*` equivalent (the ABI
only has start/stop) — implemented one layer down instead:
`D6SerialConnection`'s reader thread keeps the USB port open but stops
forwarding bytes into `push_serial_bytes` while paused, so the `.lscan`
session simply receives nothing; no ABI gap, no ABI change needed.

### 4. D6 connect flow

- **Dependency**: `com.github.mik3y:usb-serial-for-android:3.9.0` via
  JitPack (`settings.gradle.kts` adds a scoped `maven { url =
  "https://jitpack.io"; content { includeGroup("com.github.mik3y") } }` —
  narrow, not a blanket "any group from JitPack" addition, since
  `FAIL_ON_PROJECT_REPOS` funnels every module's resolution through this
  one `dependencyResolutionManagement` block).
- **Attach/permission**: `AndroidManifest.xml` adds
  `android.hardware.usb.host` (required) and a `USB_DEVICE_ATTACHED`
  intent-filter + `res/xml/usb_device_filter.xml` (CH340 VID `0x1A86`,
  common clone PIDs `0x7523`/`0x5523`) for the cold/background-launch case;
  `MainActivity` additionally registers a dynamic `BroadcastReceiver` for
  attach/detach for the app-already-foregrounded case, forwarding into
  `AppContainer.usbAttachEvents` (a `SharedFlow` `ConnectWizardViewModel`
  collects to re-scan). Runtime permission goes through
  `D6UsbConnectionRegistry.requestPermission` (`UsbManager.requestPermission`
  + a `PendingIntent`/`BroadcastReceiver` pair, wrapped in
  `suspendCancellableCoroutine`).
- **State machine**: `com.lidarscan.core.engine.D6ConnectController` (`:core`,
  plain Kotlin, no Android/`UsbManager` dependency) — `NoDevice →
  AwaitingPermission → Connecting → Connected`/`Failed`, driving
  `EngineBridge.connect`/`disconnect`. This split (Android USB plumbing in
  `:app`'s `ConnectWizardViewModel`, state machine in `:core`) is what makes
  it JVM-testable against `FakeEngineBridge` — see "Tests" below.
- **Wizard screen**: `com.lidarscan.app.ui.connect.ConnectWizardScreen` —
  lists CH340/serial drivers (`UsbSerialProber.getDefaultProber()
  .findAllDrivers(usbManager)`), a Connect button per device, and once
  connected a health panel showing everything `scan_device_health` exposes:
  state (via `ScanEngineNative.DeviceState.label`), points/sec, rotation Hz,
  checksum pass rate, packets ok/bad, bytes in, last error.
- **Reader**: `D6SerialConnection` opens the port at **230400 8N1, DTR
  cleared** (`port.setParameters(230_400, DATABITS_8, STOPBITS_1,
  PARITY_NONE); port.setDTR(false)`), then a daemon reader thread loops
  `port.read()` into a reused direct `ByteBuffer`, forwarding to
  `push_serial_bytes` (see zero-copy note above).
- **Capture wiring**: `ui/detail/ProjectDetailScreen.kt`'s Capture card is
  no longer a `StubNavCard` — it navigates to a real
  `com.lidarscan.app.ui.capture.CaptureScreen` (`project/{projectId}/capture`)
  that shows connection state, start/pause/resume/stop against the active
  `EngineBridge`, and live numeric stats: points captured, points/sec
  (computed from consecutive `CaptureStats` event deltas), elapsed time, and
  recording size (a throttled `streams/` directory walk, once per stats
  tick — cheap at that cadence). Live 3D/AR point-cloud rendering is
  explicitly left as "Arrives with B4" in the screen's own copy — the
  Filament point pipeline that needs doesn't exist yet.

### 5. Tests + verification

**Verified in this environment** (`./gradlew clean :core:test
:app:assembleDebug`, fresh toolchain install — no JDK/SDK/NDK/CMake existed
at task start, same posture as B1's original scaffold):

- `:core:test` — **22/22 tests, 0 failures**: the 16 from B1 unchanged, plus
  6 new `D6ConnectControllerTest` cases (starts-with-no-device,
  found→awaiting-permission, permission-denied→failed,
  permission-granted→connected-through-the-real-`FakeEngineBridge`,
  device-lost-while-connected disconnects the bridge, retry-after-failure
  reconnects) — instrumentation-free, plain JVM, exercised against
  `FakeEngineBridge` exactly like B1's `FakeEngineBridgeTest`.
- `:app:assembleDebug` — **succeeds**, with the real native build (not
  skipped/stubbed): `libscanengine_jni.so` compiles `engine/`'s 33 `.cpp`
  files (SLAM/GNSS/export/record/etc., ~10k lines) plus the JNI shim under
  the NDK r27d toolchain, links, and packages.
- **APK contents verified with `unzip -l`**:
  `lib/arm64-v8a/libscanengine_jni.so` (1.38 MB) and `lib/arm64-v8a/
  libc++_shared.so` present; **no** `armeabi-v7a`/`x86_64` entries (confirms
  `abiFilters` took effect).
- **JNI symbol export verified** with `llvm-nm -D`: `JNI_OnLoad` and all 15
  `Java_com_lidarscan_app_engine_ScanEngineNative_native*` entry points are
  present in the `.so`'s dynamic symbol table, with names matching the
  Kotlin `external fun` declarations exactly (confirms the JNI naming
  convention lines up end-to-end, not just "it compiled").
- `file` on the extracted `.so` confirms `ELF 64-bit LSB shared object, ARM
  aarch64`.

**Explicitly NOT verified (device-deferred — no emulator/device available in
this environment, consistent with the Tech Spec's 2026-08-15
hardware-absent addendum)**:

- Runtime `System.loadLibrary` success, `JNI_OnLoad`'s `FindClass`/
  `GetMethodID` cache calls actually resolving (they reference exact class/
  method signatures — e.g. `NativeDeviceHealth`'s constructor signature
  `(IIIIJJJJJDDDJ)V` — that only a real classloader can confirm; a typo
  there would compile fine on both sides and only fail at `JNI_OnLoad`
  time).
- Any actual D6 hardware interaction: USB permission dialog, `UsbSerialPort`
  open/read/write against real CH340 bytes, `push_serial_bytes` decoding
  real packets, `scan_device_health` numbers under a live stream.
- The health-panel numbers, capture start/stop against a live session, and
  recording-size-on-disk growth — all only exercised against
  `FakeEngineBridge`'s synthetic values so far.

### 6. C-ABI gaps found (for the rebind list)

Found by reading `engine/capi/scanengine_c.h` and
`engine/capi/scanengine_c.cpp` against what B2 needed to build, without
modifying either:

1. **`scan_event`'s payload union has no case for `SCAN_EVENT_DEVICE_HEALTH`
   (21)**, nor `POSE_UPDATE`(40)/`GNSS_FIX`(50)/`JOB_PROGRESS`(60).
   `scanengine_c.cpp`'s `convert_event()` switch only has cases for
   `EVENTS_DROPPED`/`ENGINE_STATE`/`SESSION_STATE`/`DEVICE_STATE`/
   `POINTS_AVAILABLE`/`ROTATION`/`ERROR`; everything else falls through to
   the `default:` branch and travels as opaque `raw[64]` bytes (the file's
   own comment there names "pose, gnss, job" as pending A10/A15 work but
   doesn't mention device-health explicitly — it hits the same fallthrough
   for the same reason). Confirmed `EventType::kDeviceHealth` genuinely
   *can* be published today (only `mid360_driver.cpp` does,
   `ctx_.bus->publish(EventType::kDeviceHealth, ...)`) — its payload just
   doesn't survive the C ABI crossing intact. **Consequence for B2**:
   `scanengine_jni.cpp`'s event pump deliberately does not attempt to
   interpret `DEVICE_HEALTH` events (would mean guessing an unmirrored C++
   struct layout); device health is polled instead via
   `scan_engine_device_health()`, which is fully specified and is what this
   task uses. D6 itself doesn't publish `kDeviceHealth` anyway (per
   `engine/docs/A2-d6-driver.md` §3: D6 health is polled-only, "an app that
   never polls health will not observe a silent-stall transition"), so this
   gap didn't block B2, but it will block anything wanting a *pushed*
   health update (e.g. a lower-latency health panel, or Mid-360 in B3 which
   *does* publish these) until `convert_event()` grows a case.
2. **`scan_session_config` has no live-SLAM toggle.** `:core`'s
   `EngineBridge.startCapture(projectDirectory, liveSlam: Boolean)` — from
   B1 — has no ABI-level equivalent to bind to; the struct is just
   `lscan_dir`/`profile`/`record`. `RealEngineBridge.startCapture` records
   `liveSlam` only for its own status-message text today, doesn't pass it
   into `scan_engine_start`. A future ABI addition (or a documented
   profile-based convention) is needed before B4 can actually gate live
   SLAM through this path.
3. **No pause/resume in the C ABI** (`scan_engine_start`/`scan_engine_stop`
   only) — not necessarily a gap to fix, since B2 found a clean
   workaround entirely on the app side (see §3 above: the reader thread
   stops forwarding bytes while paused), but worth knowing before some
   later task reaches for a `scan_engine_pause` that doesn't exist.
4. **`scan_session_config.profile` is a bare `const char*` with no
   documented-nullable contract** (unlike `lscan_dir`, which the header
   explicitly says "may be NULL/empty = do not record") and no enum/
   constant list in the C ABI itself (the four values — `survey | floorplan
   | research | quickscan` — are only named in a header comment). B2 passes
   the literal string `"quickscan"` unconditionally, since `:core`'s
   `EngineBridge` interface (B1) has no project-profile parameter on
   `startCapture` to thread `ProjectManifest.profile`
   (`WorkflowProfile`) through yet — B4/B5 territory.

## B4 — Filament capture screen + live 3D rendering

Task B4 (Tech Spec §3.12/§3.13, workstream B). Ownership strictly
`android/**` — `engine/` read-only. Pinned against `SCAN_ABI_VERSION` **2**
as observed in `engine/capi/scanengine_c.h` at task start (B2 had pinned
against 1; the INT-24 ABI bump to 2 landed poses/pushbroom/mount-calibration
*and* `scan_session_config.live_slam`, closing one of B2's own documented
gaps — see "C ABI gaps found" above, item 2, and §4 below).

### 1. Renderer: Filament, version-pinned separately from desktop

`com.google.android.filament:filament-android` + `filament-utils-android`
(for `Manipulator`'s orbit-camera math only — `filament-utils-android`
transitively pulls in `gltfio-android`, which this app never calls; its
`.so` just rides along in the APK, ~3 MB, not worth fighting the AAR's own
dependency graph to strip). **Pinned to v1.71.5**, not desktop's v1.75.0:

- Maven Central's `com.google.android.filament:filament-android` group only
  publishes Android AARs up to **v1.71.5** as of this task (confirmed via
  Maven Central's search API — no 1.72+ Android release exists there yet).
  Desktop's v1.75.0 pin (`desktop/tools/fetch_filament.sh`) is a *native*
  release with no Android AAR counterpart to match.
- `matc` (the material compiler) is **version-matched to the AAR**, not
  reused from desktop's v1.75.0 binary: `.filamat`'s binary format is
  checked against the runtime engine version it loads into, so a
  version-mismatched `matc` risks either a load-time rejection or (worse) a
  silent miscompile. `android/scripts/fetch_filament_tools.sh` fetches the
  **v1.71.5** `filament-<version>-mac.tgz`/`-linux.tgz` host-tools release
  (same release family desktop's script uses, just the matching version) —
  see that script's header comment for the full reasoning.

**SurfaceView + `UiHelper`** is Filament's documented Android idiom (not a
Compose-native surface — Filament doesn't have one): `PointCloudRenderer`
(`app/src/main/kotlin/com/lidarscan/app/render/PointCloudRenderer.kt`) owns
one `Engine`/`Renderer`/`Scene`/`View`/`Camera`, attaches a `UiHelper` to a
plain `android.view.SurfaceView`, and drives rendering off a
`Choreographer.FrameCallback` — line-for-line the same pattern as Filament's
own `hello-triangle` Android sample. `PointCloudView.kt` wraps that in a
`@Composable` via `AndroidView`, so `CaptureScreen` uses it like any other
Compose element.

### 2. Point-cloud pipeline: paged `VertexBuffer`s fed from JNI page reads

Mirrors `desktop/src/render/PagedCloudRenderer.{h,cpp}` one layer down —
same page-per-`VertexBuffer` approach, same "poll every frame, never trust
an incremental event stream" reasoning (`page_store.h`'s "readers take no
lock" contract; `EventType::kPointsAvailable` drives *stats* only, never
GPU sync, exactly like desktop's own documented reasoning for why it
polls). Two differences from desktop, both simplifications made explicitly
for this task's time box, not correctness requirements:

- **No real frustum culling** — every page's `RenderableManager` builds with
  `.culling(false)`, always visible, instead of desktop's per-page AABB
  culling. Worth adding as follow-up (the plumbing already computes and
  sets each page's `Box` via `setAxisAlignedBoundingBox` every sync, so
  flipping `.culling(true)` is a small, tested-independently change) — left
  off because a stale/mis-set AABB silently hiding real points is a worse
  failure mode to ship un-verified than the modest overdraw cost at the
  point counts this task could actually test against (the bundled synthetic
  capture, tens of thousands of points, not the 1M-pt/page desktop proved
  at 10M+ points).
- **One shared identity `IndexBuffer`** sized to the largest page capacity
  seen (same reasoning as desktop: Filament requires an index buffer even
  for `PrimitiveType.POINTS`, so sharing one across pages avoids
  `4 bytes × capacity` per page).

**The minimal JNI added for page reads** (`scanengine_jni.cpp`, live capture
engine): `nativePageCount`/`nativePageIdAt`/`nativeGetPointPage`/
`nativeTotalPoints`, thin wrappers over the C ABI's existing
`scan_engine_page_count`/`page_id_at`/`get_point_page`/`total_points`
(already in `scanengine_c.h` — B2 didn't need them, B4 does). The one
interesting piece is `nativeGetPointPage`: it hands back a
`NativePointPage` (`app/src/main/kotlin/.../engine/NativePointPage.kt`)
wrapping a **direct `ByteBuffer` aliasing `scan_point_page.data` itself** —
zero-copy from the engine's page memory straight into
`VertexBuffer.setBufferAt(engine, 0, buffer, destOffsetInBytes, count)`,
the same "zero-copy across the JNI boundary" property B2's
`nativePushSerialBytes` already has in the write direction. Safe per
`scanengine_c.h`'s own contract ("stable for the page's lifetime") and
`page_store.h`'s ("a page allocates its full capacity once and never
reallocates").

`PointCloudSource` (`app/src/main/kotlin/.../render/PointCloudSource.kt`) is
the interface `PointCloudRenderer` polls — `LiveEngineCloudSource` (over the
functions above) and `ReplayEngineCloudSource` (§3 below) both implement it,
so the renderer and `CaptureScreen` around it never need to know which one
is driving the view.

### 3. Replay path: a second, standalone `scanengine::Engine`

The acceptance path ("Replay synthetic capture", a debug action on the
Settings screen's existing "Engine (developer)" card) needs
`scanengine::lscan::ReplaySource`, which takes a C++ `Engine&` —not
something the C ABI's opaque `scan_engine*` exposes (its wrapping
`EngineHandle` is a file-local implementation detail of `scanengine_c.cpp`,
out of B4's read-only `engine/` scope to reach into). So replay gets its own
`scanengine::Engine` instance, entirely separate from the live capture
engine B2's `RealEngineBridge` owns:

- **`android/app/src/main/cpp/replay_engine.{h,cpp}`** — a small C++ helper
  (`lidarscan_jni::ReplayEngine`) that creates a standalone `Engine`, adds
  one receive-only D6 device (`send_start_stop_commands=false`,
  `require_start_ack=false`, mirroring
  `desktop/src/app/ReplayController.cpp`'s own replay device config exactly),
  and runs `ReplaySource::run()` on a detached thread. Links `scanengine`
  statically (same static lib the C-ABI path already links) — allowed per
  the task brief.
- **`android/app/src/main/cpp/replay_jni.cpp`** — its JNI bindings, plus
  page-read functions that call `engine_->points()` (a real `PageStore&`)
  directly instead of going through `scan_engine_get_point_page`, since
  there is no C-ABI handle for this engine. Field-for-field, byte-for-byte
  the same `NativePointPage` marshalling as the live path.
- **`android/app/src/main/cpp/jni_shared.h`** — the two cached JNI
  classes/ctors (`NativeDeviceHealth`, `NativePointPage`) both
  `scanengine_jni.cpp` and `replay_jni.cpp` need, resolved once in
  `scanengine_jni.cpp`'s `JNI_OnLoad` (the only place a `.so` may define
  it) and exposed via `extern` so `replay_jni.cpp` doesn't re-resolve (and
  leak a second global-ref set for) the same classes.

**Documented gap, not worked around**: `ReplaySource::run()` is a single
blocking call with its own internal real-time pacing loop and no pause hook
or seek-and-resume primitive in its public API (`ReplayConfig` has no
start-offset field) — forking that loop would mean re-implementing engine/
pacing logic, out of this task's read-only scope. `ReplayEngineBridge`'s
`pauseCapture()`/`resumeCapture()` therefore fail cleanly
(`Result.failure`, with a message explaining why) instead of faking a
resume that would silently restart from t=0; `CaptureViewModel
.isReplaySession` is what lets `CaptureScreen` hide the Pause button for a
replay session rather than shipping a control that does nothing. Same
spirit as B2's own documented "no pause/resume in the C ABI" gap, one layer
further out.

**The bundled synthetic capture**: `assets/replay/synth.lscan/` (396 KB —
just `manifest.json` + `streams/lidar.bin`, copied from
`desktop/evidence/synth.lscan/`, itself produced by S1's `d6synth` tool).
Bundled as a plain asset rather than generated at build time — reusing a
capture another task already produced and verified (per S1's own
`REPORT.md`) is simpler and no less "real" than re-running `d6synth` from
this task, and keeps `android/`'s build from gaining a dependency on
`spikes/s1-d6-parser`'s CMake project. `SyntheticReplayAssets.kt` extracts
it to `context.filesDir` on first use (idempotent) since
`FileRecordReader::open()` needs a real filesystem directory, not an APK
asset stream. `ReplayEngineBridge` (`app/src/main/kotlin/.../engine/
ReplayEngineBridge.kt`) implements `:core`'s `EngineBridge` exactly like
`RealEngineBridge` does, so the **same** `CaptureScreen`/`CaptureViewModel`
drives a replay session live — no separate "replay UI." The debug action
(`SettingsViewModel.replaySyntheticCapture`) find-or-creates a real
"Synthetic Replay Demo" project via `ProjectStore` (visible in the Projects
list like any other — deliberate, keeps the path fully inspectable) and
navigates to a dedicated route (`Routes.REPLAY_CAPTURE`, same `CaptureRoute`
composable, `isReplay = true`) rather than a query-param variant of the
normal capture route.

### 4. Live-SLAM toggle: closes B2's own documented ABI gap

`scan_session_config.live_slam` exists as of `SCAN_ABI_VERSION` 2 (it did
not at B2's ABI-1 pin). `nativeStartSession` gained a `liveSlam: Boolean`
parameter wired straight into `cfg.live_slam`; `RealEngineBridge
.startCapture` now threads its `liveSlam` argument through for real instead
of only recording it for status text. The Capture screen's Live-SLAM /
Record-only toggle (a `Switch`, editable only while `CaptureState.IDLE`)
sits on `CaptureViewModel`'s own `liveSlam: StateFlow<Boolean>` so it
survives rotation.

### 5. Color modes — A14's uniform contract, ported field-for-field

`android/app/src/main/materials/points.mat` is a from-scratch port of
`desktop/materials/points.mat` (not a shared `#include` — `matc` has no
cross-module include, and the two now target different Filament versions,
so they're allowed to diverge if a future engine version needs a shader
change on one side only). Same parameter names/types/order as
`DisplayParamsUniforms` (`engine/include/scanengine/cloud/display_params.h`,
`engine/docs/A14-display.md` §4), bound individually via
`MaterialInstance.setParameter(name, ...)` from
`PointCloudRenderer.applyDynamicMaterialParams()` — the same "Filament
takes named parameters, not a raw UBO blob" binding style A14's own header
documents for C1 (Qt/Filament), just from Kotlin. **RGB/height/intensity**
implemented (the B4 brief's "at minimum"); `kTime`/`kFixQuality` degrade to
the RGB pass-through per A14's own documented rule (`PointVertex` carries
neither field) — same scope cut desktop's `points.mat` documents.

**The colormap LUT is reimplemented in Kotlin, not fetched via JNI** —
because there is nothing to fetch: `engine/capi/scanengine_c.h` has **no
mirror at all** of `display_params.h`'s API (confirmed by reading the
header end to end), so `colormap_lut()` never crosses the C ABI in either
direction. `android/core/src/main/kotlin/com/lidarscan/core/render/
Colormap.kt` (plain Kotlin, `:core`, so it's JVM-testable — see
`ColormapLutTest.kt`, 6 new test cases) ports `grayscale_raw`/
`spectrum_raw`/`thermal_raw` from `display_params.cpp` by formula, not
approximation, so the shader's height/intensity colouring agrees with
`evaluate_point_color()`'s ground truth by construction — A14's own stated
goal, carried one hop further because the JNI boundary has no route for the
LUT bytes themselves. `PointCloudRenderer` uploads
`ColormapLut.buildTextureRgba8()` as one 256×3 RGBA8 `Texture` at init,
sampled by row exactly like desktop's `points.mat` samples its own LUT
texture.

Point size: `PointSizeMode.FIXED_PIXELS` only (a `Slider`, 0.5–12 px,
`CaptureViewModel.pointSizePx`) — adaptive/world-space modes are in the
material (ported verbatim) and in `Colormap.kt`'s `PointSizeMode` enum, just
not wired to a UI control yet; flagged as B10 display-params-panel
follow-up, not a gap in the material contract itself.

### 6. Orbit + follow camera

**Orbit**: `filament-utils-android`'s `Manipulator` (`Mode.ORBIT`), fed
`grabBegin`/`grabUpdate`/`grabEnd` from a `SurfaceView` touch listener and
`scroll()` from a `ScaleGestureDetector` for pinch-zoom. Its target is
anchored at the session's local-frame origin `(0,0,0)` — `Manipulator` has
**no runtime retarget setter** (confirmed via `javap` against the actual
v1.71.5 AAR classes, not assumed — its target is fixed at `Builder.build()`
time), and a target that silently drifted to the growing point-cloud
centroid every frame would fight the user's own drag input anyway, so
anchoring at the fixed capture origin (`PointVertex`'s own coordinate
frame) is the more usable behaviour, not just the expedient one.

**Follow**: a simple chase cam — looks at the combined point-cloud bounds'
centroid from a fixed elevated-behind offset that scales with the cloud's
current span, recomputed every frame as pages grow. This is "follow the
data," not a device-pose-driven AR follow (no ARCore in B4's scope; that is
B7's mount-calibration work) — documented as such in
`PointCloudRenderer.updateCamera()`'s own comment so it isn't mistaken for
one.

### 7. Build integration: `compileMaterials` Gradle task

`android/app/build.gradle.kts` gains a `compileMaterials` task
(`fetchFilamentTools` -> runs `matc -a opengl -a vulkan -p mobile` over
every `.mat` in `src/main/materials/` -> `build/generated/materials/
assets/materials/*.filamat`), wired as an asset source dir and as a
dependency of every variant's `merge*Assets` task. `fetchFilamentTools`
only runs when `android/third_party/filament-tools-v1.71.5/filament/bin/
matc` doesn't already exist (network fetch, `android/.gitignore`d — same
"fetched on demand, not committed" pattern as `desktop/tools/
fetch_filament.sh` + `desktop/.gitignore`). Verified directly:
`matc -a opengl -a vulkan -p mobile -o points.filamat points.mat` exits 0
and `matinfo` confirms `Feature level: 1`, `Blending: masked`, all 20
parameters present with the right types.

### 8. Tests / verification

**Verified in this environment** (fresh `JAVA_HOME`/`sdkmanager` setup
needed — same posture as B1/B2; `./gradlew clean :core:test
:app:assembleDebug`):

- **`:core:test` — 28/28 tests, 0 failures**: the 22 from B1/B2 unchanged,
  plus 6 new `ColormapLutTest` cases (grayscale/spectrum/thermal endpoint
  values hand-computed from A14-display.md §3's formulas, thermal's
  near-monotonic-luminance property checked exhaustively over all 256
  entries, and the 256×3 texture layout).
- **`:app:assembleDebug` — succeeds**, with the native build (not
  skipped): `libscanengine_jni.so` now compiles `replay_engine.cpp` +
  `replay_jni.cpp` alongside B2's `scanengine_jni.cpp` and the same ~10k
  lines of `engine/`.
- **APK contents verified with `unzip -l`** (arm64-v8a only, confirming
  `abiFilters` still takes effect for the new Filament AARs' own native
  libs too): `lib/arm64-v8a/libscanengine_jni.so` (grew from B2's 1.38 MB to
  1.84 MB), `libfilament-jni.so` (3.0 MB), `libfilament-utils-jni.so`
  (0.55 MB), `libgltfio-jni.so` (3.1 MB, transitively pulled in, unused —
  see §1), `libc++_shared.so`; `assets/materials/points.filamat`
  (44.7 KB); `assets/replay/synth.lscan/manifest.json` +
  `assets/replay/synth.lscan/streams/lidar.bin` (398 KB). Debug APK grew
  from B2's ~61 MB to ~72 MB, almost entirely the three Filament `.so`s.
- **JNI symbol export verified with `llvm-nm -D`**: all 31
  `Java_com_lidarscan_app_engine_ScanEngineNative_native*` entry points
  present (15 from B2 unchanged + 4 live page-read + 12 replay), names
  matching the Kotlin `external fun` declarations exactly.
- A second, truly-independent `./gradlew clean :core:test :app:assembleDebug`
  run (not just incremental) also passed clean.

**Headless-emulator attempt — tried, did not finish in time**: per the
task's instruction to attempt this "within a reasonable time box" before
falling back to stating things honestly, this environment ran
`sdkmanager --install "platform-tools" "emulator"
"system-images;android-34;google_apis;arm64-v8a"`, then prepared an
`avdmanager create avd` + headless `emulator -no-window -gpu
swiftshader_indirect` + `adb wait-for-device`/boot-completed-poll/`adb
install`/`adb shell am start`/`adb exec-out screencap` script
(`scripts` were run from the scratchpad, not committed under `android/` —
they're a one-off verification aid, not part of the app). `platform-tools`
and `emulator` installed quickly; the `android-34/google_apis/arm64-v8a`
system image (~1.5–2 GB) was still downloading, at roughly 1.4 GB and
progressing slowly, when this task's implementation work concluded — this
environment's outbound network throughput was the bottleneck, not anything
about the AVD/emulator setup itself. **The emulator was never booted, no
APK was installed on it, and no screenshot was taken.** This is stated
plainly rather than fabricated: nothing below "APK contents verified with
unzip/nm" was exercised on an actual device or emulator.

**Explicitly NOT verified (device/emulator-deferred)**:

- **Runtime `System.loadLibrary`/JNI_OnLoad success** — the new
  `NativePointPage`/`NativeReplayStats` `FindClass`/`GetMethodID` cache
  calls (exact constructor signatures like
  `(IIIIJJFFFFFFLjava/nio/ByteBuffer;)V`) only a real classloader can
  confirm; a typo there compiles fine on both sides and only fails at
  `JNI_OnLoad` time — same class of risk B2's own NOTES flagged for
  `NativeDeviceHealth`'s constructor.
- **Filament actually initializing and rendering a frame** — `Filament
  .init()`/`Engine.create()`/`UiHelper.attachTo()`/the `Choreographer`
  frame loop compile against the real v1.71.5 AAR classes (confirmed via
  `javap` for the trickier APIs — `Manipulator`, `RenderableManager`,
  `VertexBuffer.setBufferAt`'s overloads — not just "it compiled"), but
  none of it has executed: whether `matc`'s mobile-tier `.filamat` actually
  loads and renders a point on a real Adreno/Mali GPU, whether the
  `SurfaceView`/`UiHelper` surface-lifecycle callbacks fire as expected, and
  whether the touch/pinch camera controls feel right are all unverified.
- **The replay path end-to-end at runtime** — `SyntheticReplayAssets`
  extracting the bundled `.lscan` from the APK's assets, `ReplayEngine`
  actually decoding the bundled D6 bytes through the real driver and
  populating `PageStore`, and the Capture screen rendering the result live —
  all compiled and reasoned through against the engine's documented
  contracts (§2/§3 above), none of it run.
- **Any live D6/hardware interaction** — unchanged from B2's own posture;
  this task added no hardware-facing code.
- **Landscape/portrait rotation, the session-summary sheet, and the
  device-health chip's colour thresholds** — implemented and read back
  for logical correctness, not seen rendered.

A future pass with a faster network path (or a pre-warmed SDK cache) should
finish this exact script in a few more minutes — nothing about the attempt
needs redoing, it just needs to finish downloading.

## B7 + B8 — ARCore, the AR overlay, the mount-calibration wizard, camera keyframes

Tasks B7 (Tech Spec §3.3/§3.5/§3.7/§3.13, S6 `spikes/s6-calibration/WIZARD.md`)
and B8 (§3.5's camera keyframes, A11's `frames.idx`), done together because
B8 depends on B7's session. Ownership strictly `android/**`; `engine/` stayed
read-only. Pinned against **`SCAN_ABI_VERSION` 3** as observed in
`engine/capi/scanengine_c.h` at task start — `scan_engine_push_pose`,
`scan_engine_set_mount_extrinsics`, the pushbroom calls and the whole
`scan_mount_calib_*` solver handle all exist at 3; `scan_engine_record_keyframe`
does **not** (see §4 below).

> **The ABI moved to 4 while this task was running**, and it landed exactly the
> two things §7 lists as gaps: `scan_engine_record_keyframe` + `scan_keyframe`,
> and `scan_clock_sweep_estimate` + `scan_clock_sweep_result`. This task's brief
> said in as many words not to depend on the first ("may land mid-task from a
> concurrent agent — do NOT depend on it; write keyframes via your own C++
> helper"), so it did not rebind, and the final build was re-run from a clean
> `.cxx` against the ABI-4 header and is green. Two consequences for whoever
> picks this up:
>
> * **`cpp/keyframe_writer.{h,cpp}` can now be replaced by a one-line
>   `scan_engine_record_keyframe()` call**, which would put keyframes through
>   the session's own recorder (shared flush policy, one open file handle) —
>   the §5 note on `KeyframeIndexWriter` vs `FileRecordWriter` explains what
>   changes. `KeyframeInput` is already a field-for-field mirror of
>   `scan_keyframe`, so the swap is mechanical. The host round-trip tool in
>   `android/tools/keyframe_roundtrip/` should be pointed at whichever writer
>   survives.
> * **WIZARD.md screen 3 (the clock sweep) is now implementable.** It is
>   currently an honest "not available in this build" screen and the saved
>   calibration leaves `clockOffsetNs` unset; `scan_clock_sweep_estimate()` is
>   what fills it in.
>
> Note also that the shim's ABI check is compile-time-pinned
> (`scanengine_jni.cpp` compares `scan_engine_abi_version()` against the
> `SCAN_ABI_VERSION` of the header it was built with), so it stays
> self-consistent across a bump — it catches a `.so` linked against a
> *different* engine build, which is what it is for.

### 1. ARCore session management

`com.google.ar:core:1.54.0` from **Google's Maven** (`google()` — it is not on
Maven Central). Checked before pinning: its AAR declares no
`aar-metadata.properties`, so it imposes no `minCompileSdk`/AGP floor and works
under this project's deliberately-held-back AGP 8.13.2 (see the version-pinning
section above).

- **`com.lidarscan.app.ar.ArAvailability` / `ArInstaller`** — the availability
  states as an enum with one place that owns the user-facing copy, plus the
  install dance. `ArInstaller` carries the `userRequestedInstall` latch ARCore
  requires: the first `requestInstall()` may launch Play's installer and pause
  the activity, and only the call *after* resuming reports whether it finished.
  Missing that second call is the classic ARCore integration bug (the app comes
  back and `new Session()` throws `UnavailableArcoreNotInstalledException`).
- **`CaptureArController`** — owns the one `Session` for the process. It owns
  **no thread**: it is driven from the `GLSurfaceView` render thread via
  `onFrame()`, because `Session.update()` must run on the thread owning the GL
  context the camera texture lives in. `AppContainer` holds a single instance;
  the Capture screen and the wizard both drive it, since ARCore permits one
  session per process and two screens each creating one is how you get
  `CameraNotAvailableException` when navigating between them.
- **Poses into the engine**: `scan_engine_push_pose` once per ARCore frame,
  from that same GL thread — explicitly safe per the header ("Safe from the AR
  thread while points are decoded on another"). No clock conversion anywhere:
  `Frame.getTimestamp()` is CLOCK_BOOTTIME, which *is* the engine's domain, so
  A4 installs a passthrough estimator for `kPoseAr` (A8 §3.5). Frames whose
  timestamp did not advance (which `LATEST_CAMERA_IMAGE` can hand out when the
  render thread outruns the camera) are filtered before the call rather than
  sent and counted as engine rejections — the engine *rejects* an out-of-order
  pose rather than corrupting every later interpolation.
- **Tracking state → quality**, in `poseQualityOf()`: `TRACKING` → `GOOD`;
  `PAUSED` with a recoverable reason (excessive motion / insufficient features
  / insufficient light) → `POOR`; everything else → `INVALID`. `tracking_lost`
  is pushed alongside, which is what makes §3.3's "points during ARCore
  tracking loss are flagged and excluded by default" work — implemented by
  telling the truth about the pose, not by dropping it app-side. `confidence`
  is passed as **-1** ("derive it from quality/tracking_lost"): ARCore reports
  a state and a failure reason, not a scalar, and manufacturing one here would
  be a number with no measurement behind it. `position_sigma_m` /
  `orientation_sigma_deg` are **stated assumptions, not measurements** (0.02 m
  / 0.5° while tracking, pessimistic while not), documented as such in the
  code — ARCore publishes no per-pose covariance.
- **Camera permission** is requested in-context by the two screens that need
  it (Capture, wizard), not at app start. The manifest marks both
  `android.hardware.camera.ar` and the `com.google.ar.core` meta-data
  **`optional`**, deliberately: the Mid-360 path, replay and review need no AR
  at all, and `required` would hide the app from every non-AR device.

### 2. AR overlay (§3.7) — two surfaces, and why not one

**Chosen: a `GLSurfaceView` drawing the ARCore camera image underneath a
translucent Filament `SurfaceView`.** B4's `PointCloudRenderer` gained a
`translucent` constructor flag (`UiHelper.setOpaque(false)` +
`setMediaOverlay(true)` + `View.BlendMode.TRANSLUCENT` + a zero-alpha
`ClearOptions` — all four are needed; miss one and you get points on black)
and a `CameraMode.AR` that drives the Filament camera from ARCore's own
projection and display-oriented pose via `setCustomProjection`/`setModelMatrix`.

The single-surface alternative (Filament `Texture` with `SAMPLER_EXTERNAL` +
`Stream` over an OES texture, as Filament's own ArCore sample does) was **not**
taken, for two concrete reasons rather than taste:

1. **`samplerExternal` is OpenGL-only in `matc`.** B4's `compileMaterials`
   task compiles every `.mat` with `-a opengl -a vulkan`; a `samplerExternal`
   material cannot emit a Vulkan variant. Taking that path means splitting the
   material pipeline per backend or pinning the whole app to
   `Engine.Backend.OPENGL` — a change to B4's renderer with no device here to
   validate it.
2. **The external texture must live in a context shared with Filament's own
   driver context**, which Filament creates internally on its own thread. That
   is exactly the kind of thing that works or does not work per device, and
   **no ARCore device was available to this task**.

The honest costs of the composition: two GL contexts and one extra full-screen
composite by SurfaceFlinger. The single-surface path is the right follow-up
*once a device exists to validate it on*, and nothing else has to change to
take it. Texture coordinates come from `Frame.transformCoordinates2d` re-run on
`hasDisplayGeometryChanged()`, never hardcoded — hardcoding is how a preview
ends up mirrored on one device and stretched on another.

The §3.7 toggle is a **structural** switch, not a flag: `CameraMode.AR` mounts
`ArOverlayView` (two surfaces), `ORBIT`/`FOLLOW` mount B4's `PointCloudView`
(one opaque surface) unchanged, so neither mode carries the other's setup. AR
is offered in the segmented control only when there is a session to drive it.

### 3. Mount-calibration wizard (S6 WIZARD.md's five screens)

Route `project/{id}/mount_calibration`, reached from a new Mount-calibration
card on Project Detail that shows the stored verdict headline. All five screens
are implemented: Prepare (target spec, the three rules, blocking preconditions,
a *measured square size* field because printers silently rescale PDFs, and —
for the D6 — the S6 verdict stated plainly, that this is a check on a bench
calibration and not a replacement for one), Capture (live chips + automatic
shutter + diversity wheel), Clock sweep, Verdict, Verify.

**The geometry is real and lives in `:core`** (plain Kotlin, JVM-testable, no
Android dependency), which is what made 53 new unit tests possible:

- `calib/CheckerboardDetector.kt` — **a real detector, not a stub**. Saddle
  response `Ixy² − Ixx·Iyy` from the image Hessian → NMS → lattice growth with
  a per-cell basis (so perspective is tracked as the grid walks) → X-junction
  verification of every node → an exact `cols x rows` size match → a
  whole-grid **projective-regularity** check (one homography must explain all
  48 corners) → subpixel refinement.

  Three of those steps exist because the naive version measurably failed on
  synthetic imagery, and each is documented at its site: the X-junction test
  because the outer ring of a printed board's squares is also a saddle-response
  peak (an 8x6 board presents as a 10x8 lattice without it); the multi-basis
  seed search because NMS can leave a spurious peak 9 px from a true corner
  while the real step is 30 px, and the shortest verified basis is then wrong;
  the projective check because one mis-snapped corner in an otherwise perfect
  grid is the worst failure mode — the grid still looks complete.

  **Verified envelope, stated exactly**: rendered boards (a ray-caster, so the
  ground truth is the *pose*, not a drawn quad) across the wizard's own
  prescribed sweep including ±60° roll, plus a hard oblique pose and a
  heavier blur+noise case. Worst corner error over the 8-pose sweep at the
  time of writing: **0.26 px**. **No real camera image has been through this
  code**, because no ARCore device was available — that is in the class's own
  doc comment, not only here.
- `calib/TargetPlane.kt` — the camera half of the observation: normalised DLT
  homography (null vector of `AᵀA` via a Jacobi eigen-decomposition in
  `LinAlg.kt`) → decomposition against K → plane `(n, d)` in the camera frame.
  A homography, not a full PnP, because the target is planar and A8 §4.1 wants
  the *plane*, not the pose. Degenerate inputs are rejected by a point-spread
  test **before** the fit, since a collinear set admits a family of
  homographies that reproject perfectly — no residual-based test can catch it.
  Measured against exactly-known poses: normal to <0.05°, `d` to <0.5 mm on
  noise-free correspondences; <0.6° and 10 mm with 0.5 px of corner noise.
- `calib/BoardSegmentation.kt` — the lidar half. The **bootstrap is stated
  openly**: the camera-measured plane is transformed into the lidar frame with
  the bracket's *CAD nominal*, returns are gated to ±20 cm around that
  prediction and a range window, then the model is re-fitted by RANSAC so the
  fit owes the prediction nothing but which points it looked at. This is what
  WIZARD.md's "board at least 0.5 m clear of anything behind it" rule is for.
  **Mid-360 fits a plane; the D6 fits a LINE** in its own z = 0 scan plane —
  fitting a plane to a 2-D scanner's returns is rank-deficient by construction
  and would "succeed" while meaning nothing. That is the same geometry behind
  S6's "2 constraints per pose instead of 3".
- `calib/PosePlan.kt` — the prescribed sweep (azimuth −38…+38, elevation
  −24…+26, **roll −60…+60**) as a low-discrepancy additive recurrence, so any
  *prefix* is well spread and "stop at 5 / 8 / 12" are all well-conditioned
  choices. Plus the diversity wheel, which reports the weakest axis so the
  reject screen can say "tilt the phone more between shots".
- `calib/PoseChecks.kt` — WIZARD.md screen 2's five checks and their failing
  copy, verbatim from the spec's table so it has exactly one implementation,
  plus the automatic shutter (all-green for the dwell; any red frame resets
  the ring to zero).
- `calib/MountCalibration.kt` — the persisted record, the gate bands, and the
  readout that shows the verdict **in millimetres at 3 m, never in pixels**.

**Where the numbers come from**: the solve is the engine's own
`scan_mount_calib_*` handle (create → add_observation per pose → solve from the
bracket's CAD nominal), so the split-half gate is A8's, not a reimplementation.
The verdict screen shows `sigma_rot`/`sigma_trans`/`condition` as *diagnostics*
with the reason they are never gated on printed beside them.

**Persistence** follows WIZARD.md §3's rule that *calibration belongs to the
bracket, not the project*: a device-level `FileMountCalibrationStore` keyed by
(phone model, bracket ID, lidar serial) next to the projects root, **and** a
full copy in the project's `manifest.json` (new `mountCalibration` field beside
B1's reserved `mountCalibrationId`) so a `.lscan` opened on a desktop that has
never seen this phone is still colorizable. `ProjectStore` gained
`updateManifest(id) { … }` for this — B1 shipped a create-only store, and every
later field (`pointCountEstimate`, `crsEpsg`) needs the same thing, so it went
on the interface rather than staying a B7-local helper. It writes via a temp
file + rename: a truncated manifest makes the whole project unreadable, which
is far worse than losing one edit.

At capture start the stored extrinsic is applied with
`scan_engine_set_mount_extrinsics` + `pushbroom_enable`, in that order (the
engine returns `SCAN_ERR_INVALID_STATE` for the second without the first —
which is precisely why the wizard has to run before a D6 capture), after an
app-side `Mat4.isRigid()` check so a bad matrix is named rather than relayed as
a bare `SCAN_ERR_INVALID_ARGUMENT`.

### 4. B8 — camera keyframes into `frames.idx`

**No CameraX and no second camera client.** §3.5 says "CameraX shared with
ARCore session"; on ARCore that means either a shared-camera session or
`Frame.acquireCameraImage()`. This uses the latter: it is the same image ARCore
tracked with, so its timestamp, pose and intrinsics are the ones ARCore
actually computed — no second stream to correlate and no second clock, which
matters when 83% of S6's budget is time-sync. The honest cost: the CPU image is
the session's *image* resolution, not a full-resolution still. That is adequate
for sampling a colour per point and is what the 2–5 fps cadence assumes; a
higher-resolution path is a shared-camera follow-up and a **resolution**
decision, not an architecture one.

**What ARCore gives vs what needs Camera2 interop** (the B8 brief asked for
this distinction explicitly; the full table is in `ArCameraMetadata.kt`):

| field | source |
| --- | --- |
| `fx, fy, cx, cy`, `width`, `height` | ARCore `Camera.getImageIntrinsics()` |
| `t_engine_ns` (exposure of row 0) | ARCore `Frame.getTimestamp()` |
| pose | ARCore `Camera.getPose()` |
| `exposure_ns`, `iso`, AE-lock | ARCore `Frame.getImageMetadata()` |
| **`row_time_ns`** | ARCore `ImageMetadata.SENSOR_ROLLING_SHUTTER_SKEW` / (height − 1) |
| timestamp-source sanity check | **Camera2** `SENSOR_INFO_TIMESTAMP_SOURCE` |
| `distortion` | **nobody** — deliberately zero, see below |

The rolling-shutter row time is the interesting one: it is 6.8 px of S6's
20.2 px budget and it is the field a naive implementation writes as 0. It needs
**no** Camera2 interop — `com.google.ar.core.ImageMetadata` re-exposes the
`CaptureResult` tags including `SENSOR_ROLLING_SHUTTER_SKEW` (verified by
`javap` against the 1.54.0 AAR, not assumed). What genuinely needs Camera2
interop is only the *static* per-camera characteristic
`SENSOR_INFO_TIMESTAMP_SOURCE`, which is checked once per session and **logged
loudly** if it is not `REALTIME`: that is the single assumption the whole clock
domain rests on, and if it is wrong it silently mis-times every keyframe and
every pushbroom point. A device that reports no skew gets `row_time_ns = 0`
*and* a `rollingShutterKnown = false` flag surfaced in the capture UI, because
the format encodes 0 as "global shutter" and a phone never is one.

`distortion` is written all-zero **deliberately**: ARCore's intrinsics describe
an already-rectified image. `LENS_RADIAL_DISTORTION` exists but describes the
*raw sensor*, and attaching it to rectified intrinsics would double-correct.

**Cadence and motion gate** live in `:core`'s `KeyframeSelector` (unit-tested):
3 fps inside §3.5's 2–5 fps band, refuse anything over S6's 15 °/s, and within
each slot take the **slowest** frame rather than the first. A slot where
nothing qualifies produces no keyframe. Angular rate and linear speed come from
`RigMotionTracker`'s centred finite differences over the ARCore pose stream
(±100 ms window — one 33 ms frame pair is dominated by ARCore's own pose
jitter) and are recorded per keyframe, which is exactly what lets a desktop
re-apply the gate from a `.lscan` alone (A11 §3.3 item 4).

**Threading**: the gate + a plane-stride-correct NV21 copy run on the GL
thread, and the `Image` is closed immediately (ARCore's reader pool is small
and a held image stalls tracking); JPEG encode, the file write and the JNI
index append run on a single-threaded encoder executor — which also gives the
native writer the one-thread-at-a-time contract it documents. A record refused
by `validate_keyframe()` deletes its own JPEG rather than leaving an orphan
image no index points at.

### 5. JNI additions (15 new entry points, all in `cpp/arcore_jni.cpp`)

`nativePushPose`, `nativePoseGateAt`, `nativeSetMountExtrinsics`,
`nativePushbroomEnable/Flush/Stats`, `nativeMountCalibCreate/Destroy/
AddObservation/Solve`, and `nativeKeyframeWriterOpen/Add/Records/Flush/Close`,
plus two new marshalling classes (`NativeMountCalibResult`,
`NativePushbroomStats`) cached in the existing `JNI_OnLoad`.

**Row-major vs column-major is stated at every boundary**, because it is the
documented field failure: everything crossing to the engine (`double[16]`) is
ROW-major and the engine rejects a column-major matrix outright; everything
crossing to Filament and coming from ARCore is COLUMN-major.
`Mat4.fromColumnMajor()` is the single conversion point, and
`Mat4.isRigid()` refuses a transposed matrix (there is a test that a
transposed rigid transform fails it — an orthonormality-only check would pass
a mirror).

**`keyframe_writer.{h,cpp}` — the one place this task needed C++.** A11 §8.2
asks for `scan_engine_record_keyframe()` in the C ABI; **it does not exist at
`SCAN_ABI_VERSION` 3** (checked) and `engine/` is read-only here, so the shim
links the engine's C++ `color/` module directly — the same pattern B4's
`replay_engine.{h,cpp}` established for `lscan::ReplaySource`.

It wraps **`color::KeyframeIndexWriter`**, not a second `lscan::FileRecordWriter`,
and the reason is worth recording: A11 recommends the recorder "because it is
already open", but it is *not* open to us (it lives inside the opaque
`scan_engine*`), and opening a second `FileRecordWriter` on the same `.lscan`
would rewrite `manifest.json` — a file already owned twice over (by the
engine's recorder and by `:core`'s `FileProjectStore`, with a different
schema). `KeyframeIndexWriter` is A11's own byte-identical standalone writer,
creates only `streams/frames/` + (lazily) `frames.idx`, and cannot collide:
nothing in the engine publishes `StreamId::kCameraFrames` today.

### 6. Verification — what was actually run

**`:core:test` — 81/81 pass, 0 failures** (28 from B1/B2/B4, **53 new**), plain
JVM, no emulator:
- `CheckerboardDetectorTest` (7): fronto-parallel, the full prescribed sweep
  including roll, a hard oblique pose, blur+noise, an empty scene returning
  null, a wrong-size grid returning null, and subpixel refinement actually
  moving corners off the integer grid.
- `TargetPlaneEstimatorTest` (6): plane recovery against exactly-known poses,
  graceful degradation under corner noise, `d > 0` and board-in-front,
  incidence/distance, degenerate-input refusal, self-reprojection.
- `CalibrationPipelineTest` (27): rigid-transform round-trip, the
  column-major rejection, ARCore-matrix conversion, quaternion sign
  insensitivity, Mid-360 plane segmentation rejecting the wall behind the
  board, D6 line segmentation, empty-segmentation honesty, the JNI float
  layout, the pose plan's ranges and prefix spread, the diversity wheel
  naming a neglected axis, all five live checks (each failing for its own
  reason), roll wraparound, the shutter's continuous-dwell rule, the gate
  bands, and the device store (round-trip, key replacement, no-serial
  fallback, corrupt-store recovery).
- `RigMotionTest` (10): commanded rate/speed recovery, stationary, unknown vs
  zero, tracking-loss invalidation, out-of-order rejection matching the
  engine's, ring bound, the 2–5 fps cadence, the 15 °/s refusal,
  slowest-in-slot, no-tracking refusal, and the gate predicate.
- `FileProjectStoreTest` +3: `updateManifest` persisting a mount calibration
  across a re-read, missing-project null, and no stray temp file.

**`android/tools/keyframe_roundtrip` — a host tool that actually ran, and
passed.** It compiles the *same* `keyframe_writer.cpp` the `.so` ships,
links the same `scanengine` static library, writes 12 synthetic keyframes into
a temp `.lscan`, and reads them back through the engine's own
`color::read_frame_index()`:

```
$ cmake --build <build> --target keyframe_roundtrip && ./keyframe_roundtrip
  ok   open() succeeds
  ok   frames.idx is NOT created by open() (lazy, per A11 §3.1)
  ok   records() == N
  ok   a non-unit quaternion is REFUSED at add()
  ok   an absolute image name is REFUSED at add()
  ok   a '..' image name is REFUSED at add()
  ok   read_frame_index(): ok
  ok   read back 12 of 12
  ok   FrameIndexStats is clean (no truncated/CRC/malformed/rejected/out-of-order)
  ok   [0] … [11] every field: t_mono_ns, exposure, pose, intrinsics,
       rolling-shutter row time, sigmas, quality, flags, motion, iso,
       image_bytes, and the image-name compose/decompose round trip
  info frames.idx is 2348 bytes for 12 records
PASS: 0 failure(s)
```

That covers the whole writer path minus JNI marshalling, and it is what the
B8 brief's "write N synthetic keyframes → read the `.lscan` back" asked for. An
instrumentation test could have been written but never run here, and an unrun
test is not verification.

**`:app:assembleDebug` — succeeds**, native build included. APK inspected:
- `lib/arm64-v8a/`: `libscanengine_jni.so` (3.1 MB, up from B4's 1.84 MB),
  **`libarcore_sdk_c.so` + `libarcore_sdk_jni.so`**, `libfilament-jni.so`,
  `libfilament-utils-jni.so`, `libgltfio-jni.so`, `libc++_shared.so` — arm64
  only, so `abiFilters` still holds for the new ARCore AAR too. Debug APK grew
  from ~72 MB to ~74.7 MB.
- `assets/materials/points.filamat` unchanged (no new material was needed —
  see §2 on why the camera background is not a Filament material).
- **All 46 `Java_com_lidarscan_app_engine_ScanEngineNative_native*` symbols
  exported** (`llvm-nm -D`), 31 from B2/B4 plus the 15 new ones, names matching
  the Kotlin `external fun` declarations exactly.

**New this task — JNI *signatures* checked mechanically, not just names.**
B2 and B4 both flagged "a typo in a constructor descriptor compiles on both
sides and only fails at `JNI_OnLoad`". `javap -s` against the compiled Kotlin
classes confirms:

```
NativeMountCalibResult  ([DZZIIJJDDDDIDDD)V   == the descriptor JNI_OnLoad looks up
NativePushbroomStats    (JJJJJJJJJJJJJ)V      == ditto
nativePushPose          (JJDDDDDDDFFIZF)I     == arcore_jni.cpp's parameter list
nativeMountCalibAddObservation (JDDDD[FD)I    == ditto
nativeKeyframeWriterAdd (JJJDDDDDDDFFFF[FIIFFFIZIIFFFILjava/lang/String;)Ljava/lang/String;
```

That closes the largest class of "compiles fine, dies at load" risk for the new
surface. What it still does not prove is that `FindClass` resolves at runtime
(it needs a real classloader) — see below.

**Explicitly NOT verified — device-deferred (no ARCore device or emulator was
available; consistent with the Tech Spec's 2026-08-15 hardware-absent
addendum):**

- **Everything visual.** The AR overlay has never been rendered: whether the
  translucent Filament surface actually composites over the `GLSurfaceView` on
  a real Adreno/Mali driver, whether `setZOrderMediaOverlay` puts it in front,
  whether the ARCore projection lands points on the right pixels, and whether
  the two surfaces stay in sync under rotation.
- **Any ARCore runtime behaviour**: session creation, the install flow, the
  permission dialog, `Session.update()` on the GL thread, tracking-state
  transitions, and whether `Frame.getImageMetadata()` on a given device
  actually reports `SENSOR_ROLLING_SHUTTER_SKEW` (the API exists; whether a
  particular phone populates it is a per-device fact).
- **The checkerboard detector on a real camera image.** Its accuracy numbers
  above are synthetic. Real images add lens distortion in the corners, motion
  blur, rolling-shutter skew of the board itself, uneven lighting and print
  artefacts. The detector may need its blur sigma and NMS radius adapted to
  the observed corner spacing; that is a bench task, and the wizard's live
  checks plus the split-half gate are what keep a worse-than-expected detector
  from producing a confidently wrong calibration rather than a rejected one.
- **A complete calibration solve end to end** — no lidar returns have gone
  through `BoardSegmenter` from real hardware, so the CAD-nominal bootstrap's
  ±20 cm gate is unvalidated against a real bracket (and no real bracket
  exists: `BracketNominals` are stated placeholders).
- **`JNI_OnLoad`'s `FindClass` calls** resolving at runtime — the descriptors
  are now verified (above), the class *lookups* still are not.
- **The keyframe pipeline against a real camera**: NV21 plane-stride handling
  across devices, `YuvImage.compressToJpeg` throughput at 3 fps, and whether
  the encoder executor keeps up without backing up the GL thread.

### 7. C-ABI gaps found (for the rebind list)

1. **`scan_engine_record_keyframe()` was missing at ABI 3 — and landed in ABI
   4 mid-task** (see the note at the top of this section). A11 §8.2 names it
   as the first thing B8 needs ("it is what makes B8 a capture task rather
   than a format task"). Worked around by linking `color::` directly (§5) per
   this task's brief; the workaround is sound but means the keyframe writer is
   *not* the session's recorder, so its flush policy is
   `KeyframeIndexWriter`'s rather than the recorder's shared one. **Rebinding
   to the ABI-4 call is the first follow-up.**
2. **`scan_clock_sweep_estimate()` was missing at ABI 3 — also landed in ABI
   4.** WIZARD.md screen 3 (the 8-second sweep S6 calls "the highest-value 8
   seconds in the wizard") is therefore implemented as an honest "not
   available in this build" screen, and the saved calibration leaves
   `clockOffsetNs` **unset** rather than writing a zero as if it had been
   measured. That screen is now unblocked.
3. **No colorizer in the C ABI** (`scan_colorizer_*`, also A11 §8.2), so
   nothing on Android can yet *use* the keyframes it records. B6/B8's
   processing path will need it.
4. **`scan_mount_calib` is add-only** — no way to remove an observation. The
   wizard's "drop the worst 2 poses and re-solve" (WIZARD.md screen 4's third
   diagnosis) is implemented by destroying the handle and replaying the kept
   observations, which is equivalent and needs no ABI change; noting it in
   case a future caller expects removal.
5. **`scan_pose` carries no covariance input beyond two scalars**
   (`position_sigma_m`, `orientation_sigma_deg`), which is fine — but ARCore
   supplies neither, so those two fields are app-side assumptions on this
   platform. Worth stating wherever they are consumed.

### 8. Follow-ups this task deliberately did not take

- **Single-surface AR** (Filament external texture), once a device can
  validate it — see §2.
- **Higher-resolution keyframes** via ARCore shared-camera mode — see §4.
- **`pointCountEstimate` on capture stop** is *still* unwired (flagged by B4
  and B2 before it). B7 added `ProjectStore.updateManifest`, which is the
  missing piece; wiring it is now a two-line change in `CaptureViewModel`.
- **A target ghost outline** on screen 2 shows the prescribed roll and azimuth
  as text rather than a perspective outline of where the board should sit: the
  outline needs the board's intended pose in the *current* camera frame, which
  is only known once the board has been seen. What is drawn is the part that
  is both known and actionable.
- **Re-calibration prompts** (WIZARD.md §3: bracket changed, phone re-seated,
  calibration older than an interval, user reports misalignment) — the stored
  record carries everything needed (timestamp, bracket, serial, gate), and the
  Project Detail card already surfaces the verdict, but nothing prompts yet.

## B3 — Mid-360 over USB-C Ethernet

Task B3 (Tech Spec §3.1's Android row: "USB-C Ethernet; `ConnectivityManager`
`TRANSPORT_ETHERNET` + `Network.bindSocket`; static-IP wizard + per-OEM
guidance + pre-capture self-test"). Ownership strictly `android/**`; `engine/`
stayed read-only. Pinned against **`SCAN_ABI_VERSION` 4** as observed in
`engine/capi/scanengine_c.h` at task start.

**Read `engine/docs/A3-mid360-driver.md` before this section.** Nearly every
decision below is downstream of one of its measured findings, and the ones
that matter most are: explicit `lidar_ip` **and** `host_ip` are mandatory
because the device is *told* where to stream and never discovers its host; a
link drop is invisible to the packet counter (0 counted losses across three
15-second cable pulls), so silence is the only honest outage signal; and a
power-cycled device is never re-configured by the SDK, which is why the driver
forces a full re-init rather than waiting for a self-heal.

### 1. SDK2 on bionic: it compiles clean, and the overlay is empty

`android/app/src/main/cpp/CMakeLists.txt` now sets
`ENGINE_WITH_LIVOX_SDK2 = "ON"` (B2 had it `OFF`). `ON`, not `AUTO`,
deliberately: `AUTO` silently degrades to a build with no SDK2 backend when
the tree is missing, and the user finds out on a bench when `start()` returns
`kNotSupported`. `ON` is a configure-time `FATAL_ERROR` naming the fetch
script, which is the right failure for a path that is not optional.

It is a **`STRING`** cache entry, not a `BOOL` — the engine declares it as a
tri-state (`AUTO`/`ON`/`OFF`) and compares it with `STREQUAL "AUTO"`, so
forcing it to `BOOL` from the shim would hand that comparison a type it never
expects.

**The bionic question, answered before anything was wired.** Rather than
discover SDK2's portability through Gradle, the SDK's own CMake project was
configured directly against the NDK toolchain and the one target the engine
links was built:

```
$ cmake -S engine/third_party/Livox-SDK2 -B <tmp> -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ cmake --build <tmp> --target livox_lidar_sdk_static
...
17 warnings generated.
[37/37] Linking CXX static library sdk_core/liblivox_lidar_sdk_static.a
```

**37/37 objects, zero errors, no Android-specific source change needed.**
`android/third_party/patches-android/` therefore contains only a README
recording that result and the rules any future patch has to follow.

Two of the *engine's* three existing patches turn out to be load-bearing on
Android even though neither was written for it, and this is worth knowing
before anyone "cleans them up":

* **`0001-cmake-minimum-required-3.10.patch`** — the NDK ships CMake 3.31.6,
  which refuses `cmake_minimum_required(VERSION 3.0)` exactly as CMake ≥ 4.0
  does. Without it the Android configure fails the same way the macOS one did.
* **`0002-no-werror-on-clang.patch`** — this is the one that makes bionic
  work. The NDK's compiler identifies as `Clang` (not `AppleClang`), so it
  hits the same `-Werror` line, and it raises **17** warnings here. Most are
  rapidjson's malformed `RAPIDJSON_DIAG_OFF` pragma strings
  (`-Wunknown-warning-option`), but one is **NDK-libc++-specific and does not
  fire on Apple's libc++ at all**: the bundled fmt instantiates
  `std::char_traits<fmt::char8_t>`, which the NDK's libc++ marks
  `_LIBCPP_DEPRECATED_`. Had patch 0002 not already existed, a bionic-only
  `-Werror` patch would have had to be written for exactly that one warning.
* `0003-darwin-no-broadcast-bind.patch` is inert here by construction — its
  guard is `#if defined(WIN32) || defined(__APPLE__)`, so Android keeps
  upstream's Linux path and still creates the broadcast-bound detection
  sockets. That is correct: Linux (and therefore Android) accepts `bind()` to
  `255.255.255.255` (`RTN_BROADCAST` is an allowed local-address class), which
  is the whole reason the Darwin patch was needed in the first place.

**The overlay's one hard rule, if a patch ever does land there**: it must be
guarded by `#ifdef __ANDROID__` / `if(ANDROID)`. The overlay is applied **in
place** to `engine/third_party/Livox-SDK2`, which is the same gitignored,
script-generated tree the engine's macOS/Linux/CI builds configure against.
Mutating a generated tree is not "editing `engine/`", but it *is* shared, and
an unguarded change would silently alter a build this task does not own.

### 2. Build integration: a Gradle task, because CMake is too late

`ENGINE_WITH_LIVOX_SDK2=ON` needs the tree to exist **at configure time**, so
the fetch cannot live in CMake — by the time the configure step runs, it has
already failed. `app/build.gradle.kts` gains `prepareLivoxSdk2`, wired as a
dependency of every task whose name starts with `configureCMake` or
`buildCMake` (AGP names them `configureCMakeDebug[arm64-v8a]` — the bracketed
ABI is part of the task name, so matching by prefix is what keeps a release
build or a second ABI from silently missing the dependency).

It shells out to `android/scripts/prepare_livox_sdk2.sh`, which:

1. runs the **engine's own** `engine/third_party/fetch_sdk2.sh` unmodified
   (pinned tarball + its three patches; a no-op when the tree is present, and
   it honours `LIVOX_SDK2_TARBALL` for an air-gapped build), then
2. applies `android/third_party/patches-android/*.patch` idempotently — each
   patch is skipped when `patch -R --dry-run` succeeds, i.e. when it is
   already in the tree, so a re-run never half-applies.

That idempotency idiom was verified directly (apply → detected-as-applied →
skipped) rather than assumed, since the overlay itself is currently empty and
the mechanism would otherwise be untested code.

Same "fetched on demand, not committed" shape as B4's `fetchFilamentTools`.

### 3. Two runtime problems bionic creates that compiling clean does not fix

**a) There is nowhere for SDK2's config file to go.** `LivoxLidarSdkInit()`
takes a config-file *path*, so `mid360_sdk2.cpp`'s `write_config()`
synthesises one:

```cpp
std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
if (ec) dir = std::filesystem::path(".");
```

On Android **neither branch is writable**. libc++'s `temp_directory_path()`
consults `TMPDIR`/`TMP`/`TEMP`/`TEMPDIR` and falls back to `/tmp`; an Android
device has none of those set for an app and no `/tmp` directory at all, so
`ec` is set — and the engine's fallback of `"."` is the process CWD, which for
an app is `/`. The generated config would fail to write and the Mid-360 would
never start, reporting `kFileError` against a path that makes no sense.

Fixed app-side with one `setenv("TMPDIR", context.cacheDir, 1)`
(`ScanEngineNative.nativeSetTempDir`, called once from `AppContainer`'s `init`
so there is exactly one call site and no ordering question). This needs **no
ABI change**, fixes every other `temp_directory_path()` caller in the engine
at the same time, and `setenv` is the mechanism libc++ documents for it. The
cleaner long-term fix is engine-side and is listed in §8.

**b) `socket(2)` itself is permission-gated.** On Android the syscall fails
with `EACCES` unless the app is in the `inet` supplementary group, which it
joins only by holding `android.permission.INTERNET`. This affects **both** the
sockets this app creates and the ones the Livox SDK creates inside native
code, and reported as a bare "EACCES" it reads like a firewall problem and is
not. `AndroidManifest.xml` now declares `INTERNET`,`ACCESS_NETWORK_STATE` and
`CHANGE_NETWORK_STATE`, and `NetworkBoundUdpSocket.explainErrno` turns that
particular errno into the sentence that names the real fix.

### 4. Ethernet transport: `EthernetMonitor`, and why it also registers a plain callback

`app/net/EthernetMonitor.kt` builds a
`NetworkRequest(TRANSPORT_ETHERNET, NOT_VPN)` and tries
`ConnectivityManager.requestNetwork` **first**, falling back to
`registerNetworkCallback` on `SecurityException`.

That fallback is not defensive padding — `requestNetwork` is guarded by
`CHANGE_NETWORK_STATE`, whose protection level is
`signature|preinstalled|appop|pre23`. A normal app does **not** get it by
declaring it, and on many builds the call throws.
`registerNetworkCallback` takes the same `NetworkRequest`, needs only
`ACCESS_NETWORK_STATE`, is granted at install, and delivers identical
callbacks; it simply does not *ask for* the network, which for a physically
attached USB-Ethernet adapter is not something that needs asking. Which path
is live is surfaced in the UI (`EthernetState.usingRequest`), because it
changes what "no Ethernet network" means.

`NET_CAPABILITY_INTERNET` is deliberately **not** in the request. A direct
phone-to-lidar link has no internet and never will; requiring it is the
easiest way to make this callback never fire on exactly the setup it exists
for.

The monitor reads `LinkProperties.linkAddresses`, filters to IPv4 (`UdpConfig`
is dotted-quad throughout and SDK2's config JSON has no IPv6 form), and
exposes them — which is what makes the wizard's most valuable check possible
(§6). "Adapter up, no IPv4 address" is called out as its own state, because it
is precisely where a direct lidar link sits until a static IP is configured:
there is no DHCP server on the other end of the cable.

**`bindProcessToNetwork` is deliberately not used.** It is process-wide and
would push NTRIP corrections (A10/B9), Play services and everything else onto
a link with no route to the internet.

### 5. Passing a pre-bound fd — and the C-ABI wall it runs into

`engine/include/scanengine/transport/udp_source.h` documents
`UdpConfig::prebound_fd` as, in its own words, "The Android seam: the app
binds the socket to the USB-Ethernet Network object (ConnectivityManager
TRANSPORT_ETHERNET + Network.bindSocket) and hands the bound descriptor down,
because the engine cannot reach ConnectivityManager."

**The C ABI cannot express it.** `scan_device_config`'s Mid-360 half is
exactly two fields:

```c
  /* Mid-360 (UDP) — A3 */
  const char* lidar_ip;
  const char* host_ip;
```

and `scanengine_c.cpp` copies them into `dc.mid360.udp.{lidar_ip,host_ip}` and
nothing else. So the backend selector, every port, `recv_buffer_bytes`, the
point filter, `live_points_per_sec` and `prebound_fd` are all unreachable from
the C ABI — and there is no route from the opaque `scan_engine*` to the C++
`scanengine::Engine&` it wraps either (`EngineHandle` is file-local to
`scanengine_c.cpp`). B4 hit exactly this wall for `lscan::ReplaySource`.

So B3 does what B4 did, with the same reasoning, and splits the two jobs:

* **Capture goes through the C ABI.** `ScanEngineNative.nativeAddMid360Device`
  → `scan_engine_add_device(kind = SCAN_DEVICE_MID360)` on **the same**
  `scan_engine*` `RealEngineBridge` already owns, so Mid-360 points land in
  the session's `PageStore`, its raw datagrams reach the session's `.lscan`
  recorder (the engine installs its own `raw_sink` shim for that —
  `engine.cpp`'s "Record-always for a driver that owns its own sockets"), and
  live SLAM sees them. Backend is SDK2 by default, which is the only one that
  can bring an out-of-the-box device up.
* **The wizard's checks go through a standalone C++ engine.**
  `cpp/mid360_probe.{h,cpp}` builds its own `scanengine::Engine` and a full
  `Mid360Config`, which is the only place `prebound_fd`, the backend selector
  and the ports can be set at all.

**How the descriptor actually crosses**, in `app/net/NetworkBoundUdpSocket.kt`:
`android.system.Os.socket()` → `setsockopt(SO_REUSEADDR)` +
`setsockopt(SO_RCVBUF, 4 MB)` → `Os.bind(hostIp, port)` →
`Network.bindSocket(FileDescriptor)` →
`ParcelFileDescriptor.dup(fd).detachFd()` → an `int` handed to JNI.

* `Os` rather than `DatagramSocket` because `Network.bindSocket` would take
  either, but there is no public way to get an **int** out of a
  `DatagramSocket` — the usual route is reflection into `DatagramSocketImpl`,
  which is greylisted and has moved between releases. This path uses no
  reflection.
* Binding the **specific** host address rather than `INADDR_ANY` is
  deliberate: it fails immediately with `EADDRNOTAVAIL` if that address is not
  on the device, which is the commonest Mid-360 misconfiguration and otherwise
  surfaces eight seconds later as "no packet".
* `SO_RCVBUF` is set here because a pre-bound socket **bypasses the engine's
  own sizing** — `UdpSource` only sets `SO_RCVBUF` on a socket it created
  itself. A3 §8 names NIC/buffer behaviour as the single biggest untested
  risk, so silently shipping a default-sized buffer on this path would be a
  bad trade.
* Ownership is stated at every step: this class owns the original fd and
  closes it; the **dup** is owned by native, because `UdpSource` never closes
  a pre-bound descriptor ("the app owns it"), so `Mid360Probe::stop()` closes
  it after tearing the engine down (order matters — no receive thread may
  still be in `recvfrom` on it).

**`Mid360Probe` also reports what `DeviceHealth` cannot.** `Engine` exposes no
concrete-driver accessor, so `Mid360Stats` — link state, watchdog trips,
forced re-inits, window loss %, device SN — is unreachable even from C++
(desktop's NOTES §8.3 records the same constraint). Two things close the gap
without an engine change:

* the probe installs its own **`raw_sink`**, which fires per datagram *before*
  parsing. That is the only signal available when bytes arrive but nothing
  decodes, and "no datagrams at all" vs "datagrams but no points" is the
  difference between a cabling/addressing fault and a port/format one;
* **link state is re-derived** from wall-clock silence using A3 §5's own
  thresholds (`data_timeout_ms` 1 s → `kSilent`, `reinit_after_silence_ms`
  5 s → `kReinitializing`) applied to `t_last_data_ns` — the same observable
  and the same rule the driver applies internally, so the two do not tell the
  user different stories. Wall clock and not `udp_cnt`, because S2 measured
  **0 counted losses across three 15-second cable pulls**.

### 6. The connect wizard (Compose)

`ui/connect/Mid360ConnectScreen.kt` + `Mid360ConnectViewModel.kt`, reachable
from Project Detail (per-project, can save) and from the D6 connect wizard
(no project, transport check only). Five sections, ordered the way an operator
hits the problems:

**Interface status.** Adapter present/absent, interface name, the IPv4
addresses actually on it, and a one-tap "use this as the host IP". "Up but no
address" is its own message.

**Static-IP guidance, per OEM, with a Settings deep link.** It is *guidance*
and not a form because **there is no public API to configure an Ethernet
interface's IP**: `EthernetManager`/`StaticIpConfiguration` are `@SystemApi`
behind `MANAGE_ETHERNET_NETWORKS`, which is `signature`-level with no
user-grantable equivalent (`WifiManager` has a public counterpart; Ethernet
does not). `StaticIpGuidance` carries per-vendor menu paths and a per-vendor
*caveat* for Pixel/AOSP, Samsung One UI, Xiaomi HyperOS/MIUI,
OnePlus/OPPO/realme and HONOR/Huawei — including the ones that matter most:
several MagicOS/EMUI builds ship **no Ethernet settings UI at all** (a
DHCP-capable switch is the workaround), and some One UI builds drop static
settings when the adapter is unplugged. The deep link goes through
`resolveActivity` so an unresolvable action degrades to the generic Settings
screen instead of throwing.

**Per-OEM variance is stated as unverified.** The intents are documented
`Settings.ACTION_*` constants; the *menu paths* come from vendor
documentation and have **not** been walked on a device of each brand — no
device was available. The on-screen copy says so, and says that the addresses
read back from the live interface are the ground truth.

**Addresses.** Lidar IP and host IP (defaults `192.168.1.100` / `192.168.1.5`,
with the host pre-filled from the interface's real address when there is one),
the three device-side ports, and the transport selector. Validation lives in
`:core` (`net/Mid360Settings.kt`) and is **deliberately stricter than desktop
C2**, which checks only that the lidar IP is non-empty and lets `add_device`
be the validator. That is a defensible desktop trade; it is a poor one here,
because the Mid-360's characteristic failure is *silence*, and an 8-second
self-test that ends in "no packet" is a far worse diagnosis than a field that
says the host IP is not an address this phone holds. The checks:

| check | verdict | why |
| --- | --- | --- |
| both IPs present | fatal, with different reasons | one is "we cannot find it", the other "it cannot find us" |
| dotted-quad, hand-rolled | fatal | `InetAddress.getByName` does a **DNS lookup** for non-literals, on whatever thread; the wizard validates per keystroke. Leading zeros are refused because `inet_addr` reads `010` as octal 8 |
| host IP ∈ interface addresses | **fatal** | the one misconfiguration that produces no error at all |
| …but only when addresses are known | warning | do not block form-filling before the cable is in |
| loopback host | fatal | the engine's sim uses `127.000.000.001` to slip past SDK2's self-IP filter; A3 §7 says not to copy that into production |
| host ≠ lidar | fatal | |
| same /24 | warning | legal with a router, impossible on a direct cable |
| port ranges, host-port collisions | fatal | three host ports are three bound sockets |
| raw-UDP selected | note | states what it cannot do rather than failing later |

**Self-test.** `add_device` + `start` (preview session, nothing recorded) →
first-data-or-timeout, matching desktop C2's `onTestDevice()` shape. The gate
is **the first point, not a rate** — the same asymmetry C2 has, and for the
same reason: the D6's failure mode is a bad link that decodes *some* bytes, so
a rate discriminates; the Mid-360's is total silence, and once it streams it
streams at 200,000 pts/s, so there is no partial-credit regime. The window is
**8 s**, deliberately shorter than the engine's own 10 s `connect_timeout_ms`
(that timer decides whether the *watchdog* forces an SDK re-init; the UI's job
is to stop asking the user to wait once the answer is clear) and about 5× the
measured 1.45 s handshake.

A failure is diagnosed, not just reported — `Mid360SelfTest.diagnose` picks
between "datagrams ARE arriving but none decode" (wire and addressing fine,
port or format wrong), "the driver reported a fault" (usually host IP or a
bound port), and "no datagram at all", whose likelihood-ordered list ends with
the one an operator cannot deduce from the app: **the Mid-360 needs its own
9–27 V / ~6.5 W supply; USB-C cannot power it.**

The health readout shows A3's link state with its meaning spelled out
(including "a cable pull looks exactly like this" on `Silent`), plus pts/s,
IMU Hz, loss % **and** its complement, points, drops, and the pre-parse
datagram counters — against A3 §7's soak figures quoted as the reference. On
the pre-bound path IMU reads "n/a", never "0.00 Hz", because a structural
absence displayed as a zero reads as a broken device.

On a pass the device is **left streaming**, exactly as C2 does: the
interesting question (does loss % stay at zero over 30 s?) is answered after
the verdict, not before it.

**Save per project.** `ProjectManifest` gains a `mid360: Mid360Settings?`
field, written only after a pass — desktop C2's rule that a config which
failed to add is never persisted, for the same reason: saving addresses that
demonstrably do not work makes the next session's pre-fill worse than the
defaults. It lives in the manifest rather than a device-level store because,
unlike a mount calibration (which belongs to the *bracket* and follows the
phone), a lidar/host IP pair belongs to the **site** — and it is the record of
what a capture was actually taken with, which is the first thing anyone asks
when a `.lscan` turns out empty.

### 7. Capture integration

* `RealEngineBridge.connect` handles `SensorType.MID360` via a
  `"<lidarIp>|<hostIp>"` `transportHint`, creating the engine if needed and
  adding the device to the capture session's own handle.
* Project Detail shows a **Mid-360 connect** card for a Mid-360 project with
  the saved endpoint on it; the Capture screen's device card names the
  endpoint next to the connection state, and its button is "Set up" (→ wizard)
  or "Connect" (→ connect with the saved endpoint) depending on what the
  project has. The D6 wizard grew a door to the Mid-360 one rather than a tab
  — nothing on it (driver enumeration, USB permission, CH340 detection)
  applies to Ethernet.
* **Live-SLAM + Mid-360 compiles through and renders correctly — after a fix
  that is the opposite of the one the brief anticipated.** The brief asked to
  verify that stream filtering does not drop `kSlamMap` pages. It does not,
  because **B4's renderer had no stream filtering at all**: `syncPointCloud()`
  drew every page it enumerated, ignoring `scan_point_page.stream`. That is
  correct for a D6 record-only session (one point stream) and wrong the moment
  a Mid-360 runs with `live_slam = true`, when the engine's single `PageStore`
  holds both `SCAN_STREAM_LIDAR_MID360` (sensor-frame preview) and
  `SCAN_STREAM_SLAM_MAP` (A6's registered map, and also where A8's pushbroom
  cloud goes — INT24 §2). Drawing both superimposes a cloud that rotates with
  the sensor on one that does not; the symptom is smearing and doubling, not
  missing points, which is much harder to attribute.

  Fixed in `render/StreamFilter.kt` + `PointCloudRenderer.setStreamFilter`:
  `RAW_ONLY` when Record-only, `MAPPED_ONLY` when Live-SLAM, chosen from the
  same `liveSlam` flag that goes into `scan_session_config.live_slam`.
  `MAPPED_ONLY` **falls back to raw until the first mapped page exists**,
  because live SLAM takes a moment to initialise its ESKF and is allowed to
  fail without failing the session (INT24 §2) — a strict map-only filter would
  show a black screen in exactly the case where the operator most needs to see
  that points are arriving. The first mapped page drops the raw pages in the
  same pass, so the switch is one frame rather than a slow crossfade.
  `mappedSeen` is tracked on the renderer, not on the enum, because enum
  entries are process-wide singletons and would leak one session's state into
  the next.
* **Pause is not offered for a Mid-360, and that is a deliberate refusal.**
  B2's trick (the reader thread stops forwarding bytes) does not transfer: the
  Mid-360 owns its own sockets and the app never touches its bytes. Desktop C2
  pauses by stopping the recording session and resuming with a *new* one into
  the same directory — **not replicated**, because
  `FileRecordWriter::open()` creates its stream files with
  `std::fopen(path, "wb")`, so a resume would truncate everything recorded
  before the pause. Silently destroying the first half of a capture is far
  worse than not offering the button, so `pauseCapture` fails cleanly with the
  reason and `CaptureScreen` hides the control (same shape as B4's replay
  path). A real fix is an append/resume mode in the recorder, or pause/resume
  in the C ABI.

### 8. Engine-side findings (for the rebind list — none were worked around silently)

1. **`scan_device_config` cannot carry the Android seam.** Its Mid-360 half is
   `lidar_ip` + `host_ip` only, so `UdpConfig::prebound_fd` — the field
   `udp_source.h` documents *as* the Android seam — is unreachable from the C
   ABI, and so are the backend selector, all ten ports, `recv_buffer_bytes`,
   the filter and `live_points_per_sec`. B3 works around it with a standalone
   C++ engine for the wizard (§5), but **the capture session cannot use a
   pre-bound socket at all** without an ABI addition. This is the one worth
   fixing first.
2. **`UdpConfig::prebound_fd` is one fd, but the raw-UDP backend needs two.**
   `RawUdpBackend::open()` copies the whole `UdpConfig` into both its point
   and its IMU `UdpSource` (`mid360_raw_udp.cpp`), so a single pre-bound
   descriptor would be `recvfrom`'d by two receive threads that steal each
   other's datagrams. B3's pre-bound path therefore runs point-only
   (`publish_imu = false`) and says so in the UI. A per-source fd (or a small
   `prebound_fds[2]`) would close it. **Not a bionic incompatibility** — an
   API-shape gap, so it is reported rather than patched.
3. **SDK2's own sockets cannot be bound to a `Network` per-socket.** They are
   created inside `util::CreateSocket` in the vendored SDK, so
   `Network.bindSocket` never sees them, and the production bring-up path
   (kSdk2) relies on the Ethernet link being the route the kernel picks. The
   three ways out, none taken here: (a) `ConnectivityManager.bindProcessToNetwork`
   — works, including for native sockets, but is **process-wide** and would
   push NTRIP/Play/everything onto a link with no internet; (b)
   `android_setprocnetwork()` from `<android/multinetwork.h>` scoped around
   SDK init — same semantics, callable from the shim, same cost; (c) a fourth
   SDK patch calling `android_setsocknetwork()` inside `CreateSocket`, which
   is the only per-socket answer but is an Android *feature* patch rather than
   a bionic-compat one and so is outside this task's overlay rule. Worth
   deciding deliberately before a bench session.
4. **`Mid360Config::sdk_config_path` is not exposed through the C ABI**, which
   is what forced the `TMPDIR` fix-up (§3a). Exposing it — or having the
   engine prefer an app-supplied directory — would be cleaner than relying on
   an environment variable set at startup.
5. **`Engine` exposes no concrete-driver accessor**, so `Mid360Stats` (link
   state, watchdog trips, forced re-inits, window loss %, device SN/IP) is
   unreachable from the app even in C++. Desktop's NOTES §8.3 flags the same
   thing. B3 re-derives link state from `t_last_data_ns` using A3's own
   thresholds and counts datagrams through `raw_sink`, which covers the
   wizard's needs — but "how many forced re-inits has this capture had" is a
   question the app still cannot answer, and it is exactly the question a
   flaky bench session raises.
6. **B2's `SCAN_EVENT_DEVICE_HEALTH` gap still stands** and now bites: the
   Mid-360 *is* the driver that publishes `kDeviceHealth`, and its payload
   still does not survive `convert_event()`'s `default:` fallthrough. Health
   is therefore polled, not pushed. There is a second-order consequence worth
   knowing: the *published* payload carries the **per-window** loss
   (`1 - loss_pct_window/100`) while `device_health()` carries the
   **lifetime** figure, so the polled "loss %" will not snap back after a
   transient burst. Closing the event gap would also make the gauge
   responsive.

### 9. Verification — what was actually run

**Verified in this environment** (`./gradlew clean :core:test
:app:assembleDebug`, and a second run from a wiped `app/.cxx` + `app/build`):

- **`:core:test` — 104/104 pass, 0 failures** (81 from B1/B2/B4/B7/B8
  unchanged, **23 new**), plain JVM, no emulator:
  - `Ipv4Test` (4): dotted-quad parsing; rejection of the shapes a human
    actually types (`192.168.1`, `.256`, trailing dot, `1a`, and **leading
    zeros**, which `inet_addr` would read as octal); prefix-length subnet
    matching including the /32 edge; loopback/multicast/broadcast
    classification — plus an explicit assertion that the engine sim's
    `127.000.000.001` is *not* accepted here.
  - `Mid360SettingsValidationTest` (9): defaults validate against a matching
    interface; both IPs required **with distinct reasons**; a host IP the
    interface does not hold is fatal and names the addresses it does hold;
    unknown addresses degrade the check to a warning; loopback refused;
    host ≠ lidar; cross-subnet is a warning not an error; port range and
    host-port collision; the raw-UDP note is non-blocking and states both of
    its limitations.
  - `Mid360SelfTestTest` (10): one point passes at 1.45 s; the baseline is
    subtracted so a re-test on a streaming device is honest; progress across
    the window; **fails at exactly 8000 ms and not at 7999**; the window is
    locked shorter than the engine's 10 s grace; the three diagnoses are
    distinct and the silence one names the power requirement; a fault is
    named rather than blamed on the cable; the pre-bound path reports "IMU
    off" and never "0.00 Hz"; the health line quotes loss and its complement;
    the reference constants match A3 §7's soak table.
- **SDK2 compiled under the NDK before any integration** — 37/37 objects, 0
  errors (§1), which is the answer to "does it work on bionic".
- **`:app:assembleDebug` — succeeds with SDK2 genuinely compiled in**, not
  skipped. Evidence, on the *unstripped* `.so`
  (`app/build/intermediates/cxx/Debug/*/obj/arm64-v8a/libscanengine_jni.so`):
  - `llvm-nm -a` finds **4,610 symbols matching `livox`**, including
    `LivoxLidarSdkInit`, `LivoxLidarSdkStart`, `LivoxLidarSdkUninit` and
    `SetLivoxLidarPointCloudCallBack`;
  - the engine's own SDK2 backend is present:
    `scanengine::make_sdk2_backend(Mid360Driver&, unsigned, Mid360Config const&)`.
- **Size jump**: `lib/arm64-v8a/libscanengine_jni.so` grew from B7/B8's
  **3.1 MB to 5.84 MB** (+2.7 MB, the SDK2 static library and its vendored
  rapidjson/spdlog/FastCRC); the debug APK from ~74.7 MB to **77.6 MB**. Still
  arm64-v8a only — no `armeabi-v7a`/`x86_64` entries, so `abiFilters` holds.
- **JNI surface verified mechanically, both directions**:
  - `llvm-nm -D` on the packaged `.so`: **55** exported
    `Java_com_lidarscan_app_engine_ScanEngineNative_native*` entry points (46
    from B2/B4/B7/B8 plus the 9 new ones), names matching the Kotlin
    `external fun` declarations exactly;
  - `javap -s` on the compiled Kotlin: `NativeMid360Probe`'s constructor
    descriptor is `(IIJJJJJDDDJJJJJJIJZI)V`, **identical** to the string
    `JNI_OnLoad` looks up, and every new `native` method's descriptor matches
    its C++ parameter list (e.g. `nativeMid360ProbeStart`
    `(JLjava/lang/String;Ljava/lang/String;IIIIIIIIZ)Z`). This is the
    "compiles on both sides, dies at `JNI_OnLoad`" class of bug B2 and B4 both
    flagged, closed for the new surface.
- **Gradle task graph checked** (`--dry-run`): `prepareLivoxSdk2` sits ahead
  of `configureCMakeDebug[arm64-v8a]` and `buildCMakeDebug[...]`, which is the
  ordering the `ON` setting requires.
- **The overlay's idempotency idiom tested directly** (apply → detected as
  already-applied → skipped), since the overlay is empty and the mechanism
  would otherwise be unexercised code.

**Explicitly NOT verified — device-deferred. Everything network is in this
list**, consistent with the Tech Spec's 2026-08-15 hardware-absent addendum
and with A3 §8's own "what is still hardware-only":

- **Any Mid-360 hardware interaction at all.** No device, no USB-C Ethernet
  adapter, no switch. Nothing below has run: SDK2's discovery, the handshake,
  the `0x0100` host-IP configuration push, a single UDP datagram, the loss
  model against real firmware, the watchdog, or a forced re-init.
- **The whole `ConnectivityManager` path.** Whether `requestNetwork` throws on
  a given build, whether the fallback callback fires, what `LinkProperties`
  reports for a given adapter, and whether `Network.bindSocket` actually
  steers traffic — all reasoned from the documented contracts, none executed.
- **The pre-bound fd end to end.** `Os.socket`/`bind`/`bindSocket`/
  `ParcelFileDescriptor.dup().detachFd()` → `UdpConfig::prebound_fd` →
  `UdpSource` receiving on it. The descriptor-ownership split is stated
  precisely in three places, and it is exactly the kind of thing that is
  correct on paper and leaks or double-closes in practice.
- **SDK2 running on bionic.** It *compiles*; whether `LivoxLidarSdkInit`
  succeeds on an Android device is a separate question, and §3 lists the two
  Android-specific reasons found by reading that it might not have.
- **The `TMPDIR` fix.** Reasoned from libc++'s documented lookup order and
  Android's filesystem; not observed.
- **Every OEM menu path** in `StaticIpGuidance`, and whether the Settings deep
  link lands anywhere useful per vendor.
- **Live-SLAM + Mid-360 rendering.** The `StreamFilter` fix compiles and its
  policy is unit-reasoned, but no `kSlamMap` page has been drawn on a device —
  B4's renderer has never executed here either.

**What the protocol-level proof actually is, and where it lives.** This UI is
not the first thing to exercise the Mid-360 protocol, and it does not claim to
be. The protocol is proven by (a) the S2 simulator (`spikes/s2-mid360-sim/`)
driving the real, patched SDK2 against the real driver on loopback — A3 §7's
60-second soak at 199,999 pts/s / 200.00 Hz IMU / 0 lost packets, the 2%
injected-loss run measuring 1.8596% against 2% injected, the cable-pull and
power-cycle runs, and the `.lvx2` replay against Livox's own recordings — and
(b) desktop C2, whose Mid-360 self-test measured 1.59–1.63 s handshake-to-first
-packet through the same `Engine::add_device` call this app makes. B3 is a
transport and a wizard built on top of that proof; it adds Android's
`ConnectivityManager`/`Network`/bionic layer, and **that layer alone is what
needs hardware.** The first bench session should be A3 §8's: re-run the §7
soak with `Mid360Backend::kSdk2` pointed at the physical unit, which from this
app is exactly "type the two IPs into the wizard and press Run self-test".

### 10. Follow-ups this task deliberately did not take

- **A C-ABI addition for the Mid-360's full `Mid360Config`** (§8 items 1, 2,
  4). Until then the capture session cannot use a pre-bound socket, and the
  wizard's probe is a second engine rather than the real one.
- **Deciding how SDK2's sockets get onto the Ethernet network** (§8 item 3) —
  three options, all with real costs, and none of them testable here.
- **Append/resume in the recorder**, which is what Mid-360 pause needs (§7).
- **`pointCountEstimate` on capture stop** is *still* unwired — flagged by B2,
  B4 and B7 before this. `ProjectStore.updateManifest` has existed since B7.
- **A `kImuAvailable` event** (A3 §4's own recommendation): the Mid-360's IMU
  never crosses the C ABI, so the wizard reads its rate only through
  `DeviceHealth.rotation_hz`, which the driver overloads for exactly this
  reason.

## B5 + B6 + B9 + B10 + B11 + B12 + D3 — the rest of §3.13

Tasks B5 (profiles + settings), B6 (processing UI), B9 (RTK UI), B10 (display
panel), B11 (measure + plan view), B12 (georeferenced auto-merge) and D3's
Android half (the cloud client), done together because they share one native
handle and one manifest. Ownership strictly `android/**`; `engine/` and
`cloud/` stayed read-only.

**Pinned against `SCAN_ABI_VERSION` 4 at task start; the header moved to 5
mid-task and the final build is against 5.** See §7 below — nothing broke,
because ABI 5's own note says "every OTHER struct, every function signature and
every enum value from ABI 4 is unchanged", and every symbol this task binds is
an ABI-3/4 one.

> **This is the first task in this workstream that ran the app.** An emulator
> AVD (`b4_test`, android-34 google_apis arm64-v8a, software GPU) was available
> in this environment, so everything below marked "verified on device" was
> actually executed: `JNI_OnLoad`, A15's job queue, A7's post pipeline, A9's PLY
> writer and A12's DXF/PDF writers all ran on Android. That immediately found a
> **real crash that four previous tasks' worth of compile-and-`nm` verification
> could not** (§8.1). The "device-deferred" lists in the sections above were not
> a formality.

### 1. B5 — the profile stops being a label

B1 shipped the four-profile picker and persisted the choice; nothing read it,
and B2 passed the literal `"quickscan"` into `scan_session_config.profile` for
every project regardless.

`:core`'s new `model/CaptureDefaults.kt` is the table that makes the choice
mean something — live-SLAM vs record-only, export format, A14 display profile,
camera keyframes, colorize-by-default, the §3.4 RTK gate and its threshold, and
the A12 slice band — plus `engineProfileString()`, the single place the app
spells the four values `scanengine_c.h` names only in a header comment.

Two decisions worth stating, because §3.9 says "profiles set defaults" without
enumerating them (exactly the gap A14 §5 found on the display side):

* **The defaults are written into the manifest at project creation and belong
  to the project from then on** (`ProjectManifest.captureDefaults`), never
  re-derived from the profile on read. A profile is a *starting point* an
  operator may then change per project, and re-deriving would silently throw
  those changes away; it also means a later app version retuning "Survey" does
  not rewrite an existing capture's settings. Same argument `mid360` (B3) and
  `mountCalibration` (B7) already make. `effectiveCaptureDefaults()` falls back
  to the profile's for a pre-B5 project, so no migration is needed.
* **Research is Record-only.** Its whole point is "keeps every raw stream for
  detailed offline analysis"; live SLAM would spend thermal budget on a preview
  that A7 will re-run at full density from the same bytes anyway, and a phone
  that throttles mid-capture is the failure that avoids. Survey is the only
  profile that *blocks* below its fix gate, and at **RTK Float, not Fixed** — a
  Fixed-only gate makes the app unusable under canopy, and A10 §5 measures
  Float at 29 mm worst-corner error, which is still a usable survey product.

The Settings screen surfaces the table **read-only**, deliberately: a
device-level editor would change nothing about any existing capture while
looking like it did, and the per-project settings that matter are already
editable where they are used (Live-SLAM on Capture, export format on
Processing, the whole display block in Review).

**Also closed here:** `pointCountEstimate` on capture stop, flagged as unwired
by B2, B4, B7 *and* B3 in turn. `CaptureViewModel.stopCapture` now writes it —
along with B9's georef snapshot — through `ProjectStore.updateManifest`, which
has existed since B7.

### 2. B6 + B11 + B12 — one native handle, and why it links C++

`cpp/processing_engine.{h,cpp}` + `processing_jni.cpp` own **one standalone
`scanengine::Engine`** driving A15's `JobQueue`, A12's `extract_floor_plan()`
and A13's `MergeProject`.

**None of those three has a single symbol in the C ABI**, and for the queue
that is a stated design decision rather than an oversight: `scanengine_c.h`'s
own comment ends *"an app that wants a queue should drive a Colorize job
through the C++ `jobs::JobQueue` instead (there is no C surface for the queue at
ABI 4)"*, and `scanengine_c.cpp`'s test-seam comment repeats it. So this follows
the pattern B4 established for `lscan::ReplaySource`, B8 for
`color::KeyframeIndexWriter` and B3 for `Mid360Config`: link `scanengine`
directly, the same static library the C-ABI path already links.

A **separate** Engine from the capture one is also the right shape, not a
workaround: processing runs from a `.lscan` on disk (A7: "a second, better run
from the same bytes"), so it needs no devices, no session and no relationship
to what the capture engine is doing — an operator can post-process yesterday's
project while today's is recording. Using a full `Engine` rather than a bare
`JobQueue` buys three things: its `PageStore` is where every produced cloud
lands (so B4's renderer draws the processed result through the page-read path it
already has), its `EventBus` is what `kJobProgress` is published on, and
`Engine::jobs()` joins the worker thread before the store it appends into is
destroyed.

The queue is **device-level, not per project** (`AppContainer`), created lazily
on first submit: a 20-minute post-process must survive navigating away from the
screen that started it, and an app launch that never processes anything owns no
worker thread. The engine's `Job` has no project field and does not need one —
`ProcessingRepository` keeps the id→project map on the Kotlin side.

**Progress uses both channels, on purpose.** `EventType::kJobProgress` (a
callback-mode `EventBus` subscription — the payload A15 §7.1 asked for and
INT-34 landed) drives the bar promptly from the worker thread; a 500 ms
`JobQueue::list()` poll keeps the *stage label and error* honest, because the
event carries neither.

### 3. B6/D3 — Local and Transfer are engine jobs; Cloud is not

A15 has a `kCloudSubmit` job kind and it works. The app does not use it, and the
brief asked for this to be documented rather than assumed:

Driving it would need an `HttpTransport` implementation, which on Android has to
be Kotlin anyway (there is no HTTP stack in the engine), so the C++ path would
be *Kotlin → JNI → C++ → JNI → Kotlin* for every chunk — and the app would end
up with **two retry policies and two size caps**. So `:core`'s
`com.lidarscan.core.cloud` speaks the documented REST contract directly, and the
Cloud action still runs a real `kTransferExport` job to build the zip so there is
exactly one implementation of "what goes in a `.lscan.zip`" (A5's).

Because the client is plain Kotlin/JVM in `:core`, its end-to-end test is a
**`:core:test` case against the real service**, not an instrumentation test —
see §8.3.

### 4. B9 — RTK, and the one place polling beats events

`gnss_jni.cpp` is the opposite of §2: **every call goes through the C ABI.**
A10 §9.2 listed exactly what B9 would need and INT-29 landed all of it, so
there is no reason to link `gnss/` directly — and going through the C ABI keeps
the rover on the **same `scan_engine*` the capture session owns**, which is what
makes record-always work (`scan_engine_push_nmea`'s own contract: the bytes hit
the `.lscan` as `kGnssNmea` chunks *before* they are parsed).

* **Bonded devices only, no discovery.** Pairing an SPP rover means a system PIN
  dialog; a discovery UI here would add `BLUETOOTH_SCAN` (and, pre-31, a
  *location* permission — the "why does my survey app want my location" prompt,
  on a rig that already has a GNSS receiver attached) and still hand the user to
  the system dialog to finish. The manifest declares `BLUETOOTH_CONNECT` and
  caps the two legacy permissions at `maxSdkVersion="30"`.
* **NMEA in** goes to `scan_engine_push_nmea` in whatever chunk the socket
  produced — the framer handles arbitrary chunking, which is what SPP's 20–990
  byte MTU fragments need. One reused direct `ByteBuffer`, same zero-copy
  posture as B2's serial path (the one `arraycopy` is unavoidable:
  `BluetoothSocket`'s stream is `byte[]`-based).
* **RTCM3 out** is written from the NTRIP receive thread's callback. It does
  **not** tear the link down on an `IOException`: that thread belongs to the
  client and the contract is "quick, must not re-enter", so the reader thread
  notices the same broken socket on its next read.
* **Polled at 1 Hz, not evented.** `SCAN_EVENT_GNSS_FIX`/`NTRIP_STATE` exist and
  carry real payloads, but B2's event pump marshals a fixed `(i0..i4, d0)` tuple
  to Kotlin, which cannot carry a fix's ten fields — widening that signature
  would touch every existing event consumer for a 1 Hz stream. Rebind item, not
  a gap.
* **The §3.4 gate** is `evaluateCaptureGate()` in `:core`: three verdicts, not a
  boolean, because "warn" and "block" are different products of the same
  comparison and the profile decides which applies. `rtkIsTrajectorySource`
  always blocks at no-fix — that is the D6-outdoor case where the missing pose
  means *no cloud*, not merely an ungeoreferenced one (every point lands in
  `dropped_no_pose`).
* **The georef snapshot** is written into the manifest at capture stop. A10 §9.6
  asks for exactly this, and B12 is what makes it load-bearing.

### 5. B10 — a port, not a binding, and not the UBO A14 predicted

`:core`'s `render/DisplayParams.kt` ports A14's model by formula, the same
answer B4 gave for `colormap_lut()` and for the same reason: `scanengine_c.h`
mirrors **none** of `display_params.h`, at ABI 5, so there is no JNI call to
make.

Two things are deliberately **not** ported. `DisplayParamsUniforms`/`to_uniforms()`:
A14 §4 predicted B10 would treat it as a raw std140 UBO, but B4's renderer is
**Filament**, whose `MaterialInstance` takes named parameters and not a byte
blob — so this app is in C1's position, not the one A14 expected, and porting a
208-byte layout that nothing reads would be dead weight that could silently
drift. And `DisplayParamsController`: Compose `StateFlow` already gives both of
A14 §7's modes, and a second notification mechanism beside it would be two
sources of truth.

`PointCloudRenderer.setDisplayParams()` binds everything `points.mat` declares.
Three fields do not reach the shader, exactly as A14 says they should not:
`lodPointBudget` is a CPU-side page-admission decision, the two overlay toggles
are the app's, and `background` is the `Renderer`'s clear colour (only on the
opaque path — the AR overlay's clear must stay transparent black or it paints
over the camera). A14 §2's `auto_range` rule is implemented on the side the
header says owns it: the renderer refreshes `manual_min/max` from the combined
page bounds each frame.

**Two things are honest about not working**, rather than shipping a control that
silently does nothing: the LOD budget is a *page-admission ceiling* applied in
page order (it stops before the budget; it does not decimate within a page), and
the EDL switch persists but is not rendered — `points.mat` has no post-process
pass and S3 never measured EDL's cost on a phone GPU. Both say so on screen.
`colorModeAvailability()` does the same for Time (no per-point timestamp in
`PointVertex`) and Fix quality (needs a live rover).

### 6. B11/B12 — the two honest refusals

**Measure** picks by projecting a bounded sample to screen and taking the
nearest within a radius, with **depth as the tie-break** — without it a tap on a
near wall routinely selects the far wall seen through the gaps between points.
The sample is 200k points taken with a stride across the whole cloud, and the
readout says "nearest sampled point" rather than implying the pick is exact.

**Merge refuses politely when not georeferenced** (§3.10: "Android offers
georeferenced auto-merge only") and says why: manual 3-point alignment needs a
two-cloud picking workspace, which is C6's merge workbench. The alternative —
merging at the identity — is what A13 itself calls "the worst possible failure
mode: it looks like data".

The Android merge also **post-processes each session first**, and the UI states
that up front because it is the expensive part. It is unavoidable: A13 cannot
read a cloud out of a `.lscan` (`SessionMerger::add_session(lscan_dir)` is
unimplemented — nothing writes a processed cloud into one, A7 §8 item 2), so
"open two finished projects and merge them" necessarily means re-running the
pipeline for each. A session already post-processed in the same app session is
reused via its job id.

### 7. Rebind items for ABI 5 (and what did NOT break)

The header moved 4 → 5 while this task ran (INT-FINAL). The final build is
against 5 and the runtime check logs `engine ABI version 5`. Nothing this task
binds changed — ABI 5's own note is explicit that only `scan_device_config`
changed layout and that every other struct, signature and enum value is
unchanged, and `gnss_jni.cpp` zero-fills that struct and sets only `kind`.

Rebind items, in priority order:

1. **`scan_device_config`'s Mid-360 half is now complete** — backend selector,
   **two** pre-bound descriptors, all ten ports, `recv_buffer_bytes`, the point
   filter, the live budget and `sdk_config_path`. This closes B3's §8 findings
   1, 2 and 4 outright: the capture session can now use a pre-bound socket, the
   two-socket backend has two fds, and the `TMPDIR` work-around in
   `AppContainer.init` can be replaced by `sdk_config_path`. B3's standalone
   `Mid360Probe` could collapse back into the C ABI.
2. **`scan_engine_set_crs()` is new** — the CRS escape hatch INT29 §7 item 5
   asked for. B9's Survey profile should grow an EPSG picker with a
   caller-supplied WKT on top of it (§3.4's "EPSG picker (survey profile)"),
   which is the one part of §3.4 this task did not build.
3. **The event pump's `(i0..i4, d0)` tuple cannot carry a GNSS fix**, so B9
   polls (§4). Widening `EngineEventListener.onEvent` — or giving it a per-type
   marshalling class — would let `SCAN_EVENT_GNSS_FIX`/`NTRIP_STATE`/
   `GEOREF_CONVERGED` drive the status strip instead.
4. **No C-ABI accessor for the GNSS ENU frame.** `scan_gnss_stats` has
   `has_origin`/`origin_*`, and `scan_georef_solution` carries the transform
   without the frame it maps into — but A13 needs the frame
   (`merge/session.h`: "THE ENU FRAME IS NOT OPTIONAL AND IS NOT SHARED"). B9
   records the first fix at or above the origin gate itself, using the same rule
   `GnssSourceConfig::min_fix_for_origin` uses. Reading `origin_*` through the
   existing stats call would be strictly better and is a two-line change.
5. **No `scan_engine_timesync_quality()`**, so a *recorded* session's A4 verdict
   cannot be read back. B6 uses the mount calibration's accepted clock sweep as
   the only evidence the app has that the two clocks were tied together, and
   reports `UNKNOWN` (which fails closed) otherwise.
6. **Still no C surface for the job queue, plan or merge** — by design for the
   queue (§2). Nothing needs to change; this is recorded so the next reader does
   not go looking.
7. **USB-serial GNSS (e.g. a Unicore UM982 eval board) is not wired into the
   app at all — found during ANDROID TEST PACKAGE field-test-kit prep, same
   class of gap as B3's pre-bound socket but on the RTK side.** §4's
   Bluetooth-SPP rover path is the *only* way this app feeds bytes into
   `scan_engine_push_nmea`. A GNSS receiver presented as USB-serial
   (CH340/CP210x — the common shape of a UM982 carrier/eval board with no
   Bluetooth added) has no reader: there is no `UsbSerialProber` device
   entry routed to `push_nmea`, and `usb_device_filter.xml` only lists D6's
   CH340 VIDs/PIDs for the D6 flow specifically — nothing distinguishes a
   second CH340 port as "this one is GNSS, not lidar." **Field verdict**: a
   UM982 with no Bluetooth SPP carrier cannot be tested against this Android
   app today; test it against the desktop/Windows kit instead (which has a
   general serial path), or use a UM982 carrier that adds Bluetooth SPP and
   pair it per §4. **The fix, sized for next integration**: a small
   `GnssUsbSerial` reader that reuses `D6SerialConnection`'s
   `usb-serial-for-android` open/permission/read plumbing (§4 above, "D6
   connect flow") against a second `UsbSerialPort`, feeding
   `scan_engine_push_nmea` with the same one-reused-direct-`ByteBuffer`
   posture the Bluetooth path already uses (§4, "NMEA in"), plus adding
   GNSS-class VID/PID pairs (CH340 `0x1A86`/`0x7523`, already declared for
   D6; CP210x `0x10C4`/`0xEA60`, not yet declared anywhere) to a
   GNSS-specific device picker on the RTK connect screen so a rig running a
   D6 and a UM982 both over USB-serial can tell them apart. Small — this is
   a third instantiation of a pattern (serial open + permission + a
   direct-buffer reader thread) that already exists twice in this codebase
   (D6, Bluetooth-SPP NMEA), not new plumbing.

### 8. Verification — what actually ran

#### 8.0 The emulator, and how to get one

B4 tried this and ran out of time on the system-image download. It has since
finished: an AVD named **`b4_test`** (`android-34`, `google_apis`,
`arm64-v8a`, software GPU) exists in `~/.android/avd/`. Booting and driving it:

```
export ANDROID_HOME=/opt/homebrew/share/android-commandlinetools
$ANDROID_HOME/emulator/emulator -avd b4_test -no-window -no-audio \
    -no-boot-anim -gpu swiftshader_indirect -no-snapshot &
$ANDROID_HOME/platform-tools/adb wait-for-device
adb install -r -t app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.lidarscan.app.debug/com.lidarscan.app.MainActivity
adb exec-out screencap -p > shot.png     # 1080x2400
adb shell input tap <x> <y>              # drive the UI
adb logcat -d -b crash                   # the thing that matters
```

Note the **`.debug` suffix** on the application id — `am start` against
`com.lidarscan.app` fails with "Invalid packageName", which is a confusing two
minutes if you have not hit it before. The GPU is software, so Filament
initializes and uploads but nothing about how the cloud *looks* is verified
there (§9).

#### 8.1 The crash only a device could find

`Manipulator.nCreateBuilder()` threw `UnsatisfiedLinkError` the first time a
Filament view attached, taking the process down. `Filament.init()` loads
`libfilament-jni.so` and **nothing else**; `Manipulator` lives in the separate
`filament-utils` artifact whose JNI is in `libfilament-utils-jni.so`, which only
`Utils.init()` loads. B4 called just the first.

This compiled cleanly, passed every unit test, and was invisible to `javap` and
`llvm-nm` — the classes and symbols all exist, they were simply in an unloaded
library. It would have crashed B4's **Capture** screen too, on the first frame,
on every device. Fixed in `FilamentLoader.ensureInitialized()`.

#### 8.2 Unit tests — 206 total, 0 failures

```
:core:test            194 tests, 0 failures, 5 skipped
:app:testDebugUnitTest 12 tests, 0 failures
```

`:core` was 104 at B3; the new cases are `CaptureDefaultsTest` (11 — the profile
table, including "colorization is never pre-checked without keyframes to sample
from"), `DisplayParamsTest` (14 — A14 §5's four presets value-for-value, the
clamp's boundaries, and `evaluate_point_color`'s fallback table including the
alpha-is-always-the-vertex's rule in every mode), `GnssModelsTest` (13 — the fix
ladder's ordering, the §3.4 gate's three verdicts, NTRIP validation),
`MeasureTest` (12 — projection convention, the depth tie-break, and the
feet+inches formatter's 11.99″ rounding), `ProcessingPolicyTest` (12 — each
refusal in its documented order), plus the 28 the cloud client brought.
The 5 skips are the cloud e2e cases, which skip with an explanation when the
service env vars are absent (§8.3).

`:app` gained a plain-JVM `NativeMarshallingTest` (12) over the flat-array JNI
layouts — see §8.5 for why those exist. **Two of these tests failed first and
found real bugs**: `MergeRepository.encodeGeoref` was padding a wrong-length
matrix element-wise (producing a singular transform that would collapse a
session to a point) instead of falling back to the identity wholesale.

#### 8.3 The cloud client, against the real service

`cloud/service` was started locally (its existing venv, uvicorn, a fake worker
script, `LIDARSCAN_DATA_DIR` in a scratchpad so `cloud/` stayed untouched) and
the `:core` e2e test drove the Kotlin client against it. Real results:

* **create → chunked upload → poll → download**: a 3,147,499-byte
  `.lscan.zip` uploaded in 8 chunk acks; states observed
  `PROCESSING@0.0 → 0.35 → 0.95 → DONE@1.0`; result downloaded and verified
  byte-identical on a second download.
* **A genuine resume**: the transport dropped three acks for chunk 1 while the
  bytes really reached uvicorn; retries were exhausted; the client issued
  **exactly one** probe (`Content-Range: bytes */3147499`); the service answered
  `308` with `Upload-Offset: 1048576` — two chunks in, i.e. *past* the chunk the
  client thought had failed — and the client resumed there rather than
  re-sending. The service's own log confirms the sequence independently.
* **401** with a wrong token and **404** for an unknown job, from the real
  service, not a fake.

The e2e cases skip with a printed explanation when `LIDARSCAN_E2E_BASE_URL` /
`LIDARSCAN_E2E_TOKEN` are unset, so `./gradlew :core:test` stays green for
anyone without the service running.

#### 8.4 On the device — the whole B6 → B11 chain

Installed on the `b4_test` AVD and driven through the UI (`adb input` +
`screencap`; screenshots are in the task's scratchpad, not committed):

* `JNI_OnLoad` succeeded — `scanengine_jni loaded; engine ABI version 5`. That
  is every `FindClass`/`GetMethodID` resolving, **including the two new
  marshalling classes**, which closes the "compiles fine, dies at load" risk
  B2/B4/B7/B3 each flagged and none could retire.
* **B5**: a Survey project created through the UI wrote `captureDefaults`
  (LAS 1.4, camera keyframes on, blocks below RTK Float) and A14's Survey
  `displayParams` into `manifest.json`. Read back off the device.
* **B6**: a real `kTransferExport` job produced a valid `.lscan.zip`
  (`manifest.json` + `streams/lidar.bin`, 263,230 bytes), and the queue row
  showed `Transfer bundle #1 — Done`.
* **A7 post-process on a real capture**: `desktop/evidence/c4-synth-mid360.lscan`
  (S2's protocol-faithful simulator output, decoded by the same driver a real
  device feeds) was seeded as a project and post-processed **on the phone** →
  **83,228 points** resident, and the Export/Colorize gates advanced to their
  next reason exactly as the policy says.
* **A9 export**: a 1,331,908-byte binary PLY written on device.
* **B11 floor plan**: A12's extraction run on that cloud → **2 walls,
  5 openings**; **DXF** (4,694 bytes, `AC1009`, 12 `POLYLINE`s, `WALLS`/
  `OPENINGS`/`DIMENSIONS` layers) and **PDF** (2,512 bytes, `%PDF-1.4`, valid
  `%%EOF` trailer) both written by A12's own writers through this JNI.
* **B10**: the display bottom-sheet opened with Height pre-selected from the
  persisted Survey profile, Time disabled with its explanation, and the
  renderer's clear colour taken from the profile's background.
* **B9/B12**: the RTK screen rendered the fix strip, the §3.4 **warning** banner
  (Survey's gate, no rover), the Bluetooth-permission flow and live NTRIP
  validation; the Merge screen refused with "No georeference recorded — this
  capture had no RTK rover attached."
* **Zero `FATAL EXCEPTION`s** across the whole walkthrough after the §8.1 fix.

#### 8.5 JNI surface — 105 entry points, descriptors checked mechanically

`llvm-nm -D` on the packaged `.so`: **105** exported
`Java_com_lidarscan_app_engine_ScanEngineNative_native*` symbols (55 from
B2/B4/B7/B8/B3 plus 50 new), arm64-v8a only. `javap -s` confirms every new
`native` method's descriptor matches its C++ parameter list, and both new
constructor descriptors match the strings `JNI_OnLoad` looks up:

```
NativeJob            (JIIFILjava/lang/String;Ljava/lang/String;)V
NativeMergeSummary   (ZIIIIIFFJJZLjava/lang/String;Ljava/lang/String;)V
nativeProcRunPlan    (JFFFZFIZZZ)Z
nativeProcRunMerge   (J[Ljava/lang/String;[Ljava/lang/String;[J[DLjava/lang/String;L…MergeProgressListener;)L…NativeMergeSummary;
nativeNtripConnect   (JLjava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;IZIZ)I
```

**Only two new marshalling classes, and that is a deliberate trade.** B2, B4 and
B7 all flagged that a hand-typed constructor descriptor compiles on both sides
and only fails at `JNI_OnLoad`. So a class is used only where a record genuinely
mixes numbers and strings (`NativeJob`, `NativeMergeSummary`); the floor-plan
model and the four GNSS structs cross as **flat primitive arrays with a
documented index layout** — no descriptor, no `FindClass`. The cost is that a
shifted layout is a wrong *number* rather than a load-time abort, which is
exactly what `NativeMarshallingTest` exists to catch. `scan_gnss_fix.utc_unix_ns`
is the one field that does not fit a double, so it crosses as **milliseconds**
and is named `utcUnixMillis` on the wire.

`:app:assembleDebug` is green from a wiped `app/.cxx` + `app/build`; the debug
APK is **79.8 MB** and `lib/arm64-v8a/libscanengine_jni.so` is **6.96 MB**, up
from B3's 5.84 MB — the growth is `jobs/`, `plan/` and `merge/` being linked in
for the first time. Still arm64-v8a only, so `abiFilters` holds.

### 9. Explicitly NOT verified

* **Any real hardware.** No RTK rover, no D6, no Mid-360, no ARCore device. The
  whole B9 stack — Bluetooth SPP `connect()`, the NMEA reader thread,
  `scan_engine_push_nmea` against real sentences, the NTRIP handshake against a
  real caster, RTCM3 reaching a rover — is reasoned from the documented
  contracts and **none of it has run**. The mountpoint picker has never fetched a
  source table.
* **The 3D view rendering actual points.** Filament initializes and the page
  pipeline uploads (83,228 points resident, confirmed on screen), but the
  emulator is software-rendered with `hw.gpu.enabled=no`, so whether
  `gl_PointSize` is honoured and whether the cloud looks right is untested. The
  measure tool's picking was therefore never exercised against a *drawn* frame —
  its geometry is unit-tested, its ergonomics are not.
* **Colorize end to end.** No capture in this environment has camera keyframes,
  so `run_colorize` has never executed; only its gate has.
* **B12's merge past the refusal.** No two georeferenced sessions exist without a
  rover, so `align_georeferenced` → `refine` → `build` has not run. The refusal
  path is verified; the success path is not.
* **The Cloud path from the phone.** The client is proven against the real
  service from the JVM (§8.3); the *Android* side of it — DataStore config,
  `HttpURLConnection` on bionic, a cleartext-traffic exception for a local
  server — has not been exercised on the device.
* **The share sheet's receiving end.** `FileProvider` grants were never followed
  into another app.
* **Rotation, dark theme, and every screen's behaviour under configuration
  change.**

### 10. Follow-ups this task deliberately did not take

* **§3.4's EPSG picker** for the Survey profile — now unblocked by ABI 5's
  `scan_engine_set_crs()` (§7 item 2).
* **A foreground service** for processing. §3.8 says "Android (foreground
  service)" and this runs the queue in-process, so a long post-process dies if
  the app is backgrounded and reaped. The queue and its progress plumbing are
  service-ready; wiring one is the next thing B6 needs.
* **The cloud token in the Android Keystore.** It is in app-private DataStore
  today, which the Settings copy states plainly.
* **A12's include/exclude regions** (`PlanRegion`) — they need a draw-on-the-plan
  interaction that belongs to the desktop workspace; the slice-height slider,
  which §3.6 names as editor v1, is the half that carries the value.
* **Upload resume across app restarts** and a streaming result download.
* **Per-page frustum culling** in the renderer (still B4's open item).

## Things intentionally deferred (not oversights)

- No Hilt/Dagger — manual `AppContainer` + `viewModelFactory { initializer {} }`
  per the brief ("DI-lite… no Hilt yet").
- No storage-location picker (SAF) — Settings shows the path read-only.
- No project rename — directory id is the name-at-creation-time slug; adding
  rename later doesn't require a schema change (id and display `name` are
  already decoupled).
- `material-icons-extended` is used instead of `material-icons-core` —
  pulling in both causes duplicate-class build failures (extended already
  contains everything core does), so only one is declared.

## Android emulator smoke test

A permanent instrumentation test (`android/app/src/androidTest/`), gated on
every push in `.github/workflows/android-emulator.yml`, added specifically
because unit tests, `javap`, and `llvm-nm` all missed a real crash and only a
booted device found it — see §8.1 above ("The crash only a device could
find"): `Manipulator.nCreateBuilder()` threw `UnsatisfiedLinkError` the first
time a Filament view attached, because `Filament.init()` loads
`libfilament-jni.so` and nothing else — `Manipulator` needs the separate
`filament-utils` artifact's `libfilament-utils-jni.so`, loaded only by
`Utils.init()`. B4 called just the first. It compiled cleanly, passed every
unit test, and was invisible to static inspection of the `.so` — the classes
and symbols all existed, just in an unloaded library. Nothing before this
test ran that check automatically; every prior "on device" verification
pass in this file (B4 §8, B3/B7/B8/B5-B12 §8) was a one-off, run by hand,
never wired into CI.

### What it covers

`android/app/src/androidTest/kotlin/com/lidarscan/app/ReplayCaptureSmokeTest.kt`,
two `@Test`s, both driven through `MainActivity` (a real Activity launch, not
a Robolectric shadow):

1. `launchReachesProjectsListWithoutCrashing` — the cheap end. A cold launch
   alone exercises `LidarScanApplication`/`AppContainer` construction (which
   touches `ScanEngineNative`'s `System.loadLibrary("scanengine_jni")`) and
   Compose's first composition. This is the JNI_OnLoad class of crash the
   task brief calls out, and it is everything a crash *before* any
   `SurfaceView` screen is reachable would look like.
2. `replaySyntheticCaptureDecodesPointsWithoutCrashing` — the one that
   actually reaches Filament. It launches `MainActivity` with a debug-only
   intent extra that deep-links straight to the "Replay synthetic capture"
   acceptance path (bundled `assets/replay/synth.lscan/`, no hardware — see
   B4 §3 above), taps "Start replay", then polls the live stats panel's
   "Points captured" value for ~10s, asserting it is positive, never goes
   backwards, and grows at some point in that window. A crash anywhere in
   that path (Filament init, the native replay engine, the JNI page-read
   marshalling) fails the whole instrumentation run outright — the same
   failure mode B4's own bug had — rather than tripping a normal assertion.

### Hooks added to app code (kept minimal, documented at each site)

- **`com.lidarscan.app.debug.ReplayDeepLink`** (new file): a debug-only
  launch intent extra, `EXTRA_LAUNCH_REPLAY_CAPTURE` (boolean). When
  `MainActivity.onCreate` sees it set, it find-or-creates the same
  "Synthetic Replay Demo" project `SettingsViewModel.replaySyntheticCapture`
  does and navigates straight to `Routes.replayCapture(projectId)` —
  skipping the Projects -> Settings -> tap path a human uses. This is a real,
  reusable deep link, not a test-only branch: nothing about it depends on
  the instrumentation-test process, and it is what let the smoke test avoid
  tapping through two screens (and their own async project-list load) on
  every run. `SettingsViewModel` was refactored to import the shared
  `REPLAY_PROJECT_NAME` constant from this file instead of keeping its own
  private copy, so the two call sites can't drift on the project name.
- **`Modifier.testTag("pointsCapturedValue")`** on the "Points captured"
  `StatRow`'s value `Text` in `CaptureScreen.kt` (`StatRow` gained an
  optional `valueTestTag` parameter — every other call site is unaffected).
  This is what the test polls; parsing the on-screen `"%,d"`-formatted
  string is simpler and more honest than adding a second, parallel
  test-only data path that could itself drift from what a user sees.
- **`Modifier.testTag("replaySyntheticCaptureButton")`** on the Settings
  screen's "Replay synthetic capture" button — not used by the current test
  (which uses the deep link instead), but added because that screen has two
  `Text` nodes with the identical string ("Replay synthetic capture" is both
  the button's label and the line of copy above it), which would make a
  plain-text UI-navigation variant of this test ambiguous without it. Left
  in place for exactly that future variant.

### Workflow: `.github/workflows/android-emulator.yml` — three real CI
attempts, a pivot, and the status this task ends on

Separate from `engine-ci.yml`'s `android-app` job (JDK/SDK setup,
`:core:test`, `:app:assembleDebug`, `continue-on-error: true`, no device) —
this new job is meant as a hard gate (`continue-on-error: false`) that
actually boots a device. What follows is the real sequence three pushes to
`ci/emulator` produced, not the plan written before any of them ran — worth
keeping honest because iteration 2 alone cost real CI minutes finding out a
flag didn't do what its own documentation implied.

**Iteration 1 — `macos-latest` + `arm64-v8a`, no special flags.** The
original design: `android/app/build.gradle.kts` pinned `ndk.abiFilters` to
`arm64-v8a` only (B2 §1 — Pixel 7+/Galaxy S22+ reference hardware), so the
debug APK shipped **only** arm64-v8a native libraries
(`libscanengine_jni.so`, `libfilament-jni.so`, `libfilament-utils-jni.so`,
all of it) — an x86_64 emulator could install that APK but every native
call, Filament's own init included, would fail immediately with a
missing-ABI `UnsatisfiedLinkError`, worthless as a regression gate. GitHub's
`macos-latest` runners are Apple Silicon hosts, so an arm64-v8a system image
looked like the natural hardware-accelerated pairing (Apple's
Hypervisor.Framework doing for arm64-on-arm64 what KVM does for
x86_64-on-x86_64) — the exact ABI NOTES.md's B4/B7 sections already used on
a real Mac (the `b4_test` AVD, §8.0 above). **Result: FAILED.**
`HVF error: HV_UNSUPPORTED` / `qemu-system-aarch64-headless: failed to
initialize HVF: Invalid argument`, emulator process exited at start, the
action's boot-wait loop then polled a dead `emulator-5554` for its full
timeout. **Root cause**: GitHub-hosted macOS runners are themselves VMs, and
Apple's Hypervisor.Framework does not support the nested virtualization a
*second*-level guest (the Android emulator's own QEMU) needs. This is a
restriction on GitHub's *hosted* macOS VMs specifically — a physical Apple
Silicon Mac (this task's own dev environment, see "Local validation" below)
has no such restriction.

**Iteration 2 — same, + `-accel off`.** The obvious fix for iteration 1:
force software (TCG) CPU emulation instead of HVF, same architecture so not
the crippling cost cross-arch translation would be. **Result: FAILED the
same way.** The emulator's own frontend printed "arm64-v8a emulation may not
work without hardware acceleration!" (i.e. it saw and accepted the flag),
but the underlying `qemu-system-aarch64-headless` process still
unconditionally attempted HVF init and crashed regardless — confirmed from
the run's own log, not assumed. On this emulator build (37.1.11.0),
`-accel off` does not fully suppress the HVF attempt on a GitHub-hosted
macOS runner. This is a genuine dead end for `macos-latest`, not a flag this
task got wrong.

**Iteration 3 (current) — pivot to `ubuntu-latest` + `x86_64`, KVM.**
GitHub-hosted Linux runners expose `/dev/kvm` (after the standard "Enable
KVM group perms" udev step — `reactivecircus/android-emulator-runner`'s own
documented pattern, now in the workflow) for a same-arch
(x86_64-on-x86_64) guest; this is that action's primary documented path and
is unrelated to the macOS nested-virtualization restriction above. Switching
architectures reopens the ABI problem iteration 1's design was built to
avoid, so `android/app/build.gradle.kts`'s `defaultConfig.ndk.abiFilters`
now lists **both** `arm64-v8a` and `x86_64` (previously arm64-v8a-only) —
purely additive, and also lets any developer run this app on a plain x86_64
Android Studio emulator locally, which arm64-only never allowed.
**Verified locally that this compiles**: a clean
`:app:externalNativeBuildDebug` cross-compiles `engine/` + the vendored
Livox-SDK2 + the JNI shim for x86_64 with zero source changes needed (no
ARM-specific code anywhere in that path — unsurprising given `engine/`'s own
`macos-universal` CI leg already lipo's arm64+x86_64 together, just not
through the Android NDK before now).

**This task ends with the ubuntu-latest+x86_64+KVM workflow committed but
NOT verified against a live GitHub Actions run** — this repository's Actions
minutes were exhausted (billing/quota) partway through iteration 3, before
it could be pushed through a real run. New pushes to `ci/emulator` after
this task's commits will insta-fail with zero steps executed (a runner
refusal, not a code problem) until quota/billing is restored. The KVM step,
`x86_64` arch, and `google_apis` target are the action's own documented
`ubuntu-latest` pattern used as-is, and nothing in this pairing has the
class of surprise iterations 1–2 found (KVM-for-same-arch on Linux is by far
the most common, most battle-tested configuration for this action across
the wider ecosystem) — but "should work" is exactly the confidence level
iteration 1 also started at, so treat this as unverified until a real run
says otherwise.

### Local validation (while CI quota is unavailable)

This task's own environment turned out to be a **physical** machine with a
real JDK/NDK/SDK toolchain and — critically — the `b4_test` AVD (§8.0 above:
android-34, google_apis, **arm64-v8a**) already present, i.e. the exact ABI
the original macos-latest design needed, hardware-accelerated for real
because this machine is not a nested GitHub-hosted VM. Booting it and
running `./gradlew :app:connectedDebugAndroidTest` against it is the closest
substitute available for the blocked CI run, and it is what this task used
for its last two iterations instead of consuming (nonexistent) Actions
minutes:

- **First local run: 1 of 2 tests failed** —
  `replaySyntheticCaptureDecodesPointsWithoutCrashing` timed out after 20s
  waiting for the "Start replay" button. Diagnostic `Log.d` calls added
  temporarily (and removed again once done) to `MainActivity`'s
  `LaunchedEffect` and `CaptureViewModel`'s replay-connect coroutine showed
  the deep link, project creation, navigation, and
  `ReplayEngineBridge.connect()` all completing correctly in under 200ms —
  `connectionState` really did reach `CONNECTED` almost immediately. **The
  bug was in the test, not the app**: `RecordingControls`' button text is
  `"  Start replay"` — two literal leading spaces, from the
  `Icon(...) + Text(...)` row layout — and Compose's `onNodeWithText`/
  `onAllNodesWithText` default to an **exact** match, which silently never
  matched and spun the full `waitUntil` budget before failing. Fixed by
  passing `substring = true` to both call sites
  (`ReplayCaptureSmokeTest.kt`).
- **Second local run, after the fix: both tests passed.**
  `./gradlew :app:connectedDebugAndroidTest` → `BUILD SUCCESSFUL`, 0
  failures, on the real `b4_test` (arm64-v8a) AVD. The full
  `replaySyntheticCaptureDecodesPointsWithoutCrashing` run (navigate,
  connect, start replay, wait for points > 0, hold ~10s re-sampling) took
  about 11.7 seconds end to end and the point count genuinely grew during
  the hold window (not just a single sample above zero). This is real
  acceptance evidence that the test class works exactly as designed: it
  reaches Filament, reaches the native replay engine, and would fail loudly
  (an instrumentation-run abort, not a normal assertion) on a repeat of
  B4's `Utils.init()` bug.
- This local pass used **arm64-v8a**, the same ABI/target as the original
  design and as NOTES.md's other on-device verification passes — it is not
  evidence for the ubuntu-latest+x86_64+KVM *workflow* specifically (that
  pairing is still CI-unverified, see above), only for the **test code and
  app code** being correct, which is the harder-to-fake part.

### Hooks added to app code (kept minimal, documented at each site)

- **`com.lidarscan.app.debug.ReplayDeepLink`** (new file): a debug-only
  launch intent extra, `EXTRA_LAUNCH_REPLAY_CAPTURE` (boolean). When
  `MainActivity.onCreate` sees it set, it find-or-creates the same
  "Synthetic Replay Demo" project `SettingsViewModel.replaySyntheticCapture`
  does and navigates straight to `Routes.replayCapture(projectId)` —
  skipping the Projects -> Settings -> tap path a human uses. This is a real,
  reusable deep link, not a test-only branch: nothing about it depends on
  the instrumentation-test process, and it is what let the smoke test avoid
  tapping through two screens (and their own async project-list load) on
  every run. `SettingsViewModel` was refactored to import the shared
  `REPLAY_PROJECT_NAME` constant from this file instead of keeping its own
  private copy, so the two call sites can't drift on the project name.
- **`Modifier.testTag("pointsCapturedValue")`** on the "Points captured"
  `StatRow`'s value `Text` in `CaptureScreen.kt` (`StatRow` gained an
  optional `valueTestTag` parameter — every other call site is unaffected).
  This is what the test polls; parsing the on-screen `"%,d"`-formatted
  string is simpler and more honest than adding a second, parallel
  test-only data path that could itself drift from what a user sees.
- **`Modifier.testTag("replaySyntheticCaptureButton")`** on the Settings
  screen's "Replay synthetic capture" button — not used by the current test
  (which uses the deep link instead), but added because that screen has two
  `Text` nodes with the identical string ("Replay synthetic capture" is both
  the button's label and the line of copy above it), which would make a
  plain-text UI-navigation variant of this test ambiguous without it. Left
  in place for exactly that future variant.
- **`android/app/build.gradle.kts`'s `defaultConfig.ndk.abiFilters`** now
  includes `x86_64` alongside `arm64-v8a` — see "Iteration 3" above. Not a
  test-only hook in the same sense as the others (it changes what ships in
  every debug **and release** build, since AGP has no per-build-type
  `abiFilters` override — only `defaultConfig`/product-flavor level), but
  recorded here because the CI pivot is the reason it exists.

### Workflow config knobs (unrelated to the accel history above)

- **AVD caching** (`actions/cache` keyed on API level/target/arch, a
  create-and-snapshot step gated on cache miss) — `reactivecircus/
  android-emulator-runner`'s own documented pattern; a cold AVD create + first
  boot (system-image download + setup wizard) is the single slowest, least
  interesting part of this job and does not change run to run.
- **Two disable-animations knobs**: `app/build.gradle.kts`'s
  `testOptions.animationsDisabled = true` (instrumentation-runner side) and
  the emulator-runner action's own `disable-animations: true` input
  (device-settings side) — window-animation flakiness and boot flakiness are
  the two classic emulator-CI failure modes; each knob covers one.
  `-no-boot-anim` on the emulator itself covers a third.
- **Logcat capture**: `adb logcat -v threadtime` piped to a file for the
  whole `:app:connectedDebugAndroidTest` run, uploaded (with the
  instrumentation HTML/XML report) only `if: failure()`. This is deliberate,
  not boilerplate — B4's own crash (§8.1) was found by reading exactly this
  kind of log, not by a JUnit assertion tripping afterward; a bare
  test-failure message would not have said *why*.
- Gradle cache: same key pattern as `engine-ci.yml`'s `android-app` job
  (hash of `**/*.gradle*` + the wrapper properties + the version catalog),
  under its own `gradle-emulator-` prefix so the two jobs' caches don't
  collide on `runner.os` alone.

### Running it locally

```
cd android
./gradlew :app:assembleDebug :app:assembleDebugAndroidTest
# with a booted emulator/device (adb devices shows one) — arm64-v8a or
# x86_64 both work now that abiFilters covers both:
./gradlew :app:connectedDebugAndroidTest
```

On Apple Silicon, an `arm64-v8a` AVD (e.g. this file's own `b4_test`, §8.0
above) is hardware-accelerated via HVF; that pairing is also what "Local
validation" above actually ran. An `x86_64` AVD works too (KVM on Linux,
HAXM/WHPX on Windows/Intel Mac) now that the APK ships both ABIs. Results
land in `app/build/reports/androidTests/connected/` and
`app/build/outputs/androidTest-results/connected/` either way.

### Outstanding: what still needs to happen

1. **Restore GitHub Actions quota/billing** on this repository.
2. **Push any small change to `ci/emulator`** (or re-run the workflow via
   `workflow_dispatch`) to get the ubuntu-latest+x86_64+KVM configuration
   through one real run. Expect it to need at least one more small
   iteration even if the big HVF-class surprise is behind it — emulator CI
   configs commonly need one round on boot timing/caching details the first
   time they run somewhere new.
3. Once green, this becomes the enforced gate the task asked for
   (`continue-on-error: false` is already set — nothing else to flip).

## UI redesign — the owner-approved cockpit (Projects / Capture / Jobs / Review / Settings)

The design spec, in the priority order the task set it:

1. `docs/design/REVIEW_FEEDBACK.md` — five owner review rounds **with their
   resolutions**. These are requirements, not suggestions, and the four
   numbered items are all implemented below (§4 in particular: the AR/camera
   telemetry block is gone from the capture body).
2. `docs/design/redesign-exports/*.png` — the visual ground truth
   (`01`–`04` for the Android screens, `fix-r*` for the sheet and diagnostics
   iterations).
3. `docs/design/lidarscan-interfaces.html` — the interactive mockup, read as
   the *behavioural* spec: `A.capture`, `capSheet()`, `diagSheet()`,
   `paintDiag()`, `closeCapSheets()`, `arTrackState()`, `TABOF`.

**This was a UI-layer rework.** No `EngineBridge` implementation, JNI shim,
CMake target, renderer or repository changed shape. The three behavioural
changes outside `ui/` are all in service of a control the redesign introduces,
and each is called out where it happens (§4 below).

### 1. Theme: tokens, three bundled families, rounder shapes

`ui/theme/Color.kt` is no longer "a neutral placeholder palette" — it is the
product palette: ground `#12161B`, panel `#1A2027` / `#222A33`, line
`#2B3540`, ink `#ECF1F5`, mute `#94A1AD`, ember `#FF7A52`, teal `#3EC4B0`,
sand `#E5C468`, pose `#6AA7E8`, and the semantic trio good `#49D17F` / warn
`#E5B93C` / bad `#E05252`.

`ui/theme/Theme.kt` maps them onto Material roles (the table is in its KDoc)
so a component nobody restyled — a dialog, a snackbar, a text field — still
lands in the right world. The semantic trio and the sensor identities stay as
fixed values rather than roles, because a "Float" badge must not change
meaning with the theme.

Two deliberate changes fall out of having a real palette:

- **Dynamic colour is off by default.** It was on while the palette admitted
  to being a placeholder; wallpaper-derived colour would replace exactly the
  one accent the cockpit is built around. The parameter still exists.
- **`ThemeMode` defaults to `DARK`, not `SYSTEM`** (`data/SettingsModels.kt`
  *and* `data/SettingsRepository.kt` — both defaults had to move, since the
  DataStore flow's value replaces `AppSettings()`'s the moment it emits).
  Every approved screenshot is dark and the mockup says so in its first
  comment. Light is still a faithful inversion of the same tokens and still
  selectable; it is the fallback, not the design. This was caught by looking
  at the first emulator screenshot, which came up light.

**Fonts are bundled, not fetched** — Space Grotesk (display), Inter (UI),
JetBrains Mono (telemetry), all SIL OFL 1.1, licences shipped inside the APK
at `assets/fonts/OFL-*.txt` so the attribution travels with the binary.

What is in `res/font/` is not the upstream file. Each family is downloaded as
its **variable master** from google/fonts, then instanced to the weights this
app asks for and subset to Latin plus the marks the UI actually draws (`·`,
`→`, `✓`, `Δ`, `±`, `—`, the arrow/math blocks). Instancing at prep time
rather than shipping the master keeps the runtime free of `FontVariation`
questions on the minSdk-29 floor; subsetting is the difference between
~1.2 MB and **703 KB** of APK for glyphs no screen can reach. The recipe is a
committed, re-runnable script — `android/scripts/prepare_fonts.py` (needs
`pip3 install --user fonttools brotli`) — so a later weight is one edit away
rather than an archaeology exercise.

Shapes moved to the redesign's radii: cards 20 dp, tiles 14 dp, and
`Shapes.extraLarge` is a **pill**, which is what makes an un-restyled
`Button`/`FilterChip`/`SegmentedButton` round the same way a hand-built one
does.

`ui/components/ScanUi.kt` is the shared kit — hero header, back bar, card,
chip, pills, the segmented pill, the mono stat panel, the sheet rows. The
measurements the owner rounds actually argued about (48 dp View halves, 44 dp
switch rows, 40 dp chips, 28 dp slider thumbs, the 44 dp target on a
chip-sized health readout) are named constants in `ScanDims`, not literals
re-typed per screen.

### 2. Navigation: a floating capsule tab bar, and two newly top-level tabs

`ui/nav/ScanTabBar.kt` + a reworked `ui/nav/LidarScanApp.kt`.

The bar is a floating pill over the content — 16 dp side inset, 12 dp bottom,
58 dp tall, radius half its height, translucent panel ground with a hairline
border and a shadow — with an ember-washed capsule on the active tab.
`tabForRoute()` is the mockup's `TABOF` table: Review, Plan, Merge, RTK, the
calibration and connect wizards and the new-project flow all light
**Projects** while keeping their own back arrow, which is exactly what lets a
secondary screen stay a secondary screen.

**Capture and Jobs became top-level.** They used to be reachable only through
a project's detail screen. Promoting them needs one piece of state the old
graph did not have: *which* project a bare "Capture" tap means. That is
`activeProjectId`, set whenever the user opens a project, picks one, creates
one, or starts a capture, and `rememberSaveable` so it survives rotation and
back-stack churn but **not** process death — after a cold start the first
Capture tap lands on the picker rather than resuming a project the user may
not have meant.

When there is no active project, `Routes.CAPTURE_PICK` / `Routes.JOBS_PICK`
render `ui/pick/ProjectPickerScreen.kt`: the project list plus "New scan".
Both tabs are per-project underneath (the engine records into one `.lscan`;
A15's queue is scoped to a project directory), so a tab that silently picked
the newest project would eventually record into the wrong one, and one that
opened onto nothing would be a dead tab. It asks once, then the tab
remembers.

Because the bar **floats** rather than docks, every screen has to leave room
for it. The redesigned screens each reserve `ScanDims.TabBarClearance` in
their own content padding (the mockup does the same with its `.haspad` rule).
The screens this task did not restyle — the wizards, project detail,
plan/RTK/merge, new project — get it from one `UnderTabBar { }` wrapper in
the NavHost instead of a restyle each, which is the honest scope line here.

### 3. Projects: hero, cloud thumbnails, chips, mono meta

`ui/projects/ProjectsListScreen.kt`. Hero title in Space Grotesk 28 with the
mockup's aggregate line (project count · georeferenced · total points,
computed from manifests already in memory), an avatar/Settings button, cards
carrying a cloud thumbnail, the sensor/profile/`GEOREF ✓` chip row and a mono
meta line, then the ember **New scan** pill.

Delete moved from a permanently-visible trash `IconButton` in the title row
to a **long-press** — the icon was a destructive control one thumb-width from
the card's own tap target and the redesigned card has no room for it beside
the title. The confirmation dialog is unchanged, so the safety of the action
did not move; only its discoverability, which the hint under the list states
outright.

#### The thumbnail approach, and why it is not Filament

`ui/projects/CloudThumbnail.kt` draws a **Compose `Canvas` scatter**, not an
offscreen Filament render.

Filament is wired into this app through `SurfaceView` + `UiHelper` + a
`Choreographer` frame loop (B4, above). There is no offscreen/readback path,
and adding one means a second `Engine`, a render target, a pixel readback and
a bitmap cache **per visible row of a `LazyColumn`** — a large amount of GPU
machinery for a 108 dp tile that needs none of Filament's actual
capabilities (no camera control, no material, no depth), and a good way to
leak a GL context out of a scrolling list. A `Canvas` draws the same
information for a fraction of the cost.

**It still draws the project's own data.** `CaptureViewModel.stopCapture`
writes a bounded, strided XYZ sample of what the session actually recorded to
`<project>/processed/preview.f32` (`writeProjectPreview`, ≤4 000 points,
~48 KB) while the pages are still resident — anywhere else would mean
re-decoding the raw streams to draw a tile. `ProjectPreviewCache` reads it
back on an IO dispatcher into a bounded synchronized LRU (24 entries, misses
memoised so a project with no capture does not stat the filesystem on every
recomposition) and the tile projects it with a light isometric tilt, coloured
on the teal→sand height ramp `points.mat` uses, so a thumbnail and the live
viewport agree on what "high" looks like.

A project with no preview yet gets a **seeded** placeholder derived from its
id — the same project always looks like itself — drawn dimmer, and it is the
only case that draws the ember trajectory sweep. That asymmetry is
deliberate: a real preview stores points, not poses, and painting an invented
path over real data would be the one dishonest pixel on the screen.

**Not verified end to end**: the real-data branch has been exercised only in
so far as the write path compiles and runs; the emulator sessions available
here ended by the replay source running out rather than by a user-driven
stop, so every tile in the screenshots below is the placeholder. The read
path (`normalise`, the projection) is unit-shaped but has no test.

### 4. Capture: the v7 screen, and the two sheets

`ui/capture/CaptureScreen.kt` + `ui/capture/CaptureSheets.kt`.

Top to bottom: back bar → RTK chip strip → **hero viewport (~50 % of the
screen)** → four-stat mono panel → transport. Nothing else. The five-row
"AR + camera keyframes" telemetry block is **gone from the body** (review
round 3, item 4); the height it freed went to the live cloud and to the
record cluster's clearance over the tab bar.

- **Transport**: Live-SLAM switch (editable while idle, locked during a
  session, and the caption says which), a 52 dp pause circle, and the 64 dp
  ember record button. The record button is drawn, not iconified — a filled
  circle while idle, a rounded square while live — and its
  `contentDescription` ("Start recording" / "Start replay" / "Stop
  recording") is what names it for TalkBack **and** for the smoke test.
- **Capture settings sheet**, from the 48 dp Display button on the viewport:
  74 % tall so a live band of cloud stays visible above it, **View row
  pinned** between the head and the scrolling body (round 3's documented
  fallback — pin View rather than shrink targets), then AR & Camera (44 dp
  keyframes switch, 2/3/5 fps row, AR-tracking readout) and Display (colour
  mode, colormap, point size, LOD) scrolling under it. Everything live-applies
  to the view behind the sheet.
- **Diagnostics sheet**, from the health chip: 60 % tall because nothing in
  it is a target, Device and AR + camera sections, read-only, live off the
  same state the screen behind it reads. The chip's own ink is chip-sized;
  the 44 dp target comes from the `Box` it sits in, so the target grows
  without the chip inflating.
- **One sheet at a time** is structural here, not enforced: `CaptureSheet` is
  a three-valued enum, so opening one *is* closing the other and there is no
  state in which both are true. The mockup needed two flags and a
  `closeCapSheets()` to get the same guarantee.

Three changes outside `ui/` were needed to make the sheet's controls real
rather than decorative, and each is one control's worth:

1. **`core`'s `KeyframeSelector.setTargetFps`** — the 2/3/5 fps row is
   changeable *mid-session*, so `targetFps` stopped being a `val` captured at
   construction. Changing it resets the slot rather than rescaling the
   pending deadline (which was computed under the old rate and would emit one
   keyframe at neither). The already-written count is untouched.
2. **`KeyframeRecorder.setEnabled`** — gates `onFrame` rather than detaching
   the listener, because the ARCore callback is shared with the pose pump and
   because round 3 is explicit that the count must **freeze** where it stood,
   not reset or be rewritten. Turning it back on resets the selector's slot.
3. **`CaptureViewModel.displayParams`** — the LOD slider needs a live path
   into the renderer and `lodPointBudget` only travels inside
   `DisplayParams`, so the four display controls are now combined into one
   block and the viewport uses `PointCloudView(displayParams = …)`. That path
   already existed for Review; Capture just started using it.

One honest departure from the mockup: it labels the LOD slider `2 – 20` and
reads it out as a **percentage**. What this renderer implements is
`DisplayParams.lodPointBudget`, a page-admission ceiling in points, not
per-page decimation — so the control keeps the mockup's range and its
"sparse → every return" caption but reads out in **M points**, which is the
number the renderer obeys. A percentage would have been a nicer-looking lie.

A pre-existing bug was fixed on the way, in the code whose own KDoc described
it: `startArPipelines` wrote `recorder::onFrame` twice — once into
`keyframeFrameListener`, once into `addFrameListener` — producing two
distinct objects, so `detachKeyframeListener` removed neither. It is one
reference now.

### 5. Jobs, Review, Settings

- **Jobs** (`ui/processing/ProcessingScreen.kt`): "Processing" hero, the
  This phone / Cloud / Bundle segmented pill, and icon-tile job cards with an
  ember progress bar. Same three modes, same gates with the same refusal
  reasons (a refused action still prints *why* instead of showing a dead
  button — B6's rule, kept), same queue with cancel.
- **Review** (`ui/review/ReviewScreen.kt`): hero viewport,
  HEIGHT/RGB/INTENSITY/TIME chips, Floor plan / Export split, Export now
  landing on this project's Jobs queue. **The measure UX is untouched** — the
  same transparent tap-catcher over the `SurfaceView` so a pick never fights
  the orbit gesture, the same "nearest sampled point" caveat, the same unit
  chips and Clear. The full §3.9 display panel is still one tap away behind
  the Tune action; the four chips are a shortcut for the one setting people
  change constantly, not a replacement for it.
- **Settings** (`ui/settings/SettingsScreen.kt`): restyled, nothing removed.
  Units, theme, cloud server + token, the poor-clock-sync colorize policy,
  the simulated-engine developer switch, the "Replay synthetic capture"
  acceptance path (still tagged `replaySyntheticCaptureButton`), the
  read-only workflow-profile reference and the storage location are all
  still there and still do the same thing.

### 6. What the emulator caught that review did not

Both were found by looking at a screenshot, not by reading the code:

- **A `fillMaxSize()` child of a `Column` takes the full incoming height, not
  the height left over.** Capture's and Projects' content columns did exactly
  that, so the transport row and the last card sat under the floating tab
  bar. Both are `Box(Modifier.weight(1f))` now, and the redesigned roots also
  carry `navigationBarsPadding()` so the clearance is measured from above the
  gesture bar rather than from the screen edge.
- **M3's default `Slider` track is sized for M3's own 4 dp thumb.** Under the
  redesign's 28 dp thumb its thumb-track gap read as a stub of active track
  to the left of the knob and its stop indicator as a stray dot on the right.
  `SheetSlider` now passes an explicit track with `thumbTrackGapSize = 0.dp`
  and `drawStopIndicator = null`. A disabled-but-checked `Switch` had the
  related problem — M3 greys both states to nearly the same thing, so a row
  that is *on and merely unavailable* read as off; explicit
  `disabledChecked*` colours fix it.

### 7. Test hooks the redesign moved (and why)

`app/src/androidTest/.../ReplayCaptureSmokeTest.kt` still tests the same two
things; two hooks had to move because the UI they pointed at genuinely
changed shape.

- `onNodeWithContentDescription("Settings")` still works and is still
  unambiguous — but only because the tab bar's Settings tab deliberately
  gives its icon `contentDescription = null` and lets its visible label be
  the accessible name. Two nodes with that description would have made the
  existing assertion throw on multiplicity. The test also now waits on
  `testTag("projectsAvatar")` so a re-wording cannot silently break it, and
  asserts `testTag("scanTabBar")` — if the bar failed to compose, every route
  is unreachable by touch.
- The old `onNodeWithText("Start replay", substring = true)` cannot match a
  64 dp circle with no text. The hook is the record button's
  `contentDescription`, which is state-dependent, so waiting for it is still
  waiting for CONNECTED + IDLE exactly as the text match was.
- `pointsCapturedValue` still tags the POINTS cell. The stat panel prints
  `1.24 M` above a million and a plain grouped integer below it
  (`CaptureScreen.formatPoints`), specifically so the growth assertion keeps
  seeing motion at the bundled synthetic capture's tens-of-thousands scale —
  `0.03 M` would be flat for the whole 10 s window and the test would be
  measuring the formatter. The test's parser handles both forms anyway.

### 8. Verification (what actually ran, on what)

Toolchain as elsewhere in this file: JDK 17 (`openjdk@17`), Android SDK at
`/opt/homebrew/share/android-commandlinetools`, NDK r27d, Gradle 8.14.5 via
the wrapper.

```
$ ./gradlew :core:test :app:assembleDebug :app:connectedDebugAndroidTest
...
> Task :app:connectedDebugAndroidTest
Starting 2 tests on b4_test(AVD) - 14
Finished 2 tests on b4_test(AVD) - 14
BUILD SUCCESSFUL
```

- **`:core:test` — 194 tests, 0 failures** (5 skipped, as before). The
  `KeyframeSelector` change is covered by the existing `RigMotionTest` suite,
  which still passes; no new JVM test was added for it, which is a gap worth
  naming rather than a claim of coverage.
- **`:app:assembleDebug` — succeeds**, native build included (unchanged).
- **`:app:connectedDebugAndroidTest` — 2/2 green on a booted `b4_test` AVD**
  (API 34, `google_apis`, arm64-v8a, HVF-accelerated), i.e. the tab-bar
  navigation rework does not break the smoke test's routes.
- **APK size**: 95,597,986 → **97,243,458 bytes** debug, **+1.57 MB
  (+1.7 %)**. 703 KB of that is the eight subset font files; the rest is the
  new Compose code plus the extra `material-icons-extended` vectors the tab
  bar and the job tiles reference.
- **Fonts confirmed in the APK** with `unzip -l`: `res/font/*.ttf` (8 files)
  and `assets/fonts/OFL-*.txt` (3 files).

**Screenshots** — `android/evidence/redesign/`, captured with
`adb exec-out screencap` on the same booted AVD and downscaled to 640 px
wide: Projects, Capture recording, the Capture-settings sheet, Diagnostics,
the Capture tab's project picker, Jobs, Settings, Review.

### 9. Deferred / not verified

- **The thumbnail's real-data branch has never been seen rendering real
  points.** The write happens in `stopCapture`; the emulator's replay
  sessions ended by the source running out rather than by a user stop, so
  every tile in the evidence is the seeded placeholder. Needs one
  hardware (or user-stopped replay) session to confirm.
- **The AR paths are all unexercised** — this AVD is `google_apis`, not
  Play, so ARCore is unavailable and `arAvailable` is false throughout. The
  sheet's AR & Camera section, the AR-overlay view mode, the KF chip's tick
  rate, the TRACKING/LIMITED readout and `KeyframeRecorder.setEnabled` /
  `setTargetFps` are all compiled and reasoned through, none of them run
  against a real ARCore session.
- **Landscape** is implemented (the viewport takes the left, the stats and
  transport a 340 dp right column) but was only compiled, not seen.
- **The RTK chip strip** shows the honest no-rover state on this emulator;
  the fixed/float colouring has not been seen with a live rover.
- **The screens the redesign did not restyle** — project detail, new
  project, plan, RTK, merge, connect wizard, mount calibration — pick up the
  new palette, type and shapes through `MaterialTheme` and get tab-bar
  clearance from `UnderTabBar`, but keep their B1–B12 `Scaffold`/`TopAppBar`
  chrome. That was the scope line; restyling them is the obvious follow-up.
- **No screenshot test / Paparazzi-style golden** was added. The mockup's own
  110-item checklist has no counterpart here, and the two-test emulator smoke
  suite is the only automated UI gate.

## AUTO-DETECT — Mid-360 heartbeat + D6 signature auto-detect

Owner task: add AUTO-DETECT to the device connect wizards. Scope was
strictly `android/**` — `engine/**` was read-only, and a concurrent agent
was landing engine discovery + C ABI v6 (`scan_discover_mid360`,
`scan_host_check`, `scan_probe_d6`, `scan_enumerate_serial`) in that
directory at the same time.

### 0. ABI pin, and why this stayed on the Kotlin path even after v6 landed

`SCAN_ABI_VERSION` in `engine/capi/scanengine_c.h` was **5** with none of
`scan_discover_mid360`/`scan_host_check`/`scan_probe_d6`/
`scan_enumerate_serial` present at the moment this task started (checked
first, before writing anything). Per the brief's instruction to pin to the
ABI as it stood at task start rather than race a concurrent edit, this task
built both auto-detect paths in pure Kotlin:

- **Mid-360**: a plain `DatagramSocket` heartbeat listener
  (`UdpMid360Detector`) — this is the brief's stated *permanent* choice for
  Mid-360 regardless of ABI ("Kotlin fallback is fine permanently for
  Mid-360 (pure UDP)"), not a stopgap.
- **D6**: a CH340-first-open + `AA 55`-signature read over the existing
  usb-serial stack (`D6AutoProbe`), since B2's connect flow had no existing
  auto-probe to reuse (checked this file first — B2's wizard is
  tap-a-device-to-connect only).

By the time `:app:assembleDebug` actually ran (after both this task's edits
and the concurrent engine work were done), `SCAN_ABI_VERSION` had moved to
**6** and `engine/src/discovery/serial_enum.cpp` etc. exist. **None of it is
used here** — this task never edited `engine/**` or the JNI shim
(`android/app/src/main/cpp/*.cpp`) to add new native entry points for the
v6 discovery calls, by design (pinned scope, not an oversight). One
transient build failure was observed and self-resolved: mid-way through,
`engine/capi/scanengine_c.h`'s `SCAN_ABI_VERSION` and
`engine/include/scanengine/core/engine.h`'s `kEngineAbiVersion` briefly
disagreed (6 vs 5) while the other agent's edit was in flight — a plain
`static_assert` failure in `scanengine_c.cpp`, nothing to do with anything
in `android/**`. Waiting for the concurrent edit to finish (polling the two
constants until they agreed again) and re-running `:app:assembleDebug` was
enough; no `engine/**` file was touched to work around it.

**Follow-up worth sizing separately**: `scan_discover_mid360`/
`scan_probe_d6` now exist and would let both detectors move behind the JNI
boundary instead of a raw socket / a speculative serial open. Both this
task's seams — `com.lidarscan.core.net.Mid360Detector` and
`com.lidarscan.app.usb.D6AutoProbe`'s `D6AutoProbeResult` — are narrow
enough that a JNI-backed implementation could be swapped in later without
touching either wizard's UI or state-machine code; the D6 wizard is D6-only
already, but per the brief's guidance the Mid-360 path should stay on the
Kotlin listener permanently even if that follow-up happens.

### 1. Mid-360: the heartbeat, decoded from real hardware capture bytes

The Mid-360 broadcasts a UDP "push message" (SDK2's `cmd_id` `0x0102`,
`kCommandIDLidarPushMsg`) once a second from its own port 56200 to the LAN
broadcast address, port **56201** (`kMid360HostPushMsgPort`) — receive-only,
no handshake, works on a completely unconfigured device. The wire format
was reverse-engineered against **real captured bytes**, not written from
the SDK header alone:

- `captures/mid360_real_30s.livoxdump` (a field-recorded 30 s capture, see
  `captures/FIELD_SESSION_2026-08-17.md`) was parsed with the project's own
  `.livoxdump` reader (`spikes/s2-mid360-sim/scripts/livoxdump.py`'s
  documented container format) to pull the 30 raw records on port-table
  index 3 (port 56201).
- The vendored `Livox-SDK2` source
  (`spikes/s2-mid360-sim/third_party/Livox-SDK2/`) gives the general
  command-frame header (`sdk_core/comm/sdk_protocol.h`'s `SdkPacket`, 24
  fixed bytes: `sof,version,length,seq_num,cmd_id,cmd_type,sender_type,
  rsvd[6],crc16,crc32`) and the TLV entry shape
  (`LivoxLidarKeyValueParam`: `u16 key, u16 length, value[length]`,
  `include/livox_lidar_def.h`) — confirmed against the real bytes: `length`
  (offset 2, LE) equals the UDP payload's own size and `cmd_id` (offset 8,
  LE) is `0x0102` on every one of the 30 real records.
- **The first TLV tag does not sit where the generic SDK push-message path
  (`command_handler/parse_lidar_state_info.cpp`'s `key_num`+reserved
  4-byte prefix) would put it** — computed that lands at offset 28; every
  real record actually has it at offset 38. Rather than hard-code either
  number, `Mid360HeartbeatParser` locates the TLV list by scanning a
  bounded window for the one entry that's cheap to recognise unambiguously
  — `key=0x0004` (`kKeyLidarIpCfg`), `length=12` — matching the brief's own
  hint ("binary IPv4 fields — lidar IP first w/ ff ff ff 00 mask after
  it"). This is robust to the exact header-size question being unresolved
  (there may be a per-lidar addressing field the static analysis missed;
  it doesn't matter which, since the parser never assumes the offset).
- Fields pulled from the TLV list: `0x0004` → lidar IP/netmask/gateway,
  `0x0006`/`0x0007` (`kKeyLidarPointDataHostIpCfg`/`kKeyLidarImuHostIpCfg`)
  → **the persisted host IP** (`192.168.1.5` in the real capture — matches
  `FIELD_SESSION_2026-08-17.md`'s own note about the required host alias),
  `0x8000` (`kKeySn`) → serial number (real bytes are a 2-letter prefix +
  serial, `"AR" + "MCP7K0034759"`, stripped by a regex), `0x8001`
  (`kKeyProductInfo`) → the printable `"DevType:Mid-360 FmType:App
  FmVer:35010108 BuildTime:..."` string.
- Two real payloads (byte-for-byte, not synthesized) live in
  `core/src/test/resources/mid360_heartbeat/` (for the fast JVM test) and
  `app/src/androidTest/assets/mid360_heartbeat/` (for the instrumented
  counterpart, exercising the real `AssetManager`/on-device byte path) —
  both extracted from the same two capture records with a one-off Python
  script (not committed; the two 430-byte `.bin` files are the artifact).

`Mid360HeartbeatParser`, `Mid360Heartbeat`, `Mid360Detector` (the seam),
`Mid360DetectionResult` and `withDetectedHeartbeat` live in `:core`
(`com.lidarscan.core.net`, `Mid360Heartbeat.kt`) — plain Kotlin, no Android
dependency, matching the module's existing convention. The real detector,
`UdpMid360Detector` (`:app`, `com.lidarscan.app.net`), is exactly what the
brief asked for: **a plain `DatagramSocket` bound `0.0.0.0:56201`**
(`broadcast = true`, polled with a short `soTimeout` so cancellation and a
`~5 s` overall deadline both work), optionally `Network.bindSocket`-bound to
the Ethernet link when `EthernetMonitor` already has one (degrades cleanly
to an unbound socket otherwise — auto-detect must not go permanently dark
just because the adapter attached after the detector was constructed).

`Mid360AutoDetectController` (`:core`) is the wizard-state machine —
`IDLE → LISTENING → FOUND/TIMED_OUT/ERROR` — same split as B2's
`D6ConnectController`: no Android dependency, JVM-tested against a fake
`Mid360Detector` (`Mid360AutoDetectControllerTest`, 9 cases: idle start,
found-with-matching-host, found-with-mismatched-host, no-local-address
edge case, timeout, detector error, cancel-while-listening, reset, and
re-starting cancelling an in-flight attempt).

### 2. Mid-360 wizard UX

`Mid360ConnectScreen`'s first card is now **`AutoDetectCard`** — "Auto-detect"
is the primary button, "Enter manually" the escape hatch — and the
pre-existing Interface/Static-IP/Addresses/self-test cards (all B3, unchanged
otherwise) are gated behind `showManualEntry`, which flips true the moment
either button is used or an auto-detect attempt resolves (found, timed out,
or errored — all three still want the form visible for review/editing).

- **On found**: shows `"Found Mid-360 SN <sn> · fw <fw> at <ip>"`, prefills
  `lidarIp` from the beacon, and prefills `hostIp` to the beacon's
  **persisted host** (`Mid360Settings.withDetectedHeartbeat`) — which
  doubles as the mismatch signal, because it feeds straight into the
  pre-existing `validateMid360Settings` (the AddressesCard already knows
  how to say "this phone's Ethernet interface holds X, the host IP must be
  one of them").
- **Host comparison**: `Mid360AutoDetectController` compares the beacon's
  persisted host against `EthernetMonitor`'s current addresses at the
  moment of *finding*, not when auto-detect started (the adapter can pick
  up an address mid-listen). Match → a plain "ready to run the self-test
  below" line. Mismatch → `StaticIpGuidance.steps()` gained an optional
  `targetHostIp` parameter; when set, the OEM guidance's last line becomes
  "Set this phone's static IP to `<persistedHostIp>`/24 — the exact address
  the Mid-360 is already configured to stream to" instead of the generic
  per-OEM phrasing that never named a value. `StaticIpCard` threads
  `state.autoDetectedHostIp` into it once a heartbeat has been seen.
- **Manual entry** is unchanged underneath — same fields, same self-test,
  same per-project save.

### 3. Capture defaults — last-detected Mid-360 addresses (DataStore)

`AppSettings` (`app/.../data/SettingsModels.kt`) gained
`lastDetectedMid360LidarIp`/`HostIp`/`SerialNumber` (device-level, not
per-project — same scope as `ntrip`/`useFakeEngine`), persisted by
`SettingsRepository.setLastDetectedMid360`, called the moment
`Mid360ConnectViewModel` sees a `FOUND` auto-detect result (never from a
manually-typed address — only a heartbeat that was actually decoded).
`Mid360ConnectViewModel`'s init now prefers these over the bare
`Mid360Settings()` `192.168.1.100`/`192.168.1.5` constants whenever no
per-project `manifest.json` value exists yet, before applying the existing
Ethernet-interface-address override. A device that's been auto-detected
once opens the wizard next time already pointed at it.

### 4. D6 wizard: attach-time auto-probe

`D6SignatureScanner` (`:core`, pure byte-array scan, 8 JVM tests) checks a
freshly-read serial chunk for the D6's `AA 55` frame preamble (the same
marker `D6SerialConnection`'s write-side doc already names for the D6's own
start/stop command bytes), with a `carry`-byte parameter so a preamble
split exactly across two USB reads is still caught. This — not a blind
"any CH340 device is a D6" — matters because `NOTES.md`'s own §7 GNSS gap
note already flags the ambiguity: a Unicore UM982 eval board enumerates as
the *same* CH340 VID/PID class this app already declares for the D6.

`D6AutoProbe` (`:app`, `com.lidarscan.app.usb`) does the orchestration:
requests permission (if needed), opens the port, reads for a 1.5 s window
(several D6 revolutions at the ~10 Hz rate `FIELD_SESSION_2026-08-17.md`
recorded), and returns `Identified`/`NotIdentified`/`PermissionDenied`/
`Error`. On `Identified` the already-open connection is handed straight to
`D6ConnectController` (`connectAlreadyOpen` — no second `registry.open`,
matching `D6SerialConnection.startReading`'s documented "safe to swap the
callback later" contract, which is exactly what the engine's own
`conn.startReading{...}` call does next). On `NotIdentified` the
speculatively-opened connection is closed again, so a UM982 (or anything
else) sitting on the wire isn't left held open on a guess.

`ConnectWizardViewModel.refreshDevices()` now also drives
`maybeAutoProbe()`: exactly one attached serial device and the wizard still
at `NoDevice` → probe it; more than one device is left alone (ambiguous —
a rig with both a D6 and a UM982 attached is exactly the case above) with
the manual list and per-device "Connect" still there. The screen shows a
small "Checking device signature…" spinner in place of that one device's
Connect button while its probe is in flight (`autoProbingDevicePath`); on
identification the wizard jumps straight from the device list to the
health panel, now labelled **"COIN-D6 detected — `<devicePath>`"**. Manual
tap-to-connect is untouched and still works at any point, including for a
second device sitting in the same list.

### 5. RTK screen — cosmetic 230400 note

Android's RTK rover link is Bluetooth SPP only (`RtkRoverConnection`) —
there is no serial baud setting on this screen to be wrong, so no functional
change was needed or made. Added one line of copy to the rover card noting
that a *wired* UM982 defaults to 115200 but is field-verified 230400-capable
(`captures/FIELD_SESSION_2026-08-17.md`), and pointing at §7's existing
USB-serial-GNSS gap note — the previous copy said nothing about baud at all,
which read as silence rather than as "not applicable here."

### 6. Tests + verification

```
$ ./gradlew :core:test :app:assembleDebug
...
BUILD SUCCESSFUL
$ ./gradlew :app:connectedDebugAndroidTest   # b4_test AVD, booted -no-window
...
Starting 3 tests on b4_test(AVD) - 14
Finished 3 tests on b4_test(AVD) - 14
BUILD SUCCESSFUL
```

- **`:core:test` — 218 tests, 0 failures** (5 skipped, pre-existing and
  unrelated). New: `Mid360HeartbeatParserTest` (7 — includes the two real
  fixtures decoding to the exact field-session values: SN `MCP7K0034759`,
  lidar IP `192.168.1.159`, persisted host `192.168.1.5`, plus
  malformed-payload rejection cases), `Mid360AutoDetectControllerTest` (9),
  `D6SignatureScannerTest` (8).
- **`:app:assembleDebug` — succeeds**, native build included (this pulled
  in the concurrent agent's `engine/src/discovery/*` alongside the
  unrelated JNI shim this task already had — see §0 above for the one
  transient ABI-mismatch failure and how it resolved without touching
  `engine/**`).
- **`:app:connectedDebugAndroidTest` — 3/3 green on `b4_test`** (API 34,
  `google_apis`, arm64-v8a): the pre-existing `ReplayCaptureSmokeTest`'s 2
  cases (unaffected — no route/tag in this task's diff touches the
  Capture/Projects flow) plus the new
  `Mid360HeartbeatFixtureAndroidTest`, which reads the same two real
  payloads through Android's actual `AssetManager` off
  `app/src/androidTest/assets/mid360_heartbeat/`. That test's first run
  failed with `FileNotFoundException` — `ApplicationProvider
  .getApplicationContext()` returns the **target app's** Context
  (`com.lidarscan.app`), and `src/androidTest/assets/` packages into the
  **test APK** (`com.lidarscan.app.test`); the fix was
  `InstrumentationRegistry.getInstrumentation().context` (the test
  package's own Context), left as a doc comment on the test so the next
  androidTest asset doesn't repeat it.
- **Not verified against real hardware in this task**: no D6/Mid-360 device
  was attached to this environment, so `D6AutoProbe` and
  `UdpMid360Detector` are verified by construction/unit test (`AA 55`
  scanner, heartbeat parser against real captured bytes, wizard state
  machine against fakes) but the actual `DatagramSocket.receive()`/
  `UsbSerialPort.read()` runtime paths were not exercised live. That is the
  same posture the rest of this file records for B2/B3's own USB/Ethernet
  code.

## ROUND 5 REDESIGN — one Capture tab, a phone-tracked 3D D6, and no wizards

Owner review rounds 5, 5.1, 5.2 and 5.3 (`docs/design/REVIEW_FEEDBACK.md` items
7–11 plus the three mockup-review additions and contract items 17–18), scoped to
`android/**`. `engine/**` was read-only for this task; every place the C ABI did
not reach is recorded in §9 rather than worked around silently.

### 1. What the app looks like now

**Projects tab = list + preview.** Tapping a card no longer navigates — it
*selects*, and the card expands in place: the same `ProjectThumbnail` at 260 dp
instead of 108, with the chips and the mono meta line under it. Two quiet doors
live inside the selected card and nowhere else (`Open in viewer` → Review,
`Details, jobs & export` → the detail screen, which still owns processing,
export, mount calibration, RTK and merge). The **New scan** pill is gone; the
empty state points at the Capture tab instead, which is the one exception
(a tab whose only message is "go elsewhere" with no way to go there is a dead
end, so the empty state carries a `Go to Capture` shortcut).

**Capture tab = creating new scans, and nothing else.** One route
(`Routes.CAPTURE_NEW`, no project id) and one screen:

```
  app bar (New scan · profile · state)      ← no project yet
  georeference + RTK chips                  ← round 5.2
  ── pre-capture strip (collapses at record) ─────────────────
  scan name  [ Scan-014-2026-08-17-1932 ]   ← placeholder = the auto name
  auto-detect status line + Retry / Enter manually
  inline manual panel (opens ITSELF when nothing is found)
  D6 mount hint + tracking chip
  ── always ──────────────────────────────────────────────────
  live viewport (points before recording), trail inset, chips
  four mono stats
  Live-view switch · pause · Start new scan
```

Removed outright, because each was a step: the **new-project screen**
(`ui/newproject/**` — name/sensor/profile), the **capture project picker**
(`PickPurpose.CAPTURE`), the **D6 connect wizard** (`ui/connect/ConnectWizard*`),
the project-scoped `project/{id}/capture` route and the project-less Mid-360
wizard route. The Mid-360 wizard survives per project (it is the only place
addresses can be saved into a manifest); `MountCalibration` survives and is now
reached from the capture screen's own "Calibrate mount" link when a D6 is running
on the CAD nominal.

**The flow, end to end.** Open Capture → `CaptureAutoConnectController` races a
D6 USB signature probe against a Mid-360 heartbeat listen → first to answer wins
→ `engineBridge.connect` → **live preview streaming, not recording** → adjust any
display parameter against real points → **Start creates the project** and begins
recording into it. No self-test gate anywhere: round 5 item 7 is explicit that
live points are the proof, so the wizard's health panel became the viewport.

### 2. Auto-naming and the project store

`ScanAutoName` (`:core`) formats `Scan-<series>-<yyyy-MM-dd>-<HHmm>` —
`Scan-014-2026-08-17-1932` for the owner's own example. Filesystem-safe on
purpose: the name is slugified into a directory by `FileProjectStore` *and*
travels on an exported `.lscan` to a Windows desktop, where `:` is illegal, so it
is never typed in the first place. The series counter is device-level and
**monotonic**, persisted in DataStore (`SettingsRepository.nextScanSeries()`,
read-modify-write inside one `edit {}` so a double-tapped Start cannot spend the
same number twice) — deliberately not "number of projects on disk", because
deleting a project must not make the next scan re-use its name.

Projects created by Start go through `container.projectStore` — the same
`FileProjectStore` instance the Projects tab lists, so an auto-created scan
appears there with no plumbing of its own.

### 3. D6 = a 3D scanner via the phone — and the bug that made it 2D

Round 5 item 11 is the substantive one. Three things were wrong or missing:

**(a) Poses were only pumped in AR view.** `CaptureArController.onFrame()` — the
one call that runs `Session.update()`, publishes the pose and calls
`scan_engine_push_pose` — is driven exclusively by
`ArCameraBackgroundRenderer.onDrawFrame`, which only exists while `ArOverlayView`
is composed, i.e. only in `CameraMode.AR`. In the **default 3D-orbit view** the
ARCore session was created and resumed and then *nothing ever updated it*: zero
poses pushed, zero keyframes (B8's recorder is fed from the same frame listener),
and a pushbroom with no trajectory to interpolate against. The AR overlay was a
*view* option and the entire pose pipeline was silently riding on it. Fixed with
`ArPosePumpView`: a 2 dp `GLSurfaceView` running the same
`ArCameraBackgroundRenderer`, mounted whenever poses are required and the AR
overlay is **not** the renderer on screen (never both — two pumps would call
`Session.update()` from two threads). Why 2 dp of real surface rather than a
headless EGL pbuffer context: `Session.update()` needs a GL context and a camera
texture, and a hand-rolled EGL context is exactly the code that works on one
driver and not another — **no ARCore device was available to this task**, so this
reuses the arrangement B7 already shipped. One call site; swapping in an offscreen
pump later touches nothing else.

**(b) No calibration meant no pushbroom.** `startArPipelines` only called
`pushbroom_enable` when a measured `MountCalibration` existed, so a fresh phone
recorded fan slices plus poses that nothing ever combined. Now a D6 session with
no measured calibration applies **`BracketNominals.cadNominal(COIN_D6)`** — the
CAD extrinsic for exactly the owner's mount (scanner on the phone's back, scan
plane vertical) — and `mountIsNominal` says so inline with a `Calibrate mount`
link. A nominal extrinsic costs millimetres of registration; not enabling the
pushbroom costs the third dimension. `stopCapture`'s flush was also gated on the
*calibration* being non-null and is now gated on `pushbroomEnabled`, or every
nominal-extrinsic session would have skipped the flush that resolves its points.

**(c) The copy called it a 2D-ish device.** Diagnostics printed the sensor badge
in the IMU row (reading as "the D6 has an IMU called COIN-D6"); it now reads
`none on device · phone IMU via ARCore`. The detection line says `3D scan ·
phone-tracked (ARCore VIO supplies the pose)`, and the capture screen carries the
mount hint ("D6 flat on the BACK of the phone, scan fan VERTICAL, walk forward").
No "2D" anywhere.

**What a phone D6 capture writes into the `.lscan` now:** D6 packets (engine,
record-always), the ARCore pose stream (`scan_engine_push_pose` at camera rate,
with `tracking_lost` and per-pose sigma), B8 camera keyframes + `frames.idx` when
keyframes are on, the pushbroom's resolved 3D points (nominal or measured
extrinsic), plus round 5.2's GNSS epochs. Tracking quality is inline on the
viewport (`3D TRACKING` / `TRACKING…` / `TRACKING LOST` / `NO TRACKING`) because
for this sensor it *is* the state of the third dimension, and the numbers (poses
pushed, extrinsic provenance, georef source) are three new Diagnostics rows.

### 4. Round 5.1 — the manual fallback, point size, persistence

* **Auto-detect → manual, automatically.** `CaptureAutoConnectState.manualEntryOpen`
  is set by the controller itself when nothing is found, when a connect fails, or
  when the transport drops mid-preview: the fields are already open with the
  last-known addresses by the time the operator looks up. `Enter manually` stays
  reachable at every phase, including a successful detect (a rig with two devices
  can auto-detect the wrong one). The panel is inline, capped at 260 dp and
  scrolling — never a dialog.
* **Point size is 0.1 – 3.0 px in 0.1 steps** everywhere it is set
  (`DisplayLimits`, snapped in the ViewModel so the slider, the read-out and the
  manifest cannot disagree). This required relaxing the Kotlin `clamped()` floor
  from A14's 0.5 px — see §9.
* **Preview settings carry into the recording and into the project.** The sheet is
  ViewModel-scoped, so nothing resets at Start; `displayParams` is written into
  the manifest at creation *and* at stop, so the scan re-opens in Review framed
  the way it was recorded.

### 5. Round 5.2 — phone-location georeferencing when there is no rover

`GeorefSourcePolicy` (`:core`, 11 tests) ranks strictly: a rover fix always wins,
otherwise the phone's own location, otherwise nothing. No blending — centimetres
and metres averaged together describe neither — and no hand-over state machine: a
rover that connects mid-session simply wins the next evaluation and the chip
upgrades from `PHONE GPS ±4.2 m` to `RTK FIXED ±2 cm`.

`PhoneLocationSource` uses the **platform** `LocationManager`
(`FUSED_PROVIDER` on API 31+, else GPS — never `NETWORK_PROVIDER`, which is
hundreds of metres and worse than no georeference) because
`play-services-location` is not a dependency of this project and the build runs
offline. Every fix carries `Location.getAccuracy()` verbatim as 1-sigma; a fix
with no accuracy is **dropped** rather than emitted with a zero, because zero
sigma reads downstream as "perfect".

Fixes reach the engine as **synthesized NMEA** (`PhoneFixNmea`: GGA → GST → RMC,
one shared UTC, 12 tests) pushed through `scan_engine_push_nmea`. That is not a
workaround — it is the only GNSS *ingest* call the C ABI has (§9.1) — and it is
the better door: the bytes land in the `.lscan` as `kGnssNmea` chunks under
record-always, A4 time-syncs them from `Location.elapsedRealtimeNanos`
(CLOCK_BOOTTIME, already the engine's domain), and `getAccuracy()` travels in the
GST fields A10's fusion actually weights by. GGA quality is `1` (single), never
4/5; geoid separation and unknown vertical sigma are left **empty** rather than
zeroed.

Permission: `ACCESS_FINE_LOCATION` is requested **at Start, once, and only when no
rover fix is present** — never on app launch, never on opening the tab. Denial
records a flag, shows one quiet inline line, and the capture proceeds in its own
local frame.

### 6. Round 5.3 — refresh ceiling, the crash path, walkthrough-first

* **The refresh control's max is the hardware's.** `RefreshGovernor.optionsFor()`
  builds the option list from `Display.getRefreshRate()`, so a 60 Hz phone never
  sees 120. When measured intervals between *rendered* frames sustain a >1.35×
  overrun for 2 s, the governor eases the live view **one notch** down the ladder
  (120/90/60/45/30/20/15/10), publishes an inline note, and never raises the cap
  on its own (an oscillating renderer measures nothing). Recording is untouched —
  the note says so in words.
* **Bounded uploads.** `syncPointCloud` previously re-uploaded every grown page in
  one frame; a burst (a replay decoding ahead of the display, a stream-filter flip
  admitting a backlog) queued an unbounded number of `setBufferAt` calls into a
  single Filament frame. Now capped at **4 MB of vertices and 24 new pages per
  frame**, with the remainder resuming next frame from the same offset, and
  `setGeometryAt` bound to `gpu.uploaded` rather than `page.count` so points that
  have not been uploaded yet are never drawn from uninitialised GPU memory.
* **Walkthrough-first.** `keepScreenOn` for the duration of a recording (scoped to
  RECORDING/PAUSED, not to the screen); Stop grows 64 → 76 dp and pause 52 → 64 dp
  while live; a **trajectory trail** (`TrajectoryTrail` in `:core`, 11 tests;
  `TrajectoryTrailRecorder` in `:app`) drawn as a small top-down inset from the
  ARCore pose stream during preview *and* capture, faint where tracking was poor
  (those are the stretches the pushbroom excludes); and a gentle inline
  "moving/turning too fast" hint derived from ARCore's `EXCESSIVE_MOTION` and B8's
  motion-gate skip counter — the *hint* is inline, the *numbers* stay in
  Diagnostics, per round 3.

### 7. One UX bug the emulator caught

Entering the Capture tab used to fire the **system camera-permission dialog**
immediately (B7's unconditional `LaunchedEffect`), which pauses the Activity —
the smoke test found it as "No compose hierarchies found" while trying to walk the
tab. Beyond breaking the test it is a modal interruption in front of the screen
whose whole point is that it has no steps, and on a Mid-360 walk the camera may
never be needed. The ARCore session is now created (and the permission asked for)
only when a phone-tracked D6 is previewing, the AR view is selected, or a
recording is running.

A second layout bug from the same session: with the manual panel open, the
pre-capture strip squeezed the live viewport to a sliver — the one thing round 5
says must always be on screen. The strip is now capped at 46 % of the screen and
scrolls internally.

### 7b. THE SECOND VANISHING ARTIFACT — export went nowhere

Owner, same week: *"I exported scan-008 (sealed OK, 216,653 pts, listable) and
the file is nowhere — not in Downloads, no error."*

**The owner's next attempt named the actual wall:** the app answered
**"No cloud to export."** So the story is worse than "the file went somewhere
unbrowsable" — for a phone-D6 project there was, in practice, *no local export
at all*:

* **Export (PLY/LAS/PCD)** is gated on `hasProcessedCloud || hasLiveCloud`. On a
  D6 project both are false after the capture session ends (the pushbroom cloud
  lives only in the live `PageStore`, §6) and Post-process is refused (§6 again),
  so that gate can never open. Its refusal text mentioned the cloud and read as
  *"you need a server"*.
* **Extract for transfer** — the one path that always worked, needs no server,
  and produces exactly the `.lscan.zip` the desktop imports — was called
  "Extract for transfer", sat third in a row next to "Cloud", and its only
  delivery route was a share sheet.

**Fixed by making the local path the primary one and naming it in plain words:**

* The mode chooser now reads **"Save to phone" / "Send to cloud" / "Process
  here"**. `EXTRACT_FOR_TRANSFER`'s own summary says it: *"no server, no
  account, nothing to configure. The desktop app imports that zip directly.
  This is the way to get a scan off the phone."*
* **A COIN-D6 project opens on "Save to phone"** — landing the operator on a
  mode that cannot run, and then on a refusal that mentions the cloud, is
  exactly how `scan-008` got stuck. An explicit mode pick sticks.
* The card's primary button is **"Save to Downloads"**; "Save + share…" is the
  secondary. A share sheet is an extra, never the delivery mechanism.
* The point-cloud Export refusal now names the open door instead of being a dead
  end: *"No point cloud in memory to convert to PLY/LAS/PCD… To get this scan
  off the phone right now, use 'Save to phone'."*

**And a second bug found while wiring it: the bundle used to swallow itself.**
`zip_export()` walks the project directory recursively and takes **every**
regular file (`src/record/zip.cpp` — no filter, which is right for a
package-everything bundle). The app staged its `.lscan.zip` in
`<project>/exports/`, i.e. *inside* the directory being zipped — so a second
export bundled the first, a third bundled both, and a 200 MB capture exported
three times ships nested copies of itself. Bundles now stage in the app's cache
directory, outside the project; the deliverable is the Downloads copy.
`engine/tests/test_jobs.cpp` pins both facts —
`round7_a_bundle_carries_the_apps_own_project_json` (the ROUND 6 `project.json`
and the camera frames survive `zip_export → zip_import → import_and_validate`
with `sane() == true`, so a bundle does not arrive on the desktop nameless) and
`round7_a_bundle_written_inside_the_project_swallows_itself`, which demonstrates
the nesting and says in its comment why the staging directory moved.

**And where the bytes went even when it did run.** Every export this app
produced was written to `<project>.lscan/exports/<name>`, i.e. under
`/storage/emulated/0/Android/data/com.lidarscan.app.debug/files/…`. That is
app-specific external storage, and **since Android 11 the system Files app
refuses to browse `Android/data/` at all.** The bytes were exactly where the app
said they were and no human with a file manager could reach them. The
`.lscan.zip` path then opened a share sheet — `startActivity(chooser)`, which
returns immediately and has **no result callback**, so dismissing it (or picking
a target that fails) left the app having already forgotten the whole thing. The
plain Export path (PLY/LAS/DXF) did not even do that: it said *"Export queued ->
name"* and never mentioned the file again.

Three silent exits on two buttons. Combined with ROUND 6's vanished captures and
ROUND 7 §3's `points=0` seal, that is the class:
**a user-triggered operation that ends in neither a visible success with a path
nor a visible failure.** The rule now, written down in `DownloadsExporter`'s
header: there is no third outcome.

**Fixed:**

* **`app/share/DownloadsExporter.kt`** copies a finished export into
  `Downloads/LidarScan/` through `MediaStore`. `MediaStore`, not
  `Environment.getExternalStoragePublicDirectory` — the latter needs
  `WRITE_EXTERNAL_STORAGE`, which is not granted on API 29+ and which this app
  deliberately does not request; a `MediaStore.Downloads` row the app inserts
  itself needs **no permission at all**, which is why this works on a
  scoped-storage phone with nothing added to the manifest. `IS_PENDING` is set
  while copying and cleared at the end, so a multi-gigabyte bundle is never
  visible half-written, and a failed copy deletes its own pending row rather
  than leaving a ghost.
* **`ProcessingViewModel.awaitJobThenDeliver`** replaces `awaitJobThenShare` and
  now runs on **both** export paths. It waits for the job, copies to Downloads
  **first** (so the artifact is reachable whether or not the operator picks
  anything from the share sheet), then reports
  *"Exported to Downloads/LidarScan/Scan-008.lscan.zip (184.2 MB)."* — or, on a
  copy failure, names the on-phone path and offers the share sheet as the only
  remaining route. Every branch writes an `[export]` line to `capture.log`.
* The **cloud result download** got the same treatment: `processed/` is exactly
  as unreachable as `exports/`, so a finished cloud job now says where it put
  the result.
* **Settings → Export log** likewise: it used to `?: return` on a staging
  failure and hand off to a share sheet that reports nothing — on the one button
  whose entire purpose is producing evidence. It now says
  *"Capture log saved to Downloads/LidarScan/capture-….log."* or why not.
* `CaptureLog.TAG_EXPORT` is new, so the next report about a missing file
  arrives with the destination attached.

**Proof.** `app/androidTest/…/DownloadsExporterTest.kt` — on the device, against
a real `MediaStore`, because this is precisely the code that compiles fine and
does nothing on a phone. It writes a 400 KB payload (big enough to cross the copy
buffer more than once — a truncating copy is what a 12-byte fixture would hide),
**reads the bytes back out through the provider** rather than trusting the
insert, asserts the `RELATIVE_PATH` really is `Downloads/LidarScan`, asserts the
returned display path is what the UI will show, asserts a missing source fails
loudly with a non-blank message, and asserts a second export of the same name
does not overwrite the first (MediaStore de-duplicates, which is what
re-exporting should do). It cleans up after itself.

### 8. Files

New (`:core`): `capture/ScanAutoName.kt`, `capture/CaptureAutoConnect.kt`,
`capture/TrajectoryTrail.kt`, `gnss/PhoneFix.kt`, `gnss/GeorefSourcePolicy.kt`,
`render/DisplayLimits.kt`, `render/RefreshGovernor.kt` (+ 6 test files).
New (`:app`): `capture/CaptureSensorDetectors.kt`,
`capture/TrajectoryTrailRecorder.kt`, `ar/ArPosePumpView.kt`,
`gnss/PhoneLocationSource.kt`, `gnss/PhoneGeorefRecorder.kt`.
Rewritten: `ui/capture/CaptureScreen.kt`, heavily extended
`ui/capture/CaptureViewModel.kt` and `ui/capture/CaptureSheets.kt`,
`ui/projects/ProjectsListScreen.kt`, `ui/nav/{LidarScanApp,Routes,ScanTabBar}.kt`,
`render/{PointCloudRenderer,PointCloudView}.kt`, `data/Settings*.kt`,
`ui/detail/ProjectDetailScreen.kt`, `ui/pick/ProjectPickerScreen.kt`,
`AndroidManifest.xml`, `androidTest/.../ReplayCaptureSmokeTest.kt`.
Deleted: `ui/newproject/**`, `ui/connect/ConnectWizardScreen.kt`,
`ui/connect/ConnectWizardViewModel.kt`.

`com.lidarscan.core.engine.D6ConnectController` now has **no production caller**
(its wizard is gone; the Capture tab uses `CaptureAutoConnectController`). It and
its tests were kept rather than deleted — it is the state machine any future
device-setup flow would rebuild, and deleting a tested `:core` class in the same
change that redesigned the UI on top of it would have been two decisions dressed
as one. Flagged here so it is a decision and not an oversight.

### 9. Engine seams this round needed and did not have

1. **No fix-shaped GNSS ingest.** `scan_engine_push_nmea` is the only way in;
   there is no `scan_engine_push_fix(lat, lon, h, sigma…)`. Round 5.2 therefore
   synthesizes NMEA. Works, and is arguably better (record-always + A4 + A10 for
   free), but a decoded-fix entry point would remove a text-formatting step from
   the hot path of every phone-georeferenced capture.
2. **No device *kind* for a phone GNSS source.** `scan_add_rtk_rover_device` is
   the only NMEA-capable factory, so a phone fix enters as a "rover". A
   `SCAN_DEVICE_PHONE_GNSS` kind (or a source tag on the push) would let the
   engine and a desktop tell a 4 m phone epoch from a 2 cm rover epoch without
   inferring it from the GGA quality digit.
3. **No live-trajectory getter.** The trail is built from ARCore poses because the
   C ABI exposes no "last N poses" for A6's LIO output (`scan_engine_pose_gate_at`
   answers a different question). On a Mid-360 walk the trail therefore only
   appears when ARCore is also running.
4. **`clamp_display_params()`'s 0.5 px floor.** The owner's live point-size range
   starts at 0.1 px, so `:core`'s `clamped()` now floors at 0.1 and diverges from
   `display_params.cpp`. If these params are ever handed to the engine's own
   clamp, sizes below 0.5 px come back as 0.5.
5. Everything B3 §8 listed (device-config gaps, one `prebound_fd` for two
   sources, `SCAN_EVENT_DEVICE_HEALTH` not surviving `convert_event()`) still
   stands and still bites the same way.

### 10. Verification

```
$ ./gradlew :core:test        # 299 tests, 0 failures (5 skipped, pre-existing)
$ ./gradlew :app:assembleDebug
BUILD SUCCESSFUL
$ ./gradlew :app:connectedDebugAndroidTest   # b4_test AVD, API 34, arm64-v8a
Starting 4 tests on b4_test(AVD) - 14 … BUILD SUCCESSFUL
```

* **`:core:test` — 299 (was 218).** New: `ScanAutoNameTest` (10),
  `CaptureAutoConnectControllerTest` (19 — including the manual-fallback and
  first-detector-wins cases), `GeorefSourcePolicyTest` (11),
  `PhoneFixNmeaTest` (12), `DisplayLimitsTest` (7), `RefreshGovernorTest` (7),
  `TrajectoryTrailTest` (10).
* **Emulator — 4/4 green**, including the new
  `captureTabIsANewScanWithAutoDetectAndAnInlineManualFallback`, which walks
  Projects → Capture tab, asserts the name field + auto-detect line + a disabled
  `Start new scan`, waits out the detect window and asserts the manual panel
  **opened itself** with both transports on it. The replay test now also opens the
  Capture-settings sheet mid-session and drags the point-size slider to its 0.1 px
  minimum while points are still landing, then asserts the count did not go
  backwards.
* **Screenshots** (emulator, `scratchpad/r5_*.png`): the empty Projects state
  pointing at Capture, the Capture tab mid-fallback with the inline manual panel,
  and the Diagnostics sheet showing `IMU — none on device · phone IMU via ARCore`.

### 11. Explicitly NOT verified

No D6, no Mid-360, no RTK rover and **no ARCore-capable device** were available.
So: the pose pump, the nominal-extrinsic pushbroom path, the phone-GNSS NMEA
actually being parsed by the engine's `GnssSource`, the auto-detect *success*
path and the refresh governor's real downshift are verified by construction and
by unit test against fakes — not on hardware. The two that would repay a bench
hour first are (a) that the 2 dp pose pump really does keep `Session.update()`
running in 3D-orbit view on a physical phone, and (b) that a synthesized GGA/GST
burst produces a non-zero `scan_engine_last_fix` with the phone's own accuracy.

## ROUND 5 AUDIT — two desktop-round-5 field bugs, checked against Android, plus two urgent live field reports

Scope: `android/**` only, no git, `engine/**` read-only (read for reference —
the pose/pushbroom investigation below reads three `engine/` files to confirm
a documented formula — never modified). Triggered by two desktop-round-5 field
bugs the owner wanted audited on Android for the same failure modes, then
expanded mid-task by two urgent real-hardware field reports from the same
Pixel 8 Pro + COIN-D6 session. Per-symptom verdicts below; §5's verification
block has the full test/build results.

### 1. FALSE MOTION WHILE STATIONARY — bug found, fixed (two contributing bugs)

The round-5 item 18 "walkthrough motion hint"
(`CaptureViewModel.updateMotionHint`, §6 above) has two inputs: ARCore's own
`EXCESSIVE_MOTION` tracking-failure reason, and B8's motion-gated keyframe
skip counter (`RigMotionTracker`/`KeyframeSelector`, `core/capture/RigMotion.kt`).
Auditing "pose jitter as displacement, bad dt, stale anchors, unit confusion"
against both found one bug in each half.

**(a) `RigMotionTracker.estimateAt()` had a bad-dt bug that made it permanently
invalid online — `core/capture/RigMotion.kt`.** Its `before`/`after` sample
search used to resolve to the exact SAME sample whenever called in the one
pattern every real caller actually uses: `KeyframeRecorder.onFrame` and
`MountCalibrationViewModel.processDetection` both call `motion.add(sample)`
and then, in the same call, `motion.estimateAt(sample.tMonoNs)` — i.e. "the
estimate AT the timestamp of the sample just added," with no future sample
ever existing yet (this is live streaming, not a replay over a pre-loaded
buffer). `before`'s search (`<= tMonoNs`) and `after`'s search (`<= tMonoNs +
windowNs`, with **no lower bound at all**) both landed on that same
just-added sample, giving `dtNs == 0` and `valid = false` on **every single
live call** — confirmed empirically with a 40-sample live-streaming
simulation before the fix (`validCount = 0`). This silently defeated B8's
whole online motion gate (every frame "qualified" via `KeyframeSelector`'s
`!motionValid` short-circuit, so `skippedMotion` never grew and the "Turning
too fast" hint could never fire even when genuinely turning fast) and left
the mount-calibration wizard's live motion readout permanently reading
"stationary" regardless of actual motion.

Fixed by requiring `before` to be **strictly older** than `after` (searching
for the oldest sample within `windowNs` of `after`, rather than of the raw
query time) — this still gives a true centred difference when replaying a
fully-buffered stream offline (where future samples exist and the fix is a
no-op against the old behaviour, confirmed: the pre-existing
"recovers the commanded angular rate" test is byte-identical in output before
and after), and gives a genuine **backward** difference over the available
window when called live, instead of a permanent zero. New tests in
`RigMotionTest.kt`: a stationary live stream reads ~0 (not "always invalid"),
a stationary live stream **with pose jitter** (±3 mm / ±0.2° synthetic noise)
stays well under both the 15°/s turn gate and any walking speed, a moving
live stream (1.2 m/s) reads back within 0.05 m/s, and a direct regression
pin for the exact live-call-pattern bug above.

**(b) `CaptureArController.pause()` left `failureReason` stale across a
pause/resume cycle — `app/ar/CaptureArController.kt`.** `pause()` reset
`sessionRunning`/`tracking` but not `failureReason` (only `close()` did,
via a full `ArStatus()` reset). `CaptureScreen.kt`'s `needsArSession` flips
the AR session through `pause()`/`resume()` any time it goes momentarily
false — which happens for a Mid-360 session (no `poseTrackingRequired`)
exactly at Stop, and flips back true at the very next Start. If the LAST
pose pushed before that pause happened to carry `EXCESSIVE_MOTION` (quite
plausible at the tail end of an active walkthrough — people keep moving
right up until they tap Stop), the stale `failureReason` survives the
pause/resume, and `updateMotionHint()`'s 500 ms poll can read it — with
`tracking` also still `false` from the same pause — before the next
`onFrame()` gets a chance to refresh it. Result: **"Moving too fast — slow
the walk so tracking can keep up" fires immediately on the second recording
of a Start → Stop → Start session, while the phone is sitting still.** This
is the closest, most field-realistic match to the reported symptom, and it
lines up with task 2's own scenario. Fixed by resetting `failureReason` to
`NONE` in `pause()` too, matching `close()`'s existing full reset.
`CaptureArController` needs a real `Context`/ARCore `Session` this project
has no Robolectric harness for, so this fix is verified by code inspection
and by mirroring `close()`'s already-correct, already-tested-by-construction
pattern, not by a dedicated JVM test — flagged here rather than silently
skipped.

Verdict: **bug found, fixed** (two contributing bugs, both in the capture
path exactly as scoped). 10 new/changed `:core` tests in `RigMotionTest.kt`.

### 2. MULTI-CYCLE RECORDING — bug found, fixed

Traced `CaptureViewModel`'s state machine end to end. `startCapture()` reads
`(_uiState.value as? CaptureUiState.Loaded)?.project ?:
createProjectForThisScan()` — i.e. "record into whatever project is already
loaded, and only create a new one if nothing is." `stopCapture()` used to
unconditionally end with `projectStore.open(activeId)?.let { _uiState.value =
CaptureUiState.Loaded(it) }` — refreshing the JUST-SEALED project back into
`Loaded` state, with a comment explaining why ("the in-memory project must
not go stale, or a later start would re-read the old manifest"). The
consequence the comment's author did not follow through: **a second Start
within the same connect session saw `_uiState` still `Loaded(project1)` and
silently re-opened and re-recorded into project #1's already-sealed
directory** — `createProjectForThisScan()` was never reached, the auto-name
series counter was never spent for scan #2, and no second `.lscan` directory
was ever created. The connect session itself (`CaptureAutoConnectController`,
`EngineBridge.connectionState`) was untouched by this and stayed correctly
re-armable — only the "which project does Start record into" state was
wrong.

Fixed by branching on `projectId` (the constructor param that is `null` on
the Capture tab's own "create a new scan" route and non-null on a
project-scoped route like replay/deep-link): the project-less case now
returns to a fresh `CaptureUiState.NewScan(autoName = ...)` — re-computing
the placeholder the same way `init{}` does — so `startCapture()`'s own
`Loaded` check is naturally null again and `createProjectForThisScan()` runs
on every Start, not just the first. The project-scoped case (replay) is
unchanged, keeping the original comment's behaviour where it actually
applies (there is only ever the one project on that route, so there is no
ambiguity to introduce a bug).

**Verified two ways:**
- `app/src/test/kotlin/.../CaptureViewModelMultiCycleTest.kt` (new, JVM,
  `:app:testDebugUnitTest`) drives the real `CaptureViewModel` against
  `:core`'s `FakeEngineBridge` + a real `FileProjectStore` over a temp dir —
  Start → Stop → Start → Stop, asserting the two projects have **different
  ids**, that `store.list()` has **exactly two** entries, and that **both**
  have a non-zero sealed `pointCountEstimate`. This is the test that can
  actually assert "two distinct projects" — the emulator's only
  hardware-free recording path (replay) structurally cannot, see below.
- `ReplayCaptureSmokeTest.kt`'s `replaySyntheticCaptureDecodesPointsWithoutCrashing`
  (extended, `:app:connectedDebugAndroidTest`, run on a booted `b4_test` AVD
  in this task — see §5) now runs a **second** Start/Stop cycle after the
  first (Stop → wait for the button to read "Start replay" again → Start →
  assert the decoded point count actually grows again, not stuck at cycle
  1's frozen value → Stop). This exercises the real JNI/native replay engine
  restarting on the same handle twice in a row, end to end, with no crash.
  **It is not the same two-distinct-projects assertion** — `ReplayEngineBridge`
  is documented (`app/engine/ReplayEngineBridge.kt`) to ignore
  `projectDirectory` and always reuse the one "Synthetic Replay Demo"
  project (there is no hardware-free way to reach the New-scan Capture tab's
  own Start on a bare emulator: it stays disabled with nothing to
  auto-detect, per the existing `captureTabIsANewScanWithAutoDetectAndAnInlineManualFallback`
  test) — so it complements the JVM test rather than duplicating it: JVM
  proves the state-machine fix, the emulator proves the underlying
  stop/restart machinery survives on real device/JNI code.

Verdict: **bug found, fixed.**

### 3. REFRESH GOVERNOR — downward shift already correct; two upward-recovery bugs found and fixed; no motion/hint state gates rendering (confirmed)

`RefreshGovernor` (`core/render/RefreshGovernor.kt`) itself was already
correct and already tested: downshift-only by design (never raises the cap
on its own — a deliberate, documented, tested rule, not a gap), sustained-not-spiky,
one notch at a time, and an explicit `request()` **always** fully resets the
downshift regardless of whether the requested value differs from before (no
"unchanged, skip" guard in the class itself) — confirmed with a new test,
`re-requesting the SAME already-selected rate still clears an active
downshift`.

**What was broken is entirely in the app-layer wiring around it** — the
operator's actual path to recovering after an auto-downshift:

**(a) `PointCloudRenderer.setMaxRefreshHz`'s own no-op guard blocked
recovery.** `PointCloudView` calls `renderer.setMaxRefreshHz(maxRefreshHz)`
unconditionally on every recomposition (by design — cheap, idempotent,
avoids fighting the governor's own auto-downshift every frame). Its guard,
`if (hz == maxRefreshHz) return`, is what made that safe — but it also meant
that re-selecting the SAME option the operator already had chosen (the
natural way to ask for recovery, since the control still visually shows that
option selected) was **completely indistinguishable from Compose merely
recomposing with an unchanged value**, so `governor.request()` was never
called again and a downshift was permanent for the rest of the session. The
only way out was picking a genuinely different rate first, then flipping
back — a two-tap dance the UI gave no hint was necessary.

Fixed with an explicit "the operator asked" signal:
`CaptureViewModel.refreshRequestToken` (bumped on every `setRefreshHz` call,
even when the numeric value does not change) flows down through
`CaptureScreen` → `CaptureViewport` → `PointCloudView` →
`PointCloudRenderer.setMaxRefreshHz(hz, requestToken)`, whose guard now
requires **both** the value and the token to be unchanged before skipping —
so a genuine re-pick reaches `governor.request()` even when the numeric
value is the same, while the per-recomposition repeats (same value, same
token) still cost nothing.

**(b) `CaptureViewModel.setRefreshHz` still had round-5.3's own removed
clamp.** `if (hz in 1..59) hz else 0` silently coerced any pick of 60, 90 or
120 fps (all real options `RefreshGovernor.optionsFor()` offers on a fast
phone) back to `0` ("Max") — a leftover from before round 5.3 explicitly
lifted this ceiling (`PointCloudRenderer.setMaxRefreshHz`'s own doc: "the cap
is no longer clamped at 59"). Fixed to `if (hz > 0) hz else 0`, matching
`PointCloudRenderer`'s own clamp.

**No motion/hint state gates rendering — audited, confirmed true, nothing
to fix.** `motionHint` (§1) and `refreshDownshiftNote` (this section) are
both plain `String?` `StateFlow`s read only by `CaptureScreen`'s `Hint(...)`
composables — one inline sentence each, painted over the viewport, never
touched by `PointCloudRenderer`'s draw path, `PointCloudSource` polling, or
any `if (motionHint != null)`-style gate anywhere in the render loop. The
"stream filter" that DOES gate what's drawn (`StreamFilter`, §4 below) is
driven by `liveSlam`/stream-id availability, never by hint/motion state.

Verdict: **two bugs found and fixed** (recovery path); downshift and the
"no rendering gate" property were already correct — confirmed, not assumed,
by the tests above.

### 4. URGENT FIELD ITEM — "the D6 scan result is a single vertical plane, not extruded along the walk"

Real Pixel 8 Pro + COIN-D6 walk. Reported: ARCore tracking/the trail looked
good; the D6 result did not — "capture the plane of Z-axis instead of XY."
Investigated in two passes (a dedicated research pass over `engine/`, read
only, plus this task's own synthetic test), since `engine/` cannot be
modified even if the bug had turned out to live there.

**Ruled out, with evidence, not assumption:**

- **No axis-convention bug in the pose/composition chain.** ARCore's raw
  pose (`pose.tx()/ty()/tz()`, `pose.qx()/qy()/qz()/qw()`) is passed
  unchanged through every layer — `CaptureArController.publishPose` →
  `ScanEngineNative.nativePushPose` → `arcore_jni.cpp` (a byte-for-byte
  field copy) → `scan_engine_push_pose`. `engine/src/slam/pushbroom/pushbroom_assembler.cpp`'s
  `resolve_()` and `engine/include/scanengine/poses/se3.h` are generic SE(3)
  composition with **no axis specifically treated as "up"** — confirmed by
  reading both files end to end. The D6/ARCore path never touches the
  Mid-360/LIO path's own Z-up gravity-aligned frame
  (`engine/docs/A6-lio.md`); the two conventions exist side by side but are
  never mixed for one session.
- **`BracketNominals.cadNominal(SensorType.COIN_D6)`'s rotation (identity)
  is geometrically sound for a straight walk — proven, not just reasoned
  about.** `core/src/test/kotlin/.../calib/D6PushbroomGeometryTest.kt` (new)
  reimplements A8 §3.1's exact documented formula (`world_from_lidar =
  world_from_phone(t) · phone_from_lidar`, `p_lidar = (d·sinθ, d·cosθ, 0)`)
  using `:core`'s own `Mat4`/`Quat`/`Vec3` and the REAL production
  `BracketNominals` matrix, against a synthetic 10 m straight walk with a
  full D6 revolution per pose. Result: the walk (Z) axis extent spans ~the
  walked distance (>8 m), the fan's own (X, Y) sweep stays bounded to its
  own diameter (~2 m) regardless of how far the walk goes, and Z dominates Y
  by >3x — axes asserted **by name** so a Y/Z mixup anywhere in this chain
  would fail loudly. A second test confirms a **stationary** revolution
  stays bounded on every axis (the negative-space check, proving the first
  test's large Z spread really is the walk and not an unrelated blow-up).
  This does not touch `engine/` — it is a from-scratch reimplementation in
  `:core`, run against the actual Android-side matrix that goes into a real
  capture.

**Found and fixed — a live-view honesty bug that plausibly explains what the
operator actually SAW while walking, independent of what the recorded
`.lscan` itself contains:** `A8's assembled pushbroom cloud is
INT24-wiring.md's own documented rule — it publishes under
`SCAN_STREAM_SLAM_MAP` (id 8), the SAME stream id Mid-360's live-SLAM map
uses, **regardless of the `live_slam` config flag** (that flag is
`core/model/CaptureDefaults.kt`'s own documented "a Mid-360 concept, and the
D6 case is not a bug" — the D6's pushbroom is gated on a mount calibration,
not on `live_slam`). `StreamFilter.MAPPED_ONLY` (selected whenever
`liveSlam == true`, which three of the four workflow profiles — including
`QUICK_SCAN`, the app's own default — set true) correctly falls back to
drawing **raw, un-pushbroomed sensor-frame pages** until the first mapped
page has actually landed (a deliberate, documented, correct choice — a
strict map-only filter would show a black screen while A6/A8 initialise).
The bug: `CaptureScreen.kt`'s "what stream is on screen" chip read `liveSlam`
alone and said **"LIVE MAP · SLAM" for the entire stretch that fallback was
still active** — telling the operator they were looking at a registered,
walk-extruded map while they were actually looking at raw fan slices (which,
drawn without pose interpolation, look exactly like "a flat plane," matching
the report almost word for word) — for however long A6/A8 take to resolve
and flush a first page during a real walk on real hardware, which this
environment cannot measure without the device.

Fixed: `PointCloudRenderStats` gained `hasSeenMappedPage` (mirrors
`StreamFilter.MAPPED_ONLY`'s own `mappedSeen` rule); `CaptureViewport` polls
it (300 ms, cheap) and re-arms on every fresh `recording` transition (so it
does not carry a stale "seen it" from cycle 1 into cycle 2 of a multi-cycle
session — see §2); the chip now reads "BUILDING MAP…" until a mapped page
has genuinely been seen, only then "LIVE MAP · SLAM".

**Verdict: audited, root cause not found in code (proven, not assumed, via
the synthetic geometry test) — a plausible, fixed, live-view-honesty
contributor found and fixed instead.** What remains explicitly unverified,
and needs a bench hour with the real bracket rather than more code reading:
whether `BracketNominals.cadNominal`'s **physical** premise (a real D6
bracket's spin axis actually mounted the way the CAD nominal assumes)
matches the actual hardware used in the field test — `A8-pushbroom.md`
itself already flags this nominal as an honest, unvalidated placeholder
("no physical bracket exists yet") and recommends the mount-calibration
wizard over trusting it for a real capture. Whether that wizard had been run
before this field session is also unknown from this environment (the app's
own `mountIsNominal` diagnostic row would answer it from a live manifest,
which this environment does not have).

### 5. URGENT FIELD ITEM — "the AR camera not show up"

Same session, switching to the AR overlay view showed no camera image (the
B7 stacked-surface AR overlay: camera background + point overlay), though
the trail still looked correct — i.e. ARCore itself kept tracking; this is
view/session plumbing, not ARCore startup.

**Root cause: a session-ownership race the class's own doc comment warned
about but did not structurally prevent.** `ArPosePumpView` (mounted in every
non-AR view) and `ArOverlayView` (mounted in `CameraMode.AR`) both build an
`ArCameraBackgroundRenderer` around the SAME shared `CaptureArController`
(the app's one ARCore `Session`), and the class doc already said "never
composed at the same time... two pumps would call `Session.update()` from
two threads." That was true of *what Compose decides to compose*, never of
*the underlying GL threads' lifecycle*: `AndroidView`'s `factory`/`onRelease`
run on the main/Compose thread, but each `GLSurfaceView`'s
`onSurfaceCreated` fires asynchronously, on its OWN GL thread, whenever the
platform hands it a real `Surface` — nothing guaranteed the OLD renderer's
thread had genuinely stopped calling `Session.update()`/
`setCameraTextureName()` before the NEW one's thread started. Concretely:
the pump's `RENDERMODE_CONTINUOUSLY` thread re-binds **its own** texture id
on every single frame (`ArCameraBackgroundRenderer.onDrawFrame`), so if it
won even one more race after the overlay's surface came up, ARCore would
keep writing the camera image into the (now off-screen, 2 dp) pump's
texture — the overlay's own texture is simply never written. No exception,
no log line, just a permanently black background: exactly the report.

Fixed with an explicit ownership token instead of an implicit
Compose-branch assumption — `CaptureArController.RendererOwner` (`POSE_PUMP`
/ `OVERLAY`), `claimRenderer(owner)`/`releaseRenderer(owner)` (the latter a
no-op unless `owner` still holds it, so an out-of-order release from a
stale, still-tearing-down renderer can never undo a newer claim), and
`onFrame(owner)`/`setCameraTextureName(textureId, owner)` now both no-op
entirely (never touch `session`) unless `owner` is the current claim. Both
`ArPosePumpView` and `ArOverlayView` claim in their `AndroidView` `factory`
(main thread, at the exact moment the view is created) and release in
`onRelease`, and `MountCalibrationScreen` gets the same fix for free since
it reuses `ArOverlayView` directly. This is the half of the fix that
matters most for correctness (it is what actually prevents
`Session.update()` from ever being called by two threads at once, which
ARCore's own contract leaves undefined), not just the texture symptom.

**Verdict: bug found, fixed.** `CaptureArController` needs a real ARCore
`Session`/GL surfaces this project has no Robolectric/headless harness for
(same posture as §1(b) above), so this is verified by code inspection
against the exact race described, not by a device-run test — the emulator
used for §2/§5's other verification cannot exercise real ARCore camera
frames at all (no Play Services / ARCore support on a bare AVD). A real
device is the only way to confirm the black-camera symptom is gone; this
fix removes the specific mechanism that produces it either way.

### 6. Files

New (`:core`): `test/.../capture/D6PushbroomGeometryTest.kt`.
New (`:app`): `test/.../ui/capture/CaptureViewModelMultiCycleTest.kt`.
Changed (`:core`): `capture/RigMotion.kt` (+its test file).
Changed (`:app`): `ar/CaptureArController.kt`, `ar/ArCameraBackgroundRenderer.kt`,
`ar/ArPosePumpView.kt`, `ar/ArOverlayView.kt`, `ui/capture/CaptureViewModel.kt`,
`ui/capture/CaptureScreen.kt`, `render/PointCloudRenderer.kt`,
`render/PointCloudView.kt`, `androidTest/.../ReplayCaptureSmokeTest.kt`.

### 7. Verification

```
$ ./gradlew clean :core:test :app:assembleDebug
BUILD SUCCESSFUL
$ ./gradlew :app:testDebugUnitTest
BUILD SUCCESSFUL
$ ./gradlew :app:connectedDebugAndroidTest   # b4_test AVD, API 34, arm64-v8a, booted THIS task
Starting 4 tests on b4_test(AVD) - 14 ... BUILD SUCCESSFUL
```

- **`:core:test` — 306 (was 299), 0 failures** (5 skipped, pre-existing,
  unrelated). New/changed: `RigMotionTest` (+5 tests: live-stationary,
  live-stationary-with-jitter, live-moving, the exact live-call-pattern
  regression pin, plus the pre-existing suite unchanged in behaviour),
  `RefreshGovernorTest` (+1: same-value re-request clears a downshift),
  `D6PushbroomGeometryTest` (+2, new file).
- **`:app:testDebugUnitTest` — 13, 0 failures**, plain JVM (no
  emulator/Robolectric — `FakeEngineBridge` + `FileProjectStore` +
  `Dispatchers.Unconfined` for `viewModelScope`, real wall-clock delays).
  New: `CaptureViewModelMultiCycleTest` (1 test, the multi-cycle
  two-distinct-projects assertion).
- **`:app:assembleDebug`** — succeeds, clean build from scratch including a
  full native rebuild (arm64-v8a + x86_64) against the real `engine/` C++
  sources — nothing here was skipped or stubbed.
- **Emulator — booted a fresh `b4_test` AVD in this task** (API 34,
  `google_apis/arm64-v8a`, the same system image B4's own NOTES entry used;
  `avdmanager`'s default hardware profile is a tiny 320×640@160dpi phone
  that made the PRE-EXISTING `captureTabIsAn...` test fail on an unrelated
  scroll/visibility assertion — fixed by hand-editing `config.ini` to a
  1080×2400@420dpi profile matching a real modern phone, not by touching
  that test). **4/4 green**, including the extended
  `replaySyntheticCaptureDecodesPointsWithoutCrashing` (§2's second
  Start/Stop cycle, ~33 s total, real JNI/native replay code exercised
  twice with no crash).
- **Not run**: real ARCore camera frames, real D6/Mid-360 hardware, the
  physical mount-bracket check §4 flags as the one remaining unverified
  step — none of that is available in this environment, consistent with
  every prior round's own stated posture.

## ROUND 6 — five field items from a real Pixel 8 Pro + COIN-D6 session

Scope: `android/` only, no git, the engine tree **read-only** (read extensively —
`engine/src/record/lscan.cpp`, `engine/src/core/engine.cpp`,
`engine/src/cloud/page_store.cpp`, `engine/src/slam/pushbroom/pushbroom_assembler.cpp`
— never modified; a concurrent task owns `engine/`). Triggered by five owner
items from a second field session on 0.2.1 (`docs/design/REVIEW_FEEDBACK.md`
items 19–23). VERSION 0.2.1 → **0.3.0** (versionCode 30000).

Per-item verdicts first, then the evidence.

| # | Owner's words | Verdict |
| --- | --- | --- |
| 1 | "the AR overlay crush the app when enable" | **Root cause found (3 crash paths), fixed, hardened.** Not device-verified. |
| 2 | "the capture not saved… the app project not see any saved" | **Root cause found, fixed, proven against the real native engine on device.** Lost captures recover. |
| 3 | "bearly maping the 3d slam… points not aligned… dont default to max" | **Two root causes found and fixed** (stream filter + page-store sizing) + conservative defaults. |
| 4 | Light / Optimal / Full quick-select | **Shipped**, with the full parameter set still individually editable. |
| 5 | D6 mount re-zero | **Shipped**, composition math unit-tested. Not device-verified. |

---

### 1. "the AR overlay crush the app when enable" — bug found, fixed (three crash paths)

No stack trace came with the report, so the whole enable path was audited cold.
The prime suspect named in the brief — the ROUND 5 `RendererOwner` hand-off —
turned out **not** to be it. The hand-off was already correct. What was missing
was the *lifecycle* half, and it produced a genuine, reproducible-by-inspection
process kill.

`Session.update()` throws `SessionPausedException` — an **unchecked**
`RuntimeException` — when the session has not been resumed. It is called from
the `GLSurfaceView` render thread, where an uncaught exception is an uncaught
exception on a non-UI thread: the default handler takes the process down.
`CaptureArController.onFrame()` caught exactly one type,
`CameraNotAvailableException`. Every other ARCore exception went straight to the
killer.

Three concrete routes into it, and enabling the AR overlay hit all three:

**(a) The grant-the-permission path — the likely one.**
`CaptureScreen`'s camera-permission callback was
`{ granted -> if (granted) container.arController.createSession() }`. **No
`resume()`.** Since ROUND 5 moved the camera request off screen entry, the
normal first time anyone selects AR overlay is: `needsArSession` flips true →
`ArOverlayView` is composed and its `RENDERMODE_CONTINUOUSLY` GL thread starts →
the permission dialog appears → grant → `createSession()` publishes a **live,
un-resumed** `Session` → the GL thread's next `onDrawFrame` calls `update()` →
throw → dead. The `LaunchedEffect(needsArSession)` that does call `resume()`
does not re-run, because `needsArSession` never changed.

**(b) The create/resume race.** Even with permission already granted,
`createSession()` assigned `session = s` and *then* ran
`ArCameraCharacteristicsProbe.probe()` (a Camera2 characteristics query,
milliseconds) before `resume()` could run. A 60–120 fps GL thread only has to
win that window once.

**(c) Pause during teardown.** `DisposableEffect(needsArSession)`'s `onDispose`
pauses the controller on the main thread while the GL thread is still running —
backgrounding or leaving the screen with AR on.

**Fixed** with `app/ar/ArSessionGate.kt`: one plain-Kotlin state machine holding
*ownership* (ROUND 5's claim/release, unchanged in behaviour) **and** lifecycle
(`sessionCreated`, `resumed`, sticky `failure`). `mayDrive(owner)` returns
`PROCEED` only when the caller owns the session, the session exists, it is
resumed, and nothing has failed; everything else is a named reason
(`NOT_OWNER` / `NO_SESSION` / `NOT_RESUMED` / `FAILED`) and a silent no-op.
Alongside it:

* `CaptureArController.onFrame()` is gated by `mayDrive` and wrapped so it
  **cannot throw** — `SessionPausedException`, `MissingGlContextException`,
  `DeadlineExceededException`, a driver's own `IllegalStateException`, or a
  frame listener throwing, all degrade instead. `pause()` shuts the gate
  **before** pausing the session, so a GL thread mid-frame cannot slip past.
  `createSession()` now probes *before* publishing `session`, so window (b) is
  not merely guarded but never opened.
* `ArCameraBackgroundRenderer` wraps `onSurfaceCreated` / `onSurfaceChanged` /
  `onDrawFrame` in one try/catch each and latches a `degraded` flag, so a broken
  driver reports once and draws black rather than throwing per frame. Its shader
  **program link status was never checked** — added.
* The permission callback now calls `createSession()` **and** `resume()`. This
  is the half that makes AR actually work after a grant rather than merely not
  crash.
* `ArStatus.arError` carries the reason; `CaptureScreen` shows
  **"AR unavailable — <reason>. The 3D view and the recording are unaffected."**
  inline and falls the view back to 3D orbit. Toggling into AR again clears the
  failure and retries, so one bad frame cannot disable AR for the session.

**Proof.** `app/src/test/.../ar/ArSessionGateTest.kt` — 14 JVM tests, no
Robolectric (this project has none, and adding a dependency to an offline build
was not worth it; extracting the logic into a plain class is the better shape
anyway). Covers exactly the brief's scenarios: rapid toggle (200 alternating
claims with late out-of-order releases), background/foreground during a claim,
surface destroyed mid-claim, the never-resumed session, pause-shuts-the-gate,
failure reported once, re-claim clears it, and a 3-thread contention run.

**Not verified**: no ARCore-capable device exists in this environment, so the
crash is proven by construction and by the state machine's tests, not by
watching it not happen on a phone. The mechanism is removed either way.

### 2. "the capture not saved to the phone, it just gone" — ROOT CAUSE FOUND. Data loss. Fixed.

**The app was writing its project metadata into a file the ENGINE owns.**

`FileProjectStore` had persisted `ProjectManifest` as
`<project>.lscan/manifest.json` since B1. That is the engine's file:
`engine/src/record/lscan.cpp`'s `FileRecordWriter::open()` writes its own,
completely different `manifest.json` there the moment `scan_engine_start()` is
called (engine.cpp's `if (cfg.record)` block), and rewrites it `"sealed": true`
at `scan_engine_stop()`. Tech Spec §3.11 is on the engine's side — `manifest.json`
inside a `.lscan` is the *container's* manifest.

So the first real capture into a project **destroyed that project's app-side
metadata**. The app schema requires `name`, `sensor`, `createdAtEpochMillis` and
`appVersion` (none of which the engine writes) and its `profile` is an enum where
the engine writes `"quickscan"`, so `readProject()` could no longer decode it →
`list()` skipped the directory → the project was invisible in Projects → and
every later `updateManifest()` returned null, so the seal at Stop silently did
nothing too. The bytes were on disk; the project was gone from the app. Word for
word the field report.

**Why five rounds of tests never caught it:** the two hardware-free paths cannot
collide. `ReplayEngineBridge` documents that it ignores `projectDirectory`
entirely, and `FakeEngineBridge` writes no files at all. The app's writer and
the engine's writer only ever meet on a real device with a real sensor — which
is precisely the session the owner ran.

**The fix, in three parts, all Android-side:**

1. **The app's record moved to `project.json`** (`FileProjectStore.APP_MANIFEST_FILE_NAME`),
   a filename the engine has no concept of. `manifest.json` goes back to being
   the engine's.
2. **Legacy migration.** A project whose `manifest.json` still parses as the app
   schema (created before this change, never recorded into) is read as before
   and copied to `project.json` on first read, so the next capture cannot destroy
   it.
3. **Recovery of captures already lost.** A `.lscan` whose `manifest.json` is the
   ENGINE's is rebuilt into a listable project from what is genuinely knowable —
   name from the directory slug the app itself chose (`scan-014-2026-08-17-1932-a1b2c3.lscan`
   → `Scan-014-2026-08-17-1932`), `createdAt` from the engine's `createdAtUtcNs`,
   profile from the engine's own session string, sensor inferred from whether an
   IMU stream exists (only a Mid-360 has one; the D6 has no IMU at all — ROUND 5
   item 11). It is flagged `ProjectManifest.recovered` and the Projects card
   shows a **RECOVERED** chip, because a rebuilt manifest deserves less trust
   than its points. **The owner's vanished field captures should reappear on
   first launch of 0.3.0.**

**The seal itself was also wrong in three ways, independently of the above:**

* It ran entirely in `viewModelScope`. Stop is the exact moment a walkthrough
  operator also leaves the screen or pockets the phone, and every `withContext`
  in it is a cancellation point. Now wrapped in `NonCancellable`.
* `updateManifest`'s result was **not even assigned**. Now checked.
* Nothing verified anything. The seal now **re-opens the project through the
  same store the Projects tab lists with** and reports what it found.
* Anything thrown inside the seal escaped into the scope's handler and the seal
  simply stopped happening, silently. Now caught and surfaced.

**And a safety net the emulator earned.** Watching the live run showed the replay
engine **self-terminating** at end of file — `captureState` went IDLE with the
ViewModel's `stopCapture()` never invoked, so that session was never sealed at
all. A `captureState` watcher now seals any session that ends without Stop
(`sealPending` + a mutex so a session seals exactly once). Proven on device:

```
[session] start: project=synthetic-replay-demo-20c7b9.lscan … preset=FULL tier=MODEST
[seal] session ended without Stop (state=IDLE) — sealing anyway
[seal] sealed OK id=synthetic-replay-demo-20c7b9.lscan name="Synthetic Replay Demo"
       points=120300 elapsedMs=17146 listable=true dir=/storage/emulated/0/…/…lscan
```

**Surfaced loudly.** A failed save is a red-bordered **SCAN NOT SAVED** banner at
the top of the capture body (not a `Hint`, not a toast), naming the on-disk path
so the raw capture can be rescued; the session summary sheet now reads
*"Saved to /storage/…/scan-…lscan — it is in the Projects tab now."* or the
failure. A Start that cannot create its folder, and an engine that refuses to
start, both say so instead of returning silently as they did.

**The on-device rolling log** (`app/debug/CaptureLog.kt`): project created,
session start, pushbroom/extrinsic applied, live-store full, every seal verdict,
every failure — into `filesDir/logs/capture.log`, 512 KB live + 512 KB rotated,
path + live tail + **Export log** / Clear on the Settings screen. Internal
storage, not external, so a file manager cannot wipe it. Never contains a
location fix, an address or a token.

**Proof.**
* `core/…/store/ProjectManifestCollisionTest.kt` (10 tests) writes a byte-shape
  copy of `write_manifest()`'s output and asserts the project survives, seals,
  re-opens, migrates, recovers, and that junk is *not* invented into a project.
* `app/…/ui/capture/CaptureSealSurvivalTest.kt` (4 tests) drives the **real
  `CaptureViewModel`** against a bridge that writes the engine manifest at start
  and stop, and asserts the finished capture is in `store.list()`, sealed,
  with a saved path and log lines — plus a storage-lost case that must raise a
  loud error naming the path.
* **`app/androidTest/…/CaptureSurvivesEngineSessionTest.kt` (2 tests) — the real
  thing.** On the emulator it creates a genuine `scan_engine*` through the JNI,
  starts a genuine recording session on a genuine `FileProjectStore` project
  directory, asserts the ENGINE really did overwrite `manifest.json` with its own
  schema, stops, and then asserts the capture is still listed, still named, and
  still sealable. The second test deletes `project.json` to reproduce the 0.2.1
  outcome exactly and asserts the capture is recovered with its name and profile.
  Both ran (0 skipped) against the real native engine.

### 3. "bearly maping the 3d slam for d6… the point are not really aligned" + "dont default to max"

Two independent root causes, both found, plus the defaults audit.

**(a) ROOT CAUSE — the D6's live map was never drawn at all.** The viewport's
stream filter was `StreamFilter.forSession(liveSlam)`. On the Capture tab
`_liveSlam` initialises to `false` and is only ever read from a manifest on the
*project-scoped* route — so on the tab the owner actually uses it is false unless
somebody opens the settings sheet and toggles it. `StreamFilter.RAW_ONLY`, by
construction, **rejects `SCAN_STREAM_SLAM_MAP`** — which is exactly the stream
A8's pushbroom publishes its world-frame cloud on (`engine_pushbroom_defaults()`
sets `out_stream = kSlamMap`). So a D6 session drew the raw sensor-frame fan, in
the sensor's own frame, and **never once drew the pushbroom-resolved cloud the
entire D6 pipeline exists to produce**. "Barely mapping", and "points not
aligned" — because what was on screen was un-posed fan slices at the origin.

`liveSlam` is a Mid-360 concept — `CaptureDefaults`' own KDoc says so. The D6's
live map is gated on the **pushbroom**, so that is what gates its filter now:
`liveMapRequested = liveMapEnabled && (liveSlam || pushbroomActive)`, where
`pushbroomActive` is set when `pushbroom_enable(true)` is actually accepted. The
bottom-left chip follows (`RAW · COIN-D6` → `BUILDING MAP…` → `LIVE MAP · 3D`,
plus `RAW · LIGHT PRESET` when it is off by choice).

**(b) ROOT CAUSE — the live page store was sized for a desktop and filled in
about a minute.** `page_store.h` defaults to `page_capacity = 1<<20` and
`max_pages = 64`: with a 16-byte `PointVertex` that is **16 MB per page and a
1 GB ceiling**. `RealEngineBridge` passed `0, 0` ("engine defaults") from B2
until now, so a phone got a desktop's numbers.

That would be merely wasteful if pages filled up. They do not, because
`PageStore::append()` only ever appends to the **tail** page and a page belongs
to exactly one stream:

```cpp
const bool need_page = tail == nullptr
    || tail->count >= tail->capacity
    || tail->stream != stream;   // pages are single-stream
```

A D6 capture has two producers publishing alternately — the driver's raw preview
on `kLidarD6` and the pushbroom on `kSlamMap` — so **every alternation allocates
a fresh 16 MB page for the ~4096 points of one pushbroom batch**. Utilisation
well under 1 %, the 64-page ceiling reached in a few dozen alternations, and when
the store is full `append()` stores **nothing, forever** (its header says so:
"When full it appends nothing… A14 replaces the cap with an LOD/eviction
policy"). The map grows for about a minute and then silently stops. The desktop
shell independently proved the same store filling this round
(`page store full (64 pages): dropped N points`, 1400× in one session).

Fixed **without touching the engine**: `scan_engine_config` already exposes both
numbers and the JNI already marshals them, so the app stops asking for desktop
defaults. `core/render/LivePageStoreSizing.kt`, per device tier:

| tier | page | pages | resident ceiling | worst case |
| --- | --- | --- | --- | --- |
| modest | 32 k pts | 192 | 6.3 M pts | 96 MB |
| standard | 64 k pts | 256 | 16.8 M pts | 256 MB |
| flagship | 128 k pts | 192 | 25.2 M pts | 384 MB |

An alternation now costs ~1 MB instead of 16 MB, and there are 3–4× more pages.
**It does not remove the ceiling** — a long enough walk fills any bounded store —
so the app now *watches* for it (`pageCount() >= maxPages`, polled at 1 Hz, one
JNI int) and says so inline: *"Live map is full (16.8 M points) and has stopped
growing — the phone's preview buffer, not the scan. Recording is unaffected;
processing the project afterwards uses every point."* **Eviction is an engine
change and `engine/` is read-only here; a concurrent task owns it** — when that
seam lands, the opt-in goes into `RealEngineBridge.createEngineHandle()` (one
call site, deliberately consolidated from the two copies it had) and this note
becomes "showing the most recent points" instead of "stopped growing".

**(c) Conservative defaults.** Every live-view default was the *maximum* its
control offered: refresh `0` = uncapped (120 Hz on the owner's phone), LOD budget
20 M (top of its own 2–20 M slider), keyframes on at 3 fps, trail at its full
600-point ring. All of them now come from
`PerformancePresets.tuningFor(OPTIMAL, tier, ceiling)` — genuinely mid-tier per
device class (30 fps of 120, 5–8 M of 20 M, keyframes at 2 fps on a standard
phone). `PerformancePresetTest` asserts *"no default is a maximum on any device
class"* as a property rather than pinning numbers.

**Honest limits of live D6 quality** (asked for explicitly): the live map is a
bounded, decimated preview of what the pushbroom resolves *while poses are
already available*; it is not the scan. Points whose ARCore pose has not arrived
yet stay pending inside the assembler and appear later; points resolved after the
store fills never appear at all. **Post-processing remains ground truth** — it
re-runs from the raw streams that record-always already wrote to disk, with the
full trajectory, and is unaffected by every number on this page.

### 4. Light / Optimal / Full — shipped

`core/capture/PerformancePreset.kt`: three selectable presets plus a
non-selectable `CUSTOM` read-out, a `CaptureTuning` of the five knobs that cost
sustained GPU/CPU/disk during a walk (live map on/off, refresh cap, LOD budget,
keyframes + rate, trail length), and a coarse `DeviceTier` from RAM, cores and
the panel's real refresh ceiling.

* **Light** = raw preview + record, no live map, no keyframes, smallest budget.
* **Optimal** = the default, mid-tier on every axis.
* **Full** = everything on — and *still capped* on a modest phone, because "Full"
  must not mean "crash"; only a flagship gets `refreshHz = 0`.

Presets **prefill** the same flows the settings sheet's own controls write, so
every parameter stays individually editable; moving one flips the chip row to
CUSTOM and **nothing is reverted** — that is item 22's "keep the full parameter
for advance user setting", enforced by a test. Switching reports what it changed
(*"Full: live 3D map on · live refresh 15 fps → 30 fps · point budget 2 M → 20 M ·
camera keyframes on · trail length 200 → 600 points"*), and Full carries an
inline caution on weaker devices. Persisted **per device profile**
(`<manufacturer>/<model>/<tier>`) rather than globally, so a preset chosen on a
flagship is not restored onto a budget phone — which would reintroduce item 21.

The chip row sits above the collapsible pre-capture strip and stays visible
during a recording: "this is getting hot, drop to Light" is a mid-walk decision.

### 5. D6 mount re-zero — shipped

`core/calib/MountTrim.kt`. The derivation, in full, because it is the part that
decides where every point lands:

```
  nominal, by construction:   W_R_P = q_ref  and  P_R_L = N  (the CAD nominal)
  actual, same physical pose: W_R_P = q_hold (what ARCore reports)

  the LIDAR's world attitude is the same in both, so
      q_ref · N = q_hold · (P_R_L)_actual
  =>  trim = q_hold⁻¹ · q_ref
```

With `REFERENCE_HOLD` = identity (ARCore defines its world frame from the
device's attitude at session start, so "the pose the nominal assumes" *is*
identity) the trim collapses to `q_hold⁻¹` — literally "zero out the attitude you
are holding right now", the owner's own description. **Rotation only**: a
re-clamp also changes the lever arm by millimetres and nothing in this
measurement can observe that, so `composedWith()` keeps the nominal's translation
verbatim rather than inventing a number.

`MountTrimSampler` averages ~1 s of the pose stream that is *already running*
(no extra ARCore work) with a chordal mean that aligns quaternion signs — `q` and
`-q` are the same rotation and ARCore hands out either — and **refuses** a window
that is too short, has too few samples, lost tracking, or spread more than 1.5°.
Each refusal is a sentence the panel prints verbatim.

Wiring: the extrinsic ladder is measured calibration → nominal + trim → bare
nominal. The trim is composed onto the **nominal only**, never onto a measured
calibration: a solve already contains the real bracket geometry and multiplying a
fresh attitude re-zero into it would double-count. `applyMountExtrinsic()` is
split out of `startArPipelines()` so a re-zero **mid-walk** re-pushes
`set_mount_extrinsics` and takes effect from the next resolved point.
`ProjectManifest.mountTrim` persists it, so post-processing uses the same trim.
UI: a **Set mount reference** / **Re-zero mount** pill in the D6 panel with the
trim's magnitude and age (*"Mount trim 3.4° · set 2 min ago · travels with the
project"*) and a Clear.

**Proof.** `core/…/calib/MountTrimTest.kt` — 15 tests: identity, exact 90° roll
cancellation, **upside-down (180°) including the quaternion double-cover sign
flip**, a compound 12°/−37°/94° tilt, `trim × nominal` ordering, translation
preserved, rigidity preserved, Mid-360 vs D6 nominals, plus every sampler
rejection gate and the age labels.

### 6. Files

New (`:core`): `capture/PerformancePreset.kt`, `calib/MountTrim.kt`,
`render/LivePageStoreSizing.kt`, `store/LscanManifests.kt`
(+ `test/.../store/ProjectManifestCollisionTest.kt`,
`test/.../calib/MountTrimTest.kt`, `test/.../capture/PerformancePresetTest.kt`).
New (`:app`): `ar/ArSessionGate.kt`, `debug/CaptureLog.kt`
(+ `test/.../ar/ArSessionGateTest.kt`,
`test/.../ui/capture/CaptureSealSurvivalTest.kt`,
`androidTest/.../CaptureSurvivesEngineSessionTest.kt`).
Changed (`:core`): `store/FileProjectStore.kt` (the data-loss fix),
`model/ProjectManifest.kt` (`mountTrim`, `recovered`), `capture/RigMotion.kt`
(`snapshot()`, `@Synchronized`), `capture/TrajectoryTrail.kt` (settable capacity),
`test/.../store/FileProjectStoreTest.kt`.
Changed (`:app`): `ar/CaptureArController.kt`, `ar/ArCameraBackgroundRenderer.kt`,
`ar/ArPosePumpView.kt`, `capture/TrajectoryTrailRecorder.kt`,
`data/SettingsRepository.kt`, `di/AppContainer.kt`, `engine/RealEngineBridge.kt`,
`ui/capture/{CaptureViewModel,CaptureScreen,CaptureSheets}.kt`,
`ui/projects/ProjectsListScreen.kt`,
`ui/settings/{SettingsScreen,SettingsViewModel}.kt`.

### 7. Engine seams this round needed and did not have

1. **`manifest.json` has one name and two owners.** The whole of item 20. The
   engine's `FileRecordWriter` writes `<lscan>/manifest.json` unconditionally at
   `open()` and `close()`, and Tech Spec §3.11 gives it that file — but nothing
   in the C ABI *says* so to a client, and there is no reserved-filename list a
   host app can check itself against. The app now stays out of the way
   (`project.json`), but a `scan_lscan_reserved_files()` — or simply a documented
   list in `scanengine_c.h` — would have made this collision impossible to write
   in the first place, and it survived five rounds of review.
2. **No page-store eviction, and no way to ask for it.** `PageStore` is a hard
   cap that stores nothing once full (§3 above). `scan_engine_config` exposes
   size but not policy. A live-capture ring (oldest-first) is the fix and it is
   an engine change; until then a phone's live map has a hard, silent end that
   the app can only *detect* (`page_count == max_pages`) and narrate.
3. **`pushbroom_flush()` is destructive; there is no `drain()` in the C ABI.**
   `D6PushbroomAssembler::drain()` resolves what the poses allow and *keeps* the
   rest; `flush()` force-resolves and **discards** everything still pending
   (`dropped_no_pose`). Only `flush()` is exposed
   (`scan_engine_pushbroom_flush`). So the app cannot publish the assembler's
   partial batch promptly during a live walk without throwing away points whose
   pose has not landed yet — which is why this round deliberately does **not**
   call flush periodically, and why the live map lags the recording by up to one
   4096-point batch. `scan_engine_pushbroom_drain()` would close it.
4. **No stream-provenance hint for the page store.** Pages are single-stream and
   only the tail is appended to, so two interleaved producers (D6 raw + pushbroom)
   fragment the store catastrophically at large page sizes. A per-stream tail —
   or just a documented warning next to `max_pages` — would have made §3(b)
   visible without reading `page_store.cpp`.
5. Everything ROUND 5 §9 and B3 §8 listed still stands and still bites the same
   way (no fix-shaped GNSS ingest, no phone-GNSS device kind, no live-trajectory
   getter, the 0.5 px display clamp, `SCAN_EVENT_DEVICE_HEALTH` not surviving
   `convert_event()`).

### 8. Verification

```
$ ./gradlew :core:test                       # 343 tests, 0 failures (5 skipped, pre-existing)
$ ./gradlew :app:testDebugUnitTest           # 31 tests, 0 failures  (was 13)
$ ./gradlew clean :app:assembleDebug         # BUILD SUCCESSFUL (arm64-v8a + x86_64, real engine/ C++)
$ ./gradlew :app:connectedDebugAndroidTest   # b4_test AVD, API 34 — 6 tests, 0 failures (was 4)
```

* **`:core:test` — 343** (was 306; +37). New: `ProjectManifestCollisionTest` (10),
  `MountTrimTest` (15), `PerformancePresetTest` (12) and the existing suites
  unchanged apart from `FileProjectStoreTest`'s one assertion that now names
  `project.json` (with the reason in the diff).
* **`:app:testDebugUnitTest` — 31** (was 13). New: `ArSessionGateTest` (14),
  `CaptureSealSurvivalTest` (4). Plain JVM, no Robolectric.
* **Emulator — 6/6**, including the two new real-native-engine tests that are the
  actual proof of item 20 (`assumeTrue` did not skip: `scanengine_jni` loaded and
  both ran).
* **Screenshots** (`scratchpad/r6_*.png`, emulator 1080x2400):
  `r6_01_capture` — the capture tab with the preset chip row and the
  **Set mount reference** affordance in the D6 panel;
  `r6_02_preset_light` / `r6_03_preset_full` — a preset applied, with its
  what-changed line and (for Full) the modest-device caution;
  `r6_04_settings_log` — the Settings capture-log card with path, live tail,
  Export and Clear; `r6_06_replay_recording` — a live recording with the preset
  row still present and the chip reading `BUILDING MAP…`;
  `r6_05_session_summary` — the summary reading *"Saved to
  /storage/…/synthetic-replay-demo-20c7b9.lscan — it is in the Projects tab
  now."*
* **One bug the emulator caught that no test would have**: `sealPending` was
  declared *below* `init {}`, so the new `captureState` collector dereferenced a
  null `AtomicBoolean` on the first emission and the app died on entering
  Capture. Kotlin initialisation order; fixed by moving the declaration above
  `init`, and the whole class was then re-audited programmatically for other
  forward references from `init` (none).

### 9. Explicitly NOT verified

No D6, no Mid-360, no RTK rover and **no ARCore-capable device**. So: item 1's
crash not recurring on a real phone, item 5's trim against a real bracket, item
3's live-map improvement on real D6 data, and the recovery of the owner's actual
lost captures (the recovery logic is proven on synthetic and on real
engine-written manifests, but not on his phone's specific directories) are all
verified by construction, by unit test, and — for item 2 — against the **real
native engine on device**, which is as close as this environment gets. The two
that would repay a bench hour first are (a) enabling the AR overlay on a Pixel
and confirming the inline error path rather than a crash, and (b) a D6 walk with
the mount reference set, watching the chip go `BUILDING MAP…` → `LIVE MAP · 3D`.

## ROUND 7 — SCAN QUALITY: straight walls

Scope: `android/**` plus **three additive `engine/**` changes** (§1 — the seam
that was genuinely missing; the engine suite is green at 526 cases). Triggered
by the owner declaring this the core purpose of the app — *"when i walk through
the room its not given a stable scan with straight walls"* — plus a real capture
log from a Pixel 8 Pro + COIN-D6 session (`~/Downloads/lidarscan-capture-log.txt`)
that contains two proven bugs. VERSION 0.3.0 → **0.4.0** (versionCode 40000).

Verdicts first.

| # | Item | Verdict |
| --- | --- | --- |
| 1 | Walls bend / "sections" — per-point pose pairing | **Root cause found in the D6 driver, fixed, proven twice** (engine + `:core`, independently) |
| 2 | Trim lost between captures (`trim=none` on scan-009) | **Root cause found, fixed, regression-pinned** |
| 3 | Second capture of one connect recorded nothing (`points=0 elapsedMs=0`) | **Root cause found, fixed, pinned on the emulator against the real engine** — plus a watchdog that makes the whole class loud |
| 4 | Time-sync / clock domains | **Audited: no domain bug.** One residual constant term, now measured out or exposed |
| 5 | ARCore relocalization → visible slab seams | **Detected, recorded, surfaced.** Alignment in post is the documented next step |
| 6 | Post-process path for a phone D6 | **Audited: it does not exist and cannot yet.** Gate made honest; the exact missing engine seam is named in §7 |
| 7 | Archive AR (owner directive, extended mid-task) | **Done.** ARCore survives as the invisible pose engine; the product has no AR in it |

---

### 1. WHY THE WALLS BENT — one timestamp for 178 ms of returns

This is the whole of the scan-quality complaint, and it was not where five
rounds of notes assumed it was.

**A8 was never the problem.** `pushbroom_assembler.cpp`'s `resolve_()` pops one
`ProfilePoint` at a time and calls `poses_->sample_at(p.t_mono_ns)` for **each
one**; `ExternalPoseSource::sample_at()` does a binary search for the bracketing
pair, LERPs position and shortest-arc SLERPs orientation. Per-point pose
interpolation has been correct since A8 landed.

**The per-point *timestamps* feeding it were fiction.** Two headers claim them —
`pushbroom_assembler.h`'s property #1 ("Per-point time, not per-packet time")
and `d6_driver.cpp`'s own comment ("a revolution spans 100 ms = 10 cm of rig
travel at walking pace, which is why this is a per-point callback") — and
neither the parser nor the driver ever produced one:

```cpp
// d6_parser.cpp:227, inside the per-sample loop of emit_packet()
pt.t_rx_ns = t_rx_ns;          // the same value for every sample in the packet
// d6_driver.cpp:166 / :190
void D6Driver::on_bytes(ByteSpan bytes, TimePoint t) { t_current_ns_ = t.nanos; ... }
parser_.feed(bytes.data(), bytes.size(), t.nanos);
// d6_driver.cpp:382 — and the A8 sink did not even use p.t_rx_ns
cfg_.profile_sink(..., t_current_ns_, ...);
```

Everything decoded out of one `push_serial_bytes` call claimed one instant. On
the phone that call is `D6SerialConnection`'s 4096-byte read, and **4096 bytes at
230400 8N1 is 177.8 ms of wire time — 1.8 full 10 Hz revolutions.** At 1 m/s
every chunk is laid down as one rigid slab, offset from its neighbour by up to
18 cm. That is shingled walls and it is "sections", exactly, and it is worse
while turning because the *rotation* is smeared too.

**The fix needs no new clock and no ABI.** A UART delivers bytes at a known
constant rate, so byte position inside a chunk *is* time. `D6Driver::on_bytes`
now feeds the parser in slices of `D6Config::time_slice_bytes` (default **64**,
~2.8 ms at 230400), each stamped `t_chunk_end − remaining_bytes × byte_period`,
and the A8 sink passes the **point's own** `p.t_rx_ns` instead of the driver's
"time of the current chunk". Residual smear: one slice, i.e. **2.8 mm of rig
travel at 1 m/s**, an order of magnitude under the D6's own range noise.
`time_slice_bytes = 0` restores the old behaviour and is reachable on purpose
(a replay handing over a whole file is not a fixed-rate UART).

Engine files touched, all additive: `include/scanengine/drivers/d6/d6_driver.h`
(`time_slice_bytes`, `wire_bits_per_byte`, two private methods),
`src/drivers/d6/d6_driver.cpp` (`feed_time_sliced`, `byte_period_ns`, the sink's
timestamp). No C ABI change; no struct grew.

#### The proof, twice, independently

**(a) `engine/tests/test_pushbroom.cpp` — the walking-gait wall test, against
the real assembler.** Walk 4 s at 1 m/s past a flat wall with a realistic gait
(±2 cm lateral sway and ±3 cm bob at 2 Hz, ±3° yaw and ±1.7° roll per step),
ARCore poses at 30 Hz, the D6 at 10 Hz × 360 returns, ray-cast ranges taken at
each return's TRUE time, resolved through the real `ExternalPoseSource` and the
real `D6PushbroomAssembler`, then fitted to a best-fit plane (best-fit, not the
known wall: a constant offset is a *latency* symptom, not a bending one, and must
not be allowed to masquerade as one). Three arms, same stimulus, only the
timestamps differ:

| pairing | plane-fit RMS |
| --- | --- |
| **per-point** (what ships now) | **0.041 cm** |
| one pose per 100 ms revolution | 0.59 cm |
| one pose per 178 ms USB read (what the phone did) | **2.38 cm** |

Asserted: per-point < 2 cm **and** < 0.4 cm; the 178 ms chunk arm **> 2 cm** —
the falsifiable control, so a change that quietly disables the slicing fails
loudly instead of passing both ways; and `rms_rev > 4 × rms_point`.

**(b) `core/…/calib/D6WalkingGaitPlanarityTest.kt` — the same experiment, Android
side, through the production `BracketNominals.cadNominal(COIN_D6)` matrix** and a
from-scratch reimplementation of A8 §3.1/§3.4 (`world_from_lidar =
world_from_phone(t) · phone_from_lidar`, LERP + shortest-arc SLERP). Two
independently written implementations, agreeing to the third decimal:
**0.0416 cm per-point vs 0.5805 cm per-revolution.** The control asserts the
per-revolution arm is > 4× worse.

**(c) `engine/tests/test_d6_driver.cpp` — the mechanism itself.** A 4096-byte
chunk at 230400, and the stamps the A8 sink receives must span the chunk's own
wire duration, be monotonic, never claim a time later than the chunk's arrival,
and be no more than one slice apart. Its matched control sets
`time_slice_bytes = 0` and asserts every return claims the *same* instant —
the bug, written down.

One existing assertion changed as a consequence and is called out here rather
than quietly edited: `test_capi.cpp`'s
`pushbroom_world_points_cross_the_abi_on_the_slam_map_stream` asserted
`st.t_first_ns == t`, which was only true when every point in a chunk shared one
stamp. It now asserts the invariant that survives — `t_first_ns` is inside the
chunk's own wire window and `t_last_ns == t`.

### 2. THE TRIM WAS LOST BETWEEN CAPTURES — field bug 1

From the owner's log, three good re-zeros and then, 57 seconds later:

```
22:53:04 [ar]        mount re-zero captured: magnitude=132.44deg spread=0.47deg samples=37
22:53:09 [pushbroom] extrinsic applied: source=nominal trim=132.81deg   ← scan-008, 216,653 pts
22:54:06 [pushbroom] extrinsic applied: source=nominal trim=none        ← scan-009
```

**132° of unmodelled mount rotation went straight into every point of the next
scan.** Nothing had gone wrong with the measurement (spread 0.47°, 37 samples,
the MOVING gate refusing four attempts before accepting — that machinery works).
The trim lived in one `MutableStateFlow` inside `CaptureViewModel`, which is
`viewModel(key = "capture-new-false")` on the Capture tab's own
`NavBackStackEntry`. **Walking to Projects to look at the scan you just took, and
back, clears it** — silently, with the panel then showing "Set mount reference"
as if none had ever been taken. ROUND 6 persisted the trim into the *project
manifest*, which on the Capture tab is written at the moment a project exists —
and a re-zero is taken *before* Start, when there is none.

**Fixed** by making the trim what the owner already assumed it was: a property of
the rig, not of a screen.

* `core/calib/MountTrim.kt` gains `StoredMountTrim` (the trim plus the app run
  that captured it) and `MountTrimProvenances.describe()`, which turns
  trim + run-id + now into the panel's sentence and the log's suffix.
* `SettingsRepository.storedMountTrim()` / `setStoredMountTrim()` persist it in
  DataStore — device-level, not per device profile: there is one bracket and one
  phone in the operator's hands. Unparseable JSON reads as "no trim" rather than
  throwing, so a bad row can never keep the Capture tab from opening.
* `CaptureViewModel` loads it in `init` **before** anything else, applies it, and
  writes it on every capture and clear. A trim from a previous app run is
  **still applied** — losing it silently is the bug — and the panel says
  *"restored from your last session. Re-zero only if the mount has shifted."*
  Past 12 h it becomes a caution in `SemWarn`, not a refusal.
* The age label re-computes on a 15 s tick, so "just now" becomes "3 min ago"
  without the operator touching anything.
* The capture log now carries provenance:
  `extrinsic applied: source=nominal trim=132.81deg trimAgeMs=57000 trimSource=this-run`.
  The next field report arrives with the answer attached.

**Proof.** `core/…/calib/MountTrimProvenanceTest.kt` (7 tests: this-run vs
restored vs stale, the exact stale boundary, and a JSON round-trip — because a
shape change that silently stopped decoding puts us straight back in the field
bug). `app/…/ui/capture/CaptureRound7FieldBugsTest.kt` drives the real
`CaptureViewModel` across a **ViewModel rebuild over the same store**, which is
precisely what navigating away and back is, and asserts the trim is still 132.81°
— and that `Clear` reaches the store rather than just the flow.

### 3. THE SECOND CAPTURE RECORDED NOTHING — field bug 2

```
22:53:40 [seal] sealed OK id=scan-008 … points=216653 elapsedMs=30543
22:54:06 [session] start: project=scan-009 … sensor=COIN_D6
22:54:16 [seal] sealed OK id=scan-009 … points=0 elapsedMs=0
```

**Root cause: `RealEngineBridge` latched its own transport off and never
re-armed it.** The C ABI has no pause/resume, so a D6 pause is implemented one
layer down — the reader thread keeps the port open and stops forwarding bytes
into `push_serial_bytes`. `stopCapture()` calls `pauseForwarding()` too (correct:
Stop should close the tap), and **only `resumeCapture()`, i.e. the Pause button,
ever opened it again.** So the first Stop of a connect session set
`forwarding = false` for the rest of that connect. The second Start created a
healthy `.lscan`, ran `scan_engine_start()`, got `SCAN_OK` — and received not one
byte. Zero packets → zero `POINTS_AVAILABLE` → `CaptureStats` never fires →
`points=0 elapsedMs=0`, sealed as OK, with a green Stop button the whole way.

Neither hardware-free path could ever have caught it: `ReplayEngineBridge` has no
serial connection at all and `FakeEngineBridge` has no transport. It is the
real-USB path's own state.

**Fixed:** `RealEngineBridge.startCapture()` re-arms the connection before
starting the session. One line, with the full autopsy above it in the file.

**And the class of bug is now impossible to ship silently.** A recording past
2 s with zero points raises a red `NO SENSOR DATA` banner **during the
capture** — outside the collapsible pre-capture strip, so it is readable at
exactly the moment the rest of that UI is gone — and names which half of the
chain failed, from `DeviceHealth`:

* `bytesIn == 0` → *"NO DATA — 4s into this scan and the COIN-D6 has sent 0
  bytes… re-seat the USB-C cable"*
* bytes but `packetsOk == 0` → *"98304 bytes arrived but not one valid packet
  (41 rejected). The cable is alive and the data is not the sensor's."*
* packets but no points → *"…send the capture log from Settings."*

The log line is written **before** the banner (the record must exist before the
UI can show the state), and a zero-point seal is never again just `sealed OK`:
it carries `NO-DATA=true reason="…"`, and the banner persists after the stop
reading *"THIS SCAN RECORDED NO POINTS… The project was saved so the evidence is
not lost."*

**Proof.** `CaptureRound7FieldBugsTest` (JVM): a bridge that starts and stops
successfully and delivers nothing must produce the alert *while*
`captureState == RECORDING`, with the numbers in the log and `NO-DATA=true` in
the seal; a bytes-but-no-packets variant must say something different; and a
healthy `FakeEngineBridge` capture must stay quiet for 3.5 s.
**`app/androidTest/…/D6TransportReArmTest.kt` (emulator, real native engine)** is
the one that pins the actual bug: a `UsbSerialPort` that is a byte source rather
than a device, registered through `D6UsbConnectionRegistry.register()` (a new
`@VisibleForTesting` seam — `RealEngineBridge` looks connections up by path and
there is no other way in), then the genuine
**connect → start → stop → start** sequence against a genuine `scan_engine*`,
asserting that after the second Start `connection.isForwarding` is true *and*
that reads are genuinely still happening. Against the pre-ROUND-7 bridge its last
assertion fails. A second test pins pause/resume; a third pins that chunk stamps
are `elapsedRealtimeNanos`-shaped, monotonic and shifted by the latency knob.

### 4. TIME SYNC — audited, no domain bug, one constant term

Checked end to end rather than assumed:

* **ARCore** `Frame.getTimestamp()` — nanoseconds in the `SystemClock.elapsedRealtimeNanos()`
  base, i.e. `CLOCK_BOOTTIME`.
* **The engine** — `timesync/clock.h` calls `clock_gettime(CLOCK_BOOTTIME)`
  *directly* on Android, specifically because bionic backs `steady_clock` with
  `CLOCK_MONOTONIC`, which stops during suspend. **Same domain, no conversion.**
* **A4's min-delay estimator is deliberately not on this path.**
  `TimeSync::stream_has_device_clock()` returns `false` for `kLidarD6` and
  `kPoseAr` and `true` for `kLidarMid360`/`kImu`/`kGnss`. The D6 has no device
  clock to estimate an offset against; ARCore is already in the engine's domain.
  That is correct, not a gap — the Mid-360 driver is the only caller of `add_pair`
  and that is right.

So there was no clock-domain bug. What there **was**: the app passed `t_mono_ns = 0`
("engine, stamp it on arrival"), so the stamp was taken *after* the reader thread
had copied 4 KB, crossed into native code and taken a lock — a function of how
busy the phone was. `D6SerialConnection` now samples
`SystemClock.elapsedRealtimeNanos()` as the first act after `read()` returns and
passes it explicitly.

The **variable** error — a chunk taking up to 178 ms to arrive and being handed
over at once — is removed exactly by §1's per-byte back-dating. What remains is a
genuinely constant transport delay, and a constant delay is not harmless: at
1 m/s, 20 ms translates the whole cloud 2 cm along the walk and bends corners in
proportion to turn rate. It is exposed as **Settings → Sensor timing (advanced) →
COIN-D6 sensor latency**, applied live to the open connection (next chunk, no
reconnect), with the row printing what the number *means*
(*"2 ms — shifts the cloud 0.2 cm along a 1 m/s walk"*).

**How the default was chosen (2 ms), stated honestly because no D6 exists here:**
by construction, from the two knowable terms — one full-speed USB bulk frame
(the CH340 is a bulk device; the last byte waits at most one 1 ms frame) plus one
reader-thread wake-up. ~2 ms, which is 2 mm at walking pace: an order of
magnitude under the sensor's own range noise, i.e. not a number worth arguing
about. It is a setting anyway, because that derivation assumes a healthy USB
stack and an unthrottled phone, and one afternoon with a plumb line and a
corridor beats any amount of reasoning. Range is −50…+50 ms; **negative is legal
on purpose** — if ARCore's pose stamps turn out to lag their exposure, the
correction goes the other way. `core/capture/D6TimeSync.kt` holds the derivation
and the arithmetic; `D6TimeSyncTest` pins the scale claims (4096 bytes ≈ 178 ms;
a 64-byte slice ≈ 2.8 ms ≈ under 4 mm at 1 m/s).

### 5. "SECTIONS" — ARCore relocalization, detected and recorded

Origin-at-capture-start is by design and is not the complaint. The other half is:
**ARCore's world frame is not fixed.** After a tracking loss, relocalization
corrects accumulated drift as a *step*, not a drift, and the pushbroom composes
`world_from_phone(t)` with no way to know that "world" just moved. Everything
resolved before the jump is in the old frame, everything after in the new one —
two slabs, offset by exactly the correction, seam wherever tracking hiccupped.

`core/capture/PoseSections.kt` (`PoseSectionTracker`, `PoseSectionBreak`) consumes
the same `PoseSample` stream the keyframe recorder and the mount re-zero already
read, and fires on two independent signals:

1. **Tracking regained** — measured against the last sample that was *actually
   tracking*, not the last sample seen (the poses reported during a loss are the
   tracker's own guesses and comparing to them measures nothing).
2. **A kinematically impossible step while tracking** — > 6 m/s or > 400°/s
   between consecutive poses. This catches the silent case: ARCore correcting
   drift **without ever reporting a loss**, which is the seam nobody can account
   for afterwards. Guarded by an 8 ms dt floor, because two poses 1 ms apart
   imply 30 m/s from ordinary VIO jitter.

Both thresholds are deliberately generous: a false seam costs a line in the
manifest, a missed one costs a bent room. Wired into `CaptureArController` (the
one place every pose passes exactly once), reset at each Start, surfaced inline
while walking — *"Tracking jumped — this scan is now in 3 sections… walking the
seam again with good texture in view helps"* — logged
(`SECTION BREAK #1 reason=TRACKING_REGAINED jump=0.412m/3.10deg gapMs=2100`), and
written into the manifest as `ProjectManifest.sectionBreaks` plus
`sections=N` on the seal line.

**Proof.** `core/…/capture/PoseSectionsTest.kt` — 9 tests. The two that matter
most are the negative ones: a clean 10 s walk with ±5 mm of VIO jitter and 2 Hz
gait must be **one** section, and a brisk deliberate 150°/s turn must not split
it either.

**What is NOT done, and why:** *aligning* the sections. A13's merge is exactly
this machinery one level up (two clouds, a rigid transform, ICP refinement) but
it is C++-only with **no C ABI** (A13-merge.md §9 item 3 says so), it takes
in-memory `SessionInput`s rather than `.lscan` directories, and — decisively —
§6 below: a D6 capture's cloud does not survive the session at all, so there is
nothing on disk to align. Detection and recording land now; the alignment step is
blocked behind the same seam §7 names, and this note is the handoff.

### 6. POST-PROCESSING A PHONE D6 — it does not exist, and now it says so

Traced the whole path. Tapping **Post-process** submits
`ScanEngineNative.nativeProcSubmitPostProcess` → `JobKind::kPostProcess` →
`PostSlamPipeline::run(lscan_dir)`. That pipeline is **Mid-360-only by
construction** — its decode loop counts `kLidarMid360`/`kImu` chunks and returns
`kNotFound` when there are none (`post_pipeline.cpp:254`). A D6 `.lscan` has
neither. There is no D6 branch anywhere in `processing_engine.cpp`,
`ProcessingRepository` or `ProcessingViewModel`.

So on a phone-D6 project the button was **enabled** (the gate only checked "is
there anything in `streams/`") and the job failed — and failed with the bare
string `"not found"`, because `JobQueue` discards the pipeline's own explanatory
message. An enabled button that produces a two-word error is worse than a
disabled one that explains itself, and `ProcessingJob.kt`'s own header already
said so ("every refusal has to name its own cause").

`ProcessingPolicy.postProcess` now takes the sensor and refuses a D6 project with
the truth: post-processing is the Mid-360 LIO pipeline; a D6 cloud is built
**live** by the pushbroom from the phone's trajectory, so what you saw while
walking *is* the registered result and is what Export writes; an offline re-run
needs the pose stream inside the `.lscan`, which the engine cannot write yet.
Three tests in `ProcessingPolicyTest`.

**The one concrete thing that blocks a real D6 post pipeline** — and it is small,
which is why it is worth naming precisely: `ChunkType::kPoseAr` is **defined**
(`lscan.h:76`) and **mapped** (`lscan.cpp:142`) and has **no writer anywhere**.
`Engine::push_pose` feeds the interpolator and the event bus and never touches
the recorder. So a "record-always" D6 capture stores the raw UART bytes but not
the trajectory, and the assembled cloud exists only in the live `PageStore` and
dies with the session. A8-pushbroom.md §7.4 already asks for this: *"A pose
stream chunk type for `StreamId::kPoseAr` would also let a replayed `.lscan`
re-assemble without the app — the assembler is already replay-clean, it just
needs the poses to come back off disk."* With that one writer, a D6 post job is:
replay `kD6Raw` through `push_serial_bytes` (which `ReplaySource` already does),
replay `kPoseAr` through `push_pose`, `set_mount_extrinsics` from
`manifest.mountTrim` (already persisted), `pushbroom_enable` — and §5's section
breaks become A13's inputs. Deliberately **not** attempted this round: it is an
engine feature, not a seam, and this round's engine budget went to the bug that
was bending the walls.

### 7. AR, ARCHIVED — the phone + D6 *is* the 3D lidar

Owner directive, extended mid-round: *"The AR function archive not only from UI,
work it like the 3d lidar."* The mental model is the deliverable: this is not an
AR app with a lidar on the back.

* **The AR overlay is out of the product.** The View row in the Capture-settings
  sheet now offers `3D orbit / Follow`; the overlay branch in `CaptureViewport`
  is switched off at one named constant, `AR_OVERLAY_ARCHIVED`. Nothing is
  deleted — `ArOverlayView`, `ArSessionGate` and every ROUND 6 crash fix stay
  compiled, tested and in use by the mount-calibration wizard, which genuinely
  needs to see the board through the camera. Reviving is that constant plus the
  `CameraMode.AR` entry in the View row.
* **ARCore is untouched, because it is the third dimension.** `ArPosePumpView`
  still drives `Session.update()` every frame, every pose still reaches
  `scan_engine_push_pose`, and **camera keyframes still record** — colorization
  is a product feature, not an AR one.
* **"AR" is gone from the user-facing vocabulary of the capture flow.**
  `AR & Camera` → `Tracking & camera`; `AR tracking` → `Scanner tracking`;
  *"AR unavailable — …"* → *"Phone tracking degraded — … a COIN-D6 needs tracking
  to build 3D"*; *"Too dark for AR tracking"* → *"Too dark to track"*;
  `ArAvailability`'s sentences now talk about the phone tracking its own motion.
  The one deliberate survivor is the literal product name in
  *"Google Play Services for AR needs to be installed"*, because that is what the
  user has to go and install.
* **The D6's live view already IS the 3D map view** as of ROUND 6 item 3 —
  `liveMapRequested = liveMapEnabled && (liveSlam || pushbroomActive)`, drawing
  `SCAN_STREAM_SLAM_MAP`, with the chip going `RAW · COIN-D6` → `BUILDING MAP…`
  → `LIVE MAP · 3D`. Same shape as the Mid-360 flow, which is the point.

### 8. Files

**Engine (additive):** `include/scanengine/drivers/d6/d6_driver.h`,
`src/drivers/d6/d6_driver.cpp`, `tests/test_d6_driver.cpp` (+2 cases),
`tests/test_pushbroom.cpp` (+1 three-arm case), `tests/test_jobs.cpp` (+2 bundle
round-trip cases, §7b), `tests/test_capi.cpp` (one assertion updated, §1). No
engine *source* change for §7b — the bundle format is already
`zip_export`/`zip_import`, unchanged, which is the point.

**New (`:core`):** `capture/D6TimeSync.kt`, `capture/PoseSections.kt`
(+ `test/…/capture/D6TimeSyncTest.kt`, `test/…/capture/PoseSectionsTest.kt`,
`test/…/calib/MountTrimProvenanceTest.kt`,
`test/…/calib/D6WalkingGaitPlanarityTest.kt`).
**Changed (`:core`):** `calib/MountTrim.kt` (`StoredMountTrim`,
`MountTrimProvenance(s)`), `model/ProjectManifest.kt` (`sectionBreaks`),
`jobs/ProcessingJob.kt` (sensor-aware post gate, `ProcessingMode` renamed to
"Save to phone"/"Send to cloud"/"Process here", the export refusal now names the
local path, + its tests).

**New (`:app`):** `share/DownloadsExporter.kt`,
`test/…/ui/capture/CaptureRound7FieldBugsTest.kt`,
`androidTest/…/D6TransportReArmTest.kt`,
`androidTest/…/DownloadsExporterTest.kt`.
**Changed (`:app`):** `engine/RealEngineBridge.kt` (the re-arm),
`usb/D6SerialConnection.kt` (explicit BOOTTIME stamps, latency, `isForwarding`),
`usb/D6UsbConnectionRegistry.kt` (latency fan-out, `register`),
`data/Settings{Models,Repository}.kt`, `di/AppContainer.kt` (`appRunId`, latency),
`ui/capture/{CaptureViewModel,CaptureScreen,CaptureSheets}.kt`,
`ui/settings/{SettingsScreen,SettingsViewModel}.kt`,
`ar/{CaptureArController,ArCameraBackgroundRenderer,ArAvailability}.kt`,
`ui/calib/MountCalibrationScreen.kt`,
`ui/processing/{ProcessingViewModel,ProcessingScreen}.kt`, `debug/CaptureLog.kt`.

### 9. Engine seams this round needed

1. **No `kPoseAr` writer** (§6). The single blocker for offline D6 re-assembly,
   for section alignment, and for "replay == capture" being true of a D6 cloud
   rather than only of its bytes.
2. **`JobQueue` discards the pipeline's own error message** (`job_queue.cpp:132`
   reduces everything to `error_str(status.error())`). *"post: '<dir>' holds no
   Mid-360 point/IMU chunks"* is a sentence an operator could act on;
   `"not found"` is not.
3. **The post-processed cloud is never written back to disk** — it lives in an
   in-process `PageStore`, so Colorize/Export silently re-gate to blocked after
   an app restart. A7-post.md §8 item 2 (`stream_of(kPointsXyzRgba) → kUnknown`)
   is the same gap from the other side.
4. **No C ABI for A13 merge** (A13-merge.md §9 item 3), which is what §5's
   section alignment would use.
5. Everything ROUND 6 §7 and ROUND 5 §9 listed still stands (page-store
   eviction, `pushbroom_drain()`, the `manifest.json` reserved-name list, the
   fix-shaped GNSS ingest, the 0.5 px display clamp).

### 10. Verification

```
$ cmake --build engine/build/a16 --target scanengine_tests
$ ./engine/build/a16/scanengine_tests          # 529 cases, 0 failures (7 skipped)
$ ctest --test-dir engine/build/a16 -LE "sim|sim-rtk"   # 5/5 passed
$ ./gradlew :core:test                         # 370 tests, 0 failures (5 skipped, pre-existing)
$ ./gradlew :app:testDebugUnitTest             # 36 tests, 0 failures
$ ./gradlew :app:assembleDebug                 # BUILD SUCCESSFUL
$ ./gradlew :app:connectedDebugAndroidTest     # b4_test AVD, API 34 — 12 tests, 0 failures (was 6)
```

* **`:core:test` — 370** (was 343). New: `D6WalkingGaitPlanarityTest` (2),
  `PoseSectionsTest` (9), `MountTrimProvenanceTest` (6), `D6TimeSyncTest` (6),
  `ProcessingPolicyTest` (+3).
* **`:app:testDebugUnitTest` — 36** (was 31). New: `CaptureRound7FieldBugsTest`
  (5) — the two field bugs, driven through the real `CaptureViewModel`.
* **Engine — 529 cases** (was 526), **0 failures.** New: the three-arm gait test,
  the two per-point-time driver tests (each with its own falsifiable control),
  and the two bundle round-trip tests.
* **Emulator — 12/12** (was 6). New: `D6TransportReArmTest` (3 — the transport
  re-arm against the real native engine, pause/resume, and the BOOTTIME chunk
  stamps) and `DownloadsExporterTest` (3 — the Downloads writer against a real
  `MediaStore`).

### 11. Explicitly NOT verified

No D6, no Mid-360, no ARCore device. So the numbers above are synthetic
geometry and state-machine proofs, not field measurements. The three things a
bench hour would settle, in order:

1. **The real planarity.** The owner's exported scan-008 is the right input:
   re-resolving it (or simply looking at its walls in Review after this build)
   against the 2 cm bar is the only measurement that closes §1 on real returns
   rather than on a gait model.
2. **The sensor latency** (§4). Two ms is derived, not measured. A corridor and
   a plumb line settle it in minutes, and the setting exists for exactly that.
3. **A second capture in one connect** (§3) actually recording, on the cable that
   produced `scan-009`.
4. **An export landing in Downloads** (§7b) on the owner's own phone — the
   emulator proves the writer, not that a Pixel's Files app shows it where
   expected.

The mount-bracket question ROUND 5 AUDIT §4 flagged — whether the physical
bracket matches `cadNominal`'s premise — is now much less load-bearing than it
was, because the re-zero measures the difference and (as of §2) keeps it.

---

## ROUND 8 — THE RECORDING IS THE 3D MAP

Scope: `android/**` plus **engine changes** (this round's headline was an
engine seam, and the brief said so). VERSION 0.4.0 → **0.5.0** (versionCode
50000). Triggered by one sentence from the owner, on a real Pixel 8 Pro +
COIN-D6 session:

> "When i check the recording, it still show a 2D scan. i need a 3d mapping."

and, mid-round, by two more:

> "i need a live 3d mapping too"
> "will the follow still work? the camera is facing forward while the scanning
> is the two side"

Unlike every previous round, this one had **the owner's actual data**: a sealed
0.4.0 export (`captures/scan-015-pixel-0.4.0.lscan/`, 191,381 points over
26.3 s) and the capture log that produced it
(`captures/pixel_capture_log_2026-08-18.txt`). Both are checked in, and three
of the findings below were measured out of them rather than reasoned about.

Verdicts first.

| # | Item | Verdict |
| --- | --- | --- |
| 27 | Recorded D6 scans must BE 3D | **Root cause was the seam ROUND 7 named. Fixed end to end, proved on the reopened project** |
| — | The Projects thumbnail was 50 % raw 2D fan | **Found by measuring the owner's own export. Fixed, pinned against that file** |
| — | `sensors: []` and `mountCalibration: null` in every `.lscan` ever written | **Two engine bugs, both fixed — `add_sensor()` had existed since A5 with no caller** |
| 28 | Capture layout, live view >= 60 % | see §7 |
| 29 | Display defaults | see §7 |
| 30 | "Set mount reference look not working" | **Root cause found in the owner's log: the gate refused 7/7 on 0.4.0** — see §7 |
| 31 | Post-capture flow | see §7 |
| — | Live 3D map on by default for D6; third-person Follow | see §7 / §8 |

---

### 1. WHY A SAVED SCAN WAS NOT 3D — and it was not one bug, it was three

ROUND 7 §9 item 1 named the first one precisely and then, correctly, declined
to fix it inside a bug-fix round. It is this:

```cpp
// engine/src/core/engine.cpp, before ROUND 8
Status Engine::push_pose(const Pose& pose) {
  const Status s = impl_->poses->push_pose(pose);   // the interpolator
  if (!s.ok()) return s;
  publish_pose_(pose);                              // the event bus
  return kOkStatus;                                 // ...and nothing else
}
```

`ChunkType::kPoseAr` was **defined** (`lscan.h:76`), **mapped**
(`lscan.cpp:142`) and had **no writer anywhere**. Record-always — Tech Spec §3
key rule 2, "every raw stream hits the recorder before any processing" — was
true of every stream except the one that matters most.

It matters most because **a COIN-D6 is a 2D lidar**. The third dimension of a
D6 scan is *entirely* the phone's trajectory: A8 resolves each return as
`world_from_lidar(t) = world_from_phone(t) · phone_from_lidar`. So a D6
`.lscan` holding the raw UART bytes and not the poses has recorded a pile of
range-and-angle pairs from which **the 3D result can never be rebuilt by
anyone** — not by a later version of this app, not by the desktop shell, not
by the cloud worker. The assembled cloud lived only in the live `PageStore`
and died with the session.

That is the owner's sentence, exactly. There was nothing three-dimensional on
disk to show him.

**The second bug** was that nothing on the Android side would have drawn it
even if it had been there. `ReviewViewModel.cloudSource` is
`ProcessingCloudSource` — the processing engine's `PageStore` — and the only
thing that ever filled that store was a post-process job, which **refused a D6
project** (ROUND 7 §6 made the refusal honest; it could not make it work).
So Review was an empty box with a paragraph telling the operator to run a
button that would not work.

**The third bug is the one he was actually looking at**, and it was found by
measuring his export rather than by reading code. `processed/preview.f32` —
the file the Projects tab draws a scan's tile from:

```
  4,040 points
  2,027 with z == 0.0f EXACTLY   (50.2 %)
  2,013 with z from -4.90 to +0.10 m
      0 non-finite
```

Exactly half. Not corruption — every value is a perfectly good float, which is
why four rounds of tests missed it. It is **two streams superimposed**. A D6
capture with the pushbroom running holds two point streams in one `PageStore`
(INT24-wiring.md §2): `SCAN_STREAM_LIDAR_D6`, the driver's raw sensor-frame
preview whose returns lie in the lidar's own scan plane **by construction**
(hence `z == 0` to the bit), and `SCAN_STREAM_SLAM_MAP`, the resolved room.
The live renderer has filtered between them since B3 (`StreamFilter`).
`writeProjectPreview` never did. So the tile a user judges a scan by was the
3D room with a flat 2D disc drawn through it at 1:1 — which, at 108 dp,
reads as a flat 2D scan.

### 2. THE ENGINE — record-always finally applies to the trajectory

Five additive changes, no ABI break (the C ABI is untouched; `SCAN_ABI_VERSION`
stays 7 — nothing new needed to cross it, because every consumer that needed
the new capability links the C++ API directly, as B4/B3/B6 already do).

1. **`Engine::record_pose_()`** — `push_pose()` writes a `kPoseAr` chunk into
   the active recording. After the interpolator accepts it (a pose it rejects
   is not part of the trajectory and recording it would make a replay diverge
   from the capture) and before the event bus hears about it (whatever a
   consumer reacts to must already be durable — the same ordering
   `push_serial_bytes` uses). Locked on the same `record_m` the D6 raw path
   takes, because the ARCore pump thread and the USB reader thread genuinely
   do write concurrently.

   **Payload**: a fixed 68-byte little-endian record —
   `lscan::PoseChunkRecord` + `encode_pose_chunk`/`decode_pose_chunk`
   (position f64[3], orientation f64[4], two sigmas f32, source/quality/
   tracking_lost/reserved u8). The stamp is NOT in the payload: a chunk
   already carries `t_mono_ns` and duplicating it would let the two disagree.
   The floats are written through shift-and-mask like the integers, not
   `memcpy`'d, so the file is byte-identical on a big-endian host — the format
   contract says "all little-endian", and a chunk that is only correct because
   x86 and arm64 happen to agree is not a format.

   **Cost**: 68 B × 30 Hz = **2.0 KB/s**, 7.3 MB over a one-hour walk, against
   the ~200 KB/s of raw D6 UART the same session already writes. Under 1 %.

2. **The resolved cloud is cached into the container.**
   `stream_of(ChunkType::kPointsXyzRgba)` returned `kUnknown` — A7-post.md §8
   item 2 and ROUND 7 §9 item 3 are the same gap from two directions — so a
   processed cloud could not be written into a `.lscan` at all. It now maps to
   `StreamId::kSlamMap`, which gets its own file, `streams/map.bin`.

   Its own file, not `lidar.bin`, for a reason that is easy to miss: replaying
   `kD6Raw` means walking `lidar.bin`, and interleaving a 16-byte-per-point
   vertex stream into it would make every replay read and CRC tens of
   megabytes it immediately discards.

   The write is deliberately narrow — **only while the pushbroom is on**, i.e.
   only for a D6 session. At the COIN-D6's ~3,600 pts/s that is 57 KB/s
   (~8 % on top of the raw UART) and it makes "open a saved scan" instant
   instead of a re-resolve. A Mid-360's 40k pts/s map would cost 640 KB/s for
   a map that A7 will discard and rebuild better; that trade does not hold and
   is not taken. **It is a cache, not the source of truth**: Process
   re-resolves from `kD6Raw` + `kPoseAr` and overwrites it, and deleting
   `map.bin` costs speed, never data.

3. **`FileRecordWriter::set_mount_calibration()`** — `"mountCalibration"` had
   been a reserved `null` since A5, and the owner's real manifest shows what
   that costs: without `phone_from_lidar` a D6 project is **not
   self-contained**, because the returns are in the lidar's own frame and
   nothing else says where that frame sat on the rig. `Engine::start_session`
   now writes whatever `set_mount_extrinsics()` last accepted.

4. **`sensors: []` — in every `.lscan` this engine has ever written.**
   `add_sensor()` has existed since A5 and **nothing ever called it**. Found in
   the owner's manifest, not in a test. `start_session` now enumerates the
   registered devices into it. That is not cosmetic: it is how a reader decides
   what a container holds without decoding it.

5. **`ReplaySource` replays the trajectory too.** `kPoseAr` chunks dispatch to
   `Engine::push_pose()` rather than `push_serial_bytes()` — a different entry
   point, so they ride *alongside* `cfg.chunk_type` rather than instead of it.
   `ReplayConfig::replay_poses` defaults to **true**, and that is provably not
   a behaviour change: nothing wrote a `kPoseAr` chunk before ROUND 8, so no
   `.lscan` in existence contains one.

### 3. THE OFFLINE PIPELINE — `post::D6ResolvePipeline`

New: `engine/include/scanengine/slam/post/d6_resolve.h` +
`engine/src/slam/post/d6_resolve.cpp`.

**Why it is not "A7 with a D6 branch."** The two pipelines share a name and
nothing else. A7 *estimates* a trajectory from lidar and IMU, so its expensive
stages (loop detection, pose-graph optimization, re-integration) exist to make
that estimate better. A D6 estimates nothing: its trajectory was **measured**
by ARCore during the walk and is now on disk. What is left is the arithmetic
A8 already owns, per point, with the pose interpolated to that point's own
timestamp. No graph, no second pass — it runs in about the time it takes to
read the file.

**It drives the production classes, not a reimplementation**: the real
`D6Driver` (so ROUND 7's per-byte time slicing — the fix that made walls
straight — applies identically offline), the real `ExternalPoseSource` (same
LERP/SLERP, same staleness and tracking-loss gates) and the real
`D6PushbroomAssembler`. Nothing here re-derives geometry; if it did, the
offline result could drift away from the live one and nobody would notice.
That is also what makes "replay == capture" true of a D6 **cloud** rather than
only of its bytes.

**Routing is read from the container, not declared by the caller.**
`jobs::run_post_process()` calls `post::lscan_is_d6_project()` and picks the
pipeline. A `.lscan` already knows which sensor wrote it — the chunk types say
so — and making the caller state it would add a way for the two to disagree,
whose failure mode is a job running the wrong pipeline and reporting
"not found". `PostProcessParams` gains an optional `d6_mount[16]`; unset, the
pipeline reads the extrinsic out of the container's own manifest.

**A project with no poses fails, in words.** `ScanError::kNotFound` with a
message naming the version, and `stats().poses_read == 0` as the
machine-readable half. It never silently resolves through identity: a D6
mounted at ~130° to the phone (the owner's rig) resolved through identity
produces a *confidently wrong room*, which is worse than a refusal.

`post::load_recorded_cloud()` is the Review fast path — the cached cloud, no
decode, no interpolation. A container without one is not an error; it returns
`kOk` with 0 points and the caller falls through to the re-resolve.

### 4. THE PROOF — `engine/tests/test_round8_d6_reopen.cpp` (6 cases)

Records a synthetic walk through a **real `Engine` with a real
`FileRecordWriter`**, seals the container, throws the engine away, and reopens
the directory from disk. Same gait model, same mount and same plane fit as
ROUND 7's wall test — the difference is that this one goes through a file.

Measured, on the REOPENED project:

| | |
| --- | --- |
| chunks sealed | 720 `kD6Raw`, **120 `kPoseAr`**, 1 `kPointsXyzRgba` |
| points resolved | 2,456 |
| **wall plane-fit RMS** | **0.052 cm** (bar: 2 cm) |
| extent along the walk | 4.05 m (a 4 s walk at 1 m/s) |
| extent floor-to-ceiling | 2.84 m |
| **points at exactly z == 0** | **0** (a raw fan gives 100 %; the owner's preview gave 50 %) |
| live vs reopened | **0 mismatched points** of 2,456 |
| live vs replayed | **0 mismatched points** |

and the two falsifiable controls, which matter as much:

* **delete `poses_ar.bin`** (i.e. make it a pre-0.5.0 capture) → the resolve
  refuses with `kNotFound`, `poses_read == 0`, and a message naming 0.5.0.
* **replay the bytes with `replay_poses = false`** → 0 world points, 2,456
  returns dropped for want of a pose. Same recording, same assembler, no
  geometry: *the trajectory is the geometry*.

Plus the manifest assertions that came from the owner's file: `sensors` is not
`[]`, it names `coin-d6`; `mountCalibration` is present and round-trips to the
matrix that was applied.

### 5. ANDROID — opening a saved scan (item 27c)

`ProcessingEngine::probe_project()` reads what a container actually holds
(D6 / poses / cached map / mount) **off the bytes**, not off the app's sidecar
manifest — a project can arrive by import, transfer bundle or ROUND 6's
manifest recovery, and the question being asked is precisely "was this
recorded by a version that stored poses".

`ReviewViewModel` then takes one of three paths and **says which one it is
on**, because they have very different latencies and an operator who knows
which is running does not think the app has hung:

1. `LOADING_RECORDED` — read the cached cloud. A file read.
2. `RESOLVING` — re-resolve from returns + trajectory + extrinsic. Seconds,
   and the room draws *while it is being built* (the points poll flips to
   READY on the first page, at 250 ms).
3. `NO_TRAJECTORY` — the honest one, and the exact wording the brief asked
   for: *"Recorded before trajectory storage — showing raw sensor view"*,
   followed by why it can never be fixed for that file and what changed.

The mount precedence is: the project's measured calibration, else its stored
trim composed onto the CAD nominal, else (null) the container's own manifest.
The app's is preferred because the operator's persisted re-zero is fresher
than a manifest written when the capture started.

`hasProcessedCloud()` now also counts a project opened from its cache, so
Colorize and Export gate on "is there a cloud" rather than "did a job run" —
they read the same `PageStore` either way.

New JNI: `nativeProcProbeProject` (a bitfield, one call, decoded by
`ProjectProbe.of` — the Review screen makes it on every open),
`nativeProcOpenRecordedCloud`, `nativeProcClearCloud`, and
`nativeProcSubmitPostProcess` gains a nullable 16-double mount whose length is
**checked, not assumed** (a 16-double contract crossing JNI is exactly where a
silent truncation would go unnoticed).

### 6. THE THUMBNAIL — `core/render/PreviewSanity.kt`

Two fixes, because the preview is written once per capture and read on every
list scroll for the life of the project, so a bad one is effectively permanent.

* **The cause**: `writeProjectPreview` now mirrors `StreamFilter.MAPPED_ONLY`
  exactly, fallback included — prefer the resolved stream, fall back to raw
  only when no mapped page exists at all (a Record-only session genuinely has
  nothing else, and a blank tile is worse than an honest sensor-frame one).
  `PointCloudSource.samplePoints()` gained a stream predicate and
  `streamsPresent()`; asking the store what it HAS rather than inferring from
  the `liveSlam`/`pushbroomActive` flags is deliberate — those say what was
  requested.
* **The guard**: a verdict on the numbers, applied on write *and* on read.
  Non-finite values, implausible extents (the uninitialised-memory case: ~1e38
  values are finite and pass every naive check), and — the one that matters —
  more than a third of points sitting at exactly `z == 0`, which is the raw
  fan's signature. It runs on read too because 0.4.0-era previews are already
  on people's phones, including the owner's, and this app cannot rewrite them;
  such a tile falls back to the seeded placeholder, which is visibly
  not-your-data (dimmer, no trajectory head).

`PreviewSanityTest` (7 cases) is a **characterisation test against the checked-in
real export**: it asserts 4,040 points, 2,027 exact zeros, 0 non-finite, and
that the shipped file is refused with a reason naming the raw fan. Plus the
required false-positive control — 15 % of points on the floor plane must NOT
trip the gate, because a thumbnail is not worth a false alarm.

(One correction to the round's own brief, stated because it changed what was
looked for: the export's preview was reported as containing "garbage floats,
extents ~6.4e38". It does not. That reading was little-endian;
`DataOutputStream` writes big-endian, and read correctly every value is finite
and in a ±3 m room. The real defect was subtler and worse — half the points
were the wrong stream.)

### 8. THE FOLLOW CAMERA — "the camera is facing forward while the scanning is the two side"

The owner is right, and the observation is sharper than it looks. A COIN-D6 is
a vertical fan on the back of the phone, **across** the direction of travel: it
paints a ring around the operator — left wall, ceiling, right wall, floor — and
**never anything ahead**. A first-person forward-facing follow camera would
frame empty space, and the FOLLOW mode that existed was not a follow at all:
it was a fixed offset from the whole cloud's bounding-box centroid, so it
zoomed steadily out as the walk grew and stopped following anything.

`core/render/FollowCamera.kt` (new, pure `:core`, no Android or Filament types
— same reasoning `calib/MountTrim.kt` gives for being pure) is a **third-person
chase camera**: behind along the negative walk heading, above, pitched down
35°, looking at the rig, distance fitted to the RECENT geometry only.

The four choices that matter, with their numbers:

* **Heading comes from the trajectory, never from phone yaw.** The heading is
  the chord between two halves of a **1.0 s window — exactly two gait cycles at
  ROUND 7's 2 Hz** — so the periodic sway averages to zero and never enters the
  heading at all. Measured wobble: **0.062° peak-to-peak** on a gait trail.
  Below a 0.05 m baseline (a tenth of walking pace) it **holds the last
  heading** rather than dividing by a near-zero displacement.
* **Smoothing time constants** are chosen against the gait, not by feel:
  position τ = 0.5 s gives a first-order gain of 0.157 at 2 Hz, turning ±3 cm
  of bob into ±4.7 mm. Measured: lateral **13.95 → 1.44 mm RMS (9.7×)**,
  vertical **20.93 → 3.58 mm (5.85×)**. Heading τ = 0.6 s is deliberately
  slower than position (rotation is more nauseating than translation); a 90°
  corner is 95 % converged in 1.8 s. Distance τ = 1.0 s is slowest, so the view
  does not pump in and out at every doorway.
* **A velocity lead term** cancels the low-pass's own `τ·v` steady-state lag
  exactly — without it the camera trails the rig by half a metre at walking
  pace; with it the target error is 16 mm. The velocity estimate is the same
  half-window chord, so it carries no gait energy either.
* **Distance is fitted to recent geometry**, `d = R / sin(vFov/2)`, clamped to
  1–8 m. The clamp is what keeps this a *recent* fit instead of the old
  whole-cloud zoom-out: measured flat at 5.23 m across a 60 m walk.
  **Vertical** FoV, not `min(vertical, horizontal)`, because the phone is
  portrait and horizontal containment would push the camera ~3× further back.

`FollowCameraTest` — 14 cases, including the ones that matter most: the
degenerate set (empty trail, single pose, stationary rig, non-finite input, a
rewound clock, a long stall) must all produce a finite camera and never a NaN
or a jump cut, and a stopped rig must hold its heading rather than spin.

**World frame: Y-up, stated explicitly rather than assumed.** The runtime frame
is ARCore's — `publishPose` pushes `camera.pose` verbatim, so `PointVertex`'s
"session local metric frame" *is* ARCore's world frame, and `TrajectoryTrail`
and the existing ORBIT up vector agree. (The engine's +z-up appears only in
`test_pushbroom.cpp`'s synthetic wall, which is test-fixture geometry, not a
device frame.) `FollowCamera` takes the axis as configuration and never
guesses.

3D orbit is untouched and remains the free camera.

### 7. THE CAPTURE SCREEN — items 28, 29, 30, 31

#### 7.1 Item 30: "Set mount reference look not working"

It was not "look". **The gate was refusing every attempt.** The owner's log,
on the ROUND 7 build:

```
00:50:43 [ar] mount re-zero refused: MOVING
00:51:05 [ar] mount re-zero refused: MOVING
00:51:16 [ar] mount re-zero refused: MOVING
00:51:23 [ar] mount re-zero refused: MOVING
00:51:24 [ar] mount re-zero refused: MOVING
00:51:25 [ar] mount re-zero refused: MOVING
00:51:26 [ar] mount re-zero refused: MOVING
```

Seven attempts in 44 seconds and **not one success on that build** — and so
scan-013, scan-014 and scan-015 all ran `trim=none`. (The successful re-zeros
earlier in the same file are from an older build: the seal lines there have no
`sections=` field, which ROUND 7 added, so the log spans two versions.)

The old gate compared `spreadDeg = window.maxOf { deviation from mean }`
against a single `MAX_SPREAD_DEG = 1.5` over a 1200 ms window — i.e. **every
one** of ~37 consecutive ARCore frames had to land within 1.5° of the mean.
One bad feature match, one footstep travelling up an arm, one post-
relocalisation yaw correction vetoes the whole hold. It is not a steadiness
test, it is a worst-frame test.

Now two numbers instead of one:

* `MAX_SPREAD_P90_DEG = 2.5` on the **p90** deviation — steadiness with the
  top decile of VIO jitter excluded. Against the 0.47–0.82° *max* the 0.3.0
  successes measured, a hold as good as the ones that already worked clears
  this by 3×.
* `MAX_SPREAD_OUTLIER_DEG = 6.0` on the **max** — the falsifiable half, which
  is why relaxing the first number is safe. Motion is a trend, not a tail: a
  rig carried at 12°/s puts its extremes ±6° from their own mean over a 1 s
  window, so it still fails.
* `WINDOW_MS` 1200 → 1000 and `MIN_SAMPLE_SPAN_MS` 800 → 700, so a steady hand
  is done in ~1.5 s door to door. `MIN_SAMPLES` and the `NOT_TRACKING` gate are
  untouched — those were never the problem.
* `MountTrim.spreadDeg` keeps its ROUND 6 meaning (the worst frame) so old
  field log lines and persisted JSON stay comparable; `spreadP90Deg` is added
  alongside with a default, so a 0.4.0-persisted trim still decodes.

**Refusals now carry their measurement**, in the log and on screen:

```
mount re-zero refused: MOVING p90=2.90deg max=5.10deg limit=2.50deg
                              outlierLimit=6.00deg samples=31 spanMs=980
```

and the panel shows the numbers **plus what to do** ("Brace the phone against
your body, hold still ~1 s") in a loud amber banner rather than a grey hint —
the next field report arrives with its own diagnosis attached, which is the
same principle ROUND 7 §2 applied to the trim's provenance.

**The persistent state** (item 30c) is a `MountStateRow` that is part of the
compact chrome rather than something behind a sheet:
`MOUNT SET · 132.8° · 2 min ago`, or `NO MOUNT REF · CAD NOMINAL`. The age
re-ticks off the existing 15 s provenance tick.

**And one more bug found while proving item 30d.** `applyMountExtrinsic` sat
inside `startArPipelines`, behind an `arController ?: return` — so a session
with no camera controller recorded 2D and **logged nothing at all about it**.
It moved into `startCapture`, and it now logs even when there is no engine
handle, under a deliberately different verb
(`extrinsic resolved (no engine handle): …`) so that a field log can never be
read as "the engine is using this".

#### 7.2 Item 28: the live view keeps >= 60 %

`app/ui/capture/CaptureLayout.kt` (new, pure Kotlin, no Compose, JVM-testable)
holds `MIN_VIEWPORT_FRACTION = 0.60f` and a dp band budget — mount row 46,
chip row 46, transport 80, tab-bar clearance 86 = **258 dp of fixed chrome**
— and it is *used*: `viewportMinHeightDp()` feeds the viewport's
`heightIn(min = )`. The guarantee is then arithmetic rather than a hope,
because the viewport is the column's only weighted child.

Measured before: ~370 dp of fixed chrome at 800 dp, plus a pre-capture strip
capped at 46 % — the live view could be **60 dp**.

What was collapsed: the `BackBar` (the floating tab bar already goes to
Projects), the RTK chip strip (now shown only when there is RTK to report),
the `PERFORMANCE` label and its three preset buttons, the whole pre-capture
strip, and the four-cell stat panel with its spacers (~80 dp) which is now one
mono line inside the transport row. In their place: one mount row and one chip
row — `[Capture · Optimal] [Display] [Diag]` — plus a new capture-settings
sheet.

**Measured after, on the emulator in the connected state:** the viewport spans
1350 of 2000 px = **67.5 %** (`screenshots/round8/05-capture-connected.png`).

Two honest limits, both documented in the file rather than in a commit
message. The compact form is keyed off **connected**: a disconnected Capture
screen's job is the connect flow and it needs the room, so the auto-detect
line, manual entry and the mount explanation stay expanded there
(`screenshots/round8/04-capture-mount-state.png`). And the 60 % rule cannot
hold below ~660 dp of portrait height — the transport row plus the 86 dp
tab-bar clearance exceed 40 % on their own — so `REFERENCE_SCREEN_HEIGHT_DP`
names that floor and below it the viewport takes everything left over (~57 %
at 640 dp) rather than over-constraining the column and measuring the record
button to zero.

#### 7.3 Item 29: display defaults

`DisplayParams.captureDefaults()`: colour mode **INTENSITY**, point size
**1.0 px**, gamma **1.0**, brightness **1.0**; 30 fps was already OPTIMAL's
live refresh. `PerformancePresets.displayDefaultsFor()` exposes it per preset
and returns the same values for all of them — presets own *performance*, not
taste (ROUND 6 item 22).

The owner's `project.json` recorded `"pointSize": {"fixedPx": 2.5}`, which is
what became 1.0 — and reading that same object turned up a bug nobody had
asked about: it also said `"mode": "ADAPTIVE"`. The ViewModel built
`PointSizeParams(fixedPx = size)` and left `mode` on its default, so a renderer
honouring the mode never reads `fixedPx` at all and **the point-size slider had
no defined effect**. Mode is now explicitly `FIXED_PIXELS`.

One clarification against the brief's wording: "colormap = INTENSITY" is a
colour *mode*, not a colormap. `ColorMode.INTENSITY` is set; `Colormap` stays
SPECTRUM.

#### 7.4 Item 31: the post-capture flow

`CaptureViewModel.sealedProjectId` is a `SharedFlow<String>` emitted **only on
the verified seal path** (the one ROUND 6 made re-read the project back through
the store). `CaptureRoute(onScanSealed = )` hands it to `LidarScanApp`, which
sets `activeProjectId` and switches to Projects; `ProjectsListRoute` takes a
new `initialSelectedId` and adopts it **once**, keyed on the id — keyed rather
than unconditional because the operator must stay in charge afterwards, and a
card that springs back open on every recomposition is worse than one that never
opened.

The Capture tab's re-arm now also clears the scan name, zeroes the stats and
resets the section count, so returning to it gives a fresh auto-name and a
ready Start. Connection, auto-connect state, preview source, preset, display
params and **the mount trim** are deliberately untouched — re-zeroing between
two scans of the same room is exactly what ROUND 7 §2 fixed.

#### 7.5 The live 3D map, and the live view during a D6 capture

* `PerformancePresets.liveMapDefault(preset) = preset != LIGHT` — on by
  default in every preset but LIGHT, now asserted for every preset × tier ×
  display-ceiling combination rather than being an emergent property.
* `_cameraMode` defaults to `FOLLOW` for a D6 capture (ORBIT for a replay
  session, which has no rig to follow).
* **The live map does render with `trim=none`**, which the round brief asked to
  be checked. `applyMountExtrinsic` resolves
  `measured ?: trim?.composedWith(nominal) ?: nominal`, so a null trim falls
  through to the CAD nominal, the extrinsic is still pushed,
  `pushbroom_enable` still runs, `_pushbroomActive` goes true and
  `liveMapRequested = liveMapEnabled && (liveSlam || pushbroomActive)` is true.
  The owner's own log is the proof —
  `extrinsic applied: source=nominal trim=none pushbroomEnabled=true` — and it
  is now pinned by a test rather than inferred.
* Related, and worth stating because the brief read it the other way:
  **`liveSlam=false` under `preset=OPTIMAL` is not a bug and is not "live map
  off"**. `liveSlam` is the Mid-360's LIO switch; a D6's live map is the
  pushbroom, and the same log line's own suffix says so.

### 9. VERIFICATION

```
$ cmake --build engine/build/a16 --target scanengine_tests
$ ./engine/build/a16/scanengine_tests            # 535 cases, 0 failures (7 skipped)
$ ctest --test-dir engine/build/a16 -LE "sim|sim-rtk"   # 5/5 passed (incl. capi smoke)
$ cmake --build desktop/build                    # BUILD SUCCESSFUL (engine changes are additive)
$ ./gradlew :core:test                           # 405 tests, 0 failures (5 skipped, pre-existing)
$ ./gradlew :app:testDebugUnitTest               # 55 tests, 0 failures
$ ./gradlew :app:assembleDebug                   # BUILD SUCCESSFUL
$ ./gradlew :app:connectedDebugAndroidTest       # b4_test AVD, API 34 — 14 tests, 0 failures
```

* **Engine — 535 cases** (was 529). New: `test_round8_d6_reopen.cpp` (6) — the
  sealed container's contents, the reopened 3D room, the recorded-map fast
  path, and the three controls.
* **`:core:test` — 405** (was 370). New: `FollowCameraTest` (14),
  `MountTrimGateTest` (10), `PreviewSanityTest` (7), plus `DisplayParamsTest`
  (+2) and `PerformancePresetTest` (+2).
* **`:app:testDebugUnitTest` — 55** (was 36). New: `CaptureLayoutTest` (8),
  `ProjectProbeTest` (6), `CaptureRound8FlowTest` (5).
* **Emulator — 14/14** (was 12). New: `ReopenedD6ProjectIs3dTest` (2) — the
  reopened-project-is-3D proof against the real native engine, and the
  pre-0.5.0 control.

**Screenshots** (`android/screenshots/round8/`): `01-projects.png` /
`02-selected.png` (a recorded 0.5.0 D6 project listed and selected),
`03-review-3d.png` (**Review drawing the resolved 3D cloud, 2,456 pts** — the
answer to owner item 27), `04-capture-mount-state.png` (the Capture tab
disconnected, i.e. the setup state), `05-capture-connected.png` (the connected
state, **viewport 67.5 % of screen height**, settings collapsed to three
chips, Orbit/Follow on the viewport).

The Review screenshot was produced by pushing a container the engine test
itself recorded (`test_round8_d6_reopen.cpp`'s walk) onto the emulator and
opening it in the app — i.e. the bytes in that screenshot are the same bytes
the 0.052 cm plane fit was measured on.

### 10. EXPLICITLY NOT VERIFIED

Still no D6, no Mid-360 and no ARCore device here, so:

1. **The mount gate on a real hand.** The new thresholds are derived from the
   owner's own logged numbers (7/7 refusals at the old limit; 0.47–0.82° on the
   holds that used to pass) and tested against a jitter model, not against a
   hand. The first field session either passes in ~1.5 s or arrives with
   `p90=`/`max=`/`limit=` in the log, which is the point of putting them there.
2. **The `MOUNT SET · … ` chip could not be screenshotted.** It renders only
   for a connected COIN-D6 with pose tracking, which an emulator cannot
   provide; it is covered by unit tests on `chipLabel` and by
   `CaptureRound8FlowTest`, not by a picture.
3. **The follow camera on a real walk.** Its smoothing is measured against the
   ROUND 7 gait model (9.7× lateral, 5.85× vertical), which is a model.
4. **Re-resolving the owner's own scan-015 is impossible and always will be** —
   it has no trajectory. The first 0.5.0 capture is the one that closes item 27
   on real returns rather than on synthetic ones.
5. **Recording overhead on a phone.** The map cache adds ~57 KB/s and the pose
   stream ~2 KB/s by construction; neither has been measured against a real
   phone's storage under thermal load.

---

## ROUND 9 — THE MIRROR, AND TWO CLOCKS THAT DID NOT AGREE

Scope: `android/**` and `engine/**`. Triggered by the owner's verdict on 0.5.0 —

> "This version is much better ... The output is left right reversed"

— arriving with the **first true-3D phone capture**, `captures/scan-017-pixel-0.5.0.lscan.zip`:
`kD6Raw` + `kPoseAr` (150 chunks, 68-byte payloads) + `kSlamMap` (15,631 points),
`mountCalibration.phoneFromLidar` det = +1, mount trim 89.82°. Plus the owner's
own spec sheet for the D6 and the insight that *"lidar data and the imu position
data need sync the frequency"*.

VERSION 0.5.0 → **0.6.0** (versionCode 600 — `major*10000 + minor*100 + patch`,
per `android/app/build.gradle.kts:118`; note ROUNDS 6–8 wrote 30000/40000/50000
in this file, which is 100x the value the build actually produces).

Verdicts first.

| # | Item | Verdict |
| --- | --- | --- |
| 34 | Output is left-right mirrored | **Root cause found in the vendor datasheet, fixed at the one wrong sign, proven by the first chirality-sensitive test in the tree** |
| 35 | Pose/lidar rate sync — densify with the phone IMU | **Implemented; 97.3% of the recoverable error closed on the falsifiable bench** |
| — | D6 per-sample timestamping (owner's spec numbers) | **Refined: samples are now dated when TAKEN, on a min-delay sample clock** |
| 33 | Capture flow leaves nothing behind | **Fixed — and the real stray source was not the one suspected** |
| 32 | Legacy rescue for pre-0.5.0 scans | **Design sketch only, as asked — §6** |

---

### 1. ITEM 34 — THE MIRROR

#### 1.1 What it was, in one sentence

**The vendor states the D6's angle convention in a LEFT-HANDED coordinate
system, the engine read it into a RIGHT-handed one, and that silently reverses
which way the beam sweeps.**

`docs/bench/BENCH_SETUP.md` §3.1 has been quoting the manual since Phase 0:

> "left-hand coordinate system ... rotation angle increases clockwise ...
> zero-degree direction marked in the figure"

Both halves of that sentence matter and only one was ever acted on. S1 took the
datasheet's `(x, y)` pair verbatim into `x = d·sin θ, y = d·cos θ`. Keeping `x`
and `y` while the handedness flips **reverses the sense of rotation about the
third axis**. The datasheet's figure is a top view — the sweep is clockwise seen
from the **cap** — and the shipped code implemented clockwise seen from the
**base**.

#### 1.2 Why eight rounds of tests never saw it

Two independent reasons, and both are worth remembering.

**(a) A planar fan's mirror is a proper rotation.** Every D6 return has `z == 0`
*exactly*. Restricted to that plane, the reflection `x → −x` is identical to
`diag(−1, +1, −1)` — a **det = +1** rotation of 180° about the fan's own 0° axis.
So A8 §4.4's rigidity guard (`se3::mat4_is_rigid`), which exists precisely to
catch "a plausible-looking but mirrored cloud", could not fire; and the owner's
`scan-017` manifest storing a det = +1 extrinsic proved nothing at all. The
ROUND 9 brief's premise that "a mirrored FAN cannot be fixed by any rotation" is
true for a volumetric sensor and **false for a planar one** — that difference is
the whole trap.

**(b) Every geometry test in both trees measured a sign-blind quantity.**
`D6PushbroomGeometryTest` asserts axis extents. `D6WalkingGaitPlanarityTest`,
ROUND 7's engine gait test and ROUND 8's reopen test all assert best-fit plane
RMS. Others assert point counts and live-vs-offline equality. **A mirrored room
has identical extents, identical planarity and identical point counts.** There
was no test in the repository capable of distinguishing a room from its
reflection.

#### 1.3 The frame, written down once

New file `engine/include/scanengine/drivers/d6/d6_fan.h` — the only place the
conversion is computed, and it carries the full derivation:

```
D6 fan frame, RIGHT-handed:
  +y = the 0-degree beam direction (the vendor's zero mark)
  +z = the spin axis, pointing out of the BASE of the unit (away from the cap)
  +x = y × z

p_lidar = ( −d·sin θ,  d·cos θ,  0 )
```

`D6Driver::on_point()` (live preview) and `D6PushbroomAssembler::resolve_()`
(the cloud) both call it, along with the Cartesian-seam inverse
`fan_angle_deg()`. Before this round the formula was written out longhand in
both, and **neither said which physical end of the sensor its `+z` came out of**
— a convention that is never written down cannot be checked.

#### 1.4 `BracketNominals` did NOT change, and that is the interesting part

The obvious "fix" is to yaw the CAD nominal 180°. It produces the identical
cloud, and doing **both** that and the formula fix is a no-op — a trap worth
naming, because each looks correct in isolation.

Only one of them was actually wrong. Under the corrected frame, the owner's
stated rig — D6 on the back of a portrait phone, 0° beam **UP**, cap **FORWARD**
along the walk — maps:

| lidar axis | physical | camera frame |
| --- | --- | --- |
| `+y` | 0° beam, UP | `+Y` |
| `+z` | the BASE (so the CAP faces forward) | `+Z` (ARCore looks along `−Z`) |
| `+x` | operator's RIGHT | `+X` |

That is the **identity rotation** `BracketNominals.cadNominal(COIN_D6)` has
carried since A8. The CAD nominal was right; the formula was wrong. Its KDoc now
states the frame axis-by-axis instead of restating the old formula, and the
"honest placeholder" caveat is narrowed to the translation, which is still one.

**Fixing the formula rather than the nominal also fixes every archived capture
for free**, with no manifest migration: `old_fan(θ) ≡ diag(−1,+1,−1)·new_fan(θ)`,
so re-resolving a pre-0.6.0 `.lscan` with its own stored extrinsic un-mirrors it.
Fixing the nominal instead would have left every existing recording mirrored
forever. `d6::d6_legacy_fan_extrinsic()` goes the other way and is what the
control arm below uses.

#### 1.5 The proof — `engine/tests/test_round9_chirality.cpp`

A corridor walk with exactly one asymmetric feature: a 2 m doorway cut into the
wall on the operator's **LEFT**. The phone is portrait with its back to the walk
direction, so its ARCore orientation is the identity for the whole run and the
trajectory's rotation is removed from the argument entirely — the fan convention
and the extrinsic are the only things left that can decide handedness. "Left" is
**computed** as `up × forward` from the resolved trajectory, not hard-coded.

```
walk forward = (0, 0, -1), left = up x forward = (-1, 0, 0)
doorway band, wall returns -- FIXED: left 0, right 1440 | LEGACY: left 1440, right 0
worst |fixed - mirror(legacy)| = 0 mm
```

The control is the point: the **same** synthetic returns through the **same**
assembler under the pre-fix convention put the doorway on the right — it fails
the real assertion, exactly. A second case confirms the corridor is otherwise
symmetric to 2%, so the doorway is the only thing the proof is reading. Kotlin
gets the mirror of this in `:core` as `D6ChiralityTest`.

#### 1.6 The owner's scan-017, re-resolved

The container re-resolves cleanly with the fix and **no change to its stored
manifest**: 823 D6 chunks / 71,667 bytes, 150/150 poses accepted, 15,631 world
points — the same returns, placed differently.

| | x extent | y extent (ARCore **up**) | z extent |
| --- | ---: | ---: | ---: |
| 0.5.0 as shipped | 4.18 m | **4.16 m** | 3.08 m |
| 0.6.0 re-resolved | 4.06 m | **3.20 m** | 3.17 m |

Mean per-point displacement between the two is **1.77 m**, worst 6.52 m. They
are *not* a global mirror of each other, because the phone's attitude changes
during the walk, so each return is reflected in a different plane — which is
also why this could never have been fixed by post-processing the exported cloud.

Two things point the same way, though neither is proof and **the owner's own
eyes are the arbiter**:

* ARCore's world is gravity-aligned with **+Y up**, so the y extent is the
  room's floor-to-ceiling height. The owner describes a ~3.1 m room. The fixed
  resolve gives **3.20 m**; the shipped one claimed **4.16 m**.
* The two strongest horizontal 5 cm bands (floor and ceiling) hold **18.4%** of
  all returns after the fix versus **13.0%** before — the same surfaces, ~40%
  more tightly stacked.

---

### 2. ITEM 35 — DENSIFYING THE POSE STREAM WITH THE PHONE'S IMU

> "lidar data and the imu position data need sync the frequency"

#### 2.1 The rate gap, measured on the owner's own capture

`scan-017`'s pose stream: **150 poses over 4.999 s, median interval 33.33 ms,
29.8 Hz**. The D6 samples at 4000 Hz and, after §3, every return carries its own
instant. **One ARCore bracket therefore spans ~133 lidar returns**, and for all
133 the trajectory is whatever a lerp/slerp between two endpoints says.

That is fine for the slow part of walking and wrong for the fast part: heel
strike, hand tremor and the small rotations of a handheld rig live at 5–15 Hz,
which a 30 Hz sampler attenuates badly even where it does not alias. The phone's
gyro runs at 200–400 Hz and sees all of it.

#### 2.2 Orientation, not position — and why

1° of orientation error puts a 3 m return **5 cm** out of place. 1 mm of
position error puts it 1 mm out. Orientation is where short-horizon IMU shines
and where the payoff is two orders of magnitude larger, so position stays on the
lerp exactly as the item asks.

#### 2.3 The method (`engine/include/scanengine/poses/imu_densified_pose.h`)

For a query at `t` bracketed by ARCore poses `a`(ta) and `b`(tb):

1. integrate the bias-corrected gyro from `ta` to `t` in the body frame;
2. integrate on to `tb` and form the closing error `e = q_int(tb)⁻¹ · q_b` —
   everything the gyro got wrong over the bracket;
3. distribute it linearly: `q(t) = q_int(t) · exp(u · log e)`, `u = (t−ta)/(tb−ta)`.

Step 3 is what makes it safe. **At both knots the result is exactly the ARCore
pose**, so the densifier can never drag the trajectory away from VIO, can never
accumulate drift across brackets, and degrades continuously — if the gyro has
nothing to say, `e` absorbs the whole rotation and the output is a slerp again.
All the IMU is permitted to do is choose the *path* between two points VIO has
already fixed.

`ImuDensifiedPoseSource` is a strict wrapper implementing `PoseInterpolator`;
`ExternalPoseSource` and `D6PushbroomAssembler` are untouched, because the
assembler already took the interface rather than the concrete class. `PoseSample`
gained `bracket_t0_ns`/`bracket_t1_ns` (additive) so the densifier integrates
over exactly the interval the interpolator used.

Bias comes from the same closing error (a bias `b` over an interval `T` *is*
`b·T` of closing error, to first order), leaked in slowly, clamped, and weighted
harder when the rig is judged still. Every query falls back to plain
interpolation — counted, by reason — if the IMU has a hole wider than 25 ms, the
bracket is wider than 200 ms, or the closing error exceeds 20° (the guard
against a wrong `camera_from_imu` or a mis-stamped stream).

#### 2.4 The numbers — `engine/tests/test_round9_imu_densify.cpp`

A rig walks past a flat wall carrying 2 Hz gait sway **plus 1.5° of 12 Hz
rotational jitter on all three axes**. 12 Hz is deliberately *below* the 30 Hz
pose Nyquist, so this is an attenuation argument and not an aliasing one: the
poses do sample the jitter, at 2.5 samples per cycle, which is nowhere near
enough for a slerp to follow the arc. Wall flatness is the measure.

| arm | wall plane-fit RMS |
| --- | ---: |
| plain slerp, 30 Hz ARCore (what 0.5.0 ships) | **0.739 cm** |
| **IMU-densified, 400 Hz gyro** | **0.021 cm** |
| analytic truth (1 kHz poses) | 0.0007 cm |

**36x better, and 97.3% of the recoverable error closed.** 3,856 queries
densified, **0 fallbacks**; worst gyro-vs-ARCore closing error over a bracket
0.038°, mean 0.009°.

The controls matter as much as the headline:

* **Remove the jitter and the win collapses** to 0.55x — a 30 Hz slerp is
  already good when there is nothing hiding between the samples. If that ratio
  ever drops, the fixture has acquired motion it should not have and the
  headline is measuring the wrong thing.
* **A 0.01 rad/s gyro bias** (0.57°/s, a realistic consumer MEMS offset) still
  beats plain slerp, and the estimator recovers 0.0146 rad/s of the true
  0.0173 rad/s magnitude with the right sign on all three axes — the
  endpoint-pinning means a bias can distort the path but never accumulate.
* **A 5 Hz "IMU"** (every bracket holed) falls back on 3,856 of 3,856 queries
  and reproduces the plain answer, rather than fabricating a shape.

#### 2.5 Do the two halves of this round fight each other?

Worth asking, because it is not obvious. Densification makes the trajectory
*follow* 12 Hz motion instead of smoothing it away, and a faithful path is in
principle more sensitive to being sampled at the wrong instant than a smoothed
one — a plain slerp is accidentally robust to timing error precisely because it
has already discarded the fast motion. If that effect dominated, item 35 would
be actively dangerous on a device whose lidar stamps are poor.

Measured, with a random per-packet stamp error (constant within a 24-sample
packet, independent between packets — the shape a byte-position reconstruction
really produces):

| per-packet stamp error | plain slerp | IMU-densified | ratio |
| ---: | ---: | ---: | ---: |
| ±0 ms | 0.739 cm | 0.021 cm | 0.03x |
| ±1 ms | 0.743 cm | 0.059 cm | 0.08x |
| ±4 ms | 0.772 cm | 0.234 cm | 0.30x |
| ±8 ms | 0.840 cm | 0.473 cm | 0.56x |

**Densification never becomes counter-productive**, even at stamp errors an
order of magnitude worse than §3's sample clock should produce — so it ships on
by default rather than gated on timestamp quality. It does degrade
monotonically, which is the honest half, and that is also §3's justification in
one table: the better the stamps, the more the gyro is worth.

---

### 3. D6 PER-SAMPLE TIMESTAMPING — THE OWNER'S SPEC NUMBERS

The owner supplied: 10 Hz rotation, **4000 Hz sampling** (400 returns/rev,
250 µs, 0.9° apart), 230400 8N1 = 23,040 B/s capacity — against a **measured
~13.7 KB/s**, i.e. **~60% wire duty**. The D6 buffers a packet, blasts it at the
line rate (**~1.7x faster than it samples**), then idles.

The packet size falls out of those numbers and confirms them: at `lsn = 24` the
stream costs `4000·3 + (4000/24)·10` = **13,667 B/s**. An 82-byte packet takes
3.56 ms to send and covers 6.0 ms of sampling — 59% duty, as measured.

ROUND 7's wire-rate back-dating is right at *packet* granularity and stays. It
is wrong *inside* a packet, compressing 24 samples of 250 µs spacing into
3.56 ms of wire time. `d6::Config::per_sample_timestamps` (default on) spaces
samples at the **sampling** period — derived per packet from its own FSA/LSA
angle span and the reported scan frequency, so a motor at 9.7 Hz is honoured —
anchored on the packet's **first byte**, since transmission begins right after
its last sample. `d6::Point` gained `t_sample_ns`; `t_rx_ns` keeps its exact old
meaning.

**A8/A2 also needed one thing the brief did not ask for.** That anchor is
**biased late**, and the bias is the duty cycle: back-dating at the line rate
assumes the link was busy when it was idle 40% of the time, so the head of a
4 KB read can be mis-dated by ~120 ms — far worse than the 1–3 mm the refinement
was about. The wire anchor is an *upper bound*, never a lower one, and the
device's sampling rate is a far better clock. So `sample_clock_anchor` runs a
**min-delay estimator** over the two: propagate the previous packet forward at
the sampling period, take whichever is **earlier**. It converges with no tuning —
at each read's *tail* `bytes_after ≈ 0` so the anchor is tight, and the chain
carries that value through the *head* of the next read where it is loose. See
A2 §9.3, which also derives why both assembler invariants (non-decreasing
stamps; never later than the transport) fall out of it, and why a saturated
stream compresses rather than slides.

ROUND 7's control needed updating to stay falsifiable: per-sample stamping works
off each packet's byte *offset*, so it recovers structure even with slicing off
and would have quietly disarmed the `time_slice_bytes = 0` arm. That arm now
turns both off.

---

### 4. ITEM 33 — CAPTURE LEAVES NOTHING BEHIND

**The suspected bug was not the bug.** Entering Capture has created nothing
since ROUND 5 (`CaptureViewModel.kt`: "the project is created at Start, not at
screen entry"); leaving without recording was already clean, and the regression
test added for it passes unchanged. The three real sources of strays were:

1. **A Start the engine refuses.** `startCapture()` created the `.lscan` *before*
   asking the engine to record, and on failure left the directory, `project.json`
   and a spent series number behind — the code said so in a comment. Now rolled
   back, and only when *that* call created it (a reopened project is never
   deleted).
2. **Stop with 0 points**, kept deliberately since ROUND 7 ("the project was
   saved so the evidence is not lost"). Now pruned by default, with
   `_sealedProjectId` suppressed so the shell cannot navigate to a project that
   no longer exists. A **failed** seal is still never pruned, so the "its raw
   data IS on the phone" banner can never be contradicted.
3. **Legacy 0-point strays** (the scan-012/-014 clutter). `ProjectManifest.
   isEmptyScan` is now the single definition; the Projects list filters them in
   the **ViewModel**, not in `FileProjectStore.list()`, which merge/processing/
   settings share. Settings gained a Scans section: a `keepEmptyScans` switch
   (default **false**) and a "Clean up N empty scans" action.

Verified on the `b4_test` emulator: **16 connected tests, 0 failures**, including
the new `CaptureFlowNoStrayTest` (enter Capture → leave → no new project; record
2 s → stop → kept). JVM: app unit 62 → and `:core` 405, both green.

---

### 5. ENGINE CHANGES

| file | what |
| --- | --- |
| `drivers/d6/d6_fan.h` | **new** — the fan frame, derived and computed in one place |
| `drivers/d6/d6_parser.h/.cpp` | `Point::t_sample_ns`; per-sample timestamping + the min-delay sample clock; `Stats::sample_hz_est` / `sample_rate_warnings` / `sample_clock_resyncs` |
| `drivers/d6/d6_driver.cpp` | live preview uses `d6::fan_point`; the A8 sink prefers `t_sample_ns` |
| `slam/pushbroom/pushbroom_assembler.cpp` | uses `d6::fan_point` / `fan_angle_deg` |
| `poses/se3.h` | **additive** `quat_mul`, `quat_conj`, `quat_from_rotvec`, `quat_to_rotvec` — the engine's only quaternion multiply had been trapped in an anonymous namespace in `post_pipeline.cpp` |
| `poses/pose_interpolator.h` | **additive** `PoseSample::bracket_t0_ns` / `bracket_t1_ns` |
| `poses/imu_densified_pose.h/.cpp` | **new** — `ImuDensifiedPoseSource` |
| `record/lscan.h/.cpp` | `ChunkType::kPhoneImu = 12`, `StreamId::kImuPhone = 10`, `streams/imu_phone.bin`, a 24-byte codec, and manifest `"imuCalibration"` |
| `core/engine.cpp` | `push_phone_imu()`, `set_imu_extrinsics()`, the densifier wired between the pose source and the pushbroom |
| `record/replay.cpp`, `slam/post/d6_resolve.cpp` | replay and offline re-resolve both feed the densifier |
| `capi/scanengine_c.h/.cpp` | `scan_engine_push_imu`, `scan_engine_set_imu_extrinsics`, `scan_engine_imu_densify_stats`; **ABI 7 → 8** |

Three findings from that wiring worth keeping:

* **A real latent bug: `FileRecordReader::open()` carried a hard-coded
  `kCandidates[]` list of stream files.** `streams/imu_phone.bin` was not in it,
  so `kPhoneImu` chunks were written correctly and then never read back —
  every reopen silently reported zero. Fixed. **Any future stream must update
  that list**, and nothing enforces it.
* **`plane_fit_rms` is not a valid metric through the real D6 driver**, and it
  initially ranked the A/B backwards. The fixture models each sample's range at
  its *packet's* time while the driver assigns per-sample times up to ~5 ms
  earlier (ROUND 7 slicing + ROUND 9 `t_sample_ns`) and applies the datasheet
  angle correction. The proof that it was the metric and not the code: a
  **1 kHz-pose truth arm** resolved from identical bytes measures 1.541 cm —
  *less flat* than the plain-slerp arm's 1.329 cm. Replaced with a
  point-by-point comparison of two resolves of the same bytes, which cancels
  every fixture artefact exactly: **0.036 cm with the gyro vs 2.684 cm without,
  75x**, and the densified arm's flatness (1.539 cm) is now indistinguishable
  from truth's (1.541 cm).
* **The offline IMU pre-pass was considered and deliberately rejected.**
  Loading every `kPhoneImu` chunk before the resolve pass would raise the
  densified fraction, but it would break the stronger property — that an
  offline resolve reproduces the live cloud **bit for bit** — because it would
  densify returns the live pass fell back on. Backward-only is a requirement,
  not an accident, and there is now a case that falsifies it: truncating
  `imu_phone.bin` to 40% yields points that are each *either* exactly the
  full-densified value *or* exactly the plain one, never a third. The
  structural fallbacks were removed instead by aligning the fixture's pose
  period to a whole multiple of the IMU period (`fallback_gap` 323 → 0);
  coprime rates leave ~20% of returns falling one 2.5 ms step short at the
  trailing edge, which is a genuine effect on a real phone and is exactly what
  `fallback_gap` exists to report.

Determinism doctrine held: no Eigen, hand-rolled integration, fixed-seed
fixtures.

---

### 6. ITEM 32 — LEGACY RESCUE, DESIGN SKETCH (no implementation this round)

The goal: recover a trajectory for pre-0.5.0 captures, which hold raw D6 returns
but no `kPoseAr` stream, so A8 has nothing to resolve against and
`D6ResolvePipeline` correctly refuses them with `kNotFound` rather than
resolving through identity.

**The input that exists.** `captures/scan-015-pixel-0.4.0.lscan/` has
`streams/frames/` — **50 camera keyframes** (`kf_000000.jpg` …) plus
`frames.idx`, alongside 437 KB of `lidar.bin`. `ChunkType::kCameraFrameIndex`
already carries a keyframe descriptor with path, pose, intrinsics and timestamp.

**The pipeline.**

1. **Read the keyframe index** for stamps and intrinsics. If the recorded
   descriptors still carry ARCore poses, *stop here* — that is already a
   trajectory at keyframe rate and steps 2–3 are unnecessary. Check this first;
   it may make the whole item cheap for some captures.
2. **SfM/VIO over the keyframes.** ~50 frames over ~26 s is a small, well-posed
   incremental SfM problem: features → pairwise matches → essential matrices →
   incremental reconstruction → bundle adjustment. This is where an external
   dependency finally earns its place (A8 §2's "if a future task needs a *joint*
   solve, that is when Ceres/GTSAM earns it").
3. **Fix the scale.** Monocular SfM is scale-free, and this is the step that
   decides whether the result is a room or a scale model. Three sources, best
   first: (a) the D6's own ranges — a metric sensor observing the same surfaces,
   solved as one global scale factor against the SfM structure; (b) the recorded
   accelerometer, if any; (c) an operator-entered known dimension. Without (a)
   this is not worth shipping.
4. **Interpolate to lidar rate** and re-resolve through the *existing*
   `post::D6ResolvePipeline` by synthesising a `kPoseAr` stream — no new
   assembler, and the ROUND 9 fan fix applies automatically.

**Honest expected quality.** Keyframes at ~2 Hz against ARCore's 30 Hz is a 15x
sparser trajectory, and §2 of this round quantifies exactly what interpolating a
sparse pose stream costs: the 30 Hz→slerp arm already loses 0.74 cm of wall
flatness to 12 Hz motion. At 2 Hz, gait-band motion is not attenuated but
**aliased**, and no amount of post-processing recovers it. Expect **several
centimetres** of wall RMS at best, degrading fast wherever the operator turned
between keyframes, plus outright failure across the low-texture walls and
motion-blurred frames that make handheld indoor SfM hard. **Position this as
"recover a recognisable room from a scan you would otherwise throw away", never
as a survey-grade result** — and say so in the UI, because a mediocre cloud that
looks confident is worse than a refusal.

**Sequencing.** Behind step 1 (which may be free) and after a decision on
Ceres/GTSAM. Not scheduled.

---

### 7. VERIFICATION

```
$ cmake --build <engine-build> -j8 && ./scanengine_tests
[doctest] test cases: 553 | 553 passed | 0 failed | 7 skipped
[doctest] assertions: 2324351 | 2324351 passed | 0 failed
$ ctest                                         # 100% tests passed, 7 of 7
$ ./gradlew :core:test :app:testDebugUnitTest   # core 433/0, app 66/0
$ ./gradlew assembleDebug                       # BUILD SUCCESSFUL
$ ./gradlew :app:connectedDebugAndroidTest      # 16 tests, 0 failures (b4_test)
```

Engine: 535 → **553** cases (+18: 5 chirality/manifest, 6 IMU densification,
7 phone-IMU container), 2,289,147 → **2,324,351** assertions.
Android: `:core` 405 → **433**, `:app` unit 62 → **66**, emulator **16**.

---

### 8. EXPLICITLY NOT VERIFIED

1. **The handedness itself, against the owner's actual room.** The chirality
   test proves the pipeline is now self-consistent with the owner's *stated*
   mount and with the datasheet read as a top view. The datasheet figure's
   viewpoint is the one link in the chain that came from a translated quotation
   rather than from measurement. **The owner looking at the re-resolved
   scan-017 and saying "yes, that door is on the left now" is the acceptance
   test, and nothing here substitutes for it.**
2. **The IMU densification on real hardware.** Every number in §2 is from a
   synthetic bench. No phone gyro has been recorded through this path yet, and
   the 12 Hz / 1.5° jitter model is an assumption about handheld gait, not a
   measurement of the owner's walk.
3. **`camera_from_imu` on a real device.** The Android sensor frame and the
   ARCore camera frame differ by the camera's `SENSOR_ORIENTATION`. A wrong
   value cannot move the bracket endpoints (they are pinned to ARCore) but it
   distorts the path between them — which is the entire value being added. The
   derivation is now recorded in the container (`manifest.json`'s
   `"imuCalibration"`, alongside `"mountCalibration"`) so an offline re-resolve
   recovers it rather than guessing, but the DERIVATION itself has only been
   checked against synthetic orientations, never against a real Pixel.
4. **The 60% wire duty on the owner's rig.** It is the owner's measurement,
   reproduced arithmetically at `lsn = 24`, not one taken from `scan-017`'s
   bytes.
5. **Whether scan-015's recorded keyframe descriptors carry usable poses** —
   §6 step 1, which decides how expensive item 32 actually is. Not opened.

---

## ROUND 10 — THE CLOCKS WERE FINE, AND THE DELAY WAS SOMEWHERE ELSE

Owner verdict on 0.6.0: *"the mirror issue fixed, the scan quality much better
and real now. The thing on the left show on the left."* The handedness chain is
closed. Five items came back; this section is what each one turned out to be.

Field material: `captures/scan-020.lscan` (202.1 s walk, `sections=1`, streams
`lidar.bin` / `poses_ar.bin` / `gnss.bin` / `map.bin` / `imu_phone.bin` at
399.1 Hz, `mountTrim` 92.35°) and `captures/pixel_capture_log_2026-08-18_3.txt`.

### 1. ITEM 36 — "i need to move very slow… when i turn around the scan position shifted"

#### 1.1 The hypothesis was good, and it was wrong

A constant offset between the pose timeline and the lidar timeline is the
textbook explanation for that sentence: it costs `v·dt` along the walk (the
whole cloud slides together, walls stay straight, nobody notices) and `ω·dt` of
yaw (tangential, proportional to range, sign following the turn). It is
invisible walking straight and glaring in a turn. It fits perfectly.

So it was measured rather than assumed, twice, on the owner's own capture.

**Crossing 1 — phone IMU ↔ ARCore pose: −1.5 ms, r = 0.982.** For every one of
the 5,961 ARCore pose intervals, the pose-derived angular speed
(`2·acos|q_i·q_{i+1}| / Δt`) against the same interval's mean |gyro|, integrated
by trapezoid from the recorded 400 Hz stream, swept over lag. The peak is at
−1.5 ms and does not move: first half −1.5, second half −1.5, and the fastest
30 % of intervals (the turns, where the signal is strongest) −1.5 with r rising
to 0.987. ROUND 7 §4 asserted from documentation that `Frame.getTimestamp()` and
`SensorEvent.timestamp` are the same CLOCK_BOOTTIME; this is the hardware
agreeing, and it retires that crossing as a suspect for good.

**Crossing 2 — D6 lidar ↔ ARCore pose: +4 ms, and the curve is flat.** New tool,
`engine/tools/engine_cli.cpp --d6-timesweep`: resolve the same container once per
candidate offset through the production `post::D6ResolvePipeline`, score the
resulting cloud, take the minimum. Swept −150 ms … +150 ms. The 3 cm
occupied-voxel count bottoms out at +2.5…+5 ms (69,803 voxels against 69,879 at
zero) and **the entire ±30 ms window varies by 0.1 %**. The wall-probe thickness
is flat at 4.85 cm across the same range.

+4 ms is 4 mm at 1 m/s and 0.24° at a 60 °/s turn. scan-020's actual wall
thickness is 4.8 cm. **The turn-shift is not a clock offset**, and the default
stays 0 rather than shipping a 4 mm correction the data cannot distinguish from
zero. `kD6PoseTimeOffsetMeasuredNs` records the number so the next round does
not re-derive it.

#### 1.2 What a real offset WOULD look like — `engine/tests/test_round10_time_offset.cpp`

The null result is only useful if the method could have detected a positive one,
so the same fixture proves the mechanism and the blind spot:

| motion | 40 ms of injected lidar-stamp skew | wall RMS |
| --- | --- | ---: |
| straight walk, 1 m/s | none | 9.88e-06 cm → **9.88e-06 cm** |
| turn in place, 60 °/s | same 40 ms | 3.37e-05 cm → **8.81 cm** |

A 260,000× difference from the *same* error, decided entirely by whether the rig
was rotating. **Every geometry fixture in this repository before ROUND 10 walked
in a straight line**, which is why eight rounds of green tests could not have
caught this class of bug — the same structural blind spot ROUND 9 found on the
handedness side (every test measured a sign-blind quantity). The correction
recovers the skew exactly, and a sweep's minimum lands on the injected truth
(`best_ns == -kSkew`), which is what makes "resolve at a sweep and take the
crispest" a measurement rather than a story.

#### 1.3 The delay that WAS there: 2.8 seconds of it

`PushbroomConfig::batch_points = 4096` — a throughput number, written for a
Mid-360 doing hundreds of thousands of points a second, where 4096 points is a
few milliseconds. The owner's D6 resolves **1,453 points/s** (measured: 293,524
in-range returns over 202.1 s), so 4096 points is **2.8 seconds** of geometry
sitting in a `std::vector` before the PageStore, the renderer or the map cache
sees any of it. No refresh rate, no phone and no preset could be faster than
that. It is invisible to every offline test because offline nobody is watching
the screen.

The batch now also closes on **point-time span** (`max_batch_span_ns`, default
100 ms = one D6 revolution). Point time and not wall time is load-bearing: the
assembler's header promises it "never reads a clock, never samples wall time",
which is what makes `pushbroom/assembles_identically_live_and_offline` true. A
wall-clock flush would make page boundaries depend on how busy the phone was.
Measured: **first point visible 2,825 ms → 100 ms**, and **2,560 of 2,560 points
bit-identical** between the two batchings.

#### 1.4 The audit — every clock-domain crossing on this path

| crossing | domain | correction applied | verdict |
| --- | --- | --- | --- |
| ARCore `Frame.getTimestamp()` | CLOCK_BOOTTIME | none needed | correct; measured at −1.5 ms against the gyro |
| `SensorEvent.timestamp` | CLOCK_BOOTTIME | none needed | correct |
| engine `timesync/clock.h` | `clock_gettime(CLOCK_BOOTTIME)` on Android | none | correct — bionic backs `steady_clock` with CLOCK_MONOTONIC, which stops during suspend |
| D6 UART bytes → engine time | none (the D6 has no clock) | ROUND 7 per-byte back-dating at 230400 baud; ROUND 9 per-sample cadence + min-delay anchor; app's 2 ms sensor-latency setting | correct to within the +4 ms measured |
| lidar time → pose lookup | — | **NEW** `PushbroomConfig::pose_time_offset_ns`, default 0, ABI 9 | the knob this round added |
| `TimeSync` min-delay estimator | — | deliberately NOT on the D6/pose path (`stream_has_device_clock` is false for both) | correct — there is no device clock to estimate against |

**`liveSlam=false` on OPTIMAL gates nothing for a D6**, re-confirmed at the
source: `engine.cpp`'s `if (cfg.live_slam)` block constructs a `LioOdometry`,
which is the Mid-360's lidar-inertial odometry. The D6's live map is the
pushbroom, enabled separately, and the owner's log shows
`pushbroomEnabled=true` on every capture. **IMU densification IS live**: the
`ImuDensifiedPoseSource` is built in `Engine::create` and wired between the pose
source and the assembler unconditionally (`engine.cpp` ~line 574), not behind a
session flag.

#### 1.5 Two real bugs found while measuring

**`imuCalibration` was never recorded.** scan-020's manifest says
`"imuCalibration": null` while the session log for that same capture says
`camera_from_imu=derived (… Rz(+90) …)`. Identical in shape to ROUND 8's
`mountCalibration` bug: `start_session()` writes the manifest at `open()`, and
the app applies the IMU extrinsic ~24 ms later (log: `[session] start`
11:06:25.799 → `phone IMU start` 11:06:25.823). Consequence:
`D6ResolvePipeline` falls back to the identity `camera_from_imu`, so **every
offline re-resolve so far integrated the gyro in the wrong frame** — the
endpoints stay pinned to ARCore so it cannot run away, but the path between them
is distorted, and "replay == capture" silently stopped holding for the one
stream ROUND 9 added to shape the geometry. Fixed the way ROUND 8 fixed the
mount: `Engine::set_imu_extrinsics()` also pushes at an already-open recorder,
under `pushbroom_m → imu_m → record_m`.

**The point count shown is ~2× the truth.** `[seal] … points=584315` for
scan-020; re-resolving yields **293,166** world points and the pushbroom's own
accounting is 293,524 in / 293,166 out / **0 dropped**. The live counter is
summing the raw sensor-frame preview stream and the resolved map stream. Not
fixed — reporting, not data. **Backlog.**

**And one that looked like a bug and was not:** `imu_fallbacks` read 196,823 of
455,402 queries (43 %) with only 34,587 accounted for by reason. The missing
162,236 are `sample_at()` calls that returned no pose — the assembler's
*retries* on a point whose bracketing pose has not arrived yet, not
densification failures. `D6ResolveStats` now carries the full `ImuDensifyStats`
so the breakdown is visible: the real fallback rate among resolved returns is
**11.8 %**, essentially all `imu-gap`.

### 2. ITEM 37 — the trail was mirrored, and its test asserted the mirror

`TrajectoryTrail.normalized()` projected `screen_right = +X, screen_up = +Z`.
In ARCore's right-handed +Y-up world the out-of-screen normal of that basis is
`X × Z = −Y` — **a view from beneath the floor looking up**. A bird's-eye view
requires `X × (−Z) = +Y`, so world +Z goes *down* the tile, and since canvas y
already grows downward the correct mapping has **no flip at all**. Shipped:
`y = 1 − nz`. Now: `y = nz`.

Two siblings already had it right and disagreed with this file: `PlanScreen`'s
canvas, and the engine's `plan/occupancy.cpp` (`kY → plan_x = world z,
plan_y = world x` — the same chirality with the tile rotated 90°). The trail was
the outlier, and three comments in its own data path contradicted each other
about whether +Z was "north" or "south-ish".

**Everything else that consumes poses for display was audited and is clean.**
The 3D orbit view uploads world XYZ to Filament verbatim (no negation, no swap,
right-handed `lookAt`, `upVector(0,1,0)`); `FollowCamera`'s `atan2(dz, dx)` is
the exact inverse of its own `headingVector(θ) = (cos θ, 0, sin θ)`, so the
angle round-trips and its rotational sense is unobservable, and its default
heading `−π/2` resolves to `(0,0,−1)` = ARCore forward; the review screen draws
no path; the project thumbnail's "trajectory" is a decorative bezier drawn at
`alpha = 0` on the placeholder only. There was exactly one 2-D projection of
poses in the app.

`TrajectoryTrailTest`'s `screen y is flipped so north is up` **asserted the
bug** and is replaced by two cases: forward (−Z) must go up the tile, and the
owner's acceptance test verbatim — an L-shaped walk turning **LEFT** must render
a trail turning left, with left derived as `up × forward = −X` rather than
hard-coded.

### 3. ITEM 38 — the live cloud belongs to the process, not to the capture

Root cause, in one sentence: **`RealEngineBridge` holds one `scan_engine*` for
the app's lifetime and the engine's `PageStore` is created with the engine, not
with the session, so capture #2 opened on top of capture #1's pages.**

Everything in the ViewModel was already being reset — `_uiState` to `NewScan`,
the typed name, the stats, the section count, all of it, with ROUND 8's comment
listing what was "deliberately NOT touched" and `_pointCloudSource` on that
list. That list was wrong about one entry, and it happened to be the one filling
the screen.

The engine has had the fix since ABI 7: `Engine::start_session()` calls
`recycle_all()` and its comment names this exact failure ("a preview + N record
cycles on ONE connect all stacked into the same 64 pages"). But that reset is
gated on `PageFullPolicy::kEvictOldest`, eviction is opt-in, and **Android never
opted in** — there was no `nativeSetLivePageEviction` and no
`nativeRecycleLivePages` in `ScanEngineNative` at all. Two C-ABI calls that had
existed for three rounds and were never bound. (`LivePageStoreSizing`'s KDoc
still said "eviction is an engine change and the engine tree is read-only for
this task", which was true when it was written.)

Now: both bound; eviction enabled at `createEngineHandle()` (which also means
the live map recycles instead of dead-ending when its page budget fills — the
`fullNote` path becomes unreachable in normal use); the window emptied before
`nativeStartSession`, on entering the Capture tab, and on every seal. The
renderer half matters too — `PointCloudRenderer.setSource()` only clears its GPU
pages when handed `null`, so `clearLiveViewport()` recycles first and *then*
null-and-re-reads the source flow.

**The navigation was a second, independent bug.** `sealedProjectId` was
`MutableSharedFlow(replay = 0, extraBufferCapacity = 4)` with a comment claiming
the buffer covered "a collector that is momentarily absent". It does not:
`extraBufferCapacity` is slack for a **slow** subscriber, and with **zero**
subscribers a `replay = 0` flow discards the value while `tryEmit` still returns
true. The seal runs `NonCancellable` inside `viewModelScope`, so it survives the
composition being disposed — an Activity recreation mid-seal keeps the scan and
loses the navigation, which is exactly "sealed fine, stayed on Capture". Now
`replay = 1`, and both branches log (`navigate -> Projects id=…` or the reason
for staying) so the next field log settles it without inference.

Verification: `CaptureRound10LifecycleTest` (JVM) — a collector that subscribes
*after* the seal completes still receives the id; capture→stop→capture in one
ViewModel seals two scans and re-arms with a fresh auto-name, a cleared typed
name and zeroed stats; the live window is emptied on entry and again after each
seal (counted through an overridable `FakeEngineBridge`).
`CaptureRound10LiveWindowTest` (instrumented, emulator) — the real native
engine: eviction enabled, session #1 fed **real COIN-D6 UART bytes** from the
bundled `assets/replay/synth.lscan` fixture through the production driver seam
(`nativeAddD6Device` + `nativePushSerialBytes`), sealed, recycled, session #2
started on the same handle with the page count asserted **0**, fed, sealed —
both projects listable with points.

**What was deliberately NOT changed:** a capture that recorded zero points is
still pruned and still does *not* navigate (ROUND 9 item 33). The red no-data
banner lives on the Capture tab and navigating away from it would hide the only
explanation the operator gets.

### 4. ITEM 39 — de-clutter, flagged rather than deleted

`core/FeatureFlags.kt` — three `const val`s in `:core` (which has no
`BuildConfig`, and these have to be readable from `:core` defaults as well as
`:app` composables), following the `AR_OVERLAY_ARCHIVED` precedent from ROUND 7.

* **Follow camera** — hidden in *both* places it lived: the Display sheet's View
  row and a second Orbit/Follow pill on the viewport. Two controls for one piece
  of state; finding only one would have left the feature reachable. Default
  camera mode is now ORBIT for capture as well as replay. `FollowCamera`, its
  config and its tests are untouched.
* **RGB** — removed from the capture sheet, the review chip strip and the review
  Display panel (which iterated `ColorMode.entries` and so surfaced RGB *and*
  FIX_QUALITY regardless of what the rest of the app did), and it now carries a
  reason string in `colorModeAvailability` like every other unavailable mode.
  `ColorMode.RGB` itself stays: its ordinal crosses the C ABI into the shader
  (`colorMode.ordinal`) and it is the fallback every unsupported mode degrades
  to.
* **Colorize** — the keyframe switch, the rate row, the viewport KF chip, the
  Processing tab's Colorize action and Settings' whole "Processing" section.
  Gated at the **state** as well as the UI (`_keyframesEnabled`, `applyTuning`,
  `setKeyframesEnabled`), because `CaptureTuning` writes that flow on every
  preset change and would otherwise have turned the recorder back on behind a
  hidden switch. ARCore is untouched — it is the pose engine and the D6's third
  dimension.

**Defaults checked one by one, and one was wrong in three places at once.**

| setting | before | after |
| --- | --- | --- |
| view mode | FOLLOW (capture) | **3D orbit** |
| colour mode | INTENSITY | unchanged |
| colormap | **SPECTRUM or GRAYSCALE depending on the path** | **GRAYSCALE**, from one constant |
| point size | 1.0 px, FIXED_PIXELS | unchanged |
| gamma / brightness | 1.0 / 1.0 | unchanged |
| live refresh | 30 fps on STANDARD/FLAGSHIP | unchanged |

The colormap was the interesting one: the QUICK_SCAN profile default said
GRAYSCALE, `DisplayParams.captureDefaults()` set no colormap at all and
therefore inherited `ScalarColorParams`' own SPECTRUM, and `CaptureViewModel`
initialised its own flow to SPECTRUM independently. Three sources, two answers,
and which one won depended on which screen the value came through. There is now
one constant, `DisplayParams.CAPTURE_COLORMAP`, and all three read it.

Live refresh is left tier-dependent on purpose: 30 fps is what the owner's Pixel
gets, and a MODEST-tier phone starting at 15 fps is ROUND 6 item 21's
conservative-defaults rule, not an oversight.

### 5. ITEM 40 — the capture log's filename

`CaptureLog.exportFileName(epochMillis)` →
`lidarscan-capture-log-YYYY-MM-DD-HHMM.txt`, **device local time, to the
minute** — deliberately the same format and the same clock `ScanAutoName` uses,
so `Scan-020-2026-08-18-1106` and `lidarscan-capture-log-2026-08-18-1111.txt`
can be paired by eye. The old `EXPORT_NAME` was a bare constant, which is why
MediaStore had been de-duplicating exports into `lidarscan-capture-log (1).txt`
— an artifact this repository quotes in its own source (`MountTrim.kt` cites one
from the owner's 0.4.0 session). Seconds are omitted for the same reason the
scan names omit them; two exports inside one minute are the one case where
MediaStore's `(1)` suffix is the right answer.

### 6. FILES

Engine:
* `include/scanengine/slam/pushbroom/pushbroom_assembler.h` — `pose_time_offset_ns`,
  `max_batch_span_ns`, `kD6PoseTimeOffsetMeasuredNs`
* `src/slam/pushbroom/pushbroom_assembler.cpp` — the offset at the pose lookup,
  the point-time batch bound
* `include/scanengine/core/engine.h`, `src/core/engine.cpp` — ABI 9,
  `set_pose_time_offset_ns`, and the `imuCalibration`-at-an-open-recorder fix
* `capi/scanengine_c.h`, `capi/scanengine_c.cpp` — ABI 9 (two new symbols,
  additive)
* `include/scanengine/slam/post/d6_resolve.h`, `src/slam/post/d6_resolve.cpp` —
  full `ImuDensifyStats` in `D6ResolveStats`
* `tools/engine_cli.cpp` — `--d6-timesweep`
* `tests/test_round10_time_offset.cpp` — 5 cases
* `src/drivers/d6/d6_driver.cpp` — a leftover unused `kDegToRad` from ROUND 9's
  fan extraction (it broke the `-Werror` build)
* `docs/A2-d6-driver.md` §10

Android:
* `core/FeatureFlags.kt` (new)
* `core/render/DisplayParams.kt` — `CAPTURE_COLORMAP`, RGB availability reason
* `core/capture/TrajectoryTrail.kt` — the projection
* `core/engine/EngineBridge.kt` — `resetLiveView()`
* `core/engine/FakeEngineBridge.kt` — `open`, so a test can count resets
* `app/engine/ScanEngineNative.kt`, `app/src/main/cpp/scanengine_jni.cpp` —
  `nativeSetLivePageEviction`, `nativeRecycleLivePages`,
  `nativeSetPoseTimeOffsetNs`
* `app/engine/RealEngineBridge.kt` — eviction on, recycle at start,
  `resetLiveView`
* `app/ui/capture/CaptureViewModel.kt` — `clearLiveViewport`, ORBIT default,
  GRAYSCALE default, keyframe gating, `replay = 1`, navigation logging
* `app/ui/capture/CaptureSheets.kt`, `CaptureScreen.kt`,
  `app/ui/review/ReviewScreen.kt`, `app/ui/processing/ProcessingScreen.kt`,
  `app/ui/settings/SettingsScreen.kt` — the flagged UI
* `app/debug/CaptureLog.kt` — `exportFileName`
* tests: `CaptureRound10LifecycleTest` (JVM), `CaptureRound10LiveWindowTest`
  (instrumented), `TrajectoryTrailTest` (rewritten chirality cases)

### 6b. VERIFICATION

* **Engine**: `ctest` **7/7 green**; `scanengine_tests` **558 cases / 2,335,227
  assertions, 0 failures** (553 cases before this round — the five new ones are
  `test_round10_time_offset.cpp`). Three assertions were updated for the ABI
  8 → 9 bump and nothing else moved, which is the point of an additive ABI.
* **Android JVM**: `:core:test` **434 tests**, `:app:testDebugUnitTest`
  **70 tests**, 0 failures, run three times consecutively with `--rerun-tasks`.
* **Android instrumented**: `:app:connectedDebugAndroidTest` on the `b4_test`
  AVD — **17 tests, 0 failures** (16 before this round; the new one is
  `CaptureRound10LiveWindowTest`).
* **APK**: `versionCode 700`, `versionName 0.7.0`, derived from the repo-root
  `VERSION` file as the owner's rule requires.

Two test-harness notes, because both cost time and neither is a product bug:

* **`mid360_sim_e2e` is load-sensitive and its failures are not regressions.**
  It asserts real throughput (`pps > 0.97 × nominal`, `imu_hz > 0.97 ×
  nominal`) over a loopback UDP link, so running it beside a booted emulator
  and a parallel Gradle build makes it fail on timing. It also inherits the
  round-4 item-6 problem: **killing `ctest` leaves the `scanengine_tests` child
  alive holding the UDP ports**, and the next run then fails instantly with
  `bind failed` rather than with a timing miss. Green 7/7 on a quiet machine;
  the two failure modes are distinguishable by runtime (3 s = port conflict,
  ~205 s = genuine timing).
* **One pre-existing race in `CaptureRound7FieldBugsTest` was de-raced.** It
  polled the log for `[seal] sealed` and then sampled `noDataAlert` in the same
  breath — but the banner is set *after* the prune decision, which is a
  `withContext(Dispatchers.IO)` hop. It now waits for the banner. The claim is
  unchanged (the alert survives the stop); only the sampling stopped being a
  coin flip.

### 7. EXPLICITLY NOT VERIFIED / BACKLOG

* **The turn-around shift itself is not closed.** The clock hypothesis is
  falsified with numbers, and the live-view latency (2.8 s → 100 ms) is a real
  and large fix that plausibly accounts for "seems slow and delay" and for
  needing to walk slowly. Whether it accounts for the *positional* shift on a
  turn is unproven. The remaining ranked suspects, from the data:
  1. **Mount trim accuracy.** scan-020's trim was accepted at `spread=2.65°,
     spreadP90=2.40°` — the gate's own ceiling. A 2.4° extrinsic error is
     ~12 cm at 3 m, it is applied in the *phone* frame, and it therefore
     **reverses in world terms when the operator turns around** — the same
     wall painted twice, which is the reported symptom, and it is
     rate-independent in a way a clock offset is not. Tightening the gate (or
     averaging several re-zeros) is the cheapest next experiment.
  2. **ARCore VIO lag under motion**, which is not a constant offset and would
     not have shown up in the sweep.
  3. **The 11.8 % `imu-gap` densification fallback rate**, if it clusters on
     fast rotation.
* The +4 ms lidar↔pose offset is measured on ONE capture on ONE phone. It is
  recorded as a constant, not applied as a default.
* The `imuCalibration` fix is verified by construction and by the engine suite,
  **not** on hardware — no capture written by this build exists yet. The first
  0.7.0 capture's manifest should be checked for a non-null `imuCalibration`.
* The double-counted point total (584,315 shown vs 293,166 real) is **not
  fixed**.
* `--d6-timesweep`'s wall-probe metric found only 7 usable probes on scan-020
  and was flat across the whole sweep; the occupancy/entropy metrics carried the
  measurement. A capture with more straight wall would sharpen it.
* Nothing was run on kc-m4; macOS/desktop untouched.

## ROUND 11 — THE ACCURACY WAVE, AND THE ONE NUMBER THAT EXPLAINS THE TURN

Ships with ROUND 10 as **0.7.0** (`versionCode 700`). Items 41–45 plus the
double-counted point total ROUND 10 left on the backlog.

The headline is item 45c: **ROUND 10's ranked suspect #1 was right, and it is
now measured rather than estimated.** A 2.4° mount trim error — which is what
scan-020 was captured with, at the gate's own 2.5° ceiling — paints the same
overhead feature **13.1 cm apart** between the outbound and return legs of an
out-and-back walk, and the split **reverses with the walk direction**. That is
"when i turn around the scan position shifted", to the centimetre.

### 1. ITEM 45c — WHAT 2.4° COSTS, AND WHICH PART OF IT REVERSES

`engine/tests/test_round11_mount_trim.cpp`. An out-and-back past a ceiling beam
1.66 m above the sensor, resolved through the production
`D6PushbroomAssembler`, with the TRUE lidar orientation offset from the one the
assembler is told. Measured, not derived:

| trim error | beam painted outbound | on the return | **split** | 2·r·sin(d) predicts |
| ---: | ---: | ---: | ---: | ---: |
| 0.0° | −3.0156 m | −3.0056 m | **1.0 cm** | 0 (this is the fixture's own floor) |
| 0.5° | −3.0156 m | −3.0056 m | **1.0 cm** | 2.9 cm |
| 0.8° | −3.0156 m | −2.9789 m | **3.7 cm** | 4.6 cm |
| 1.4° | −3.0326 m | −2.9426 m | **9.0 cm** | 8.1 cm |
| **2.4°** | −3.0763 m | −2.9454 m | **13.1 cm** | 13.9 cm |

Scale by range: the split is `2·r·sin(d)`, so a return at 3 m costs 3/1.66 =
**1.8×** these numbers — **23.6 cm at 2.4°** against 6.6 cm at 0.8°. The
fixture resolves to about 1 cm (a D6 puts 400 returns into one 10 Hz
revolution, so the beam's 30 cm underside is sampled by ~11 fan angles), which
is why 0.5° reads as the floor rather than as 2.9 cm.

**Two things this measurement corrected in ROUND 10's write-up, and both
matter.**

**(a) It is a doubled FEATURE, not a thickened wall.** The displacement is
perpendicular to the return's own ray, so on a wall the D6 looks at square-on
the points slide ALONG the wall and the wall does not thicken by a millimetre.
Every geometry metric this repository had before ROUND 11 — plane-fit RMS, band
thickness, `--d6-timesweep`'s wall probes — is a wall-flatness metric, and not
one of them can see this. The owner's own words are the better description:
"the scan position shifted", not "the walls got thick".

**(b) Only PART of a trim error reverses, and it is not the part you would
guess.** Measured three ways in the same fixture:

| trim error about | effect | reverses on turn-around? |
| --- | --- | --- |
| camera **+X** (the phone's right) | tilts the fan plane fore/aft; displaces overhead and underfoot returns ALONG the walk | **yes, in full** — the axis points world +X out and world −X back |
| camera **+Y** (up) | yaws the fan; displaces sideways returns along the walk | **no** — for a vertically-held phone this axis IS world up either way. Measured: 6.5 cm of displacement at 2.4°, and a 1.0 cm split (i.e. none) |
| camera **−Z** (forward) | a rotation inside the fan's own plane | **no effect at all** on a 360° fan |

So a mount error has a component that doubles features and a component that
silently displaces the whole room. The gate measures the magnitude and cannot
tell them apart, which is the argument for driving the magnitude down rather
than for characterising the axis.

**(c) A one-way walk hides it completely.** Same 2.4°, outbound only: the beam
is painted in exactly one place, 6.1 cm from the truth, with its depth spread
unchanged (25.8 cm clean → 25.755 cm at 2.4°). One pass sees one beam, in the
wrong place, and has no way to know it. That is why eight rounds of fixtures
missed this — the same structural blind spot ROUND 9 found on handedness (every
test measured a sign-blind quantity) and ROUND 10 found on the clock (every
fixture walked in a straight line).

### 2. ITEM 45a/b — THE GUIDED RE-ZERO, AND WHY "SPREAD FALLING TO 0.8°" WOULD
### HAVE BEEN A LIE

`core/calib/MountTrimRefiner.kt`. Same ROUND 8 gate underneath — same 1 s
window, same p90 ≤ 2.5° and outlier ≤ 6°, `MountTrimSampler` untouched — with
the gate asked **ten times a second** instead of once per tap.

* **The ring.** Tap Re-zero, hold the rig still, watch the mount chip become a
  progress bar. It fills while the gate passes and **empties the instant it
  does not** (the hold anchor jumps to now), so an operator learns what "still"
  means with their hands instead of by reading a verdict on a moment that has
  already finished. The owner's 0.4.0 log has seven MOVING refusals in
  forty-four seconds; ROUND 8's answer was to make each refusal explain itself,
  which helped and was still the wrong shape.
* **The refinement, and the thing item 45c's wording nearly got wrong.**
  `spreadP90Deg` measures the JITTER of individual ARCore frames about their
  mean. Holding for four seconds instead of one does **not** reduce it — it is a
  property of the tracker and of the hand. A UI promising "improving… 1.4°"
  against the spread would be promising something that does not happen, and the
  test asserts it does not (`assertEquals(short.spreadP90Deg, long.spreadP90Deg,
  0.5)`).
  What DOES improve is the accuracy of the MEAN, which is what gets stored. And
  rather than model it — a standard-error argument needs independent samples,
  and ARCore's visibly wander together over a second or two — the refiner
  **measures** it: split the hold in half, average each half separately, report
  the angle between the two answers. That is an empirical answer to "if I took
  this trim twice, how far apart would they be", it needs no noise model, it
  catches correlated wander (asserted: a 2° wander over a 14 s period reads
  worse than none), and it is conservative by about √2. It is what the ring
  counts down and what "Improving… 1.4°" reports.
* **The trim is averaged over the whole hold**, not the gate's last second: 6 s
  at 30 Hz is ~180 samples against ~31, asserted, with the two answers agreeing
  to well inside the jitter.
* **Auto-refresh at Start (45b).** `maybeAutoRefreshMountTrim()` runs at the top
  of `startCapture()`. If the trim is older than 10 minutes or came from a
  previous app run, AND the ROUND 8 gate passes right now, it is silently
  re-taken and logged. If the gate refuses, the OLD trim stays and an amber note
  says how old it is — refusing to Start would be worse (the operator is holding
  a rig, ready to walk), but a stale trim entering a scan with no mention
  anywhere except a log line read afterwards is what this closes. Deliberately
  NOT the guided hold: Start must be instant, and this reads the pose window
  that is already streaming.
* `CaptureArController.motion` is now a **320-sample** ring rather than 64.
  64 is 2 s at ARCore's measured 30 Hz, so an 8 s refinement would have silently
  stopped improving after two seconds while the ring showed it still filling.
  `RigMotionTracker.estimateAt` filters by its own 100 ms window and is
  unaffected.

### 3. ITEM 41 — LOOP CLOSURE, AND FIVE GATES THAT ALL HAD TO EXIST

`engine/include/scanengine/slam/post/trajectory_loop.h` +
`src/slam/post/trajectory_loop.cpp`, wired into `D6ResolvePipeline` behind
`close_loops` (default **off** — see below) and driven by a new
`engine_cli --d6-loopclose`.

**Why it is not `post/pose_graph.h`.** A7 already owns an SE(3) pose-graph
optimizer and a Scan Context detector, and both are the wrong tool: they exist
because a Mid-360's trajectory is ESTIMATED and every keyframe pose is a free
variable. A D6's trajectory was MEASURED by ARCore, whose error is not the
pose-graph error model — VIO drift is a slow, smooth walk of the world frame
(excellent over one second, which is what makes ROUND 9's gyro densification
work; large over 200). So the correct correction is not "re-balance 6,000
poses", it is "the world frame has rotated and slid by THIS much, spread it back
along the path".

**The correction, and why no re-resolve is needed.** A D6 world point is exactly
`T_world_phone(t) · T_phone_lidar · p_lidar`, so a world-frame correction
`C(t)` applied to the pose is the same as `C(t)` applied to the point. The
assembler therefore grew one optional output — `PushbroomConfig::out_point_times`,
the pose-time of every point that actually reached the PageStore, in emit order
— and the closure is applied to the resolved cloud in place. `C(s) =
Exp(s·Log(T_fix))` with `s` the **arc-length** fraction between the two visits
(drift accumulates with distance, not with seconds — a 30 s pause mid-walk must
not be handed a third of the correction), clamped to 0 before the first visit
and 1 after the second. `Exp(0)` is identity to the last bit and `Exp(1·Log(T))`
is `T` to the last bit, both asserted.

**The five gates, in the order they refuse.** Every one of them was added
because something got past the ones before it:

1. **Spatial revisit** — two poses ≥15 s and ≥8 m of path apart, ≤3 m of gap. A
   one-way walk produces **no candidate at all**: the guard against the
   catastrophic case is structural, not a threshold.
2. **Excursion** — the path between them must reach ≥4 m from the first. Without
   it a rig shuffling in one corner is a perfect loop candidate at every pair of
   poses it recorded (the test walks 20 m of path inside a 1 m box and this is
   what stops it).
3. **3a. Observability** — *the gate this round exists because of, and it is a
   property of the sensor.* A D6 sweeps a PLANE perpendicular to the walk, so
   over a short window its returns land on the walls ahead and behind, the floor
   and the ceiling, and on **no surface whose normal points along the walk**.
   Point-to-plane ICP measures distance along normals, so translation along the
   walk costs it exactly zero residual — the problem has a null space, and an
   ICP with a null space does not fail, it **wanders**. Measured on this round's
   own fixture: 4°/0.30 m of injected drift came back as 3.77° (right) and
   **2.74 m** of translation, all of it along the walk. The target submap's
   normals are now collected by local plane fit, `Σ n nᵀ` eigen-decomposed, and
   a coverage below λ_min/λ_max = 0.05 refuses with the reason named. Widening
   the submap window to ±6 s is the cure where a cure exists (walking a curve
   turns the fan and fills the gap in); on a dead-straight out-and-back there is
   none, and the honest answer is a refusal.
4. **Geometric agreement** — A7's `icp_point_to_plane` and A7's
   `loop_is_acceptable`, unmodified, at ≥45 % inliers and ≤0.20 m RMS. A7's 30
   iterations and 1e-5 rad / 1e-4 m stopping thresholds were loosened to 60 and
   1e-4 rad / 1e-3 m — 0.006° and 1 mm, two orders below anything that changes
   the answer — because two sparse anisotropic D6 submaps floor out around 1e-4
   and A7's rule reported "did not converge" on perfectly good closures. **A
   stopping rule, not an acceptance rule**; every acceptance gate is untouched.
5. **Magnitude** — ≤0.60 m and ≤6.0°, and these numbers are an empirical finding
   rather than a guess. Run against the owner's scan-020 with a generous
   1.5 m / 20° bound, ICP produced a **0.97 m / 17.0°** "closure" with 77.8 %
   inliers whose same-place mismatch genuinely improved from 77 cm to 12 cm —
   and which raised the whole map's occupied-voxel count by **8.6 %**. Locally
   right, globally a fold. ARCore's real indoor drift over 200 s is tens of
   centimetres and a couple of degrees; 17° is not drift.
6. **Whole-map crispness** — the correction is applied to a copy and the cloud
   is asked, with ROUND 10's own metric (occupied 3 cm voxels). **And it is only
   asked where the question has an answer**: occupancy carries information about
   a correction only where the walk painted the same place twice. Measured both
   ways this round — on the synthetic single-lap fixture a closure that cut the
   worst per-point error from 61 cm to 16 cm still RAISED the voxel count 1.8 %
   (a re-warp of a singly-painted cloud moves the count by a percent or two from
   resampling alone), while scan-020's false 17° closure raised it 8.6 %. So the
   **overlap fraction** — the share of occupied voxels holding returns more than
   `min_loop_seconds` apart — is computed first, and the crispness rule is
   enforced only above 5 %. Below that the gate abstains and says so, rather
   than voting on no evidence. Without this it would veto every good closure
   there is.

**Synthetic ground truth** (`test_round11_loop_closure.cpp`): one lap of a
2.5 m-radius circle in an 8×8×3 m room with three posts, resolved twice from the
same ranges — once against the true poses, once against poses corrupted by 4° of
yaw growing as `s^1.5` plus 0.30 m of translation with a quadratic term
(deliberately **not** the SE(3) geodesic the correction is built from, so
"it closes" is a claim about recovery and not about a rigged fixture).

| | mean per-point error vs truth | worst |
| --- | ---: | ---: |
| drifted | 17.17 cm | 61.12 cm |
| **closed** | **8.94 cm** | **16.01 cm** |

— **74 % of the worst-case error removed**, and ICP measured 3.99° against the
4.0° injected. The residual is neither zero nor noise: the injected drift grows
as `s^1.5` while the correction is linear in `s`, so they agree exactly at both
ends and differ by up to 0.6° in the middle, ~3 cm at 3 m. That is the honest
limit of a one-loop correction, and it is the reason a multi-loop capture would
want a real graph.

**On the owner's scan-020, it refuses — and the refusal is the finding.**

```
trajectory : 5966 poses over 202.1 s, 10.8 m walked,
             extent 0.24 x 0.11 x 4.57 m, furthest from start 3.67 m, start->end 0.52 m
DECISION   : no-excursion  (27,216 spatial candidates)
```

Two things fall out of that line that nobody had looked at before:

* **The walk is 10.8 m in 202 seconds — 5.3 cm/s.** The owner said "i need to
  move very slow to capture the stable quality" and that is what very slow
  turned out to mean: about a twentieth of a walking pace.
* **It is an out-and-back along a straight line**, 3.67 m out and back, with
  24 cm of lateral extent over the whole capture. Not a loop, and 3.67 m is
  under the 4 m excursion floor. Forced past it (`--min-excursion 3.0`) it
  reaches ICP, which proposes a **176° flip** — the corridor-mistaken-for-itself
  failure, textbook — and gate 4 refuses it. **Nothing was moved; the cloud is
  byte-for-byte what the app produces.**

`close_loops` therefore defaults **off** in `D6ResolveConfig`, and that is a
decision rather than caution: this pipeline's contract is "replay == capture"
(Tech Spec §3 key rule 2), and loop closure deliberately produces DIFFERENT
points. It may never happen behind a caller's back.

### 4. ITEM 42 — COVERAGE COLOURING

`core/render/CoverageGrid.kt` + `PointCloudRenderer`. A fixed-lattice voxel
count (25 cm, anchored at the world origin so the answer never depends on
arrival order) turned into a per-point tint the renderer writes into **its own
GPU copy** of the vertices.

`ColorMode.COVERAGE` is the one value in that enum whose ordinal must never
cross the C ABI. Every other mode is a shader branch — the engine's
`scanengine::cloud::ColorMode` has the same values in the same order and
`points.mat` switches on the integer — but coverage is not a shader branch at
all, so `PointCloudRenderer.shaderColorMode()` asks for plain RGB pass-through
(mode 0) and the shader never learns coverage exists. That is what keeps item
42's promise that the tint is never written into the container: it lives in a
Filament VertexBuffer for as long as the live view does, and the engine's
PageStore — which is also the map cache that gets sealed — is read and never
touched.

* **The ramp.** A fully covered point keeps its own shade **exactly** (asserted
  byte-for-byte), so a well-scanned room in coverage mode is indistinguishable
  from the grayscale-intensity view the owner made the default in ROUND 10 item
  39 — which is what "works with the intensity/grayscale default" has to mean,
  and the only way this becomes a mode anyone leaves on. As coverage falls the
  point is pulled toward amber (not red: red is already the app's failure colour
  and "thin here" is not a failure) and brightened, because a thin region is by
  definition made of few points and a dim tint on few points is invisible.
* **25 cm is from the sensor, not from taste**: a D6 at 10 Hz × 400 returns puts
  ~40 returns into a 25 cm patch of wall in one pass at walking speed, so "one
  pass" and "several passes" land on opposite sides of the ramp.
* **Honest at the point budget.** The counts come from the points the renderer
  holds, which is the same set it draws — when the LOD budget stops admitting
  pages the tint and the drawing decimate together, so a cell that reads thin IS
  thin in the map on screen. It is not a claim about points the renderer never
  saw, and the KDoc says so.
* Already-uploaded pages are re-tinted round-robin, one page per 250 ms, without
  touching `gpu.uploaded` or the renderable's geometry count (shrinking it to
  force a re-upload would make the cloud blink). The grid is cleared with the
  source, for the same reason ROUND 10 item 38 had to clear the page store.
* Not offered in Review: coverage answers "where have I not been yet", which is
  a question about a walk in progress, and the container carries no density.

### 5. ITEM 43 — HAPTIC + AUDIO CUES

`core/capture/OperatorCues.kt` (all the deciding) + `app/capture/OperatorCuePlayer.kt`
(all the buzzing). Default **ON**, one switch in Settings.

| cue | pattern | tone | repeat |
| --- | --- | --- | --- |
| tracking degraded | 2 firm buzzes | mid | every 4 s while it lasts |
| section break | 3 short urgent buzzes | high | on the EVENT, never on the level, 1 s floor |
| moving too fast | 1 long soft buzz | low | every 3 s while it lasts |

They differ in **count** rather than in length or timbre, because through a
pocket at walking pace that is the only dimension that survives and it is the
one a person learns in a session.

Six behaviours that are properties of the timing and are therefore unit-tested
rather than eyeballed: the first tick is always silent (ARCore is degraded at
Start; buzzing before the operator has taken a step is how a default-ON feature
gets switched off); a section break fires on the delta and never on the level;
one cue at a time, highest priority wins, and **the loser does not consume its
debounce** so it fires on the next tick; disabled cues still advance the state,
so switching them on mid-capture does not replay a backlog; `reset()` per
session, or capture #2 opens with capture #1's section baseline; and no cue
fires unless a recording is actually running.

The decision is made in `updateMotionHint` — the same evaluation point as the
on-screen hint, deliberately, so the hint and the cue can never disagree about
whether the rig is moving too fast. `VIBRATE` is a normal permission (no runtime
prompt mid-capture). Everything is posted to a single-threaded executor: the cue
is decided on the main dispatcher, and `Vibrator.vibrate` and
`ToneGenerator.startTone` both cross a binder.

### 6. ITEM 44 — THE SCAN SUMMARY CARD, AND WHAT IT COMPOSES WITH

`core/capture/ScanSummary.kt` + `ScanGradeBanner` in `CaptureScreen`. Points,
duration, metres walked, `.lscan` size, sections, tracking drops, points per
metre, average rate — and one word.

**The grade's thresholds are consequences of measurements, not taste:**

* **Sections** — a break is ARCore relocalizing, so everything after it is in a
  different world frame (ROUND 7 §5). 1 is clean, ≤3 usable, >3 is a scan whose
  pieces no longer agree and no post-process fixes it.
* **Tracking drops** — points taken during tracking loss are *excluded* by the
  assembler (`exclude_flagged`), so every drop is a hole in the room.
* **Points per metre** — ROUND 10 measured the owner's rig at **1,453 resolved
  points/s**, so points-per-metre is walking speed in disguise and does not
  depend on the room's size. 800/m ≈ 1.8 m/s (GOOD floor), 400/m ≈ 3.6 m/s
  (FAIR floor).
* Zero points is POOR whatever else is true.

The reason string names the WORST thing in the same order the grade decided, so
the word and the sentence can never disagree. A tripod scan is not punished for
having no path (the density denominator is floored at 0.5 m).

**Composing with ROUND 10's seal→navigate flow, rather than fighting it.** The
two features collide if left alone: ROUND 10 made the seal jump to Projects, and
a card shown at that instant lives for one frame before `goTab` disposes the
back-stack entry. So `sealedProjectId` is unchanged — still emitted at exactly
the same point, still `replay = 1`, so an Activity recreation mid-seal still
recovers the navigation — and `CaptureRoute` **holds** the id until the card is
dismissed. A seal that produced no summary navigates immediately, exactly as
ROUND 10 shipped it.

### 7. THE DOUBLED POINT COUNT — ROUND 10's BACKLOG ITEM, CLOSED

`core/capture/PointCountTally.kt`. The owner's log said `points=584315` for
scan-020; re-resolving the same container yields **293,166**, with the
pushbroom's own accounting at 293,524 in / 293,166 out / 0 dropped.

`SCAN_EVENT_POINTS_AVAILABLE` fires once per page-append **per stream** and has
carried the stream id in `payload.points.stream` since B2. `RealEngineBridge`
summed the count field and ignored the id, so during a D6 capture every return
was counted once as a raw sensor-frame preview point and again as a resolved map
point. `StreamFilter` has drawn only one of them since B3 and `writeProjectPreview`
has preferred the map since ROUND 8 — the counter was the last consumer that
still believed there was one stream.

Now the tally reports the **resolved map** (raw fan points that never found a
pose are not in the room), falls back to raw before the first mapped point
arrives (~100 ms, since ROUND 10 bounded the batch in point time), and the log
line carries both halves — `points=293166 (map=293166 raw=293524 other=0)` —
so the next field report can be read without anyone re-deriving which number was
which. The roles rather than the stream ids cross into `:core`, per
`EngineBridge`'s own rule that the `SCAN_STREAM_*` numeric space belongs to the
C ABI.

This number also reaches `pointCountEstimate` in the sealed manifest, the
capture log, the HUD and the summary card, all of which were ~2× as well.

### 8. FILES

Engine:
* `include/scanengine/slam/post/trajectory_loop.h`, `src/slam/post/trajectory_loop.cpp` (new)
  — detection, the five gates, `se3_exp`/`se3_log`, `TrajectoryCorrection`
* `include/scanengine/slam/pushbroom/pushbroom_assembler.h`,
  `src/slam/pushbroom/pushbroom_assembler.cpp` — `out_point_times`
* `include/scanengine/slam/post/d6_resolve.h`, `src/slam/post/d6_resolve.cpp` —
  `close_loops`, `TrajectoryLoopConfig`, `out_trajectory`, `out_point_times`,
  `D6ResolveStats::loop` / `loop_applied`, the in-place correction
* `tools/engine_cli.cpp` — `--d6-loopclose`
* `tests/test_round11_loop_closure.cpp` (7 cases), `tests/test_round11_mount_trim.cpp` (4 cases)

Android `:core`:
* `capture/PointCountTally.kt`, `capture/ScanSummary.kt`, `capture/OperatorCues.kt` (new)
* `calib/MountTrimRefiner.kt` (new)
* `render/CoverageGrid.kt` (new)
* `render/Colormap.kt` — `ColorMode.COVERAGE`
* `render/DisplayParams.kt` — `activeScalar`, `evaluatePointColor`, `colorModeAvailability`

Android `:app`:
* `capture/OperatorCuePlayer.kt` (new)
* `capture/TrajectoryTrailRecorder.kt` — `totalPathM`
* `engine/RealEngineBridge.kt` — the tally and `streamRole()`
* `ar/CaptureArController.kt` — 320-sample pose ring
* `ui/capture/CaptureViewModel.kt` — `scanSummary`, the cue tick, `beginMountHold` /
  `cancelMountHold` / `applyMountTrimResult`, `maybeAutoRefreshMountTrim`
* `ui/capture/CaptureScreen.kt` — the grade banner, the summary card's new rows,
  the held navigation, the mount ring
* `ui/capture/CaptureSheets.kt`, `ui/review/ReviewScreen.kt` — the Coverage chip
* `ui/settings/SettingsScreen.kt`, `SettingsViewModel.kt`, `data/SettingsModels.kt`,
  `data/SettingsRepository.kt` — the cues switch
* `render/PointCloudRenderer.kt` — the coverage grid, the tint, the refresh
* `di/AppContainer.kt`, `AndroidManifest.xml` — the player and `VIBRATE`
* tests: `core/capture/Round11CuesAndSummaryTest.kt` (14),
  `core/calib/MountTrimRefinerTest.kt` (8), `core/render/CoverageGridTest.kt` (7),
  `app/ui/capture/CaptureRound11SummaryTest.kt` (4)

### 9. VERIFICATION

* **Engine**: `ctest` **7/7 green** (run serially — `mid360_sim_e2e` asserts real
  throughput over loopback UDP and fails on timing beside a booted emulator).
  `scanengine_tests` **570 cases / 2,457,970 assertions, 0 failures** (558
  before this round; the 12 new ones are the two `test_round11_*` files).
* **Android JVM**: `:core:test` **465 tests** (434 before), `:app:testDebugUnitTest`
  **74 tests** (70 before), 0 failures, `--rerun-tasks`.
* **APK**: `versionCode 700`, `versionName 0.7.0`, from the repo-root `VERSION`.

### 10. EXPLICITLY NOT VERIFIED / BACKLOG

* **The turn-around shift is explained but not yet confirmed on hardware.** The
  13.1 cm figure is a synthetic measurement through the production assembler,
  and the mount error it injects is a model of what a 2.4° trim does — not a
  measurement of the owner's actual bracket error, which nothing on the phone
  can observe. The next 0.7.0 capture taken with the refined re-zero, walked
  out-and-back past a distinctive overhead feature, is the experiment that
  settles it.
* **Loop closure has never fired on a real capture.** It refuses scan-020
  correctly and for a reason it can state, and its recovery is proved against
  synthetic ground truth — but no `.lscan` in this repository contains a loop.
  A capture that walks a room and returns to the start (≥4 m of excursion,
  ideally around a corner so the fan turns) is what would exercise it.
* **The observability gate is the interesting limitation, not a bug.** A
  dead-straight out-and-back with a pushbroom cannot close its own loop: the
  along-walk component of the drift is not measured by anything in the scan.
  Walking a curve fixes it. This is worth saying to the owner as scanning
  advice, not only as a code comment.
* **Coverage colouring is not verified on a device.** `CoverageGrid` is unit
  tested and the renderer integration compiles and follows the existing upload
  budget, but no Filament frame has been drawn with it here.
* The cue player itself (Vibrator/ToneGenerator) is untested by construction —
  it is the deliberately thin shell under `CueScheduler`, which is where all the
  behaviour is.
* `--d6-loopclose` re-resolves the container twice (once as shipped, once with
  closure) so its before/after comparison is honest; on scan-020 that is ~90 s.
* Nothing was run on kc-m4; macOS/desktop untouched.

---

## ROUND 12 — THE TRIM WAS NOT IT, AND THE PROJECT HAD NO RULER

Owner on 0.7.0, walking at normal pace: *"quality not so good, still shift."*

ROUND 11 closed by predicting the turn-around shift was mount-trim error, and
handed ROUND 12 the experiment that would test it: two of the owner's own
captures, minutes apart, same room, same operator, same build — one with a
refined re-zero and one without.

**The prediction was wrong, and finding that out required building a
measurement instrument this repository did not have.**

Field material: `captures/scan-026.lscan`, `captures/scan-028.lscan`,
`captures/scan-020.lscan`, `captures/lidarscan-capture-log-2026-08-18-1418.txt`.

### 1. THE A/B PAIR — and why the premise was already broken

| | scan-026 | scan-028 | scan-020 (ROUND 10's reference) |
| --- | ---: | ---: | ---: |
| `mountTrim.spreadP90Deg` | **0.44°** | **2.40°** | 2.40° |
| `mountTrim.sampleCount` | 34 | **244** | 30 |
| walked | 15.3 m in 61.2 s (0.25 m/s) | 15.8 m in 49.1 s (0.32 m/s) | 10.8 m in 202 s (0.053 m/s) |
| points (offline re-resolve) | 126,876 | 101,769 | 293,166 |
| app's grade | **GOOD SCAN** | — | — |

The A/B reads as a 5× difference in trim quality. It is not one.

**The two stored trims are 1.33° apart.** `spreadP90Deg` is the dispersion of
individual ARCore frames about the mean *over whatever window happened to be
averaged* — and ROUND 11's refiner averages over the whole hold. scan-026's
0.44° is a **one-second** dispersion (34 samples); scan-028's 2.40° is an
**eight-second** one (244 samples). The two numbers were never comparable
quantities, and the one number that IS comparable — the split-half
repeatability the refiner computes for the ring — was never stored.

ROUND 11's own write-up says this in words ("the spread measures the JITTER of
individual ARCore frames and holding longer does not reduce it") and then
shipped `spreadP90Deg` as the only quality field in the container anyway.

### 2. THE RULER — `post::measure_map_consistency`, and why it had to be built

Every geometry metric in the repository failed on these captures:

* **the ROUND 10 wall-probe thickness selects ZERO probes on both.** It needs
  200 returns inside a 0.5 m radius cell; only a 5.3 cm/s crawl reaches that
  density. Every crispness claim this project has ever made comes from
  `scan-020`, walked at a twentieth of normal speed. (`--d6-timesweep` now
  takes `--probe-min-points` / `--probe-elongation`; relaxed to 40/6 it finds
  6 probes on scan-026 and reads **8.66 cm** against scan-020's 4.86 cm.)
* **plane-fit RMS averages a doubled surface into one thick one.** Same
  structural blindness ROUND 9 found (sign-blind metrics) and ROUND 11 found
  again (a trim error slides ALONG a wall).
* **occupied-voxel counts** compare a cloud only with itself at another setting.

None of them can see the owner's error, because that error is *a surface
painted twice in two places*.

`slam/post/map_consistency.{h,cpp}` measures exactly that: split the capture
into 8 s windows; for every pair of windows that filled the same 25 cm cell,
fit a plane to the earlier one and report the mean distance of the later one
**along that plane's normal**. Along the normal is the whole trick — a D6's
returns slide freely along a wall, so any 3-D offset is dominated by where the
returns happened to land. A control (one window against itself, split in half)
gives the measurement's own floor on that capture.

Hand-rolled 3×3 Jacobi, no Eigen, no RNG, no clock, total sort order so the
answer is **bit-identical** under point reordering.
`engine/tests/test_round12_map_consistency.cpp` proves it against injected
truth: a clean two-pass map reads 0.00 cm, 2/5/10/20 cm of injected
perpendicular offset comes back within 10 %, **20 cm of slide ALONG the walls
reads 0.00 cm while 20 cm perpendicular reads 20.0 cm**, a one-pass map returns
*not measurable* rather than zero, and a shuffled cloud returns the identical
double.

New tools: `engine_cli --d6-selfcheck` (the score) and `--d6-dump` (points with
their own timestamps + trajectory, for analysis outside the printf).

### 3. THE VERDICT — outcome (b), and the trim theory is refuted by experiment

```
$ engine_cli --d6-selfcheck captures/scan-0NN.lscan
```

| capture | surfaces re-painted 8 s apart | 16 s | 24 s | measurement floor |
| --- | ---: | ---: | ---: | ---: |
| scan-020 (crawl) | **0.70 cm** | 1.70 | 1.58 | 0.29 cm |
| scan-026 (0.44° trim) | **5.26 cm** | 9.28 | 7.13 | 0.99 cm |
| scan-028 (2.40° trim) | **4.45 cm** | 6.21 | 6.23 | 0.70 cm |

**Both walking-pace captures shift, by 6–7× what the crawl does, and the one
with the "good" trim is the WORSE of the two.**

The decisive experiment — re-resolve each capture's bytes with the OTHER
capture's mount extrinsic (`--d6-selfcheck --mount-from`), which is the whole
trim hypothesis with nothing else changed:

| | own trim | other capture's trim | change |
| --- | ---: | ---: | ---: |
| scan-026 | 5.26 cm | 5.00 cm | −5 % |
| scan-028 | 4.45 cm | 4.21 cm | −5 % |

Occupied 3 cm voxels move by **0.15 %**. Swapping the trims makes both maps
very slightly *better*, which is what noise looks like and is impossible if
either trim were the right one. **A 1.33° trim difference is worth a few
centimetres at 3 m and the shift is not it.**

### 4. THE OTHER SUSPECTS, RANKED AND TESTED

**(a) Lidar↔pose clock offset — dead, again, now at 6× the speed.** ROUND 10's
null result was measured on scan-020, where 100 ms of skew is 5 mm and
invisible. Re-run on the walking captures with the new metric:

| offset | scan-026 | scan-028 |
| ---: | ---: | ---: |
| −100 ms | 5.07 | 4.51 |
| −50 ms | **4.91** | 4.99 |
| 0 ms | 5.26 | 4.45 |
| +50 ms | 6.49 | **4.44** |
| +100 ms | 6.86 | 5.02 |

Flat to ~7 %, and the two captures' minima disagree in sign. There is no
offset. The default stays 0.

**(b) Live-vs-offline divergence — dead.** The cached `map.bin` each capture
sealed and an offline re-resolve of the same bytes have **identical extents**
and voxel counts within 0.02 % (scan-028 and scan-020 produce the identical
point count; scan-026 differs by 27 points out of 126,876). What the owner
looked at is what is on disk. Outcome (c) is ruled out.

**(c) ARCore rotation — tracks the gyro at r = 0.9994.** The recorded 400 Hz
phone gyro cross-correlated against the ARCore-implied rotation rate over every
pose interval: r = 0.9994 (026), 0.9993 (028), 0.9796 (020). Total rotation
agrees to 2.5 %.

Over multi-second horizons, with the gyro bias fitted, ARCore and the gyro
disagree by:

| horizon | scan-026 | scan-028 | scan-020 |
| ---: | ---: | ---: | ---: |
| 1 s | 0.10° | 0.11° | 0.08° |
| 8 s | **0.89°** | **0.62°** | **0.33°** |
| 16 s | 1.13° | 1.05° | 0.61° |

At the D6's ~3 m median return range, 0.89° is 4.7 cm — the right size for the
observed 5.26 cm. **But the disagreement does not say which of the two is
wrong**, and a gyro scale-factor error at the walking captures' 15 °/s mean
rate would produce the same signature.

So it was tested rather than argued: the recorded pose stream was rewritten
with a complementary filter (gyro integrated forward, corrected toward ARCore
with time constant τ) and the containers re-resolved through the production
pipeline.

| τ | scan-026 @8 s | scan-028 @8 s |
| ---: | ---: | ---: |
| 0 (= ARCore, the control) | 5.26 | 5.98 |
| 0.3 s | 5.95 | 5.97 |
| 1 s | 5.62 | 5.90 |
| 3 s | 7.14 | 6.09 |
| 10 s | 5.97 | 6.30 |
| 30 s | 5.88 | 6.00 |

**No improvement at any time constant, and occupied voxels rise slightly.**
Substituting the gyro for ARCore's mid-band orientation does not recover the
error. Nothing was shipped from this; it is recorded as a falsified hypothesis
so it is not re-tried from reasoning.

**(d) IMU densification fallbacks are NOT the story, but the accounting is
broken.** 32 % of returns fall back on both walking captures (31.5 % on the
crawl) — the same fraction at 6× the speed, so it is not speed-related. But
`imu_densified + imu_fallbacks` exceeds the point count, and the per-reason
counters sum to a quarter of `imu_fallbacks` (scan-026: 54,393 fallbacks,
11,280 accounted). ROUND 10 added those counters precisely so "43 % fall back"
could be acted on; they do not add up. **Backlog.**

### 5. WHAT IT ACTUALLY IS

**Local geometry at walking pace is excellent. The trajectory is not.**

The measurement floor — one 8 s window against itself — is **0.99 cm** on
scan-026 and 0.70 cm on scan-028. Within a pass, at normal walking speed, the
pipeline resolves surfaces to under a centimetre. That retires the whole class
of per-return errors: the fan formula, the per-byte time slicing, the sample
cadence, the mount extrinsic, the pose interpolation.

What grows is the disagreement between passes, and both captures are LOOPS the
app never closes:

```
$ engine_cli --d6-loopclose captures/scan-028.lscan --window 3
  trajectory : 15.8 m walked, furthest from start 4.97 m, start->end 0.80 m
  DECISION: correction-too-big
  ICP: converged=1, 4519 inliers (0.756), rms 0.151 m
  same place, mean nearest-neighbour distance: 51.39 cm -> 13.87 cm
  measured drift over the loop: 0.7873 m, 18.870 deg
```

scan-026 ends **0.52 m** from where it started after 15.3 m; scan-028 ends
**0.80 m** after 15.8 m. ROUND 11's `TrajectoryLoopCloser` finds the revisit on
both and refuses both — 026 `icp-failed`, 028 `correction-too-big` /
`geometry-rejected` depending on the submap window. Its refusals are *correct*
given what it can observe: the rotations ICP proposes (14–19°) are impossible
against the gyro cross-check above (≤1.1° over 16 s), which is the pushbroom
null-space wander ROUND 11 built the observability gate for. **The gate is
doing its job and the closure is therefore unavailable.** That is the honest
state, and it is the largest open item.

### 6. THE scan-028 TRIM ANOMALY — root-caused, three defects

`sampleCount = 244, spreadDeg = 3.58, spreadP90Deg = 2.40`. 244 samples at
30 Hz is 8.1 s, which is `MountTrimRefiner.DEFAULT_MAX_HOLD_MS` exactly — the
**timeout** path, so `refined` was false and the label read "Set — … (as good
as it got)".

1. **The whole-hold mean was stored with its spread gated against nothing.**
   `MountTrimSampler.capture` judges the trailing 1 s; the refiner then
   discarded that trim and stored a mean over the whole hold, recomputing
   `spreadDeg`/`spreadP90Deg` over the longer set and comparing them to no
   threshold. A hold whose last second was perfect and whose first seven
   wandered passed, with the wander in the stored mean. **Fixed**: the
   whole-hold dispersion must clear the same gate, and falls back to the 1 s
   trim (which did pass) when it does not — refusing would throw away a good
   answer because a worse one existed.
2. **The accuracy figure was computed and thrown away.** `stabilityDeg` (split
   half) now reaches `MountTrim`, DataStore and `project.json`, defaulted to
   −1 so every persisted 0.7.0 trim still decodes. `MountTrim.accuracyDeg` /
   `accuracyIsPoor` / `qualityRank` are the API; `spreadP90Deg` keeps its
   meaning and its name (ROUND 8's rule — silently redefining a number in a
   field log is how the next report gets misread).
3. **The auto-recapture at Start could make the trim worse, and ran constantly.**
   Any gate-passing 1 s sample replaced the incumbent unconditionally, so a
   0.35° guided hold could be overwritten by a 2.49° one. And `fromPreviousRun`
   alone marks a trim stale — the owner's log shows `trimSource=restored-
   previous-run` ~100 s after a `this-run` line, so the app process restarts far
   more often than the "10 minutes" wording implies. **Fixed**: compares
   `qualityRank` and keeps the better, logging which and why.

Two smaller ones from the same reading, both in the owner's log:
`mount hold released early: holdMs=0 samples=1 … gate=true` — the gate looked at
the whole ring rather than the hold, so it reported a verdict on a hold that had
not happened; and `evaluate()` did not filter `tracking` while `capture()` did,
so the ring and the stored trim disagreed about what "the hold" was. Both fixed.

### 7. THE START GATE — `TrackingWarmup`

```
14:13:20.167 [session] start: project=scan-025-…
14:13:27.079 [ar] SECTION BREAK #1 reason=IMPOSSIBLE_STEP jump=2.015m/0.61deg gapMs=33
14:13:53.646 [ar] SECTION BREAK #2 reason=IMPOSSIBLE_STEP jump=0.608m/0.45deg gapMs=33
```

**2.0 metres in 33 milliseconds, 6.9 seconds after Start**; scan-028 has the
same shape 3.7 s in. Two of the session's three early breaks are inside the
first seven seconds of a capture, and scan-026 — started after a 1.1 s mount
hold from a settled tracker — has none in 61 s.

**ARCore reported TRACKING with pose quality GOOD across every one of those
jumps.** All three containers decode with `tracking_lost = 0` and a single pose
quality throughout. The tracker does not report re-anchoring as a failure; it
just moves. So the gate is not the tracking flag but a property of a WINDOW:
2 s of poses with no step a person could not take. It **waits, never refuses**
(4 s cap, then starts anyway with an amber note) — a Start that can refuse is
ROUND 10 item 38 arriving by another road.

### 8. THE GRADE STOPPED OVER-CLAIMING

0.7.0 graded scan-026 **GOOD SCAN — "One section, no tracking drops, 8757
points per metre"**, and scan-026 is the worst of the three captures measured.
Every input to that grade is a count; nothing on the card looked at geometry,
and the sentence did not say so.

Now: the GOOD sentence says *"Coverage checks passed; alignment is not measured
on the phone"*; a mount trim measured worse than 1.0° caps the grade at FAIR
with a named reason (ROUND 11 measured 1.4° = 16.3 cm of doubled feature at
3 m); and `LoopReturnTracker` reports the walk's return-to-start gap **under**
the grade with its condition attached — *"if you finished where you began, that
gap is tracker drift and it is the largest error in this scan"* — and
deliberately NOT as a grade input, because the app cannot know whether the
operator meant to finish where they started.

### 9. TESTS

* Engine **575 cases / 2,458,000 assertions**, ctest 7/7 serial (was 570 /
  2,457,970). New: `test_round12_map_consistency.cpp` (5 cases).
* `:core` **480**, `:app` unit **74**. New: `MountTrimRound12Test` (6),
  `Round12StartGateAndGradeTest` (9).
* Emulator (`b4_test`) instrumented: **17 / 17**, 0 failures — run against a
  native library rebuilt from this round's engine sources, so the new
  `map_consistency.cpp` is compiled into the Android ABI build too.
* ABI unchanged at **9** — nothing new crosses the C ABI this round.

### 10. WHAT IS STILL OPEN

* **The drift itself is not corrected.** Both of the owner's walks are loops
  with 0.5–0.8 m of end gap and the loop closer refuses them for reasons it can
  state. Making closure work for a pushbroom means constraining the rotation
  from the IMU (which the gyro cross-check shows is good to ~1° over 16 s)
  instead of letting ICP invent 14–19° in its null space, and solving for
  translation only. That is the next round's headline and it changes recorded
  geometry, so it needs the same standard of proof ROUND 11 held itself to.
* **`--d6-selfcheck` is not on the phone.** The score exists only in the engine
  CLI; the summary card still cannot measure alignment. Wiring it would be an
  additive C ABI call (ABI 10) plus JNI plus the card.
* **scan-026's vertical extent is unexplained.** 9 % of its points are below
  −2 m and 10 % above +2 m, with secondary clusters at ±3.0 and ±4.5 m at ~3.1 m
  range, while scan-028 — minutes later — is a clean 3 m room (nothing below
  −2.4 m or above +2.1 m). It may simply be a different, taller space; the
  owner can say in one sentence and it is worth asking.
* **The densifier's fallback accounting does not add up** (§4d).
* Auto re-resolve on seal was scoped and dropped: with loop closure refusing,
  an offline re-resolve reproduces the live cloud to within 0.02 %, so it would
  cost a minute of phone time and change nothing.
* The emulator suite passes but has no case for the start gate or the refined
  trim path — those are covered by JVM tests only, because both need a pose
  stream and `CaptureArController` needs ARCore.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

## ROUND 13 — THE BREAKS ARE ARCore RE-ANCHORING, AND THE FIX IS WRITTEN IN THE POSE STREAM

Owner on 0.7.1, two walks minutes apart: *"result not satisfy"*. scan-029 broke
once and graded FAIR; scan-030 cued `tracking_degraded` 0.14 s after Start, then
broke four times and graded POOR.

Field material: `captures/scan-029.lscan`, `captures/scan-030.lscan`,
`captures/scan-028.lscan` (the clean reference), `captures/scan-026.lscan` (the
contaminated mount), `captures/scan-020.lscan` (the only capture with camera
keyframes), `captures/lidarscan-capture-log-2026-08-18-1707.txt`.

### 1. THE MEASUREMENT THAT SETTLED IT

`poses_ar.bin` and `imu_phone.bin` parsed directly, in the same clock domain.
Over the same 33 ms as each break:

| break | ARCore pose jump | gyro, integrated over the SAME 33 ms |
| --- | ---: | ---: |
| scan-030 #1 (t=8.13 s) | 0.78 m / **13.53°** | 0.23° |
| scan-030 #2 (t=15.53 s) | 0.97 m / **11.55°** | 1.16° |
| scan-030 #3 (t=26.66 s) | 1.23 m / **11.58°** | 0.94° |
| scan-030 #4 (t=27.63 s) | 1.06 m / **8.08°** | 0.49° |
| scan-029 #1 (t=50.87 s) | 1.05 m / 0.54° | 0.44° |
| scan-028 #1 (t=3.73 s) | 0.30 m / 0.49° | 0.50° |

**The phone did not rotate.** 13.53° in 33 ms is 410 °/s; the gyro says 7 °/s.

And there is no warning. Rolling 1 s |ARCore − gyro| rotation disagreement,
excluding windows containing a break:

| | scan-028 | scan-029 | scan-030 |
| --- | ---: | ---: | ---: |
| median | 0.049° | 0.051° | 0.069° |
| p90 | 0.178° | 0.143° | 0.282° |
| median over the 4 s BEFORE each break | 0.062° | 0.040° | 0.037–0.209° |

VIO is healthy at 0.05°/s right up to the frame it snaps. **A section break is
not a failure the app could have prevented by gating harder at Start — it is
ARCore doing its own loop closure**: recognising a place, deciding it had
drifted, and moving its world frame. Everything before the snap is in the old
frame, everything after in the new one, and the pushbroom keeps resolving
against whichever is current — which is exactly the metre-apart map the owner
saw.

There are two flavours, and they matter: scan-028/029's breaks are
**translation-only** (0.3–1.05 m, rotation matching the gyro) — ARCore
correcting accumulated position drift; scan-030's four are **full 6-DoF
re-anchors** with 8–13.5° the phone never made.

### 2. FOUR HYPOTHESES, EACH KILLED BY ITS OWN MEASUREMENT

**(a) The mount occluding the rear camera — REFUTED. No bracket redesign.**
scan-029/030 have **no keyframes at all** (`captureCameraKeyframes=false`; ROUND
11 item 39 gated the recorder at the flow when colorize was hidden), so the
brief's premise that both bundles hold `kf_*.jpg` is wrong and is recorded here
so it is not assumed again. `scan-020` has 403 frames from the same rig and the
same mount, and answers the question: exposure-normalised per-pixel over all
403, **not one pixel is dark in more than 80 % of frames**; 1.6 % are dark in
more than half, and those are a smooth bottom-right gradient (vignetting plus
shadowed floor), not a hard-edged silhouette. A bracket intruding into the FOV
would be a stationary dark region at ~100 %. It is not there.

What the frames DO show is a real environmental hazard: `kf_000200.jpg` is a
featureless close-up of wood grain at well under half a metre. Across all five
captures the D6's own returns say the same thing — **57 % of returns are inside
1.5 m and 17–20 % inside 1 m, median range 1.31–1.39 m**. This is a small flat,
the camera is often close to a blank surface, and that is the condition ARCore
re-anchors in.

**(b) Puck vibration — REFUTED, and it points the other way.** Band-limited gyro
RMS, 8–12 Hz (the D6 spins at 10 Hz):

| | 020 | 026 | 028 | 029 | 030 |
| --- | ---: | ---: | ---: | ---: | ---: |
| gyro 8–12 Hz | 2.82° | 2.47° | 2.63° | 2.21° | **1.75°** |
| gyro >12 Hz | 0.96° | 0.69° | 0.77° | 0.73° | 0.91° |

The capture that broke four times has the least spin-band vibration of the five.

**(c) CPU starvation from `liveSlam=true` — REFUTED at the source and in the
data.** scan-029 ran `liveSlam=false` and scan-030 `liveSlam=true` (the log line
is authoritative; `project.json`'s `captureDefaults` says `true` for both, which
is the persisted default and not what the session ran with — worth knowing when
reading a container). But `Impl::on_page_update` forwards points to the LIO only
under `if (u.stream != StreamId::kLidarMid360) return;`, and the IMU path is
`on_mid360_imu`. On a D6 the odometry thread is created, waits on a condition
variable and is fed nothing; `map_store` is the caller's PageStore so there is
not even an allocation. And the streams agree — chunk inter-arrival:

| | D6 median / gaps>200 ms | pose median / gaps>200 ms | IMU median |
| --- | ---: | ---: | ---: |
| scan-028 | 6.3 ms / 2 | 33.3 ms / 1 | 2.5 ms |
| scan-029 | 6.3 ms / 1 | 33.3 ms / 1 | 2.5 ms |
| scan-030 | 6.3 ms / 1 | 33.3 ms / **0** | 2.5 ms |

scan-030 is the *healthiest* of the three by cadence. `liveSlam` on a D6 costs
one sleeping thread. (It is still wrong to offer the switch on a D6 rig —
**backlog**.)

**(d) Turning too fast — explains two of four.** Net rotation over the 2 s before
each scan-030 break: 2.5°, 8.9°, **133.5°**, 51.5°. Breaks 3 and 4 follow a real
135°-in-2 s spin. Breaks 1 and 2 follow almost nothing. Instantaneous gyro rate
at every break was 8–37 °/s, at or below the capture's own p90 of 38 °/s.

**(e) And one that is OURS.** Isolating 30–200 Hz accelerometer energy in a
25 ms envelope finds bursts that map **1:1 onto the app's own logged cues** —
10 bursts / 10 cues on scan-030, 1/1 on scan-028 and scan-029, each burst
~0.28 s ahead of its log line and of the right duration for `CuePatterns`.
**No external notification fired in any of the three captures.** But scan-030's
break #4 came **0.51 s after the 130 ms buzz fired for break #3**, and is the
only one of the four with high-frequency energy in the half second before it
(z = 178 vs 2.0 / 7.3 / 10.2). Buzzing at full amplitude while ARCore is
re-establishing itself is a self-inflicted risk. Fix named, not yet built:
defer the section-break cue until tracking has re-settled and drop its
amplitude. **Backlog.**

### 3. SECTION STITCHING — `post::stitch_sections`

`slam/post/section_stitch.{h,cpp}`. Hand-rolled, no Eigen, no RNG, no clock.

**The correction is analytic.** `T_k = pose_after · pose_before⁻¹` is the frame
change ARCore applied, and by the same identity `trajectory_loop.h` derives
(`p' = C·p` for a world-frame left-multiplication) the cloud needs no
re-resolve. Per-section corrections compose `C_N = I`, `C_k = C_{k+1}·T_k`, so
everything lands in the **last** section's frame — the most recently re-anchored
one, i.e. the frame ARCore currently believes. The correction is piecewise
CONSTANT where `TrajectoryCorrection`'s is an arc-length geodesic, and that
difference is the physics: VIO drift accumulates smoothly with distance, a
re-anchor happens in one frame.

**The refinement solves for TRANSLATION ONLY**, rotation frozen at `T_k`. This
is ROUND 12 §10's open item answered. The 14–19° rotations ICP proposed were
always the pushbroom null space; the rotation was never the unknown. With three
unknowns instead of six the point-to-plane normal equations reduce to a 3×3
solve, and **the system matrix `Σ wᵢ nᵢ nᵢᵀ` IS the observability** — λ_min/λ_max
of it, no sampled-normal proxy. On scan-030 it reads 0.05 / 0.09 / 0.42 / 0.65
and declines three of four seams by name (`unobservable`, `not-converged`,
`refinement-too-big`), keeping the analytic transform each time. On scan-029 it
accepts, and earns it: across-seam mismatch **14.65 cm → 12.56 cm**.

**Two bugs found while proving it, both worth naming.**

1. **The detector fired on poses the tracker had disowned.** scan-030 opens with
   **14 poses at exactly (0,0,0) with `quality=0`, `tracking_lost=1`** — ARCore
   had no pose yet. The step out of them is 1.20 m / 10.79 deg in 67 ms and
   looks exactly like a re-anchor, so the engine found 6 sections where the app
   found 5, and folding that phantom transform into the composition made the
   stitched map *worse*. `TrajPose` gained `quality` / `tracking_lost` (additive,
   defaulted to a good pose) and the detector skips any pair involving a pose
   the tracker disowned — which also makes it agree with the Android
   `PoseSectionTracker`, which is only ever fed tracking poses.
2. **`section_of` used `lower_bound` where the comment said `upper_bound`.** A
   point or pose stamped exactly at a seam is `pose_after`'s stamp and belongs to
   the NEW frame. Getting it backwards misplaces exactly one pose per seam — and
   since each of those is then a metre out, it took scan-030's stitched
   trajectory from 0.27 m of vertical wander to **1.56 m**. Four poses. There is
   now a test case for the composition order specifically.

Wired into `D6ResolvePipeline` behind `stitch_sections` (default **OFF** — same
"replay == capture" argument as `close_loops`), running **before** loop closure
and correcting `*out_trajectory` as well as the cloud, so the closer sees one
walk instead of five. `engine_cli --d6-stitch` resolves the container both ways
and prints the comparison.

### 4. THE RESULT, AND THE ONE NUMBER THAT IS NOT SELF-REFERENTIAL

```
$ engine_cli --d6-stitch captures/scan-030.lscan
```

| | scan-028 | scan-029 | scan-030 |
| --- | ---: | ---: | ---: |
| sections | 2 | 2 | **5** |
| first section moved into the last frame | 0.303 m / 0.49° | 1.040 m / 0.55° | 0.517 m / 5.53° |
| **trajectory vertical extent** | 0.167 → **0.102 m** | 0.637 → **0.359 m** | 0.820 → **0.271 m** |
| start→end gap | 0.799 → 0.576 m | 0.724 → **0.353 m** | 0.828 → 0.616 m |
| `--d6-selfcheck` @8 s | 4.45 → 4.45 cm | 6.00 → **5.72 cm** | 5.85 → 5.83 cm |

**The vertical extent is the proof.** It is the only quantity here that does not
come from the same measurement that produced the correction: the operator walks
on a flat floor, so the trajectory's extent along gravity is bounded by the
reach of an arm. Forward, scan-030 goes 0.82 m → 0.27 m. Inverted, it goes to
1.55 m; scan-029 0.64 m → 0.30 m forward, 1.20 m inverted. **The sign is not a
matter of opinion.**

**And the honest negative: `--d6-selfcheck` is nearly blind to a section break,
which is why nothing in this repository ever caught one.** It compares 25 cm
cells filled twice — and two sections a metre apart fill *different* cells, so
there are no pairs and the ruler reports nothing wrong. That is the same class
of structural blindness ROUND 9 found in sign-blind metrics and ROUND 11 found
in flatness metrics, a third time. Stitching makes the sections overlap, which
CREATES cross-section pairs carrying ARCore's residual drift, so a few
longer-separation buckets get slightly worse while the map gets dramatically
better. The number that speaks about a seam is the across-seam mismatch.

**Is the stitched scan-030 usable?** Structurally yes — one frame, a flat
trajectory, five pieces registered to within the seam residual. But four of its
five sections are 8–13 s long and its end gap is still 0.62 m over 15 m, which
is ARCore's own drift and is the loop closer's job, not this one's. The honest
grade for a 5-section walk is still RESCAN; stitching turns "worthless" into
"recognisable", not into "good".

### 5. THE MOUNT WATCHDOG — AND THE CHECK THE BRIEF ASKED FOR CANNOT WORK

`slam/post/mount_watch.{h,cpp}`, `engine_cli --d6-mountcheck`.

**"Fan-vs-gravity" is not observable.** Every D6 return leaves the fan formula
with `z == 0` exactly (`d6_fan.h`), so the returns lie in the *assumed* plane by
construction; the fan plane in the phone frame is `M·ẑ`, a function of the
assumed mount and of nothing measured. Comparing it with gravity compares an
assumption with itself, and a rotation about the puck's own spin axis is
invisible to any test of that shape.

**Where the returns LAND is observable.** A vertical fan on a phone held at
~1.4 m paints floor to ceiling, and nothing can be further from the sensor
vertically than the room is tall. First **six seconds** of each capture:

| capture | median per-revolution vertical extent | returns >2.5 m above/below | median range |
| --- | ---: | ---: | ---: |
| scan-020 | 2.71 m | **0.00 %** | 1.29 m |
| **scan-026** (owner rotated the puck) | **6.15 m** | **19.49 %** | 1.54 m |
| scan-028 | 2.64 m | **0.00 %** | 1.58 m |
| scan-029 | 2.59 m | **0.00 %** | 1.55 m |
| scan-030 | 2.60 m | **0.00 %** | 1.59 m |

Exactly zero on four good captures, 19.5 % on the bad one, at the *same median
return range* — so it is not a taller room, it is the same ranges arriving where
no room is. The gate is the impossible-elevation fraction (2 %, two orders of
margin either side); the extent is supporting evidence that cannot fire alone
because a genuine atrium exceeds it honestly. It **warns, never refuses**, and
returns `not-measurable` rather than `ok` when it has seen too few revolutions.

**Remaining seam, named:** the phone-side wiring needs an additive C ABI call
(ABI 10) + JNI + the cue, exactly as ROUND 12 scoped for `--d6-selfcheck`. The
measurement, the gate and the proof are shipped.

### 6. DO NOT DISTURB (owner item 47)

`core/.../capture/CaptureFocus.kt` holds every decision and is unit-tested;
`app/.../capture/DoNotDisturbGuard.kt` only talks to the framework.

`INTERRUPTION_FILTER_PRIORITY`, deliberately not ALARMS or NONE. An
interruption filter governs *notifications*; a foreground `Vibrator.vibrate()`
is not one and is unaffected — but `ToneGenerator` plays on
`STREAM_NOTIFICATION`, which ALARMS **would** mute, silencing the app's own
audio cue. PRIORITY is the weakest filter that does the job, and taking more
than is needed from a user's phone is how a default-ON feature gets switched
off. It engages only from `INTERRUPTION_FILTER_ALL`, because taking over a
filter the user already set would mean restoring a *weaker* one at stop — this
feature turning somebody's DND off. Restore is skipped if anything moved the
filter mid-capture (bedtime rule, the user, a work profile): that is a newer
decision than ours. Released on Stop, on a failed seal, and in `onCleared`
(abandon), all idempotent; `restoreOrphaned()` covers process death. Never
blocks Start — `dnd=unprotected-no-permission` goes in the session-start log
line instead, so the next field report says whether the walk was protected.
Default ON, one Settings switch.

Measured caveat, stated at the top of `CaptureFocus`: §2(e) found **no external
notification during any of the three captures**, so this is hygiene that removes
a real failure mode before it bites, not the explanation for scan-030.

### 7. THE 0.7.1 BUGS — ONE ROOT CAUSE, THREE PLACES

`"a %f" + "b".format(x)` does not format `"a" + "b"`. A method call binds
tighter than `+`, so `.format()` applied to the LAST literal fragment only.

* **The seal summary log line.** `pathM=%.1f sections=%d drops=%d ptsPerM=%.0f`
  was never substituted and printed verbatim; the six arguments were consumed by
  the two `%s` that *were* in scope, so **`trimAccuracyDeg=15.99` was metres
  walked and `loopEndGapM=2` was the section count**. That is the whole of bug
  (B): nothing was wrong with the trim, `stabilityDeg=0.39` was correct, and
  15.99/0.39 was never a unit conversion. The summary **card** was never
  affected — it reads the `ScanSummary` fields directly.
* **`PoseSectionBreak.summary`, IMPOSSIBLE_STEP.** Shipped reading *"pose
  stepped %.2f m / %.1f°"* to the operator — the sentence shown when a scan
  breaks, which is the sentence that mattered most this round.
* **`MountCalibrationScreen`** — *"Pattern size: %.2f x %.2f m"*.

A repository-wide sweep for the pattern found six candidate sites; three were
correct (the placeholders happened to live in the formatted fragment) and the
three above were not. All fixed as one string with one format call, so the arity
is checked in one place.

### 8. TESTS

* Engine **584 cases / 2,503,041 assertions** (was 575 / 2,458,000), ctest
  **7/7 serial**. New `test_round13_section_stitch.cpp` (8 cases): a clean
  capture is a **bit-identical** no-op (`corr.active()` false, max error exactly
  0.0); one injected 0.9 m/11° re-anchor is undone to the operator's own motion
  across the pose gap and nothing more; four re-anchors compose in the right
  order (the reversed order is metres out, so it discriminates by two orders);
  a disowned pose is not a seam, and clearing the flags on the same stream
  restores the seam; the answer is identical with the cloud presented in reverse
  order. Plus the watchdog: a good mount passes, a rotated one is caught in six
  seconds, and too few revolutions returns `not-measurable` rather than `ok`.
  Plus `round13/reprocess/...`, which tests the derived-product FORMAT rather
  than its arithmetic: 9,000 points across several chunks, written and read back
  **bit-identically** through the single-file reader, `has_stitched_cloud`
  flipping either side, and deleting the file returning the container to zero.
* `:core` **498** (was 480), `:app` unit **74**. New `Round13FocusAndAdviceTest`
  (9) and `Round13StitchResultTest` (9) — the latter pins the flat
  `DoubleArray(16)` JNI slot layout against the REAL numbers the engine produced
  for scan-030, because a wrong index there is a plausible number in the wrong
  place rather than a crash.
* Emulator (`b4_test`) instrumented: **19 / 19**, 0 failures (was 17), against a
  native library rebuilt from this round's engine sources — so `JNI_OnLoad`'s
  startup check validated **ABI 10** on device. New `Round13ProcessScanTest` (2)
  runs the owner's scan-030 bytes through the real JNI binding and asserts the
  numbers, the words, the untouched sealed streams and idempotency. It is the
  test that found the varargs bug.
* ABI **9 → 10**, additive.

### 9. THE PROCESS BUTTON — ABI 10, AND THE WHOLE PATH

Section stitching shipped engine-side first and the summary card promised a
button that did not exist. It exists now, end to end.

**`post::reprocess_d6_container`** (`slam/post/reprocess.{h,cpp}`) is the one
implementation; both surfaces are thin wrappers over it — the Android app
through `processing_jni.cpp` (which links `scanengine` directly, as
`processing_engine.h` has documented since B6), and everything else through
**ABI 10**: `scan_lscan_reprocess_d6()`, `scan_lscan_has_stitched_cloud()`,
`scan_lscan_mount_check()`, plus `scan_reprocess_options/_result` and
`scan_mount_check_result`. Additive: nothing existing changed size, order or
meaning, so an ABI-9 consumer relinks unmodified. The three tests that pin the
ABI number exist precisely so a bump is deliberate, and all three were updated
by hand.

**The corrected cloud is a NEW file, and that is the doctrine decision.**
`streams/map.bin` is a cache, not a raw stream, so overwriting it would have
been defensible — engine.cpp already says deleting it costs only speed. It is
still not done, because after an overwrite nothing on disk can say whether the
cloud in front of you is the live pass or a correction, and the next field
report would be written from a cloud whose provenance nobody could recover. So
the corrected cloud goes to **`processed/map_stitched.bin`** in the same chunk
framing, beside **`processed/stitch.json`** recording every seam and its
decision. `load_recorded_cloud()` prefers it — at the one function Review, the
thumbnail, Export and Colorize all already go through — so the viewer draws the
corrected map with no change to any caller, and **deleting the two files
restores exactly what the phone sealed.** Verified by checksum: after a full
reprocess of scan-030, `lidar.bin`, `poses_ar.bin`, `imu_phone.bin`, `map.bin`,
`manifest.json` and `project.json` are all byte-identical.

Because a derived product in `processed/` is deliberately NOT on
`FileRecordReader`'s hard-coded stream list (ROUND 9 named that hazard; this is
the first thing to depend on it), it is read by a small single-file chunk
reader with its own CRC check — and a bad CRC there falls back to the sealed
cache rather than failing, because a derived file is regenerable.

**On the phone**: `ReviewViewModel.processScan()` on `Dispatchers.IO`, a
determinate progress bar (the run is tens of seconds; a spinner on that is how
an operator decides the app has hung), and then the result in words — the card
appears only when `sections > 1`, because a button that is always there and
usually does nothing teaches people to ignore it. The headline is the **height
spread**, for the reason §4 gives: it is the only number in the report that is
not self-referential. The end gap is stated and deliberately never sold as an
improvement.

**scan-030 through the real Android path**, on the emulator, against the
owner's actual bytes (`Round13ProcessScanTest`):

```
5 pieces aligned — height spread 0.82 → 0.27 m.
The first piece moved 0.52 m to meet the last. The joins kept the camera's own
correction — the walls here could not measure a better one. Your walk still ends
65 cm from where it began — that is the camera's own drift over the walk, and
aligning the pieces does not remove it.
```

**Two bugs found by putting it on a device, and the second is the interesting
one.**

1. `reprocess` assumed `processed/` existed. A container that never produced a
   preview, or an exported and re-imported `.lscan`, has no such directory and
   the write failed with `kFileError`. It creates it now.
2. **A JNI varargs float promotion that presented as a silent cancel.** The
   progress callback crossed as `CallBooleanMethod(obj, mid, fraction)` — and C
   varargs promote a `float` to `double`, so a `(F)Z` method received eight
   bytes where it expected four. The call threw, the wrapper read the exception
   as "the callback said stop", and the entire reprocess **cancelled with no
   error anywhere**: `ran = 0`, nothing written, and only on the code path that
   passes a progress callback — which is every real one. `CallBooleanMethodA`
   with a typed `jvalue` fixes it. Nothing but running it on a device would have
   found this; the desktop harness over the same C entry point passed.

**The mount watchdog is on the phone too** (`scan_lscan_mount_check` →
`ProcessingRepository.mountCheck`), and it also runs for free inside every
reprocess, so `StitchResult.mountWarning` surfaces *"the lidar is not where the
mount reference says it is — N% of returns landed at heights no room has"*
whenever it fires. It stays silent on OK and NOT_MEASURABLE.

### 10. THE TWO QUICK FIXES §2 NAMED

**The section-break cue is quieter and yields to the tracker.** Amplitude
255 → 150 (the value `TRACKING_DEGRADED` has always used, and three pulses
through a pocket are recognised by count, not by force), plus a **1.5 s quiet
window after any break cue in which nothing buzzes at all** — the risk is to
the TRACKER, not to the operator's attention, so it covers every cue and not
just the one that fired. scan-030's fourth break arrived 0.51 s after the third
break's buzz, inside that window.

This supersedes half of a ROUND 11 test (`a losing cue keeps its debounce and
fires on the next tick`), which asserted the loser fires 100 ms later. Rather
than delete it, it now proves the property more sharply than the original did:
the loser is silent at 1.1 s and fires at 2.6 s — i.e. the moment the quiet
window ends, and not four seconds after the tick it lost, which is what a
consumed debounce would have cost.

**`liveSlam` is hidden on a D6 rig.** ROUND 10 confirmed it gates the Mid-360's
`LioOdometry`, which `on_page_update` can never feed from a D6, and left the
switch visible; three rounds of field logs later it was still the first thing
anyone reached for when a D6 capture went wrong. A control that does nothing is
worse than no control.

### 11. WHAT IS STILL OPEN

* **Auto-Process on seal is not wired.** The button exists in Review; a capture
  that ends with 5 sections still has to be opened and processed by hand. Doing
  it automatically at seal is a minute of phone time on a walk the operator has
  just finished, and worth it — but it changes the "Stop → Projects" flow ROUND
  10 item 38 fixed, so it wants its own round.
* **The end gap is still ARCore drift** — 0.35–0.62 m after stitching. The
  translation-only, gyro-locked solver built here is exactly the machinery the
  loop closer needs; applying it at the loop end (rather than only at seams) is
  a small change to `TrajectoryLoopConfig` and was left out of this round only
  for scope.
* ROUND 12's open items that did not move: the densifier's fallback accounting
  still does not add up (32 % fall back on all three of this round's captures
  too); scan-026's vertical extent is now explained (§5 — the puck was rotated).
* The mount watchdog runs on a SEALED container, not live during a walk. Firing
  it in the first seconds of a capture needs the statistic accumulated inside
  the pushbroom assembler as points resolve; the measurement, the gate and the
  proof are shipped and the phone can already run it on a finished scan.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

---

## ROUND 14 — THE ORIGIN NEVER ZEROED, AND SWEEPING COSTS PARALLAX, NOT ACCURACY

**0.8.1** (`versionCode 801`). Items 50–53 against the owner's 0.8.0 field
session: scan-033 (26.6 m walk, his best capture), scan-034 and scan-035 (both
standing and sweeping the phone), and `lidarscan-capture-log-2026-08-18-1909.txt`.

Two owner sentences drove the round:

> *"The new scan is much better when i go around … but its not good with tilting
> and moving around the phone."*
> *"Does the origin and imu data offset zero every time when the capture start?"*

They turned out to be the same question.

---

### 1. THE ANSWER TO THE SECOND ONE IS NO, AND IT IS WRITTEN IN THE OWNER'S OWN FILES

`CaptureArController.createSession()` short-circuits on a non-null `session`
(`CaptureArController.kt:293`), the controller is a process-scoped singleton
(`AppContainer.kt:261`), and `CaptureScreen` pauses it — never closes it
(`CaptureScreen.kt:457`). `close()` is the only `session.close()` in the app and
**had no caller anywhere in `src/main`**. One ARCore session per process, for
the life of the app.

The consequence is measurable in `poses_ar.bin` without touching any code:

| | first tracked pose | previous capture's last pose | separation |
| --- | --- | --- | ---: |
| scan-034 | (0.321, 0.022, 0.466) | scan-033 (−0.036, 0.009, 0.690) | **0.42 m** |
| scan-035 | (0.187, 0.429, 1.084) | scan-034 (0.120, 0.410, 1.074) | **0.070 m** |

Seven centimetres, across a 49-second gap and a Stop/Start, in the same
coordinate system. Capture N+1 was opening in capture N's origin, holding
capture N's feature map. And scan-033's first pose is *(0.047, 0.087, 0.121)* —
not the origin either, because that session had started 3.5 s earlier during
the mount-trim hold. Nothing about the frame was per-capture.

**Two more leaks, both visible in artifacts the owner already exported.**

* The manifest `sensors` array: scan-033 lists **1** sensor, scan-034 lists
  **3**, scan-035 lists **6**. **Two independent bugs, one symptom.**
  `FileRecordWriter::open()` reset `stats_`, `streams_` and the timestamp but
  never `sensors_`, and the writer is Engine-lifetime while containers are
  per-capture — so each `start_session()` appended onto the last capture's list
  (fixed by a new `reset_metadata()`). And the device list it snapshots was
  itself growing: `PhoneGeorefRecorder.start()` calls
  `nativeAddRtkRoverDevice()` on every capture while its `stop()` only *forgot*
  the id, never calling `scan_engine_remove_device`. scan-035 lists the rover as
  **both id 2 and id 3**, which is the fingerprint. (`RtkRoverConnection`
  already removed its device correctly; only the phone-GNSS fallback leaked.)
* The seal summaries read `drops=1`, `drops=2`, `drops=3` across those three
  captures. Decoding `poses_ar.bin` shows each of the three had **exactly one**
  tracking-loss episode of its own (scan-033 a single dropped frame at
  t = 98.97 s; scan-034 and scan-035 one run each at the very start). The
  counter was simply accumulating: `ArStatus.trackingLossEpisodes` is reset only
  in the `close()` nobody called (`CaptureArController.kt:428`), and
  `CaptureViewModel` reads it straight into `ScanSummary`.

And a third, quieter one: scan-034 and scan-035 each open with **14–15 recorded
poses at exactly (0,0,0) with `tracking_lost = 1`**, spanning 0.46–0.50 s.
That is `pause()`/`resume()` re-acquiring — the app was already paying a
half-second re-tracking cost at every Start, and getting the *old* frame back
for it.

**The fix: `CaptureArController.resetWorldFrame()`, called first thing in
`startCapture()`.** Close the session, build a fresh one, hand it back the
renderer's existing GL texture (newly remembered in `setCameraTextureName`,
because the GL context outlives the ARCore session — only the binding dies),
resume. `ArSessionGate` is what makes this safe from the render thread:
`sessionCreated`/`resumed` both go false for the duration, so an in-flight
`onFrame()` returns null rather than calling `update()` on a closing session.
That is precisely the state machine ROUND 6 built after three crashes in this
file, being used for what it was built for.

It runs **before** the ROUND 12 warmup gate, not after. The gate exists to
refuse a start on a tracker that has not settled; run it first and it would be
vouching for a session that is about to be destroyed.

Two leaks close for free because `close()` already does the work: `motion`
(the `RigMotionTracker` ring the warmup gate reads) and the `ArStatus`
counters.

---

### 2. SWEEPING DOES NOT DEGRADE THE GEOMETRY. FOUR MECHANISMS TESTED AND KILLED

Everything this project can measure says the sweeps are as good as the walk:

| | scan-033 walk | scan-034 sweep | scan-035 sweep |
| --- | ---: | ---: | ---: |
| `--d6-selfcheck` @ 8 s | 1.97 cm | 2.45 cm | 1.74 cm |
| that measurement's own floor | 0.99 cm | 1.03 cm | 1.40 cm |
| `--d6-mountcheck` impossible elevations | 0.00 % | 0.00 % | 0.00 % |
| densifier fallback rate | 31.3 % | 31.9 % | 31.8 % |
| resolved points/second | 1,985 | 2,001 | 2,006 |

* **Pose lag under rotation — REFUTED.** Cross-correlating ARCore's per-frame
  rotation magnitude against the cumulative 400 Hz gyro path over lags of
  ±60 ms puts the minimum at **−5 ms** on all three captures. There is no lag.
* **Orientation fidelity at speed — REFUTED, and this is the number that kills
  "sweep slower".** Pooling all three captures and bucketing by measured gyro
  rate:

  | gyro rate | n | mean disagreement | p90 | at 3 m |
  | --- | ---: | ---: | ---: | ---: |
  | 0–10 °/s | 3,786 | 0.016° | 0.031° | 0.16 cm |
  | 10–20 °/s | 1,828 | 0.024° | 0.050° | 0.26 cm |
  | 20–40 °/s | 1,041 | 0.038° | 0.075° | 0.39 cm |
  | 40–60 °/s | 323 | 0.053° | 0.108° | 0.57 cm |
  | 60–90 °/s | 111 | 0.083° | 0.190° | 1.00 cm |
  | 90–150 °/s | 55 | 0.111° | 0.221° | 1.16 cm |

  ARCore tracks the gyro to a fifth of a degree at 150 °/s. There is no rate
  the owner can reach that costs meaningful orientation accuracy.
* **Intra-revolution smear — REFUTED.** The D6 turns at 10 Hz with 400 returns
  per revolution, i.e. a **0.90° within-fan pitch**. Sweeping at ω °/s advances
  the fan ω/10° between revolutions. The owner's measured medians are
  **0.92° / 0.97° / 1.03°** — almost exactly isotropic. Only 2.4 / 3.0 / 6.8 %
  of revolutions are coarser than 5°.
* **Densification fallback — REFUTED.** Flat at 31–32 % across all three; not
  rate-driven.

**What IS different is parallax**, over 1 s windows:

| | median rotation | median translation | **cm per degree** | windows >20 °/s with <0.5 cm/° |
| --- | ---: | ---: | ---: | ---: |
| scan-033 (walk) | 8.7 °/s | 25.9 cm/s | **2.43** | 1.8 % |
| scan-034 (sweep) | 10.5 °/s | 4.6 cm/s | **0.53** | 10.5 % |
| scan-035 (sweep) | 9.1 °/s | 3.9 cm/s | **0.56** | 5.9 % |

A monocular VIO recovers depth from translation. Rotation on the spot changes
every bearing and creates no baseline: nothing new can be triangulated and the
tracker must lean on the map it already has. **scan-035's break is that at full
size — 1.631 m / 162.57° of pose change in 33 ms while the gyro integrated
1.56° over the same 33 ms.** ROUND 13's worst was 13.53° against 0.23°; this is
a 104× disagreement, and a 162° flip is the signature of relocalising onto a
previously-mapped place with the wrong orientation hypothesis. Which requires a
previous map — see §1. **The two items are one bug seen from two ends.**

Note the control: scan-034 swept just as hard, on the same reused session, and
never snapped. Parallax starvation makes a snap *likely*, not certain.

---

### 3. WHAT SHIPPED FOR ITEM 50

* **`ParallaxWatch`** (`:core`) — 2 s rolling window, fires when rotation ≥ 20°
  carries under 0.8 cm of travel per degree. Threshold swept against the owner's
  captures (5.5 % of the good walk against 46.7 % / 39.7 % of the sweeps, ~8×
  separation); 20°/0.8 is the only row that catches both sweeps at a comparable
  rate, so it is fitted to the technique and not to the snap. Re-anchor
  teleports are excluded from the travel total — folding scan-035's 1.631 m
  step in would report excellent parallax at the exact instant there was none,
  and there is a test for that.
* **`PARALLAX_STARVED`**, lowest priority, gentlest amplitude, **12 s** repeat.
  Every cue is a shake of the tracker this cue exists to protect (ROUND 13's
  finding), so nagging would be self-defeating. It sits under the ROUND 13
  post-break quiet window like everything else.
* **The hint sits ABOVE the keyframe-coverage hint**: a tracker about to lose
  the room outranks thin colour coverage, and both are true through a sweep.
* **`ScanSummary.isFromTheSpot`** — over 20 s with under 5 m of path is judged
  on **points per second**. scan-034's card said *50,124 points per metre* and
  the grader used it; it now reads *2,001 points per second*. The floors
  (1,000 / 400 per second) catch a dead puck and nothing else — a fixed
  viewpoint cannot be told to sweep faster for more returns, and the fact that
  all three captures land at 1,985–2,006/s walking and sweeping alike is itself
  the evidence that sweeping costs no density.
* **`nextWalkAdvice`** for a from-the-spot scan is *"keep walking while you
  sweep"*, not *"slow down a little"* — which was the advice a standing operator
  would have been given.

**The answer to "how fast can he sweep": as fast as he likes.** His median
9–10 °/s is already the isotropic-sampling rate for this sensor, and even his
p99 (64–92 °/s) costs ~1 cm of orientation error at 3 m. The constraint is not
degrees per second; it is centimetres per degree, and the number is **0.8**.

---

### 4. DND: THE FEATURE WAS COMPLETE AND HAD NO DOOR

Root cause, plainly: **`DoNotDisturbGuard.policyAccessIntent()` had zero call
sites in the repository.** Its own KDoc reads *"The caller shows this once"*.
There was no caller. Everything else was correct — the manifest permission is
declared (`AndroidManifest.xml:95`), `engage()` ran on every Start, returned
`NO_PERMISSION`, and logged the right token.

Compounding it: `CaptureFocus.note()` was computed into `_dndNote` on every
Start and **no composable ever collected it** — `CaptureScreen` collects
`georefNote`, `refreshDownshiftNote`, `presetChangeNote`, `liveMapFullNote`,
`mountTrimNote`, and not this one. It was also wiped at seal. A write-only flow
feeding a screen that never read it. And the Settings switch said *"Needs Do Not
Disturb access"* — stating a prerequisite with no way to satisfy it and no way
to check it. `restoreOrphaned()` is a third dead path (still dead; backlogged).

Shipped: `CaptureFocus.shouldAsk()` in `:core` (three inputs, unit-tested); a
first-**entry** `AlertDialog` — never at Start, because `CaptureScreen`'s own
rule is that a mid-walk modal is the worst possible interruption — leading with
the physics rather than the permission; asked once and remembered in
`SettingsRepository`, because declining is an answer; a Settings status row with
a **Grant** pill that re-reads `isNotificationPolicyAccessGranted` on every
`ON_RESUME` (the special-access screen always returns `RESULT_CANCELED`, so the
result code carries no information); and an amber capture-screen note that
**survives the seal** and clears only when the grant arrives. Capture is never
blocked.

---

### 5. MID-360: DIAGNOSED, NOT DELIVERED

`connect()` returning success was never evidence of a link — `Engine::add_device`
constructs a driver and does **no I/O**, and `start_session()` logs a driver that
fails to start and carries on. So the first honest moment came two seconds into
a recording, and the message it produced (*"re-seat the USB-C cable"*) was
probably wrong.

`Mid360Preflight` (`:core`, tested against the owner's real addresses) checks
**addressing before heartbeat** — being told "check the power" when the fault is
the IP is exactly the wrong-diagnosis failure it exists to end — and refuses the
session with the value to type. A new `[net]` log tag records the verdict; the
owner's entire 0.8.0 log contains no network line at all.

The phone's interface IP genuinely cannot be set programmatically:
`EthernetManager`/`StaticIpConfiguration` are `@SystemApi` behind signature-level
`MANAGE_ETHERNET_NETWORKS`. Guiding the operator is the whole available answer.

**Named and not fixed:** the SDK2 backend creates its own sockets inside the
vendored Livox SDK and nothing binds them to the Ethernet `Network`, so with
Wi-Fi up the kernel may still pick the wrong interface (§8 finding 3). The C ABI
has carried `mid360_prebound_fd`/`mid360_prebound_imu_fd` since ABI 5 —
`mid360_jni.cpp` fills 3 of ~30 fields of `scan_device_config` and three
Android-side comments claiming the ABI lacks these fields are **stale**. That
rewire needs the owner's bench to validate. **This is why the version is 0.8.1
and not 0.9.0.**

---

### 6. VALIDATION

Engine **589 cases / 2,503,884 assertions** (from 584 / 2,503,041), `ctest 7/7`
serial including `mid360_sim_e2e` and `gnss_rtk_sim_e2e`, werror clean.
`:core` **527** (from 498), `:app` **80** (from 74), emulator **19/19** on
`b4_test` with the native library rebuilt from this round's engine sources.

**Replay is bit-identical**, which matters because §1's changes are all session
lifecycle and must not touch offline resolve. `--d6-selfcheck` before → after on
all three of this round's captures: self-consistency @ 8 s **1.97 → 1.97**,
**2.45 → 2.45**, **1.74 → 1.74 cm**; resolved points **220,445 / 135,820 /
124,817**, unchanged; every floor, separation row and cell count identical.

The densifier's accounting now closes. scan-034:

```
densify: 124298 on gyro path, 63805 fell back (no-pose 50674, gated 1609,
         no-imu 0, gap 11522, wide-bracket 0, closing 0; sum 63805)
```

The 52,283 fallbacks that had no reason for two rounds were **50,674 returns
with no ARCore pose at all** and 1,609 with a gated one — a trajectory hole, not
a gyro problem. The densifier was never failing there; there was nothing to
densify. Split into two counters because they ask for different fixes, and
`ImuDensifyStats` is C++-side only: `scan_imu_densify_stats` is a frozen ABI
struct and was left alone, with the gap documented in `scanengine_c.h`.

### 7. BACKLOG

* **Rewire the Mid-360 capture path onto the ABI-10 device config**: RAW_UDP
  backend + `NetworkBoundUdpSocket`'s two pre-bound fds, or
  `android_setprocnetwork()` scoped around SDK2 init. Needs hardware.
* **Delete the stale ABI comments** in `ScanEngineNative.kt:309`,
  `mid360_jni.cpp:10`, `mid360_probe.h:5` and `NOTES.md` §8 findings 1 and 4.
* **`DoNotDisturbGuard.restoreOrphaned()` is still dead** — it needs a persisted
  "capture in flight" marker to be worth calling.
* **Propagate driver start failure** out of `start_session()` so a `kFault`
  device fails the Start instead of recording zero bytes.
* Log `DeviceHealth.state`/`lastError` in the NO-DATA line — the current line
  reports `CaptureState`, not the device's.
* Auto-Process on seal (ROUND 13's backlog, unchanged — it changes the
  Stop→Projects flow and wants its own round).
* Gyro-locked translation-only solver at the loop end (ROUND 13, unchanged).
* `resetWorldFrame()` is validated on the emulator and by construction, but the
  ARCore session it destroys and rebuilds is a **stub** there. The half-second
  re-acquire and the crash-safety of the gate transition want one walk on the
  owner's Pixel before this is called proven.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

## ROUND 15 — THE BREAK GOES AWAY WHILE YOU WALK, AND THE PLAN WAS BEING CUT SIDEWAYS

Ships as **0.9.0** (`versionCode 900`). Items 54–57.

Two of the four turned out to be finishing work the tree had already paid for
and never delivered, and one of those two had been quietly producing a wrong
answer for eleven rounds.

### 1. ITEM 54 — LIVE RE-ANCHOR HEALING

#### 1.1 The correction was already analytic; only its DIRECTION was new

ROUND 13 established what a section break is — ARCore recognising a place it
has seen before and snapping its world frame — and that the transform between
the two frames is not unknown but written down in the pose stream as the jump
itself, `T_k = pose_after · pose_before⁻¹`. It applied that offline.

Applying it live needs one decision ROUND 13 did not have to make. **Offline,
sections are brought into the LAST section's frame**, because that is the frame
ARCore currently believes and the one the operator's final position is
expressed in. **Live, that is exactly the wrong way round**: the map already on
screen fills the display and the operator's hands are steering by it, so
re-basing it onto the new frame would make the whole room jump under them at
the moment they are least able to absorb it. So the live correction maps the
NEW frame onto the OLD one:

    C ← C · T⁻¹      applied to every pose after the break

Nothing already drawn moves; the points arriving after the snap land where the
operator expects them. It is also O(1) per break rather than O(pages) — no
existing page is rewritten, which is what makes it affordable on the ARCore
pump thread.

#### 1.2 Where it is applied, and why one frame earlier than the obvious place

`CaptureArController.publishPose()` calls `sections.addBracketed(sample)`, and
the heal goes **between that and `nativePushPose`**. Putting it in the
ViewModel's `onSectionBreak` handler instead would have been simpler and would
have left the pose that *announced* the break to be pushed uncorrected — one
ARCore frame, ~100 D6 returns, of visibly shattered map before the fix lands.
The tracker returns the bracket, the engine takes the correction, and then the
pose is pushed. Nothing shattered ever reaches the display.

`PoseSectionTracker.addBracketed()` is `add()` with the two poses attached.
One detector, one code path — `add()` is now `addBracketed()?.breakInfo` — so
the live healer and `post::stitch_sections` can never disagree about which
frames a seam sits between. For a `TRACKING_REGAINED` break the `before` pose
is the last one that was actually **tracking**, not the last one seen, because
the poses reported during a loss are the tracker's own guesses.

#### 1.3 `Engine::push_pose` records the raw pose and feeds the corrected one

    Status Engine::push_pose(const Pose& pose) {
      const Pose live = live_pose_(pose);     // C * pose, identity until healed
      poses->push_pose(live);                 // interpolator -> densifier -> assembler
      record_pose_(pose);                     // RAW, always
      publish_pose_(live);
    }

`live_pose_()` takes `heal_m`, the smallest mutex in the class and one nothing
else takes, so it cannot participate in a lock cycle. The correction resets in
`start_session()` beside ROUND 14's densifier/ENU/sensor-list resets, for the
same reason: it is a property of ONE capture's re-anchor history.

#### 1.4 The proof, and what it costs

`engine/tests/test_round15_live_heal.cpp` records the SAME synthetic capture
twice — a ROUND 8 walk past a wall at y = 2.4 m, with a **0.89 m / 11°**
re-anchor injected into the pose stream at t = 2.0 s — once with healing and
once without, through a real `Engine`, a real `FileRecordWriter` and the
production pushbroom. A third capture with no break at all is the control.

| | seam offset (median) |
| --- | ---: |
| unhealed live map | **1.463 m** |
| healed live map | **0.027 m** |
| no-break control | 0.007 m |

**The metric is the SEAM, not the spread.** A single plane fit over both slabs
is the wrong ruler for ROUND 12's reason: least squares absorbs the
discontinuity, splitting the difference between the two paintings. The wall is
measured the way the operator sees it — fit the piece painted before the break,
then ask how far the piece painted after it sits from that plane. The injected
transform is also deliberately dominated by the wall NORMAL: the first attempt
put most of it along the wall, where returns slide freely and nothing can see
it, and the unhealed case measured 8 cm instead of 1.46 m.

**The 2.7 cm residual is physics, not slop.** `T` is measured across a 33 ms
interval in which the operator was also moving, so their own ~1° of gait yaw is
inside it: 11° injected, **12.0° recovered**. That is exactly the term
`section_stitch.h` bounds and deliberately does not try to remove, and it is
why the offline stitch — with submaps, refinement and a flat-floor referee —
still has work to do on a healed capture.

#### 1.5 The recording did not change, and the test says so precisely

`streams/lidar.bin` and `streams/poses_ar.bin` are compared **byte for byte**
between the healed and unhealed runs, and the offline re-resolve of the two
containers is **bit-identical, including the discontinuity**.

Two honesty notes the test makes explicit rather than hiding:

* The comparison starts after `lscan::kStreamHeaderBytes`, because
  `StreamFileHeader` carries `t_start_utc_ns` — the wall clock at session start
  — and two runs of a test are milliseconds apart. Exactly three bytes differ
  and they are all in that field; the header is then compared field by field
  (`format_version`, `stream`, `t_start_mono_ns`) so the exclusion is a stated
  scope and not a loophole.
* **`streams/map.bin` DOES differ**, and the test asserts that too. It is the
  live pass's resolved cache, which `reprocess.h` already documents as a cache
  the Process path overwrites, and the healed run's cache is the healed map —
  which is what you want. Asserting it makes the difference a property somebody
  chose rather than one nobody noticed.

#### 1.6 The cue now fires only for a break nobody could fix

`heal_live_frame()` returns `kInvalidArgument` — **without changing the
accumulated correction** — for a pose the tracker disowned, a degenerate or
non-finite rotation, non-increasing stamps, or a transform that fails
`mat4_is_rigid`. The Kotlin side turns exactly that into
`unhealedSectionBreaks`, and `CueConditions.sectionBreaks` is fed from that
counter instead of from the section count.

This matters more than it looks: ROUND 13 measured that the buzz for break #3
was itself the cause of break #4 (0.51 s later, the only break with HF
accelerometer energy). A cue that fires for a break the app has already made
invisible is a shake of the tracker in exchange for nothing.

Every break is still detected, still counted, still written to the manifest and
still stitched offline. What changed is only who gets told — and what they are
told. `sectionHint()` no longer says "this scan is now in 3 sections" for a
break the operator never saw; a fully-healed capture reads *"The camera
re-anchored 2 times — the map on screen was corrected as it happened, and the
joins are recorded so processing can refine them."*, and the actionable ROUND 7
sentence is kept for a break that could not be healed. The new argument
defaults to the old behaviour, so every existing caller is unchanged.

### 2. ITEM 55 — AUTO-PROCESS ON SEAL

#### 2.1 The order is the design

    Stop → seal → VERIFY (projectStore.open) → start processing → emit navigation → card → Done → Projects

Both placements are load-bearing. Processing starts **after** the read-back
verify, so nothing it does can lose a scan that is not already safely on disk;
and it starts **before** `_sealedProjectId.tryEmit`, so a slow or failing
reprocess can never strand the operator on the Capture tab with a scan they
cannot reach.

Nothing about ROUND 10's flow changed. The card is what holds navigation —
`CaptureScreen` navigates when `sessionSummary` goes null — so the card is also
the only place a progress bar can live without inventing a second modal.

#### 2.2 Why it is safe to run while the next capture is arming

`scan_lscan_reprocess_d6` is **handle-less**: it takes a directory and opens
its own `PageStore` inside the engine. It shares no state with the capture
engine, so ROUND 14's `resetWorldFrame()` can be tearing down and rebuilding
the ARCore session on a new Start while this runs, and the two cannot see each
other. That property is also what lets the whole Stop→process→card→Projects
choreography be tested on a bare JVM with no native library
(`CaptureRound15AutoProcessTest`), because the reprocess arrives as an injected
lambda the same way every other Android capability does.

#### 2.3 The fast path, and what it does NOT skip

One section and no mount warning skips the stitch and the second cloud write —
`stitch_sections` is provably identity on one section, so this is about not
paying for the refinement pass, not about correctness. It does **not** skip the
ROUND 12 ruler. A clean one-piece capture is precisely the one whose owner will
believe a repeat-accuracy number, and a card that had nothing to say about it
would be the wrong silence.

#### 2.4 Failure never loses a scan

Any throw, any null, and any `ran == false` all land on the same sentence:

> **Processing failed — the scan is saved. Open it and tap Process to try
> again.**

`ran == false` gets its own test case because ROUND 13 shipped a bug with
exactly that signature — the JNI progress callback's float→double promotion
silently cancelled every real reprocess and returned `ran = 0` with no error
anywhere. Treating it as success would re-introduce the same invisibility.

Dismissing the card does not cancel a run in flight; the job is on
`viewModelScope` under `NonCancellable`, like the seal.

### 3. ITEM 56 — FLOOR PLAN ON THE PHONE

#### 3.1 The bug: `UpAxis::kZ`, hard-coded, for eleven rounds

The brief said A12 had never been wired to Android. That was half right and the
other half was worse. `ProcessingEngine::run_plan()` **is** wired — there is a
`Routes.PLAN` destination reachable from Review's "Floor plan" pill — and it
contains:

    in.up = scanengine::plan::UpAxis::kZ;

A12 defaults to Z-up because a Mid-360 session is gravity-aligned into a Z-up
frame. **A D6 session's world frame is ARCore's, where +Y is up** — the same
axis `SectionStitchReport::up_axis = 1` reads and the same one `mount_watch.h`
calls gravity. Slicing a D6 cloud at Z 1.0–1.5 m takes a 50 cm **vertical
slab** through the room.

It is the worst shape of wrong: it does not fail. It produces walls, it
produces a scale bar, and it never closes a room. Measured on scan-033:

| | points in band | "footprint" | walls |
| --- | ---: | ---: | ---: |
| Z-up (shipped) | 9,211 | 8.38 × 2.78 m | 7 |
| Y-up (correct) | 21,143 | 14.52 × 9.38 m | 9 |

2.78 m is the ceiling height. `test_round15_plan.cpp` builds a 6.0 × 4.0 m
synthetic room in a Y-up world and asserts both halves: Y-up recovers four
paired-face walls, the footprint and the area to 3 %; Z-up closes no room and
reports a "height" under 2.6 m. The fixture is 6 × 4 and not 5 × 5 on purpose,
because `UpAxis::kY` maps plan x = world z and a square room would pass either
way.

#### 3.2 Why a D6 plan slice is thin, with numbers

A COIN-D6's fan is vertical and its 10 Hz revolution paints a **line**, not a
sheet. A 50 cm horizontal band therefore holds only where that line happened to
cross it. On the owner's best capture — scan-033, the 26.6 m loop — that is
**21,143 of 220,438 points in 1,327 cells over a 14 m room**, which fits 9
walls / 15.0 m and closes **no** room. Widening the band to 0.7–1.8 m gives 40
walls and 124.8 m, which is not more geometry — it is RANSAC fitting lines down
the diagonal traces of the fan through furniture.

#### 3.3 The floor map: one test turns a grey blob into a room

Projecting every return between 0.2 m and 2.4 m straight down is a solid blob:
a hand-carried D6 walked through a flat paints the floor, the furniture and the
coving from a hundred viewpoints, and on scan-033 that is 9 × 9 m of continuous
grey with no room in it.

What separates a WALL from everything else in a downward projection is not how
many returns land in a cell — it is **how tall the column of returns is**. A
wall is hit from skirting to coving as the fan sweeps past; a floor tile, a
tabletop, a sofa back and a ceiling patch each span a few centimetres. So a
5 cm cell counts as structure only when its returns span **≥ 0.60 m**
vertically. That single test produces a recognisable outline of the flat.

A12's `extract_walls()` takes a grid the caller already built — the seam the
desktop editor's slice slider uses — so the same RANSAC then runs on the floor
map when the thin slice fails to close anything. The plan slice is still tried
FIRST, because when it works its geometry is the more literal answer (a plan is
conventionally cut at 1.2 m, and face pairing measures a real thickness there),
and the report says which grid won, because a projection has no faces to pair
and its thicknesses are therefore assumed.

#### 3.4 What scan-033 actually produced

    cloud 220,438 points from streams/map.bin
    slice 1.00-1.50 m (up y), 21,143 in band, 1,327 cells @ 0.02 m
    floor map: 99,373 points spanning >=0.60 m in a 0.05 m cell -> 679 cells
    MODE walls — walls fitted, but no outline closed into a room
    walls 10 (1 with MEASURED thickness) from the FLOOR MAP
    openings 14 (3 door candidates), rooms 0
    wall length 24.88 m, extent 14.65 x 9.80 m
    PNG 1600x1195 @ 122.0 px/m, scale bar 2 m

The picture is three sides of the flat plus an interior partition, traced over
the returns they were fitted to. Across all seven of the owner's captures the
plan produces walls; **only scan-029 closes a room, and it is 1.60 m²**.

**Honest one-liner: this is a good, scaled floor MAP and a weak floor PLAN.**
The outlines are real, metric and shareable; the room polygons are not there
yet, and the reason is coverage rather than arithmetic.

| capture | band pts | map cells | walls | wall length | rooms | source |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| scan-020 (crawl) | 68,719 | 421 | 28 (22 paired) | 79.6 m | 0 | plan slice |
| scan-026 | 10,210 | 1,195 | 15 | 53.1 m | 0 | floor map |
| scan-028 | 19,893 | 666 | 10 | 23.2 m | 0 | floor map |
| scan-029 | 33,262 | 1,579 | 15 | 28.9 m | **1** (1.60 m²) | plan slice |
| scan-030 | 15,336 | 1,977 | 18 | 75.8 m | 0 | floor map |
| scan-033 | 21,143 | 679 | 10 | 24.9 m | 0 | floor map |
| scan-034 | 34,013 | 856 | 12 | 29.1 m | 0 | floor map |
| scan-035 | 27,934 | 626 | 10 | 27.2 m | 0 | plan slice |

scan-020 is the outlier and it is the 5 cm/s crawl: 22 of its 28 walls have a
**measured** thickness, because a crawl is the only speed at which this sensor
paints both faces of a partition. That is the same capture ROUND 12 found was
carrying every crispness claim this project had ever made.

#### 3.5 A PNG writer, hand-rolled, because a plan you cannot look at is not a plan

DXF opens in nothing on Android and PDF opens in whatever the operator happens
to have installed. A preview is the whole point of a plan on a phone: you
glance at it, decide whether the room closed, and either share it or walk the
missing wall again.

`plan/plan_raster.cpp` writes a PNG with **no zlib**, because this repository
has none on any leg (its only image dependency is the vendored `stb_image`,
which decodes). A PNG's deflate stream may be a chain of **STORED** blocks —
legal per RFC 1951 §3.2.4, universally readable, and with the property this
project values above size: bit-identical run to run, with no compression level,
no dictionary and no library version in the loop. A 1600 px plan lands around
5 MB, which is a share-sheet attachment. `test_round15_plan.cpp` reads the
bytes back with an independent decoder written in the test — signature, IHDR,
every chunk CRC recomputed, the zlib FCHECK and Adler-32, the stored blocks
walked, LEN/NLEN checked, scanlines compared pixel for pixel — and exercises
the multi-block path with an image whose raw stream crosses 65,535 bytes.

Also in the renderer, each because the first version was wrong in a way the
owner's own data exposed:

* **The extent is trimmed to 99 % of occupied cells.** A handful of returns
  through a doorway put the next room in the grid and rendered the room the
  operator walked at a third of the page. Walls are never trimmed — the model
  bounds are unioned back in.
* **The wall stroke is capped at 6 px.** A12 allows a 40 cm wall, which at
  phone scale is a 40-pixel black bar that swallows the drawing it is part of.
  The measured thickness is in the DXF and the PDF, where it can be
  dimensioned.
* **The caption says `NO ROOM CLOSED`** rather than "0 rooms 0.0 m²", and
  `SLICE DENSITY - NO WALLS FITTED` in density mode. A scaled picture of real
  returns is a measurement; an empty sheet labelled "floor plan" is a lie.

#### 3.6 Android

`Routes.PLAN` gains a **Floor plan / Redraw** button and PNG / PDF / DXF share
buttons **above** the options fold — eleven rounds of "Floor plan" had produced
a screen whose only actions were behind an Options button and whose empty state
told the operator to go and run something else first. The preview is the
engine's PNG, decoded with `inSampleSize = 2` and pinch-zoomable.

Sharing goes through `DownloadsExporter` + `ShareTargets`, like every other
export. The old plan export called `ShareTargets.shareFile` on a file inside
the app's private storage only — the exact "the file went nowhere" failure
ROUND 7 fixed everywhere else. `ShareTargets.mimeFor` also learned `png`, which
it did not know: without it the share sheet offered the preview as
`application/octet-stream` and every image-capable target vanished from it.

### 4. ITEM 57 — THE RULER ON THE CARD

`measure_map_consistency` has existed since ROUND 12 and only `--d6-selfcheck`
could reach it, so the one honest accuracy number this project owns had never
been in front of the owner. It is now computed inside
`reprocess_d6_container()` — free, because the cloud and its point times are
already in hand — on the **stitched** cloud, because that is the cloud the
operator is about to look at.

It reaches the phone as six **appended** `double[]` slots (16–21) on
`nativeProcReprocessD6`, and `StitchResult.fromNative` still accepts a 16-long
array: a native library that was not rebuilt reports no self-check rather than
reading a slot that is not there. The instrumented test asserts the array is 22
long, so "the .so is stale" is a visible failure and not a silently missing
feature.

The sentence, in `:core` so both the capture card and Review's process card
read the same one:

> **Surfaces repeat within 2.0 cm** (measured over 8 s; this measurement's own
> floor is 1.0 cm).

and, when nothing was covered twice:

> **Repeat accuracy: not measurable — nothing in this scan was covered twice.
> Walk past the same wall again and it can be measured.**

That branch is the point of the item. A single pass down a corridor paints
nothing twice, and a card that printed "0.0 cm" for it would be claiming a
perfect map on the strength of no evidence. The `:core` test asserts the
number does not appear in that sentence at all, and — ROUND 13's regression bar
— that no `%` survives into anything the operator reads.

On the owner's fixtures, measured on this tree (`--d6-selfcheck`, 8 s window,
25 cm cells) — every one of them measurable, and the floor is quoted beside
every reading because the reading means nothing without it:

| capture | surfaces repeat within | the measurement's own floor |
| --- | ---: | ---: |
| scan-020 (5 cm/s crawl) | **0.70 cm** | 0.42 cm |
| scan-033 (26.6 m walk) | **1.97 cm** | 0.99 cm |
| scan-035 (sweep) | **1.74 cm** | 1.40 cm |
| scan-034 (sweep) | **2.45 cm** | 1.03 cm |
| scan-028 | **4.45 cm** | 1.08 cm |
| scan-026 (contaminated mount) | **5.26 cm** | 2.17 cm |
| scan-030 (5 sections) | **5.85 cm** | 1.23 cm |
| scan-029 | **6.00 cm** | 1.26 cm |

Unchanged from ROUND 12/14 to the second decimal on every capture those rounds
measured, which is the point: item 57 moved a number onto a card, it did not
move the number.

### 5. THE ABI, 10 → 11, ADDITIVE

`scan_reprocess_options` and `scan_reprocess_result` are **byte-identical** to
ABI 10. That is why the ruler arrives on a NEW entry point
(`scan_lscan_reprocess_d6_ex`, taking a second out-struct) rather than as two
more fields on the old one: appending to a POD the CALLER allocates is not an
additive change, it is a buffer overrun waiting for a mismatched build. Adding
a function is.

Five new entry points, three new PODs:

* `scan_engine_heal_live_frame` / `scan_engine_clear_live_correction` /
  `scan_engine_live_heal_stats` + `scan_live_heal_stats`
* `scan_lscan_reprocess_d6_ex` + `scan_selfcheck_result`
* `scan_lscan_floor_plan` + `scan_plan_options` / `scan_plan_result`

An ABI-10 consumer relinks unmodified and behaves identically: the live
correction starts at identity and stays there unless asked.

### 6. FILES

Engine:
* `include/scanengine/plan/plan_raster.h`, `src/plan/plan_raster.cpp` (new) —
  the PNG writer, the rasterizer, the 5×7 font, the trimmed extent
* `include/scanengine/slam/post/lscan_plan.h`,
  `src/slam/post/lscan_plan.cpp` (new) — container → plan, the density ladder,
  the wall-likeness floor map
* `include/scanengine/slam/post/reprocess.h`, `src/slam/post/reprocess.cpp` —
  `measure_self_consistency`, `ReprocessReport::consistency`, six new
  `stitch.json` fields
* `include/scanengine/core/engine.h`, `src/core/engine.cpp` — ABI 11,
  `heal_live_frame`, `clear_live_correction`, `live_heal_stats`, `live_pose_`,
  the reset in `start_session`
* `capi/scanengine_c.h`, `capi/scanengine_c.cpp` — ABI 11, five symbols
* `tools/engine_cli.cpp` — `--d6-plan`
* `tests/test_round15_live_heal.cpp` (4 cases), `tests/test_round15_plan.cpp`
  (3 cases)

Android:
* `core/capture/PoseSections.kt` — `SectionBreakBracket`, `addBracketed`
* `core/capture/StitchResult.kt` — `SelfCheck`, `selfCheckLine`, 22-slot decode
* `core/capture/FloorPlanResult.kt` (new)
* `app/ar/CaptureArController.kt` — `healLiveFrame`, `resetSections`,
  `onSectionBreak(break, healed)`
* `app/engine/ScanEngineNative.kt` — `nativeHealLiveFrame`,
  `nativeLiveHealStats`, `nativeProcFloorPlan`, `nativeProcPlanFilePaths`
* `app/src/main/cpp/arcore_jni.cpp`, `processing_jni.cpp` — the bindings;
  `processing_engine.cpp` — **the `UpAxis::kZ` fix**
* `app/processing/ProcessingRepository.kt` — `floorPlan()`
* `app/ui/capture/CaptureViewModel.kt` — `AutoProcessState`, `startAutoProcess`,
  `unhealedSectionBreaks`, `runAutoProcess`
* `app/ui/capture/CaptureScreen.kt` — `AutoProcessPanel`
* `app/ui/plan/PlanViewModel.kt`, `PlanScreen.kt` — `render()`,
  `shareRendered()`, `RenderedPlanView`
* `app/ui/review/ReviewScreen.kt` — the self-check line
* `app/share/ShareTargets.kt` — `png`/`jpeg` MIME types
* tests: `core/.../Round15Test.kt`,
  `app/src/test/.../CaptureRound15AutoProcessTest.kt`,
  `app/src/androidTest/.../Round15PlanAndRulerTest.kt`

### 7. BACKLOG

* **The room polygons.** Walls come out of every one of the owner's captures
  and one room out of one capture. The gap is coverage, not arithmetic: A12's
  room detector needs a closed cycle in the wall graph, and a hand-carried D6
  leaves gaps wherever the operator did not point the fan. The two candidates
  are (a) close cycles across gaps the trajectory says the operator walked
  through, and (b) an explicit "you have not covered this wall" live cue driven
  off the floor-map grid during the capture rather than after it.
* **Thickness from a projection is assumed, not measured.** The floor-map path
  has no faces to pair. Only scan-020's crawl paints both faces of a partition;
  everything else reports A12's default 0.10 m. Reported honestly, not solved.
* **`walls_from_floor_map` changes what the DXF means** and the DXF does not
  say so. A layer note or a title-block line belongs there.
* **Live healing is validated on synthetic re-anchors and on the emulator**,
  not on a real ARCore relocalization. It wants one walk on the owner's Pixel
  in a room that reliably re-anchors (the small flat does — scan-030 broke four
  times) with the log's `live heal #N` lines checked against the manifest's
  section breaks.
* **The plan slice band is still a project setting the app never surfaces.**
  `planSliceMinM/MaxM` are in `project.json` and only the Plan screen's Options
  fold can move them.
* Everything on ROUND 14's backlog that is not listed above is unchanged: the
  Mid-360 ABI-10 rewire (needs hardware), the stale ABI comments,
  `restoreOrphaned()`, driver-start propagation, the NO-DATA line's
  `DeviceHealth`, and the gyro-locked solver at the loop end.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

---

## ROUND 16 — THE FIX FOR THE ORIGIN LEAK CREATED A POSE LEAK, AND THE LOOP GAP WAS NEVER THE TARGET

Owner's 0.9.0 field session (log `2251`, captures 036/037/038/039). Two of the
four scans are the best this project has produced: scan-036 healed a 0.378 m /
8.02° re-anchor live and auto-processed to 3.43 cm of self-consistency;
scan-038 healed two, one of them **1.39 m / 26.64°**, and came out at 2.73 cm.
DND held on all four. Then scan-039 recorded 51 seconds and 184,454 points with
**no poses at all**, and the app graded it **FAIR**.

His verdict on the round as a whole was *"scan look ok but not much improved"*,
and he asked for two things: the walked path drawn inside the cloud so he can
check a scan by eye, and the popup corner radii fixed.

### 1. THE POSE-LOSS REGRESSION (item 58) — round 14 put `close()` on the hot path

**The log had already diagnosed it and the app started recording anyway.** Five
world-frame resets in that session, two of which produced a session that never
delivered a camera frame:

```
22:47:10.106 [ar] world frame reset: new ARCore session for this capture
22:47:14.129 [ar] start gate: timed out — stableMs=0 ready=false blocker=NO_POSES
22:47:14.200 [session] start: project=scan-039-… dnd=protected
22:47:14.713 [ar] cue: tracking_degraded          ← x13, for the next 51 seconds
22:48:06.064 [seal] summary: grade=FAIR points=184454 pathM=0.0 sections=1 drops=0
```

`resetWorldFrame()` returned **true**: the session was rebuilt and `resume()`
succeeded. `publishPose` returns at its first line (`if (timestamp <= 0L)`) on a
frame with no camera image behind it, so nothing reached `motion`, the section
tracker or `nativePushPose`. The container is exactly what that predicts —
`lidar.bin` and 899 KB of 400 Hz `imu_phone.bin` present, `poses_ar.bin` and
`map.bin` absent, `sectionBreaks` empty, `pathM=0.0`.

**`ArSessionGate.mayDrive` is a check, and a check is not a lock.** `onFrame`
reads it and then proceeds into `Session.update()`; nothing prevented the main
thread from calling `Session.close()` after that check passed. Concurrent
`close()`/`update()` is undefined by ARCore's contract and here it leaves the
camera unbound to the replacement session.

Round 6 did not need a lock and said why: the only lifecycle call on the hot
path was `pause()`, and `close()` had *"zero callers anywhere in `src/main`"*.
**Round 14 gave it a caller on every Start.** The fix for "the origin never
zeroed" created this, and it is intermittent because it is a race — two of five
resets lost it (scan-037 and scan-039; 036 and 038 won).

Three layers:

* **The race.** A `ReentrantLock` on `CaptureArController`. `onFrame` takes it
  with `tryLock` and yields the frame if it cannot have it — a render thread
  must never block on the main thread. `resetWorldFrame` holds it across
  close/create/re-bind/resume; `pause()` and `close()` take it reentrantly.
  Yielded frames are counted and logged (`framesYielded=`) so the mechanism is
  visible in a field log. **The race is not reproduced** — ARCore is stubbed on
  the emulator, so `update()` and `close()` cannot be made to overlap here. What
  ships is the mutual exclusion the concurrency requires plus two deterministic
  hardenings: the rebuild retries once, and a start-gate timeout whose blocker
  is `NO_POSES` (distinct from `NOT_TRACKING`/`IMPOSSIBLE_STEP`, which mean the
  tracker is alive and unsettled) triggers one more rebuild and one more wait
  before the recorder is armed. It still never refuses to start.
* **The pose watchdog.** Fires when points are arriving AND no pose has been
  accepted for 3 s — the scan-039 signature — and never when nothing is arriving
  at all, because that is the no-data banner's diagnosis with a different
  instruction. Three seconds, not two: ARCore goes quiet through a hard turn.
  New counters (`acceptedPoseCount`, `lastAcceptedPoseAtMillis`) because nothing
  measured "is the tracker alive right now": `posesPushed` counts only poses
  that reached an engine handle and is zero before recording starts.
  **No new buzz**, and that is a decision: `TRACKING_DEGRADED` already fires on
  this exact condition (thirteen times in scan-039's log). The buzz was never
  the missing half — the operator felt it and could not know it meant "this scan
  has no room in it". A second cue would spend the round-13 cue budget twice.
* **The honest seal.** `ScanSummary.posesRecorded` (nullable) +
  `isTwoDimensionalOnly`, checked FIRST in the grade. scan-039's numbers graded
  FAIR because 51 s over a 0 m path made it "from the spot", 3,578 pts/s beat
  every floor, and only a 1.68° trim kept it off GOOD. It is now POOR, the card
  reads **2D ONLY — NO ROOM**, and auto-process refuses with a reason rather
  than attempting a run that must fail. The field is **nullable on purpose**: a
  rig with no AR controller has not measured this, and "not measured" is not
  "measured zero" — defaulting it to 0 broke four round-15 tests within a minute
  of being written, which is the fastest that mistake has ever been caught here.

### 2. LOOP-END CLOSURE (item 60) — and the loop gap is not the target

Round 11's closer refuses all three good captures, for two reasons that are not
disagreements about geometry: `min_excursion_m = 4.0` is a corridor's number and
the owner scans a flat (036 reaches 3.58 m from start, 038 3.68 m), and six-DoF
ICP wanders on a pushbroom (it proposed 5.72° on scan-033 against a gyro that
tracks ARCore to r = 0.9994).

`post/loop_end.h`: the excursion gate becomes scale-aware (`>= 3.0 m` **and**
`>= 4x the closing gap`, which still refuses scan-035's sweep on the absolute
floor), and the closing transform is **constrained** to a pure translation — the
rotation half of the se(3) vector is structurally zero, so `Exp(s·xi)` cannot
rotate anything at any `s`. Gyro-locked is a property of the type; the test
asserts `correction_rotation_deg == 0.0` exactly.

**Two real findings came out of building it.**

`plane_radius_m` had to move from round 13's 0.25 m to 0.60 m, and the reason is
geometry rather than taste. At a seam the analytic transform has removed the
jump and centimetres remain; at a loop end the whole accumulated drift remains.
With a 25 cm radius every surface whose normal points ALONG the drift is
displaced out of correspondence range and contributes nothing, while surfaces
perpendicular to it — carrying no information about it — match perfectly. The
system matrix is then singular in exactly the direction the answer lies:
measured on the fixture, the weakest direction came back as (1.000, −0.001,
−0.002), the axis holding 0.30 m of the 0.36 m injected drift, at observability
0.022. At 0.60 m it is 0.265.

And a **seventh gate** had to be added, which changed an answer. On scan-036
every solver-side gate passed — the two ends came 4.8 cm together, observability
0.358, correction 0.336 m, end gap down 0.30 m — and round 12's ruler went
**3.43 → 4.52 cm**. A translation that slides a cloud onto SOME nearby surface
always reduces the mean nearest-neighbour distance between the two clouds it was
fitted to; that number is the solver's own residual wearing a different hat. The
ruler now votes last, on the metric the card prints.

Measured through the production path (`--d6-loopend`, both legs stitched):

| scan | decision | correction | self-check | loop gap | occupancy |
|---|---|---|---|---|---|
| 033 | **closed** | 0.118 m / 0.000° | **1.97 → 1.66 cm** | 0.581 → 0.566 m | −1.01 % |
| 036 | ruler-says-worse | 0.336 m | would be 3.43 → 4.52 | refused | — |
| 038 | correction-too-big | 1.389 m vs 1.00 bound | refused | — | — |
| 034 | no-revisit | 5.4 m path < 8 m | — | — | — |
| 035 | no-excursion | 10.1 m inside 1.55 m | — | — | — |
| 039 | no-trajectory | no poses (item 58) | — | — | — |

**One in six. This claims the lever, it does not claim the room.** And the loop
GAP must not be sold as the win: on scan-033 the geometry says the walk genuinely
ended 0.57 m from where it started, so most of that gap is where the operator
stopped, not drift. What the closure removes is the 15 % the ruler measures.

`post_geom.h` extracted on the way (round 11 wrote five primitives, round 13
copied four, round 16 needed all of them). Pure move — the round-11 and round-13
fixtures assert the same numbers to the same decimals afterwards.

### 3. THE PATH IN THE CLOUD (item 59)

`TrajectoryTrail.snapshot()` has existed since round 5 with **no callers at
all**, and the recorder was throwing the height away at the door (`pose.tx()`,
`pose.tz()`, nothing else) because the trail only ever fed a 108 dp bird's-eye
tile. Adding `y` and an accessor was most of the live half.

* **Live:** a new `trail.mat` (picked up by the existing `compileMaterials` glob
  — no build change) draws a `LINE_STRIP` in the same Filament scene, depth
  tested and depth writing, so the path is occluded by the wall you walked
  behind. Rebuilt on change, not per frame. Round 5.3 refused this and priced it
  at *"a second material, a second geometry upload path and a per-frame rebuild
  of a line strip"* — two of the three were right.
* **Review:** from `processed/trajectory.bin`, a new derived product written by
  `reprocess_d6_container` beside `processed/map_stitched.bin`. A file rather
  than an ABI call because the trajectory in it is the CORRECTED one, written by
  the same pass as the cloud beside it; a path from an uncorrected trajectory
  over a corrected cloud would be a lie shaped exactly like a diagnosis.
* **The floor plan:** `Canvas::line`/`disc` already existed. On scan-033 the
  sheet now shows the loop through the flat with the start and end discs a
  visible gap apart — the loop-end gap, at true scale, on the drawing.

Colouring is `:core`'s so all three draw by one rule: teal start, ember end,
brighter markers at both ends, **red where tracking was lost** — which outranks
the markers, because a walk that lost tracking in its first half-metre must not
have its one warning colour hidden under a start marker.

### 4. THE RADIUS, AND WHAT WAS ACTUALLY DUPLICATED (item 61)

One line causes it: `LidarScanShapes.extraLarge` is
`RoundedCornerShape(percent = 50)` — deliberately a pill so un-restyled buttons
and chips round like the hand-built ones — and Material 3 hands that same token
to `ModalBottomSheet` and `AlertDialog`. Three of five sheets already passed
20 dp by hand and two did not, which is exactly why *some* windows looked right.
`ScanDims.SheetRadius`/`DialogRadius` now cover the four that were inheriting the
pill, including the session-summary sheet the merge progress lives in.

Merged: the three `StitchResult` sentences that were laid out twice (Capture's
auto panel and Review's process card) into one `ProcessResultLines`; and the
path toggle onto the existing `DisplayParams.showTrajectory` rather than a new
Boolean. Named and left, with reasons, in `REVIEW_FEEDBACK.md`: three "process"
surfaces over two different pipelines, Review reachable by two routes with
different chrome, and two display panels with divergent ranges (point size
0.1–3.0 px vs 0.5–12 px; LOD 2–20 M vs 0.5–50 M).

### 5. SECTIONS (item 62)

Neither count was wrong — the live detector splits on every discontinuity as it
arrives, the offline one bridges scan-038's 1.6 s `TRACKING_REGAINED` gap. The
card shows whichever detector last spoke (post-process once it exists); the log
carries both as `sectionsLive=` and `sectionsProcessed=`.

### TESTS AND VERSION

Engine **608 cases / 2,513,954 assertions**, ctest serial. `:core` **554**,
`:app` **94**, emulator **21/21** with the native library rebuilt from this
round's engine sources. ABI stays **11** — nothing this round changed the C ABI;
the trajectory crosses as a file and the loop-end closer is inside
`reprocess_d6_container`. VERSION **0.9.1**.

### BACKLOG

* **The pose race is hardened, not reproduced.** It wants one Pixel session of
  repeated Start/Stop with the new `framesYielded=` line in the log; a non-zero
  count proves the lock is arbitrating something real.
* **Loop-end closure fires on one capture in six.** scan-038's 1.39 m proposal
  is the interesting refusal — that is a genuine revisit whose solve ran away,
  and the submap window or the candidate choice is the next thing to look at.
* **scan-039 is not rescuable today and is not unrescuable.** It holds ranges
  and 400 Hz IMU but no poses. A rescue needs translation, which gyro cannot
  give and double-integrated accelerometer bias cannot either over 51 s; it
  would have to come from lidar-to-lidar registration constrained by the gyro —
  a real project, not a flag. The raw streams are untouched, so nothing about it
  is foreclosed.
* Unchanged from round 15: room polygons, floor-map thickness assumed, live
  healing wants one real-Pixel re-anchor walk, `planSlice` only in the Options
  fold, the Mid-360 ABI rewire (needs hardware), `restoreOrphaned()`.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

---

## ROUND 17 — THE HEAL WAS RIGHT FOR 33 ms AND WRONG FOR SIX SECONDS; START WAS ONE PRESS TOO MANY; AND THE PATH WAS BUILT AND NEVER PUBLISHED

Owner's 0.9.1 field session, `lidarscan-capture-log-2026-08-19-0038.txt`:

> *"the scan is not good. the shift of my position shifted quite a lot. my path
> not show in the point cloud. its just a 2d map of my path."*

Three sentences, three separate bugs. `docs/design/REVIEW_FEEDBACK.md` items
**63–67** carry the derivations; this is what changed and what was measured.

### 63 — THE SIX BLIND SECONDS (the headline)

`scan-040`, one line of its seal log: `HEALED live jump=0.678m/66.21deg
**gapMs=6065**`.

Round 13 wrote `T_k = pose_after · pose_before⁻¹` and wrote its own assumption
beside it — *the operator's own motion during the **33 ms** gap is inside T_k
too, but the gyro bounds it at ~1 deg*. Every break it measured was one ARCore
frame wide. Round 15 then applied that transform live, to whatever bracket the
detector handed over, and scan-040's bracket was **6.065 seconds** wide.

Measured on his bytes, against his own 399.2 Hz gyro:

| | |
|---|---|
| ARCore, last tracked → first re-acquired | 66.21° |
| gyro, integrated over the same 6.065 s | **144.94°** raw / **142.75°** bias-corrected |
| ARCore's own motion during the loss (181 poses) | 0.00° / 0.000 m |

ARCore **froze** — 181 consecutive poses identical to the last good one, which
a phone in a walking hand does not do. The 66.21° is the leftover of a 145°
turn the tracker only saw the end of, and applying it rotated his whole map.
That is "the shift of my position shifted quite a lot", in our arithmetic.

The gyro is good over exactly this span. Re-measured against ARCore over every
clean 1 s window of all three captures: median **0.109° / 0.152° / 0.465°**, p90
**0.363° / 0.494° / 0.711°** (040/041/042) — round 14's 0.221° p90, confirmed on
three more captures.

**`engine/include/scanengine/poses/reanchor.h`** — predict, then heal only the
residual. `q_pred = q_before · q_gyro`; translation is **bounded, not
predicted** (`1.8 m/s × gap + 0.30 m`, and only the excess beyond it is charged
to the frame — on scan-040 the bound is 11.2 m against 0.678 m claimed, so the
translation correction is exactly zero). Gated:

| gap | verdict |
|---|---|
| ≤ 100 ms | `snap` — round 13's transform, bit-identical |
| bridged, residual ≥ 1 cm or 0.25° | `bridged` |
| bridged, residual below that | `negligible` — the jump was the operator; **no cue** |
| no continuous gyro / > 8 s / residual > 25° or > 2 m | refused |

**scan-040 refuses at a 76.77° residual** (78.91° if the gyro bias is left in —
the engine unit test pins that one, the production path prints the corrected
one; the verdict is the same either way). Every re-anchor round 13 measured was
8–14°; ARCore correcting itself by 79° against its own session map is not a
thing that happens, nothing in the container can say what did, and a refusal
with a recorded seam is the honest answer.

One policy, two callers — `core/engine.cpp` (live, gyro from the densifier's
8 s ring) and `slam/post/section_stitch.cpp` (offline, over a kept copy of the
whole stream). Item 62's lesson about two detectors applies twice as hard to two
decisions.

**The offline detector was blind too, and this is new.** It derived seams from
RATES, and 0.678 m over 6.065 s is 0.11 m/s — under every threshold. So Process
found **one section** in a capture with a 66° fold in it. A run of poses the
tracker disowned between two it owned is now a candidate on its own. And the
**ruler votes last**, exactly like item 60's seventh gate: scan-041's 468 ms gap
proposes a defensible 14.18° correction that makes the map worse (self-check
2.86 → 3.39 cm, flat-floor wander 0.50 → 0.79 m), and the surfaces refuse it.

Through `engine_cli --d6-reprocess`, before → after:

| scan | sections | self-check | extents | outcome |
|---|---|---|---|---|
| 040 | 1 → 1 | 2.64 → **2.64 cm** | 0.134 m vert, 4.182 m end gap — unchanged | gap **named**, refused |
| 041 | 2 → 2 | 2.86 → **2.86 cm** | 0.496→0.811 m — unchanged | bridge refused by the ruler (25.29 vs 24.48 cm) |
| 042 | 1 → 1 | 3.54 → **3.54 cm** | unchanged | one negligible (5.72° gyro vs 5.70° tracker), one thin-submap |

**Every offline number is identical to round 16's, deliberately.** Offline
gained a diagnosis, not a change — a correction nobody can check is the thing
item 63 exists to stop applying. What the container says now that it never said:
`longest blind stretch 6.065 s; 1 gap(s) refused`. The LIVE pass is where the
behaviour changed, and it is the change he asked for.

New engine surface: `ImuDensifiedPoseSource::relative_rotation()` (a public door
onto the existing integrator, with no `max_bracket_ns` ceiling — that ceiling is
about distributing a closing error, and a prediction has no far end to close
against), `reanchor::GyroBridge`, `SectionStitchReport::gaps_examined`, and
`engine_cli --d6-stitch` now prints every gap it looked at.

**ABI 11 → 12, additive.** `scan_engine_last_reanchor()` +
`scan_gap_verdict_str()` + `scan_reanchor_info` + `scan_gap_verdict`. Nothing
existing changed size, order or meaning; an ABI-11 consumer relinks unmodified
and gets the fix. The new symbols only let it SAY why —
`heal_live_frame` returns a `Status`, and a `Status` cannot carry "the gyro says
he turned 145 degrees while the tracker was frozen and I am not rotating your
room on that evidence".

### 64 — TWO STARTS, ONE PROJECT (scan-045)

`startCapture()` had **no re-entry guard at all** — no `captureState` check, no
disabled button — and the round-12 tracking gate holds a press for four to
eight seconds during which nothing on screen changed, because `_startWarmup`
was computed and rendered **nowhere** (8 hits, all inside `CaptureViewModel`).
A button that does not respond gets pressed again.

The second press fell through both `if (!startPending)` blocks and started the
capture, orphaning the first press's `startGateJob` (cancelled only when a NEW
gate opens). Four seconds later the orphan re-entered `startCapture()`
mid-recording and ran `resetPoseCounters()`, `resetWorldFrame()` — rebuilding
the live ARCore session mid-walk — and `trailRecorder.clear()` **before** the
engine refused it with `invalid state`.

`trailRecorder.clear()` is `pathM=0.0`, and that did not merely lose a number:
round 14's `isFromTheSpot` reads "≥ 20 s and < 5 m" as a deliberate sweep and
grades on points per SECOND, which 55,228 returns over 28 s passes easily. One
section, no drops, 225 poses (so round 16's zero-pose check could not fire) →
**GOOD SCAN**, on a bundle with no `map.bin` and no `processed/`.

Fixed in three places: an `AtomicBoolean` claimed by the first press and held
across the gate wait; a `starting` flow the transport button renders (dimmed,
inert, spinner, "Starting — waiting for tracking"); and two new `ScanSummary`
fields ranked **above** everything else — `engineStarted` (what
`scan_engine_start` answered) and `worldPointsResolved` (read off
`streams/map.bin` at 16 B per `PointVertex` — **the file, not a counter**,
because a counter is exactly what got reset). Headlines "NOT RECORDED" and "NO
ROOM — NOTHING WAS PLACED"; auto-process now names which of the three reasons it
refused for.

Both fields nullable, and **the `?:` that is not there is the point** — round
16's rule, and it caught the same mistake within the hour: four round-15 unit
tests run with no AR controller and therefore no pushbroom writing `map.bin`,
and reading its absence as zero made every one of them "NO ROOM".

### 65 — THE PATH WAS BUILT AND NEVER PUBLISHED

Round 16 built all of it: the metric flow, its accessor, `TrajectoryRibbon`,
`trail.mat`, a `LINE_STRIP` drawn in the **same 3D pass** as the points with the
same depth state, `TrajectoryFile`, `ReviewViewModel.loadTrajectory`. And
published `_worldPoints` from `setCapacity()` and `clear()` **and nowhere else**.
`setCapacity()` runs when the performance preset changes, so the live ribbon
held whatever the walk looked like when a preset was last touched — the empty
list, on every real capture. The bird's-eye tile, published two lines away in
the same method, kept updating.

*"its just a 2d map of my path"* is a precise bug report. One assignment.

It had no test because everything it lived in took a `com.google.ar.core.Frame`,
so `onPose(x, y, z, tracking)` is split out of `onFrame` and five bare-JVM tests
walk it. Two things beside it:

* **Review said nothing when there was no path** — `EMPTY` removed the entity
  from the scene in silence, which is indistinguishable from a broken renderer,
  and **every container processed by a pre-round-16 engine is in that state**
  (which is every scan the owner already owns). There is now a line under the
  toggle saying which it is.
* **The device path had never been asserted** — the round-16 commit touched zero
  files under `src/androidTest/`. `Round17TrajectoryOnDeviceTest` drives the real
  `libscanengine_jni.so` over staged scan-030 bytes and checks the engine writes
  the file, that its length is exactly `16 + 12 × poses`, that the phone's own
  independent decoder yields ≥ 2 finite vertices with real extent, and that a
  second Process does not corrupt it.

### 66 — A DEBUG LOG THAT TRAVELS WITH THE SCAN

`<proj>.lscan/debug/capture-debug.log`, plus a `README.txt` beside it. Carries
every `[ar]`/`[session]`/`[seal]`/`[net]` line **plus** capture-only verbose
events — re-anchor decisions with item 63's six numbers via the new ABI-12 call,
pose acceptance, watchdog transitions, cues, preset changes.

Opened **before** the engine start, so a capture whose engine refuses still
leaves a bundle that says why (the scan-045 case). Closed on both seal arms,
before the empty-scan prune that may delete the directory it lives in. 5 MB × 2
files.

**Explicitly not a stream, and the README says so in the bundle**: not chunked,
not CRC'd, not in the manifest, not part of the replay guarantee.
`record/replay` walks `streams/`, so byte-identical replay is untouched — which
is what lets the log be as verbose as it likes.

Developer Mode = seven taps on the version footer (which already had a stable
test tag and an unused `clickable` import).

### 67 — CAMERA HONESTY, AND ONE REAL LEAK

Audited every path that can touch an ARCore image. The GL background renderer is
GPU-only (no `glReadPixels`/`PixelCopy`/`MediaCodec` anywhere in `app/` or
`core/`); the calibration wizard takes the **luma plane only** into a heap array
and closes the image immediately; no engine code writes an image file; **no
chunk type can carry image bytes** (`kCameraFrameIndex` is path + pose +
intrinsics + timestamp).

**One real leak.** `CaptureViewModel` gated the UI flow with
`FeatureFlags.COLORIZE_ENABLED` and then called `setEnabled(...)` on the line
below **without** it, at two sites. Three presets set `keyframesEnabled = true`
and the preset picker is reachable **during a recording**, so changing preset
mid-walk re-armed the recorder and wrote `streams/frames/kf_*.jpg` while the
HUD, reading the correctly-gated flow, reported keyframes as off. Both sites now
pass the gated flow — and `KeyframeRecorder` defaults to `false` and enforces
the flag **at the source**, so with colorization off no argument to
`setEnabled()` turns the camera writer on. That makes the class of bug
impossible rather than fixed twice. `recorder.start()` is likewise no longer
called, so no empty `streams/frames/` appears in a bundle.

The sentence, now on the capture sheet and in a Settings section of its own:
**"Camera is used for position tracking only — no images are saved."**

### TESTS AND VERSION

Engine **616 cases / 2,515,610 assertions**, ctest **7/7** serial. `:core`
**560**, `:app` **99**, emulator **22/22** on `b4_test` with the native library
rebuilt from this round's engine sources (verified: the round-17 refusal string
is present in both the arm64-v8a and x86_64 `libscanengine_jni.so` the test APK
shipped). ABI **11 → 12**, additive only. Replay bit-identical: all three owner
captures reprocess to the same self-check to two decimals (2.64 / 2.86 / 3.54
cm) and the round-11/12/13/15/16 fixtures are unmoved. VERSION **0.9.2**.

### BACKLOG

* **scan-040's 76.77° is refused, not explained.** Something made ARCore report
  a pose 77° away from where the gyro says the operator was, and nothing in the
  container says what. The next real-Pixel session should reproduce a long
  tracking loss deliberately (cover the lens for five seconds mid-walk) and see
  whether the residual is repeatable — if it is, the tracker restarted its frame
  and there is a bigger correction available; if it is not, refusing is the
  permanent answer.
* **The refused gap is a hole in the map that nobody has closed.** scan-040's
  two halves are 66° apart and the offline pass now says so instead of
  pretending otherwise. Closing it needs lidar-to-lidar registration across the
  seam, gyro-constrained — the same project scan-039's rescue needs, and the
  same one item 60's null space keeps pointing at.
* **`CaptureViewModel.displayParams` synthesizes rather than loads.** It builds
  a fresh `DisplayParams` from five live controls, so `showTrajectory` is a
  constant `true` in the live view and Review's toggle never reaches it —
  contradicting its own KDoc — and every field outside the five-way `combine`
  (`showPoseGraph`, `edlEnabled`, the clip fields, `fixQualityColors`) is reset
  to the data-class default when a project is created. Benign today because the
  defaults are what everyone wants; a lossy round trip either way. This is item
  61's "two display panels" backlog entry wearing a different hat.
* **`syncTrail` consumes `trailDirty` before its null guard** on
  `trailMaterialInstance` (`PointCloudRenderer.kt`). Cannot fire today because
  the material loads before the first frame callback; the flag is on the wrong
  side of the guard regardless.
* Unchanged from round 16: the pose race is hardened not reproduced (wants a
  Pixel session with `framesYielded=`), loop-end fires on one capture in six,
  scan-039 is rescuable-later, room polygons, floor-map thickness assumed,
  `planSlice` only in the Options fold, the Mid-360 ABI rewire (needs hardware),
  `restoreOrphaned()`.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

## ROUND 18 — THE GYRO RAN THE WHOLE TIME AND THE BRIDGE NEVER ASKED IT; THE SNAP TOOK 57° ON FAITH; AND THE PATH DREW WALKS THAT NEVER HAPPENED

Owner field session of 0.9.2 (2026-08-19, 03:11–03:25, scans 046–053; 051 and
053 carry the round-17 debug log — he found developer mode himself). His one
sentence: *"the path record seems not so accurate."* He is right, three
different ways. Items 68–72 in `docs/design/REVIEW_FEEDBACK.md`; this section
is the Android-side account plus what only the bytes could tell.

**Mid-round owner correction, folded in:** the room had GOOD lighting. The
dim-light theory for the 6–7 s losses is refuted and nothing shipped assumes
it — see "why he loses tracking", below, for what the streams actually say.

### 68 — "no continuous gyro across the gap", while phone-imu reports 399.1 Hz

scan-053's debug log carries both lines 55 seconds apart: a bridge refusal for
want of gyro, and an IMU shutdown line counting 22,401 samples at 399.1 Hz
with 3 drops. Both are true. The refusal was over a **46 ms sliver**:
SensorManager delivers its first event 44–70 ms after `registerListener`
(measured across all seven captures against the first ARCore pose, which the
already-running preview session delivers immediately), his captures lose
tracking AT START (the world-frame reset re-acquiring; every first break sits
at t≈+0.6 s), so the bracket's `t_before` is the first pose — and
`integrate_()` demanded ring coverage to within 25 ms of both interval ends.
97 % coverage of a 1554 ms window, refused whole.

Engine fix (poses/ only, ABI still 12): `bridge_edge_slack_ns` = 100 ms —
deliberately `snap_gap_ns`, because it is the same physical claim (a ≤100 ms
slice of a walking human is a statue to round 13's budget); edge slack applies
to `relative_rotation()` only, interior holes are still fatal, and
`sample_at()` densification passes 0 and is bit-identical. Plus:
`set_imu_extrinsics` carries the ring across its rebuild (samples are raw
sensor-frame; the extrinsic applies at integration time) — capi smoke step 198
now asserts the carry-over instead of the drop.

Android fix: the seal log's unhealed arm printed **"NOT healed (no usable
bracket)"** for every refusal — a fabricated reason that hid the engine's real
verdicts for a whole round. It now prints `lastReanchorSummary()`, or "no
verdict recorded" when the bracket genuinely never reached the engine.

On his bytes (engine_cli on copies; `streams/` untouched): 053's 1.554 s /
0.28° gap → gyro 0.99°, residual 0.88°, bridges live (offline the ruler says
thin-submap — it is the capture-start gap, there is nothing on the far side to
move, and moving nothing is correct). 046's 6.897 s gap → gyro **178.63°** vs
reported 72.28°, residual 106.54° → refused-gyro-disagrees WITH THE NUMBERS —
the round-17 policy working, previously mislabelled as a bracket failure. 050:
gyro 115.63° vs 29.94°, residual 85.70°, same. `stitch.json` now records every
examined gap (`gapsExamined[]`), because a refusal that leaves no trace in the
bundle is how this took two rounds to see.

### 69 — the ≤100 ms snap applied 56.85° at an implied 1,720°/s

scan-047 break #3: one frame, 0.371 m / 56.85°, healed live through the snap
path — which never consulted anything. The capture's own gyro over those 33 ms:
**0.67°**. Round 13's real re-anchors were 8–13.5°; a 57° one-frame jump is a
relocalisation or a frame restart. The snap fast-path now applies only at or
under `max_residual_rotation_deg` (25°); past that the pair takes the bridge's
gyro-checked route (residual ≈ reported → refused-disagree; a genuine violent
turn the gyro confirms → negligible, the operator; no gyro → refused in plain
words). Translation stays ungated — the gyro cannot witness it and round 13
verified the big translation-only snaps against gravity. All round-13-sized
snaps bit-identical (pinned in `test_round18_snap_gate.cpp` against the exact
analytic transform, with scan-047's real pose pair and integrated gyro as the
fixture).

**Measured: that heal HURT.** 0.9.2 (snap applied): 2 sections, first section
rotated 56.85°, self-check 6.92 cm — his session's worst. 0.9.3 (refused):
1 section, self-check **3.42 cm**. The ruler had said `map-got-worse` all
along and was overruled by its own pipeline.

### 70 — the path: frozen segments, teleports, and pathM counting them

During a loss ARCore freezes (round 17: 181 poses of 0.000 m/0.00°); the trail
held still while he walked, then teleported — drawn in confident teal, and the
teleport metres COUNTED in pathM. Now the verdict rides the whole way:

* `TrajectoryTrail.Point.jump` / `NormalizedPoint.jump`: the recorder flags a
  kept point when any pose since the last kept one was disowned, either
  endpoint is untracked, or the step implies >6 m/s (the silent refused-heal
  teleport, tracking green throughout). `totalPathM` excludes those segments;
  `totalJumpM` holds them; the seal logs `pathM=… jumpM=…` and
  `ScanSummary.jumpLengthMeters` carries it.
* 2D tile: jump segments dashed red. 3D ribbon: `TrajectoryRibbon.BRIDGE`, a
  DARKENED red — not alpha, because `trail.mat` is `blending: opaque` and a
  translucent vertex would silently render opaque (checked before shipping,
  not after).
* `trajectory.bin` → **"LSTRAJ02"**: 16-byte records, xyz + u32 flags (bit 0
  untracked, bit 1 jump-in: >150 ms between poses, >6 m/s, or the step out of
  a disowned run). Both readers (`TrajectoryFile.kt`, `lscan_plan.cpp`) accept
  v1 and v2; unknown versions draw no path rather than guessing a record size.
  Verified on scan-046's actual reprocessed bytes: 206 untracked poses
  flagged, 3 jump-ins, index 1352 = the 72.28° re-acquisition; the floor-plan
  sheet draws those three as red dashed bridges
  (`PlanRasterOptions::trajectory_breaks`), regenerated and eyeballed.
* Review drops untracked vertices (their positions are held guesses) and
  colours the bridge; the live ribbon, Review, the file and the sheet now
  tell one story.

**Why he loses tracking — measured, not guessed.** The 5 s before each long
loss: median lidar range 1.00–1.23 m with 63–70 % of returns under 1.5 m
(round 13's ARCore re-anchor diet: close, feature-poor surfaces), parallax
healthy at 1.7–2.2 cm/°. 047's short loss: 127° of turning in 5 s at the
0.8 cm/° parallax edge. The +0.6 s first breaks are the app's own
world-frame-reset re-acquisition (now healed trivially by item 68). **No
low-light warning ships** — the correction killed it and the data never
supported it. What ships is the missing signal: ARCore's own
`TrackingFailureReason` is written to the capture debug log at every loss
transition (`CaptureArController.onTrackingLost`), so the next session reads
the tracker's verdict instead of reconstructing one.

### 71 — scan-053 exported as a silent half-bundle

His export contains `processed/preview.f32` and nothing else — the export
raced auto-process. Now: `ProcessingRepository.reprocessD6` holds a
per-container lock (an export WAITS for a running auto-process — idempotent
job, waiting IS the fix); `transferBundle` ensures processing before zipping
("Processing before export…"), and on failure or an unprocessable container
writes `processed/UNPROCESSED.txt` into the bundle naming what is missing and
why. No third outcome. The capture debug log stays OPEN through auto-process
and closes with the verdicts inside (round 17 closed it at the seal — one line
before the answer the owner's question needed); `endCaptureDebugFor()` guards
by project so a late completion can never write into a newer capture's bundle;
pruned-empty scans still close at the seal because their directory is about to
be deleted. Still 5 MB-capped, still not a stream, replay untouched.

### 72 — rank=100.55 was `UNMEASURED_RANK_BASE + spreadP90` in a trench coat

The auto-refresh comparison was RIGHT (a measured 0.78° split-half accuracy
beats a 1 s sample that cannot be split-half checked); the log line printing
raw ranks and calling the candidate "worse" was wrong twice. Now plain words:
which won, why (unverifiable vs measured-worse vs jitter tie-break), numbers
labelled. And the 03:15:59 re-zero acceptance at spreadP90 2.24° was CORRECT
(the movement gate asks "still enough to average", limit 2.50°) while the
round-10 0.8° goal is about the mean's split-half accuracy — scan-050's 1.35°
missed it and nothing said so at the time. A captured trim past
`WARN_STABILITY_DEG` now warns at acceptance and the captured log line carries
`accuracyDeg=`.

### TESTS AND VERSION

Engine **621 cases / 2,517,351 assertions**, ctest **7/7 serial**, capi smoke
updated (ring carry-over). ABI stays **12** — every engine change is inside
poses/ + slam/post/ + plan/. `:core` **560**, `:app` **108** (+9:
`Round18PathHonestyTest`, `Round18TrajectoryFlagsTest`), emulator **22/22** on
`b4_test` with the native library rebuilt from this round's sources (the
on-device round-17 trajectory test now pins the v2 record size through the
real C entry point). Owner-capture regression on untouched originals:
046/048/050/051 maps and self-checks **byte-identical**; 047 changes by
design (6.92 → 3.42 cm); 053 gains processed results it never had. VERSION
**0.9.3**.

### BACKLOG

* **The long losses have a measured trigger nobody warns about live**: ~1 m
  from feature-poor surfaces, 2/3 of returns under 1.5 m. The engine has the
  range distribution live; a "too close to the wall for the tracker" cue is
  buildable and round 13's cue-budget rules apply. Deliberately not shipped
  this round — the cue system already fires tracking_degraded in those
  stretches, and a second overlapping cue needs the round-13 accelerometer
  audit repeated first.
* **Offline thin-submap refusals at capture-start gaps are correct but
  unreported to the operator** — the seam list explains it in stitch.json;
  the Review card does not surface `gapsExamined` yet.
* **`fromPoses` v1 path treats every vertex as tracked** (round-16 statement,
  still true for pre-0.9.3 containers); reprocessing any old container
  upgrades it to v2.
* **Capture-start tracking loss is app-caused** (world-frame reset
  re-acquisition, 0.9–1.6 s at every Start in this session). Item 68 heals
  it; not making it happen — e.g. delaying record-arm until first TRACKED
  pose — would change the round-12/16 start-gate semantics and wants its own
  round.
* Unchanged from rounds 16–17: pose race wants a Pixel `framesYielded=`
  session, scan-039/040 rescue via gyro-constrained lidar registration, room
  polygons, floor-map thickness, `planSlice` fold, Mid-360 rewire,
  `restoreOrphaned()`, `displayParams` synthesis, `syncTrail` flag order.
* Nothing was run on kc-m4; macOS/desktop untouched. No commit, no push.

## ROUND 19 — THE REFUSED GAPS BECOME REGISTRATION PROBLEMS; THE THROWN-AWAY RETURNS GET A SECOND HEARING; COVERAGE POINTS A DIRECTION

Owner-approved wave (his items 1, 2, 6, 7 plus the recovery item his
correction earned), on the round-18 evidence: the long losses were close
feature-poor surfaces and fast turns in GOOD light, the D6 painted straight
through every one of them at 4 kHz, and the gyro measured straight through
them at 399 Hz. Items 73–77 in `docs/design/REVIEW_FEEDBACK.md`; this section
is the Android-side account and the field-facing behaviour.

### 73 — the gap rescue (engine, `slam/post/gap_rescue.h`)

A refused gap is no longer the end of the story: `Process` now retries every
refusal as a REGISTRATION — rotation locked to the gyro that witnessed the
blind window (round 12's lesson made structural: the rotation is never the
unknown), translation solved from the walls the two sides share, in the
observable subspace only, coarse-searched deterministically, and with the
ROUND-12 ruler voting last. Owner numbers:

* **scan-050 RESCUED**: 115.63° locked, 0.298 m solved; the two sides of the
  gap went 32.2 → 12.1 cm apart, the self-check 1.77 → 1.40 cm, and the
  loop-end gap **5.70 → 3.39 m**. First blind-window fold ever repaired on
  his real bytes.
* scan-046 and scan-040 refused by the ruler (2.23 → 2.46 cm, 2.64 → 2.79 cm)
  — 046's registration is geometrically excellent and the refusal is the
  seventh gate working exactly as written; both maps stay byte-identical.
* scan-047's two short-gap refusals now carry geometric confirmation: with
  the gyro-locked rotation applied the sides agree LESS, which is what a
  transient relocalisation blip looks like from the walls' side.
* scan-039 finally has its precise refusal: `rescue-no-anchor` — no tracked
  pose exists on either side of anything, and gyro orientation without a
  translation witness is the null space this project refuses to invent.
  scan-045 likewise (every pose disowned by the double-start), and its
  zero-point refusal now accounts for all 34,436 returns instead of 223.

Nothing reaches `streams/`; rescues land through `processed/` exactly like
stitching, `stitch.json` gains `rescues[]`, and deleting `processed/` still
returns the container to what the phone sealed.

### 74 — the loss-window recovery (offline only, ruler per gap)

Bridged or rescued gaps get their excluded returns re-resolved against the
gyro-integrated, endpoint-pinned trajectory (the densifier's own closing
model; position honestly lerped). scan-050's 23,609 candidates were recovered
and then VETOED by the ruler (1.40 → 1.96 cm — a 6.4 s lerp under a 116°
pacing turn smears, and the number said so): they stay excluded, and
`recoveries[]` records the veto with its numbers. The synthetic corridor
fixture proves the admit path end-to-end (5k+ returns recovered and kept when
the interpolation is right). The LIVE `exclude_flagged` default is untouched
— this is `Process`-only, by design.

### 75 — coverage as a direction (`CoverageCompass`, trail-tile ring, card line)

Twelve azimuth sectors around the walked path, counted from where the
operator stood when each return resolved, thin judged against the sector mean
(scale-free), silent under 10 k returns. The trail tile grows amber edge arcs
pointing at the uncovered walls (1 Hz poll, visual only — no new cues, the
round-13 budget applied to pixels; amber is the round-11 coverage shade,
exactly (255, 176, 48)); the summary card gains at most ONE sentence naming
the largest thin arc relative to the walk direction, slotted BELOW tracking
advice in the existing chain. The renderer feeds the compass on the same
upload path as the coverage grid and the operator origin rides the trail
ribbon that already flows through `setTrail` — no new plumbing.

### 76 — the round-16 duplication list, closed

One D6 process pipeline (the Jobs tab's D6 "Post-process" now runs
`reprocessD6` instead of queuing the correction-less `JobKind.POST_PROCESS`;
the queue remains the Mid-360's LIO re-run). One Review chrome (the route is
wrapped in `UnderTabBar` like every sibling — both doors stay, the back-stack
risk round 16 named is not re-taken). One display truth:
`CaptureViewModel.displayParams` now `copy()`s the five live controls onto a
persisted DEVICE display block that Review's panel also writes — Review's
walked-path toggle reaches the live view for the first time, the
out-of-combine fields stop resetting at project creation, and the divergent
slider ranges collapse into `DisplayLimits` (0.1–12 px, 0.5–50 M, one pair of
constants, two panels). `DisplayLimits.POINT_SIZE_STEPS` also stops
float-truncating its own grid (Math.round, not toInt()).

### 77 — the pre-scan checklist (reads state, gates nothing)

A sheet on the FIRST Start press per device: mount trim age + measured
accuracy vs the 0.8° goal (amber past 1.0°), tracking readiness in the start
gate's own words, DND status, and one technique line from the measured
causes: *"Keep about an arm's length or more from blank close surfaces, ease
through the turns, and walk a loop that ends where it started."* Its Start
button continues the intercepted press; "don't show again" is one persisted
one-way bit (`pre_scan_checklist_dismissed`). The round-12/16 start gate is
untouched. The same correction purged the last two "light" advice strings
from the summary card (breaks ≥ 2 and trackingDrops > 0 now name the
measured diet), and a :core test makes "light" in advice a build failure.

### the D6 yield audit (item 66's log gets one line; stitch.json gets `yield`)

Where the 4,000 samples/s go, measured per capture: out-of-window is ZERO on
every owner capture (the range window needs no retuning and none happened);
true no-returns are 0.3–1.3 %; the one big loss is flagged-excluded during
long losses (12–21 % on the loss captures — items 73/74's territory).
scan-051 resolves 99.1 % of everything its sensor said. After auto-process
the capture debug log now carries `d6 yield: … = … no-return + …
out-of-window + … no-pose + … flagged-excluded + … resolved (+… recovered)
rescues=a/b`, read back from the sidecar by `StitchSidecar` (a fifteen-line
field reader, not a JSON dependency — the engine's own manifest reads set the
precedent in the other direction).

### TESTS AND VERSION

Engine **629 cases / 2,527,464 assertions**, ctest 7/7 serial. `:core`
**573** (+13: CoverageCompass, the advice-text guards, the widened
DisplayLimits pins), `:app` **119** (+11: the checklist intercept, the
display-base copy() regression tests, the sidecar reader), emulator suite on
`b4_test` (API 34, arm64) with the native library rebuilt from this round's
engine sources. ABI stays **12** — rescue, recovery and the yield audit are
all inside post/ and the sidecar. Owner-capture regression:
046/040/047/048/051/053 byte-identical to 0.9.3; **050 changes by design**
(rescued). VERSION **0.9.4**.

Backlog (noted, not built): capture-start tracking loss is app-caused — the
start-reset re-acquisition breaks the first ~0.6 s of every capture and is
round 20's item. The floor plan's path overlay has no per-user toggle (a capi
change; consciously left). scan-046's ruler refusal is structural — a fold
hides from a metric its two halves never share cells with — and a
fold-aware referee (occupancy-style, like loop-end's gate 6) is the honest
next step if the owner wants 046's rescue landed.
