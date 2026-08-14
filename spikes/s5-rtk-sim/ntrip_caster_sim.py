"""ntrip_caster_sim.py -- minimal NTRIP v1/v2 caster for spike S5.

Serves:
  GET /                 sourcetable (NTRIP v1 "SOURCETABLE 200 OK" or v2
                         HTTP-headered variant, chosen by whether the
                         request carries `Ntrip-Version: Ntrip/2.0`)
  GET /<mountpoint>      HTTP Basic-auth gated RTCM3 stream. On success,
                         replays canned RTCM3 frames (built with
                         rtcm_tool.py) at a configurable cadence, while a
                         background reader thread logs any NMEA GGA lines
                         the client uploads on the same connection (the
                         standard NTRIP way a rover reports its position
                         back to a VRS/network-RTK caster).

Also supports configurable drop/reconnect injection (`drop_after_frames`
+ `max_drops` per mountpoint) so a client's reconnect logic can be
exercised deterministically.

This is deliberately minimal: no chunked transfer-encoding, no TLS
(ntrips://), no NTRIP-v1 SOURCE (base-station push) role, no real
sourcetable caching/refresh. See REPORT.md for the full list of
simplifications.
"""

from __future__ import annotations

import argparse
import base64
import os
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

import rtcm_tool

SERVER_ID = "NtripCasterSim/0.1"


def load_frames(path: str) -> List[bytes]:
    """Load a canned RTCM3 stream file (as written by rtcm_tool.generate_canned_stream)
    and split it into individual, whole, valid frame byte-strings."""
    with open(path, "rb") as f:
        data = f.read()
    frames = []
    for fr in rtcm_tool.iter_frames(data):
        if not fr.crc_ok:
            continue
        frames.append(data[fr.offset: fr.offset + fr.total_len])
    if not frames:
        raise ValueError(f"no valid RTCM3 frames found in {path}")
    return frames


@dataclass
class SourcetableEntry:
    identifier: str = "LidarScan-Sim"
    format: str = "RTCM 3.3"
    format_details: str = "1005(1),1077(1),1087(1),1097(1),1127(1),1230(5)"
    carrier: int = 2
    nav_system: str = "GPS+GLO+GAL+BDS"
    network: str = "SIM"
    country: str = "HKG"
    lat: float = 22.28
    lon: float = 114.15
    nmea_flag: int = 1
    solution: int = 0
    generator: str = SERVER_ID
    compr_enc: str = "none"
    authentication: str = "B"
    fee: str = "N"
    bitrate: int = 9600
    misc: str = ""


@dataclass
class MountConfig:
    name: str
    username: str
    password: str
    rtcm_file: str
    frame_interval_s: float = 1.0
    drop_after_frames: Optional[int] = None
    max_drops: int = 0
    sourcetable: SourcetableEntry = field(default_factory=SourcetableEntry)

    def __post_init__(self) -> None:
        self.frames: List[bytes] = load_frames(self.rtcm_file)
        self.gga_log: List[Tuple[float, str]] = []
        self.drops_used = 0
        self._lock = threading.Lock()

    def sourcetable_line(self) -> str:
        st = self.sourcetable
        fields = [
            "STR", self.name, st.identifier, st.format, st.format_details,
            str(st.carrier), st.nav_system, st.network, st.country,
            f"{st.lat:.2f}", f"{st.lon:.2f}", str(st.nmea_flag), str(st.solution),
            st.generator, st.compr_enc, st.authentication, st.fee, str(st.bitrate), st.misc,
        ]
        return ";".join(fields)


def build_sourcetable(mounts: Dict[str, MountConfig]) -> str:
    lines = [m.sourcetable_line() for m in mounts.values()]
    lines.append("ENDSOURCETABLE")
    return "\r\n".join(lines) + "\r\n"


