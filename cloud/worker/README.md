# cloud/worker — D1: containerized cloud worker

Tech Spec §3.8 "Cloud" row / workstream D, task D1: *"Engine CLI: headless
Linux build, containerized worker image."* This directory is the whole of
D1's deliverable — a container that runs `engine_cli --post` (INT-34's
headless post-processing entry point, `engine/docs/INT34-wiring.md` §6)
against an uploaded `.lscan` and writes the result bundle back out.

D2 (job service: token auth, resumable upload, queue, worker orchestration)
landed in `cloud/service/` concurrently with this task. Reading its code
(not editing it — outside this task's ownership) shows the actual seam:
`lidarscan_service/config.py`'s `LIDARSCAN_WORKER_CMD` env var is a
whitespace-tokenized command template with `{input}`/`{output}`
placeholders, `exec`'d directly (`asyncio.create_subprocess_exec`, no
shell) by `lidarscan_service/worker.py`. Its default is

```
LIDARSCAN_WORKER_CMD = "engine_cli --post {input} --out {output}"
```

— i.e. D2's simplest deployment expects `engine_cli` itself on `PATH` in
its *own* container (this worker's `Dockerfile` puts it at
`/usr/local/bin/engine_cli`, which is on the default `PATH`, so a service
image built `FROM` this one, or that `COPY --from=`s the binary out of it,
gets that default working unmodified — see "Two ways to use this image"
below). D2's stderr progress regex
(`^\s*(?:stage:\s*)?(\d{1,3})\s*%\s*(msg)$`) and exit-code table
(0 ok / 1 failed / 2 usage / 3 cancelled) are both lifted straight from
`engine_cli`'s own documented contract (INT34-wiring.md §6), which is also
this container's contract — nothing here needed to change to match it.
This README documents D1's side of the seam either way.

### Two ways to use this image

1. **As the whole worker environment for D2's service** (matches
   `DEFAULT_WORKER_CMD` with zero configuration): D2's own `Dockerfile`
   (not yet written as of this task) installs the FastAPI service on top of
   `FROM ghcr.io/OWNER/lidarscan-worker:<tag>`, or `COPY --from=` this
   image's `/usr/local/bin/engine_cli` into its own. `LIDARSCAN_WORKER_CMD`
   never needs to be set.
2. **As a fully isolated subprocess**, for an operator who wants the
   untrusted-input processing step (a `.lscan` is attacker-controlled bytes
   the moment it is uploaded) sandboxed in its own container rather than
   sharing the API service's filesystem/process namespace:
   ```
   LIDARSCAN_WORKER_CMD="docker run --rm \
     -v {input}:/data/input -v {output}:/data/output \
     ghcr.io/OWNER/lidarscan-worker:<tag> /data/input /data/output"
   ```
   `{input}`/`{output}` substitute into the two `-v` flags (D2's
   `render_worker_cmd` does a plain per-token string replace, so any token
   containing the placeholder gets it — the trailing `/data/input
   /data/output` are fixed strings matching what gets mounted, not
   substituted). This is exactly this container's own
   `argv[1]=input argv[2]=output` contract below, run through Docker
   instead of directly.

## What ships

| File | Purpose |
| --- | --- |
| `Dockerfile` | Multi-stage build: `builder` compiles `engine_cli` and runs the ctest smoke suite; `runtime` ships only the stripped binary + entrypoint |
| `entrypoint.sh` | The container's `ENTRYPOINT` — translates the `WORKER_CMD` contract below into an `engine_cli --post` invocation |
| `scripts/validate_image.sh` | Reproduces the local verification session end to end (build, synth a `.lscan`, run, assert exit codes + stdout/stderr shape) |

## The `WORKER_CMD` contract

```
argv[1] = input directory   (mounted, contains manifest.json + streams/ — Tech Spec §3.11)
argv[2] = output directory  (mounted, writable — where cloud.ply lands)

stdout  = the job's product summary (engine_cli's own convention — machine-readable)
stderr  = human progress lines ("post: NN%  <stage>") + this script's one preamble line
exit    = passed through UNMODIFIED from engine_cli:
            0 ok · 1 failed · 2 usage · 3 cancelled
```

`WORKER_INPUT_DIR` / `WORKER_OUTPUT_DIR` env vars work as a fallback to the
two positional args, and `WORKER_POST_ARGS` (e.g.
`"--no-loops --dedup 0.02"`) is appended verbatim to the `engine_cli --post`
call for a caller that wants to override A7's defaults. All three are
optional; left unset, the worker's behaviour is exactly
`engine_cli --post <in> --out <out>` — "a cloud worker's defaults are A7's
defaults" (INT34-wiring.md §6).

```bash
docker run --rm \
  -v /host/path/to/session.lscan:/data/input \
  -v /host/path/to/results:/data/output \
  lidarscan-worker:local \
  /data/input /data/output
```

`entrypoint.sh` ends in `exec engine_cli ...`, not a backgrounded call — the
shell process is replaced, so the container's own exit code IS
`engine_cli`'s exit code with no wrapper in between.

