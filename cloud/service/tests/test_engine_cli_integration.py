"""End-to-end against the REAL `engine_cli --post`, if it can be found.

Everything else in this suite uses `fake_worker.py`. This test replaces it
with the actual binary the cloud worker runs (INT-34 §6), on a real synthetic
`.lscan` produced by `engine_cli --synth-lscan`, and asserts the service's
whole path: chunked upload -> zip-slip-safe extraction -> exec -> stderr
progress -> exit code -> result bundle -> download.

Discovery order for the binary:
  1. $LIDARSCAN_ENGINE_CLI
  2. cloud/service/.engine-build/engine_cli   (tools/build_engine_cli.sh)
  3. engine/build/*/engine_cli                (the CMake presets' layout)
Skips with an explanation if none exists — building the C++ engine is not a
precondition for the service's own test suite.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest
from client_sim import CloudSubmitSim, SimTransport
from conftest import ROOT, TOKEN, make_config, serve

REPO = ROOT.parent.parent


def find_engine_cli() -> Path | None:
    env = os.environ.get("LIDARSCAN_ENGINE_CLI")
    if env and Path(env).is_file() and os.access(env, os.X_OK):
        return Path(env)
    candidates = [ROOT / ".engine-build" / "engine_cli"]
    candidates += sorted((REPO / "engine" / "build").glob("*/engine_cli"))
    candidates += [REPO / "engine" / "build" / "engine_cli"]
    found = shutil.which("engine_cli")
    if found:
        candidates.append(Path(found))
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    return None


ENGINE_CLI = find_engine_cli()
requires_engine = pytest.mark.skipif(
    ENGINE_CLI is None,
    reason=(
        "engine_cli not found — build it with cloud/service/tools/build_engine_cli.sh "
        "or set LIDARSCAN_ENGINE_CLI. Skipping the real-worker integration test."
    ),
)


@requires_engine
async def test_real_engine_cli_post_end_to_end(tmp_path):
    assert ENGINE_CLI is not None

    # 1. A real (tiny, stationary, 2 s) synthetic Mid-360 capture.
    lscan = tmp_path / "session.lscan"
    subprocess.run(
        [str(ENGINE_CLI), "--synth-lscan", str(lscan), "2.0"],
        check=True,
        capture_output=True,
        timeout=120,
    )
    assert (lscan / "manifest.json").is_file()

    # 2. Package it the way the app's "extract for transfer" step does.
    bundle = tmp_path / "session.lscan.zip"
    with zipfile.ZipFile(bundle, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(p for p in lscan.rglob("*") if p.is_file()):
            zf.write(path, arcname=str(Path("session.lscan") / path.relative_to(lscan)))
    size = bundle.stat().st_size
    assert size > 1024

    # 3. A service whose worker IS the engine CLI. --no-loops/--no-outlier are
    #    what INT-34 §6 documents for a short stationary synthetic capture.
    cfg = make_config(
        tmp_path,
        LIDARSCAN_WORKER_CMD=(
            f'"{ENGINE_CLI}" --post {{input}} --out {{output}} --no-loops --no-outlier'
        ),
        LIDARSCAN_JOB_TIMEOUT_S="300",
    )
    async with serve(cfg) as svc:
        transport = SimTransport(svc.client)
        # 4 KiB chunks so the real run is genuinely multi-chunk, and one
        # chunk's acks are dropped so the resume probe is on the real path too.
        sim = CloudSubmitSim(transport, TOKEN, chunk_bytes=4096, max_retries=1)
        transport.drop_acks_for("bytes 4096-", times=2)
        progress: list[float] = []
        job_id = await sim.submit(bundle, progress)
        assert progress[-1] == 1.0
        chunks = len([r for r in transport.log if r[2] and "*" not in r[2]])
        assert chunks >= 3
        assert len([r for r in transport.log if r[2] == f"bytes */{size}"]) == 1

        status = await sim.wait_until_terminal(job_id, timeout_s=300)
        assert status.state == "done", f"{status.state}: {status.message}"

        out = tmp_path / "result.zip"
        await sim.download_result(job_id, out)
        with zipfile.ZipFile(out) as zf:
            names = sorted(zf.namelist())
            ply = zf.read("cloud.ply")
        assert names == ["cloud.ply"], names
        assert ply.startswith(b"ply"), ply[:16]
        assert len(ply) > 200, "the pipeline ran but produced a trivial cloud"

        # The engine's own stderr progress made it into job progress.
        stderr_log = svc.paths.stderr_log(job_id).read_text()
        assert "post:" in stderr_log and "100%" in stderr_log
        row = svc.store.get(job_id)
        assert row.exit_code == 0 and row.progress == 1.0

        print(
            f"\n[integration] engine_cli={ENGINE_CLI}\n"
            f"[integration] upload={size} bytes in {chunks} PUTs (1 resume probe), "
            f"result cloud.ply={len(ply)} bytes, exit={row.exit_code}"
        )


@requires_engine
async def test_real_engine_cli_reports_failure_for_a_junk_bundle(tmp_path):
    """A valid zip that is not a .lscan: exit != 0 -> failed, with the reason."""
    assert ENGINE_CLI is not None
    bundle = tmp_path / "junk.lscan.zip"
    with zipfile.ZipFile(bundle, "w") as zf:
        zf.writestr("session.lscan/manifest.json", "this is not json")
        zf.writestr("session.lscan/streams/lidar.bin", b"\x00\x01\x02")

    cfg = make_config(
        tmp_path,
        LIDARSCAN_WORKER_CMD=f'"{ENGINE_CLI}" --post {{input}} --out {{output}} --no-loops',
        LIDARSCAN_JOB_TIMEOUT_S="120",
    )
    payload = bundle.read_bytes()
    async with serve(cfg) as svc:
        job_id = (await svc.create(len(payload))).json()["id"]
        await svc.upload_all(job_id, payload)
        body = await svc.wait_terminal(job_id, timeout_s=120)
        assert body["state"] == "failed"
        assert body["exit_code"] == 1  # engine_cli: 0 ok / 1 failed / 2 usage / 3 cancelled
        assert "FAILED" in body["message"], body["message"]
        assert not svc.paths.result_zip(job_id).exists()
