"""Filesystem layout, atomic writes, and zip-slip-safe extraction.

Layout under ``LIDARSCAN_DATA_DIR``::

    data/
      jobs.db                    SQLite job table (authoritative state)
      uploads/<id>.part          the resumable upload, appended in place
      jobs/<id>/state.json       atomic mirror of the job row (audit/recovery)
      jobs/<id>/input/           extracted .lscan (never trusted, never reused)
      jobs/<id>/output/          the worker's --out directory
      jobs/<id>/worker.stderr.log
      jobs/<id>/worker.stdout.log
      results/<id>.zip           the result bundle handed to GET /jobs/{id}/result

``<id>`` is always a server-generated uuid4 hex string, validated against
``^[0-9a-f]{32}$`` before it ever reaches a path join — the only reason path
traversal is impossible here rather than merely unlikely.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import stat
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterator

JOB_ID_RE = re.compile(r"^[0-9a-f]{32}$")

_COPY_BLOCK = 1024 * 1024


class UnsafeArchiveError(Exception):
    """The uploaded zip tried something an archive should not try."""


def is_job_id(value: str) -> bool:
    return bool(JOB_ID_RE.fullmatch(value))


class Paths:
    """Every path the service ever writes, derived from a validated job id."""

    def __init__(self, data_dir: Path) -> None:
        self.root = Path(data_dir)
        self.uploads = self.root / "uploads"
        self.jobs = self.root / "jobs"
        self.results = self.root / "results"

    def ensure(self) -> None:
        for d in (self.root, self.uploads, self.jobs, self.results):
            d.mkdir(parents=True, exist_ok=True)
        # The data dir holds the only copy of a customer's capture; 0700.
        try:
            os.chmod(self.root, 0o700)
        except OSError:  # pragma: no cover - platform dependent
            pass

    @property
    def db(self) -> Path:
        return self.root / "jobs.db"

    def _checked(self, job_id: str) -> str:
        if not is_job_id(job_id):
            raise ValueError(f"not a server-generated job id: {job_id!r}")
        return job_id

    def upload_part(self, job_id: str) -> Path:
        return self.uploads / f"{self._checked(job_id)}.part"

    def job_dir(self, job_id: str) -> Path:
        return self.jobs / self._checked(job_id)

    def state_file(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "state.json"

    def input_dir(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "input"

    def output_dir(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "output"

    def stderr_log(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "worker.stderr.log"

    def stdout_log(self, job_id: str) -> Path:
        return self.job_dir(job_id) / "worker.stdout.log"

    def result_zip(self, job_id: str) -> Path:
        return self.results / f"{self._checked(job_id)}.zip"


def atomic_write_bytes(path: Path, data: bytes) -> None:
    """Write-and-rename, so a reader never sees a half-written state file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def atomic_write_json(path: Path, obj: Any) -> None:
    atomic_write_bytes(path, json.dumps(obj, indent=2, sort_keys=True).encode("utf-8") + b"\n")


# --- zip-slip-safe extraction ------------------------------------------------


def _safe_target(dest: Path, name: str) -> Path:
    """Resolve an archive entry name under ``dest`` or raise.

    Rejects: absolute paths (posix and windows), drive letters, ``..``
    anywhere, NUL bytes, and anything whose resolved path escapes ``dest``.
    """
    if not name or name in (".", "/"):
        raise UnsafeArchiveError(f"empty entry name: {name!r}")
    if "\x00" in name:
        raise UnsafeArchiveError("entry name contains NUL")
    normalized = name.replace("\\", "/")
    if normalized.startswith("/"):
        raise UnsafeArchiveError(f"absolute entry path: {name!r}")
    if re.match(r"^[A-Za-z]:", normalized):
        raise UnsafeArchiveError(f"drive-qualified entry path: {name!r}")
    parts = [p for p in normalized.split("/") if p not in ("", ".")]
    if any(p == ".." for p in parts):
        raise UnsafeArchiveError(f"parent traversal in entry path: {name!r}")
    if not parts:
        raise UnsafeArchiveError(f"entry resolves to nothing: {name!r}")
    target = dest.joinpath(*parts)
    # dest is already resolved by the caller; target has no symlinks because we
    # never create any (see the symlink rejection below), so a lexical check is
    # sound and does not depend on the entry existing yet.
    resolved = Path(os.path.normpath(str(target)))
    if resolved != dest and dest not in resolved.parents:
        raise UnsafeArchiveError(f"entry escapes the destination: {name!r}")
    return resolved


