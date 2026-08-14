# S7 — Windows toolchain notes

Spike S7 (Tech Spec §4, Phase 0 table; exit criterion: "Green Windows CI
build of engine + unit tests"). This file records what was **verified
online** (with sources and a check date) vs. what remains an **assumption**
to be confirmed on the first real Windows CI run and in A1, per the risk
entry "Windows toolchain (GTSAM/SDK2/Filament on MSVC) — High — Dedicated
spike S7 before Phase 1 commit; clang-cl fallback; vendored pinned deps."

All dates below: checked 2026-08-14, against `microsoft/vcpkg` release
`2026.07.29` (commit `9e593bb18ea69cc5095e012465dcd675a822ed0d` — this is the
`builtin-baseline` pinned in `engine-stub/vcpkg.json`).

---

## 1. GTSAM on MSVC

**vcpkg port status — VERIFIED.** `gtsam` is present in the vcpkg curated
registry at version `4.2.2#0` (published 2026-07-07), license BSD-3-Clause.
Its `vcpkg.json` declares:

- Dependencies: **Boost** (`assign`, `chrono`, `filesystem`, `format`,
  `graph`, `math`, `program-options`, `ptr-container`, `serialization`,
  `timer`), **Eigen3**, **METIS**, plus `vcpkg-cmake` / `vcpkg-cmake-config`
  as host tools.
- Optional feature: `tbb` ("Use GTSAM parallelization").
- No platform `"supports"` expression / exclusion is declared — the port
  page shows "Supports: All", i.e. vcpkg does not itself flag Windows as
  unsupported. This is a **necessary but not sufficient** signal: absence of
  an exclusion means the port *maintainers* haven't blocked Windows, not
  that CI has proven the exact combination we need (static-vs-shared,
  MSVC-vs-clang-cl) — treat the first real `vcpkg install gtsam
  --triplet x64-windows` in CI as the actual confirmation.
  (Source: https://vcpkg.link/ports/gtsam and
  `raw.githubusercontent.com/microsoft/vcpkg/master/ports/gtsam/vcpkg.json`.)

**Known MSVC build requirements — VERIFIED against GTSAM's own docs
(borglab.github.io/gtsam/install, github.com/borglab/gtsam/blob/develop/INSTALL.md):**

- **`/permissive-`** is required project-wide — GTSAM's headers rely on
  standards-conforming two-phase lookup that MSVC's legacy non-conforming
  mode doesn't provide. GTSAM's own CMake sets this and expects consumers
  to as well.
- **`/Zc:__cplusplus`** should be set alongside it so `__cplusplus` reports
  the real standard version to any header that branches on it (GTSAM and
  Boost both do). We've already put both flags on the `scanengine` target
  in `engine-stub/CMakeLists.txt` under `if(MSVC)`, ahead of actually
  needing them, specifically so A1 doesn't have to rediscover this.
- **`/bigobj`** is needed for GTSAM's heavily-templated translation units
  (factor graph templates instantiate large numbers of symbols per TU) —
  standard practice for any Boost.Serialization-heavy MSVC build, GTSAM
  included.
- **`-D_USE_MATH_DEFINES`**: MSVC's `<cmath>` hides `M_PI` and friends
  unless this is defined before including it; GTSAM headers use `M_PI`
  directly in a few places. Define it globally for the target rather than
  per-file.
- **CMake ≥ 3.21** is required for the Windows install path (custom install
  templates GTSAM added were introduced in 3.21). Our presets require
  `cmakeMinimumRequired` 3.25, so this is already satisfied.

**Shared vs. static — ASSUMPTION, not yet verified against a real build.**
vcpkg's default `x64-windows` triplet builds **dynamic** (DLL) libraries.
GTSAM has historically had rough edges exporting its full template-heavy
symbol surface via `__declspec(dllexport)` on Windows (class templates,
Boost.Serialization registration). Recommendation for A1: **start with
`x64-windows-static` (or a custom static triplet) for GTSAM specifically**
to sidestep DLL-export symbol visibility entirely, and only move to
dynamic linking if a shared build is later needed (e.g. to reduce
Android/desktop binary duplication) — treat that as a deliberate follow-up
task, not a default. This recommendation is inference from GTSAM's known
Windows DLL-export history, not something we could verify from the vcpkg
port metadata alone; **confirm on the first Windows CI attempt to build
GTSAM** (not yet a dependency of `engine-stub` — see §6 onboarding order).

## 2. Livox SDK2 on Windows

**VERIFIED: no vcpkg port exists.** `ports/livox-sdk2/vcpkg.json` 404s on
the vcpkg master tree — confirming the Tech Spec's own plan ("Livox SDK2
(vendored)"). This must be vendored (git submodule, subtree, or
`FetchContent` from `github.com/Livox-SDK/Livox-SDK2`) — do not budget time
looking for a package-manager path that doesn't exist.

