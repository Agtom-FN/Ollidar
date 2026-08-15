"""The worker: one in-process asyncio task, one subprocess at a time.

Tech Spec §3.8's MVP boundary is "one worker instance", so this is literally
one `asyncio.Task` pulling job ids off one `asyncio.Queue` and running one
child process. A second job submitted while the first runs waits its turn —
the queueing is the absence of concurrency, not a scheduler.

Per job:
  1. zip-slip-safe extract of `uploads/<id>.part` into `jobs/<id>/input/`
  2. locate the `.lscan` root inside it
  3. exec LIDARSCAN_WORKER_CMD with {input}/{output} substituted, in its own
     process group (so a timeout or a DELETE kills the whole tree)
  4. parse `NN%  message` progress lines off stderr into job progress
  5. exit 0 -> zip `output/` into `results/<id>.zip` and go `done`;
     exit 3 -> `cancelled`; anything else -> `failed` with the reason
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import re
import signal
import time
import zipfile
from pathlib import Path

from .config import Config
from .storage import Paths, UnsafeArchiveError, find_lscan_root, rmtree_quiet, safe_extract, zip_directory
from .store import CANCELLED, DONE, FAILED, QUEUED, RUNNING, JobStore

log = logging.getLogger("lidarscan.worker")

#: `engine_cli --post` writes `post: %3d%%  <message>` to stderr (INT-34 §6:
#: "Progress goes to stderr, results to stdout"). The optional leading
#: `word:` makes this work for any worker that follows the same shape.
_PROGRESS_RE = re.compile(
    r"^\s*(?:(?P<stage>[A-Za-z0-9_.+\-]{1,32}):\s*)?(?P<pct>\d{1,3})\s*%\s*(?P<msg>.*)$"
)

#: engine_cli's documented exit codes (INT-34 §6): 0 ok, 1 failed, 2 usage,
#: 3 cancelled.
EXIT_OK = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_CANCELLED = 3

_LOG_CAP_BYTES = 8 * 1024 * 1024
_READ_BLOCK = 8192


def parse_progress(line: str) -> tuple[float, str] | None:
    """`'post:  42%  optimizing'` -> `(0.42, 'optimizing')`, else None."""
    m = _PROGRESS_RE.match(line)
    if m is None:
        return None
    pct = int(m.group("pct"))
    if pct > 100:
        return None
    msg = m.group("msg").strip()
    if not msg:
        msg = m.group("stage") or "processing"
    return pct / 100.0, msg


class Worker:
    def __init__(self, cfg: Config, store: JobStore, paths: Paths) -> None:
        self._cfg = cfg
        self._store = store
        self._paths = paths
        self._queue: asyncio.Queue[str] = asyncio.Queue()
        self._task: asyncio.Task | None = None
        self._proc: asyncio.subprocess.Process | None = None
        self._current: str | None = None
        self._cancel_requested: set[str] = set()
        self._idle = asyncio.Event()
        self._idle.set()

    # --- lifecycle -----------------------------------------------------------

    async def start(self) -> None:
        if self._task is None:
            self._task = asyncio.create_task(self._loop(), name="lidarscan-worker")

    async def stop(self) -> None:
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        self._kill_current()

    @property
    def current_job_id(self) -> str | None:
        return self._current

    def queue_depth(self) -> int:
        return self._queue.qsize()

    def enqueue(self, job_id: str) -> None:
        self._queue.put_nowait(job_id)

    async def wait_idle(self) -> None:
        """Test/ops helper: returns when the queue is drained and nothing runs."""
        await self._queue.join()
        await self._idle.wait()

    # --- cancellation --------------------------------------------------------

    def request_cancel(self, job_id: str) -> None:
        """Mark a job cancelled; kill its subprocess if it is the running one.

        A queued job is skipped when the loop reaches it (its state is no
        longer QUEUED), which is why nothing has to be removed from the queue.
        """
        self._cancel_requested.add(job_id)
        if self._current == job_id:
            self._kill_current()

    def _kill_current(self) -> None:
        proc = self._proc
        if proc is None or proc.returncode is not None:
            return
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError, OSError):
            with contextlib.suppress(ProcessLookupError, OSError):
                proc.kill()

    # --- the loop ------------------------------------------------------------

    async def _loop(self) -> None:
        while True:
            job_id = await self._queue.get()
            self._idle.clear()
            try:
                job = self._store.get(job_id)
                if job is None:
                    continue
                if job.state != QUEUED:
                    # Cancelled (or otherwise resolved) while it waited.
                    self._cancel_requested.discard(job_id)
                    continue
                await self._run_job(job_id)
            except asyncio.CancelledError:
                current = self._store.get(job_id)
                if current is not None and current.state == RUNNING:
                    self._fail(job_id, "service stopped while the job was running")
                raise
            except Exception as exc:  # a worker bug must not kill the queue
                log.exception("job %s crashed the worker loop", job_id)
                self._fail(job_id, f"internal error: {type(exc).__name__}")
            finally:
                self._current = None
                self._proc = None
                self._queue.task_done()
                if self._queue.empty():
                    self._idle.set()

    def _fail(self, job_id: str, message: str, exit_code: int | None = None) -> None:
        self._store.update(job_id, state=FAILED, message=message, exit_code=exit_code)

    async def _run_job(self, job_id: str) -> None:
        cfg, paths, store = self._cfg, self._paths, self._store
        self._current = job_id
        store.update(job_id, state=RUNNING, started_at=time.time(), progress=0.0, message="extracting upload")

        input_dir = paths.input_dir(job_id)
        output_dir = paths.output_dir(job_id)
        rmtree_quiet(input_dir)
        input_dir.mkdir(parents=True, exist_ok=True)
        output_dir.mkdir(parents=True, exist_ok=True)

        try:
            await asyncio.to_thread(
                safe_extract, paths.upload_part(job_id), input_dir, cfg.max_extract_bytes
            )
        except UnsafeArchiveError as exc:
            self._fail(job_id, f"rejected upload: {exc}")
            return
        except (zipfile.BadZipFile, OSError, EOFError) as exc:
            self._fail(job_id, f"upload is not a readable zip: {exc}")
            return

        if job_id in self._cancel_requested:
            self._cancel_requested.discard(job_id)
            store.update(job_id, state=CANCELLED, message="cancelled before the worker started")
            return

        lscan_root = await asyncio.to_thread(find_lscan_root, input_dir)
        argv = cfg.render_worker_cmd(lscan_root, output_dir)
        store.update(job_id, progress=0.0, message="starting worker")
        log.info("job %s: exec %s", job_id, " ".join(argv))

        try:
            proc = await asyncio.create_subprocess_exec(
                *argv,
                cwd=str(paths.job_dir(job_id)),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                start_new_session=True,  # own process group -> killpg works
            )
        except (FileNotFoundError, PermissionError, OSError) as exc:
            self._fail(job_id, f"cannot start worker command: {exc}")
            return

        self._proc = proc
        last_lines: list[str] = []
        state = {"pct": -1, "msg": ""}

        def on_stderr_line(line: str) -> None:
            text = line.rstrip("\r")
            if text.strip():
                last_lines.append(text.strip())
                del last_lines[:-5]
            parsed = parse_progress(text)
            if parsed is None:
                return
            progress, msg = parsed
            pct = int(round(progress * 100))
            if pct == state["pct"] and msg == state["msg"]:
                return
            state["pct"], state["msg"] = pct, msg
            store.update(job_id, progress=progress, message=msg)

        stderr_task = asyncio.create_task(
            self._pump(proc.stderr, paths.stderr_log(job_id), on_stderr_line)
        )
        stdout_task = asyncio.create_task(self._pump(proc.stdout, paths.stdout_log(job_id), None))

        timed_out = False
        try:
            rc = await asyncio.wait_for(proc.wait(), timeout=cfg.job_timeout_s)
        except asyncio.TimeoutError:
            timed_out = True
            self._kill_current()
            rc = await proc.wait()
        except BaseException:
            # Shutdown (or any other cancellation): kill the child first, then
            # drop the pumps. Awaiting a pump here would block forever on a
            # process that is still holding its pipes open.
            self._kill_current()
            for t in (stderr_task, stdout_task):
                t.cancel()
            raise
        # The child is gone, so both pipes are at EOF and the pumps finish
        # immediately; the timeout is a belt-and-braces guard against an
        # inherited pipe held open by a grandchild.
        _, pending = await asyncio.wait({stderr_task, stdout_task}, timeout=5.0)
        for t in pending:
            t.cancel()
        for t in (stderr_task, stdout_task):
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await t

        tail = last_lines[-1] if last_lines else ""
        cancelled = job_id in self._cancel_requested
        self._cancel_requested.discard(job_id)

        if cancelled:
            store.update(job_id, state=CANCELLED, message="cancelled by client", exit_code=rc)
            return
        if timed_out:
            self._fail(job_id, f"worker timed out after {cfg.job_timeout_s:g}s", exit_code=rc)
            return
        if rc == EXIT_CANCELLED:
            store.update(job_id, state=CANCELLED, message="worker reported cancellation", exit_code=rc)
            return
        if rc == EXIT_USAGE:
            self._fail(job_id, f"worker rejected its arguments (exit 2): {tail}", exit_code=rc)
            return
        if rc != EXIT_OK:
            reason = f"worker exited with code {rc}"
            self._fail(job_id, f"{reason}: {tail}" if tail else reason, exit_code=rc)
            return

        try:
            entries = await asyncio.to_thread(zip_directory, output_dir, paths.result_zip(job_id))
        except OSError as exc:
            self._fail(job_id, f"could not package results: {exc}", exit_code=rc)
            return

        message = "done" if entries else "done (worker produced no output files)"
        store.update(job_id, state=DONE, progress=1.0, message=message, exit_code=rc)
        if not cfg.keep_inputs:
            # Success only: a failed job keeps everything for post-mortem.
            # The result zip is now the durable artifact, so the staging
            # copies (upload, extracted input, worker output) go — otherwise a
            # 2 GiB capture occupies the disk four times over.
            rmtree_quiet(input_dir)
            rmtree_quiet(output_dir)
            with contextlib.suppress(OSError):
                paths.upload_part(job_id).unlink()

    async def _pump(self, stream, log_path: Path, on_line) -> None:
        """Drain a pipe to a log file, optionally calling `on_line` per line."""
        buf = b""
        written = 0
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(log_path, "ab") as fh:
            while True:
                try:
                    block = await stream.read(_READ_BLOCK)
                except (asyncio.CancelledError, GeneratorExit):
                    raise
                except Exception:  # pragma: no cover - pipe torn down by a kill
                    break
                if not block:
                    break
                if written < _LOG_CAP_BYTES:
                    fh.write(block)
                    written += len(block)
                if on_line is None:
                    continue
                buf += block
                if b"\n" in buf:
                    *lines, buf = buf.split(b"\n")
                    for raw in lines:
                        on_line(raw.decode("utf-8", "replace"))
                if len(buf) > _READ_BLOCK * 4:  # a line with no newline in sight
                    on_line(buf.decode("utf-8", "replace"))
                    buf = b""
        if on_line is not None and buf:
            on_line(buf.decode("utf-8", "replace"))


def recover_on_startup(store: JobStore, worker: Worker) -> None:
    """A restart has no subprocesses: running -> failed, queued -> re-queued."""
    for job_id in store.ids_in_state(RUNNING):
        store.update(
            job_id, state=FAILED, message="service restarted while this job was running"
        )
    for job_id in store.ids_in_state(QUEUED):
        worker.enqueue(job_id)
