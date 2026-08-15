"""A faithful Python port of the engine's `CloudSubmitClient`.

This is the point of the whole test suite: the tests do not exercise a
convenient client, they exercise *the algorithm in
`engine/src/jobs/cloud_submit.cpp`* — same request order, same headers, same
`send_with_retry` (5xx and transport failure only), same single resume probe
after retries are exhausted, same status-code interpretation. If the service
ever drifts from what the shipped desktop/Android client does, these fail.

`SimTransport` mirrors `jobs::HttpTransport`: one call is one round trip, and
`transport_ok == False` means "no HTTP status line ever arrived", which is
exactly what `QtHttpTransport` reports for a timeout/refusal/abort.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

import httpx


class SimError(RuntimeError):
    def __init__(self, kind: str, message: str) -> None:
        super().__init__(f"{kind}: {message}")
        self.kind = kind  # mirrors ScanError's name, e.g. kPermissionDenied


@dataclass
class SimResponse:
    transport_ok: bool = False
    status_code: int = 0
    headers: dict[str, str] = field(default_factory=dict)
    body: bytes = b""

    def header(self, name: str) -> str | None:
        for k, v in self.headers.items():
            if k.lower() == name.lower():
                return v
        return None


class SimTransport:
    """Wraps an httpx client and can simulate the two failure modes that
    matter: a lost ack (bytes land, no response) and a dead connection."""

    def __init__(self, client: httpx.AsyncClient, base_url: str = "") -> None:
        self._client = client
        self._base = base_url
        self._drop_acks: dict[str, int] = {}  # Content-Range prefix -> times
        self._fail_sends: dict[str, int] = {}
        self.log: list[tuple[str, str, str | None]] = []

    def drop_acks_for(self, range_prefix: str, times: int) -> None:
        """The bytes land; the response never arrives (a lost ack)."""
        self._drop_acks[range_prefix] = times

    def fail_sends_for(self, range_prefix: str, times: int) -> None:
        """The request never leaves the client (a dead connection)."""
        self._fail_sends[range_prefix] = times

    @staticmethod
    def _take(table: dict[str, int], rng: str | None) -> bool:
        if rng is None:
            return False
        for prefix, remaining in list(table.items()):
            if rng.startswith(prefix) and remaining > 0:
                table[prefix] = remaining - 1
                return True
        return False

    async def request(
        self, method: str, url: str, headers: dict[str, str], content: bytes = b""
    ) -> SimResponse:
        rng = headers.get("Content-Range")
        self.log.append((method, url, rng))
        if self._take(self._fail_sends, rng):
            return SimResponse(transport_ok=False)
        response = await self._client.request(
            method, url, headers=headers, content=content, follow_redirects=False
        )
        if self._take(self._drop_acks, rng):
            return SimResponse(transport_ok=False)
        return SimResponse(
            transport_ok=True,
            status_code=response.status_code,
            headers=dict(response.headers),
            body=response.content,
        )


@dataclass
class CloudJobStatus:
    state: str
    progress: float
    message: str


class CloudSubmitSim:
    def __init__(
        self,
        transport: SimTransport,
        token: str,
        base_url: str = "",
        chunk_bytes: int = 64 * 1024,
        max_retries: int = 2,
    ) -> None:
        self.t = transport
        self.token = token
        self.base = base_url
        self.chunk_bytes = chunk_bytes
        self.max_retries = max_retries

    def _auth(self) -> str:
        return f"Bearer {self.token}"

    async def _send_with_retry(self, method: str, url: str, headers: dict, content: bytes = b""):
        resp = SimResponse()
        for attempt in range(self.max_retries + 1):
            resp = await self.t.request(method, url, headers, content)
            retryable = (not resp.transport_ok) or resp.status_code >= 500
            if not retryable or attempt == self.max_retries:
                return resp
        return resp

    async def submit(self, path: Path, progress: list[float] | None = None) -> str:
        total = path.stat().st_size

        create = await self._send_with_retry(
            "POST",
            f"{self.base}/jobs",
            {"Authorization": self._auth(), "Content-Type": "application/json"},
            json.dumps({"kind": "lscan", "size_bytes": total}).encode(),
        )
        if not create.transport_ok:
            raise SimError("kNetworkError", "POST /jobs: no response")
        if create.status_code == 401:
            raise SimError("kPermissionDenied", "POST /jobs: token rejected")
        if create.status_code == 413:
            raise SimError("kCapacityExceeded", "POST /jobs: server rejected upload size")
        if create.status_code != 201:
            raise SimError("kUnknown", f"POST /jobs: unexpected status {create.status_code}")
        body = json.loads(create.body)
        job_id, upload_path = body["id"], body["upload_url"]
        upload_url = f"{self.base}{upload_path}"

        data = path.read_bytes()
        offset = 0
        while offset < total:
            n = min(self.chunk_bytes, total - offset)
            chunk = data[offset : offset + n]
            resp = await self._send_with_retry(
                "PUT",
                upload_url,
                {
                    "Authorization": self._auth(),
                    "Content-Range": f"bytes {offset}-{offset + n - 1}/{total}",
                },
                chunk,
            )
            if not resp.transport_ok:
                probe = await self._send_with_retry(
                    "PUT",
                    upload_url,
                    {"Authorization": self._auth(), "Content-Range": f"bytes */{total}"},
                    b"",
                )
                if not probe.transport_ok:
                    raise SimError("kDisconnected", "upload disconnected and the probe failed too")
                off_hdr = probe.header("Upload-Offset")
                if probe.status_code not in (200, 308) or off_hdr is None:
                    raise SimError(
                        "kNetworkError",
                        f"resume probe returned no Upload-Offset (status {probe.status_code})",
                    )
                offset = int(off_hdr)
                if offset > total:
                    raise SimError("kCorruptData", "resume offset past EOF")
                if progress is not None:
                    progress.append(offset / total if total else 1.0)
                continue

            if resp.status_code == 401:
                raise SimError("kPermissionDenied", "PUT upload: token rejected")
            if resp.status_code not in (200, 201):
                raise SimError("kUnknown", f"PUT upload: unexpected status {resp.status_code}")
            offset += n
            if progress is not None:
                progress.append(offset / total if total else 1.0)
        return job_id

    async def poll(self, job_id: str) -> CloudJobStatus:
        resp = await self._send_with_retry(
            "GET", f"{self.base}/jobs/{job_id}", {"Authorization": self._auth()}
        )
        if not resp.transport_ok:
            raise SimError("kNetworkError", "GET /jobs: no response")
        if resp.status_code == 401:
            raise SimError("kPermissionDenied", "GET /jobs: token rejected")
        if resp.status_code == 404:
            raise SimError("kNotFound", "GET /jobs: not found")
        if resp.status_code != 200:
            raise SimError("kUnknown", f"GET /jobs: unexpected status {resp.status_code}")
        text = resp.body.decode()
        return CloudJobStatus(
            state=_json_string(text, "state") or "unknown",
            progress=_json_number(text, "progress"),
            message=_json_string(text, "message") or "",
        )

    async def wait_until_terminal(self, job_id: str, timeout_s: float = 30.0) -> CloudJobStatus:
        import asyncio
        import time

        deadline = time.monotonic() + timeout_s
        while True:
            status = await self.poll(job_id)
            if status.state in ("done", "failed"):
                return status
            if time.monotonic() > deadline:
                raise SimError("kTimeout", f"job stuck in {status.state}")
            await asyncio.sleep(0.02)

    async def download_result(self, job_id: str, dest: Path) -> None:
        resp = await self._send_with_retry(
            "GET", f"{self.base}/jobs/{job_id}/result", {"Authorization": self._auth()}
        )
        if not resp.transport_ok:
            raise SimError("kNetworkError", "download: no response")
        if resp.status_code == 401:
            raise SimError("kPermissionDenied", "download: token rejected")
        if resp.status_code == 404:
            raise SimError("kNotFound", "download: result not ready")
        if resp.status_code != 200:
            raise SimError("kUnknown", f"download: unexpected status {resp.status_code}")
        dest.write_bytes(resp.body)


# --- the client's own hand-rolled JSON reader, ported ------------------------
# cloud_submit.cpp does not use a JSON library; it finds `"key":` at the top
# level. Porting that (rather than json.loads) is deliberate — it is what
# proves our response bodies are parseable by the shipped client.


def _json_raw(body: str, key: str) -> str | None:
    needle = f'"{key}"'
    pos = body.find(needle)
    if pos < 0:
        return None
    pos = body.find(":", pos + len(needle))
    if pos < 0:
        return None
    pos += 1
    while pos < len(body) and body[pos] in " \t":
        pos += 1
    if pos >= len(body):
        return None
    if body[pos] == '"':
        out = []
        pos += 1
        while pos < len(body) and body[pos] != '"':
            if body[pos] == "\\" and pos + 1 < len(body):
                pos += 1
            out.append(body[pos])
            pos += 1
        return "".join(out)
    end = pos
    while end < len(body) and body[end] not in ",} \n\r\t":
        end += 1
    return body[pos:end]


def _json_string(body: str, key: str) -> str | None:
    return _json_raw(body, key)


def _json_number(body: str, key: str) -> float:
    raw = _json_raw(body, key)
    if raw is None:
        return 0.0
    m = re.match(r"[-+0-9.eE]+", raw)
    return float(m.group(0)) if m else 0.0
