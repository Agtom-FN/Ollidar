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
    project(lidarscan_desktop VERSION 0.1.0 LANGUAGES CXX OBJCXX)
else()
    project(lidarscan_desktop VERSION 0.1.0 LANGUAGES CXX)
endif()

# C8: the single source of the version string that reaches Info.plist,
# the NSIS installer and the .deb/AppImage metadata.
set(LIDARSCAN_VERSION "${PROJECT_VERSION}")

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
# Network: C4's "Submit to cloud…" dialog makes a REAL QNetworkAccessManager
# request against A15's cloud REST contract (see src/app/QtHttpTransport.*) —
# the task is explicit that the scripted fake HttpTransport A15's own tests
# use "is not for UI", so this has to be a real socket attempt that fails
# gracefully when (as in this environment) no server exists.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort Network)

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
#
# C8: a *universal* macOS build needs universal Filament static libs, and the
# v1.75.0 mac release tarball ships lib/arm64 ONLY (NOTES.md §3.2 — the C8
# blocker C1 flagged). tools/build_filament_x86_64.sh builds the missing x86_64
# slice from source and tools/make_universal_filament.sh lipos the two together
# into third_party/filament-universal/. When this configure asks for both
# architectures, that tree is REQUIRED — falling back to the arm64-only
# prebuilt would link (with warnings) and produce an app whose Intel slice is
# missing every Filament symbol.
# ---------------------------------------------------------------------------
set(LIDARSCAN_MAC_UNIVERSAL OFF)
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    set(LIDARSCAN_MAC_UNIVERSAL ON)
endif()

if(LIDARSCAN_MAC_UNIVERSAL)
    set(FILAMENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/filament-universal/filament")
    if(NOT EXISTS "${FILAMENT_DIR}/lib/universal/libfilament.a")
        message(FATAL_ERROR
            "CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}' asks for a universal "
            "build, but universal Filament libs are not present at\n"
            "  ${FILAMENT_DIR}/lib/universal\n"
            "Build them (measured ~7 min once: 3 min clone + 2 min build):\n"
            "  ./tools/fetch_filament.sh v1.75.0            # arm64 slice (prebuilt)\n"
            "  ./tools/build_filament_x86_64.sh v1.75.0     # x86_64 slice (from source)\n"
            "  ./tools/make_universal_filament.sh           # lipo the two together")
    endif()
    set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/universal")
else()
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
endif()
if(NOT EXISTS "${FILAMENT_DIR}/include/filament/Engine.h")
    message(FATAL_ERROR "Filament headers not found at ${FILAMENT_DIR}/include")
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
# C8: macOS .app bundle
#
# Default OFF, deliberately: scripts/verify*.sh (C1-C7's whole evidence
# corpus) invoke `build/lidarscan`, and flipping the default would move the
# executable to build/lidarscan.app/Contents/MacOS/lidarscan and silently
# break all four of them. tools/package_macos.sh turns it ON.
#
# What the bundle changes, concretely:
#   * points.filamat moves from next-to-the-binary into Contents/Resources.
#     ViewportWindow.cpp searches both (NOTES.md §7 called that one line out as
#     C8's to change) so one binary works in both layouts.
#   * packaging/Info.plist.in (written by C7) becomes the real Info.plist, which
#     is what makes LaunchServices offer this app for a .lscan.zip at all — a
#     bare Mach-O is never a document handler no matter what QEvent::FileOpen
#     handling MainWindow has.
# ---------------------------------------------------------------------------
option(LIDARSCAN_MACOS_BUNDLE "Build LidarScan.app instead of a plain binary" OFF)
set(LIDARSCAN_BUNDLE_FLAG "")
set(LIDARSCAN_EXECUTABLE "lidarscan")
if(APPLE AND LIDARSCAN_MACOS_BUNDLE)
    set(LIDARSCAN_BUNDLE_FLAG MACOSX_BUNDLE)
    set(LIDARSCAN_EXECUTABLE "LidarScan")
    # configure_file WITHOUT @ONLY: Info.plist.in uses ${LIDARSCAN_VERSION}
    # syntax (C7 wrote it that way and documented the choice), which @ONLY
    # would leave un-substituted.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
endif()

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
    ${LIDARSCAN_BUNDLE_FLAG}
    src/main.cpp
    src/app/EngineHost.cpp
    src/app/Project.cpp
    src/app/ReplayController.cpp
    src/app/DisplayParamsDock.cpp
    src/app/CaptureWindow.cpp
    src/app/MeasureDock.cpp
    src/app/ExportDialog.cpp
    src/app/ProcessingDock.cpp
    src/app/PlanDock.cpp
    src/app/QtHttpTransport.cpp
    src/app/SyntheticMid360.cpp
    src/app/SyntheticBuilding.cpp
    src/app/MergeFixture.cpp
    src/app/MergeSessionLoader.cpp
    src/app/MergeDock.cpp
    src/app/TransferDialog.cpp
    src/app/MainWindow.cpp
    src/render/NativeSurface.cpp
    src/render/DisplayLinkFallback.cpp
    src/render/PagedCloudRenderer.cpp
    src/render/ViewportWindow.cpp
    ${LIDARSCAN_PLATFORM_SOURCES}
)
add_dependencies(lidarscan lidarscan_points_material)

if(APPLE AND LIDARSCAN_MACOS_BUNDLE)
    # OUTPUT_NAME drives BOTH the .app directory name and the Mach-O inside it,
    # so it is "LidarScan" (the user-visible name in Finder / the Dock / the
    # Applications folder) and Info.plist.in's CFBundleExecutable is filled from
    # the same variable — they cannot drift apart.
    set_target_properties(lidarscan PROPERTIES
        OUTPUT_NAME "${LIDARSCAN_EXECUTABLE}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
        MACOSX_BUNDLE_BUNDLE_NAME "LidarScan")
    # points.filamat is a generated file, so it cannot go in add_executable()'s
    # source list with MACOSX_PACKAGE_LOCATION (the copy would race the matc
    # custom command). A POST_BUILD copy is ordered after both.
    add_custom_command(TARGET lidarscan POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "${POINTS_FILAMAT}"
                "$<TARGET_BUNDLE_CONTENT_DIR:lidarscan>/Resources/points.filamat"
        COMMENT "bundle: points.filamat -> Contents/Resources"
        VERBATIM)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/lidarscan.icns")
        add_custom_command(TARGET lidarscan POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/lidarscan.icns"
                    "$<TARGET_BUNDLE_CONTENT_DIR:lidarscan>/Resources/lidarscan.icns"
            COMMENT "bundle: lidarscan.icns -> Contents/Resources"
            VERBATIM)
    else()
        message(STATUS "lidarscan: no packaging/macos/lidarscan.icns — the bundle "
                       "will use the generic app icon (run tools/make_icon.sh)")
    endif()
endif()

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
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::SerialPort Qt6::Network
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
