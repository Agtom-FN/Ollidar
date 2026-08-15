"""The job table: SQLite for queries, an atomic state.json per job for eyes.

SQLite is authoritative. ``state.json`` is a mirror written with
write-and-rename after every transition so that (a) an operator can `cat` a
job's state without a client, and (b) a data dir survives a lost database as
a readable record of what happened.
"""

from __future__ import annotations

import sqlite3
import threading
import time
import uuid
from dataclasses import asdict, dataclass
from typing import Any, Iterable

from .storage import Paths, atomic_write_json

# --- states ------------------------------------------------------------------
# Internal states are finer-grained than the wire vocabulary A15 defines,
# because the service has to distinguish "nothing uploaded yet" from "upload
# complete, waiting for the worker" and a client does not.
CREATED = "created"
UPLOADING = "uploading"
QUEUED = "queued"
RUNNING = "running"
DONE = "done"
FAILED = "failed"
CANCELLED = "cancelled"

TERMINAL = frozenset({DONE, FAILED, CANCELLED})

#: internal state -> the five strings `CloudJobState` parses
#: (engine/src/jobs/cloud_submit.cpp:state_from_string).
WIRE_STATE = {
    CREATED: "queued",
    UPLOADING: "uploading",
    QUEUED: "queued",
    RUNNING: "processing",
    DONE: "done",
    FAILED: "failed",
    # A15 has no "cancelled" on the wire: the engine folds cancellation into
    # kFailed + ScanError::kCancelled (docs/A15-jobs.md §2), so we do the same
    # and put the word in `message`.
    CANCELLED: "failed",
}

_ALLOWED_MESSAGE = set(range(0x20, 0x7F)) - {ord('"'), ord("\\")}
_MAX_MESSAGE = 200


def sanitize_message(text: str) -> str:
    """Printable ASCII, no quote or backslash, bounded length.

    The engine client parses status bodies with a deliberately non-general
    hand-rolled JSON reader (cloud_submit.cpp). Keeping every message value
    trivially quotable means no server-side string can ever confuse it.
    """
    cleaned = "".join(ch if ord(ch) in _ALLOWED_MESSAGE else " " for ch in text)
    cleaned = " ".join(cleaned.split())
    if len(cleaned) > _MAX_MESSAGE:
        cleaned = cleaned[: _MAX_MESSAGE - 3] + "..."
    return cleaned


@dataclass
class Job:
    id: str
    kind: str
    state: str
    size_bytes: int
    received_bytes: int
    progress: float
    message: str
    exit_code: int | None
    created_at: float
    updated_at: float
    started_at: float | None
    finished_at: float | None

    @property
    def wire_state(self) -> str:
        return WIRE_STATE[self.state]

    def status_body(self) -> dict[str, Any]:
        """Exactly the GET /jobs/{id} body A15 §5 specifies, plus extras.

        `id`, `state`, `progress`, `message` are the contract; the rest are
        additive and ignored by the engine client.
        """
        return {
            "id": self.id,
            "state": self.wire_state,
            "progress": round(max(0.0, min(1.0, self.progress)), 4),
            "message": self.message,
            "size_bytes": self.size_bytes,
            "received_bytes": self.received_bytes,
            "internal_state": self.state,
            "exit_code": self.exit_code,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
        }


_SCHEMA = """
CREATE TABLE IF NOT EXISTS jobs (
  id             TEXT PRIMARY KEY,
  kind           TEXT NOT NULL,
  state          TEXT NOT NULL,
  size_bytes     INTEGER NOT NULL,
  received_bytes INTEGER NOT NULL DEFAULT 0,
  progress       REAL    NOT NULL DEFAULT 0.0,
  message        TEXT    NOT NULL DEFAULT '',
  exit_code      INTEGER,
  created_at     REAL    NOT NULL,
  updated_at     REAL    NOT NULL,
  started_at     REAL,
  finished_at    REAL
);
CREATE INDEX IF NOT EXISTS jobs_state_created ON jobs(state, created_at);
"""

_FIELDS = (
    "id",
    "kind",
    "state",
    "size_bytes",
    "received_bytes",
    "progress",
    "message",
    "exit_code",
    "created_at",
    "updated_at",
    "started_at",
    "finished_at",
)


class JobStore:
    def __init__(self, paths: Paths) -> None:
        self._paths = paths
        paths.ensure()
        self._lock = threading.RLock()
        self._conn = sqlite3.connect(str(paths.db), check_same_thread=False, isolation_level=None)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=FULL")
        self._conn.executescript(_SCHEMA)

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    # --- writes --------------------------------------------------------------

    def create(self, size_bytes: int, kind: str = "lscan") -> Job:
        now = time.time()
        job = Job(
            id=uuid.uuid4().hex,
            kind=kind,
            state=CREATED,
            size_bytes=int(size_bytes),
            received_bytes=0,
            progress=0.0,
            message="created",
            exit_code=None,
            created_at=now,
            updated_at=now,
            started_at=None,
            finished_at=None,
        )
        with self._lock:
            self._conn.execute(
                f"INSERT INTO jobs ({','.join(_FIELDS)}) VALUES ({','.join('?' * len(_FIELDS))})",
                tuple(getattr(job, f) for f in _FIELDS),
            )
        self._paths.job_dir(job.id).mkdir(parents=True, exist_ok=True)
        self._mirror(job)
        return job

    def update(self, job_id: str, **fields: Any) -> Job | None:
        if "message" in fields and fields["message"] is not None:
            fields["message"] = sanitize_message(str(fields["message"]))
        if "state" in fields and fields["state"] in TERMINAL:
            fields.setdefault("finished_at", time.time())
        fields["updated_at"] = time.time()
        keys = [k for k in fields if k in _FIELDS and k != "id"]
        if not keys:
            return self.get(job_id)
        with self._lock:
            self._conn.execute(
                f"UPDATE jobs SET {','.join(k + '=?' for k in keys)} WHERE id=?",
                tuple(fields[k] for k in keys) + (job_id,),
            )
            job = self.get(job_id)
        if job is not None:
            self._mirror(job)
        return job

    def _mirror(self, job: Job) -> None:
        try:
            atomic_write_json(self._paths.state_file(job.id), asdict(job))
        except OSError:  # a mirror must never take the request down
            pass

    # --- reads ---------------------------------------------------------------

    def get(self, job_id: str) -> Job | None:
        with self._lock:
            row = self._conn.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
        return None if row is None else Job(**{f: row[f] for f in _FIELDS})

    def ids_in_state(self, *states: str) -> list[str]:
        marks = ",".join("?" * len(states))
        with self._lock:
            rows = self._conn.execute(
                f"SELECT id FROM jobs WHERE state IN ({marks}) ORDER BY created_at", states
            ).fetchall()
        return [r["id"] for r in rows]

    def counts(self) -> dict[str, int]:
        with self._lock:
            rows = self._conn.execute("SELECT state, COUNT(*) n FROM jobs GROUP BY state").fetchall()
        return {r["state"]: r["n"] for r in rows}

    def all_jobs(self) -> Iterable[Job]:
        with self._lock:
            rows = self._conn.execute("SELECT * FROM jobs ORDER BY created_at").fetchall()
        return [Job(**{f: row[f] for f in _FIELDS}) for row in rows]
