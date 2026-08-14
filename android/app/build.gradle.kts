plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
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

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
}
