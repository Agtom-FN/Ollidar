plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

// --- B4: matc material compile pipeline -------------------------------------
//
// Filament materials (android/app/src/main/materials/*.mat) are text source;
// the .filamat binary a MaterialInstance actually loads is compiled by
// `matc`, Filament's own tool, at BUILD time — there is no runtime .mat
// interpreter. matc ships in Filament's release tarball, not on Maven, so it
// is fetched on demand (once, cached) rather than committed as a binary; see
// android/scripts/fetch_filament_tools.sh for exactly why its version must
// match the filament-android AAR version (libs.versions.toml's `filament`
// entry), not desktop's separately-pinned native v1.75.0.
val filamentToolsVersion = libs.versions.filament.get()
val filamentToolsDir = file("$projectDir/../third_party/filament-tools-v$filamentToolsVersion")
val filamentMatc: File
    get() {
        val exeName = "matc"
        return File(filamentToolsDir, "filament/bin/$exeName")
    }
val materialsSrcDir = file("$projectDir/src/main/materials")
val materialsOutDir = layout.buildDirectory.dir("generated/materials/assets/materials")

val fetchFilamentTools by tasks.registering(Exec::class) {
    description = "Downloads the matc material compiler matching the pinned Filament AAR version (see NOTES.md)."
    val script = file("$projectDir/../scripts/fetch_filament_tools.sh")
    val marker = File(filamentToolsDir, "filament/bin/matc")
    inputs.file(script)
    outputs.file(marker)
    onlyIf { !marker.exists() }
    commandLine("bash", script.absolutePath, "v$filamentToolsVersion")
}

val compileMaterials by tasks.registering {
    description = "Compiles android/app/src/main/materials/*.mat to .filamat via matc, into an asset source dir."
    dependsOn(fetchFilamentTools)
    inputs.dir(materialsSrcDir)
    outputs.dir(materialsOutDir)
    doLast {
        val outDir = materialsOutDir.get().asFile
        outDir.mkdirs()
        val matFiles = materialsSrcDir.listFiles { f -> f.extension == "mat" }.orEmpty()
        if (matFiles.isEmpty()) {
            logger.warn("compileMaterials: no .mat files found in $materialsSrcDir")
        }
        matFiles.forEach { matFile ->
            val outFile = File(outDir, "${matFile.nameWithoutExtension}.filamat")
            logger.lifecycle("matc: ${matFile.name} -> ${outFile.name}")
            exec {
                // Compiled for both backends (opengl default + vulkan, since
                // this app does not hard-pin Engine.Backend) at the mobile
                // feature tier — see points.mat's own header comment.
                commandLine(
                    filamentMatc.absolutePath,
                    "-a", "opengl",
                    "-a", "vulkan",
                    "-p", "mobile",
                    "-o", outFile.absolutePath,
                    matFile.absolutePath,
                )
            }
        }
    }
}

// --- B3: vendored Livox-SDK2 (Mid-360) --------------------------------------
//
// src/main/cpp/CMakeLists.txt sets ENGINE_WITH_LIVOX_SDK2=ON, and the engine
// treats ON-with-a-missing-tree as a configure-time FATAL_ERROR (deliberately —
// AUTO would silently ship an app whose Mid-360 path fails at runtime). So the
// SDK tree has to exist BEFORE AGP's CMake configure step runs, which is too
// early for CMake itself to fetch it. This task is that step.
//
// It shells out to android/scripts/prepare_livox_sdk2.sh, which wraps the
// ENGINE's own engine/third_party/fetch_sdk2.sh (unmodified — engine/ is
// read-only for B3) and then applies android/third_party/patches-android/*.patch,
// this task's own bionic overlay. Same "fetched on demand, not committed"
// shape as B4's fetchFilamentTools above.
//
// `onlyIf` + an output marker keep it a no-op once the tree is there, so this
// costs nothing on an incremental build and needs no network after the first
// run. Set LIVOX_SDK2_TARBALL to point at a local tarball for an air-gapped
// build (fetch_sdk2.sh honours it).
val livoxSdk2Dir = file("$projectDir/../../engine/third_party/Livox-SDK2")

val prepareLivoxSdk2 by tasks.registering(Exec::class) {
    description = "Fetches + patches the vendored Livox-SDK2 (Mid-360 SDK2 backend) before the CMake configure step."
    val script = file("$projectDir/../scripts/prepare_livox_sdk2.sh")
    val overlayDir = file("$projectDir/../third_party/patches-android")
    val marker = File(livoxSdk2Dir, "CMakeLists.txt")
    inputs.file(script)
    inputs.files(fileTree(overlayDir) { include("*.patch") })
    outputs.file(marker)
    onlyIf { !marker.exists() }
    commandLine("bash", script.absolutePath)
}

