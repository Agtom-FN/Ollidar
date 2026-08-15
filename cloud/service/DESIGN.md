# DESIGN — LidarScan cloud job service (D2)

Scope: `cloud/service/**`. Nothing outside it is touched — the engine, the
apps and the CI are other tasks' files.

The client is the fixed point. `engine/src/jobs/cloud_submit.cpp` ships today,
`desktop/src/app/QtHttpTransport.cpp` puts it on a socket, and
`engine/docs/A15-jobs.md` §5 is the written contract between them. Every
decision below either implements that contract or resolves something the
contract left open — and where it was open, it was closed **in the client's
favour**, meaning: the shipped client must not see an error for anything it
does that is not actually wrong.

---

## 1. Shape

```
                  ┌──────────── one process ────────────┐
  client ──HTTP──▶│ FastAPI routes    (uvicorn, asyncio)│
                  │        │                            │
                  │        ├── SQLite job table + state.json mirrors
                  │        │                            │
                  │        └── asyncio.Queue ──▶ ONE worker task
                  └────────────────────────────────│────┘
                                                   │ exec, own process group
                                                   ▼
                                   LIDARSCAN_WORKER_CMD  (engine_cli --post)
```

Boring on purpose: FastAPI + uvicorn, the standard library for everything
else (`sqlite3`, `zipfile`, `asyncio.subprocess`). No Celery, no Redis, no
S3, no ORM. §3.8 caps this at "one worker instance", and a queue library
whose entire value is *many* workers would be scaffolding for a building
nobody is allowed to build yet.

Files:

| File | Holds |
| --- | --- |
| `config.py` | env parsing + validation; the worker argv template |
| `storage.py` | path layout, atomic writes, zip-slip-safe extract, result zip |
| `store.py` | the SQLite job table, the state vocabulary, `state.json` mirror |
| `worker.py` | the single worker task: extract → exec → progress → package |
| `api.py` | the routes, auth, `Content-Range` handling |
| `asgi.py` / `main.py` | entry points |

---

## 2. State machine

Internal states are finer than the wire's, because the service must
distinguish "nothing uploaded" from "upload done, waiting for the worker"
and a client does not.

```
   POST /jobs
       │
       ▼
   created ──first partial chunk──▶ uploading ──┐
       │                                 │      │ last chunk (201)
       │ last chunk in one PUT (201)     └──────┤
       ▼                                        ▼
                                            queued ──worker picks it up──▶ running
                                               │                             │
                       DELETE ──▶ cancelled ◀──┴─────── DELETE (kills pgid) ─┤
                                                                             │
                                       exit 0 ──▶ done  (results/<id>.zip)   │
                                       exit 1/2/other, timeout ──▶ failed  ◀──┘
                                       exit 3 ──▶ cancelled
```

| internal | wire (`state`) | why |
| --- | --- | --- |
| `created` | `queued` | the job exists, no bytes yet |
| `uploading` | `uploading` | a partial upload; `progress` = received/total |
| `queued` | `queued` | upload complete, worker busy |
| `running` | `processing` | the subprocess is alive; `progress` from its stderr |
| `done` | `done` | `results/<id>.zip` exists |
| `failed` | `failed` | with a reason in `message` and the child's `exit_code` |
| `cancelled` | `failed` | **A15 §2's own convention** — the engine folds cancel into `kFailed` + `kCancelled`, so the wire does too and the word lives in `message` |

Terminal states are terminal: nothing re-opens a job.

`progress` means two different things by design: the upload fraction while
`uploading`, the worker's own progress while `processing`. That mirrors the
client, which tracks upload progress from `submit()` and processing progress
from `poll()` and never compares the two.

---

## 3. Storage layout

```
data/
  jobs.db                  SQLite (WAL, synchronous=FULL) — authoritative
  uploads/<id>.part        the upload, written in place at its true offset
  jobs/<id>/state.json     atomic mirror of the row, after every transition
  jobs/<id>/input/         extracted .lscan; deleted after a successful job
  jobs/<id>/output/        the worker's --out; zipped, then deleted on success
  jobs/<id>/worker.{stdout,stderr}.log   (capped at 8 MiB each)
  results/<id>.zip         served by GET /jobs/{id}/result
```

**SQLite is the job table; the filesystem is the bulk store.** Rows are tiny
and transitions are frequent, which is what a database is for; a 2 GiB upload
is what a file is for. `synchronous=FULL` plus an `fsync` at the end of every
accepted chunk is what makes the resume offset *true*: the server may only
report `Upload-Offset: N` if those N bytes will still be there after a power
cut, because the client will never send them again.