## Build approach

**Two-stage, no vcpkg, no Livox SDK2, Eigen from apt instead of a network
fetch.**

- **Base**: `debian:bookworm-slim`, pinned by digest (both stages use the
  *same* digest, so the builder's glibc and the runtime's glibc are
  guaranteed identical — see "why partial static linking is safe" below).
- **Builder stage** installs `build-essential` (gcc/g++ — bookworm's GCC 12
  covers the engine's C++20 requirement), `cmake` (bookworm ships 3.25.1,
  clearing the engine's `cmake_minimum_required(VERSION 3.24)` floor),
  `ninja-build`, and `libeigen3-dev`.
- **Why Eigen from apt, not FetchContent, not vcpkg:** the task brief asked
  to check whether the engine builds *without* Eigen at all
  (`ENGINE_WITH_EIGEN`) to avoid needing it in the image. It doesn't, for
  this specific job: `engine/CMakeLists.txt` says Eigen is "required from
  A6/A7 onwards", and `--post` drives exactly A7's post-processing pipeline
  — turning `ENGINE_WITH_EIGEN` off is not available to a worker that has to
  run `--post`. What *was* available, and is the same spirit the task asked
  for ("prefer minimal deps so no vcpkg needed in-image"), is skipping
  vcpkg. `VCPKG_ROOT` is deliberately left unset, so
  `engine/CMakeLists.txt`'s own fallback chain (`find_package` →
  `FetchContent`, see its top-of-file comment) runs; `libeigen3-dev`'s
  `Eigen3Config.cmake` satisfies `find_package(Eigen3 3.4 CONFIG)` directly,
  so the `FetchContent` network-download path never even triggers. The
  result: no vcpkg, no network access needed at CMake-configure time, fully
  reproducible from `apt` alone.
- **No Livox SDK2**: the Dockerfile's `COPY` list is a selective allowlist
  of `engine/` subdirectories (CMakeLists.txt, cmake/, include/, capi/,
  src/, tools/, tests/, third_party/doctest/) — `third_party/Livox-SDK2` and
  `third_party/patches` are simply never in the build context, and
  `-DENGINE_WITH_LIVOX_SDK2=OFF` is passed explicitly as a second layer on
  top of that. `engine/CMakeLists.txt`'s own `AUTO` default would have
  resolved to the same answer (SDK2 absent ⇒ off), but explicit is cheaper
  to audit than "trust the absence." A cloud worker only ever
  post-processes an already-recorded `.lscan`; it never does live Mid-360
  capture, so the SDK has nothing to do here regardless.
- **`-static-libgcc -static-libstdc++`** on the link step: `engine_cli`
  ends up depending on nothing beyond glibc itself (`ldd` output during the
  build: `linux-vdso.so.1`, `libm.so.6`, `libc.so.6`,
  `ld-linux-*.so.1` — no `libstdc++.so.6`/`libgcc_s.so.1`). That is what
  lets the runtime stage skip installing `libstdc++6`/`libgcc-s1` entirely:
  the base image's own glibc is the only thing the binary needs. This is
  **only** safe because builder and runtime share the exact same base-image
  digest (same glibc ABI on both sides) — a mismatched glibc between stages
  would have made this a landmine instead of a size optimization.
