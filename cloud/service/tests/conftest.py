"""Fixtures: a real app instance (lifespan and worker included) driven over
httpx's in-process ASGI transport — no sockets, no ports, no flakes."""

from __future__ import annotations

import os
import random
import sys
import zipfile
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path

import httpx
import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from lidarscan_service.api import create_app  # noqa: E402
from lidarscan_service.config import Config  # noqa: E402
from lidarscan_service.storage import Paths  # noqa: E402
from lidarscan_service.store import JobStore  # noqa: E402

TOKEN = "test-token-abcdefghijklmnopqrstuvwxyz"
BAD_TOKEN = "test-token-abcdefghijklmnopqrstuvwxyy"
FAKE_WORKER = Path(__file__).with_name("fake_worker.py")


def fake_worker_cmd(mode: str = "ok", steps: int = 4, step_sleep: float = 0.0) -> str:
    return (
        f'"{sys.executable}" "{FAKE_WORKER}" --mode {mode} --steps {steps} '
        f"--step-sleep {step_sleep} {{input}} {{output}}"
    )


def make_config(tmp_path: Path, **overrides: str) -> Config:
    env = {
        "LIDARSCAN_TOKEN": TOKEN,
        "LIDARSCAN_DATA_DIR": str(tmp_path / "data"),
        "LIDARSCAN_WORKER_CMD": fake_worker_cmd(),
        "LIDARSCAN_JOB_TIMEOUT_S": "30",
    }
    env.update(overrides)
    return Config.from_env(env)


@dataclass
class Svc:
    client: httpx.AsyncClient
    app: object
    cfg: Config
    paths: Paths

    @property
    def store(self) -> JobStore:
        return self.app.state.store  # type: ignore[attr-defined]

    @property
    def worker(self):
        return self.app.state.worker  # type: ignore[attr-defined]

    @property
    def auth(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.cfg.token}"}

    async def create(self, size_bytes: int) -> httpx.Response:
        return await self.client.post(
            "/jobs", headers=self.auth, json={"kind": "lscan", "size_bytes": size_bytes}
        )

    async def put_chunk(self, job_id: str, data: bytes, start: int, total: int) -> httpx.Response:
        end = start + len(data) - 1
        return await self.client.put(
            f"/jobs/{job_id}/upload",
            headers={**self.auth, "Content-Range": f"bytes {start}-{end}/{total}"},
            content=data,
        )

    async def probe(self, job_id: str, total: int) -> httpx.Response:
        return await self.client.put(
            f"/jobs/{job_id}/upload",
            headers={**self.auth, "Content-Range": f"bytes */{total}"},
            content=b"",
        )

    async def upload_all(self, job_id: str, payload: bytes, chunk: int = 8192) -> httpx.Response:
        total = len(payload)
        resp = None
        for start in range(0, total, chunk):
            resp = await self.put_chunk(job_id, payload[start : start + chunk], start, total)
            assert resp.status_code in (200, 201), resp.text
        assert resp is not None
        return resp

    async def status(self, job_id: str) -> dict:
        resp = await self.client.get(f"/jobs/{job_id}", headers=self.auth)
        assert resp.status_code == 200, resp.text
        return resp.json()

    async def wait_terminal(self, job_id: str, timeout_s: float = 30.0) -> dict:
        import asyncio
        import time

        deadline = time.monotonic() + timeout_s
        while True:
            body = await self.status(job_id)
            if body["state"] in ("done", "failed"):
                return body
            assert time.monotonic() < deadline, f"job stuck in {body['state']}: {body}"
            await asyncio.sleep(0.02)


@asynccontextmanager
async def serve(cfg: Config):
    app = create_app(cfg)
    async with app.router.lifespan_context(app):
        transport = httpx.ASGITransport(app=app)
        async with httpx.AsyncClient(transport=transport, base_url="http://lidarscan.test") as client:
            yield Svc(client=client, app=app, cfg=cfg, paths=Paths(cfg.data_dir))


@pytest.fixture
async def svc(tmp_path):
    async with serve(make_config(tmp_path)) as s:
        yield s


# --- payload helpers ---------------------------------------------------------


def make_lscan_zip(path: Path, payload_bytes: int = 40_000, seed: int = 7) -> bytes:
    """A stand-in `.lscan.zip`: a manifest plus one incompressible stream.

    ZIP_STORED and random bytes keep the archive size predictable, which makes
    the chunk-count arithmetic in the upload tests exact.
    """
    rng = random.Random(seed)
    blob = bytes(rng.getrandbits(8) for _ in range(payload_bytes))
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("session.lscan/manifest.json", '{"schema":1,"sensor":"mid360"}')
        zf.writestr("session.lscan/streams/lidar.bin", blob)
    return path.read_bytes()


def zip_names(path: Path) -> list[str]:
    with zipfile.ZipFile(path) as zf:
        return sorted(zf.namelist())


def zip_bytes_of(path: Path, member: str) -> bytes:
    with zipfile.ZipFile(path) as zf:
        return zf.read(member)


@pytest.fixture
def lscan_zip(tmp_path) -> Path:
    path = tmp_path / "session.lscan.zip"
    make_lscan_zip(path)
    return path
