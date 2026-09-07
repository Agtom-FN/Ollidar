#!/usr/bin/env python3
"""
capture_mid70.py -- record raw Livox Mid-70 (SDK v1) UDP traffic to a .livoxdump

The Mid-70 speaks Livox SDK v1, not the SDK2 protocol capture_mid360.py
listens for. Two kinds of datagram matter, and this script records both,
verbatim, with an arrival timestamp:

  1. BROADCAST frames. While no host has connected, the lidar sends an SDK
     command frame to UDP port 55000 about once a second announcing its
     broadcast code (the 15-character code on the label), device type and
     IP. These are what discovery listens for, and the parser's test fixture
     (engine/tests/integration/data/mid70_broadcast.bin) is cut from a real
     capture of them -- the CRC parameters are verified against bytes the
     device actually sent, not against a datasheet.

  2. POINT datagrams. The lidar only sends these after an SDK v1 handshake
     has told it which host port to stream to. Run the ROS driver, the SDK's
     lidar_sample, or the Ollidar engine's SDK v1 backend on THIS host so the
     handshake happens, then capture the data port with --data-port. The
     port is whatever that software bound: livox_ros_driver / Livox-SDK use
     an ephemeral port chosen by the SDK; see the notes below on finding it.

USAGE
    # broadcasts only (no other software running; the port must be free):
    python3 capture_mid70.py --seconds 10 --broadcast --out mid70_broadcast.livoxdump

    # points, while a driver on this host has already done the handshake.
    # Find the data port with:  ss -ulnp | grep livox   (Linux)  or
    #                           lsof -nP -iUDP | grep -i livox   (macOS)
    python3 capture_mid70.py --seconds 30 --data-port 60001 --out mid70_real_30s.livoxdump

    The two can be combined (--broadcast --data-port N) if the SDK is not the
    one holding 55000 -- it normally is, so capture broadcasts FIRST, with
    nothing else running.

NOTE ON PORT SHARING. The SDK's own broadcast listener binds 55000 without
SO_REUSEPORT on most platforms, so this script and a running SDK cannot both
receive broadcasts. That is why mode 1 wants nothing else running.

FILE FORMAT -- the same container as capture_mid360.py, different magic so
verify_capture.py can tell them apart:

  Header (fixed):
    8 bytes   magic       b"LX70_CAP"
    u16 LE    version     currently 1
    u16 LE    num_ports   N
    N x u32 LE  port_table  the UDP port bound for port_idx 0..N-1
                           (port 55000 is always the broadcast port)

  Records, back to back until EOF:
    u64 LE    t_ns        arrival time, nanoseconds, time.time_ns() epoch
    u16 LE    port_idx    index into port_table
    u32 LE    len         datagram length
    len bytes payload     the datagram exactly as received

  A truncated last record can happen if the process was killed mid-write;
  readers stop at the first incomplete record.
"""

import argparse
import select
import socket
import struct
import sys
import time

MAGIC = b"LX70_CAP"
VERSION = 1
BROADCAST_PORT = 55000

RECORD_HDR = struct.Struct("<QHI")  # t_ns, port_idx, len
FILE_HDR_FIXED = struct.Struct("<8sHH")  # magic, version, num_ports


def bind_sockets(host_ip, ports):
    socks = []
    for port in ports:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Broadcast frames arrive addressed to 255.255.255.255; binding the
        # wildcard receives them on every platform. A specific --host-ip is
        # only useful for the data port on a multi-NIC host.
        bind_ip = "0.0.0.0" if port == BROADCAST_PORT else host_ip
        try:
            s.bind((bind_ip, port))
        except OSError as exc:
            for opened in socks:
                opened.close()
            hint = ("something else owns this port -- for 55000 that is usually the Livox "
                    "SDK inside a running driver (livox_ros_driver, Livox Viewer, the Ollidar "
                    "engine). Stop it and re-run."
                    if port == BROADCAST_PORT else
                    "something else owns this port, or it is not the port the driver bound.")
            raise OSError(f"could not bind {bind_ip}:{port} -- {exc}\n  {hint}") from exc
        s.setblocking(False)
        socks.append(s)
    return socks


def write_header(f, ports):
    f.write(FILE_HDR_FIXED.pack(MAGIC, VERSION, len(ports)))
    for p in ports:
        f.write(struct.pack("<I", p))


