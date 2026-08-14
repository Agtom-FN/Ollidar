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
# Mid-360 (Livox SDK2) is explicitly OFF: B2 is D6-only; third_party/Livox-SDK2
# happens to be checked out in this workspace (fetched for A3/B3), but
# building it here would pull in Mid-360 UDP/networking code this task does
# not need and has not verified cross-compiles for Android. B3 flips this ON
# when it wires Mid-360 connect.
cmake_minimum_required(VERSION 3.22)
project(scanengine_jni_shim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- pull in libscanengine, app-build flavored ------------------------------
set(ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENGINE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENGINE_WITH_LIVOX_SDK2 OFF CACHE BOOL "" FORCE)   # D6-only for B2; B3 rebinds
set(ENGINE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

get_filename_component(LIDARSCAN_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../engine" ABSOLUTE)
if(NOT EXISTS "${LIDARSCAN_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "scanengine_jni: expected the engine at ${LIDARSCAN_ENGINE_DIR} (repo_root/engine) — layout changed?")
endif()

add_subdirectory("${LIDARSCAN_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/scanengine")

# --- the JNI shim itself -----------------------------------------------------
add_library(scanengine_jni SHARED
    scanengine_jni.cpp
)

target_link_libraries(scanengine_jni PRIVATE
    scanengine
    log        # NDK liblog, for __android_log_print diagnostics
)

target_compile_options(scanengine_jni PRIVATE -Wall -Wextra)
