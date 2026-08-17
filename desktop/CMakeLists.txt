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

# ---------------------------------------------------------------------------
# APP VERSION — the repo-root VERSION file, and nothing else
# ---------------------------------------------------------------------------
#
# Owner rule (2026-08-17): every shipped update bumps ONE file, `<repo>/VERSION`,
# and every artefact derives from it. The Android app already does
# (android/app/build.gradle.kts reads the same file); this is the desktop half.
#
#   LIDARSCAN_VERSION       "0.2.1"  -> Info.plist CFBundleShortVersionString,
#                                       QCoreApplication::setApplicationVersion
#                                       (i.e. `--version`), the window title,
#                                       the NSIS/.deb metadata, and — via
#                                       tools/package_macos.sh, which reads the
#                                       BUILT plist — the DMG's file name.
#   LIDARSCAN_VERSION_CODE  201      -> Info.plist CFBundleVersion. The same
#                                       monotonic integer Android uses,
#                                       major*10000 + minor*100 + patch, so the
#                                       two stores can never disagree about
#                                       which build is newer.
#
# This is read BEFORE project(), which is allowed (file(READ) needs nothing but
# cmake_minimum_required) and is the only way project()'s VERSION can carry it.
# The engine's own version string (scanengine 0.1.0) is deliberately NOT touched:
# the engine versions independently of the apps that embed it.
set(LIDARSCAN_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
if(EXISTS "${LIDARSCAN_VERSION_FILE}")
    file(READ "${LIDARSCAN_VERSION_FILE}" LIDARSCAN_VERSION)
    string(STRIP "${LIDARSCAN_VERSION}" LIDARSCAN_VERSION)
else()
    # A desktop/ tree checked out on its own still has to configure. Say so
    # rather than silently shipping a made-up number.
    set(LIDARSCAN_VERSION "0.0.0")
    message(WARNING "no ${LIDARSCAN_VERSION_FILE}: building as version "
                    "${LIDARSCAN_VERSION}. Every shipped build must come from a "
                    "tree that has the repo-root VERSION file.")
endif()
if(NOT LIDARSCAN_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, got '${LIDARSCAN_VERSION}'")
endif()
math(EXPR LIDARSCAN_VERSION_CODE
     "${CMAKE_MATCH_1} * 10000 + ${CMAKE_MATCH_2} * 100 + ${CMAKE_MATCH_3}")
message(STATUS "LidarScan desktop version ${LIDARSCAN_VERSION} (code ${LIDARSCAN_VERSION_CODE})")

if(APPLE)
    project(lidarscan_desktop VERSION ${LIDARSCAN_VERSION} LANGUAGES CXX OBJCXX)
else()
    project(lidarscan_desktop VERSION ${LIDARSCAN_VERSION} LANGUAGES CXX)
endif()

# Reconfigure whenever VERSION changes, so a bump is picked up by an INCREMENTAL
# build instead of quietly shipping the previous number.
if(EXISTS "${LIDARSCAN_VERSION_FILE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${LIDARSCAN_VERSION_FILE}")
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
if(NOT CMAKE_PREFIX_PATH AND APPLE AND EXISTS "/opt/homebrew/opt/qt")
    set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
endif()
set(CMAKE_AUTOMOC ON)
# AUTORCC: resources/lidarscan.qrc carries the redesign's bundled typefaces
# (Space Grotesk / Inter / JetBrains Mono, all SIL OFL 1.1) and their licence
# texts. They are compiled INTO the binary rather than copied next to it so
# that the plain dev build, the .app bundle and the NSIS/AppImage installers
# all resolve ":/fonts/..." identically — src/ui/Theme.cpp is the only reader
# and it has no filesystem path to get wrong.
set(CMAKE_AUTORCC ON)
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
    if(MSVC)
        # S7/CI finding (first Windows configure): the WINDOWS release tarball is
        # laid out differently from the mac/linux ones in TWO ways.
        #   * it has no top-level `filament/` directory at all — it unpacks
        #     straight to bin/ include/ lib/ (tools/fetch_filament.ps1 therefore
        #     extracts it INTO third_party/filament/filament so FILAMENT_DIR is
        #     the same shape on all three platforms), and
        #   * its libs are lib/x86_64/{md,mdd}/<name>.lib — one set per CRT
        #     (md = /MD release, mdd = /MDd debug) with no `lib` prefix and no
        #     `.a` suffix. The mac/linux tarballs are lib/<arch>/lib<name>.a.
        # Picking the wrong CRT set is not a warning, it is a pile of
        # _ITERATOR_DEBUG_LEVEL / RuntimeLibrary mismatch link errors, so the
        # choice is tied to the CMake config, not guessed.
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/x86_64/mdd")
        else()
            set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/x86_64/md")
        endif()
    elseif(EXISTS "${FILAMENT_DIR}/lib/arm64" AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
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
    elseif(EXISTS "${FILAMENT_LIB_DIR}/${l}.lib")          # MSVC naming
        list(APPEND FILAMENT_LINK "${FILAMENT_LIB_DIR}/${l}.lib")
    endif()
endforeach()
list(LENGTH FILAMENT_LINK _fil_found)
if(_fil_found EQUAL 0)
    message(FATAL_ERROR
        "no Filament static libraries found in ${FILAMENT_LIB_DIR}. Expected "
        "lib<name>.a (mac/linux) or <name>.lib (MSVC). The release tarball "
        "layout differs per platform — see the comment above.")
endif()
list(LENGTH FILAMENT_LIBS _fil_wanted)
message(STATUS "lidarscan: ${_fil_found}/${_fil_wanted} Filament libs resolved from ${FILAMENT_LIB_DIR}")

# ---------------------------------------------------------------------------
# Point material: matc from the same pinned release
# ---------------------------------------------------------------------------
set(MATC "${FILAMENT_DIR}/bin/matc${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${MATC}")
    message(FATAL_ERROR "matc not found at ${MATC} — the Filament release under "
                        "third_party/ is incomplete (see tools/fetch_filament.*)")
endif()
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

# ---------------------------------------------------------------------------
# S3-cross-platform CI: compile-only mode
#
# The Linux Filament release is built against **libc++** (its libutils.a is full
# of `_ZNSt3__1...` symbols) while the Qt binaries and the distro toolchain are
# **libstdc++**. Those two C++ standard libraries do not share an ABI, so a final
# link of this app against the Linux prebuilt is not guaranteed to resolve — and
# the documented alternative (rebuild Filament from source with libstdc++) is a
# multi-hour job that does not belong in a per-push CI job.
#
# LIDARSCAN_COMPILE_ONLY=ON builds every desktop translation unit — the same
# sources, the same flags, the same Filament and Qt headers, the same moc run —
# into an OBJECT library and stops before the executable link. That proves the
# code compiles on the platform, which is exactly the half of the S3 exit
# criterion that was open, and it does not pretend to prove the other half.
# Default OFF: macOS and Windows link for real.
# ---------------------------------------------------------------------------
option(LIDARSCAN_COMPILE_ONLY
       "Compile every TU into an object library; skip the executable link" OFF)
if(LIDARSCAN_COMPILE_ONLY)
    set(LIDARSCAN_TARGET_KIND OBJECT)
else()
    set(LIDARSCAN_TARGET_KIND EXECUTABLE)
endif()

set(LIDARSCAN_SOURCES
    src/main.cpp
    resources/lidarscan.qrc
    src/ui/Theme.cpp
    src/ui/Icons.cpp
    src/ui/Widgets.cpp
    src/ui/IconRail.cpp
    src/ui/ViewportHost.cpp
    src/ui/InspectorCard.cpp
    src/ui/RecordCluster.cpp
    src/app/EngineHost.cpp
    src/app/Project.cpp
    src/app/ReplayController.cpp
    src/app/DisplayParamsDock.cpp
    src/app/CaptureWindow.cpp
    src/app/DeviceDiscovery.cpp
    src/app/DisplayAwake.cpp
    src/app/WalkSpeedEstimator.cpp
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

if(LIDARSCAN_COMPILE_ONLY)
    add_library(lidarscan OBJECT ${LIDARSCAN_SOURCES})
else()
    add_executable(lidarscan ${LIDARSCAN_BUNDLE_FLAG} ${LIDARSCAN_SOURCES})
endif()
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
# The app version reaches the binary from exactly one place: the repo-root
# VERSION file, via LIDARSCAN_VERSION above. main.cpp feeds it to
# QCoreApplication::setApplicationVersion(), which is what `--version` prints and
# what the window title shows.
target_compile_definitions(lidarscan PRIVATE
    LIDARSCAN_APP_VERSION="${LIDARSCAN_VERSION}"
    LIDARSCAN_APP_VERSION_CODE=${LIDARSCAN_VERSION_CODE})

if(WIN32)
    # _USE_MATH_DEFINES: ViewportWindow.cpp uses M_PI, which the MSVC CRT only
    #   declares when this is defined before <cmath> — a command-line define is
    #   the only way to get it in front of every include in every TU.
    # NOMINMAX: <windows.h> (pulled in by Filament's backend headers and by Qt's
    #   Windows QPA-facing headers) defines min/max as macros, which breaks
    #   std::min/std::max and QVector3D's members alike.
    # WIN32_LEAN_AND_MEAN: keeps winsock.h out, which would otherwise collide
    #   with the winsock2.h the vendored Livox SDK2 in the engine includes.
    target_compile_definitions(lidarscan PRIVATE
        _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

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
elseif(WIN32)
    # Filament's Vulkan/OpenGL backends resolve their loaders at runtime
    # (bluevk/bluegl LoadLibrary vulkan-1.dll / opengl32.dll), but the static
    # libs still reference the Win32 window/GDI entry points directly.
    target_link_libraries(lidarscan PRIVATE opengl32 gdi32 user32 shlwapi)
elseif(UNIX)
    # dl: bluevk dlopen()s libvulkan.so.1 rather than link-time-binding it.
    find_package(Threads REQUIRED)
    target_link_libraries(lidarscan PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

if(MSVC)
    target_compile_options(lidarscan PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8
        # /bigobj: MainWindow.cpp and MergeDock.cpp exceed the 65,279-section
        # COFF limit once Qt's moc'd metaobjects and Filament's heavily
        # templated headers are both in the same TU.
        /bigobj)
else()
    target_compile_options(lidarscan PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
