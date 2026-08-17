#!/usr/bin/env python3
"""replay_mid360_heartbeat.py -- test-only UDP heartbeat replayer for the
desktop auto-detect feature (docs/design/REVIEW_FEEDBACK.md 2026-08-17 round
4 item 5), used when spikes/s2-mid360-sim's mid360_sim cannot stand in for a
real Mid-360's unsolicited heartbeat.

WHY THIS EXISTS
----------------
scanengine::discovery::DiscoverMid360() is a PASSIVE listener: per
engine/include/scanengine/transport/udp_source.h, a Mid-360 pushes its state
(cmd 0x0102, "push-state") at 1 Hz to `host_push_port` (56201 by default) once
it has been told to -- and captures/FIELD_SESSION_2026-08-17.md's field notes
("heartbeat broadcasts reach any-bound sockets only") say discovery reads
that traffic directly rather than going through the vendored SDK2's own
(macOS-broken, per spikes/s2-mid360-sim REPORT.md section 3's
0003-darwin-no-broadcast-bind.patch) discovery path.

mid360_sim CANNOT reproduce this unprompted: REPORT.md section 5 documents
its 0x0102 push as answering an SDK client that has ALREADY driven a real
0x0100 host-IP handshake (sdk_client_demo does this on startup) -- there is
no free-running, un-handshaken heartbeat to listen for, only the discovery
ACK on port 56000 (section 4c: "announces unprompted... to the host's
control port", i.e. that one IS free-running, but it is not what port 56201
carries). This script closes that gap for auto-detect verification: it does
NOT synthesize a packet, it re-sends the REAL, byte-for-byte push-state
payloads a real Mid-360 (SN MCP7K0034759, fw 35010108, lidar IP
192.168.1.159, persisted host 192.168.1.5) put on port 56201 during the
field session's 30 s capture (captures/mid360_real_30s.livoxdump), at their
original cadence, to 127.0.0.1:56201 -- i.e. exactly where DiscoverMid360()
listens on loopback.

WHAT IS REAL VS SYNTHESIZED
----------------------------
Real: every byte of every push-state UDP payload (SN, firmware, lidar IP,
persisted host IP, all other KVs), and the relative timing between them
(these packets arrived roughly 1 Hz apart on the real device; this script
reproduces that spacing rather than blasting them out back-to-back).
Synthesized: nothing in the payload. The only synthetic part of this tool is
its OWN existence as a substitute unsolicited-broadcast source -- the file
header comment above states that plainly, as the task asked.

USAGE
-----
    python3 replay_mid360_heartbeat.py
        # loops captures/mid360_real_30s.livoxdump's port-56201 records to
        # 127.0.0.1:56201 forever, at their original ~1 Hz cadence

    python3 replay_mid360_heartbeat.py --once
        # one pass through the 30 recorded packets (~29 s), then exit

    python3 replay_mid360_heartbeat.py --dump PATH --host 127.0.0.1 --port 56201 \\
        --speed 4 --loops 3

Standard library only (socket, struct, time) -- nothing to install, same
posture as tools/remote-capture/*.py and spikes/s2-mid360-sim/scripts/*.py.
"""
import argparse
import os
import socket
import struct
import sys
import time

MAGIC = b"LX360CAP"
_FILE_HDR_FIXED = struct.Struct("<8sHH")   # magic, version, num_ports
_RECORD_HDR = struct.Struct("<QHI")        # t_ns, port_idx, len

DEFAULT_DUMP = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "..", "captures", "mid360_real_30s.livoxdump")
)


def load_port_records(path, want_port):
    """Returns [(t_ns, payload_bytes), ...] for every record on UDP port
    `want_port`, in file order (which is arrival order)."""
    with open(path, "rb") as f:
        data = f.read()

    magic, version, num_ports = _FILE_HDR_FIXED.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: not a .livoxdump file (bad magic {magic!r})")
    off = _FILE_HDR_FIXED.size
    ports = []
    for _ in range(num_ports):
        (p,) = struct.unpack_from("<I", data, off)
        ports.append(p)
        off += 4
    if want_port not in ports:
        raise ValueError(f"{path}: port {want_port} not in this capture's port table {ports}")
    want_idx = ports.index(want_port)

    out = []
    while off < len(data):
        if off + _RECORD_HDR.size > len(data):
            break  # truncated tail record (a live capture can end mid-write) -- stop, not an error
        t_ns, port_idx, length = _RECORD_HDR.unpack_from(data, off)
        off += _RECORD_HDR.size
        if off + length > len(data):
            break
        payload = data[off:off + length]
        off += length
        if port_idx == want_idx:
            out.append((t_ns, payload))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", default=DEFAULT_DUMP,
                    help=f"source .livoxdump (default: {DEFAULT_DUMP})")
    ap.add_argument("--source-port", type=int, default=56201,
                    help="which port's records to replay from the dump (default: 56201, "
                         "the Mid-360's host_push_port -- see udp_source.h)")
    ap.add_argument("--host", default="127.0.0.1", help="destination host (default: 127.0.0.1)")
    ap.add_argument("--port", type=int, default=None,
                    help="destination UDP port (default: same as --source-port)")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="playback speed multiplier (default: 1.0 -- real cadence)")
    ap.add_argument("--loops", type=int, default=0, help="0 = forever (default), else N passes")
    ap.add_argument("--once", action="store_true", help="shorthand for --loops 1")
    args = ap.parse_args()

    dest_port = args.port if args.port is not None else args.source_port
    loops = 1 if args.once else args.loops

    records = load_port_records(args.dump, args.source_port)
    if not records:
        print(f"[replay-heartbeat] no port-{args.source_port} records in {args.dump}",
              file=sys.stderr)
        return 1
    span_s = (records[-1][0] - records[0][0]) / 1e9
    print(f"[replay-heartbeat] {len(records)} real push-state packets from {args.dump}, "
          f"{span_s:.1f}s span, first {len(records[0][1])}B -> {args.host}:{dest_port} "
          f"(speed {args.speed}x, {'forever' if loops == 0 else f'{loops} loop(s)'})",
          file=sys.stderr)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    loop_idx = 0
    try:
        while True:
            loop_idx += 1
            prev_t_ns = None
            for t_ns, payload in records:
                if prev_t_ns is not None:
                    gap_s = (t_ns - prev_t_ns) / 1e9 / max(args.speed, 1e-6)
                    if gap_s > 0:
                        time.sleep(gap_s)
                sock.sendto(payload, (args.host, dest_port))
                sent += 1
                prev_t_ns = t_ns
            print(f"[replay-heartbeat] loop {loop_idx} done ({len(records)} packets, "
                  f"{sent} total)", file=sys.stderr)
            if loops and loop_idx >= loops:
                break
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
    print(f"[replay-heartbeat] exiting, {sent} packets sent", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
