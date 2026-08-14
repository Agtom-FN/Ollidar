"""rtcm_tool.py -- RTCM3 framing helpers for spike S5 (RTK simulation infra).

Implements the transport-level RTCM 3.x message framing defined in RTCM
10403.x Annex A / D:

    byte0        0xD3 preamble
    byte1..2     6 reserved bits (must be 0) + 10-bit payload length
    byte3..N     payload (message-number DF002 is the first 12 bits)
    last 3 bytes CRC-24Q over [preamble + length + payload]

CRC-24Q is the "Qualcomm" 24-bit CRC specified by RTCM SC-104: generator
polynomial x^24+x^23+x^18+x^17+x^14+x^11+x^10+x^7+x^6+x^5+x^4+x^3+x+1
(== 0x1864CFB with the implicit leading 1 folded in), MSB-first, initial
value 0, no reflection, no final XOR.

This module does NOT decode RTCM message payloads (no MSM/ephemeris
parsing) -- only frames/validates/generates them. That is enough to give
A10 (NTRIP client + NMEA parsing + georef fusion) something transport-real
to parse without needing a real correction source. See REPORT.md for what
is and isn't faithfully simulated.
"""

from __future__ import annotations

import argparse
import random
import sys
from dataclasses import dataclass
from typing import Iterable, Iterator, List, Optional, Sequence

RTCM_PREAMBLE = 0xD3
RTCM_POLY = 0x1864CFB  # CRC-24Q generator polynomial (see module docstring)
MAX_PAYLOAD_LEN = 1023  # 10-bit length field


def crc24q(data: bytes, crc: int = 0) -> int:
    """Compute CRC-24Q (RTCM SC-104 variant) over `data`.

    MSB-first, initial value 0 (RTCM3 does not seed or invert). Returns a
    24-bit integer.
    """
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= RTCM_POLY
        crc &= 0xFFFFFF
    return crc & 0xFFFFFF


def build_frame(payload: bytes) -> bytes:
    """Wrap `payload` bytes in a standards-correct RTCM3 frame."""
    length = len(payload)
    if not (0 <= length <= MAX_PAYLOAD_LEN):
        raise ValueError(f"payload length {length} out of range 0..{MAX_PAYLOAD_LEN}")
    header = bytes([RTCM_PREAMBLE, (length >> 8) & 0x03, length & 0xFF])
    body = header + payload
    crc = crc24q(body)
    return body + crc.to_bytes(3, "big")


@dataclass
class FrameResult:
    offset: int          # byte offset of the 0xD3 preamble within the scanned buffer
    length: int          # payload length in bytes (from the header)
    msg_type: Optional[int]  # RTCM message number (DF002), if payload long enough
    crc_ok: bool
    total_len: int        # 3 (header) + length (payload) + 3 (crc)


def iter_frames(data: bytes) -> Iterator[FrameResult]:
    """Scan `data` for RTCM3 frames, in order.

    Resyncs on stray bytes (anything that isn't a valid, in-bounds 0xD3
    frame is skipped one byte at a time) so it behaves like a real
    stream-framing parser, not just "split on 0xD3". Stops when it runs
    out of bytes for a complete frame (a truncated trailing frame is
    silently dropped, matching how a live serial/TCP reader would just
    wait for more bytes).
    """
    i = 0
    n = len(data)
    while i < n:
        if data[i] != RTCM_PREAMBLE:
            i += 1
            continue
        if i + 3 > n:
            break
        length = ((data[i + 1] & 0x03) << 8) | data[i + 2]
        total_len = 3 + length + 3
        if i + total_len > n:
            break
        body = data[i : i + 3 + length]
        crc_recv = data[i + 3 + length : i + total_len]
        crc_calc = crc24q(body).to_bytes(3, "big")
        msg_type = None
        if length >= 2:
            msg_type = (data[i + 3] << 4) | (data[i + 4] >> 4)
        yield FrameResult(
            offset=i,
            length=length,
            msg_type=msg_type,
            crc_ok=(crc_recv == crc_calc),
            total_len=total_len,
        )
        i += total_len


