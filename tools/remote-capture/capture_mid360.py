#!/usr/bin/env python3
"""capture_mid360.py -- raw Livox Mid-360 UDP capture for the LidarScan remote-capture kit.

Binds the Mid-360 host-side UDP ports and records every datagram received on
each of them, verbatim, with an arrival timestamp, into a simple
length-prefixed container file (".livoxdump"). This closes spike S2's "raw
Mid-360 UDP captures" need -- no Livox SDK2 decoding happens here on purpose;
verify_capture.py validates the container framing and per-port rates back at
the dev machine, and full point-cloud decoding happens later with SDK2.

Requires: Python 3.7+. Standard library only (socket, struct, select) --
nothing to install.

Usage:
    python3 capture_mid360.py --seconds 60 --out bench_mid360_60s.livoxdump
    python3 capture_mid360.py --host-ip 192.168.1.50 --seconds 60 --out bench_mid360_60s.livoxdump
    python3 capture_mid360.py --ports 56100,56200,56300,56400,56500 --seconds 60 --out FILE

IMPORTANT -- two things must both be true for data to arrive:
    1. This script must be running and bound to the SAME ports the lidar is
       configured to send to (defaults below match the Mid-360's factory
       default host ports).
    2. The lidar must already be configured/started to stream to THIS
       machine's IP address (--host-ip, or its default detected below).
       That configuration step normally happens via the vendor's Livox
       Viewer 2 tool or the Livox SDK2 command-control channel -- this
       script does not do lidar configuration, it only listens.

Two workable modes at the remote site:
    A) Run Livox Viewer 2 first to get the lidar streaming to this host's
       IP, then QUIT Viewer 2 (it will have the ports open and this script
       will fail to bind with "address already in use" otherwise), then run
       this script -- the lidar keeps streaming to the same host/ports.
    B) If Viewer 2 must stay running, use --ports with an alternate set
       configured on the lidar itself (per the lidar's host-config, e.g. via
       Livox SDK2's config JSON) so this script listens on ports Viewer 2
       isn't using.

------------------------------------------------------------------------------
FILE FORMAT (".livoxdump") -- documented here as the source of truth:

  Header (fixed):
    8 bytes   magic       b"LX360CAP"
    u16 LE    version     currently 1
    u16 LE    num_ports   N  (number of UDP ports captured)
    N x u32 LE  port_table  the actual UDP port number bound for port_idx 0..N-1

  Then zero or more records, back to back, until EOF:
    u64 LE    t_ns        arrival time, nanoseconds, time.time_ns() epoch
    u16 LE    port_idx    index into the header's port_table (which port this
                           datagram arrived on)
    u32 LE    len         payload length in bytes
    len bytes payload     the raw UDP datagram, byte for byte

  A truncated last record (EOF mid-record) can happen if the process was
  killed mid-write; readers should stop at the first incomplete record
  rather than treating it as an error.
------------------------------------------------------------------------------
"""

import argparse
import select
import socket
import struct
import sys
import time

MAGIC = b"LX360CAP"
VERSION = 1

# Mid-360 factory-default host-side UDP ports (point cloud, IMU, and the
# control/log channels). Configurable per the lidar's own host-config if the
# vendor tool set it up differently.
DEFAULT_PORTS = [56100, 56200, 56300, 56400, 56500]

RECORD_HDR = struct.Struct("<QHI")  # t_ns, port_idx, len
FILE_HDR_FIXED = struct.Struct("<8sHH")  # magic, version, num_ports


def parse_ports(s):
    try:
        ports = [int(p.strip()) for p in s.split(",") if p.strip()]
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid --ports list: {s!r}")
    if not ports:
        raise argparse.ArgumentTypeError("--ports must list at least one port")
    return ports


def bind_sockets(host_ip, ports):
    socks = []
    for port in ports:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host_ip, port))
        except OSError as exc:
            for opened in socks:
                opened.close()
            raise OSError(
                f"could not bind {host_ip}:{port} -- {exc}\n"
                "This usually means something else already owns this port.\n"
                "  - If Livox Viewer 2 is running, quit it and re-run (mode A in the\n"
                "    module docstring / INSTRUCTIONS.md), or\n"
                "  - Configure the lidar to use alternate ports and pass --ports to match\n"
                "    (mode B)."
            ) from exc
        s.setblocking(False)
        socks.append(s)
    return socks


def write_header(f, ports):
    f.write(FILE_HDR_FIXED.pack(MAGIC, VERSION, len(ports)))
    for p in ports:
        f.write(struct.pack("<I", p))


