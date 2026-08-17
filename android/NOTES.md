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