def make_payload(msg_type: int, extra_len: int, rng: random.Random) -> bytes:
    """Build a payload whose first 12 bits are `msg_type` (DF002), the way
    a real RTCM3 message would start, with deterministic-but-arbitrary
    filler bytes after it standing in for the actual message body.

    This is transport-valid, NOT semantically valid: nothing after the
    12-bit message number is a real MSM/ephemeris/station-coordinate
    encoding. See REPORT.md.
    """
    if not (0 <= msg_type <= 0xFFF):
        raise ValueError("RTCM message numbers are 12-bit (0..4095)")
    buf = bytearray(2 + extra_len)
    buf[0] = (msg_type >> 4) & 0xFF
    buf[1] = ((msg_type & 0xF) << 4) | rng.randint(0, 15)
    if extra_len:
        buf[2:] = bytes(rng.getrandbits(8) for _ in range(extra_len))
    return bytes(buf)


# Roughly realistic payload sizes (bytes) for a handful of common message
# types, just so a canned stream "looks" plausible in a hex dump / length
# histogram. Not derived from real encodes.
_TYPICAL_PAYLOAD_LEN = {
    1005: 17,   # stationary RTK reference station ARP
    1006: 19,   # + antenna height
    1077: 210,  # GPS MSM7
    1087: 210,  # GLONASS MSM7
    1097: 210,  # Galileo MSM7
    1127: 210,  # BeiDou MSM7
    1230: 8,    # GLONASS code-phase biases
}
DEFAULT_MESSAGE_TYPES = [1005, 1077, 1087, 1097, 1127, 1230]


def generate_canned_stream(
    out_path: str,
    types: Optional[Sequence[int]] = None,
    count: int = 100,
    seed: Optional[int] = None,
) -> int:
    """Write `count` valid, concatenated RTCM3 frames to `out_path`, cycling
    through `types`. Returns the number of frames written.

    Intended as fixture generation for ntrip_caster_sim.py: the caster
    replays this file's frames to clients so the transport (framing,
    CRC, message-type cadence) is exercised without needing a real base
    station or correction feed.
    """
    types = list(types) if types else DEFAULT_MESSAGE_TYPES
    rng = random.Random(seed)
    with open(out_path, "wb") as f:
        for i in range(count):
            msg_type = types[i % len(types)]
            extra_len = _TYPICAL_PAYLOAD_LEN.get(msg_type, 40)
            # Small jitter so frame sizes aren't perfectly periodic.
            extra_len = max(0, extra_len + rng.randint(-3, 3))
            payload = make_payload(msg_type, extra_len, rng)
            f.write(build_frame(payload))
    return count


def _cmd_validate(args: argparse.Namespace) -> int:
    if args.path == "-":
        data = sys.stdin.buffer.read()
    else:
        with open(args.path, "rb") as f:
            data = f.read()
    results = list(iter_frames(data))
    bad = [r for r in results if not r.crc_ok]
    covered = sum(r.total_len for r in results)
    print(f"frames: {len(results)}  bytes scanned: {len(data)}  bytes framed: {covered}")
    if results:
        types = sorted({r.msg_type for r in results if r.msg_type is not None})
        print(f"message types seen: {types}")
    if bad:
        print(f"CRC FAILURES: {len(bad)} at offsets {[r.offset for r in bad][:10]}")
        return 1
    if not results:
        print("no RTCM3 frames found")
        return 1
    trailing = len(data) - (results[-1].offset + results[-1].total_len)
    if trailing:
        print(f"note: {trailing} unparsed trailing byte(s) (truncated frame or noise)")
    print("OK: all frames CRC-valid")
    return 0


def _cmd_generate(args: argparse.Namespace) -> int:
    types = [int(t) for t in args.types.split(",")] if args.types else None
    n = generate_canned_stream(args.out, types=types, count=args.count, seed=args.seed)
    print(f"wrote {n} frames to {args.out}")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    pv = sub.add_parser("validate", help="validate RTCM3 framing/CRC of a file (or stdin with -)")
    pv.add_argument("path", help="path to a binary RTCM3 stream, or - for stdin")
    pv.set_defaults(func=_cmd_validate)

    pg = sub.add_parser("generate", help="generate a canned valid RTCM3 stream file")
    pg.add_argument("out", help="output path")
    pg.add_argument("--count", type=int, default=100, help="number of frames to write")
    pg.add_argument("--types", default=None, help="comma-separated RTCM message numbers to cycle through")
    pg.add_argument("--seed", type=int, default=None)
    pg.set_defaults(func=_cmd_generate)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
