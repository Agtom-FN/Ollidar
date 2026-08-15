"""The REST surface — exactly engine/docs/A15-jobs.md §5, plus /healthz and
a DELETE (cancel) the MVP worker needs and the contract did not specify.

    POST   /jobs                  -> 201 {"id", "upload_url"}
    PUT    /jobs/{id}/upload      -> 200/201 (+ Upload-Offset), 308 on the probe
    GET    /jobs/{id}             -> 200 {"id","state","progress","message"}
    GET    /jobs/{id}/result      -> 200 application/zip
    DELETE /jobs/{id}             -> 200 (cancel; extension)
    GET    /healthz               -> 200 (unauthenticated liveness)
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import re
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse, Response

from .config import Config
from .storage import Paths, is_job_id
from .store import (
    CANCELLED,
    CREATED,
    DONE,
    FAILED,
    QUEUED,
    RUNNING,
    TERMINAL,
    UPLOADING,
    JobStore,
)
from .worker import Worker

log = logging.getLogger("lidarscan.api")

#: `bytes <start>-<end>/<total>` (a data chunk) or `bytes */<total>` (the
#: resume-offset probe cloud_submit.cpp issues when a chunk's retries are
#: exhausted).
_CONTENT_RANGE_RE = re.compile(r"^bytes\s+(?:(?P<start>\d+)-(?P<end>\d+)|\*)/(?P<total>\d+)$")

_UPLOAD_BLOCK_HINT = 1024 * 1024


def _err(status: int, message: str, headers: dict[str, str] | None = None) -> JSONResponse:
    return JSONResponse({"error": message}, status_code=status, headers=headers or {})


def _unauthorized() -> JSONResponse:
    return _err(401, "unauthorized", {"WWW-Authenticate": "Bearer"})


def _authorized(request: Request) -> bool:
    """Constant-time bearer-token check. No timing oracle, no logging of it."""
    cfg: Config = request.app.state.cfg
    header = request.headers.get("authorization", "")
    scheme, _, value = header.partition(" ")
    if scheme.lower() != "bearer":
        return False
    return hmac.compare_digest(value.strip(), cfg.token)


def create_app(cfg: Config | None = None) -> FastAPI:
    from contextlib import asynccontextmanager

    cfg = cfg or Config.from_env()
    paths = Paths(cfg.data_dir)
    paths.ensure()
    store = JobStore(paths)

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        from .worker import recover_on_startup

        worker = Worker(cfg, store, paths)
        app.state.worker = worker
        recover_on_startup(store, worker)
        await worker.start()
        try:
            yield
        finally:
            await worker.stop()

    app = FastAPI(title="LidarScan cloud job service", version="0.1.0", lifespan=lifespan)
    app.state.cfg = cfg
    app.state.paths = paths
    app.state.store = store
    app.state.upload_locks = {}

    # ---------------------------------------------------------------- health

    @app.get("/healthz")
    async def healthz() -> JSONResponse:
        # Unauthenticated on purpose: a load balancer / systemd watchdog needs
        # it, and it reveals only counts, never ids or paths.
        worker: Worker = app.state.worker
        return JSONResponse(
            {
                "status": "ok",
                "version": app.version,
                "states": store.counts(),
                "queue_depth": worker.queue_depth(),
                "busy": worker.current_job_id is not None,
                "max_upload_bytes": cfg.max_upload_bytes,
            }
        )

    # ------------------------------------------------------------ POST /jobs

    @app.post("/jobs")
    async def create_job(request: Request) -> Response:
        if not _authorized(request):
            return _unauthorized()
        raw = await request.body()
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            return _err(400, "body is not JSON")
        if not isinstance(body, dict):
            return _err(400, "body must be a JSON object")

        kind = body.get("kind", "lscan")
        if kind != "lscan":
            return _err(400, f"unsupported kind {kind!r}; this MVP accepts 'lscan'")

        size_raw = body.get("size_bytes")
        try:
            size_bytes = int(size_raw)
        except (TypeError, ValueError):
            return _err(400, "size_bytes is required and must be an integer")
        if size_bytes <= 0:
            # A zero-byte upload can never become a .lscan, and a client that
            # declared 0 would never send a PUT — so it would poll a job that
            # can never leave 'queued'. Refusing here is the honest answer.
            return _err(400, "size_bytes must be > 0")
        if size_bytes > cfg.max_upload_bytes:
            return _err(
                413,
                f"{size_bytes} bytes exceeds this server's {cfg.max_upload_bytes}-byte cap",
            )

        job = store.create(size_bytes=size_bytes, kind=kind)
        paths.upload_part(job.id).write_bytes(b"")
        log.info("job %s created (%d bytes declared)", job.id, size_bytes)
        return JSONResponse(
            {"id": job.id, "upload_url": f"{cfg.url_prefix}/jobs/{job.id}/upload"},
            status_code=201,
        )

    # -------------------------------------------------- PUT /jobs/{id}/upload

    @app.put("/jobs/{job_id}/upload")
    async def upload(job_id: str, request: Request) -> Response:
        if not _authorized(request):
            return _unauthorized()
        if not is_job_id(job_id):
            return _err(404, "no such job")
        job = store.get(job_id)
        if job is None:
            return _err(404, "no such job")

        header = request.headers.get("content-range")
        if not header:
            return _err(400, "Content-Range is required (bytes <start>-<end>/<total>)")
        m = _CONTENT_RANGE_RE.match(header.strip())
        if m is None:
            return _err(400, f"malformed Content-Range: {header!r}")
        total = int(m.group("total"))

        # Cap check before a single body byte is read.
        if total > cfg.max_upload_bytes:
            return _err(413, f"{total} bytes exceeds this server's {cfg.max_upload_bytes}-byte cap")
        if total != job.size_bytes:
            return _err(
                400,
                f"Content-Range total {total} does not match the {job.size_bytes} declared at POST /jobs",
            )

        is_probe = m.group("start") is None
        lock = app.state.upload_locks.setdefault(job_id, asyncio.Lock())
        async with lock:
            job = store.get(job_id)  # re-read under the lock
            if job is None:
                return _err(404, "no such job")

            if is_probe:
                # The resume probe. Answer it for any job whose bytes still
                # mean something — including one already processing, where the
                # honest answer (offset == total) is what lets a client whose
                # final ack was lost finish cleanly instead of re-uploading.
                if job.state in (FAILED, CANCELLED):
                    return _err(409, f"job is {job.wire_state} and no longer accepts upload")
                # No Location header: this is a Tus-style "resume incomplete"
                # 308, not a redirect. The engine client keys off the status
                # plus Upload-Offset (cloud_submit.cpp), and Qt's redirect
                # policy ignores a 3xx without a Location.
                return Response(status_code=308, headers={"Upload-Offset": str(job.received_bytes)})

            start, end = int(m.group("start")), int(m.group("end"))
            if end < start:
                return _err(400, f"inverted Content-Range: {header!r}")
            if end >= total:
                return _err(400, f"Content-Range end {end} is past the declared total {total}")
            declared_len = end - start + 1

            received = job.received_bytes

            # A chunk entirely below the received offset is a duplicate: the
            # ack was lost, the bytes landed. Acknowledge it idempotently
            # rather than 409-ing a client that did nothing wrong.
            if end < received:
                await _drain(request)
                return _offset_response(received, total)

            if job.state not in (CREATED, UPLOADING):
                return _err(409, f"job is {job.wire_state} and no longer accepts upload")

            if start > received:
                # A gap: we cannot write bytes we never got. Tell the client
                # where we actually are, in the header it already parses.
                await _drain(request)
                return _err(
                    416,
                    f"upload has {received} bytes; chunk starts at {start}",
                    {"Upload-Offset": str(received)},
                )

            part = paths.upload_part(job_id)
            part.parent.mkdir(parents=True, exist_ok=True)
            if not part.exists():
                part.write_bytes(b"")

            body_seen = 0
            new_received = received
            try:
                with open(part, "r+b") as fh:
                    pos = start
                    async for block in request.stream():
                        if not block:
                            continue
                        body_seen += len(block)
                        # Size is enforced on the bytes that actually arrive,
                        # not on what the headers claimed.
                        if body_seen > declared_len:
                            raise _UploadRejected(
                                400, f"body is longer than the {declared_len}-byte Content-Range"
                            )
                        blk_end = pos + len(block)
                        if blk_end > cfg.max_upload_bytes:
                            raise _UploadRejected(413, "upload exceeded the server cap mid-stream")
                        if blk_end > new_received:
                            skip = max(0, new_received - pos)
                            fh.seek(pos + skip)
                            fh.write(block[skip:])
                            new_received = blk_end
                        pos = blk_end
                    if body_seen != declared_len:
                        raise _UploadRejected(
                            400,
                            f"body is {body_seen} bytes, Content-Range declared {declared_len}",
                        )
                    fh.flush()
                    os.fsync(fh.fileno())
            except _UploadRejected as rejected:
                _truncate(part, received)
                return _err(rejected.status, rejected.message, {"Upload-Offset": str(received)})
            except (OSError, ConnectionError) as exc:
                _truncate(part, received)
                log.warning("job %s: upload chunk failed: %s", job_id, exc)
                return _err(500, "could not persist the chunk", {"Upload-Offset": str(received)})

            if new_received >= total:
                store.update(
                    job_id,
                    received_bytes=total,
                    state=QUEUED,
                    progress=0.0,
                    message="queued for the worker",
                )
                app.state.worker.enqueue(job_id)
                log.info("job %s: upload complete (%d bytes), queued", job_id, total)
                return _offset_response(total, total)

            store.update(
                job_id,
                received_bytes=new_received,
                state=UPLOADING,
                progress=new_received / total,
                message="uploading",
            )
            return _offset_response(new_received, total)

    # -------------------------------------------------------- GET /jobs/{id}

    @app.get("/jobs/{job_id}")
    async def job_status(job_id: str, request: Request) -> Response:
        if not _authorized(request):
            return _unauthorized()
        if not is_job_id(job_id):
            return _err(404, "no such job")
        job = store.get(job_id)
        if job is None:
            return _err(404, "no such job")
        return JSONResponse(job.status_body())

    # ------------------------------------------------- GET /jobs/{id}/result

    @app.get("/jobs/{job_id}/result")
    async def job_result(job_id: str, request: Request) -> Response:
        if not _authorized(request):
            return _unauthorized()
        if not is_job_id(job_id):
            return _err(404, "no such job")
        job = store.get(job_id)
        if job is None:
            return _err(404, "no such job")
        result = paths.result_zip(job_id)
        if job.state != DONE or not result.is_file():
            # A15: "404 (not ready yet)" — the client reads 404 as exactly that.
            return _err(404, f"no result available (job is {job.wire_state})")
        return FileResponse(
            path=str(result),
            media_type="application/zip",
            filename=f"{job_id}-result.zip",
        )

    # ----------------------------------------------------- DELETE /jobs/{id}

    @app.delete("/jobs/{job_id}")
    async def cancel_job(job_id: str, request: Request) -> Response:
        if not _authorized(request):
            return _unauthorized()
        if not is_job_id(job_id):
            return _err(404, "no such job")
        job = store.get(job_id)
        if job is None:
            return _err(404, "no such job")
        if job.state == DONE:
            return _err(409, "job already finished; nothing to cancel")
        if job.state in TERMINAL:
            return JSONResponse(job.status_body())  # idempotent

        worker: Worker = app.state.worker
        worker.request_cancel(job_id)
        if job.state != RUNNING:
            # Nothing to kill: settle it here so a queued job never starts.
            store.update(job_id, state=CANCELLED, message="cancelled by client")
        else:
            store.update(job_id, message="cancelling")
        updated = store.get(job_id)
        return JSONResponse(updated.status_body() if updated else {"id": job_id})

    return app


class _UploadRejected(Exception):
    def __init__(self, status: int, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.message = message


def _offset_response(received: int, total: int) -> Response:
    """200 while more is expected, 201 when the upload is complete (A15 §5)."""
    return Response(
        status_code=201 if received >= total else 200,
        headers={"Upload-Offset": str(received)},
    )


def _truncate(part: Path, size: int) -> None:
    try:
        with open(part, "r+b") as fh:
            fh.truncate(size)
            fh.flush()
            os.fsync(fh.fileno())
    except OSError:  # pragma: no cover
        pass


async def _drain(request: Request) -> None:
    """Read and discard a body we are not going to store."""
    try:
        async for _ in request.stream():
            pass
    except (OSError, ConnectionError, RuntimeError):  # pragma: no cover
        pass
