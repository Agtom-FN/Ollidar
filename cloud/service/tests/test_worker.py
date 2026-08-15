"""Worker orchestration: exit codes, cancellation, timeout, queueing."""

from __future__ import annotations

import asyncio
import json
import os
import zipfile

import pytest
from conftest import fake_worker_cmd, make_config, make_lscan_zip, serve


async def submit(svc, lscan_zip) -> str:
    payload = lscan_zip.read_bytes()
    resp = await svc.create(len(payload))
    job_id = resp.json()["id"]
    await svc.upload_all(job_id, payload)
    return job_id


async def test_happy_path_states_and_result(svc, lscan_zip, tmp_path):
    job_id = await submit(svc, lscan_zip)
    body = await svc.wait_terminal(job_id)
    assert body["state"] == "done"
    assert body["progress"] == 1.0
    assert body["exit_code"] == 0

    resp = await svc.client.get(f"/jobs/{job_id}/result", headers=svc.auth)
    assert resp.status_code == 200
    assert resp.headers["content-type"] == "application/zip"
    out = tmp_path / "r.zip"
    out.write_bytes(resp.content)
    with zipfile.ZipFile(out) as zf:
        assert "summary.json" in zf.namelist()


async def test_progress_from_stderr_reaches_the_status_endpoint(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(steps=5, step_sleep=0.12))
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        seen = set()
        for _ in range(200):
            body = await svc.status(job_id)
            seen.add(body["progress"])
            if body["state"] in ("done", "failed"):
                break
            await asyncio.sleep(0.02)
        assert (await svc.status(job_id))["state"] == "done"
        # The fake worker emits 16/33/50/66/83/100; we must have observed at
        # least one strictly-intermediate value parsed off stderr.
        assert any(0.0 < p < 1.0 for p in seen), seen
        log = svc.paths.stderr_log(job_id).read_text()
        assert "post:" in log and "%" in log


async def test_worker_crash_becomes_failed_with_a_reason(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(mode="crash"))
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        body = await svc.wait_terminal(job_id)
        assert body["state"] == "failed"
        assert body["exit_code"] == 1
        assert "synthetic worker crash" in body["message"]
        # A failed job keeps its input for post-mortem.
        assert svc.paths.input_dir(job_id).exists()
        assert (await svc.client.get(f"/jobs/{job_id}/result", headers=svc.auth)).status_code == 404


async def test_worker_usage_error_exit_2(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(mode="usage"))
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        body = await svc.wait_terminal(job_id)
        assert body["state"] == "failed" and body["exit_code"] == 2
        assert "rejected its arguments" in body["message"]


async def test_worker_self_cancel_exit_3(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(mode="selfcancel"))
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        body = await svc.wait_terminal(job_id)
        # A15 has no 'cancelled' wire state; it is 'failed' plus the word.
        assert body["state"] == "failed"
        assert body["internal_state"] == "cancelled"
        assert body["exit_code"] == 3


async def test_missing_worker_binary_fails_the_job_not_the_service(tmp_path, lscan_zip):
    cfg = make_config(
        tmp_path, LIDARSCAN_WORKER_CMD="/nonexistent/engine_cli --post {input} --out {output}"
    )
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        body = await svc.wait_terminal(job_id)
        assert body["state"] == "failed"
        assert "cannot start worker command" in body["message"]
        assert (await svc.client.get("/healthz")).status_code == 200


async def test_cancel_mid_run_kills_the_subprocess(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(mode="hang"))
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        for _ in range(200):
            if (await svc.status(job_id))["internal_state"] == "running":
                break
            await asyncio.sleep(0.02)
        assert (await svc.status(job_id))["internal_state"] == "running"
        pid = svc.worker._proc.pid

        resp = await svc.client.delete(f"/jobs/{job_id}", headers=svc.auth)
        assert resp.status_code == 200
        body = await svc.wait_terminal(job_id)
        assert body["state"] == "failed"  # A15 wire vocabulary
        assert body["internal_state"] == "cancelled"
        assert "cancel" in body["message"]

        # The child is really gone (reaped by the event loop).
        await asyncio.sleep(0.05)
        with pytest.raises(OSError):
            os.kill(pid, 0)


async def test_cancel_a_queued_job_never_starts_it(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(steps=3, step_sleep=0.2))
    async with serve(cfg) as svc:
        first = await submit(svc, lscan_zip)
        second = await submit(svc, lscan_zip)
        resp = await svc.client.delete(f"/jobs/{second}", headers=svc.auth)
        assert resp.status_code == 200
        assert (await svc.status(second))["internal_state"] == "cancelled"

        await svc.wait_terminal(first)
        assert (await svc.status(first))["state"] == "done"
        # It never ran: no worker output, no result.
        assert not svc.paths.result_zip(second).exists()
        assert (await svc.status(second))["exit_code"] is None


async def test_cancel_of_a_finished_job_is_409(svc, lscan_zip):
    job_id = await submit(svc, lscan_zip)
    await svc.wait_terminal(job_id)
    resp = await svc.client.delete(f"/jobs/{job_id}", headers=svc.auth)
    assert resp.status_code == 409


