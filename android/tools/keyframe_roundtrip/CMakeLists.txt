# android/tools/keyframe_roundtrip — a HOST-buildable round-trip test for the
# keyframe writer the JNI shim ships (app/src/main/cpp/keyframe_writer.cpp).
#
# WHY IT EXISTS. The B8 brief asks for "a replay-based test of the JNI
# keyframe writer (write N synthetic keyframes -> read the .lscan back)".
# There is no device or emulator in this environment (Tech Spec's 2026-08-15
# hardware-absent addendum), so an instrumentation test could be written but
# never run — and an unrun test is not verification. This tool compiles the
# EXACT SAME keyframe_writer.cpp the .so contains, on the host, links the same
# `scanengine` static library, writes N synthetic keyframes into a temporary
# .lscan directory and reads them back through the engine's own
# `color::read_frame_index()`. That is the whole writer path minus the JNI
# marshalling layer, and it actually runs.
#
# What it therefore does NOT cover, stated plainly: the JNI entry points in
# arcore_jni.cpp (argument marshalling, the cached-class lookups), and
# anything Android-specific. Those stay device-deferred like every other JNI
# signature in this app (see android/NOTES.md).
#
# Build + run (from android/tools/keyframe_roundtrip):
#   cmake -S . -B <build-dir> -DCMAKE_BUILD_TYPE=Release
#   cmake --build <build-dir> --target keyframe_roundtrip
#   <build-dir>/keyframe_roundtrip <scratch-dir>
cmake_minimum_required(VERSION 3.22)
project(keyframe_roundtrip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENGINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENGINE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ENGINE_WITH_LIVOX_SDK2 OFF CACHE BOOL "" FORCE)
set(ENGINE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

get_filename_component(LIDARSCAN_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../engine" ABSOLUTE)
if(NOT EXISTS "${LIDARSCAN_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "keyframe_roundtrip: expected the engine at ${LIDARSCAN_ENGINE_DIR}")
endif()
add_subdirectory("${LIDARSCAN_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/scanengine")

add_executable(keyframe_roundtrip
    main.cpp
    ../../app/src/main/cpp/keyframe_writer.cpp
)
target_include_directories(keyframe_roundtrip PRIVATE ../../app/src/main/cpp)
target_link_libraries(keyframe_roundtrip PRIVATE scanengine)
