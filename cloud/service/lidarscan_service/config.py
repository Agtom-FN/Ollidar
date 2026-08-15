"""Configuration — environment only, validated once at startup.

Every knob is an environment variable so the service can be deployed from a
systemd unit / container without a config file, and so a test can build a
Config directly without touching the process environment.
"""

from __future__ import annotations

import logging
import os
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

log = logging.getLogger("lidarscan.config")

#: Tech Spec §3.8's "hard upload-size cap", same number as the engine client's
#: ``CloudSubmitConfig::max_upload_bytes`` default (2 GiB). The client refuses
#: before it sends anything; the server refuses again, which is the half that
#: actually protects the disk.
DEFAULT_MAX_UPLOAD_BYTES = 2 * 1024 * 1024 * 1024

#: The worker is a *command*, not an import. `{input}` is the extracted
#: `.lscan` directory, `{output}` is the directory whose contents become the
#: result bundle. Default points at INT-34's `engine_cli --post`.
DEFAULT_WORKER_CMD = "engine_cli --post {input} --out {output}"

DEFAULT_JOB_TIMEOUT_S = 3600.0


class ConfigError(RuntimeError):
    """Raised at startup for a configuration the service must not run with."""


def _env_int(env: Mapping[str, str], key: str, default: int) -> int:
    raw = env.get(key)
    if raw is None or raw == "":
        return default
    try:
        value = int(raw)
    except ValueError as exc:  # pragma: no cover - trivial
        raise ConfigError(f"{key}={raw!r} is not an integer") from exc
    if value <= 0:
        raise ConfigError(f"{key} must be > 0 (got {value})")
    return value


def _env_float(env: Mapping[str, str], key: str, default: float) -> float:
    raw = env.get(key)
    if raw is None or raw == "":
        return default
    try:
        value = float(raw)
    except ValueError as exc:  # pragma: no cover - trivial
        raise ConfigError(f"{key}={raw!r} is not a number") from exc
    if value <= 0:
        raise ConfigError(f"{key} must be > 0 (got {value})")
    return value


def _env_bool(env: Mapping[str, str], key: str, default: bool) -> bool:
    raw = env.get(key)
    if raw is None or raw == "":
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


@dataclass(frozen=True)
class Config:
    token: str
    data_dir: Path
    max_upload_bytes: int
    max_extract_bytes: int
    worker_cmd: tuple[str, ...]
    job_timeout_s: float
    host: str
    port: int
    url_prefix: str
    keep_inputs: bool

    @staticmethod
    def from_env(env: Mapping[str, str] | None = None) -> "Config":
        env = os.environ if env is None else env

        token = env.get("LIDARSCAN_TOKEN", "")
        if not token:
            raise ConfigError(
                "LIDARSCAN_TOKEN is required — the single-tenant MVP (Tech Spec §3.8) "
                "has exactly one credential and refuses to start without it"
            )
        if len(token) < 16:
            log.warning(
                "LIDARSCAN_TOKEN is only %d characters; use >= 32 bytes of randomness "
                "(`python -c \"import secrets;print(secrets.token_urlsafe(32))\"`)",
                len(token),
            )

        data_dir = Path(env.get("LIDARSCAN_DATA_DIR", "./data")).expanduser()

        max_upload = _env_int(env, "LIDARSCAN_MAX_UPLOAD_BYTES", DEFAULT_MAX_UPLOAD_BYTES)
        # A zip bomb is not bounded by the upload cap. Default to 8x, which is
        # a generous ratio for a `.lscan` (its bulk is already-packed binary
        # streams and JPEGs).
        max_extract = _env_int(env, "LIDARSCAN_MAX_EXTRACT_BYTES", max_upload * 8)

        raw_cmd = env.get("LIDARSCAN_WORKER_CMD", DEFAULT_WORKER_CMD)
        try:
            argv = tuple(shlex.split(raw_cmd))
        except ValueError as exc:
            raise ConfigError(f"LIDARSCAN_WORKER_CMD is not parseable: {exc}") from exc
        if not argv:
            raise ConfigError("LIDARSCAN_WORKER_CMD is empty")
        joined = " ".join(argv)
        if "{input}" not in joined:
            raise ConfigError("LIDARSCAN_WORKER_CMD must contain the {input} placeholder")
        if "{output}" not in joined:
            raise ConfigError("LIDARSCAN_WORKER_CMD must contain the {output} placeholder")

        prefix = env.get("LIDARSCAN_URL_PREFIX", "").rstrip("/")
        if prefix and not prefix.startswith("/"):
            raise ConfigError("LIDARSCAN_URL_PREFIX must start with '/' (e.g. /lidarscan)")

        return Config(
            token=token,
            data_dir=data_dir,
            max_upload_bytes=max_upload,
            max_extract_bytes=max_extract,
            worker_cmd=argv,
            job_timeout_s=_env_float(env, "LIDARSCAN_JOB_TIMEOUT_S", DEFAULT_JOB_TIMEOUT_S),
            host=env.get("LIDARSCAN_HOST", "127.0.0.1"),
            port=_env_int(env, "LIDARSCAN_PORT", 8080),
            url_prefix=prefix,
            keep_inputs=_env_bool(env, "LIDARSCAN_KEEP_INPUTS", False),
        )

    def render_worker_cmd(self, input_dir: Path, output_dir: Path) -> list[str]:
        """Substitute the two placeholders. Never goes through a shell."""
        out: list[str] = []
        for arg in self.worker_cmd:
            out.append(arg.replace("{input}", str(input_dir)).replace("{output}", str(output_dir)))
        return out


def describe(cfg: Config) -> Sequence[str]:
    """Human-readable startup banner lines (never includes the token)."""
    return (
        f"data_dir         = {cfg.data_dir}",
        f"max_upload_bytes = {cfg.max_upload_bytes}",
        f"worker_cmd       = {' '.join(cfg.worker_cmd)}",
        f"job_timeout_s    = {cfg.job_timeout_s}",
        f"bind             = {cfg.host}:{cfg.port}",
    )
