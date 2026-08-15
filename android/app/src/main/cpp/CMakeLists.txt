# android/app/src/main/cpp/CMakeLists.txt — B2 JNI shim build.
#
# OWNERSHIP: this file (and everything else under android/) belongs to B2.
# It reaches into ../../../../../engine (repo_root/engine) via add_subdirectory
# and CONSUMES the engine's CMakeLists.txt / capi headers exactly as they are
# — nothing under engine/ is written to. If A1/integration-#24 lands new capi
# functions mid-task, this file does not pick them up automatically; it was
# pinned against SCAN_ABI_VERSION 1 (see scanengine_jni.cpp's startup check).
#
# BUILD INTEGRATION CHOICE (documented per the B2 brief, which left this
# open): Gradle's externalNativeBuild { cmake { ... } }, i.e. the Android
# Gradle Plugin drives CMake itself rather than us invoking the engine's own
# CMakePresets.json "android-arm64" preset by hand. Why:
#
#   - AGP already knows the right NDK toolchain file, ANDROID_ABI, sysroot
#     and STL for whatever abiFilters/minSdk the app declares — no manual
#     ANDROID_NDK_HOME / android.toolchain.cmake wiring needed here, and it
#     is the toolchain a real Android Studio user would get automatically.
#   - The engine's own "android-arm64" CMake preset assumes a vcpkg
#     toolchain (VCPKG_ROOT) to cross-compile Eigen etc. for arm64-android;
#     bootstrapping vcpkg + its Android triplet in this environment is heavy
#     and unnecessary here, because engine/CMakeLists.txt already falls back
#     to FetchContent for Eigen3 when no vcpkg toolchain is chainloaded (see
#     its "ENGINE_FETCH_EIGEN" path) — so add_subdirectory(engine) below
#     cross-compiles cleanly under the plain NDK toolchain AGP provides,
#     with no vcpkg involved at all.
#   - ANDROID_PLATFORM: the engine preset pins android-24; this build uses
#     whatever AGP derives from the app's minSdk (29, already higher), which
#     is the standard Gradle-managed behaviour and a strictly newer/stricter
#     platform than the preset's floor.
#
# Only the `scanengine` STATIC library target is consumed from engine/ (its
# own tests/tools/CLI are turned off below — irrelevant to an app build).
#
# B3 FLIPPED ENGINE_WITH_LIVOX_SDK2 TO **ON** (B2 had it OFF, D6-only). `ON`
# rather than `AUTO` deliberately: AUTO silently degrades to a build with no
# SDK2 backend if the tree happens to be missing, and the failure then shows
# up only at runtime as `start()` returning kNotSupported on a user's device.
# `ON` turns that into a configure-time FATAL_ERROR naming the fetch script,
# which is what we want for an app whose Mid-360 path is not optional.
#
# Because `ON` requires the tree to exist AT CONFIGURE TIME, the fetch cannot
# live in CMake — Gradle's `prepareLivoxSdk2` task (app/build.gradle.kts) runs
# android/scripts/prepare_livox_sdk2.sh before AGP's configure step. That
# script wraps the ENGINE's own engine/third_party/fetch_sdk2.sh (unmodified;
# engine/ is read-only here) and then applies android/third_party/
# patches-android/*.patch — this task's own bionic overlay, which is empty:
# stock-plus-engine-patches SDK2 compiles clean under NDK r27d. See that
# directory's README.md for the exact verification and for the rule any
# future overlay patch has to follow.
cmake_minimum_required(VERSION 3.22)
project(scanengine_jni_shim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- pull in libscanengine, app-build flavored ------------------------------
set(ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENGINE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
# B3: Mid-360 needs the SDK2 backend — it is the only one that can bring an
# out-of-the-box device up (A3 §3's backend table). STRING, not BOOL: the
# engine declares this cache entry as a tri-state STRING (AUTO/ON/OFF), and
# forcing it to BOOL here makes engine/CMakeLists.txt's
# `if(ENGINE_WITH_LIVOX_SDK2 STREQUAL "AUTO")` compare against a type it never
# expects.
set(ENGINE_WITH_LIVOX_SDK2 "ON" CACHE STRING "" FORCE)
set(ENGINE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

get_filename_component(LIDARSCAN_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine" ABSOLUTE)
if(NOT EXISTS "${LIDARSCAN_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "scanengine_jni: expected the engine at ${LIDARSCAN_ENGINE_DIR} (repo_root/engine) — layout changed?")
endif()

add_subdirectory("${LIDARSCAN_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/scanengine")

# --- the JNI shim itself -----------------------------------------------------
# B4 adds replay_engine.{h,cpp} (a standalone scanengine::Engine + ReplaySource
# helper — see replay_engine.h's header comment) and replay_jni.cpp (its JNI
# bindings, plus the page-read functions in scanengine_jni.cpp). Both link
# against engine/'s C++ API directly (not just capi/scanengine_c.h), which is
# fine: `scanengine` (linked below) is the same static library either way.
# B7/B8 add arcore_jni.cpp (ARCore poses in, the D6 pushbroom, the
# mount-calibration solver) and keyframe_writer.{h,cpp} (the frames.idx
# writer — see its header for why it links the engine's C++ color/ module
# instead of a `scan_engine_record_keyframe` C-ABI call that does not exist
# at SCAN_ABI_VERSION 3). keyframe_writer.cpp is deliberately free of any JNI
# type so android/tools/keyframe_roundtrip/ can compile the SAME source on a
# host and actually exercise it — see that tool's CMakeLists.
# B3 adds mid360_probe.{h,cpp} (a standalone scanengine::Engine + Mid360Config
# helper for the connect wizard's self-test — the only way to reach
# Mid360Config's backend selector, ports, filter and UdpConfig::prebound_fd,
# none of which the C ABI's scan_device_config exposes; see its header) and
# mid360_jni.cpp (its JNI bindings, plus the C-ABI Mid-360 add_device the
# capture path uses and the TMPDIR fix-up SDK2 needs on Android).
# B6/B11/B12 add processing_engine.{h,cpp} (a standalone scanengine::Engine
# driving A15's JobQueue, A12's floor-plan extractor and A13's MergeProject —
# none of which has ANY C-ABI surface at SCAN_ABI_VERSION 4, by A15's own
# explicit design decision; see that header's comment) and processing_jni.cpp.
# B9 adds gnss_jni.cpp, which — unlike every other helper here — needs no C++
# linkage at all: INT-29 landed A10's whole surface in the C ABI, so the RTK
# rover goes through scan_engine_push_nmea/scan_engine_last_fix/scan_ntrip_* on
# the SAME scan_engine* the capture session owns, which is what keeps
# record-always intact for the NMEA stream.
add_library(scanengine_jni SHARED
    scanengine_jni.cpp
    replay_engine.cpp
    replay_jni.cpp
    arcore_jni.cpp
    keyframe_writer.cpp
    mid360_probe.cpp
    mid360_jni.cpp
    processing_engine.cpp
    processing_jni.cpp
    gnss_jni.cpp
)

target_link_libraries(scanengine_jni PRIVATE
    scanengine
    log        # NDK liblog, for __android_log_print diagnostics
)

target_compile_options(scanengine_jni PRIVATE -Wall -Wextra)
