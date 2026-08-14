"""test_roundtrip.py -- end-to-end self-test for spike S5 (RTK simulation infra).

Covers the exit bar from the spike brief:

  * caster up -> a simple test client (implemented right here) connects
    with basic auth, receives N valid (CRC-correct) RTCM3 frames, and
    uploads a GGA sentence which the caster logs
  * caster drop/reconnect injection actually forces a disconnect and the
    client can reconnect and keep reading valid frames
  * nmea_sim in TCP mode: parse ~90 simulated seconds of sentences,
    verify every checksum, and confirm the scripted fix-quality timeline
    ("RTK Fixed 60s -> Float 20s -> Single 10s -> Fixed") is reproduced
    in the GGA quality field in order and at the right times
  * a strict third-party NMEA parser (pynmea2) accepts our sentences,
    if it's installed (requirements.txt) -- this stands in for "a strict
    NMEA parser built against the standard accepts it"

Run with:  python3 test_roundtrip.py -v   (or: python3 -m unittest -v test_roundtrip)
"""

from __future__ import annotations

import base64
import os
import socket
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import nmea_sim
import ntrip_caster_sim
import rtcm_tool

try:
    import pynmea2
    HAVE_PYNMEA2 = True
except ImportError:
    HAVE_PYNMEA2 = False


def recv_until(sock: socket.socket, marker: bytes, timeout: float = 2.0) -> bytes:
    sock.settimeout(timeout)
    data = b""
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    return data


def recv_for(sock: socket.socket, seconds: float) -> bytes:
    sock.settimeout(0.3)
    data = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        data += chunk
    return data


class TestRtcmFraming(unittest.TestCase):
    """rtcm_tool.py sanity: build_frame / iter_frames round-trip and the
    canned-stream generator produce valid, CRC-correct frames."""

    def test_build_and_validate_roundtrip(self):
        payload = b"an arbitrary RTCM payload, transport bytes only"
        frame = rtcm_tool.build_frame(payload)
        self.assertEqual(frame[0], 0xD3)
        results = list(rtcm_tool.iter_frames(frame))
        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].crc_ok)
        self.assertEqual(results[0].length, len(payload))

    def test_corrupted_frame_fails_crc(self):
        frame = bytearray(rtcm_tool.build_frame(b"hello"))
        frame[-1] ^= 0xFF  # flip a CRC byte
        results = list(rtcm_tool.iter_frames(bytes(frame)))
        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].crc_ok)

    def test_generate_canned_stream(self):
        path = os.path.join(tempfile.gettempdir(), "s5_test_canned.rtcm")
        n = rtcm_tool.generate_canned_stream(path, types=[1005, 1077, 1087], count=30, seed=1)
        self.assertEqual(n, 30)
        with open(path, "rb") as f:
            data = f.read()
        results = list(rtcm_tool.iter_frames(data))
        self.assertEqual(len(results), 30)
        self.assertTrue(all(r.crc_ok for r in results))
        self.assertTrue(set(r.msg_type for r in results) <= {1005, 1077, 1087})


