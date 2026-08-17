#!/usr/bin/env python3
"""serial_probe_fakes.py -- pty-based fakes for verifying
scanengine::discovery::ProbeSerialD6 / ProbeSerialUm982 without real
hardware attached (docs/design/REVIEW_FEEDBACK.md 2026-08-17 round 4 item 5,
the task's "D6/UM982 probe paths against pty-based fakes if feasible").

Opens a BSD pseudo-terminal (Python's stdlib `pty` module, no socat/external
dependency), sets the MASTER side to raw mode so nothing it writes gets
cooked-mode-mangled (echo, CR/LF translation, etc. would corrupt a binary D6
frame), then replays REAL captured bytes into the master end while the
SLAVE end's device path is handed to a probe.

Fixtures used are REAL captures already in this repo, not synthesized:
  captures/bench_d6_30s.bin   -- real COIN-D6 byte stream (engine/tests
                                  ships this as field_d6_30s.bin too)
  captures/um982_30s.nmea     -- real UM982 NMEA stream (7 sentence types,
                                  incl. GPTHS => dual-antenna heading)

Usage:
    python3 serial_probe_fakes.py d6 [--speed N]
    python3 serial_probe_fakes.py um982 [--speed N]

Either prints the pty slave path to stdout and then streams the fixture
bytes into the master end at roughly the real capture's byte rate (D6:
230400 8N1 ~= 23040 B/s; UM982: paced by NMEA line count over the capture's
nominal 30 s) until EOF, then exits. Meant to be run in the background while
a small companion C++ program (see NOTES.md section on this script) opens
the printed slave path and calls ProbeSerialD6/ProbeSerialUm982 on it.
"""
import argparse
import os
import pty
import sys
import termios
import time

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
D6_FIXTURE = os.path.join(REPO_ROOT, "captures", "bench_d6_30s.bin")
UM982_FIXTURE = os.path.join(REPO_ROOT, "captures", "um982_30s.nmea")


def make_raw_pty():
    master_fd, slave_fd = pty.openpty()
    slave_path = os.ttyname(slave_fd)
    # Raw mode on the master: no line discipline, no echo, no CR/LF mangling
    # -- a D6 AA55 frame or an NMEA line must arrive byte-for-byte.
    attrs = termios.tcgetattr(master_fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[3] = 0  # lflag (no ECHO, no ICANON)
    termios.tcsetattr(master_fd, termios.TCSANOW, attrs)
    return master_fd, slave_fd, slave_path


def stream_d6(master_fd, speed):
    with open(D6_FIXTURE, "rb") as f:
        data = f.read()
    print(f"[serial-fake] streaming {len(data)}B from {D6_FIXTURE} (real D6 capture) "
          f"at {230400 * speed / 8:.0f} B/s", file=sys.stderr)
    chunk = max(1, int(230400 * speed / 8 / 20))  # ~50ms chunks
    delay = chunk / (230400 * speed / 8)
    for i in range(0, len(data), chunk):
        os.write(master_fd, data[i:i + chunk])
        time.sleep(delay)


def stream_um982(master_fd, speed):
    with open(UM982_FIXTURE, "rb") as f:
        lines = f.readlines()
    print(f"[serial-fake] streaming {len(lines)} NMEA lines from {UM982_FIXTURE} "
          f"(real UM982 capture) at {speed}x", file=sys.stderr)
    # The real capture is ~30s of 1 Hz epochs (7 sentences/epoch); reproduce
    # that burst-then-pause shape rather than a flat rate.
    per_epoch = 7
    for i in range(0, len(lines), per_epoch):
        for line in lines[i:i + per_epoch]:
            os.write(master_fd, line if line.endswith(b"\n") else line + b"\r\n")
        time.sleep(1.0 / max(speed, 1e-6))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("device", choices=["d6", "um982"])
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--loop", action="store_true", help="repeat the fixture until killed")
    args = ap.parse_args()

    master_fd, slave_fd, slave_path = make_raw_pty()
    print(slave_path)  # the ONE line a wrapper script should capture
    sys.stdout.flush()
    print(f"[serial-fake] pty slave ready at {slave_path}", file=sys.stderr)

    try:
        while True:
            if args.device == "d6":
                stream_d6(master_fd, args.speed)
            else:
                stream_um982(master_fd, args.speed)
            if not args.loop:
                break
    except (BrokenPipeError, OSError) as exc:
        print(f"[serial-fake] stream ended: {exc}", file=sys.stderr)
    finally:
        os.close(master_fd)
        os.close(slave_fd)


if __name__ == "__main__":
    main()
