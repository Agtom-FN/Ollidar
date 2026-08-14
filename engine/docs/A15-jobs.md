# A15 — jobs module: processing modes

**Scope:** `engine/src/jobs/**`, `engine/include/scanengine/jobs/**`,
`engine/tests/test_jobs.cpp`.
**Spec:** §3.8 "Processing modes" — Local / Cloud / Extract-for-transfer, one
pipeline in three places.
**Contract:** `engine/DESIGN.md` §2 (threading), §6 (module conventions);
`jobs/job.h` (A1's seam: `JobMode`, `JobRequest`/`JobStatus`/`JobRunner`,
and the `EventType::kJobProgress` contract, all left untouched).
**Consumes:** A7's `PostSlamPipeline` (`docs/A7-post.md`), A9's
`export_points()` (`docs/A9-export.md`), A5's `zip_export`/`zip_import`/
`FileRecordReader` (`docs/A5-lscan.md`), and — landed concurrently with this
task — A11's `color::PointColorizer` (`include/scanengine/color/colorizer.h`).

---

## 1. What ships

| Piece | Header | Impl |
| --- | --- | --- |
| Job model: `Job`, `JobKind`, `JobState`, `JobSpec` + per-kind params | `jobs/job_types.h` | `jobs/job_types.cpp` (`to_string` only) |
| `HttpTransport` seam + REST types | `jobs/http_transport.h` | `jobs/http_transport.cpp` |
| Cloud submit client (upload/poll/download, retry+resume) | `jobs/cloud_submit.h` | `jobs/cloud_submit.cpp` |
| Transfer export/import + manifest sanity report | `jobs/transfer.h` | `jobs/transfer.cpp` |
| Local pipeline drivers (PostProcess/Colorize/ExportPoints) | `jobs/local_runner.h` | `jobs/local_runner.cpp` |
| The queue: one worker thread, priority+FIFO, cancel, completion, event republish | `jobs/job_queue.h` | `jobs/job_queue.cpp` |
| Tests | — | `tests/test_jobs.cpp` (25 cases) |

