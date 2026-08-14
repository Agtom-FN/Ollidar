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

# --- Livox-SDK2 (A3) -------------------------------------------------------
# NOT a vcpkg port, and deliberately so: S2 proved stock SDK2 cannot even
# bind a socket on macOS (it bind()s 255.255.255.255, which Darwin rejects)
# and does not configure under CMake >= 4. We vendor a pinned tarball plus
# three patches; third_party/fetch_sdk2.sh is the whole recipe and the tree
# itself is gitignored.
#
# AUTO by default: present ⇒ build the SDK2 backend, absent ⇒ build
# everything else. That is what keeps the five CI legs green without a
# network fetch — the driver's decode/filter/loss/watchdog/reconnect logic
# is SDK-free and fully tested either way; only live capture needs the SDK.
set(LIVOX_SDK2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Livox-SDK2")
set(ENGINE_WITH_LIVOX_SDK2 "AUTO" CACHE STRING
    "Build the Mid-360 SDK2 backend: AUTO (if third_party/Livox-SDK2 exists), ON, or OFF")
set_property(CACHE ENGINE_WITH_LIVOX_SDK2 PROPERTY STRINGS AUTO ON OFF)

set(_engine_use_sdk2 OFF)
if(ENGINE_WITH_LIVOX_SDK2 STREQUAL "AUTO")
    if(EXISTS "${LIVOX_SDK2_DIR}/CMakeLists.txt")
        set(_engine_use_sdk2 ON)
    endif()
elseif(ENGINE_WITH_LIVOX_SDK2)
    if(NOT EXISTS "${LIVOX_SDK2_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "scanengine: ENGINE_WITH_LIVOX_SDK2=ON but ${LIVOX_SDK2_DIR} is missing. "
            "Run engine/third_party/fetch_sdk2.sh first.")
    endif()
    set(_engine_use_sdk2 ON)
endif()

if(_engine_use_sdk2)
    # EXCLUDE_FROM_ALL: the SDK's own CMake also adds 7 sample executables and
    # a shared library we do not link. Only livox_lidar_sdk_static gets built.
    add_subdirectory(${LIVOX_SDK2_DIR} livox_sdk2 EXCLUDE_FROM_ALL)
    message(STATUS "scanengine: Livox-SDK2 (patched) from ${LIVOX_SDK2_DIR}")
else()
    message(STATUS "scanengine: building WITHOUT Livox-SDK2 "
                   "(Mid-360 live capture disabled; run third_party/fetch_sdk2.sh to enable)")
endif()

# GTSAM / Ceres / Filament are NOT dependencies yet — see vcpkg.json for the
# ordering and TOOLCHAIN_NOTES.md for the per-port caveats (autotools ports
# break under the macOS universal overlay triplet; verify each one before
# assuming it builds).

# --- library ---------------------------------------------------------------
file(GLOB_RECURSE SCANENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(scanengine STATIC
    ${SCANENGINE_SOURCES}
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

if(_engine_use_sdk2)
    # PUBLIC so anything linking scanengine (engine_cli, the tests, the JNI
    # .so) pulls the static SDK in; the define is PRIVATE because no public
    # header mentions the SDK — src/drivers/mid360/mid360_sdk2.cpp is the only
    # translation unit that includes it, which is what keeps the C ABI and
    # every other consumer free of Livox headers.
    target_link_libraries(scanengine PUBLIC livox_lidar_sdk_static)
    target_compile_definitions(scanengine PRIVATE SCANENGINE_HAVE_LIVOX_SDK2=1)
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

    file(GLOB ENGINE_TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
    add_executable(scanengine_tests
        ${ENGINE_TEST_SOURCES}
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

    # --- optional: Mid-360 end-to-end against the S2 simulator (A3) --------
    #
    # The `mid360sim/*` cases in tests/test_mid360_driver.cpp carry
    # doctest::skip(), so `scanengine_tests` and every OTHER ctest entry
    # never pay for them. One extra entry, labelled "sim", re-invokes the
    # same binary with --no-skip:
    #
    #     ctest -L sim            # ~200 s: soak, injected loss, cable pull,
    #                             #         power-cycle, .lvx2 replay
    #     ctest -LE sim           # everything else, the fast path
    #
    # It is registered ONLY when the S2 simulator binary is actually there,
    # which is why every CI leg and every fresh clone is unaffected: the
    # spike's build tree is gitignored, so `ctest` on those machines has
    # nothing extra to run and `ctest -L sim` correctly reports no tests.
    # Build it with spikes/s2-mid360-sim/scripts/fetch_sdk2.sh + cmake, and
    # this engine with Livox-SDK2, to get the real thing. Set
    # ENGINE_SIM_TESTS=OFF to keep a built simulator out of the default run.
    #
    # If the pieces are present at configure time but missing or unusable at
    # run time, the cases print "SKIPPED: <why>" and
    # SKIP_REGULAR_EXPRESSION turns that into a CTest "Skipped" rather than a
    # false pass.
    set(ENGINE_S2_SIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../spikes/s2-mid360-sim"
        CACHE PATH "S2 Mid-360 simulator spike (for the 'sim'-labelled ctest)")
    option(ENGINE_SIM_TESTS "Register the Mid-360 simulator end-to-end ctest (label: sim)" ON)
    set(_s2_bin "${ENGINE_S2_SIM_DIR}/build/spike")

    if(ENGINE_SIM_TESTS AND EXISTS "${_s2_bin}/mid360_sim")
        add_test(NAME mid360_sim_e2e
                 COMMAND scanengine_tests --no-skip "--test-case=mid360sim/*" --success=false)
        set_tests_properties(mid360_sim_e2e PROPERTIES
            LABELS "sim"
            TIMEOUT 900
            # The simulator and the SDK bind the REAL Mid-360 ports (56000,
            # 561xx–564xx) on loopback, so no two of these may run at once —
            # not in parallel, and not against a second build tree on the
            # same host.
            RUN_SERIAL TRUE
            SKIP_REGULAR_EXPRESSION "SKIPPED:"
            ENVIRONMENT
                "SCANENGINE_SIM_BIN=${_s2_bin}/mid360_sim;SCANENGINE_LVX2_REPLAY_BIN=${_s2_bin}/lvx2_replay;SCANENGINE_LVX2_FILE=${ENGINE_S2_SIM_DIR}/datasets/Indoor_sampledata.lvx2"
        )
        message(STATUS "scanengine: Mid-360 simulator tests registered (ctest -L sim)")
    endif()
    unset(_s2_bin)

    # --- optional: GNSS/RTK end-to-end against the S5 simulators (A10) -----
    #     ctest -L sim-rtk      # ~40 s: live NMEA over TCP, live NTRIP
    #     ctest -LE sim-rtk     # everything else
    #
    # docs/A10-gnss.md §9.1, verbatim: src/gnss/*.cpp and tests/test_gnss.cpp
    # are already picked up by the globs above, so the 36 offline cases need no
    # CMake at all. This registers only the two doctest::skip()-tagged live
    # cases. Unlike A3's they bind PER-PROCESS ports (39000/40000 + pid % 900)
    # and write per-process temp files, so two build trees on one host cannot
    # interfere and RUN_SERIAL is not needed. They want python3 and the spike
    # directory; either missing prints "SKIPPED: ..." which
    # SKIP_REGULAR_EXPRESSION turns into a CTest skip rather than a false pass.
    # SCANENGINE_S5_SIM_DIR is a hint only — the test also searches upwards
    # from its CWD for spikes/s5-rtk-sim.
    set(ENGINE_S5_SIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../spikes/s5-rtk-sim"
        CACHE PATH "S5 RTK simulator spike (for the 'sim-rtk'-labelled ctest)")
    if(ENGINE_SIM_TESTS AND EXISTS "${ENGINE_S5_SIM_DIR}/nmea_sim.py")
        add_test(NAME gnss_rtk_sim_e2e
                 COMMAND scanengine_tests --no-skip "--test-case=gnsssim/*" --success=false)
        set_tests_properties(gnss_rtk_sim_e2e PROPERTIES
            LABELS "sim-rtk"
            TIMEOUT 300
            SKIP_REGULAR_EXPRESSION "SKIPPED:"
            ENVIRONMENT "SCANENGINE_S5_SIM_DIR=${ENGINE_S5_SIM_DIR}")
        message(STATUS "scanengine: GNSS/RTK simulator tests registered (ctest -L sim-rtk)")
    endif()
endif()
