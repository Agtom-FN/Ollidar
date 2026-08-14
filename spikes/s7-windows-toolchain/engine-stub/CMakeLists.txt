cmake_minimum_required(VERSION 3.24)

project(scanengine_stub
    LANGUAGES CXX
    DESCRIPTION "S7 spike: minimal cross-platform skeleton for libscanengine"
)

# --- Options -----------------------------------------------------------
option(ENGINE_BUILD_TESTS "Build scanengine unit tests (doctest)" ON)

# --- Global settings -----------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# vcpkg manifest mode (see ../vcpkg.json + ../CMakePresets.json) installs
# Eigen3 for us when a preset's CMAKE_TOOLCHAIN_FILE points at
# vcpkg/scripts/buildsystems/vcpkg.cmake. When configuring this directory
# standalone without vcpkg (e.g. a quick local check), fall back to
# FetchContent so the stub still builds. This dual path is intentional:
# it's the "prove the dep path" requirement from the S7 spike brief, tested
# against BOTH resolution mechanisms.
find_package(Eigen3 3.4 CONFIG QUIET)

if(TARGET Eigen3::Eigen)
    message(STATUS "scanengine: using Eigen3 from find_package (vcpkg/system): ${Eigen3_DIR}")
else()
    message(STATUS "scanengine: Eigen3 not found via find_package; fetching via FetchContent")
    include(FetchContent)
    set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        Eigen3
        GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
        GIT_TAG 3.4.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Eigen3)
    # Eigen's own CMakeLists (when built as a subproject) does not always
    # define the Eigen3::Eigen ALIAS the way the installed CONFIG package
    # does; normalize it here so downstream target_link_libraries() calls
    # are identical regardless of which path was taken.
    if(NOT TARGET Eigen3::Eigen)
        add_library(Eigen3::Eigen ALIAS eigen)
    endif()
endif()

find_package(Threads REQUIRED)

# --- Library target: scanengine ------------------------------------------
add_library(scanengine STATIC
    src/frame.cpp
    src/worker_pool.cpp
    src/eigen_demo.cpp
)

target_include_directories(scanengine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(scanengine
    PUBLIC
        Eigen3::Eigen
        Threads::Threads
)

target_compile_features(scanengine PUBLIC cxx_std_20)

if(MSVC)
    # See TOOLCHAIN_NOTES.md: GTSAM (and Boost transitively) require
    # /permissive- and correct __cplusplus reporting; adopt both now so the
    # stub's baseline matches what A1 will need once real deps land.
    target_compile_options(scanengine PUBLIC /permissive- /Zc:__cplusplus /utf-8)
else()
    target_compile_options(scanengine PUBLIC -Wall -Wextra)
endif()

# --- Unit tests ------------------------------------------------------------
if(ENGINE_BUILD_TESTS)
    enable_testing()

    add_executable(scanengine_tests
        tests/test_scanengine.cpp
    )
    target_link_libraries(scanengine_tests PRIVATE scanengine)
    target_include_directories(scanengine_tests PRIVATE third_party/doctest)
    target_compile_features(scanengine_tests PRIVATE cxx_std_20)

    include(CTest)
    add_test(NAME scanengine_tests COMMAND scanengine_tests)
endif()