**`state.json` is a mirror, not a second source of truth.** Written
write-and-rename so a reader never sees a partial file. It exists so an
operator can `cat` a job, and so a data dir remains a readable record if the
database is lost. A failure to write it never fails a request.

**Atomicity rules.** `results/<id>.zip` is built under a temp name in the same
directory and `os.replace`d, so a reader either sees a complete bundle or no
bundle. An upload chunk that fails mid-write truncates back to the last
durable offset, so a partial write can never be mistaken for received bytes.

**Job ids are server-generated `uuid4().hex`** and every path derives from one
that has been matched against `^[0-9a-f]{32}$` first (`storage.Paths` raises
otherwise). That is why path traversal is *impossible* here rather than
*defended against*: no user-controlled string ever reaches a path join.

---

## 4. The upload endpoint

One PUT is one chunk, exactly as A15 §5 and `CloudSubmitClient::submit()`
describe. The interesting logic is what happens when a chunk is not the next
one:

| Situation | Response | Why |
| --- | --- | --- |
| `end < received` (a full duplicate) | `200`/`201` + `Upload-Offset` | The ack was lost, the bytes landed. A15's resume story *produces* duplicates; failing them would break the very case the probe exists for. |
| `start ≤ received ≤ end` (overlap) | write only the tail, `200`/`201` | Same reason, partial. |
| `start > received` (a gap) | `416` + `Upload-Offset` | We cannot invent bytes. `416` is in A15's "never retried" list, so it surfaces to the caller, and the header tells a smarter client where to go. |
| `total ≠ size_bytes` from POST | `400` | Otherwise the cap check at POST means nothing. |
| declared `total` > cap | `413` **before reading a byte** | |
| body longer/shorter than the range | `400`, truncated back | Size is enforced on bytes that *arrive*, not on headers. |
| job already `failed`/`cancelled` | `409` | |
| job `queued`/`running`/`done`, new bytes | can only be a duplicate (see row 1) | |

The **resume probe** (`Content-Range: bytes */<total>`, empty body) answers
`308` + `Upload-Offset` for every job whose bytes still mean something —
including one already `processing` or `done`, where the honest answer
(`offset == total`) is what lets a client whose *final* ack was lost finish
cleanly instead of concluding the upload failed. Only `failed`/`cancelled`
jobs refuse the probe (`409`).

The `308` carries **no `Location` header**: it is a "resume incomplete" status
in the Tus sense, not a redirect. The client keys off the status plus the
header, and Qt's `NoLessSafeRedirectPolicy` ignores a 3xx that has nowhere to
go. A reverse proxy must not rewrite it (`proxy_intercept_errors off`).

**5xx is a promise.** The client retries transport failures and 5xx only. So
the service answers 5xx only where a retry of the identical request is safe:
the one case is a failed chunk write, which rolls back to the last durable
offset first. Every client error is a 4xx the client will not hammer.

A per-job `asyncio.Lock` serializes concurrent PUTs on one job. The shipped
client is strictly sequential; the lock costs nothing and removes a whole
class of "two chunks raced" bug from the resume story.

---

## 5. The worker

One `asyncio.Task`, one `asyncio.Queue`, one child at a time — §3.8's "one
worker instance" is not a scheduler with a concurrency of 1, it is the
absence of a scheduler. `test_second_job_queues_behind_the_first` asserts
strict serialization (`first.finished_at ≤ second.started_at`), not "usually".

Per job:

1. **Extract** `uploads/<id>.part` into `jobs/<id>/input/`, entry by entry —
   never `ZipFile.extractall`. Rejected: absolute paths, drive letters, `..`
   in any component, NUL bytes, symlink entries, anything whose resolved path
   escapes the destination, and any archive that exceeds
   `LIDARSCAN_MAX_EXTRACT_BYTES` (checked against the central directory
   *and* counted while writing, because a header can lie).
2. **Find the `.lscan` root** — `manifest.json` at the extraction root, one
   level down (`MyScan.lscan/manifest.json`), or two; else the single
   top-level directory; else the root. A5's `zip_export()` bundles have been
   seen both ways and the *worker* is the right thing to reject a bad input.
