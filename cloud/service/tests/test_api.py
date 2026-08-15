"""Auth, health, and the path-safety properties that hold for every route."""

from __future__ import annotations

import pytest
from conftest import TOKEN, make_config, serve

TRAVERSAL_IDS = [
    "..",
    "../../etc/passwd",
    "%2e%2e%2fetc%2fpasswd",
    "0" * 31,
    "0" * 33,
    "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",
    "not-a-uuid",
]


@pytest.mark.parametrize(
    "method,path",
    [
        ("POST", "/jobs"),
        ("PUT", "/jobs/{id}/upload"),
        ("GET", "/jobs/{id}"),
        ("GET", "/jobs/{id}/result"),
        ("DELETE", "/jobs/{id}"),
    ],
)
async def test_every_job_route_requires_the_token(svc, method, path):
    job_id = (await svc.create(100)).json()["id"]
    url = path.format(id=job_id)
    for headers in (
        {},
        {"Authorization": "Bearer "},
        {"Authorization": "Bearer wrong"},
        {"Authorization": TOKEN},  # no scheme
        {"Authorization": f"Basic {TOKEN}"},
        {"Authorization": f"Bearer {TOKEN}x"},
        {"Authorization": f"Bearer {TOKEN[:-1]}"},
    ):
        resp = await svc.client.request(method, url, headers=headers, content=b"")
        assert resp.status_code == 401, (method, path, headers, resp.status_code)
        assert resp.headers.get("WWW-Authenticate") == "Bearer"


async def test_healthz_needs_no_token_and_leaks_no_ids(svc):
    job_id = (await svc.create(100)).json()["id"]
    resp = await svc.client.get("/healthz")
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "ok"
    assert body["max_upload_bytes"] == svc.cfg.max_upload_bytes
    assert job_id not in resp.text
    assert str(svc.cfg.data_dir) not in resp.text
    assert svc.cfg.token not in resp.text


@pytest.mark.parametrize("job_id", TRAVERSAL_IDS)
async def test_non_uuid_job_ids_are_404_never_a_path(svc, job_id):
    for method, path in (
        ("GET", f"/jobs/{job_id}"),
        ("GET", f"/jobs/{job_id}/result"),
        ("DELETE", f"/jobs/{job_id}"),
        ("PUT", f"/jobs/{job_id}/upload"),
    ):
        resp = await svc.client.request(method, path, headers=svc.auth, content=b"")
        assert resp.status_code in (404, 405), (path, resp.status_code)


async def test_job_ids_are_server_generated_and_unguessable(svc):
    ids = {(await svc.create(100)).json()["id"] for _ in range(20)}
    assert len(ids) == 20
    assert all(len(i) == 32 and all(c in "0123456789abcdef" for c in i) for i in ids)


async def test_client_supplied_id_is_ignored(svc):
    resp = await svc.client.post(
        "/jobs",
        headers=svc.auth,
        json={"kind": "lscan", "size_bytes": 100, "id": "../../../etc/passwd"},
    )
    assert resp.status_code == 201
    assert resp.json()["id"] != "../../../etc/passwd"


async def test_data_dir_layout_is_created_and_private(svc):
    import stat

    job_id = (await svc.create(100)).json()["id"]
    root = svc.paths.root
    for sub in ("uploads", "jobs", "results"):
        assert (root / sub).is_dir()
    assert svc.paths.db.exists()
    assert svc.paths.state_file(job_id).is_file()
    mode = stat.S_IMODE((root).stat().st_mode)
    assert mode & 0o077 == 0, oct(mode)


async def test_state_file_mirrors_the_row(svc, lscan_zip):
    import json

    payload = lscan_zip.read_bytes()
    job_id = (await svc.create(len(payload))).json()["id"]
    await svc.upload_all(job_id, payload)
    await svc.wait_terminal(job_id)
    mirrored = json.loads(svc.paths.state_file(job_id).read_text())
    row = svc.store.get(job_id)
    assert mirrored["state"] == row.state == "done"
    assert mirrored["received_bytes"] == len(payload)


async def test_config_requires_a_token(tmp_path):
    from lidarscan_service.config import Config, ConfigError

    with pytest.raises(ConfigError):
        Config.from_env({"LIDARSCAN_DATA_DIR": str(tmp_path)})


async def test_config_rejects_a_worker_cmd_without_placeholders(tmp_path):
    from lidarscan_service.config import Config, ConfigError

    with pytest.raises(ConfigError):
        make_config(tmp_path, LIDARSCAN_WORKER_CMD="engine_cli --post")
    with pytest.raises(ConfigError):
        Config.from_env(
            {
                "LIDARSCAN_TOKEN": TOKEN,
                "LIDARSCAN_DATA_DIR": str(tmp_path),
                "LIDARSCAN_WORKER_CMD": "engine_cli --post {input}",
            }
        )


async def test_defaults_are_the_spec_mvp_boundaries(tmp_path):
    from lidarscan_service.config import DEFAULT_MAX_UPLOAD_BYTES, Config

    cfg = Config.from_env({"LIDARSCAN_TOKEN": TOKEN, "LIDARSCAN_DATA_DIR": str(tmp_path)})
    assert cfg.max_upload_bytes == DEFAULT_MAX_UPLOAD_BYTES == 2 * 1024**3
    assert cfg.host == "127.0.0.1", "localhost by default; TLS terminates in front"
    assert cfg.worker_cmd[0] == "engine_cli" and "--post" in cfg.worker_cmd


async def test_worker_command_never_goes_through_a_shell(tmp_path):
    """A path with a shell metacharacter is an argument, not an injection."""
    from lidarscan_service.config import Config

    cfg = Config.from_env(
        {
            "LIDARSCAN_TOKEN": TOKEN,
            "LIDARSCAN_DATA_DIR": str(tmp_path),
            "LIDARSCAN_WORKER_CMD": "engine_cli --post {input} --out {output}",
        }
    )
    argv = cfg.render_worker_cmd("/tmp/a b; rm -rf /", "/tmp/out")
    assert argv == ["engine_cli", "--post", "/tmp/a b; rm -rf /", "--out", "/tmp/out"]
