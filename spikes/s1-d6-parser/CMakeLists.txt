cmake_minimum_required(VERSION 3.16)
project(s1_d6_parser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(D6_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# ---------------------------------------------------------------------------
# d6 -- the portable parser. No platform code, no dependencies. This target is
# what will be lifted into engine/drivers/d6/ in workstream A2.
# ---------------------------------------------------------------------------
add_library(d6 STATIC
  d6/d6_parser.cpp
  d6/d6_parser.h
  d6/commands.h
)
target_include_directories(d6 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/d6)

if(MSVC)
  target_compile_options(d6 PRIVATE /W4)
else()
  target_compile_options(d6 PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()
if(D6_WARNINGS_AS_ERRORS)
  if(MSVC)
    target_compile_options(d6 PRIVATE /WX)
  else()
    target_compile_options(d6 PRIVATE -Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(d6_tests tests/test_d6.cpp)
target_link_libraries(d6_tests PRIVATE d6)
target_include_directories(d6_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME d6_tests COMMAND d6_tests)

# ---------------------------------------------------------------------------
# tools (platform code lives here only)
# ---------------------------------------------------------------------------
add_executable(d6cli tools/d6cli.cpp)
target_link_libraries(d6cli PRIVATE d6)

add_executable(d6synth tools/d6synth.cpp)
target_link_libraries(d6synth PRIVATE d6)
target_include_directories(d6synth PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