`jobs/job.h` (A1's seam header) is untouched: `JobMode`/`JobState`/
`JobRequest`/`JobStatus`/`JobRunner` still exist exactly as before —
`tests/test_headers.cpp` (not this task's file) instantiates `JobRequest`
and must keep compiling. This task's own `JobState` lives in
`scanengine::jobs` (a nested namespace, distinct from the top-level
`scanengine::JobState`) precisely so the two never collide; `JobQueue` is a
concrete engine that a future `JobRunner` adapter can sit in front of, not
an implementation of that interface itself (see §7).

No `CMakeLists.txt` edit was made or needed: `src/*.cpp` and
`tests/test_*.cpp` are globbed with `CONFIGURE_DEPENDS`, same as every A2–A15
task before this one.

---

## 2. Job model

```cpp
enum class JobKind : uint8_t { kPostProcess, kColorize, kExportPoints,
                                kTransferExport, kCloudSubmit };
enum class JobState : uint8_t { kQueued, kRunning, kCancelling, kDone, kFailed };

struct Job {
  uint64_t id;
  JobKind kind;
  JobState state;
  int priority;
  float progress;     // 0..1
  std::string stage;  // stable label, kind-specific
  ScanError error;     // kOk unless state == kFailed
  std::string message;
};
```

Deliberately five states, not six: a cancelled job settles into `kFailed`
with `error == ScanError::kCancelled` — the same convention `Status`/
`SCAN_TRY` already use everywhere in the engine, so a UI does not need a
second code path to notice a cancellation, it reads the error.

`JobSpec` carries one `JobKind` plus all five per-kind parameter structs
(`PostProcessParams`, `ColorizeParams`, `ExportPointsParams`,
`TransferExportParams`, `CloudSubmitParams`); only the one matching
`spec.kind` is read. Chaining ("jobs may chain exports", A9's phrasing):
`ColorizeParams`/`ExportPointsParams`/`CloudSubmitParams` each carry a
`chain_from` job id as an alternative to a direct input — `JobQueue`
resolves it at run time against a prior job's produced artifact
(`produced_store()` for a `PageStore`, `produced_zip_path()` for a zip),
failing with `kInvalidState`/`kNotFound` if the source job is not a
finished, successful producer of the right kind.

---

## 3. JobQueue

One worker thread, started in the constructor and joined in the destructor
— the DESIGN.md §2 thread-table row **"(A15) job workers"** now has one
concrete instance; that row's text still says "post pipeline, exports,
uploads", which this implementation matches (plus Colorize and Transfer).
**#29 owns editing DESIGN.md itself** — this doc is where the detail lives
until that lands; see §7.

* **FIFO with priorities.** `std::map<int, std::deque<uint64_t>, std::greater<int>>`:
  highest `JobSpec::priority` first, FIFO within a priority level.
  `tests/test_jobs.cpp`'s `queue/priority_order_is_observable_in_completion_order`
  proves this with a deterministic (non-timing-based) occupier job — see
  §6's testing note.
* **Cancellation.** `cancel()` on a `Queued` job removes it from `ready` and
  finalizes it immediately, without ever running. `cancel()` on a `Running`
  job sets `kCancelling` and invokes whatever cooperative-cancel mechanism
  that job kind registered: `post::CancelToken` for `kPostProcess` (and for
  `kColorize` **when** the concrete `color::PointColorizer` is behind the
  seam, see §4), `ExportCancelToken` for `kExportPoints`, a plain
  `std::atomic<bool>` polled between chunks/polls for `kCloudSubmit`.
  `kTransferExport` and a `kColorize` job driven by a plain (non-
  `PointColorizer`) `Colorizer*` have a real gap: the primitives they
  drive (`zip_export()`/`zip_import()`, the abstract `Colorizer::colorize()`)
  expose no cancel hook, so the job settles wherever that blocking call
  lands. Documented at the call sites (`jobs/transfer.h`,
  `jobs/local_runner.h`), not silently swallowed.
* **Completion callbacks.** `on_completion()` fires once per job, on the
  worker thread, when it reaches `kDone`/`kFailed` — same "quick, no
  re-entry" rule as `EventBus` callbacks and `PageStore` subscribers
  (DESIGN.md §2).
* **Progress → `EventType::kJobProgress`.** Every progress update — from any
  job kind — updates the in-memory `Job` snapshot and, if the queue was
  constructed with an `EventBus*`, publishes
  `JobProgressPayload{job_id, progress, state}` on it. `state` is
  `static_cast<uint8_t>(jobs::JobState)` using **this task's** 5-value
  encoding (0=Queued, 1=Running, 2=Cancelling, 3=Done, 4=Failed) — `job.h`'s
  header comment says progress is reported this way but left the payload's
  `state` encoding to whoever fills it; that's now fixed here. This is the
  "two-line republishing lambda" `docs/A7-post.md` §8 item 3 asked A15 for,
  implemented once for every job kind rather than per-pipeline.

`JobQueue` is a standalone concrete class; it does not implement
`jobs/job.h`'s `JobRunner` interface directly (see §7 for why, and what a
thin adapter would look like).

---

## 4. Local runner — wired vs. seamed

| Job kind | Status | Notes |
| --- | --- | --- |
| `kPostProcess` | **Wired** | Drives `post::PostSlamPipeline` directly. Always routes it at an externally-owned `PageStore` (never the pipeline's own internally-owned one) so the store outlives the pipeline object — required for a chained `kColorize`/`kExportPoints` to reach it. Real cancellation via `post::CancelToken`; real per-stage progress via `PostProgressFn`. |
| `kColorize` | **Wired**, two paths | `ColorizeParams::colorizer` is caller-injected (any `Colorizer*`). `run_colorize()` `dynamic_cast`s to `color::PointColorizer` (landed alongside this task, `include/scanengine/color/colorizer.h`): when it is one, real cancellation (`PointColorizer::set_cancel_token`) and fine-grained progress (`set_progress_callback`) are wired, and `load_keyframes(lscan_dir)` is called automatically when the caller left `keyframes` empty — a `kNotFound` from that (no camera for this session, Tech Spec §3.5 "gracefully unavailable") is **not** a job failure. Any other `Colorizer*` (a test double, or a future second implementation) goes through the bare abstract-seam sequence (`set_extrinsics`/`add_keyframe`*/`colorize`) with only two progress ticks and no cancellation, because the abstract interface has neither. A null `colorizer` fails fast with `kUnimplemented`. |
| `kExportPoints` | **Wired** | Calls A9's `export_points()` exactly as `docs/A9-export.md` specifies it should be called from `jobs/`. Real cancellation via `ExportCancelToken`. |
| `kTransferExport` | **Wired**, documented gap | Drives A5's `zip_export()`. `include_results = false` is implemented by staging `manifest.json` + `streams/` into a temp dir first (A5's `zip_export()` has no include/exclude filter — not this task's file to add one to) then zipping that; the staging dir is always cleaned up. No progress/cancel hook exists on `zip_export()` itself — see §3. |
| `kCloudSubmit` | **Wired**, seam is `HttpTransport` | See §5. |

Import-side validation (goal 3's other half) is **not** a job kind — the
task's five kinds don't include one, and the spec (§3.13) treats "import a
bundle" as a synchronous drag-drop/file-association action, not a queued
background job. `jobs::import_and_validate()` (`jobs/transfer.h`) is a plain
function: `zip_import()` then `FileRecordReader` to produce an
`ImportValidationReport` (manifest present/ok, truncated/CRC-mismatch
counts, per-stream summaries, a `sane()` verdict) — call it directly from
the app right after a bundle arrives.

---

## 5. Cloud REST contract (§3.8's "Cloud" row)

Single-tenant MVP (§3.8): token auth, no payments/quotas, one worker
instance, hard upload-size cap. This is A15's own design — no such contract
existed before this task — documented here for D1/D2/D3 to implement a real
service against, and for D3 specifically to implement `HttpTransport` over
a real socket/TLS stack. `CloudSubmitClient` (`jobs/cloud_submit.h`) is
written and tested **entirely** against a scripted fake transport
(`tests/test_jobs.cpp`'s `FakeCloudServer`) — no network dependency was
added.

```
POST   {base}/jobs
  headers: Authorization: Bearer <token>, Content-Type: application/json
  body:    {"kind":"lscan","size_bytes":<N>}
  201:     {"id":"<job_id>","upload_url":"/jobs/<job_id>/upload"}
  401:     token rejected
  413:     size exceeds the server's cap

PUT    {base}{upload_url}          — one call per chunk, resumable
  headers: Authorization, Content-Range: bytes <start>-<end>/<total>
  body:    that chunk's bytes
  200:     chunk accepted, more expected      (Upload-Offset: <received>)
  201:     final chunk accepted, upload complete, processing begins
  Resume probe (after a disconnect exhausts retries):
  headers: Authorization, Content-Range: bytes */<total>, empty body
  308:     Upload-Offset: <bytes the server actually has> — resume from there

GET    {base}/jobs/{id}
  headers: Authorization
  200:     {"id":..., "state":"queued|uploading|processing|done|failed",
            "progress":0..1, "message":"..."}
  401 / 404

GET    {base}/jobs/{id}/result
  headers: Authorization
  200:     binary body = the result bundle
  401 / 404 (not ready yet)
```

**Resumable upload semantics.** `CloudSubmitClient::submit()` sends the file
in `chunk_bytes`-sized `Content-Range` PUTs. A chunk that fails (no response
at all, or a 5xx) is retried up to `max_retries` times with exponential
backoff (`backoff_initial_ms` × `backoff_multiplier`, capped at
`backoff_max_ms`). If retries are exhausted, the client issues **one**
resume-offset probe and continues from the server's authoritative offset —
this is what makes "the ack was lost but the bytes landed" resumable rather
than a duplicate-or-fail choice.
`tests/test_jobs.cpp`'s `cloud/mid_upload_disconnect_then_resume_completes`
proves the file lands byte-for-byte across exactly this: the fake server
stores the chunk and drops the ack once, the client's retries fail, the
probe reports the true (advanced) offset, and upload continues from there.

**Size cap.** `CloudSubmitConfig::max_upload_bytes` (MVP default 2 GiB) is
checked against the local file's size **before** `POST /jobs` is even sent
— `cloud/size_cap_rejects_before_any_request` asserts zero requests reach
the transport when it trips.

**Retry policy.** Only a transport-level failure (`transport_ok == false`)
or a 5xx status is retried. A real status the server *did* return (401,
413, 400/416) is never retried — that is the caller's decision, made in
`submit()`/`poll()`/`download_result()` directly.

**`HttpTransport`** (`jobs/http_transport.h`) is the whole seam:
`HttpResponse request(const HttpRequest&)`. One call is one HTTP round
trip; resumable upload is built on top of it by issuing one call per chunk,
not by the interface streaming internally — which is what keeps a fake
transport a pure function with no connection state. The real
socket/TLS-backed implementation is D3's, satisfying exactly this
interface.

---

## 6. Tests (`tests/test_jobs.cpp`, 25 cases)

* **`queue/*`** (8) — FIFO within a priority and priority-before-FIFO
  (both made deterministic by a `BlockingColorizer` test double that
  blocks inside `colorize()` until released — the queue's one worker
  thread is thereby provably occupied while later jobs are submitted, so
  there is **no timing race** in the ordering assertions), cancel-queued
  (never runs), cancel-running (a real `kPostProcess` job, tolerant of the
  legitimate race where a tiny synthetic run finishes before cancel lands —
  same tolerance `tests/test_post.cpp`'s own cancellation test uses),
  unknown-id and already-finished cancel errors, `stop()` draining queued
  work as cancelled, and `EventType::kJobProgress` republishing.
* **`post/*` + `export/*`** (4) — a **real** `PostSlamPipeline` run via
  `JobQueue` on a tiny synthetic `.lscan` (built the way
  `tests/test_lscan_io.cpp`/`tests/test_post.cpp` build one, with
  `lscan::FileRecordWriter`, kept seconds-fast: a stationary capture, no
  loop detection, outlier filter off — this is a plumbing test, not an
  accuracy one, which is what `tests/test_post.cpp` is for), asserting
  `kDone`, a nonempty produced `PageStore`, and a chained `kExportPoints`
  job producing a real, nonempty `.ply` from that store; plus the
  `chain_from`-not-ready failure path.
* **`transfer/*`** (4) — export → import → manifest-sanity-report round
  trip (`sane()` true on a clean bundle), the same round trip via
  `JobQueue`, a corrupt-zip import reporting `sane() == false` without
  crashing, and `include_results = false` staging raw-only (asserts a
  `processed/` file is excluded from the bundle).
* **`cloud/*`** (9) — happy path (upload progress reaching 1.0, poll to
  completion, byte-exact result download), token reject
  (`kPermissionDenied`), the MVP size cap (client-side, zero requests),
  mid-upload disconnect + resume (byte-exact file landing despite one lost
  ack), a total-outage case (`kNetworkError` at job creation), mid-upload
  cancellation, a server-reported job failure, and a `JobQueue`-level
  `kCloudSubmit` chained from a finished `kTransferExport`'s zip, through
  to a downloaded result file.

---

## 7. Integration items for the next pass

1. **`capi/scanengine_c.h`/`.cpp` — `kJobProgress` is not mirrored.**
   `SCAN_EVENT_JOB_PROGRESS = 60` exists and is `static_assert`ed against
   `EventType::kJobProgress`, but `convert_event()` has no
   `case EventType::kJobProgress:` and there is no `scan_job_progress`
   payload struct in `scanengine_c.h` — today a `kJobProgress` event
   crossing the C ABI arrives with a zeroed payload (DESIGN.md §6 item 6's
   "otherwise the payload crosses the ABI as opaque bytes"). Needed before
   Android (B6's "Processing UI: mode chooser, foreground service, queue")
   can read job progress over JNI. Not this task's file.
2. **`DESIGN.md`'s thread table** — the `(A15) job workers` row's text
   ("post pipeline, exports, uploads") should gain "colorize, transfer" and
   a pointer to this doc for the one-worker-thread/FIFO+priority detail.
   #29 owns `DESIGN.md`.
3. **No `JobRunner` adapter yet.** `jobs/job.h`'s `JobRunner` interface
   (`submit(JobRequest)`/`cancel`/`status` in terms of `JobMode` +
   `pipeline` string) is what B6/C-workstream's "mode chooser" UI would
   naturally code against, but `JobQueue`'s richer `JobSpec`/`Job` model
   does not implement it directly — translating a `JobRequest{mode,
   lscan_dir, output_dir, pipeline}` into the right `JobSpec` (e.g.
   `mode=kExtractForTransfer` → `kTransferExport`; `mode=kCloud` →
   `kCloudSubmit` chained from one; `pipeline="post"/"colorize"/"export"`
   under `mode=kLocal` → the matching local kind) is ~30 lines a thin
   adapter class can do without either header changing. Left undone here
   because `JobRequest`/`JobRunner` have no way to express `chain_from`,
   priority, or per-kind options (`ExportFormat`, `CloudSubmitConfig`,
   ...) — a real UI wants those, so the natural place for this adapter is
   the app-facing layer that already knows the richer choices, not a
   lossy shim in `jobs/`.
4. **`zip_export()`/`zip_import()` have no progress or cancel hook.** A
   `kTransferExport` job reports only start/end progress and can only
   honour cancellation either side of the (blocking) zip call — see §3 and
   §4. A5 adding an optional poll-based cancel token (mirroring
   `ExportCancelToken`'s shape) and a progress callback to `record/zip.h`
   would remove both gaps with no change to this task's files beyond
   passing them through.
5. **`engine_cli` has no job-queue-driven CLI surface.** The cloud
   worker (§3.8, workstream D) needs to run exactly the `kPostProcess` (or
   a future `kCloudSubmit`-server-side "run the pipeline") path
   headlessly; `docs/A7-post.md` §8 item 4 already flagged the missing
   `--post <lscan-dir>` flag on `tools/engine_cli.cpp` (not this task's
   file) — `JobQueue`/`local_runner.h` are ready to be driven from it once
   it exists.
6. **A11's `PointColorizer` fast path is `dynamic_cast`-detected.** This
   works today because `Colorizer` has a vtable (RTTI is on, no
   `-fno-rtti`/`-fno-exceptions` anywhere in `CMakeLists.txt`) but it is a
   slightly unusual seam-crossing pattern for this codebase (most seams
   here use a config field or a factory function instead of `dynamic_cast`,
   e.g. `PostConfig::store`, `make_exporter()`). If A11 later wants a
   cleaner story, adding `virtual void set_cancel_token(post::CancelToken*)`
   and a progress-callback hook to the *abstract* `Colorizer` interface
   itself (`color/colorize.h`) would let `run_colorize()` drop the
   `dynamic_cast` entirely and give every future `Colorizer` implementation
   real cancellation for free. Not done here because `color/colorize.h` is
   not this task's file.

---

## 8. Build/test verification

Clean configure + build (`cmake --preset macos-universal`, build dir under
`engine/build/`, deleted after this run) + `cmake --build`: zero warnings
from anything under `src/jobs/`, `include/scanengine/jobs/`, or
`tests/test_jobs.cpp`, including a second pass with `touch` to force a full
recompile of just those files.

`ctest -LE 'sim|sim-rtk'` runs the whole `scanengine_tests` doctest binary
plus `scanengine_capi_smoke`/`engine_cli_selftest`/`engine_cli_version`.
Excluding `color/*`/`gate/*`/`img/*` (`tests/test_color.cpp`, A11's file —
mid-flight from a concurrently-running task at the time of this run, per
the 5-concurrent-agent setup this task's brief describes; the same
situation `docs/A9-export.md` documented for A8's `test_pushbroom.cpp`
during that task's own run), the full remaining suite is **379/379 test
cases, 2,260,510/2,260,510 assertions, green** — including every
pre-existing case plus this task's 37 new ones in `tests/test_jobs.cpp`.
`scanengine_capi_smoke`, `engine_cli_selftest` and `engine_cli_version`
pass outright.
