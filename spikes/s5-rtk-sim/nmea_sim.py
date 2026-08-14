"""nmea_sim.py -- GNSS rover simulator for spike S5 (RTK simulation infra).

Generates a configurable walking-speed trajectory around a closed
waypoint loop and emits standards-correct NMEA 0183 sentences (GGA, RMC,
GST, GSA, VTG) at 1-5 Hz, with scriptable fix-quality scenarios (e.g.
"RTK Fixed 60s -> Float 20s -> Single 10s -> Fixed") and per-fix-state
position noise (Fixed ~2cm, Float ~30cm, Single ~2m 1-sigma) whose GST
error estimates are consistent with the noise actually injected.

Output modes:
  pty   open a pseudo-terminal, print the /dev/ttys* (or /dev/pts/*) path
        to connect a serial client to
  tcp   run a TCP server, streaming sentences to every connected client
  file  write a fixed span of sentences to a plain text file

Designed to be both a CLI tool and an importable module (test_roundtrip.py
drives GnssRoverSim / TcpNmeaServer directly, without going through a
subprocess).
"""

from __future__ import annotations

import argparse
import datetime
import math
import os
import random
import socket
import sys
import threading
import time
from dataclasses import dataclass
from enum import Enum
from typing import List, Optional, Sequence, Tuple

EARTH_RADIUS_M = 6371000.0
KNOTS_PER_MPS = 1.9438444924
KMH_PER_MPS = 3.6

# Fixed simulation epoch so sentence timestamps are deterministic and
# reproducible across runs (rather than tied to wall-clock UTC), which
# matters for the self-test. Chosen date is otherwise arbitrary.
SIM_EPOCH = datetime.datetime(2026, 1, 1, 0, 0, 0)

# EGM96 geoid separation is not looked up for real -- see REPORT.md.
GEOID_SEP_M = -2.0


# --------------------------------------------------------------------------
# NMEA sentence primitives
# --------------------------------------------------------------------------

def nmea_checksum(body: str) -> str:
    """XOR checksum over `body` (the text between '$' and '*', exclusive),
    as two uppercase hex characters, per NMEA 0183."""
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"{cs:02X}"


def build_sentence(talker: str, sentence_id: str, fields: Sequence[str]) -> str:
    body = f"{talker}{sentence_id}," + ",".join(fields)
    return f"${body}*{nmea_checksum(body)}\r\n"


def to_nmea_lat(lat: float) -> Tuple[str, str]:
    hemi = "N" if lat >= 0 else "S"
    lat = abs(lat)
    deg = int(lat)
    minutes = (lat - deg) * 60.0
    return f"{deg:02d}{minutes:07.4f}", hemi


def to_nmea_lon(lon: float) -> Tuple[str, str]:
    hemi = "E" if lon >= 0 else "W"
    lon = abs(lon)
    deg = int(lon)
    minutes = (lon - deg) * 60.0
    return f"{deg:03d}{minutes:07.4f}", hemi


def nmea_time(dt: datetime.datetime) -> str:
    return f"{dt.hour:02d}{dt.minute:02d}{dt.second:02d}.{int(dt.microsecond / 10000):02d}"


def nmea_date(dt: datetime.datetime) -> str:
    return f"{dt.day:02d}{dt.month:02d}{dt.year % 100:02d}"


# --------------------------------------------------------------------------
# Fix-quality state machine
# --------------------------------------------------------------------------

class FixState(Enum):
    NONE = "NONE"
    SINGLE = "SINGLE"
    DGPS = "DGPS"
    FLOAT = "FLOAT"
    FIXED = "FIXED"


@dataclass(frozen=True)
class FixProfile:
    quality: int          # GGA fix quality field (0/1/2/4/5/6)
    gsa_fix_type: int     # GSA field 2: 1=no fix, 2=2D, 3=3D
    mode_char: str        # RMC/VTG mode indicator (NMEA 2.3+): N/A/D/R/F/E
    num_sv: int
    hdop: float
    pdop: float
    vdop: float
    h_std_m: Optional[float]  # 1-sigma horizontal noise injected, None = no fix
    v_std_m: Optional[float]  # 1-sigma vertical noise injected


