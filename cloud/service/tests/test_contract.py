"""The A15 REST contract, exercised by a port of the shipped engine client.

Every test here drives `CloudSubmitSim` (tests/client_sim.py), which is
`engine/src/jobs/cloud_submit.cpp` translated call for call. A failure in
this file means the desktop/Android client would fail against this service.
"""

from __future__ import annotations

import json
import math
import zipfile

import pytest
from client_sim import CloudSubmitSim, SimError, SimTransport
from conftest import TOKEN, fake_worker_cmd, make_config, make_lscan_zip, serve

CHUNK = 8192


def client_for(svc, chunk_bytes: int = CHUNK, max_retries: int = 1, token: str = TOKEN):
    transport = SimTransport(svc.client)
    sim = CloudSubmitSim(transport, token, chunk_bytes=chunk_bytes, max_retries=max_retries)
    return sim, transport


async def test_happy_path_create_upload_poll_download(svc, lscan_zip, tmp_path):
    sim, transport = client_for(svc)
    progress: list[float] = []

    job_id = await sim.submit(lscan_zip, progress)
    assert len(job_id) == 32
    assert progress[-1] == pytest.approx(1.0)
    assert progress == sorted(progress)

    status = await sim.wait_until_terminal(job_id)
    assert status.state == "done", status
    assert status.progress == pytest.approx(1.0)

    dest = tmp_path / "result.zip"
    await sim.download_result(job_id, dest)
    with zipfile.ZipFile(dest) as zf:
        names = sorted(zf.namelist())
        summary = json.loads(zf.read("summary.json"))
    assert names == ["cloud.ply", "summary.json"]
    # The worker saw the real extracted bundle, manifest and all.
    assert summary["manifest_present"] is True
    assert summary["file_count"] == 2

    total = lscan_zip.stat().st_size
    puts = [r for r in transport.log if r[0] == "PUT"]
    assert len(puts) == math.ceil(total / CHUNK)
    assert all(rng and rng.startswith("bytes ") and "*" not in rng for _, _, rng in puts)
    # A successful job releases its staging upload.
    assert not svc.paths.upload_part(job_id).exists()


async def test_mid_upload_disconnect_then_resume_is_byte_exact(svc, lscan_zip, tmp_path):
    """A15 §5's resume story: the bytes land, the ack does not.

    Both attempts at the second chunk reach the server (so the second one is a
    duplicate the server must swallow idempotently), both acks are lost, the
    client's retries are exhausted, and the single probe reports the ADVANCED
    offset — so the upload continues rather than restarting.
    """
    sim, transport = client_for(svc, max_retries=1)
    total = lscan_zip.stat().st_size
    transport.drop_acks_for(f"bytes {CHUNK}-", times=2)  # attempt + retry

    job_id = await sim.submit(lscan_zip)

    probes = [r for r in transport.log if r[2] == f"bytes */{total}"]
    assert len(probes) == 1, "exactly one resume probe, as cloud_submit.cpp specifies"
    resent = [r for r in transport.log if r[2] and r[2].startswith(f"bytes {CHUNK}-")]
    assert len(resent) == 2, "the client never re-sent the chunk the probe said had landed"

    status = await sim.wait_until_terminal(job_id)
    assert status.state == "done", status

    dest = tmp_path / "out.zip"
    await sim.download_result(job_id, dest)
    with zipfile.ZipFile(dest) as zf:
        summary = json.loads(zf.read("summary.json"))
    # The worker hashed the extracted tree; matching the hash of the same
    # tree extracted locally proves the reassembled upload is byte-exact.
    assert summary["sha256"] == _sha256_of_zip(lscan_zip)


async def test_hard_disconnect_replays_the_same_chunk(svc, lscan_zip):
    """Bytes never left the client: the probe reports the OLD offset."""
    sim, transport = client_for(svc, max_retries=1)
    transport.fail_sends_for(f"bytes {2 * CHUNK}-", times=2)

    job_id = await sim.submit(lscan_zip)
    status = await sim.wait_until_terminal(job_id)
    assert status.state == "done"

    sent = [r for r in transport.log if r[2] and r[2].startswith(f"bytes {2 * CHUNK}-")]
    assert len(sent) == 3, "2 failed sends + 1 real replay after the probe"


