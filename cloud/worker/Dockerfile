# cloud/worker/Dockerfile — D1: containerized cloud worker (Tech Spec §3.8,
# workstream D). Runs the SAME engine_cli --post path a phone/desktop's
# local-processing mode runs (engine/docs/INT34-wiring.md §6) against an
# uploaded .lscan directory, headless.
#
# Build from the REPO ROOT so the build context can see engine/:
#   docker build -f cloud/worker/Dockerfile -t lidarscan-worker:local .
#
# Two stages: `builder` compiles engine_cli and runs the ctest smoke suite
# (the same three CLI-level tests engine-ci.yml's linux-x64 leg trusts);
# `runtime` ships only the stripped binary + entrypoint on a slim base, no
# build toolchain, no vcpkg, no Livox SDK2.

# ---------------------------------------------------------------------------
# Stage 1: builder
# ---------------------------------------------------------------------------
# Pinned by digest (debian:bookworm-slim as of 2026-08-15) so the toolchain
# and libc ABI are reproducible and match the runtime stage's base exactly
# (both bookworm -> same glibc, which is what makes the partial-static-link
# trick below safe). Re-pin deliberately, never float the tag.
FROM debian@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241 AS builder

# build-essential: gcc/g++ (C++20 — engine/CMakeLists.txt sets CXX_STANDARD 20,
#   GCC 12 in bookworm supports it) + libc6-dev + make.
# cmake/ninja-build: bookworm ships cmake 3.25.1, which clears the engine's
#   `cmake_minimum_required(VERSION 3.24)` floor.
# libeigen3-dev: Eigen 3.4 header-only from the Debian archive. The engine's
#   CMakeLists (ENGINE_WITH_EIGEN, default ON — "required from A6/A7 onwards",
#   and A7's post pipeline is exactly what --post drives) does NOT build
#   without Eigen for this worker's job, so "WITHOUT Eigen" was not available
#   here; what IS available, and is what the task asked for in spirit, is
#   avoiding vcpkg. find_package(Eigen3 3.4 CONFIG) resolves straight to this
#   apt package's Eigen3Config.cmake, so ENGINE_FETCH_EIGEN's FetchContent
#   path (a network download at configure time) never triggers either —
#   fully offline once apt has run.
# ca-certificates: harmless safety net in case FetchContent ever does trigger
#   (e.g. a future Eigen bump ahead of Debian's package).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        libeigen3-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Selective COPY, not `COPY engine/ .` — deliberately. A checkout that has
# already run third_party/fetch_sdk2.sh has a real (multi-hundred-MB)
# Livox-SDK2 tree on disk; the worker must never link it (no Mid-360 live
# capture happens in the cloud, only post-processing of an already-recorded
# .lscan), so it is simplest to just not COPY third_party/Livox-SDK2 or
# third_party/patches at all rather than depend on a .dockerignore that
# would have to live outside this task's cloud/worker/** ownership. Docs,
# spikes/, android/, desktop/ are likewise never in the build context.
COPY engine/CMakeLists.txt engine/CMakePresets.json engine/vcpkg.json engine/
COPY engine/cmake engine/cmake
COPY engine/include engine/include
COPY engine/capi engine/capi
COPY engine/src engine/src
COPY engine/tools engine/tools
COPY engine/tests engine/tests
COPY engine/third_party/doctest engine/third_party/doctest

# Plain cmake, no vcpkg toolchain (VCPKG_ROOT is intentionally left unset —
# see engine/CMakeLists.txt's top comment: without it the build falls back to
# find_package/FetchContent, which is exactly the "no vcpkg needed in-image"
# path this worker wants).
#
#   ENGINE_WITH_LIVOX_SDK2=OFF   explicit, not AUTO — belt-and-suspenders on
#                                top of third_party/Livox-SDK2 simply not
#                                being present in this build context.
#   CMAKE_BUILD_TYPE=Release     the task's ask; the presets' own "base"
#                                default is RelWithDebInfo.
#   -static-libgcc/-static-libstdc++  statically link the two GCC runtime
#                                libs into engine_cli so the runtime stage
#                                needs nothing from the toolchain beyond
#                                glibc itself, which its base image already
#                                has. Safe here specifically because builder
#                                and runtime are pinned to the same bookworm
#                                base (same glibc ABI).
RUN cmake -S engine -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DENGINE_BUILD_TESTS=ON \
        -DENGINE_BUILD_TOOLS=ON \
        -DENGINE_WITH_EIGEN=ON \
        -DENGINE_FETCH_EIGEN=ON \
        -DENGINE_WITH_LIVOX_SDK2=OFF \
        -DENGINE_WARNINGS_AS_ERRORS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc -static-libstdc++"

# Build only engine_cli (and the scanengine static lib it needs) — the three
# ctest cases this stage runs (engine_cli_selftest / _version / _post) all
# invoke the engine_cli binary itself, not scanengine_tests/capi_smoke, so
# there is no reason to pay for compiling the 480-case doctest suite in the
# image build.
RUN cmake --build build --target engine_cli --parallel "$(nproc)"

# ctest smoke: the same three CLI-level cases engine-ci.yml's linux-x64 leg
# runs, plus engine_cli_post — INT-34's --post-selftest ctest, which is the
# D1 entry point exercised end to end (synthesize a tiny .lscan, run the
# real --post --out path through A7's pipeline + A9's export, assert a real
# non-trivial .ply came out). This is the build stage's gate: if it fails,
# the image build fails.
RUN ctest --test-dir build --output-on-failure -R 'engine_cli_(selftest|version|post)$'

# Strip debug symbols now that the binary has passed its tests — smaller
# COPY into the runtime stage, same reasoning `-s` would give the linker.
RUN strip --strip-unneeded build/engine_cli \
    && ldd build/engine_cli \
    && ls -la build/engine_cli

# ---------------------------------------------------------------------------
# Stage 2: runtime
# ---------------------------------------------------------------------------
# Slim, not (fully) distroless: entrypoint.sh needs a POSIX shell, and true
# distroless (gcr.io/distroless/*) ships no /bin/sh at all — the non-debug
# variants are shell-less by design. debian:bookworm-slim is the closest
# "distroless spirit" base that still gives entrypoint.sh somewhere to run:
# same pinned digest as the builder (matching glibc), no compiler, no
# package manager cache, and — because engine_cli was linked with
# -static-libgcc -static-libstdc++ above — no libstdc++6/libgcc-s1 packages
# need installing here either; only glibc, which the base already has.
# `ldd` in the builder step above is the check that this claim is true.
FROM debian@sha256:abd67ffcfa541b485a3dff59865ab629aa048a6c613e639d36e7456b0b229241 AS runtime

LABEL org.opencontainers.image.title="lidarscan-worker" \
      org.opencontainers.image.description="Headless engine_cli --post worker container (Tech Spec workstream D1)" \
      org.opencontainers.image.source="https://github.com/OWNER/lidarscan"

# Non-root: no package installs happen at runtime and nothing in the
# container needs to write outside the two bind-mounted directories the
# entrypoint is given.
RUN groupadd --gid 10001 worker \
    && useradd --uid 10001 --gid worker --home-dir /work --shell /usr/sbin/nologin worker \
    && mkdir -p /work \
    && chown worker:worker /work

COPY --from=builder /src/build/engine_cli /usr/local/bin/engine_cli
COPY --chmod=0755 cloud/worker/entrypoint.sh /usr/local/bin/entrypoint.sh

USER worker
WORKDIR /work

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
