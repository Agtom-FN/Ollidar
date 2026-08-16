#!/usr/bin/env python3
"""make_synthetic.py -- fake sensor data for testing the field-test kit itself.

No hardware required. Produces:

  um982 FILE [--seconds N] [--fix Q] [--corrupt K] [--no-unicore]
        A UM982-style NMEA 0183 stream at 1 Hz: GGA, RMC, GSA, GST, VTG plus
        Unicore's proprietary #UNIHEADINGA log (8-hex CRC32 tail, NOT an NMEA
        XOR checksum) and a $GNHDT heading sentence.

  mid360 --port P [--seconds N] [--rate R]
        Blasts Mid-360-shaped UDP datagrams at 127.0.0.1:P so the kit's own
        receiver can be exercised end to end.

Used by smoke_windows.ps1 / smoke_macos.sh and by the verify_capture.py check.
"""

import argparse
import math
import random
import socket
import struct
import sys
import time


def nmea_checksum(body: str) -> str:
    ck = 0
    for ch in body:
        ck ^= ord(ch)
    return "%02X" % ck


def nmea(body: str) -> str:
    return "$" + body + "*" + nmea_checksum(body)


# Unicore/NovAtel ASCII-log CRC32: standard CRC-32 polynomial, init 0, no
# final inversion. Kept here (and mirrored in verify_capture.py) so a
# proprietary heading log is never mistaken for a corrupt sentence.
def _crc32_value(i: int) -> int:
    crc = i
    for _ in range(8):
        if crc & 1:
            crc = (crc >> 1) ^ 0xEDB88320
        else:
            crc >>= 1
    return crc


def unicore_crc32(data: bytes) -> int:
    crc = 0
    for b in data:
        crc = _crc32_value((crc ^ b) & 0xFF) ^ (crc >> 8)
    return crc & 0xFFFFFFFF


def unicore_log(body: str) -> str:
    return "#" + body + "*" + ("%08x" % unicore_crc32(body.encode("ascii")))


def gen_um982(path, seconds, fix_quality, corrupt_every, with_unicore):
    lat, lon = 2229.9042, 11410.5533        # Shenzhen-ish, ddmm.mmmm
    out = []
    n_line = 0
    for s in range(seconds):
        hh, mm, ss = 12, 34, s % 60
        t = "%02d%02d%05.2f" % (hh, mm, ss)
        sats = 0 if fix_quality == 0 else 9 + (s % 5)
        latf = "%.4f" % (lat + s * 0.00001) if fix_quality else ""
        lonf = "%.4f" % (lon + s * 0.00001) if fix_quality else ""
        ns = "N" if fix_quality else ""
        ew = "E" if fix_quality else ""
        hdop = "0.9" if fix_quality else ""
        alt = "31.4" if fix_quality else ""

        out.append(nmea("GNGGA,%s,%s,%s,%s,%s,%d,%02d,%s,%s,M,-2.3,M,,"
                        % (t, latf, ns, lonf, ew, fix_quality, sats, hdop, alt)))
        out.append(nmea("GNRMC,%s,%s,%s,%s,%s,%s,0.03,%.1f,150826,,,%s"
                        % (t, "A" if fix_quality else "V", latf, ns, lonf, ew,
                           (s * 3.0) % 360.0, "A" if fix_quality else "N")))
        out.append(nmea("GNGSA,A,%d,01,03,06,11,17,19,22,28,,,,,1.7,0.9,1.4,1"
                        % (3 if fix_quality else 1)))
        out.append(nmea("GNGST,%s,0.8,0.9,0.6,32.1,0.9,0.9,1.8" % t))
        out.append(nmea("GNVTG,%.1f,T,,M,0.03,N,0.05,K,%s"
                        % ((s * 3.0) % 360.0, "A" if fix_quality else "N")))
        if with_unicore:
            out.append(nmea("GNHDT,%.2f,T" % ((s * 3.0) % 360.0)))
            out.append(unicore_log(
                "UNIHEADINGA,COM1,0,45.5,FINE,2190,%d.000,0,0,18;"
                "SOL_COMPUTED,NARROW_INT,1.021,%.4f,-0.6242,0.0,0.3,0.2,"
                '"999",30,25,25,22,3,01,3,33'
                % (275030 + s, (s * 3.0) % 360.0)))

        if corrupt_every and (s % corrupt_every) == (corrupt_every - 1):
            # a genuinely corrupted sentence: right shape, wrong checksum
            out.append("$GNGGA,%s,BROKEN,,,,,,,,,,,,*00" % t)
        n_line = len(out)

    with open(path, "w", newline="\r\n") as f:
        for line in out:
            f.write(line + "\n")
    return n_line


def gen_mid360(port, seconds, rate, host="127.0.0.1"):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Mid-360 point packet: 36-byte header then 96 points x 14 bytes = 1380 B.
    payload = bytearray(1380)
    payload[0:2] = struct.pack("<H", 0)
    interval = 1.0 / rate
    t_end = time.monotonic() + seconds
    n = 0
    nxt = time.monotonic()
    while time.monotonic() < t_end:
        struct.pack_into("<H", payload, 28, n & 0xFFFF)   # udp_cnt-ish slot
        s.sendto(bytes(payload), (host, port))
        n += 1
        nxt += interval
        d = nxt - time.monotonic()
        if d > 0:
            time.sleep(d)
        elif d < -0.25:
            nxt = time.monotonic()
    s.close()
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("um982")
    a.add_argument("out")
    a.add_argument("--seconds", type=int, default=90)
    a.add_argument("--fix", type=int, default=1, help="GGA fix quality digit")
    a.add_argument("--corrupt", type=int, default=0,
                   help="emit one corrupt sentence every N seconds (0 = none)")
    a.add_argument("--no-unicore", action="store_true")

    b = sub.add_parser("mid360")
    b.add_argument("--port", type=int, required=True)
    b.add_argument("--seconds", type=float, default=5)
    b.add_argument("--rate", type=float, default=2000)

    args = ap.parse_args()
    if args.cmd == "um982":
        n = gen_um982(args.out, args.seconds, args.fix, args.corrupt, not args.no_unicore)
        print("wrote %d lines to %s" % (n, args.out))
    else:
        n = gen_mid360(args.port, args.seconds, args.rate)
        print("sent %d datagrams to 127.0.0.1:%d" % (n, args.port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
