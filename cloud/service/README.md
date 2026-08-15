# LidarScan cloud job service (D2, MVP)

The server half of Tech Spec §3.8's **Cloud** processing mode: a phone or
desktop uploads a `.lscan.zip`, a Linux worker runs the engine CLI on it, and
the app downloads the results.

It implements `engine/docs/A15-jobs.md` §5 exactly — that contract is already
shipped in `engine/src/jobs/cloud_submit.cpp` and driven by
`desktop/src/app/QtHttpTransport.cpp`, so the client is the fixed point and
this service moves to meet it. See `DESIGN.md` for the state machine, the
storage layout and every contract decision (including the ambiguities A15 left
open and how each was resolved).

**MVP boundaries, contractual (Tech Spec §3.8):** single-tenant (one owner
token), token auth, **no payments or quotas**, **one worker instance**, hard
upload-size cap. Productionizing is Phase 2+.

---

## Quick start

```bash
cd cloud/service
python3.12 -m venv .venv                 # 3.11+ required
.venv/bin/pip install -r requirements.txt

export LIDARSCAN_TOKEN="$(python3 -c 'import secrets;print(secrets.token_urlsafe(32))')"
export LIDARSCAN_DATA_DIR=./data
export LIDARSCAN_WORKER_CMD="/path/to/engine_cli --post {input} --out {output}"

.venv/bin/python -m lidarscan_service        # or:
.venv/bin/uvicorn lidarscan_service.asgi:app --host 127.0.0.1 --port 8080
```

Smoke test it:

```bash
curl -s localhost:8080/healthz
curl -s -X POST localhost:8080/jobs \
     -H "Authorization: Bearer $LIDARSCAN_TOKEN" \
     -H 'Content-Type: application/json' \
     -d '{"kind":"lscan","size_bytes":1234}'
# {"id":"<32 hex>","upload_url":"/jobs/<32 hex>/upload"}
```

Point the desktop/Android app at `http://127.0.0.1:8080` (its
`CloudSubmitConfig::base_url`) with the same token and it works end to end.

---

## API

All job routes need `Authorization: Bearer <LIDARSCAN_TOKEN>`; a wrong or
missing token is `401` with `WWW-Authenticate: Bearer`.

| Method | Path | Body / headers | Success | Errors |
| --- | --- | --- | --- | --- |
| `POST` | `/jobs` | `{"kind":"lscan","size_bytes":N}` | `201 {"id","upload_url"}` | `400`, `401`, `413` over cap |
| `PUT` | `{upload_url}` | `Content-Range: bytes <start>-<end>/<total>` + chunk | `200` + `Upload-Offset` (more expected) · `201` + `Upload-Offset` (complete, processing starts) | `400`, `401`, `404`, `409`, `413`, `416` + `Upload-Offset` |
| `PUT` | `{upload_url}` | `Content-Range: bytes */<total>`, empty body — the **resume probe** | `308` + `Upload-Offset: <bytes we actually have>` | `401`, `404`, `409` |
| `GET` | `/jobs/{id}` | — | `200 {"id","state","progress","message",…}` | `401`, `404` |
| `GET` | `/jobs/{id}/result` | — | `200` `application/zip` | `401`, `404` (also "not ready yet") |
| `DELETE` | `/jobs/{id}` | — | `200` job status (cancelled) | `401`, `404`, `409` if already done |
| `GET` | `/healthz` | no auth | `200` counts + queue depth | — |

`state` is one of `queued · uploading · processing · done · failed` — the five
strings `CloudJobState` parses. There is no `cancelled` on the wire: like the
engine (A15 §2), a cancelled job settles into `failed` with the word in
`message`. `GET /jobs/{id}` also returns `internal_state`, `received_bytes`,
`size_bytes` and `exit_code`, which the engine client ignores.

`DELETE` is an **extension** — A15 §5 defines no cancel verb, and one worker
with a hard job timeout needs one. It kills the worker's process group.

---

## Configuration

| Variable | Default | Meaning |
| --- | --- | --- |
| `LIDARSCAN_TOKEN` | — (**required**) | The single tenant's bearer token. The service refuses to start without it, and warns below 16 chars. |
| `LIDARSCAN_DATA_DIR` | `./data` | Uploads, job state, results. Created `0700`. |
| `LIDARSCAN_MAX_UPLOAD_BYTES` | `2147483648` (2 GiB) | §3.8's hard cap; same number as the client's own pre-flight check. |
| `LIDARSCAN_MAX_EXTRACT_BYTES` | `8 ×` the upload cap | Zip-bomb budget, enforced while extracting. |
| `LIDARSCAN_WORKER_CMD` | `engine_cli --post {input} --out {output}` | argv template (`shlex`-split, **never** a shell). Both placeholders are required. |
| `LIDARSCAN_JOB_TIMEOUT_S` | `3600` | Kill the worker's process group after this. |
| `LIDARSCAN_HOST` / `LIDARSCAN_PORT` | `127.0.0.1` / `8080` | Bind address. Localhost on purpose. |
| `LIDARSCAN_URL_PREFIX` | — | Prefix reflected in `upload_url` when a proxy mounts the service on a subpath. |
| `LIDARSCAN_KEEP_INPUTS` | `0` | Keep extracted inputs after a *successful* job (failures always keep theirs). |