# Noise magnitudes match the spec: Fixed ~2cm, Float ~30cm, Single ~2m.
PROFILES = {
    FixState.NONE:   FixProfile(quality=0, gsa_fix_type=1, mode_char="N", num_sv=0,
                                 hdop=99.9, pdop=99.9, vdop=99.9, h_std_m=None, v_std_m=None),
    FixState.SINGLE: FixProfile(quality=1, gsa_fix_type=3, mode_char="A", num_sv=9,
                                 hdop=1.1, pdop=1.8, vdop=1.4, h_std_m=2.0, v_std_m=3.0),
    FixState.DGPS:   FixProfile(quality=2, gsa_fix_type=3, mode_char="D", num_sv=10,
                                 hdop=1.0, pdop=1.6, vdop=1.3, h_std_m=0.5, v_std_m=0.8),
    FixState.FLOAT:  FixProfile(quality=5, gsa_fix_type=3, mode_char="F", num_sv=20,
                                 hdop=0.7, pdop=1.1, vdop=0.9, h_std_m=0.30, v_std_m=0.45),
    FixState.FIXED:  FixProfile(quality=4, gsa_fix_type=3, mode_char="R", num_sv=22,
                                 hdop=0.6, pdop=0.9, vdop=0.8, h_std_m=0.02, v_std_m=0.03),
}


@dataclass(frozen=True)
class ScenarioSegment:
    state: FixState
    duration_s: float  # 0 is only valid on the LAST segment: "hold forever"


class Scenario:
    """A fix-quality timeline, e.g. "RTK Fixed 60s -> Float 20s -> Single
    10s -> Fixed (forever)", parsed from "FIXED:60,FLOAT:20,SINGLE:10,FIXED:0".

    A trailing segment with duration 0 means "hold this state forever"
    once reached. Otherwise the whole sequence loops.
    """

    def __init__(self, segments: Sequence[ScenarioSegment]):
        if not segments:
            raise ValueError("scenario must have at least one segment")
        for seg in segments[:-1]:
            if seg.duration_s <= 0:
                raise ValueError("only the last scenario segment may have duration 0")
        self.segments = list(segments)
        self.hold_forever = self.segments[-1].duration_s == 0
        finite = self.segments[:-1] if self.hold_forever else self.segments
        self.total_finite_s = sum(s.duration_s for s in finite)

    def state_at(self, t: float) -> FixState:
        if self.hold_forever:
            acc = 0.0
            for seg in self.segments[:-1]:
                if t < acc + seg.duration_s:
                    return seg.state
                acc += seg.duration_s
            return self.segments[-1].state
        if self.total_finite_s <= 0:
            return self.segments[0].state
        t_mod = t % self.total_finite_s
        acc = 0.0
        for seg in self.segments:
            if t_mod < acc + seg.duration_s:
                return seg.state
            acc += seg.duration_s
        return self.segments[-1].state

    @classmethod
    def parse(cls, spec: str) -> "Scenario":
        segments = []
        for part in spec.split(","):
            part = part.strip()
            if not part:
                continue
            name, _, dur = part.partition(":")
            segments.append(ScenarioSegment(FixState[name.strip().upper()], float(dur)))
        return cls(segments)


DEFAULT_SCENARIO_STR = "FIXED:60,FLOAT:20,SINGLE:10,FIXED:0"


# --------------------------------------------------------------------------
# Trajectory: closed waypoint loop walked at constant speed
# --------------------------------------------------------------------------

class WaypointLoop:
    """A closed polyline in local ENU meters (x=east, y=north). Auto-closes
    back to the first point if not already closed."""

    def __init__(self, waypoints: Sequence[Tuple[float, float]]):
        pts = list(waypoints)
        if len(pts) < 2:
            raise ValueError("need at least 2 waypoints")
        if pts[0] != pts[-1]:
            pts.append(pts[0])
        self.pts = pts
        self.seg_lens = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:])]
        self.total_m = sum(self.seg_lens)
        if self.total_m <= 0:
            raise ValueError("waypoint loop has zero perimeter")

    def position_at(self, dist_m: float) -> Tuple[float, float, float]:
        """Returns (x, y, heading_deg) for distance travelled along the loop
        (wrapped modulo the perimeter). heading_deg is compass bearing
        (0=N, 90=E) of the current segment."""
        d = dist_m % self.total_m
        acc = 0.0
        for (a, b), seg_len in zip(zip(self.pts, self.pts[1:]), self.seg_lens):
            if seg_len == 0:
                continue
            if d <= acc + seg_len:
                frac = (d - acc) / seg_len
                x = a[0] + (b[0] - a[0]) * frac
                y = a[1] + (b[1] - a[1]) * frac
                heading = math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) % 360.0
                return x, y, heading
            acc += seg_len
        a, b = self.pts[-2], self.pts[-1]
        heading = math.degrees(math.atan2(b[0] - a[0], b[1] - a[1])) % 360.0
        return b[0], b[1], heading