async def test_second_job_queues_behind_the_first(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(steps=3, step_sleep=0.15))
    async with serve(cfg) as svc:
        first = await submit(svc, lscan_zip)
        second = await submit(svc, lscan_zip)

        # While the first runs, the second must still be queued.
        saw_queued_behind_running = False
        for _ in range(200):
            a, b = await svc.status(first), await svc.status(second)
            if a["internal_state"] == "running" and b["internal_state"] == "queued":
                saw_queued_behind_running = True
            if b["state"] in ("done", "failed"):
                break
            await asyncio.sleep(0.02)
        assert saw_queued_behind_running

        assert (await svc.wait_terminal(first))["state"] == "done"
        assert (await svc.wait_terminal(second))["state"] == "done"

        # One worker instance means strict serialization, not "mostly".
        row_a, row_b = svc.store.get(first), svc.store.get(second)
        assert row_a.finished_at <= row_b.started_at
        pid_a = json.loads(_result_member(svc, first, "summary.json"))["pid"]
        pid_b = json.loads(_result_member(svc, second, "summary.json"))["pid"]
        assert pid_a != pid_b


async def test_job_timeout_kills_and_fails(tmp_path, lscan_zip):
    cfg = make_config(
        tmp_path,
        LIDARSCAN_WORKER_CMD=fake_worker_cmd(mode="hang"),
        LIDARSCAN_JOB_TIMEOUT_S="0.4",
    )
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        body = await svc.wait_terminal(job_id, timeout_s=15)
        assert body["state"] == "failed"
        assert "timed out" in body["message"]


async def test_upload_that_is_not_a_zip_fails_cleanly(svc):
    payload = b"this is not a zip file, not even close" * 10
    resp = await svc.create(len(payload))
    job_id = resp.json()["id"]
    await svc.upload_all(job_id, payload)
    body = await svc.wait_terminal(job_id)
    assert body["state"] == "failed"
    assert "not a readable zip" in body["message"]


async def test_zip_slip_upload_is_refused_before_the_worker_runs(svc, tmp_path):
    evil = tmp_path / "evil.zip"
    with zipfile.ZipFile(evil, "w") as zf:
        zf.writestr("../../escaped.txt", "pwned")
    payload = evil.read_bytes()
    job_id = (await svc.create(len(payload))).json()["id"]
    await svc.upload_all(job_id, payload)
    body = await svc.wait_terminal(job_id)
    assert body["state"] == "failed"
    assert "rejected upload" in body["message"]
    assert not (svc.paths.root.parent / "escaped.txt").exists()
    assert not (svc.paths.jobs / "escaped.txt").exists()
    assert body["exit_code"] is None


async def test_extraction_budget_refuses_a_zip_bomb(tmp_path):
    cfg = make_config(tmp_path, LIDARSCAN_MAX_EXTRACT_BYTES="4096")
    bomb = tmp_path / "bomb.zip"
    with zipfile.ZipFile(bomb, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("session.lscan/manifest.json", "{}")
        zf.writestr("session.lscan/streams/lidar.bin", b"\0" * (1 << 20))
    payload = bomb.read_bytes()
    async with serve(cfg) as svc:
        job_id = (await svc.create(len(payload))).json()["id"]
        await svc.upload_all(job_id, payload)
        body = await svc.wait_terminal(job_id)
        assert body["state"] == "failed"
        assert "budget" in body["message"]


async def test_graceful_shutdown_fails_the_running_job(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(mode="hang"))
    async with serve(cfg) as svc:
        job_id = await submit(svc, lscan_zip)
        for _ in range(200):
            if (await svc.status(job_id))["internal_state"] == "running":
                break
            await asyncio.sleep(0.02)
        store = svc.store
    assert store.get(job_id).state == "failed"
    assert "service stopped" in store.get(job_id).message


async def test_restart_recovery_requeues_and_fails_orphans(tmp_path, lscan_zip):
    """A crash leaves no subprocesses: queued work resumes, running work fails."""
    cfg = make_config(tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(steps=3, step_sleep=0.2))
    payload = lscan_zip.read_bytes()
    async with serve(cfg) as svc:
        orphan = await submit(svc, lscan_zip)
        for _ in range(200):
            if (await svc.status(orphan))["internal_state"] == "running":
                break
            await asyncio.sleep(0.02)
        # A second job that never got picked up before the process died.
        queued = (await svc.create(len(payload))).json()["id"]
        await svc.upload_all(queued, payload)
        store = svc.store

    # Simulate a hard crash rather than the graceful shutdown serve() does:
    # the row is left exactly as a SIGKILL would have left it.
    store.update(orphan, state="running", message="running")
    store.close()

    async with serve(cfg) as svc2:
        body = await svc2.status(orphan)
        assert body["state"] == "failed"
        assert "restarted" in body["message"]
        assert (await svc2.wait_terminal(queued))["state"] == "done"


def _result_member(svc, job_id: str, name: str) -> bytes:
    with zipfile.ZipFile(svc.paths.result_zip(job_id)) as zf:
        return zf.read(name)