class TestNtripCasterRoundtrip(unittest.TestCase):
    """caster up -> client connects with auth -> receives N valid RTCM
    frames -> uploads a GGA sentence -> caster logs it. Plus sourcetable,
    auth-rejection, and drop/reconnect-injection checks."""

    def setUp(self):
        self.fixture_path = os.path.join(tempfile.gettempdir(), "s5_test_stream.rtcm")
        rtcm_tool.generate_canned_stream(
            self.fixture_path, types=[1005, 1077, 1087, 1097, 1127, 1230], count=100, seed=7)
        self.mount = ntrip_caster_sim.MountConfig(
            name="TEST", username="tester", password="secret",
            rtcm_file=self.fixture_path, frame_interval_s=0.02)
        self.caster = ntrip_caster_sim.NtripCasterSim([self.mount], host="127.0.0.1", port=0)
        self.caster.start()
        time.sleep(0.1)

    def tearDown(self):
        self.caster.stop()

    def test_sourcetable_v2(self):
        sock = socket.create_connection((self.caster.host, self.caster.port), timeout=5)
        sock.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nNtrip-Version: Ntrip/2.0\r\nUser-Agent: S5Test/1.0\r\n\r\n")
        data = recv_for(sock, 1.0)
        sock.close()
        self.assertIn(b"SOURCETABLE 200 OK", data)
        self.assertIn(b"STR;TEST;", data)
        self.assertIn(b"ENDSOURCETABLE", data)

    def test_sourcetable_v1(self):
        sock = socket.create_connection((self.caster.host, self.caster.port), timeout=5)
        sock.sendall(b"GET / HTTP/1.0\r\n\r\n")
        data = recv_for(sock, 1.0)
        sock.close()
        self.assertIn(b"SOURCETABLE 200 OK", data)
        self.assertIn(b"STR;TEST;", data)

    def test_auth_required(self):
        sock = socket.create_connection((self.caster.host, self.caster.port), timeout=5)
        sock.sendall(b"GET /TEST HTTP/1.1\r\nHost: x\r\nNtrip-Version: Ntrip/2.0\r\n\r\n")
        data = recv_for(sock, 1.0)
        sock.close()
        self.assertIn(b"401", data)

    def test_unknown_mountpoint(self):
        sock = socket.create_connection((self.caster.host, self.caster.port), timeout=5)
        sock.sendall(b"GET /NOPE HTTP/1.1\r\nHost: x\r\nNtrip-Version: Ntrip/2.0\r\n\r\n")
        data = recv_for(sock, 1.0)
        sock.close()
        self.assertIn(b"404", data)

    def test_stream_and_gga_upload(self):
        sock = socket.create_connection((self.caster.host, self.caster.port), timeout=5)
        cred = base64.b64encode(b"tester:secret").decode()
        req = (
            f"GET /TEST HTTP/1.1\r\nHost: x\r\nNtrip-Version: Ntrip/2.0\r\n"
            f"User-Agent: S5Test/1.0\r\nAuthorization: Basic {cred}\r\n\r\n"
        )
        sock.sendall(req.encode())
        status = recv_until(sock, b"\r\n\r\n", timeout=2.0)
        self.assertIn(b"200", status)

        # Upload a GGA sentence, as a real rover periodically does on the
        # same connection so a VRS/network-RTK caster can pick the nearest
        # correction source.
        gga_body = "GNGGA,120000.00,2216.9800,N,11409.5100,E,4,22,0.6,50.0,M,-2.0,M,,"
        gga_line = f"${gga_body}*{nmea_sim.nmea_checksum(gga_body)}\r\n"
        sock.sendall(gga_line.encode())

        target_frames = 20
        buf = b""
        deadline = time.time() + 5.0
        sock.settimeout(0.5)
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            buf += chunk
            if len(list(rtcm_tool.iter_frames(buf))) >= target_frames:
                break
        sock.close()

        frames = list(rtcm_tool.iter_frames(buf))
        self.assertGreaterEqual(len(frames), target_frames)
        for fr in frames[:target_frames]:
            self.assertTrue(fr.crc_ok)

        # Give the reader thread a moment to log the GGA upload.
        deadline = time.time() + 2.0
        while time.time() < deadline:
            with self.mount._lock:
                if self.mount.gga_log:
                    break
            time.sleep(0.05)
        with self.mount._lock:
            self.assertTrue(any("GGA" in text for _, text in self.mount.gga_log))

    def test_reconnect_after_drop_injection(self):
        mount = ntrip_caster_sim.MountConfig(
            name="TEST2", username="tester", password="secret", rtcm_file=self.fixture_path,
            frame_interval_s=0.01, drop_after_frames=5, max_drops=1)
        caster = ntrip_caster_sim.NtripCasterSim([mount], host="127.0.0.1", port=0)
        caster.start()
        time.sleep(0.1)
        try:
            cred = base64.b64encode(b"tester:secret").decode()
            req = f"GET /TEST2 HTTP/1.1\r\nHost: x\r\nNtrip-Version: Ntrip/2.0\r\nAuthorization: Basic {cred}\r\n\r\n"

            sock = socket.create_connection((caster.host, caster.port), timeout=5)
            sock.sendall(req.encode())
            recv_until(sock, b"\r\n\r\n", timeout=2.0)
            first_run = recv_for(sock, 2.0)
            sock.close()
            self.assertGreater(len(first_run), 0, "expected some RTCM bytes before the injected drop")
            self.assertEqual(mount.drops_used, 1, "the caster should have force-closed the connection once")

            # Reconnect (this is what A10's NTRIP client is expected to do)
            # and confirm the caster serves valid frames again without
            # applying a second drop (max_drops=1 was exhausted).
            sock2 = socket.create_connection((caster.host, caster.port), timeout=5)
            sock2.sendall(req.encode())
            recv_until(sock2, b"\r\n\r\n", timeout=2.0)
            second_run = recv_for(sock2, 1.0)
            sock2.close()
            self.assertGreater(len(second_run), 0, "expected RTCM bytes after reconnecting")
            frames = list(rtcm_tool.iter_frames(second_run))
            self.assertTrue(all(f.crc_ok for f in frames))
        finally:
            caster.stop()