# --------------------------------------------------------------------------
# Rover simulator: ties trajectory + scenario + noise -> NMEA sentences
# --------------------------------------------------------------------------

class GnssRoverSim:
    def __init__(
        self,
        start_lat: float,
        start_lon: float,
        waypoints_m: Sequence[Tuple[float, float]],
        speed_mps: float,
        rate_hz: float,
        scenario: Scenario,
        alt0_m: float = 50.0,
        talker: str = "GN",
    ):
        self.start_lat = start_lat
        self.start_lon = start_lon
        self.loop = WaypointLoop(waypoints_m)
        self.speed_mps = speed_mps
        self.rate_hz = rate_hz
        self.scenario = scenario
        self.alt0_m = alt0_m
        self.talker = talker
        self._lat_scale = 180.0 / (math.pi * EARTH_RADIUS_M)
        self._lon_scale = 180.0 / (math.pi * EARTH_RADIUS_M * math.cos(math.radians(start_lat)))

    def local_to_latlon(self, x: float, y: float) -> Tuple[float, float]:
        lat = self.start_lat + y * self._lat_scale
        lon = self.start_lon + x * self._lon_scale
        return lat, lon

    def epoch_sentences(self, t: float, rng: random.Random, seq: int) -> List[str]:
        """Return the burst of sentences (GGA, RMC, GST, GSA, VTG) for
        simulated elapsed time `t` (seconds since the sim started)."""
        state = self.scenario.state_at(t)
        prof = PROFILES[state]
        dist_m = self.speed_mps * t
        x, y, heading = self.loop.position_at(dist_m)

        if prof.h_std_m is not None:
            nx = x + rng.gauss(0.0, prof.h_std_m)
            ny = y + rng.gauss(0.0, prof.h_std_m)
            lat, lon = self.local_to_latlon(nx, ny)
            alt = self.alt0_m + rng.gauss(0.0, prof.v_std_m)
        else:
            lat = lon = alt = None

        sim_time = SIM_EPOCH + datetime.timedelta(seconds=t)

        return [
            self._gga(sim_time, lat, lon, alt, prof),
            self._rmc(sim_time, lat, lon, heading, prof),
            self._gst(sim_time, prof, rng),
            self._gsa(prof),
            self._vtg(heading, prof),
        ]

    # -- individual sentence builders -----------------------------------

    def _gga(self, dt, lat, lon, alt, prof: FixProfile) -> str:
        if lat is None:
            lat_s = ns = lon_s = ew = alt_s = ""
        else:
            lat_s, ns = to_nmea_lat(lat)
            lon_s, ew = to_nmea_lon(lon)
            alt_s = f"{alt:.2f}"
        fields = [
            nmea_time(dt), lat_s, ns, lon_s, ew,
            str(prof.quality), f"{prof.num_sv:02d}", f"{prof.hdop:.1f}",
            alt_s, "M", f"{GEOID_SEP_M:.1f}", "M", "", "",
        ]
        return build_sentence(self.talker, "GGA", fields)

    def _rmc(self, dt, lat, lon, heading, prof: FixProfile) -> str:
        status = "A" if prof.quality > 0 else "V"
        if lat is None:
            lat_s = ns = lon_s = ew = ""
        else:
            lat_s, ns = to_nmea_lat(lat)
            lon_s, ew = to_nmea_lon(lon)
        speed_kn = self.speed_mps * KNOTS_PER_MPS if prof.quality > 0 else 0.0
        fields = [
            nmea_time(dt), status, lat_s, ns, lon_s, ew,
            f"{speed_kn:.2f}", f"{heading:.1f}", nmea_date(dt), "", "", prof.mode_char,
        ]
        return build_sentence(self.talker, "RMC", fields)

    def _gst(self, dt, prof: FixProfile, rng: random.Random) -> str:
        if prof.h_std_m is None:
            fields = [nmea_time(dt), "", "", "", "", "", "", ""]
        else:
            def jitter(std: float) -> float:
                return std * rng.uniform(0.9, 1.1)

            rms = jitter(prof.h_std_m)
            semi_major = jitter(prof.h_std_m)
            semi_minor = jitter(prof.h_std_m * 0.8)
            lat_err = jitter(prof.h_std_m)
            lon_err = jitter(prof.h_std_m)
            alt_err = jitter(prof.v_std_m)
            fields = [
                nmea_time(dt), f"{rms:.3f}", f"{semi_major:.3f}", f"{semi_minor:.3f}",
                "0.0", f"{lat_err:.3f}", f"{lon_err:.3f}", f"{alt_err:.3f}",
            ]
        return build_sentence(self.talker, "GST", fields)

    def _gsa(self, prof: FixProfile) -> str:
        n_listed = min(prof.num_sv, 12)
        sats = [str(i + 1) for i in range(n_listed)] + [""] * (12 - n_listed)
        fields = ["A", str(prof.gsa_fix_type)] + sats + [
            f"{prof.pdop:.1f}", f"{prof.hdop:.1f}", f"{prof.vdop:.1f}",
        ]
        return build_sentence(self.talker, "GSA", fields)

    def _vtg(self, heading, prof: FixProfile) -> str:
        speed_kn = self.speed_mps * KNOTS_PER_MPS if prof.quality > 0 else 0.0
        speed_kmh = self.speed_mps * KMH_PER_MPS if prof.quality > 0 else 0.0
        fields = [
            f"{heading:.1f}", "T", "", "M",
            f"{speed_kn:.2f}", "N", f"{speed_kmh:.2f}", "K", prof.mode_char,
        ]
        return build_sentence(self.talker, "VTG", fields)


