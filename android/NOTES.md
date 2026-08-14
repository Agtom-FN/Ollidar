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

- **B2 (D6 connect) / B3 (Mid-360 connect)**: implement the real
  `EngineBridge` (JNI-backed), call `EngineBridgeProvider.override(...)` at
  startup. Connect-flow UI (USB permission, static-IP wizard) is new
  screens/composables under `ui/`, reachable from Project Detail or a new
  "Device setup" stub the same way Capture/Processing/Review are today.
- **B4 (Capture screen)**: replace the "Arrives with B4" `StubNavCard` in
  `ui/detail/ProjectDetailScreen.kt` with a real destination in
  `ui/nav/LidarScanApp.kt`/`Routes.kt`; drive it off `AppContainer.engineBridge`
  (already wired, currently the fake). `pointCountEstimate` on
  `ProjectManifest` is where live/finished point counts should land so the
  Projects list card stops showing "No capture yet".
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
