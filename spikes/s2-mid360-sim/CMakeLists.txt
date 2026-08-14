cmake_minimum_required(VERSION 3.16)
project(s2_mid360_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(SDK2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Livox-SDK2)
if(NOT EXISTS ${SDK2_DIR}/CMakeLists.txt)
  message(FATAL_ERROR "Livox-SDK2 missing. Run scripts/fetch_sdk2.sh first.")
endif()

# Builds livox_lidar_sdk_static / _shared plus the upstream samples.
add_subdirectory(${SDK2_DIR} livox_sdk2)

# ---------------------------------------------------------------------------
# mid360_sim — the lidar-side simulator. Deliberately does NOT link the SDK:
# it re-implements the wire format from the protocol spec so that a mistake in
# the SDK's view of the protocol cannot be masked by sharing its code. The only
# thing it borrows is the SDK's FastCRC, so CRC bytes are bit-identical.
# ---------------------------------------------------------------------------
add_executable(mid360_sim
  sim/mid360_sim.cpp
  ${SDK2_DIR}/3rdparty/FastCRC/FastCRCsw.cpp
)
target_include_directories(mid360_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/sim
  ${SDK2_DIR}/include
  ${SDK2_DIR}/3rdparty
)
target_compile_options(mid360_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(mid360_sim PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# lvx2_replay — streams a real Livox .lvx2 recording as real-time-paced UDP
# packets in the same wire format mid360_sim emits (S2 follow-up, for A6).
# Deliberately does NOT link the SDK, for the same reason mid360_sim doesn't.
# ---------------------------------------------------------------------------
add_executable(lvx2_replay
  replay/lvx2_replay.cpp
  ${SDK2_DIR}/3rdparty/FastCRC/FastCRCsw.cpp
)
target_include_directories(lvx2_replay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/sim
  ${SDK2_DIR}/include
  ${SDK2_DIR}/3rdparty
)
target_compile_options(lvx2_replay PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(lvx2_replay PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# sdk_client_demo — the real SDK2 client, used the way engine task A3 will.
# ---------------------------------------------------------------------------
add_executable(sdk_client_demo demo/sdk_client_demo.cpp)
target_link_libraries(sdk_client_demo PRIVATE livox_lidar_sdk_static Threads::Threads)
target_compile_options(sdk_client_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# wire_selftest — proves the simulator's frame builder round-trips through the
# SDK's own parser/CRC before any socket is involved.
# ---------------------------------------------------------------------------
add_executable(wire_selftest
  tests/wire_selftest.cpp
  ${SDK2_DIR}/3rdparty/FastCRC/FastCRCsw.cpp
  ${SDK2_DIR}/sdk_core/comm/sdk_protocol.cpp
)
target_include_directories(wire_selftest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/sim
  ${SDK2_DIR}/include
  ${SDK2_DIR}/3rdparty
  ${SDK2_DIR}/sdk_core
)
target_compile_options(wire_selftest PRIVATE -Wall -Wextra -Wno-unused-parameter)

enable_testing()
add_test(NAME wire_selftest COMMAND wire_selftest)
