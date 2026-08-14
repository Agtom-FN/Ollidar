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

        // arm64-v8a only for now (Tech Spec §3.1: Android capture hardware is
        // Pixel 7+/Galaxy S22+ class, all arm64). armeabi-v7a/x86_64 would
        // need their own libscanengine cross-compiles; not this task's scope.
        ndk {
            abiFilters += "arm64-v8a"
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
}

// Every variant's asset-merge step needs the compiled .filamat(s) to exist
// first; matched by name rather than a hard `tasks.named("mergeDebugAssets")`
// reference so it also covers mergeReleaseAssets without duplicating this
// block per build type.
tasks.matching { it.name.matches(Regex("merge[A-Za-z]*Assets")) }.configureEach {
    dependsOn(compileMaterials)
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

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}
