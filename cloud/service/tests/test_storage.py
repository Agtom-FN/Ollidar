"""Unit tests for the pieces that must be right before any HTTP happens."""

from __future__ import annotations

import stat
import zipfile
from pathlib import Path

import pytest
from lidarscan_service.storage import (
    UnsafeArchiveError,
    atomic_write_json,
    find_lscan_root,
    is_job_id,
    safe_extract,
    zip_directory,
)
from lidarscan_service.store import WIRE_STATE, sanitize_message
from lidarscan_service.worker import parse_progress


@pytest.mark.parametrize(
    "name",
    [
        "../escaped.txt",
        "a/../../escaped.txt",
        "/etc/passwd",
        "C:/windows/system32/evil.dll",
        "..\\..\\escaped.txt",
    ],
)
def test_safe_extract_refuses_traversal(tmp_path, name):
    archive = tmp_path / "evil.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr(name, "pwned")
    with pytest.raises(UnsafeArchiveError):
        safe_extract(archive, tmp_path / "dest", 1 << 20)
    assert not (tmp_path / "escaped.txt").exists()


def test_safe_extract_refuses_symlinks(tmp_path):
    archive = tmp_path / "link.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        info = zipfile.ZipInfo("link")
        info.external_attr = (stat.S_IFLNK | 0o777) << 16
        zf.writestr(info, "/etc/passwd")
    with pytest.raises(UnsafeArchiveError):
        safe_extract(archive, tmp_path / "dest", 1 << 20)


def test_safe_extract_enforces_the_budget(tmp_path):
    archive = tmp_path / "big.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("big.bin", b"\0" * 100_000)
    with pytest.raises(UnsafeArchiveError):
        safe_extract(archive, tmp_path / "dest", 1000)


def test_safe_extract_round_trips_a_normal_bundle(tmp_path):
    archive = tmp_path / "ok.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("session.lscan/manifest.json", "{}")
        zf.writestr("session.lscan/streams/lidar.bin", b"\x01\x02\x03")
    written = safe_extract(archive, tmp_path / "dest", 1 << 20)
    assert written == 5
    assert (tmp_path / "dest/session.lscan/streams/lidar.bin").read_bytes() == b"\x01\x02\x03"


def test_find_lscan_root_handles_both_layouts(tmp_path):
    flat = tmp_path / "flat"
    (flat / "streams").mkdir(parents=True)
    (flat / "manifest.json").write_text("{}")
    assert find_lscan_root(flat) == flat

    nested = tmp_path / "nested"
    (nested / "MyScan.lscan").mkdir(parents=True)
    (nested / "MyScan.lscan" / "manifest.json").write_text("{}")
    assert find_lscan_root(nested) == nested / "MyScan.lscan"

    lonely = tmp_path / "lonely"
    (lonely / "whatever").mkdir(parents=True)
    assert find_lscan_root(lonely) == lonely / "whatever"


def test_zip_directory_is_atomic_and_relative(tmp_path):
    src = tmp_path / "out"
    (src / "sub").mkdir(parents=True)
    (src / "cloud.ply").write_bytes(b"ply")
    (src / "sub" / "report.json").write_text("{}")
    dest = tmp_path / "result.zip"
    assert zip_directory(src, dest) == 2
    with zipfile.ZipFile(dest) as zf:
        assert sorted(zf.namelist()) == ["cloud.ply", "sub/report.json"]
    assert not list(tmp_path.glob(".result.zip.*"))


def test_atomic_write_leaves_no_temp_files(tmp_path):
    target = tmp_path / "state.json"
    atomic_write_json(target, {"a": 1})
    atomic_write_json(target, {"a": 2})
    assert target.read_text().strip().endswith("}")
    assert [p.name for p in tmp_path.iterdir()] == ["state.json"]


@pytest.mark.parametrize(
    "line,expected",
    [
        ("post:   0%  ", (0.0, "post")),
        ("post:  42%  optimizing", (0.42, "optimizing")),
        ("post: 100%  exporting", (1.0, "exporting")),
        ("42% halfway", (0.42, "halfway")),
        ("[scanengine][info][lio] initialized: |a|=9.8067", None),
        ("post: FAILED (kIoError): boom", None),
        ("post: 999%  nonsense", None),
        ("", None),
    ],
)
def test_parse_progress(line, expected):
    assert parse_progress(line) == expected


def test_wire_states_are_exactly_the_five_the_client_parses():
    # engine/src/jobs/cloud_submit.cpp: state_from_string()
    assert set(WIRE_STATE.values()) == {"queued", "uploading", "processing", "done", "failed"}
    assert WIRE_STATE["cancelled"] == "failed"


def test_sanitize_message_keeps_the_clients_parser_safe():
    assert '"' not in sanitize_message('he said "hi"')
    assert "\\" not in sanitize_message("C:\\path")
    assert "\n" not in sanitize_message("two\nlines")
    assert len(sanitize_message("x" * 500)) <= 200


def test_is_job_id():
    assert is_job_id("0123456789abcdef0123456789abcdef")
    assert not is_job_id("0123456789ABCDEF0123456789abcdef")
    assert not is_job_id("../../etc/passwd")
    assert not is_job_id("")