3. **Exec** `LIDARSCAN_WORKER_CMD` with `{input}`/`{output}` substituted.
   `shlex`-split at startup and executed with `create_subprocess_exec` —
   **never a shell**, so a path with a `;` in it is a path.
   `start_new_session=True` gives the child its own process group, which is
   what makes a timeout or a `DELETE` able to kill a whole process tree.
4. **Progress** is parsed off stderr: `NN%  message`, with an optional
   leading `word:` so `engine_cli`'s `post:  42%  optimizing` matches and its
   `[scanengine][info]` lines do not. INT-34 §6 fixed stderr as the progress
   channel and stdout as the product, so stdout is logged and never parsed.
   Both pipes are drained continuously — a worker that fills a 64 KiB pipe
   buffer must not deadlock against its own progress reporting.
5. **Exit codes** map straight from INT-34 §6: `0` → package results and
   `done`; `3` → `cancelled`; `2` → `failed` ("rejected its arguments");
   anything else → `failed`, with the last stderr line as the reason. A
   timeout kills the group and fails with "timed out after Ns". A cancel
   requested by `DELETE` wins over whatever exit code the kill produced.
6. **Package**: `output/`'s contents are zipped to `results/<id>.zip`
   atomically. An empty output directory is still `done` — with
   "worker produced no output files" in the message, because the worker's exit
   code is the authority on success, not our opinion of its output.

On success the three staging copies — the `.part` upload, `input/` and
`output/` — are deleted, leaving `results/<id>.zip` as the durable artifact;
otherwise one 2 GiB capture would occupy the disk four times over.
`LIDARSCAN_KEEP_INPUTS=1` keeps them, and **a failed job always keeps
everything** for post-mortem.

**Restart.** There are no subprocesses after a restart, so `running` rows
become `failed` ("service restarted while this job was running") and `queued`
rows are re-enqueued. A graceful shutdown does the same thing for the job it
kills on the way out. An *upload* interrupted by a restart simply resumes —
the `.part` file and its `fsync`ed length survived.

---

## 6. Security (MVP minimums, all tested)

* **Constant-time token compare** (`hmac.compare_digest`), scheme parsed
  case-insensitively, `401` + `WWW-Authenticate: Bearer` for everything that
  is not an exact match. The token is never logged and never appears in a
  response body.
* **Every job route is authenticated**, including the result download — a
  point cloud is customer data, and a 32-hex id is not a capability.
  `/healthz` is the one unauthenticated route and returns counts only, no
  ids, no paths, no config secrets.
* **No path traversal anywhere**: server-generated uuid4 ids, validated
  before any join (§3); zip entries validated per-entry (§5).
* **Upload size enforced while streaming**, not just declared: the declared
  total is refused at `POST` and again at `PUT` before the body is read, and
  the bytes that actually arrive are counted, with a rollback if they exceed
  the range.
* **Zip bomb budget** enforced during extraction.
* **No shell**, ever, for the worker command.
* **Binds `127.0.0.1` by default**; TLS is a reverse proxy's job (README
  "Deploy"), with the systemd unit adding `ProtectSystem=strict`,
  `NoNewPrivileges`, a `ReadWritePaths` of exactly the data dir, and CPU/
  memory caps so one hostile upload cannot take the box.
* Data dir is created `0700`.

Not in the MVP and deliberately so: rate limiting (nginx), per-tenant
isolation, at-rest encryption, audit log. §3.8's boundaries.

---

## 7. Contract ambiguities in A15 §5, and how each was resolved

Every one of these was resolved so the shipped client sees success where it
deserves success.

1. **Which wire state covers "uploaded, waiting for the worker"?** Unspecified.
   Chosen `queued` (§2). A client can therefore observe `uploading → queued`,
   which reads backwards; it is harmless because the client only waits for a
   terminal state, and `message` says "queued for the worker".
2. **A duplicate chunk after a lost ack.** Unspecified, and the resume story
   guarantees it happens. Resolved: idempotent `200`/`201`, never `409`,
   never a second write.
3. **A gap.** Unspecified. Resolved: `416` + `Upload-Offset` (not `400`), so
   the offset the client needs is in the header it already parses.
4. **The probe against an already-complete upload.** Unspecified. Resolved:
   answer `308` with `offset == total`, so the "final ack lost" case
   terminates the client's loop successfully. Refusing it would strand a
   completed upload.
5. **`size_bytes` vs the PUT's `total`.** Unspecified. Resolved: they must
   match (`400` otherwise), because the POST-time cap check is otherwise
   trivially bypassed.