async def test_token_rejected_everywhere(svc, lscan_zip, tmp_path):
    bad, _ = client_for(svc, token="test-token-abcdefghijklmnopqrstuvwxyy")
    with pytest.raises(SimError) as exc:
        await bad.submit(lscan_zip)
    assert exc.value.kind == "kPermissionDenied"

    good, _ = client_for(svc)
    job_id = await good.submit(lscan_zip)
    assert (await good.wait_until_terminal(job_id)).state == "done"

    with pytest.raises(SimError) as exc:
        await bad.poll(job_id)
    assert exc.value.kind == "kPermissionDenied"
    with pytest.raises(SimError) as exc:
        await bad.download_result(job_id, tmp_path / "nope.zip")
    assert exc.value.kind == "kPermissionDenied"
    assert not (tmp_path / "nope.zip").exists()


async def test_size_cap_is_413_before_any_body(tmp_path):
    cfg = make_config(tmp_path, LIDARSCAN_MAX_UPLOAD_BYTES="20000")
    big = tmp_path / "big.lscan.zip"
    make_lscan_zip(big, payload_bytes=40_000)
    async with serve(cfg) as svc:
        sim, transport = client_for(svc)
        with pytest.raises(SimError) as exc:
            await sim.submit(big)
        assert exc.value.kind == "kCapacityExceeded"
        assert [r[0] for r in transport.log] == ["POST"]


async def test_result_is_404_while_processing(tmp_path, lscan_zip):
    cfg = make_config(
        tmp_path, LIDARSCAN_WORKER_CMD=fake_worker_cmd(steps=4, step_sleep=0.15)
    )
    async with serve(cfg) as svc:
        sim, _ = client_for(svc)
        job_id = await sim.submit(lscan_zip)
        status = await sim.poll(job_id)
        assert status.state in ("queued", "processing")
        with pytest.raises(SimError) as exc:
            await sim.download_result(job_id, tmp_path / "early.zip")
        assert exc.value.kind == "kNotFound"
        assert (await sim.wait_until_terminal(job_id)).state == "done"
        await sim.download_result(job_id, tmp_path / "late.zip")
        assert (tmp_path / "late.zip").stat().st_size > 0


async def test_unknown_job_is_404_on_poll_and_result(svc, tmp_path):
    sim, _ = client_for(svc)
    with pytest.raises(SimError) as exc:
        await sim.poll("f" * 32)
    assert exc.value.kind == "kNotFound"
    with pytest.raises(SimError) as exc:
        await sim.download_result("0" * 32, tmp_path / "x.zip")
    assert exc.value.kind == "kNotFound"


async def test_status_body_matches_the_engines_hand_rolled_parser(svc, lscan_zip):
    sim, _ = client_for(svc)
    job_id = await sim.submit(lscan_zip)
    status = await sim.wait_until_terminal(job_id)
    assert status.state == "done"
    assert 0.0 <= status.progress <= 1.0
    assert '"' not in status.message and "\\" not in status.message

    raw = (await svc.client.get(f"/jobs/{job_id}", headers=svc.auth)).text
    body = json.loads(raw)
    for key in ("id", "state", "progress", "message"):
        assert key in body
    assert body["state"] in ("queued", "uploading", "processing", "done", "failed")
    assert body["id"] == job_id


async def test_upload_url_is_the_path_the_client_appends_to_base(svc, lscan_zip):
    resp = await svc.create(lscan_zip.stat().st_size)
    assert resp.status_code == 201
    body = resp.json()
    assert body["upload_url"] == f"/jobs/{body['id']}/upload"
    assert body["upload_url"].startswith("/"), "must concatenate onto base_url"


async def test_url_prefix_is_reflected_for_subpath_deployments(tmp_path, lscan_zip):
    cfg = make_config(tmp_path, LIDARSCAN_URL_PREFIX="/lidarscan")
    async with serve(cfg) as svc:
        body = (await svc.create(lscan_zip.stat().st_size)).json()
        assert body["upload_url"] == f"/lidarscan/jobs/{body['id']}/upload"


def _sha256_of_zip(zip_path) -> str:
    """Reproduce fake_worker.py's digest over the extracted tree."""
    import hashlib
    import tempfile
    from pathlib import Path

    from lidarscan_service.storage import find_lscan_root, safe_extract

    with tempfile.TemporaryDirectory() as tmp:
        dest = Path(tmp) / "x"
        safe_extract(zip_path, dest, 1 << 30)
        root = find_lscan_root(dest)
        digest = hashlib.sha256()
        for path in sorted(p for p in root.rglob("*") if p.is_file()):
            digest.update(str(path.relative_to(root)).encode())
            digest.update(path.read_bytes())
        return digest.hexdigest()