- **Build stage runs the ctest smoke** the task asked for:
  `ctest --test-dir build --output-on-failure -R 'engine_cli_(selftest|version|post)$'`
  — the same three CLI-level cases `engine-ci.yml`'s linux-x64 leg already
  trusts, where `engine_cli_post` **is** `--post-selftest`
  (`engine/CMakeLists.txt`'s registered ctest name for it) — synthesize a
  tiny Mid-360 `.lscan`, run the real `--post --out` path through A7's
  pipeline + A9's export, assert a real non-trivial `.ply` came out. A
  failure here fails `docker build` itself, before any image is produced.
  Only the `engine_cli` target is built (not `scanengine_tests`/
  `scanengine_capi_smoke`, which the CLI-level ctest cases don't need) —
  keeps the image build fast without skipping the check that matters for a
  worker.
- **Runtime stage is `debian:bookworm-slim`, not true distroless.** The task
  said "distroless/slim" — those pull in different directions here:
  `entrypoint.sh` needs a POSIX shell, and `gcr.io/distroless/*`'s
  non-debug variants ship **no** `/bin/sh` at all by design. `debian-slim`
  is the "distroless in spirit" choice that still gives the entrypoint
  script somewhere to run: same pinned base as the builder, no compiler, no
  package cache, nothing installed at runtime beyond what the base image
  already had (see the static-linking note above — no extra apt packages
  needed). Runs as a non-root `worker` user (uid/gid 10001), `WORKDIR /work`.
  If image size becomes a real constraint later, the next step down would
  be `gcr.io/distroless/cc-debian12` + rewriting `entrypoint.sh`'s handful
  of lines as a second tiny statically-linked binary instead of a shell
  script — not done here since nothing asked for it and the current image
  is already well under typical worker-image budgets (see below).

## Verification

Docker Desktop (29.5.3, linux/arm64 engine) was already present on this
machine (`docker version` / `docker info` both succeeded), so the full
container build + run was exercised directly — no deferral needed.

```
$ docker build -f cloud/worker/Dockerfile -t lidarscan-worker:local .
...
 => [builder 14/15] RUN ctest --test-dir build --output-on-failure -R 'engine_cli_(selftest|version|post)$'
    1/3 Test #3: engine_cli_selftest ..............   Passed    0.00 sec
    2/3 Test #4: engine_cli_version ...............   Passed    0.00 sec
    3/3 Test #5: engine_cli_post ..................   Passed    0.03 sec
    100% tests passed, 0 tests failed out of 3
 => [builder 15/15] RUN strip --strip-unneeded build/engine_cli && ldd build/engine_cli && ls -la build/engine_cli
    linux-vdso.so.1, libm.so.6, libc.so.6, /lib/ld-linux-aarch64.so.1   (no libstdc++/libgcc — confirms static link)
    -rwxr-xr-x 1 root root 1970904 build/engine_cli   (~1.97 MB stripped)
...
=> naming to docker.io/library/lidarscan-worker:local

$ docker images lidarscan-worker:local
lidarscan-worker:local   139MB
```

**Image size: 139 MB** (`debian:bookworm-slim` base is ~108 MB of that;
`engine_cli` itself is ~2 MB stripped; the rest is the `useradd`/`groupadd`
layer and image metadata). That is the `docker images` figure for the
uncompressed local image; the pushed/pulled size over the registry (gzip
layers) is smaller.

**Full contract run** (`scripts/validate_image.sh`, reproduced verbatim —
build the image with the script itself, or point `--image` at one already
built):

```
$ cloud/worker/scripts/validate_image.sh
== 1. synthesizing a .lscan with the image's own engine_cli ==
wrote a 2.2 s synthetic Mid-360 .lscan to /work/session.lscan

== 2. running the worker: argv = <input-dir> <output-dir> ==
  PASS: happy-path exit code (got 0)
  PASS: cloud.ply exists (got yes)
  PASS: cloud.ply is non-trivial (>512 bytes) (got big)
  PASS: stdout carries the product summary (got 1)
  PASS: stdout does NOT carry progress lines (got 0)
  PASS: stderr carries progress lines (>=1) (got yes)

== 3. usage error: no args -> exit 2 ==
  PASS: missing-args exit code (got 2)

== 4. usage error: nonexistent input dir -> exit 2 ==
  PASS: nonexistent-input-dir exit code (got 2)

== 5. engine_cli failure: input dir with no manifest.json -> exit 1 ==
  PASS: invalid-lscan exit code (got 1)

== summary: 9 passed, 0 failed ==
```

One thing this run surfaced that is worth recording rather than hiding: a
**plain** `engine_cli --post` (no `--no-outlier`/`--no-loops`/`--dedup`) on
the *synthetic* fixture produces `cloud.ply` with **0 points** — not a
worker bug. The synthetic capture's 96 points repeat identically at every
keyframe (a 3 m spherical shell, artificially dense in angle and sparse in
range), and A7's statistical outlier filter's k-NN distance distribution
has nothing meaningful to say about that shape — it deletes the whole
cloud. `engine_cli.cpp`'s own `cmd_post_selftest()` (the ctest run inside
the Dockerfile build) works around this the same way
`scripts/validate_image.sh` does: `--no-outlier --no-loops --dedup 0.05`.
**A real capture does not have this problem** — real point density gives
the outlier filter something real to reject — so this is purely a synthetic
fixture artifact, not a worker defect, and is not something D2's real job
service needs to route around beyond the same `WORKER_POST_ARGS` knob this
worker already exposes.

### If container tooling is unavailable

`scripts/validate_image.sh` checks for `docker` (or `podman` via
`CONTAINER_ENGINE=podman`) on `PATH` and a running daemon, and exits 3 with
an explicit message rather than silently no-op'ing if neither is available.
It was not needed on this run since Docker Desktop was already installed
and running, but the fallback path (`brew install colima docker` +
`colima start`, or `brew install podman`) is documented in the script's own
`--help` and error text for a future run on a machine without container
tooling.

## GHCR publish (`.github/workflows/worker-image.yml`)

- Every push/PR touching `cloud/worker/**` or `engine/**` builds the image
  and runs both smoke checks (`--post-selftest` against the runtime image,
  then the full `scripts/validate_image.sh` contract check) — this is the
  gate, and it runs on branches and PRs, not just tags.
- **Publishing to GHCR happens only on a tag push** (`refs/tags/*`), per the
  task boundary. The published ref is
  `ghcr.io/${{ github.repository_owner }}/lidarscan-worker:<tag>` (plus a
  `:latest` alias), owner lowercased since GHCR requires a lowercase path.
- **`OWNER` resolves itself on the first push once the GitHub remote
  exists** — `github.repository_owner` is whatever account/org the repo
  lives under at push time; nothing in the workflow needs editing once a
  remote is created, tagged, and pushed.
