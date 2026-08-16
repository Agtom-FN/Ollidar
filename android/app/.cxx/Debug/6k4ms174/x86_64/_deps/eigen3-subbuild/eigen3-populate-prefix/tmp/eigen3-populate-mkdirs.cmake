# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-src")
  file(MAKE_DIRECTORY "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-src")
endif()
file(MAKE_DIRECTORY
  "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-build"
  "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix"
  "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix/tmp"
  "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix/src/eigen3-populate-stamp"
  "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix/src"
  "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix/src/eigen3-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix/src/eigen3-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/admin/lidarscan/android/app/.cxx/Debug/6k4ms174/x86_64/_deps/eigen3-subbuild/eigen3-populate-prefix/src/eigen3-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