android {
    namespace = "com.lidarscan.app"
    compileSdk = 36

    // B2: NDK r27d, matching S7/CI's pin (.github/workflows/engine-ci.yml
    // "ndk-version: r27d" == package ndk;27.3.13750724 in sdkmanager's
    // numbering — see android/NOTES.md). AGP resolves this from the SDK's
    // ndk/<version>/ directory; no ANDROID_NDK_HOME needed for a Gradle build.
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "com.lidarscan.app"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        // CI Android-emulator smoke test (:app:connectedDebugAndroidTest,
        // android/app/src/androidTest/) — see NOTES.md's "Android emulator
        // smoke test" section.
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        vectorDrawables {
            useSupportLibrary = true
        }

        // B2 dev-settings flag (see AppContainer / Settings screen "Use
        // simulated engine"): a build-time default for whether to prefer
        // FakeEngineBridge even when the native lib loads. False here — the
        // runtime Settings toggle is the day-to-day override; this exists
        // for a build variant that always wants the simulator (e.g. a demo
        // APK with no hardware access) without touching Settings.
        buildConfigField("boolean", "FORCE_FAKE_ENGINE", "false")

        // arm64-v8a is the real-hardware target (Tech Spec §3.1: Android
        // capture hardware is Pixel 7+/Galaxy S22+ class, all arm64) and
        // stays first/primary. x86_64 was added on top for the Android
        // emulator smoke test's CI leg (.github/workflows/android-emulator.yml,
        // see NOTES.md's "Android emulator smoke test" section) — GitHub-hosted
        // macOS runners can't hardware-accelerate an arm64-v8a emulator (no
        // nested Hypervisor.Framework), so that CI job runs on ubuntu-latest
        // with a KVM-accelerated x86_64 emulator instead, which needs a
        // native lib actually built for that ABI or every native call
        // (Filament's init included — the exact thing that test exists to
        // catch) fails immediately with a missing-ABI UnsatisfiedLinkError.
        // Verified locally that engine/ + the vendored Livox-SDK2 need zero
        // source changes to cross-compile for x86_64 — armeabi-v7a is still
        // not built, and still not this task's scope.
        ndk {
            abiFilters += "arm64-v8a"
            abiFilters += "x86_64"
        }

        externalNativeBuild {
            cmake {
                // engine/CMakeLists.txt's own ENGINE_BUILD_TESTS/TOOLS/
                // WITH_LIVOX_SDK2 are already forced off from
                // src/main/cpp/CMakeLists.txt (see its header comment for
                // why); nothing else to pass here today.
                arguments += "-DANDROID_STL=c++_shared"
                // Without this, AGP/ninja builds every buildable target in
                // the configured CMake project, not just ours — including
                // Eigen's own FetchContent'd eigen_blas/eigen_lapack demo
                // libraries and its spsolver/spbenchsolver/test_sparseLU
                // targets (Eigen adds those unconditionally; they are not a
                // build dependency of `scanengine_jni`, just siblings in the
                // same configure). Scoping to our target's own dependency
                // graph (scanengine_jni -> scanengine -> Eigen3::Eigen,
                // header-only) skips all of that.
                targets += "scanengine_jni"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.31.6"
        }
    }

    // B4: the compiled .filamat lands as a plain asset (materials/points.filamat),
    // loaded at runtime via AssetManager + Engine.createMaterial(ByteBuffer) —
    // no different from any other Filament Android sample's asset-based
    // material loading.
    sourceSets {
        getByName("main") {
            assets.srcDir(layout.buildDirectory.dir("generated/materials/assets"))
            // B4 replay acceptance path: the bundled synthetic D6 capture
            // (see assets/replay/synth.lscan/, sourced from S1's d6synth via
            // desktop/evidence/synth.lscan — see NOTES.md).
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
        debug {
            isMinifyEnabled = false
            applicationIdSuffix = ".debug"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        jvmToolchain(17)
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    // CI Android-emulator smoke test: belt-and-suspenders on top of the
    // workflow's own `disable-animations: true` step — this flag makes AGP's
    // test runner call `Settings.Global` animation-scale writes itself before
    // each test, which also covers running `connectedDebugAndroidTest`
    // locally against an emulator/device that was not booted through that
    // action.
    testOptions {
        animationsDisabled = true
    }
}

// Every variant's asset-merge step needs the compiled .filamat(s) to exist
// first; matched by name rather than a hard `tasks.named("mergeDebugAssets")`
// reference so it also covers mergeReleaseAssets without duplicating this
// block per build type.
tasks.matching { it.name.matches(Regex("merge[A-Za-z]*Assets")) }.configureEach {
    dependsOn(compileMaterials)
}

// B3: the SDK2 tree must exist before AGP *configures* CMake, not merely
// before it builds — ENGINE_WITH_LIVOX_SDK2=ON fails the configure outright
// when it is missing. AGP's per-variant/per-ABI native tasks are named
// `configureCMakeDebug[arm64-v8a]` / `buildCMakeDebug[arm64-v8a]` (the
// bracketed ABI is part of the task name), so this matches by prefix rather
// than naming one variant — otherwise a release build, or a second ABI added
// later, would silently miss the dependency and fail its configure.
tasks.matching {
    it.name.startsWith("configureCMake") || it.name.startsWith("buildCMake")
}.configureEach {
    dependsOn(prepareLivoxSdk2)
}

dependencies {
    implementation(project(":core"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    // Extended icon set only — it already contains the core icons, and
    // depending on both androidx.compose.material:material-icons-core and
    // -extended pulls in duplicate icon classes.
    implementation(libs.androidx.compose.material.icons.extended)
    debugImplementation(libs.androidx.compose.ui.tooling)

    // B2: D6 connect flow (CH340 USB-serial).
    implementation(libs.usbSerial)

    // B4: live 3D point-cloud renderer (SurfaceView + Filament, see
    // ui/capture/render/PointCloudView.kt). filament-utils-android is used
    // for its Manipulator (orbit-camera math) only.
    implementation(libs.filament.android)
    implementation(libs.filament.utils.android)

    // B7/B8: ARCore session management, VIO poses, camera frames + metadata.
    implementation(libs.arcore)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)

    // CI Android-emulator smoke test (:app:connectedDebugAndroidTest) — see
    // android/app/src/androidTest/ and NOTES.md's "Android emulator smoke
    // test" section.
    androidTestImplementation(libs.androidx.test.ext.junit)
    androidTestImplementation(libs.androidx.test.runner)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
}
