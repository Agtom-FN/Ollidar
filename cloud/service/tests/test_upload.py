"""Upload-endpoint semantics: ranges, duplicates, gaps, caps, rollback."""

from __future__ import annotations

import pytest
from conftest import make_config, serve


async def create_job(svc, size: int) -> str:
    resp = await svc.create(size)
    assert resp.status_code == 201, resp.text
    return resp.json()["id"]


async def test_partial_chunk_returns_200_with_upload_offset(svc):
    payload = b"A" * 1000
    job_id = await create_job(svc, len(payload))
    resp = await svc.put_chunk(job_id, payload[:400], 0, len(payload))
    assert resp.status_code == 200
    assert resp.headers["Upload-Offset"] == "400"
    assert (await svc.status(job_id))["state"] == "uploading"


async def test_final_chunk_returns_201_and_queues(svc):
    payload = b"A" * 1000
    job_id = await create_job(svc, len(payload))
    await svc.put_chunk(job_id, payload[:400], 0, len(payload))
    resp = await svc.put_chunk(job_id, payload[400:], 400, len(payload))
    assert resp.status_code == 201
    assert resp.headers["Upload-Offset"] == "1000"
    assert svc.paths.upload_part(job_id).read_bytes() == payload


async def test_probe_reports_the_authoritative_offset(svc):
    payload = b"B" * 1000
    job_id = await create_job(svc, len(payload))
    probe = await svc.probe(job_id, len(payload))
    assert probe.status_code == 308 and probe.headers["Upload-Offset"] == "0"

    await svc.put_chunk(job_id, payload[:600], 0, len(payload))
    probe = await svc.probe(job_id, len(payload))
    assert probe.status_code == 308 and probe.headers["Upload-Offset"] == "600"


async def test_probe_after_completion_reports_total(svc, lscan_zip):
    payload = lscan_zip.read_bytes()
    job_id = await create_job(svc, len(payload))
    await svc.upload_all(job_id, payload)
    probe = await svc.probe(job_id, len(payload))
    # Answered even though the job is now queued/processing/done: a client
    # whose final ack was lost must be able to finish without re-uploading.
    assert probe.status_code == 308
    assert probe.headers["Upload-Offset"] == str(len(payload))


async def test_duplicate_chunk_is_idempotent(svc):
    payload = b"C" * 1000
    job_id = await create_job(svc, len(payload))
    first = await svc.put_chunk(job_id, payload[:500], 0, len(payload))
    again = await svc.put_chunk(job_id, payload[:500], 0, len(payload))
    assert first.status_code == again.status_code == 200
    assert again.headers["Upload-Offset"] == "500"
    assert svc.paths.upload_part(job_id).stat().st_size == 500


async def test_overlapping_chunk_writes_only_the_new_tail(svc):
    payload = bytes(range(256)) * 4  # 1024 bytes
    job_id = await create_job(svc, len(payload))
    await svc.put_chunk(job_id, payload[:600], 0, len(payload))
    resp = await svc.put_chunk(job_id, payload[400:1024], 400, len(payload))
    assert resp.status_code == 201
    assert svc.paths.upload_part(job_id).read_bytes() == payload


async def test_gap_is_416_with_the_real_offset(svc):
    payload = b"D" * 1000
    job_id = await create_job(svc, len(payload))
    await svc.put_chunk(job_id, payload[:300], 0, len(payload))
    resp = await svc.put_chunk(job_id, payload[500:800], 500, len(payload))
    assert resp.status_code == 416
    assert resp.headers["Upload-Offset"] == "300"
    assert svc.paths.upload_part(job_id).stat().st_size == 300


async def test_total_must_match_the_declared_size(svc):
    job_id = await create_job(svc, 1000)
    resp = await svc.put_chunk(job_id, b"x" * 10, 0, 999)
    assert resp.status_code == 400
    assert "does not match" in resp.json()["error"]


async def test_malformed_and_missing_content_range(svc):
    job_id = await create_job(svc, 1000)
    resp = await svc.client.put(f"/jobs/{job_id}/upload", headers=svc.auth, content=b"x")
    assert resp.status_code == 400
    resp = await svc.client.put(
        f"/jobs/{job_id}/upload",
        headers={**svc.auth, "Content-Range": "items 0-9/1000"},
        content=b"x",
    )
    assert resp.status_code == 400
    resp = await svc.client.put(
        f"/jobs/{job_id}/upload",
        headers={**svc.auth, "Content-Range": "bytes 900-800/1000"},
        content=b"x",
    )
    assert resp.status_code == 400