DEFAULT_WAYPOINTS_M: List[Tuple[float, float]] = [(0.0, 0.0), (40.0, 0.0), (40.0, 25.0), (0.0, 25.0)]

# Central, Hong Kong -- the owner is HK-based, so the default trajectory
# starts somewhere real rather than null-island.
DEFAULT_START_LAT = 22.2830
DEFAULT_START_LON = 114.1585


def parse_waypoints(spec: str) -> List[Tuple[float, float]]:
    pts = []
    for pair in spec.split(";"):
        pair = pair.strip()
        if not pair:
            continue
        dx, dy = pair.split(",")
        pts.append((float(dx), float(dy)))
    return pts


# --------------------------------------------------------------------------
# Output mode: TCP server
# --------------------------------------------------------------------------

class TcpNmeaServer:
    """Streams `sim`'s sentence epochs to every connected TCP client.

    `time_scale` > 1 compresses wall-clock pacing relative to simulated
    time (e.g. time_scale=40 makes 90 simulated seconds take ~2.25s of
    wall time) -- used by the self-test to exercise a multi-minute
    fix-quality scenario without actually waiting minutes.
    """

    def __init__(
        self,
        sim: GnssRoverSim,
        rate_hz: float,
        host: str = "127.0.0.1",
        port: int = 0,
        time_scale: float = 1.0,
        duration: Optional[float] = None,
        seed: Optional[int] = None,
    ):
        self.sim = sim
        self.rate_hz = rate_hz
        self.host = host
        self._port = port
        self.time_scale = time_scale
        self.duration = duration
        self.seed = seed

        self._sock: Optional[socket.socket] = None
        self._clients: List[socket.socket] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._accept_thread: Optional[threading.Thread] = None
        self._gen_thread: Optional[threading.Thread] = None
        self.epochs_sent = 0

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
        self._gen_thread = threading.Thread(target=self._gen_loop, daemon=True)
        self._gen_thread.start()

    def _accept_loop(self) -> None:
        assert self._sock is not None
        self._sock.settimeout(0.5)
        while not self._stop.is_set():
            try:
                conn, _addr = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self._lock:
                self._clients.append(conn)

    def _gen_loop(self) -> None:
        rng = random.Random(self.seed)
        dt = 1.0 / self.rate_hz
        t = 0.0
        seq = 0
        while not self._stop.is_set():
            if self.duration is not None and t > self.duration:
                break
            lines = self.sim.epoch_sentences(t, rng, seq)
            data = "".join(lines).encode("ascii")
            with self._lock:
                dead = []
                for c in self._clients:
                    try:
                        c.sendall(data)
                    except OSError:
                        dead.append(c)
                for c in dead:
                    self._clients.remove(c)
                    try:
                        c.close()
                    except OSError:
                        pass
            self.epochs_sent += 1
            seq += 1
            t += dt
            time.sleep(max(0.0, dt / self.time_scale))
        # duration reached: keep listening (don't tear down) but stop generating
        self._stop.set()

    def stop(self) -> None:
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        with self._lock:
            for c in self._clients:
                try:
                    c.close()
                except OSError:
                    pass
            self._clients.clear()


# --------------------------------------------------------------------------
# Output mode: PTY (virtual serial port)
# --------------------------------------------------------------------------

