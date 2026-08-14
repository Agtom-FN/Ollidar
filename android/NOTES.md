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
- **B4 (Capture screen)**: the real destination now exists
  (`ui/capture/CaptureScreen.kt`, `project/{projectId}/capture`) with
  start/stop/pause/resume and live numeric stats wired to a real
  `EngineBridge`; B4's job is the Filament live 3D / AR overlay view this
  screen explicitly defers. `pointCountEstimate` on `ProjectManifest` is
  still not populated on capture stop — worth wiring from
  `CaptureViewModel`'s final stats so the Projects list card stops showing
  "No capture yet".
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