class TestNmeaSimTcp(unittest.TestCase):
    """nmea_sim in TCP mode: 90 simulated seconds ("RTK Fixed 60s -> Float
    20s -> Single 10s -> Fixed"), read over the wire, every sentence
    checksum-verified, and the fix-quality timeline reproduced in order.

    Uses --time-scale to compress ~90s of *simulated* time into a few
    seconds of *wall* time so the test runs fast; the sentence cadence
    and content are otherwise identical to real-time operation.
    """

    SCENARIO = "FIXED:60,FLOAT:20,SINGLE:10,FIXED:0"
    RATE_HZ = 5.0
    SIM_DURATION_S = 95.0
    TIME_SCALE = 50.0

    def setUp(self):
        waypoints = nmea_sim.DEFAULT_WAYPOINTS_M
        scenario = nmea_sim.Scenario.parse(self.SCENARIO)
        self.sim = nmea_sim.GnssRoverSim(
            nmea_sim.DEFAULT_START_LAT, nmea_sim.DEFAULT_START_LON, waypoints,
            speed_mps=1.0, rate_hz=self.RATE_HZ, scenario=scenario)
        self.server = nmea_sim.TcpNmeaServer(
            self.sim, rate_hz=self.RATE_HZ, host="127.0.0.1", port=0,
            time_scale=self.TIME_SCALE, duration=self.SIM_DURATION_S, seed=123)
        self.server.start()
        time.sleep(0.1)

    def tearDown(self):
        self.server.stop()

    def test_checksums_and_fix_timeline(self):
        sock = socket.create_connection((self.server.host, self.server.port), timeout=10)
        sock.settimeout(0.5)
        buf = ""
        all_lines = []
        ggas = []
        expected_epochs = int(self.SIM_DURATION_S * self.RATE_HZ)
        wall_deadline = time.time() + max(15.0, self.SIM_DURATION_S / self.TIME_SCALE + 10.0)

        while time.time() < wall_deadline and len(ggas) < expected_epochs:
            try:
                chunk = sock.recv(8192).decode("ascii", errors="replace")
            except socket.timeout:
                if self.server._gen_thread is not None and not self.server._gen_thread.is_alive():
                    break
                continue
            if not chunk:
                break
            buf += chunk
            while "\r\n" in buf:
                line, buf = buf.split("\r\n", 1)
                if not line:
                    continue
                all_lines.append(line)
                self._assert_checksum(line)
                if "GGA" in line:
                    ggas.append(line)
        sock.close()

        # We may not catch the very first epochs (the generator thread can
        # start producing before this test's socket connects), so don't
        # assume the first GGA received is epoch 0 -- recover the actual
        # simulated elapsed time from each sentence's own timestamp field
        # instead (SIM_EPOCH is midnight, so hh:mm:ss.ss == seconds of sim
        # time elapsed, well within the ~95s span used here).
        self.assertGreater(len(ggas), expected_epochs * 0.5,
                            f"only received {len(ggas)} GGA sentences, expected ~{expected_epochs}")

        def sim_seconds_of(gga_line: str) -> float:
            time_field = gga_line.split(",")[1]  # hhmmss.ss
            hh, mm, ss = int(time_field[0:2]), int(time_field[2:4]), float(time_field[4:])
            return hh * 3600.0 + mm * 60.0 + ss

        qualities = [int(g.split(",")[6]) for g in ggas]
        times = [sim_seconds_of(g) for g in ggas]

        def expected_quality(t: float) -> int:
            if t < 60:
                return 4  # RTK Fixed
            if t < 80:
                return 5  # RTK Float
            if t < 90:
                return 1  # Single
            return 4      # back to Fixed, held forever

        mismatches = sum(1 for t, q in zip(times, qualities) if q != expected_quality(t))
        mismatch_rate = mismatches / len(qualities)
        self.assertLess(mismatch_rate, 0.02,
                         f"fix-quality timeline mismatch rate too high: {mismatch_rate:.3%}")

        # Confirm the transition SEQUENCE (not just per-sample quality) is
        # reproduced: Fixed -> Float -> Single -> Fixed, each state actually
        # observed and in the right order.
        seen_sequence = []
        for q in qualities:
            if not seen_sequence or seen_sequence[-1] != q:
                seen_sequence.append(q)
        # collapse any single-sample jitter blips at the boundaries
        collapsed = [seen_sequence[0]]
        for q in seen_sequence[1:]:
            if q != collapsed[-1]:
                collapsed.append(q)
        self.assertEqual(collapsed[:4], [4, 5, 1, 4],
                          f"unexpected fix-state sequence: {collapsed}")

        if HAVE_PYNMEA2:
            for line in all_lines[:200]:
                msg = pynmea2.parse(line)
                self.assertIsNotNone(msg)

    def _assert_checksum(self, line: str):
        self.assertTrue(line.startswith("$"), f"sentence missing '$': {line!r}")
        body, sep, cs = line[1:].partition("*")
        self.assertEqual(sep, "*", f"sentence missing checksum delimiter: {line!r}")
        self.assertEqual(len(cs), 2, f"checksum should be 2 hex chars: {line!r}")
        calc = nmea_sim.nmea_checksum(body)
        self.assertEqual(calc.upper(), cs.upper(), f"checksum mismatch in {line!r}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