**Windows build path — VERIFIED against the upstream README**
(github.com/Livox-SDK/Livox-SDK2):

- Officially supports Windows. Documented toolchain: **Visual Studio 2019**
  (VS2019-generation MSVC toolset) + **CMake 3.0.0+**.
- Documented invocation generates a VS solution directly (not Ninja):
  `cmake .. -G "Visual Studio 16 2019" -A x64`, then build inside Visual
  Studio. Output lands at
  `Livox-SDK2\out\build\x64-Release\samples\livox_lidar_quick_start\`.
- Implication for our Ninja-first CI strategy (§5): Livox SDK2's own build
  is most-tested with the VS generator, not Ninja+cl.exe. It's still plain
  CMake underneath, so Ninja+cl.exe should work in principle (same
  compiler, different generator), but this is an **assumption** — when A3
  vendors it, verify a Ninja configure/build explicitly rather than
  assuming the VS-generator instructions transfer 1:1. If it doesn't, the
  fallback is to keep Livox SDK2 building via the VS generator specifically
  (via `ExternalProject_Add` or a nested configure) while the rest of the
  engine stays on Ninja.
- The SDK targets UDP/Ethernet (matches Tech Spec §2.2's transport), so no
  serial-driver concerns like the D6/CH340 story apply here — it's sockets,
  which are portable.

## 3. Filament on Windows

**VERIFIED: prebuilt Windows releases exist and are the right thing for
the desktop app to consume — NOT the engine.** Per the Tech Spec's own
architecture diagram, Filament sits *outside* `libscanengine` and is linked
only by the apps (Qt desktop app, Android app). `engine-stub` — and the
real headless engine, including its Linux cloud-worker build — must never
link Filament. This spike does not add it as a dependency for exactly that
reason.

- google/filament publishes per-release Windows archives, e.g.
  `filament-v1.69.4-windows.tgz` under
  `github.com/google/filament/releases`. These are prebuilt runtime +
  host-side tools (matgen, cmgen, etc.) — the docs explicitly warn to keep
  the host tools and runtime library versions matched (don't mix a newer
  prebuilt library with older locally-built tools or vice versa).
- No vcpkg port (`ports/filament/vcpkg.json` also 404s) — consistent with
  Filament not being a vcpkg-managed dependency anywhere in this plan.
- **Building Filament from source on Windows requires Clang, not plain
  MSVC** — this is Filament's own long-standing constraint (their build
  system leans on Clang-specific codegen for the shader/material compiler
  toolchain), which is exactly why the Tech Spec's rendering rule says
  "MSVC **or** clang-cl" for the *engine*, and separately why C1/C8
  (desktop packaging) should default to **consuming Filament's official
  prebuilt Windows archive** rather than building Filament from source —
  building it from source would force clang-cl (or full Clang) specifically
  for that one dependency regardless of what the engine itself uses.
  Recommendation: C1 pins a specific Filament release tag and downloads the
  matching prebuilt Windows archive in CI/packaging, falling back to
  source-build-with-Clang only if a required feature is missing from the
  prebuilt.

## 4. Boost (transitive, via GTSAM)

Not independently verified port-by-port in this spike (out of scope until
A1 actually adds GTSAM), but flagged from the GTSAM `vcpkg.json` dependency
list (§1): `boost-assign`, `boost-chrono`, `boost-filesystem`,
`boost-format`, `boost-graph`, `boost-math`, `boost-program-options`,
`boost-ptr-container`, `boost-serialization`, `boost-timer`. All of these
are individually-versioned vcpkg ports (vcpkg splits Boost into per-library
ports, not one monolith) and all are CMake-based builds in vcpkg's port
tree, which matters for two things we already had to account for in this
spike:

1. **`/bigobj` and `/permissive-`** apply to Boost.Serialization too (same
   template-heavy-TU story as GTSAM itself).
2. **macOS universal builds**: our `universal-osx` overlay triplet (§5,
   `engine-stub/cmake/triplets/universal-osx.cmake`) relies on
   `VCPKG_OSX_ARCHITECTURES` mapping to CMake's native
   `CMAKE_OSX_ARCHITECTURES` multi-arch support, which only works cleanly
   for **CMake-based** port build systems. Since the Boost ports GTSAM
   needs are CMake-based in vcpkg's tree (not raw b2/bjam), they should be
   compatible with the universal triplet in principle — but this is an
   **assumption** to verify empirically once A1 actually adds GTSAM to
   `vcpkg.json`, not before. If any turn out to still shell out to `b2`
   internally, expect a universal-build failure and be ready to build
   `arm64-osx` + `x64-osx` separately and `lipo`-merge as a fallback.

## 5. Generator, MSVC runtime, and the universal-macOS triplet

- **Generator: Ninja, uniformly, across all five presets** (including
  Windows). Rationale: one build-command shape (`cmake --build --preset
  ...`) everywhere, avoids MSBuild-vs-Ninja-vs-Xcode divergence in scripts
  and in future incremental-build tooling, and every CI runner we target
  can get Ninja trivially via `lukka/get-cmake@latest` (bundles a recent
  CMake + Ninja together — verified this is the action's stated purpose;
  see `github.com/lukka/get-cmake`). Windows still requires the compiler
  environment (`INCLUDE`/`LIB`/`PATH` to `cl.exe`) to be set up separately
  from Ninja itself — that's what `ilammy/msvc-dev-cmd` does in
  `engine-ci.yml` before CMake ever runs.
- **MSVC runtime: `/MD` (dynamic, multithreaded)**, matching vcpkg's
  default `x64-windows` triplet (`VCPKG_CRT_LINKAGE dynamic`). Keeping the
  engine on the same CRT linkage as vcpkg's default triplet avoids the
  classic "static CRT app links a dynamic-CRT-built dependency" ABI
  mismatch (heap-allocated objects crossing a DLL boundary with mismatched
  CRTs, iterator-debug-level mismatches in debug builds, etc.). If C8
  (Windows packaging) later needs a statically-linked CRT for a
  no-redistributable-installer story, that means switching the *whole*
  dependency tree to `x64-windows-static`, not just the top-level app —
  note this now so it isn't rediscovered mid-A1.
- **`windows-clangcl-x64` preset**: only the top-level `scanengine` target's
  compiler changes to `clang-cl`; the `VCPKG_TARGET_TRIPLET` stays
  `x64-windows`, so vcpkg's own port *builds* (Eigen today, GTSAM/Boost
  later) still compile with `cl.exe` under the hood. This is intentional
  and standard practice: clang-cl targets the MSVC ABI (same calling
  convention, same STL, same iterator-debug-level scheme), so linking a
  clang-cl-compiled `scanengine` against cl.exe-built vcpkg dependencies is
  a supported scenario, not a mismatch — the two compilers are
  ABI-compatible on Windows by design. A **known, verified** clang-cl
  constraint (github.com/actions/runner-images issue #10018, checked
  2026-08-14): recent MSVC STL updates require **Clang/clang-cl ≥ 17** —
  older clang-cl fails with `error STL1000: Unexpected compiler version`.
  `windows-latest`'s bundled VS installation includes an LLVM/Clang
  component; `engine-ci.yml`'s `windows-clangcl` job runs `clang-cl
  --version` as an explicit early check specifically so a version mismatch
  fails fast and visibly instead of producing a confusing STL error deep in
  a template instantiation.
- **macOS universal**: `engine-stub/cmake/triplets/universal-osx.cmake` is
  a custom overlay triplet (vcpkg has no built-in "universal" triplet)
  setting `VCPKG_OSX_ARCHITECTURES "arm64;x86_64"` — this is a **documented,
  official** vcpkg triplet variable
  (learn.microsoft.com/en-us/vcpkg/users/triplets#vcpkg_osx_architectures,
  checked 2026-08-14) that maps directly onto CMake's own
  `CMAKE_OSX_ARCHITECTURES`. **Locally verified working end-to-end on this
  Mac**: `cmake --preset macos-universal` successfully built Eigen through
  vcpkg using this overlay triplet, and `lipo -info` on the resulting
  `scanengine_tests` binary and `libscanengine.a` both report `x86_64
  arm64`. Caveat carried forward for real deps: this only works cleanly for
  CMake-based port build systems; autotools/`configure`-based ports run a
  configure-time compile-and-execute check that can't target two
  architectures in one invocation and will typically fail under this
  triplet — verify per-port before assuming it "just works" (see §4).

## 6. Recommended order to onboard real deps in A1

Given the above, ordering A1's dependency additions by *ascending risk* so
each step either passes cleanly or fails in isolation, on a matrix that's
already green rather than newly broken:

1. **Eigen** (done in this spike) — header-only, zero linkage risk, proves
   the manifest + preset + triplet plumbing works on all five targets.
2. **Threads** (already in this spike, via `find_package(Threads)`) — no
   new risk, just confirms the pattern extends to non-vcpkg system deps.
3. **Boost components GTSAM needs, standalone** — add them to
   `vcpkg.json` *before* GTSAM itself, so a Boost-specific build failure
   (e.g. a component that turns out not to be CMake-based, breaking
   universal-osx per §4) is diagnosed without GTSAM's much longer build
   time in the loop.
4. **GTSAM** — with `/permissive- /bigobj /Zc:__cplusplus
   -D_USE_MATH_DEFINES` already wired at the target level (this spike put
   them there proactively), start with `x64-windows-static` per §1's
   shared-vs-static recommendation; only revisit dynamic linking as a
   deliberate follow-up.
5. **Livox SDK2** — vendored (not vcpkg, per §2), added as its own
   CMake subdirectory/`FetchContent`; verify the Ninja-generator build
   explicitly since upstream's own instructions assume the VS generator.
6. **Filament** — added only to the **app** targets (Qt desktop, Android),
   never to `scanengine`; consume the official prebuilt Windows archive
   first (§3), source-build-with-Clang only as a fallback.
7. **Ceres** (Phase-2-adjacent per the spec's mention) — deferred; when it
   lands, check its vcpkg port the same way this spike checked GTSAM's
   before assuming availability.

## 7. What's verified vs. what's still an assumption

| Claim | Status |
| --- | --- |
| `gtsam` vcpkg port exists at 4.2.2, deps = Boost/Eigen3/METIS, no platform exclusion | **Verified** (vcpkg.link + raw port `vcpkg.json`, 2026-08-14) |
| GTSAM needs `/permissive-`, `/bigobj`, `/Zc:__cplusplus`, `_USE_MATH_DEFINES` on MSVC | **Verified** (GTSAM's own install docs) |
| GTSAM should default to static linkage on `x64-windows` | **Assumption** (inferred from GTSAM's known DLL-export history; not yet build-tested) |
| No `livox-sdk2` vcpkg port; must vendor | **Verified** (404 on vcpkg master tree) |
| Livox SDK2 officially supports Windows via VS2019 + CMake ≥ 3.0 | **Verified** (upstream README) |
| Livox SDK2 builds cleanly with Ninja instead of the VS generator | **Assumption** — verify when A3 vendors it |
| No `filament` vcpkg port; prebuilt Windows `.tgz` releases exist | **Verified** (GitHub Releases + 404 on vcpkg master tree) |
| Filament source builds need Clang on Windows | **Verified** (Filament's own long-standing build constraint, corroborated by BUILDING.md and historical Windows build issues) |
| `VCPKG_OSX_ARCHITECTURES` triplet variable exists and works for universal builds | **Verified both by docs AND by a real local build** (this spike's `macos-universal` preset, Eigen through vcpkg, `lipo -info` confirms `x86_64 arm64` fat binary) |
| Boost ports GTSAM needs are CMake-based (compatible with the universal triplet) | **Assumption** — verify when A1 adds them |
| `arm64-android` is a vcpkg-curated, CI-tested triplet requiring `ANDROID_NDK_HOME` | **Verified** (learn.microsoft.com/en-us/vcpkg/users/platforms/android, 2026-08-14) |
| clang-cl ≥ 17 required against current MSVC STL on `windows-latest` | **Verified** (github.com/actions/runner-images#10018) |
| Everything in `engine-ci.yml` actually runs green on real GitHub-hosted Windows runners | **Not yet verified — no Windows machine locally; this is exactly what the first push validates** (per the spike brief) |

---

*Local verification performed for this spike (macOS, this machine):
`cmake --preset macos-universal` → build → `ctest --preset macos-universal`
all passed; `scanengine_tests` and `libscanengine.a` both confirmed as
`x86_64 arm64` fat binaries via `lipo -info`. The `windows-msvc-x64`,
`windows-clangcl-x64`, `linux-x64`, and `android-arm64` presets were
validated for syntactic/semantic consistency (`cmake --preset` argument
parsing was exercised via the shared `base` preset logic that
`macos-universal` also uses; JSON schema validity checked with
`python3 -m json.tool`) but not build-tested — no Windows machine is
available locally, and Linux/Android were deliberately left for CI rather
than spinning up local cross-toolchains this spike doesn't otherwise need.
`.github/workflows/engine-ci.yml` was validated with `actionlint` (zero
findings) and a `yaml.safe_load` parse check.*
