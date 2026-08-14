cmake_minimum_required(VERSION 3.20)
project(s3_render LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Qt 6 -------------------------------------------------------------------
# brew install qt  ->  /opt/homebrew/opt/qt
if(NOT CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt")
endif()
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# ---- Filament (prebuilt release, third_party/filament) ----------------------
set(FILAMENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/filament/filament")
if(NOT EXISTS "${FILAMENT_DIR}/include/filament/Engine.h")
  message(FATAL_ERROR "Filament not found at ${FILAMENT_DIR}. Run tools/fetch_filament.sh")
endif()
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/arm64")
else()
  set(FILAMENT_LIB_DIR "${FILAMENT_DIR}/lib/x86_64")
endif()

# NOTE: the list in Filament's own README is stale for v1.75 -- libzstd is a new
# hard dependency of libfilament (MaterialParser/ZstdHelper).
set(FILAMENT_LIBS
    filament backend bluegl bluevk filabridge filaflat utils geometry smol-v ibl abseil zstd)
set(FILAMENT_LINK "")
foreach(l ${FILAMENT_LIBS})
  list(APPEND FILAMENT_LINK "${FILAMENT_LIB_DIR}/lib${l}.a")
endforeach()

# ---- material compilation (matc from the same release) ----------------------
set(MATC "${FILAMENT_DIR}/bin/matc")
set(POINTS_MAT "${CMAKE_CURRENT_SOURCE_DIR}/materials/points.mat")
set(POINTS_FILAMAT "${CMAKE_CURRENT_BINARY_DIR}/points.filamat")
add_custom_command(
  OUTPUT "${POINTS_FILAMAT}"
  COMMAND "${MATC}" -a metal -p desktop -o "${POINTS_FILAMAT}" "${POINTS_MAT}"
  DEPENDS "${POINTS_MAT}" "${MATC}"
  COMMENT "matc: points.mat -> points.filamat (Metal)")
add_custom_target(points_material DEPENDS "${POINTS_FILAMAT}")

# ---- app --------------------------------------------------------------------
add_executable(s3_render
  src/main.cpp
  src/FilamentWindow.cpp
  src/PagedCloud.cpp
  src/PointSource.cpp
  src/MacBridge.mm)
add_dependencies(s3_render points_material)

target_include_directories(s3_render PRIVATE "${FILAMENT_DIR}/include" src)
target_link_libraries(s3_render PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets
  ${FILAMENT_LINK}
  "-framework Cocoa" "-framework Metal" "-framework QuartzCore" "-framework CoreVideo"
  "-framework Foundation" "-framework IOKit")

set_source_files_properties(src/MacBridge.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
target_compile_options(s3_render PRIVATE -O2 -Wall -Wno-unused-parameter)