def main():
    ap = argparse.ArgumentParser(
        description="Capture raw Livox Mid-360 UDP datagrams to a length-prefixed .livoxdump file.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--seconds", type=float, default=60, help="Capture duration in seconds")
    ap.add_argument("--out", default="bench_mid360_60s.livoxdump", help="Output file")
    ap.add_argument("--host-ip", default="0.0.0.0",
                     help="Local IP to bind (0.0.0.0 = all interfaces; use the Mid-360-facing "
                          "adapter's static IP, e.g. 192.168.1.50, if you have more than one NIC)")
    ap.add_argument("--ports", type=parse_ports, default=DEFAULT_PORTS,
                     help="Comma-separated list of UDP ports to bind, e.g. 56100,56200,56300,56400,56500")
    args = ap.parse_args()

    ports = args.ports
    print(f"Binding {len(ports)} UDP port(s) on {args.host_ip}: {ports}")
    try:
        socks = bind_sockets(args.host_ip, ports)
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    port_by_sock = {s: i for i, s in enumerate(socks)}
    counters_pkts = [0] * len(ports)
    counters_bytes = [0] * len(ports)

    print("Bound OK. Waiting for datagrams...")
    print("(If a Livox Viewer 2 / lidar config step hasn't pointed the lidar at this")
    print(f" host's IP yet, do that now -- see the module docstring / INSTRUCTIONS.md.)")
    print()

    t_start = time.monotonic()
    warned_no_data = False
    NO_DATA_WARN_SECONDS = 3.0

    try:
        with open(args.out, "wb") as f:
            write_header(f, ports)

            last_print = 0.0
            while True:
                now = time.monotonic()
                elapsed = now - t_start
                if elapsed >= args.seconds:
                    break

                remaining_timeout = min(0.2, max(0.0, args.seconds - elapsed))
                ready, _, _ = select.select(socks, [], [], remaining_timeout)
                for s in ready:
                    try:
                        data, _addr = s.recvfrom(65535)
                    except OSError:
                        continue
                    t_ns = time.time_ns()
                    idx = port_by_sock[s]
                    f.write(RECORD_HDR.pack(t_ns, idx, len(data)))
                    f.write(data)
                    counters_pkts[idx] += 1
                    counters_bytes[idx] += len(data)

                total_pkts = sum(counters_pkts)
                if not warned_no_data and total_pkts == 0 and elapsed > NO_DATA_WARN_SECONDS:
                    warned_no_data = True
                    print()
                    print(f"WARNING: no datagrams received in the first {NO_DATA_WARN_SECONDS:.0f} s.")
                    print("  Check: lidar powered + Ethernet link up, lidar configured to stream to")
                    print(f"  this host's IP ({args.host_ip if args.host_ip != '0.0.0.0' else 'this machine'}),")
                    print("  and --ports matches the lidar's configured host ports.")
                    print("  Still listening for the rest of the capture window...")
                    print()

                if now - last_print > 0.5:
                    remaining = max(0.0, args.seconds - elapsed)
                    counts_str = " ".join(
                        f"p{ports[i]}={counters_pkts[i]}" for i in range(len(ports))
                    )
                    sys.stdout.write(
                        f"\r  {total_pkts:>8,} pkts total | {counts_str} | "
                        f"{elapsed:5.1f}s elapsed | {remaining:5.1f}s remaining   "
                    )
                    sys.stdout.flush()
                    last_print = now

            sys.stdout.write("\n\n")

    except KeyboardInterrupt:
        sys.stdout.write("\n\nInterrupted by user -- stopping early.\n")
    finally:
        for s in socks:
            s.close()

    elapsed_total = time.monotonic() - t_start
    total_pkts = sum(counters_pkts)
    total_bytes = sum(counters_bytes)

    print("---- capture summary ----")
    print(f"File          : {args.out}")
    print(f"Duration      : {elapsed_total:.1f} s")
    print(f"Total packets : {total_pkts:,}")
    print(f"Total bytes   : {total_bytes:,} ({total_bytes / 1e6:.2f} MB)")
    print("Per-port:")
    for i, p in enumerate(ports):
        rate = counters_bytes[i] / elapsed_total / 1e6 if elapsed_total > 0 else 0.0
        print(f"  port {p:6d}: {counters_pkts[i]:>8,} pkts, {counters_bytes[i]:>10,} bytes, {rate:.2f} MB/s")

    if total_pkts == 0:
        verdict = "FAIL -- no datagrams received. Check lidar streaming config and --host-ip/--ports."
    elif any(c == 0 for c in counters_pkts):
        idle = [ports[i] for i, c in enumerate(counters_pkts) if c == 0]
        verdict = f"WARN -- some ports received nothing ({idle}). That may be expected (not every port carries data), but flag it."
    else:
        verdict = "PASS -- datagrams arrived on every configured port."
    print(f"Sanity verdict: {verdict}")
    print()
    print("Send the file back (see INSTRUCTIONS.md).")

    return 0 if total_pkts > 0 else 2


if __name__ == "__main__":
    sys.exit(main())
