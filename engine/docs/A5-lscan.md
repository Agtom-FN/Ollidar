# A5 — `.lscan` container writer/reader + replay harness

Implements Tech Spec §3.11 against the format A1 already fixed and tested
(`include/scanengine/record/lscan.h`, `tests/test_record.cpp` — magic,
chunk/stream framing, CRC32, chunk-type numbering). That file, and its
constants, are unchanged by this task except for additive declarations.

Owner: A5. Files: `include/scanengine/record/{lscan.h,replay.h,zip.h}`,
`src/record/{lscan.cpp,replay.cpp,zip.cpp}`, `tests/test_lscan_io.cpp`.

## 1. What shipped

| Piece | Class / functions | State |
| --- | --- | --- |
| Writer | `lscan::FileRecordWriter` | done — real directory writer, replaces `NullRecordWriter` as *the* real implementation (both now coexist; see §5 for why the Engine hasn't been switched over) |
| Reader | `lscan::FileRecordReader` | done — validates, chronologically merges, tolerates truncation/corruption |
| Replay | `lscan::ReplaySource` (`record/replay.h`) | done for the D6 raw stream; see §3 for the Mid-360/GNSS seam it is waiting on |
| Transfer bundle | `lscan::zip_export` / `zip_import` (`record/zip.h`) | done — dependency-free stored-ZIP, not yet compressed (§4) |

All four are exercised by `tests/test_lscan_io.cpp` (11 new test cases, 293
assertions) in addition to the untouched `tests/test_record.cpp` (7 format
cases). Full verification in §6.

## 2. FileRecordWriter

### Directory skeleton and manifest

`open(lscan_dir)` creates the whole Tech Spec §3.11 skeleton up front —
`manifest.json`, `streams/`, `streams/frames/`, `processed/`, `merged/`,
`exports/` — but each `streams/*.bin` file itself is created **lazily**, on
its stream's first `write_chunk()`, so e.g. a D6-only capture never produces
an empty `imu.bin`.

`manifest.json` fields: `schemaVersion`, `formatVersion` (mirrors
`lscan::kFormatVersion`), `engineVersion` (from the existing
`SCANENGINE_VERSION` compile definition), `createdAtUtcNs`, `sealed` /
`sealedAtUtcNs`, `profile`, `sensors` (array), `mountCalibration`, `crs`, and
a `streams` summary map. `mountCalibration` and `crs` are always emitted as
`null` — the task said "placeholder" for both, and A8 (mount wizard) / A10
(CRS/EPSG) are the tasks that fill them in; A5 only reserves the keys.
`profile` / `sensors` are settable via `set_profile()` / `add_sensor()`
*before* `open()` — see §5 for why nothing calls them yet.

The manifest is written twice: once at `open()` (`sealed: false`) and once
at `close()` (`sealed: true`), via a write-to-`.tmp`-then-rename swap
(`std::filesystem::rename`, falling back to remove+rename if the
destination already exists — relevant on Windows). A manifest still showing
`sealed: false` after the fact is a **positive crash signal**, not just an
absent one.

Manifest parsing (reader side) is a hand-rolled brace/string-balance check,
not a JSON library — see §4 for why the whole module stays dependency-free.

### Flush / fsync policy — the documented data-loss window

This is the concrete answer to "what can a crash lose":

* Every `write_chunk()` appends `[header][payload][crc32]` to that stream's
  buffered `FILE*`. A chunk is **never partially visible** to a reader,
  because `FileRecordReader` only trusts a chunk once its CRC verifies — so
  a torn OS-buffer write reads back as "not written yet", never as silent
  corruption. This is what makes the *whole* crash-safety story work: the
  writer doesn't need to get flush timing right for correctness, only for
  **how much** is at risk.
* A stream auto-flushes (`fflush` + `fsync`/`_commit`) when **either**
  `kAutoFlushBytes` (1 MiB) have accumulated since its last flush **or**
  `kAutoFlushIntervalNs` (1 s) have elapsed since its last flush for that
  stream — checked opportunistically on the next `write_chunk()` call,
  because `record/` (like every module per `DESIGN.md` §2) owns no thread
  of its own; A5 does not introduce one.
* **Documented window:** under continuous input (the normal case — D6 at
  10 Hz+ produces far more than one chunk/second) the loss window is bounded
  by `kAutoFlushIntervalNs`: at most ~1 s of un-fsync'd chunks lost on a hard
  crash. If input stalls completely with a nonempty buffer pending, that
  tail is **not** time-bounded until the next `write_chunk()` or an explicit
  `flush()` — a caller that must bound loss through idle periods should call
  `flush()` from its own periodic timer. The Android/Qt capture UIs already
  poll engine state on a timer for other reasons (health panel, RTK status
  strip); hooking `flush()` to that same cadence is the intended
  integration and needs no new engine seam — `Engine::recorder().flush()`
  is already public.
* `close()` flushes+fsyncs everything, then rewrites the manifest sealed.

### Truncated-tail contract (writer side of it)

The writer only ever appends; it never rewrites a previously-completed
chunk. Combined with the CRC-before-trust rule above, a process kill at any
point produces a file the reader can always open and read up to the last
complete, verified chunk — see §6 for the exact byte-offset truncation
matrix the tests exercise (mid-header, mid-payload, mid-CRC, and the
"exact frame boundary" case that must **not** count as a warning).

## 3. FileRecordReader

`open()` does two independent things and neither can fail the other:

1. **Manifest.** Read + structurally validate `manifest.json`. A
   missing/empty/corrupt manifest does **not** fail `open()` — crash safety
   means raw chunk data must outlive a half-written or absent manifest (a
   kill at the very first `write()` syscall can leave it empty).
   `manifest_present()` / `manifest_ok()` report what was found;
   `tests/test_lscan_io.cpp`'s `reader_tolerates_missing_empty_and_corrupt_manifests`
   covers all three cases and confirms the stream data is still fully
   readable regardless.
2. **Streams.** For each of the five known on-disk paths (`lidar.bin`,
   `imu.bin`, `poses_ar.bin`, `gnss.bin`, `frames/frames.idx` —
   `stream_of()`/`stream_file_of()` already map several `ChunkType`/
   `StreamId` values onto the same physical file, e.g. D6 and Mid-360 both
   land in `lidar.bin`; the reader trusts the file's own embedded
   `StreamFileHeader.stream` field, not the filename, to report which one
   was actually used), run a **validation pass**: walk every chunk once,
   verifying length-vs-EOF and CRC, and stop at the first failure. That
   determines `stream_summaries()` (count/bytes/t-range) and
   `warnings().truncated_tail_chunks` / `crc_mismatch_chunks` up front, once
   — not recomputed on every `next_chunk()` call. A stream file whose own
   32-byte header is unreadable is skipped and counted in
   `unreadable_streams`; a stream file whose `format_version` is *newer*
   than this reader supports is a hard `kVersionMismatch` from `open()`
   itself (per the format contract's own comment in `lscan.h`).

`next_chunk()` performs a chronological **k-way merge** across every stream
file present, so chunks come out in non-decreasing `t_mono_ns` order across
the *whole container*, not just within one stream. This is what
`record/replay.h` needs for a future multi-sensor replay. **The merge
assumes each individual stream file is itself already time-ordered** — true
by construction for any real capture (one device/stream writes in arrival
order) — it does not re-sort a stream internally. `next_chunk()` returns
`ScanError::kAgain` once every stream is exhausted (same "not an error"
convention as `EventBus::poll()`).

`seek(t)` rewinds every stream cursor to its start and linearly re-scans to
the first chunk at or after `t`. That's O(stream size), by design: there is
no index yet. `streams/frames/frames.idx` (A11/B8) is the intended
seek-acceleration structure for the camera-frame stream specifically; a
general chunk index across all streams is future work if profiling ever
shows the linear rescan mattering (a `.lscan` is read start-to-finish far
more often than seeked into, per every consumer in the module map: replay,
post-SLAM, colorization, cloud worker).

## 4. Replay harness (`record/replay.h`)

`ReplaySource::run(cfg)` reads a `.lscan` via `FileRecordReader` and calls
`Engine::push_serial_bytes(cfg.target_device, payload, TimePoint{t_mono_ns})`
— **the exact same entry point** a live USB-serial reader thread calls, with
the payload byte-for-byte and the **original recorded timestamp** as the
arrival stamp (not "now"). That last part matters: `D6Driver`'s rotation
detection and health-window packet counts are computed from arrival times,
so reusing them, not resampling the clock, is what makes replay
bit-identical to capture rather than merely "close".

`cfg.speed`: `1.0` paces chunks against a wall clock anchored at the first
replayed chunk (`std::this_thread::sleep_until`, scaled by `1/speed`); `<=
0` is unpaced (every chunk pushed as fast as the engine can decode it) —
what batch reprocessing (local/cloud/transfer, §3.8) and this task's
correctness tests use. Both paths call the identical
`push_serial_bytes()`/timestamp code; only the sleeping differs.

**Only `ChunkType::kD6Raw` replays today.** `push_serial_bytes()` is the
only "feed a device bytes" entry point `Engine` exposes; Mid-360 (A3) and
GNSS (A10) raw streams will need an analogous push entry point on `Engine`
before `ReplaySource` (whose merge/pacing logic is already generic across
chunk types — see `ReplayConfig::chunk_type`) can forward them too. That is
a small, additive change when A3/A10 land, not a redesign.

**engine_cli** already has a `--replay <raw-file>` flag from A1 that replays
a raw byte capture directly; it does not go through `.lscan` at all.
Wiring a `--replay-lscan <dir> [--speed N]` flag on top of `ReplaySource`
would be a small, natural addition, but `tools/engine_cli.cpp` is outside
A5's file ownership for this task — noted for the orchestrator in §7.

### Round-trip proof

`test_lscan_io.cpp`'s `replay_reproduces_the_live_decoded_point_stream_bit_for_bit`:
a synthetic two-revolution D6 capture (same `d6test::build_revolution`
fixture `test_engine.cpp` uses) is pushed into a live `Engine` **and**
independently written to a real `.lscan` via `FileRecordWriter` chunk by
chunk (mirroring exactly what `Engine::push_serial_bytes()`'s own
`write_chunk(kD6Raw, t, bytes)` call does — see `src/core/engine.cpp`).  A
second, fresh `Engine` is then driven purely by `ReplaySource` reading that
`.lscan` back. The test asserts, live vs. replayed:

* `DeviceHealth`: `packets_ok`, `packets_bad`, `points_out`,
  `checksum_pass_rate`, `bytes_in` — all equal.
* `PageStore`: same total point count, same single page, and **every**
  decoded `PointVertex` (`x`,`y`,`z`,`r`,`g`,`b`,`a`) compared field-by-field
  — 802 points, all identical.

A second test, `replay_speed_multiplier_paces_wall_clock`, confirms
`speed=1.0` actually sleeps (a 5-chunk, 80 ms-span capture takes ≥40 ms wall
clock to replay, vs. sub-millisecond for the unpaced correctness tests).

## 5. The seam this task could not close itself

**`Engine::Impl` (`src/core/engine.cpp`) still records into
`lscan::NullRecordWriter`, not `FileRecordWriter`.** This is deliberate, not
an oversight — read on.

`RecordWriter` is polymorphic (`Engine::recorder()` returns `RecordWriter&`)
but `Engine::Impl` holds it as a **concrete, non-pointer member**:

```cpp
struct Engine::Impl {
  ...
  lscan::NullRecordWriter recorder;  // A5 swaps in the real writer
  ...
};
```

Swapping the concrete type — or turning it into a
`std::unique_ptr<RecordWriter>` behind a factory, the way A4's
`TimeSync::set_estimator()` seam works for `OffsetEstimator` — requires
editing `src/core/engine.cpp` and/or `include/scanengine/core/engine.h`.
Both are `core/`, explicitly off-limits for this task (owned by A1, edited
concurrently by other agents this session). I looked for an existing
factory/injection seam analogous to `set_estimator()` and there isn't one
for `RecordWriter` yet — grepped for `set_recorder`/`RecordWriterFactory`/
`make_record_writer` and found nothing.

Per this task's instructions, rather than edit `core/` I built and tested
`FileRecordWriter`/`FileRecordReader`/`ReplaySource` standalone — every test
in `test_lscan_io.cpp` drives them directly, and the replay round-trip
test (§4) reconstructs "what the wired engine would have written" by
calling `FileRecordWriter::write_chunk()` with the same arguments
`Engine::push_serial_bytes()` passes today. That proves the writer and the
replay harness are correct **and** that the wiring, once added, is a pure
plumbing change with no remaining design risk.

**What the orchestrator (or A1) needs to do to finish the wiring** — either
of:

1. Smallest change: replace the member with
   `std::unique_ptr<lscan::RecordWriter> recorder = std::make_unique<lscan::FileRecordWriter>();`
   and change the handful of `impl_->recorder.` call sites in
   `engine.cpp` to `impl_->recorder->`. `NullRecordWriter` stays in
   `lscan.h`/`lscan.cpp` (test_record.cpp keeps exercising it directly) but
   Engine stops depending on it as *the* concrete type.
2. A4-style seam: add `Engine::set_recorder(std::unique_ptr<lscan::RecordWriter>)`
   for tests/callers that want `NullRecordWriter`'s no-disk-I/O behaviour
   (existing `test_engine.cpp` case
   `record_always_writes_raw_bytes_before_parsing` explicitly comments
   "NullRecordWriter touches no disk" and would need updating either to call
   `set_recorder()` with a `NullRecordWriter` explicitly, or to accept that
   it now touches `/tmp/engine-test.lscan` for real).

Either way, `SessionConfig.profile` should flow into
`FileRecordWriter::set_profile()` before `open()` so the manifest's
`profile` field stops being a placeholder — a one-line addition at the same
call site.

## 6. Zip transfer bundle (`record/zip.h`)

`zip_export()`/`zip_import()` produce the `.lscan.zip` bundle Tech Spec
§3.8 ("Extract for transfer") and §3.11 describe.

**Design decision: entries are STORED (ZIP method 0, uncompressed), not
DEFLATEd, and no compression codec (e.g. miniz) is vendored.** The task
allowed vendoring miniz "if needed"; I judged it not needed for this seam:

* Zero new dependency footprint — no new vcpkg port (every one has to clear
  all five CI legs per `vcpkg.json`'s onboarding-order note), no vendored
  third-party source under `src/record/` to license-track.
* Still a **real, standards-conformant ZIP** — local file headers, central
  directory, EOCD, correct CRC32 (reusing `lscan::crc32`, whose `seed`
  parameter chains correctly across streamed reads — same technique
  `chunk_crc()` already uses internally) — openable by Finder, `unzip`,
  Explorer, anything.
* `.lscan` payload is already high-entropy (raw D6/Mid-360 bytes, JPEG
  keyframes) — compression would buy less here than on, say, a source tree.
* The upgrade is strictly additive later: swap the method field and add an
  encoder, same function signatures, same bundle layout, no format-version
  bump.

Both functions stream through a fixed 64 KiB buffer rather than loading
whole files into memory, so multi-gigabyte captures are fine.
`zip_import()` rejects `..`/absolute-path entries (zip-slip) with
`kInvalidArgument`, non-stored entries with `kNotSupported`, and any
CRC/size mismatch with `kCorruptData`; it always recreates the standard
`.lscan` skeleton regardless of which directories had files at export time.
`test_lscan_io.cpp` covers a normal export→import round trip (content and
directory-skeleton equality via `FileRecordReader` on the imported copy)
plus two hand-crafted adversarial archives (a `../evil.txt` entry, and a
method-8 entry) proving both rejections fire.

## 7. Verification

Full clean build (temp build dir outside the repo, deleted after — did not
touch `engine/build/macos-universal`, which other concurrent agents' work
uses):

```
$ cmake -S engine -B <tmp> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DENGINE_BUILD_TESTS=ON -DENGINE_BUILD_TOOLS=ON
$ cmake --build <tmp>          # exit 0, zero warnings from any record/ file
$ ctest --test-dir <tmp>
    scanengine_capi_smoke ... Passed
    engine_cli_selftest ..... Passed
    engine_cli_version ...... Passed
    scanengine_tests ........ 146/148 cases, 9753/9756 assertions passed
```

The 2 failing cases (3 assertions) are entirely in
`tests/test_mid360_driver.cpp` / `src/drivers/mid360/` — A3's concurrent
work, unrelated files, unrelated module. One fails on a UDP `bind()`
(network-sandboxed test environment); the other
(`packets_lost == 600` vs. expected `0`) reproduces identically with the
sandbox disabled, so it is a real but pre-existing issue for A3 to look at,
not something this task's changes touch. Restricting the run to
`record_io/*`, `lscan/*` (A1's original format suite,
`tests/test_record.cpp`, unmodified) and `engine/*` (end-to-end D6 capture,
including the recorder accounting test) is fully green:

```
$ ./scanengine_tests --test-case="record_io/*,lscan/*,engine/*"
[doctest] test cases:  27 |  27 passed | 0 failed | 121 skipped
[doctest] assertions: 474 | 474 passed | 0 failed |
```

`test_lscan_io.cpp` alone: 11 test cases / 293 assertions, covering writer
accounting + directory/manifest creation, writer→reader round trip with
three interleaved streams (exact chronological order asserted), a 5-point
truncation matrix (mid-header/mid-payload/mid-CRC/exact-frame-boundary/
empty-stream), a corrupted-payload CRC-mismatch case, three manifest
failure modes (missing/empty/garbage), the replay bit-identical proof, replay
pacing, and zip export/import (happy path + two adversarial archives).

## 8. Known limitations / future work

* `seek()` is O(stream size) — no index yet (see §3).
* Reader's chronological merge assumes each stream file is internally
  time-ordered; it does not defend against a hypothetically out-of-order
  writer (no real writer produces one — `FileRecordWriter` always appends
  with the caller-supplied `t_mono_ns` in call order).
* Zip entries are uncompressed (§6) and capped at 4 GiB per file (a stored
  ZIP32 limit); multi-volume/ZIP64 is not implemented.
* `ReplaySource` only forwards `ChunkType::kD6Raw` (§4) until Mid-360/GNSS
  get their own `Engine` push entry points.
* The Engine→`FileRecordWriter` wiring itself is the one piece this task
  could not land inside its file ownership — see §5 for exactly what's
  needed and why.