def looks_like_broadcast(data):
    # SDK v1 command frame: 0xAA preamble, version 1; broadcast payload is 35
    # bytes so the frame is short. This is only a live hint for the operator;
    # the engine's parser is the authority.
    return len(data) >= 9 and data[0] == 0xAA and data[1] == 0x01


def main():
    ap = argparse.ArgumentParser(
        description="Capture raw Livox Mid-70 (SDK v1) UDP datagrams to a .livoxdump file.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--seconds", type=float, default=30, help="Capture duration in seconds")
    ap.add_argument("--out", default="mid70_capture.livoxdump", help="Output file")
    ap.add_argument("--host-ip", default="0.0.0.0",
                    help="Local IP to bind the DATA port on (0.0.0.0 = all interfaces)")
    ap.add_argument("--broadcast", action="store_true",
                    help="Also record SDK v1 broadcast frames on UDP 55000 (needs the port free)")
    ap.add_argument("--data-port", type=int, default=0,
                    help="Host UDP port the lidar streams points to (0 = do not capture points)")
    args = ap.parse_args()

    ports = []
    if args.broadcast:
        ports.append(BROADCAST_PORT)
    if args.data_port:
        ports.append(args.data_port)
    if not ports:
        print("error: nothing to capture -- pass --broadcast and/or --data-port N", file=sys.stderr)
        return 2

    print(f"Binding {len(ports)} UDP port(s): {ports}")
    try:
        socks = bind_sockets(args.host_ip, ports)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    port_by_sock = {s: i for i, s in enumerate(socks)}
    counters_pkts = [0] * len(ports)
    counters_bytes = [0] * len(ports)
    broadcast_codes = set()

    print("Bound OK. Waiting for datagrams...")
    if args.broadcast:
        print("  (a Mid-70 with nobody connected broadcasts about once a second)")
    if args.data_port:
        print("  (points arrive only after a driver on this host has done the handshake)")
    print()

    t_start = time.monotonic()
    warned_no_data = False
    NO_DATA_WARN_SECONDS = 4.0

    try:
        with open(args.out, "wb") as f:
            write_header(f, ports)
            last_print = 0.0
            while True:
                now = time.monotonic()
                elapsed = now - t_start
                if elapsed >= args.seconds:
                    break
                ready, _, _ = select.select(socks, [], [], min(0.2, max(0.0, args.seconds - elapsed)))
                for s in ready:
                    try:
                        data, addr = s.recvfrom(65535)
                    except OSError:
                        continue
                    t_ns = time.time_ns()
                    idx = port_by_sock[s]
                    f.write(RECORD_HDR.pack(t_ns, idx, len(data)))
                    f.write(data)
                    counters_pkts[idx] += 1
                    counters_bytes[idx] += len(data)
                    if ports[idx] == BROADCAST_PORT and looks_like_broadcast(data) and len(data) >= 27:
                        # broadcast_code is the first 16 bytes of the payload,
                        # which starts after the 11-byte command header.
                        code = data[11:27].split(b"\x00", 1)[0].decode("ascii", "replace")
                        if code and code not in broadcast_codes:
                            broadcast_codes.add(code)
                            print(f"  broadcast from {addr[0]}: code {code}")

                total = sum(counters_pkts)
                if not warned_no_data and total == 0 and elapsed > NO_DATA_WARN_SECONDS:
                    warned_no_data = True
                    print()
                    print(f"WARNING: nothing received in the first {NO_DATA_WARN_SECONDS:.0f} s.")
                    print("  Broadcasts: is the lidar powered and on this subnet, and is 55000 really free?")
                    print("  Points: has a driver on THIS host completed the SDK handshake, and is")
                    print("  --data-port the port it bound?")
                    print()

                if now - last_print > 0.5:
                    remaining = max(0.0, args.seconds - elapsed)
                    counts = " ".join(f"{ports[i]}:{counters_pkts[i]}" for i in range(len(ports)))
                    print(f"\r  {elapsed:6.1f}s  {counts}  ({remaining:.0f}s left)   ", end="", flush=True)
                    last_print = now
    except KeyboardInterrupt:
        print("\ninterrupted -- file is valid up to the last complete record")

    print()
    dur = max(1e-9, time.monotonic() - t_start)
    print("total_datagrams", sum(counters_pkts))
    for i, p in enumerate(ports):
        print(f"port {p}: {counters_pkts[i]} datagrams, {counters_bytes[i]} bytes, "
              f"{counters_pkts[i]/dur:.1f}/s")
    if broadcast_codes:
        print("broadcast_codes", ",".join(sorted(broadcast_codes)))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
