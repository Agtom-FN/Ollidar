// eigen_demo.hpp — proves the real dependency-resolution path end to end:
// Eigen is fetched/found through the package manager (vcpkg manifest mode
// via the CMake presets; FetchContent as a no-vcpkg local fallback — see
// CMakeLists.txt), linked into `scanengine`, and used from real code. This
// stands in for the real engine's much heavier use of Eigen throughout
// poses/, slam/, merge/.
#pragma once

namespace scanengine {

// Solves a tiny 3x3 linear least-squares problem with Eigen and returns the
// residual norm. Purely a smoke test that Eigen headers + its CMake config
// package resolve and compile on every target triplet.
double eigen_smoke_test();

}  // namespace scanengine