def run_pty(sim: GnssRoverSim, rate_hz: float, time_scale: float,
            duration: Optional[float], seed: Optional[int]) -> None:
    import pty  # POSIX-only; pty mode is not available on Windows.

    master, slave = pty.openpty()
    slave_name = os.ttyname(slave)
    print(f"NMEA sim serial port: {slave_name}")
    print("Connect a serial client to this path (e.g. `screen {} 9600`, or the "
          "engine's BtNmeaSource/serial transport in test mode).".format(slave_name))
    sys.stdout.flush()

    rng = random.Random(seed)
    dt = 1.0 / rate_hz
    t = 0.0
    seq = 0
    try:
        while duration is None or t <= duration:
            data = "".join(sim.epoch_sentences(t, rng, seq)).encode("ascii")
            os.write(master, data)  # blocks if the pty's reader is slow/absent -- matches a live UART's backpressure closely enough for a spike
            seq += 1
            t += dt
            time.sleep(max(0.0, dt / time_scale))
    except KeyboardInterrupt:
        pass
    finally:
        os.close(master)
        os.close(slave)


# --------------------------------------------------------------------------
# Output mode: file
# --------------------------------------------------------------------------

def run_file(sim: GnssRoverSim, rate_hz: float, time_scale: float, duration: float,
             path: str, seed: Optional[int], realtime: bool = False) -> int:
    rng = random.Random(seed)
    dt = 1.0 / rate_hz
    t = 0.0
    seq = 0
    with open(path, "w", newline="") as f:
        while t <= duration:
            for line in sim.epoch_sentences(t, rng, seq):
                f.write(line)
            seq += 1
            t += dt
            if realtime:
                time.sleep(max(0.0, dt / time_scale))
    return seq


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="LidarScan S5 spike: GNSS rover NMEA 0183 simulator")
    p.add_argument("--mode", choices=["pty", "tcp", "file"], default="tcp")
    p.add_argument("--host", default="127.0.0.1", help="tcp mode: bind host")
    p.add_argument("--port", type=int, default=9500, help="tcp mode: bind port (0 = pick free port)")
    p.add_argument("--file", default="nmea_sim_output.txt", help="file mode: output path")
    p.add_argument("--rate", type=float, default=5.0, help="sentence epoch rate in Hz (1-5)")
    p.add_argument("--speed", type=float, default=1.0, help="walking speed, m/s")
    p.add_argument("--start-lat", type=float, default=DEFAULT_START_LAT)
    p.add_argument("--start-lon", type=float, default=DEFAULT_START_LON)
    p.add_argument("--waypoints", default="0,0;40,0;40,25;0,25",
                    help="semicolon-separated dx,dy meters (local ENU) forming the walking loop")
    p.add_argument("--scenario", default=DEFAULT_SCENARIO_STR,
                    help='fix-quality timeline, e.g. "FIXED:60,FLOAT:20,SINGLE:10,FIXED:0" '
                         '(last segment duration 0 = hold forever; otherwise the sequence loops)')
    p.add_argument("--duration", type=float, default=None,
                    help="sim seconds to run; required for file mode, optional cap for pty/tcp")
    p.add_argument("--time-scale", type=float, default=1.0,
                    help=">1 speeds up wall-clock pacing relative to simulated time (tcp/pty/--realtime-file)")
    p.add_argument("--seed", type=int, default=None, help="RNG seed for reproducible noise")
    p.add_argument("--talker", default="GN", help="NMEA talker ID (GN=multi-GNSS, GP=GPS-only, ...)")
    p.add_argument("--realtime-file", action="store_true",
                    help="pace file-mode output in wall-clock time instead of writing as fast as possible")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    waypoints = parse_waypoints(args.waypoints)
    scenario = Scenario.parse(args.scenario)
    sim = GnssRoverSim(
        args.start_lat, args.start_lon, waypoints, args.speed, args.rate,
        scenario, talker=args.talker,
    )

    if args.mode == "file":
        if args.duration is None:
            print("error: --duration is required for --mode file", file=sys.stderr)
            return 2
        n = run_file(sim, args.rate, args.time_scale, args.duration, args.file, args.seed,
                      realtime=args.realtime_file)
        print(f"wrote {n} epochs to {args.file}")
        return 0

    if args.mode == "pty":
        run_pty(sim, args.rate, args.time_scale, args.duration, args.seed)
        return 0

    server = TcpNmeaServer(sim, args.rate, args.host, args.port, args.time_scale, args.duration, args.seed)
    server.start()
    print(f"NMEA TCP server listening on {args.host}:{server.port}  scenario={args.scenario!r}")
    sys.stdout.flush()
    try:
        while server._gen_thread is not None and server._gen_thread.is_alive():
            time.sleep(0.2)
        if args.duration is not None:
            print(f"duration reached ({args.duration}s sim time); server still accepting connections, Ctrl+C to exit")
            while True:
                time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