def safe_extract(zip_path: Path, dest: Path, max_total_bytes: int) -> int:
    """Extract ``zip_path`` into ``dest``, entry by entry, with a byte budget.

    Deliberately does not use ``ZipFile.extractall``: we want the traversal
    check, the symlink refusal and the *streaming* size budget (a lying
    ``file_size`` header must not get us to write a terabyte) to be ours and
    visible. Returns the number of bytes written.
    """
    dest = Path(os.path.abspath(dest))
    dest.mkdir(parents=True, exist_ok=True)
    written = 0
    with zipfile.ZipFile(zip_path) as zf:
        # Cheap pre-pass on the central directory: refuse an archive whose own
        # declared sizes already blow the budget before writing anything.
        declared = sum(max(0, info.file_size) for info in zf.infolist())
        if declared > max_total_bytes:
            raise UnsafeArchiveError(
                f"archive declares {declared} uncompressed bytes, over the "
                f"{max_total_bytes}-byte extraction budget"
            )
        for info in zf.infolist():
            mode = (info.external_attr >> 16) & 0o170000
            if mode == stat.S_IFLNK:
                raise UnsafeArchiveError(f"archive contains a symlink: {info.filename!r}")
            if info.filename.endswith("/"):
                _safe_target(dest, info.filename.rstrip("/")).mkdir(parents=True, exist_ok=True)
                continue
            target = _safe_target(dest, info.filename)
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(info, "r") as src, open(target, "wb") as out:
                while True:
                    block = src.read(_COPY_BLOCK)
                    if not block:
                        break
                    written += len(block)
                    if written > max_total_bytes:
                        raise UnsafeArchiveError(
                            f"extraction exceeded the {max_total_bytes}-byte budget"
                        )
                    out.write(block)
    return written


def find_lscan_root(extracted: Path) -> Path:
    """Locate the `.lscan` directory inside a freshly extracted upload.

    A bundle produced by A5's ``zip_export()`` may put ``manifest.json`` at the
    archive root or one level down (``MyScan.lscan/manifest.json``). Both are
    accepted, plus the degenerate "one directory, no manifest" case — the
    worker command is the one that gets to decide the input is unusable, not
    this function.
    """
    if (extracted / "manifest.json").is_file():
        return extracted
    candidates = sorted(p for p in extracted.iterdir() if p.is_dir() and p.name != "__MACOSX")
    for depth1 in candidates:
        if (depth1 / "manifest.json").is_file():
            return depth1
    for depth1 in candidates:
        for depth2 in sorted(p for p in depth1.iterdir() if p.is_dir()):
            if (depth2 / "manifest.json").is_file():
                return depth2
    if len(candidates) == 1:
        return candidates[0]
    return extracted


def _walk_files(root: Path) -> Iterator[Path]:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            yield Path(dirpath) / name


def zip_directory(src_dir: Path, dest_zip: Path) -> int:
    """Zip ``src_dir``'s *contents* to ``dest_zip``, atomically. Returns entries."""
    dest_zip.parent.mkdir(parents=True, exist_ok=True)
    src_dir.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(dest_zip.parent), prefix=f".{dest_zip.name}.", suffix=".tmp")
    os.close(fd)
    count = 0
    try:
        with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in _walk_files(src_dir):
                zf.write(path, arcname=str(path.relative_to(src_dir)))
                count += 1
        os.replace(tmp, dest_zip)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return count


def rmtree_quiet(path: Path) -> None:
    shutil.rmtree(path, ignore_errors=True)
