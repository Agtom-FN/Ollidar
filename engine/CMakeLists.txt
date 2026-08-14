# libscanengine — the shared C++ engine (Tech Spec §3, workstream A).
#
# Migrated from the S7 spike's engine-stub, which proved this exact machinery
# (vcpkg manifest mode + overlay universal-osx triplet + 5 CI targets) green
# on Windows/MSVC, Windows/clang-cl, macOS universal, Linux and Android NDK.
# spikes/s7-windows-toolchain/ stays in place as the historical record.
cmake_minimum_required(VERSION 3.24)

# --- vcpkg -----------------------------------------------------------------
# The presets do NOT hard-code the vcpkg toolchain. Instead, if VCPKG_ROOT is
# set (which CI's lukka/run-vcpkg step does, and which a developer with a
# vcpkg checkout sets), we chainload it here — before project(), as CMake
# requires. Without VCPKG_ROOT the build falls back to find_package and then
# to FetchContent, so `cmake --preset macos-universal` works on a laptop that
# has never heard of vcpkg. Both paths are exercised: CI uses the first, the
# A1 local verification used the second.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain (auto-detected from VCPKG_ROOT)")
    message(STATUS "scanengine: using vcpkg toolchain ${CMAKE_TOOLCHAIN_FILE}")
endif()

project(scanengine
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "LidarScan shared capture/SLAM engine"
)

# --- options ---------------------------------------------------------------
option(ENGINE_BUILD_TESTS "Build the engine unit tests (doctest)" ON)
option(ENGINE_BUILD_TOOLS "Build engine_cli" ON)
option(ENGINE_WITH_EIGEN "Link Eigen (required from A6/A7 onwards)" ON)
option(ENGINE_FETCH_EIGEN "Fall back to FetchContent when Eigen is not installed" ON)
option(ENGINE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # the Android JNI .so links this in

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# --- dependencies ----------------------------------------------------------
find_package(Threads REQUIRED)

if(ENGINE_WITH_EIGEN)
    find_package(Eigen3 3.4 CONFIG QUIET)
    if(TARGET Eigen3::Eigen)
        message(STATUS "scanengine: Eigen3 from find_package (${Eigen3_DIR})")
    elseif(ENGINE_FETCH_EIGEN)
        message(STATUS "scanengine: Eigen3 not installed; fetching 3.4.0")
        include(FetchContent)
        set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
        set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        # A release tarball rather than a git clone: no git dependency, and
        # the hash pins it as tightly as a commit would.
        FetchContent_Declare(Eigen3
            URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
            URL_HASH SHA256=8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
        )
        FetchContent_MakeAvailable(Eigen3)
        if(NOT TARGET Eigen3::Eigen)
            add_library(Eigen3::Eigen ALIAS eigen)
        endif()
    else()
        message(FATAL_ERROR "scanengine: Eigen3 not found and ENGINE_FETCH_EIGEN is OFF")
    endif()
endif()

# GTSAM / Ceres / Livox-SDK2 / Filament are NOT dependencies yet — see
# vcpkg.json for the ordering and TOOLCHAIN_NOTES.md for the per-port caveats
# (autotools ports break under the macOS universal overlay triplet; verify
# each one before assuming it builds).

# --- library ---------------------------------------------------------------
add_library(scanengine STATIC
    # core/ — lifecycle, errors, logging, events                       (A1)
    src/core/engine.cpp
    src/core/error.cpp
    src/core/event_bus.cpp
    src/core/log.cpp
    src/core/types.cpp
    # transport/ — bytes in                                        (A1/A2/A3)
    src/transport/usb_serial_source.cpp
    src/transport/udp_source.cpp
    # drivers/ — protocol decode + health                          (A1/A2/A3)
    src/drivers/d6/d6_parser.cpp
    src/drivers/d6/d6_driver.cpp
    src/drivers/mid360/mid360_driver.cpp
    # timesync/ — clock + per-stream offsets                          (A1/A4)
    src/timesync/offset_estimator.cpp
    # record/ — .lscan container                                      (A1/A5)
    src/record/lscan.cpp
    # cloud/ — the render-facing point store                         (A1/A14)
    src/cloud/page_store.cpp
    # capi/ — the flat C ABI for JNI                                     (A1)
    capi/scanengine_c.cpp
)

target_include_directories(scanengine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(scanengine PRIVATE SCANENGINE_VERSION="${PROJECT_VERSION}")
target_compile_features(scanengine PUBLIC cxx_std_20)
target_link_libraries(scanengine PUBLIC Threads::Threads)
if(TARGET Eigen3::Eigen)
    target_link_libraries(scanengine PUBLIC Eigen3::Eigen)
    target_compile_definitions(scanengine PUBLIC SCANENGINE_HAVE_EIGEN=1)
endif()

if(MSVC)
    # Adopted from the S7 spike: GTSAM (and Boost transitively) require
    # /permissive- and correct __cplusplus reporting, so the baseline matches
    # what A7 will need.
    target_compile_options(scanengine PUBLIC /permissive- /Zc:__cplusplus /utf-8)
    target_compile_options(scanengine PRIVATE /W4)
else()
    target_compile_options(scanengine PRIVATE -Wall -Wextra -Wshadow)
endif()
if(ENGINE_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(scanengine PRIVATE /WX)
    else()
        target_compile_options(scanengine PRIVATE -Werror)
    endif()
endif()

# --- tools -----------------------------------------------------------------
if(ENGINE_BUILD_TOOLS)
    add_executable(engine_cli tools/engine_cli.cpp)
    target_link_libraries(engine_cli PRIVATE scanengine)
    # The synthetic-capture builder lives with the tests (it is the S1 packet
    # builder, kept in one place so the CLI and the tests generate identical
    # bytes).
    target_include_directories(engine_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()

# --- tests -----------------------------------------------------------------
if(ENGINE_BUILD_TESTS)
    enable_testing()
    include(CTest)

    add_executable(scanengine_tests
        tests/test_main.cpp
        tests/test_core.cpp
        tests/test_event_bus.cpp
        tests/test_cloud.cpp
        tests/test_record.cpp
        tests/test_engine.cpp
        tests/test_headers.cpp
        tests/test_d6_parser.cpp
        tests/test_capi.cpp
        tests/capi_smoke.c        # compiled as C — see the file header
    )
    target_link_libraries(scanengine_tests PRIVATE scanengine)
    target_include_directories(scanengine_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_features(scanengine_tests PRIVATE cxx_std_20)
    add_test(NAME scanengine_tests COMMAND scanengine_tests)

    # A pure-C executable: proves the header is C-clean all the way to main().
    add_executable(scanengine_capi_smoke tests/capi_smoke.c)
    set_target_properties(scanengine_capi_smoke PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(scanengine_capi_smoke PRIVATE SCAN_CAPI_SMOKE_MAIN=1)
    target_link_libraries(scanengine_capi_smoke PRIVATE scanengine)
    add_test(NAME scanengine_capi_smoke COMMAND scanengine_capi_smoke)

    if(ENGINE_BUILD_TOOLS)
        add_test(NAME engine_cli_selftest COMMAND engine_cli --selftest --quiet)
        add_test(NAME engine_cli_version COMMAND engine_cli --version)
    endif()
endif()