6. **`size_bytes` ≤ 0.** Unspecified. Resolved: `400` at create. A 0-byte
   declaration would make the client skip its upload loop entirely and then
   poll a job that can never leave `queued`; a fast refusal is the only
   honest answer.
7. **Is `upload_url` absolute or a path?** A15's example shows a path and the
   client does `base_url + upload_url`, so it stays root-relative.
   `LIDARSCAN_URL_PREFIX` covers subpath deployments.
8. **`GET /result` before the job is done.** A15 annotates `404` as "not
   ready yet", so a non-`done` job is `404`, not `409`/`425`.
9. **Cancellation.** A15 §5 has no verb for it, and the engine's own
   `kCloudSubmit` cancels only client-side. `DELETE /jobs/{id}` is an
   extension: `200` with the job status, `409` once the job is `done`,
   idempotent on an already-terminal job, and it kills the worker's process
   group.
10. **What `message` may contain.** The client parses status bodies with a
    hand-rolled reader. Messages are sanitized to printable ASCII without `"`
    or `\`, and capped, so no server-side string can ever confuse it.
11. **`308` and redirects.** Sending it without a `Location` is the
    Tus-style "resume incomplete" reading, and is what keeps Qt's redirect
    policy from doing anything with it (§4).

---

## 8. Verification

`pytest`, macOS 15 (Darwin 25.5.0), Python 3.12.13, from a clean venv:

```
89 passed in 5.50s
```

Coverage of note:

* `tests/client_sim.py` is `cloud_submit.cpp` ported to Python — its retry
  rule, its single probe, its status-code handling and its hand-rolled JSON
  reader. `tests/test_contract.py` runs the whole client algorithm against
  the service, including a **lost-ack disconnect** (both attempts land, both
  acks vanish, one probe, byte-exact result) and a **dead-connection**
  disconnect (nothing lands, the probe reports the old offset, the chunk is
  replayed).
* `tests/test_upload.py` — ranges, duplicates, overlap, gaps, oversize and
  undersize bodies with rollback, caps, `409`s.
* `tests/test_worker.py` — happy path, stderr progress reaching `GET /jobs`,
  crash (exit 1), usage (exit 2), self-cancel (exit 3), missing binary,
  cancel mid-run (asserting the child pid is really gone), cancel of a queued
  job, strict serialization of a second job, timeout, non-zip upload,
  zip-slip upload, zip bomb, graceful shutdown, restart recovery.
* `tests/test_api.py` — auth on all five job routes with seven bad headers
  each, `/healthz` leaking nothing, seven traversal-shaped ids, id
  unguessability, layout permissions, config validation.
* `tests/test_engine_cli_integration.py` — the **real** `engine_cli --post`
  (built here by `tools/build_engine_cli.sh`) on a real
  `engine_cli --synth-lscan` 2 s capture:
  `upload=11226 bytes in 4 PUTs (1 resume probe), result cloud.ply=1793 bytes,
  exit=0`, plus a junk-bundle run asserting `exit 1 → failed` with the CLI's
  own reason. Skips with an explanation when the binary is absent.

---

## 9. Known limitations / next pass

1. **In-process worker.** A crash of the web process kills the job (recovered
   as `failed` on restart). Splitting the worker into its own process — or
   D1's container — is the natural Phase 2 step, and the shape here (a queue
   table plus a `WORKER_CMD`) is what makes it a small change.
2. **No retention.** Results live forever. A cron `find -mtime` is the MVP
   answer; a real policy is Phase 2.
3. **Blocking file I/O in the event loop.** Chunk writes and their `fsync`
   run on the loop thread. At 8 MiB chunks on local disk this is microseconds
   to a few milliseconds; if the data dir ever becomes network storage, move
   the write into `asyncio.to_thread` (extraction and zipping already are).
4. **`upload_locks` is never pruned** — one `asyncio.Lock` per job id for the
   process's lifetime. Bounded by job count, measured in bytes; worth a sweep
   only if this service ever stops being single-tenant.
5. **No `Range` support on the result download.** A 2 GiB result restarts
   from zero if the download breaks. The client (`download_result()`) has no
   resume either, so fixing this needs both halves — A15's contract has no
   room for it today.
6. **No `--colorize`.** INT-34 §9.6 explains why the CLI has no flag yet
   (keyframe JPEGs in the bundle, a `sync_quality` decision only the capture
   side can make). When it lands it is one more `WORKER_CMD`, not a code
   change here.