## Data directory

```
data/
  jobs.db                  SQLite job table (WAL)
  uploads/<id>.part        the resumable upload, in place
  jobs/<id>/state.json     atomic mirror of the job row
  jobs/<id>/input/         extracted .lscan (deleted after success)
  jobs/<id>/output/        the worker's --out directory
  jobs/<id>/worker.{stdout,stderr}.log
  results/<id>.zip         what GET /jobs/{id}/result serves
```

Back up `data/` — that is the whole service's state. Deleting a `results/*.zip`
turns its job's result endpoint into `404`; nothing else breaks.

## The worker contract

`LIDARSCAN_WORKER_CMD` is executed with `{input}` = the extracted `.lscan`
directory and `{output}` = a directory that starts empty. It must:

* write progress to **stderr** as `NN%  message` (a leading `word:` is fine —
  `engine_cli` writes `post:  42%  optimizing`),
* write its product into `{output}` (that directory's contents become the
  result zip),
* exit `0` ok · `1` failed · `2` usage · `3` cancelled (INT-34 §6).

Anything else on stderr is logged and ignored, so the engine's `[scanengine]`
log lines cost nothing.

Build the engine CLI for this machine with `tools/build_engine_cli.sh`
(it lands in `.engine-build/engine_cli`); the containerized Linux worker image
is D1's.

---

## Tests

```bash
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pytest                 # 89 passed in ~5.5 s
```

* `tests/client_sim.py` is a **port of `engine/src/jobs/cloud_submit.cpp`** —
  same request order, same retry rule (5xx and transport failures only), same
  single resume probe, same hand-rolled JSON reader. `tests/test_contract.py`
  drives it, so a drift from the shipped client fails the suite.
* `tests/fake_worker.py` stands in for the engine CLI everywhere else.
* `tests/test_engine_cli_integration.py` runs the **real** `engine_cli --post`
  on a real `engine_cli --synth-lscan` capture if the binary is at
  `$LIDARSCAN_ENGINE_CLI`, `.engine-build/engine_cli` or `engine/build/*/engine_cli`;
  it **skips with an explanation** otherwise.

---

## Deploy

Single box, systemd, TLS terminated by nginx:

```bash
# 1. code + venv
sudo install -d -o lidarscan -g lidarscan /opt/lidarscan/service /var/lib/lidarscan
sudo -u lidarscan cp -r lidarscan_service requirements.txt /opt/lidarscan/service/
sudo -u lidarscan python3 -m venv /opt/lidarscan/service/.venv
sudo -u lidarscan /opt/lidarscan/service/.venv/bin/pip install -r /opt/lidarscan/service/requirements.txt

# 2. the worker binary (D1's container image is the alternative)
sudo install -m 0755 engine_cli /opt/lidarscan/bin/engine_cli

# 3. config + unit
sudo install -d -m 0700 /etc/lidarscan
sudo install -m 0600 deploy/lidarscan.env.example /etc/lidarscan/service.env
sudoedit /etc/lidarscan/service.env            # set LIDARSCAN_TOKEN
sudo cp deploy/lidarscan-service.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now lidarscan-service

# 4. TLS in front
sudo cp deploy/nginx-lidarscan.conf /etc/nginx/sites-available/lidarscan
sudo ln -s ../sites-available/lidarscan /etc/nginx/sites-enabled/ && sudo nginx -t && sudo systemctl reload nginx
```

**Reverse-proxy TLS is not optional.** The bearer token is sent on every
request and the service has no TLS stack; it binds `127.0.0.1` so that the
only way in is through the proxy. In nginx, keep
`proxy_request_buffering off` (the service enforces the size cap *while*
streaming — buffering hides that until the whole chunk has landed) and let
`308` responses through untouched.

Operational notes:

* **One worker.** A second job waits. `GET /healthz` reports `queue_depth`.
* **Restart behaviour.** A job that was running when the process died comes
  back `failed` ("service restarted…"); jobs that were queued are re-queued.
  An upload interrupted by a restart resumes: the client's probe reads the
  offset off the file that survived.
* **Disk.** A successful job releases its staging copies (upload, extracted
  input, worker output) and keeps only `results/<id>.zip`; a failed job keeps
  everything for post-mortem. Results are kept forever. Prune `results/` and `jobs/` by age when it matters —
  there is no retention policy in the MVP, on purpose.
* **Logs.** `journalctl -u lidarscan-service` for the service; per-job worker
  output is in `data/jobs/<id>/worker.stderr.log`.

### Not in this MVP (deliberately)

Multi-tenant accounts, per-user quotas, payments, more than one worker,
horizontal scale, object storage (S3/GCS), result retention/expiry,
rate limiting (nginx's job), and metrics/tracing. §3.8 draws that line and
Risk "cloud service scope creep" in §5 makes it contractual.