async def test_range_past_the_declared_total_is_rejected(svc):
    job_id = await create_job(svc, 1000)
    resp = await svc.put_chunk(job_id, b"x" * 200, 900, 1000)
    assert resp.status_code == 400
    assert "past the declared total" in resp.json()["error"]


async def test_body_longer_than_declared_is_rejected_and_rolled_back(svc):
    """Size is enforced on the bytes that arrive, not on the header."""
    payload = b"E" * 1000
    job_id = await create_job(svc, len(payload))
    await svc.put_chunk(job_id, payload[:200], 0, len(payload))

    async def liar():
        yield b"F" * 100
        yield b"F" * 900  # far more than the 300-byte range claims

    resp = await svc.client.put(
        f"/jobs/{job_id}/upload",
        headers={**svc.auth, "Content-Range": f"bytes 200-499/{len(payload)}"},
        content=liar(),
    )
    assert resp.status_code == 400
    # Rolled back to the last good offset — no partial write survives.
    assert svc.paths.upload_part(job_id).stat().st_size == 200
    assert (await svc.status(job_id))["received_bytes"] == 200


async def test_body_shorter_than_declared_is_rejected_and_rolled_back(svc):
    payload = b"G" * 1000
    job_id = await create_job(svc, len(payload))

    resp = await svc.client.put(
        f"/jobs/{job_id}/upload",
        headers={**svc.auth, "Content-Range": f"bytes 0-499/{len(payload)}"},
        content=b"G" * 100,
    )
    assert resp.status_code == 400
    assert svc.paths.upload_part(job_id).stat().st_size == 0


async def test_put_declaring_more_than_the_cap_is_413(tmp_path):
    cfg = make_config(tmp_path, LIDARSCAN_MAX_UPLOAD_BYTES="1000")
    async with serve(cfg) as svc:
        resp = await svc.create(1000)
        job_id = resp.json()["id"]
        # Declared inside the cap, but the PUT claims a bigger total.
        resp = await svc.client.put(
            f"/jobs/{job_id}/upload",
            headers={**svc.auth, "Content-Range": "bytes 0-1999/2000"},
            content=b"x" * 2000,
        )
        assert resp.status_code == 413
        assert svc.paths.upload_part(job_id).stat().st_size == 0


async def test_replayed_chunk_after_completion_still_acks(svc, lscan_zip):
    """Every valid chunk of a finished upload is a duplicate: 201, not 409.

    Resolved in the client's favour — cloud_submit.cpp treats anything but
    200/201 on a PUT as a hard failure, and a replay it already survived must
    not turn a finished job into an error.
    """
    payload = lscan_zip.read_bytes()
    job_id = await create_job(svc, len(payload))
    await svc.upload_all(job_id, payload)
    await svc.wait_terminal(job_id)
    resp = await svc.put_chunk(job_id, payload[:100], 0, len(payload))
    assert resp.status_code == 201
    assert resp.headers["Upload-Offset"] == str(len(payload))
    resp = await svc.client.put(
        f"/jobs/{job_id}/upload",
        headers={**svc.auth, "Content-Range": f"bytes 0-9/{len(payload) + 1}"},
        content=b"x" * 10,
    )
    assert resp.status_code == 400


async def test_upload_to_a_cancelled_job_is_409(svc):
    payload = b"H" * 1000
    job_id = await create_job(svc, len(payload))
    await svc.put_chunk(job_id, payload[:100], 0, len(payload))
    assert (await svc.client.delete(f"/jobs/{job_id}", headers=svc.auth)).status_code == 200
    resp = await svc.put_chunk(job_id, payload[100:200], 100, len(payload))
    assert resp.status_code == 409
    probe = await svc.probe(job_id, len(payload))
    assert probe.status_code == 409


async def test_zero_and_negative_sizes_are_refused_at_create(svc):
    for size in (0, -1):
        resp = await svc.create(size)
        assert resp.status_code == 400, size
    resp = await svc.client.post("/jobs", headers=svc.auth, json={"kind": "lscan"})
    assert resp.status_code == 400
    resp = await svc.client.post(
        "/jobs", headers=svc.auth, json={"kind": "video", "size_bytes": 10}
    )
    assert resp.status_code == 400


@pytest.mark.parametrize("body", [b"not json", b"[1,2,3]"])
async def test_malformed_create_bodies(svc, body):
    resp = await svc.client.post(
        "/jobs", headers={**svc.auth, "Content-Type": "application/json"}, content=body
    )
    assert resp.status_code == 400
