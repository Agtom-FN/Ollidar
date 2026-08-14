# LidarScan desktop app — Qt 6 + Filament, linking libscanengine directly.
#
# Build (macOS):
#   brew install qt cmake ninja
#   ./tools/fetch_filament.sh v1.75.0        # pinned; see NOTES.md
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ./build/lidarscan
#
# The Filament version is PINNED on purpose. S3 found Filament's own docs wrong
# in two places in the same release (the NSView-vs-CAMetalLayer swapchain handle
# and a stale link list that omitted libzstd), so treating the version as
# fungible is not safe — REPORT.md §9 caveat 2.
cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(lidarscan_desktop LANGUAGES CXX OBJCXX)
else()
    project(lidarscan_desktop LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)          # Filament 1.75 requires C++20; so does the engine
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Qt 6
# ---------------------------------------------------------------------------
if(NOT CMAKE_PREFIX_PATH AND EXISTS "/opt/homebrew/opt/qt")
    set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
endif()
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort)

# ---------------------------------------------------------------------------
# The engine (workstream A) — linked as C++, not through the C ABI.
# ---------------------------------------------------------------------------
set(LIDARSCAN_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../engine" CACHE PATH
    "Path to the libscanengine source tree")
if(NOT EXISTS "${LIDARSCAN_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "engine not found at ${LIDARSCAN_ENGINE_DIR}")
endif()
set(ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # the engine's own CI runs those
set(ENGINE_BUILD_TOOLS ON CACHE BOOL "" FORCE)    # engine_cli --synth feeds verify.sh
add_subdirectory("${LIDARSCAN_ENGINE_DIR}" "${CMAKE_BINARY_DIR}/engine")

# A14 (display parameters) landed its header first and its implementation
# (engine/src/cloud/display_params.cpp) during this task. The desktop binds to
# the real thing: DisplayParams, DisplayParamsController, to_uniforms(),
# colormap_lut() and to_json()/from_json() all come from the engine, so there is
# no parallel copy of the display model on this side. Guard against a
# header-without-implementation tree, which links with undefined symbols that
# are hard to read.
if(NOT EXISTS "${LIDARSCAN_ENGINE_DIR}/src/cloud/display_params.cpp")
    message(FATAL_ERROR
        "engine/include/scanengine/cloud/display_params.h exists but "
        "src/cloud/display_params.cpp does not — A14 is mid-flight. The desktop "
        "app needs both.")
endif()

# ---------------------------------------------------------------------------
# Filament (prebuilt release under third_party/, gitignored)
# ---------------------------------------------------------------------------
set(FILAMENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/filament/filament")
if(NOT EXISTS "${FILAMENT_DIR}/include/filament/Engine.h")
    message(FATAL_ERROR
        "Filament not found at ${FILAMENT_DIR}.\n"
        "Run: ./tools/fetch_filament.sh v1.75.0")
endif()
if(EXISTS "${FILAMENT_DIR}/lib/arm64" AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/arm64")
elseif(EXISTS "${FILAMENT_DIR}/lib/x86_64")
    set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/x86_64")
else()
    file(GLOB _fil_libdirs "${FILAMENT_DIR}/lib/*")
    list(GET _fil_libdirs 0 FILAMENT_LIB_DIR)
endif()
message(STATUS "lidarscan: Filament libs from ${FILAMENT_LIB_DIR}")

# NOTE: the link list in Filament's own README is stale for v1.75 — libzstd is a
# hard dependency of libfilament (MaterialParser/ZstdHelper -> ZSTD_decompress)
# and omitting it fails at link time. S3 REPORT.md §2.3.
set(FILAMENT_LIBS
    filament backend bluegl bluevk filabridge filaflat utils geometry smol-v ibl abseil zstd)
set(FILAMENT_LINK "")
foreach(l ${FILAMENT_LIBS})
    if(EXISTS "${FILAMENT_LIB_DIR}/lib${l}.a")
        list(APPEND FILAMENT_LINK "${FILAMENT_LIB_DIR}/lib${l}.a")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Point material: matc from the same pinned release
# ---------------------------------------------------------------------------
set(MATC "${FILAMENT_DIR}/bin/matc")
if(APPLE)
    set(MATC_BACKENDS -a metal)
else()
    set(MATC_BACKENDS -a vulkan)
endif()
set(POINTS_MAT "${CMAKE_CURRENT_SOURCE_DIR}/materials/points.mat")
set(POINTS_FILAMAT "${CMAKE_CURRENT_BINARY_DIR}/points.filamat")
add_custom_command(
    OUTPUT "${POINTS_FILAMAT}"
    COMMAND "${MATC}" ${MATC_BACKENDS} -p desktop -o "${POINTS_FILAMAT}" "${POINTS_MAT}"
    DEPENDS "${POINTS_MAT}" "${MATC}"
    COMMENT "matc: points.mat -> points.filamat")
add_custom_target(lidarscan_points_material DEPENDS "${POINTS_FILAMAT}")

# ---------------------------------------------------------------------------
# Per-OS surface + display-link implementations (the S3-recommended shim)
# ---------------------------------------------------------------------------
if(APPLE)
    set(LIDARSCAN_PLATFORM_SOURCES
        src/render/NativeSurface_mac.mm
        src/render/DisplayLink_mac.mm)
elseif(WIN32)
    set(LIDARSCAN_PLATFORM_SOURCES
        src/render/NativeSurface_win.cpp
        src/render/DisplayLink_generic.cpp)
else()
    set(LIDARSCAN_PLATFORM_SOURCES
        src/render/NativeSurface_linux.cpp
        src/render/DisplayLink_generic.cpp)
endif()

add_executable(lidarscan
    src/main.cpp
    src/app/EngineHost.cpp
    src/app/Project.cpp
    src/app/ReplayController.cpp
    src/app/DisplayParamsDock.cpp
    src/app/CaptureWindow.cpp
    src/app/MainWindow.cpp
    src/render/NativeSurface.cpp
    src/render/DisplayLinkFallback.cpp
    src/render/PagedCloudRenderer.cpp
    src/render/ViewportWindow.cpp
    ${LIDARSCAN_PLATFORM_SOURCES}
)
add_dependencies(lidarscan lidarscan_points_material)

# QT_NO_KEYWORDS is REQUIRED, not stylistic: the engine's D6 parser has a member
# function called emit() (drivers/d6/d6_parser.h, kept byte-identical to the S1
# spike), and Qt's `emit` macro turns that declaration into a syntax error the
# moment a translation unit includes both. Every desktop source therefore uses
# Q_EMIT / Q_SIGNALS / Q_SLOTS. Any future Qt code linking libscanengine — B, C
# and the cloud tools alike — needs the same flag.
target_compile_definitions(lidarscan PRIVATE QT_NO_KEYWORDS)

target_include_directories(lidarscan PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FILAMENT_DIR}/include")

target_link_libraries(lidarscan PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialPort
    scanengine
    ${FILAMENT_LINK})

if(APPLE)
    target_link_libraries(lidarscan PRIVATE
        "-framework Cocoa" "-framework Metal" "-framework QuartzCore"
        "-framework CoreVideo" "-framework Foundation" "-framework IOKit")
    set_source_files_properties(
        src/render/NativeSurface_mac.mm src/render/DisplayLink_mac.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

if(MSVC)
    target_compile_options(lidarscan PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
    target_compile_options(lidarscan PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
