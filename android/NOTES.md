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
- **B3 (Mid-360 connect)**: same shape as B2 — a `RealEngineBridge`-adjacent
  path (or an extension of it) for `SensorType.MID360`, plus flipping
  `ENGINE_WITH_LIVOX_SDK2` back `ON` in
  `android/app/src/main/cpp/CMakeLists.txt`. `RealEngineBridge.connect`
  currently fails cleanly (not silently) for any non-`COIN_D6` target — that
  check is the seam to extend. Rebind against the C ABI as it exists when B3
  starts (see "C ABI gaps found" above — the `DEVICE_HEALTH` event gap
  matters more for B3, since Mid-360 actually publishes it).
- **B4 (Capture screen) — done**, see the "B4 — Filament capture screen +
  live 3D rendering" section below: live 3D view (Filament/`SurfaceView`),
  status strip, Live-SLAM toggle (now genuinely bound to
  `scan_session_config.live_slam`, ABI 2), pause/resume/stop with a session
  summary sheet, and the "Replay synthetic capture" debug acceptance path.
  `pointCountEstimate` on `ProjectManifest` is **still** not populated on
  capture stop (B4 didn't touch this either) — still worth wiring from
  `CaptureViewModel`'s final stats so the Projects list card stops showing
  "No capture yet"; flagging again for whichever task picks up B5/B6.
- **B5 (Profiles + Settings)**: extends `ui/settings/` — the units/theme
  DataStore plumbing already exists; profile *editing* (vs. just picking one
  at project-creation time, which B1 covers) is the new part.
- **B6 (Processing UI + Review)**: replace the two "Arrives with B6" stub
  cards the same way as B4.
- **B7 (ARCore/mount calibration)**: populates `mountCalibrationId` on the
  manifest (field already reserved).
- **B9/A10 (RTK/CRS)**: populates `crsEpsg` (field already reserved).
- **B10/B11 (display params, measure)**: `Units` (m/ft) already lives in
  `com.lidarscan.app.data.SettingsModels` — worth promoting to `:core` if
  measure/display logic needs it outside the UI layer.
- **B12 (merge)**: reads/writes the `merged/` subdirectory `FileProjectStore`
  already creates per project.

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
