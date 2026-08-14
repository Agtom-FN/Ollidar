# Overlay vcpkg triplet for a single-pass macOS universal (arm64 + x86_64)
# build, per Tech Spec §3 rule 4 ("macOS arm64 and x86_64 (universal)").
#
# vcpkg has no built-in "universal-osx" triplet; VCPKG_OSX_ARCHITECTURES is a
# documented, officially supported triplet variable
# (https://learn.microsoft.com/en-us/vcpkg/users/triplets#vcpkg_osx_architectures,
# checked 2026-08-14) that maps straight to CMake's own
# CMAKE_OSX_ARCHITECTURES, so any port whose build system is plain CMake
# (Eigen, and later GTSAM/Boost/METIS) picks this up for free.
#
# Caveat (see TOOLCHAIN_NOTES.md): autotools/configure-based ports run a
# configure-time compile-and-execute check that can't work for two
# architectures in one invocation, so they typically FAIL under this
# triplet. None of engine-stub's dependencies are autotools-based today;
# when A1 adds GTSAM's transitive Boost, confirm each required Boost
# component's vcpkg portfile is CMake-based (most are) before assuming this
# triplet "just works" for it too -- verify per-port, don't assume.

set(VCPKG_TARGET_ARCHITECTURE arm64)  # nominal; VCPKG_OSX_ARCHITECTURES below is what actually governs the build
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)

set(VCPKG_OSX_ARCHITECTURES "arm64;x86_64")
set(VCPKG_OSX_DEPLOYMENT_TARGET "12.0")