class NtripCasterSim:
    def __init__(self, mounts: Sequence[MountConfig], host: str = "127.0.0.1", port: int = 2101):
        self.mounts: Dict[str, MountConfig] = {m.name: m for m in mounts}
        self.host = host
        self._port = port
        self._sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self._accept_thread: Optional[threading.Thread] = None
        self._conn_threads: List[threading.Thread] = []

    @property
    def port(self) -> int:
        if self._sock is None:
            return self._port
        return self._sock.getsockname()[1]

    def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.host, self._port))
        self._sock.listen(8)
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass

    def _accept_loop(self) -> None:
        assert self._sock is not None
        self._sock.settimeout(0.5)
        while not self._stop.is_set():
            try:
                conn, addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            th = threading.Thread(target=self._handle_conn, args=(conn, addr), daemon=True)
            th.start()
            self._conn_threads.append(th)

    # -- request parsing --------------------------------------------------

    @staticmethod
    def _read_request(conn: socket.socket) -> Optional[Tuple[str, str, Dict[str, str], bool, bytes]]:
        conn.settimeout(5.0)
        buf = b""
        while b"\r\n\r\n" not in buf:
            try:
                chunk = conn.recv(1024)
            except (socket.timeout, OSError):
                return None
            if not chunk:
                return None
            buf += chunk
            if len(buf) > 8192:
                return None
        head, _, rest = buf.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        if not lines:
            return None
        req_line = lines[0].decode("ascii", errors="replace")
        parts = req_line.split()
        if len(parts) < 2:
            return None
        method, path = parts[0], parts[1]
        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if b":" in line:
                k, v = line.split(b":", 1)
                headers[k.decode("ascii", errors="replace").strip().lower()] = v.decode(
                    "ascii", errors="replace").strip()
        is_v2 = headers.get("ntrip-version", "").lower().startswith("ntrip/2")
        return method, path, headers, is_v2, rest

    @staticmethod
    def _check_auth(auth_header: Optional[str], mount: MountConfig) -> bool:
        if not auth_header or not auth_header.startswith("Basic "):
            return False
        try:
            decoded = base64.b64decode(auth_header[6:].strip()).decode("utf-8")
        except Exception:
            return False
        user, _, pw = decoded.partition(":")
        return user == mount.username and pw == mount.password

    # -- connection handling ----------------------------------------------

    def _handle_conn(self, conn: socket.socket, addr) -> None:
        try:
            req = self._read_request(conn)
        except Exception:
            conn.close()
            return
        if req is None:
            conn.close()
            return
        method, path, headers, is_v2, leftover = req

        if path in ("/", ""):
            self._serve_sourcetable(conn, is_v2)
            conn.close()
            return

        mount = self.mounts.get(path.lstrip("/"))
        if mount is None:
            resp = b"HTTP/1.1 404 Not Found\r\n\r\n" if is_v2 else b"ERROR - Bad Mountpoint\r\n"
            try:
                conn.sendall(resp)
            except OSError:
                pass
            conn.close()
            return

        if not self._check_auth(headers.get("authorization"), mount):
            resp = (
                b"HTTP/1.1 401 Unauthorized\r\nServer: " + SERVER_ID.encode() +
                b"\r\nWWW-Authenticate: Basic realm=\"NTRIP\"\r\nConnection: close\r\n\r\n"
                if is_v2 else b"ERROR - Bad Password\r\n"
            )
            try:
                conn.sendall(resp)
            except OSError:
                pass
            conn.close()
            return

        ok = (
            b"HTTP/1.1 200 OK\r\nServer: " + SERVER_ID.encode() +
            b"\r\nNtrip-Version: Ntrip/2.0\r\nContent-Type: gnss/data\r\nConnection: close\r\n\r\n"
            if is_v2 else b"ICY 200 OK\r\n\r\n"
        )
        try:
            conn.sendall(ok)
        except OSError:
            conn.close()
            return

        reader = threading.Thread(target=self._read_gga_loop, args=(conn, mount, leftover), daemon=True)
        reader.start()
        self._stream_frames(conn, mount)
        try:
            conn.close()
        except OSError:
            pass

    def _serve_sourcetable(self, conn: socket.socket, is_v2: bool) -> None:
        body = build_sourcetable(self.mounts).encode("ascii")
        if is_v2:
            resp = (
                f"SOURCETABLE 200 OK\r\nServer: {SERVER_ID}\r\n"
                f"Content-Type: text/plain\r\nContent-Length: {len(body)}\r\n\r\n"
            ).encode("ascii") + body
        else:
            resp = b"SOURCETABLE 200 OK\r\n\r\n" + body
        try:
            conn.sendall(resp)
        except OSError:
            pass

    def _read_gga_loop(self, conn: socket.socket, mount: MountConfig, initial: bytes) -> None:
        buf = initial
        try:
            while not self._stop.is_set():
                if b"\n" not in buf:
                    conn.settimeout(1.0)
                    try:
                        chunk = conn.recv(1024)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not chunk:
                        break
                    buf += chunk
                    continue
                line, _, buf = buf.partition(b"\n")
                text = line.decode("ascii", errors="replace").strip()
                if text.startswith("$") and "GGA" in text:
                    with mount._lock:
                        mount.gga_log.append((time.time(), text))
        except OSError:
            pass

    def _stream_frames(self, conn: socket.socket, mount: MountConfig) -> None:
        frame_count = 0
        idx = 0
        n = len(mount.frames)
        while not self._stop.is_set():
            frame = mount.frames[idx % n]
            try:
                conn.sendall(frame)
            except OSError:
                return
            frame_count += 1
            idx += 1
            if mount.drop_after_frames and frame_count >= mount.drop_after_frames:
                with mount._lock:
                    if mount.drops_used < mount.max_drops:
                        mount.drops_used += 1
                        return  # forcibly close: exercises the client's reconnect logic
            time.sleep(mount.frame_interval_s)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="LidarScan S5 spike: minimal NTRIP v1/v2 caster simulator")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=2101)
    p.add_argument("--mount", default="LIDARSCAN")
    p.add_argument("--user", default="lidarscan")
    p.add_argument("--password", default="s5spike")
    p.add_argument("--rtcm-file", default=os.path.join(os.path.dirname(__file__), "fixtures", "canned_rtcm.bin"))
    p.add_argument("--frame-interval", type=float, default=1.0, help="seconds between streamed RTCM frames")
    p.add_argument("--drop-after-frames", type=int, default=None,
                    help="force-close the stream after this many frames (reconnect-logic testing)")
    p.add_argument("--max-drops", type=int, default=0, help="how many times to apply --drop-after-frames")
    args = p.parse_args(argv)

    if not os.path.exists(args.rtcm_file):
        os.makedirs(os.path.dirname(args.rtcm_file) or ".", exist_ok=True)
        n = rtcm_tool.generate_canned_stream(args.rtcm_file, count=300, seed=0)
        print(f"generated {n}-frame canned RTCM stream at {args.rtcm_file}")

    mount = MountConfig(
        name=args.mount, username=args.user, password=args.password, rtcm_file=args.rtcm_file,
        frame_interval_s=args.frame_interval, drop_after_frames=args.drop_after_frames,
        max_drops=args.max_drops,
    )
    caster = NtripCasterSim([mount], host=args.host, port=args.port)
    caster.start()
    print(f"NTRIP caster sim listening on {args.host}:{caster.port}  mount=/{args.mount}  "
          f"user={args.user!r} password={args.password!r}")
    sys.stdout.flush()
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    finally:
        caster.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
