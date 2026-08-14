cmake_minimum_required(VERSION 3.31.6-g38307f9)
set(CMAKE_MODULE_PATH "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/arm64-v8a/_deps/eigen3-src/cmake")
project(CheckFortran Fortran)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/result.cmake"
  "set(CMAKE_Fortran_COMPILER \"${CMAKE_Fortran_COMPILER}\")\n"
  
  )