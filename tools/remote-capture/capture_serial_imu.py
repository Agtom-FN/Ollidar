#!/usr/bin/env python3
"""
capture_serial_imu.py -- record raw bytes from a serial IMU module with arrival
timestamps, into a .imudump container the engine's tests can replay.

Written for the JuxiTech ICM-42670-P module (115200 8N1, frames 0x7E 0x23 ...),
but it records whatever the port sends; it never writes to the port. Each
read() slice is one record, stamped with time.time_ns() at the moment the
bytes were handed to us -- which is exactly the information the engine's
serial IMU driver has to work with, so a replay reproduces the real arrival
pattern (bursts included) rather than an idealised stream.

WHY 60+ SECONDS. This particular module stops transmitting for ~2.9 s every
~35 s (measured, firmware). A fixture that does not span at least one of
those blackouts cannot test the code that has to detect them.

USAGE
    python3 capture_serial_imu.py --port /dev/ttyUSB0 --seconds 60 --out juxi_imu_60s.imudump
    python3 capture_serial_imu.py --port /dev/cu.wchusbserial1420 --seconds 60

FILE FORMAT -- same record framing as the .livoxdump containers so one
reader serves all three, with its own magic:

  Header (fixed):
    8 bytes   magic       b"IMUSRCAP"
    u16 LE    version     currently 1
    u16 LE    num_ports   always 1
    1 x u32 LE  port_table  the baud rate used (there is no port number)

  Records, back to back until EOF:
    u64 LE    t_ns        arrival time, nanoseconds, time.time_ns() epoch
    u16 LE    port_idx    always 0
    u32 LE    len         bytes in this read slice
    len bytes payload     the bytes exactly as received
"""

import argparse
import struct
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    print("error: pyserial is required (pip3 install pyserial)", file=sys.stderr)
    sys.exit(2)

MAGIC = b"IMUSRCAP"
VERSION = 1
RECORD_HDR = struct.Struct("<QHI")
FILE_HDR_FIXED = struct.Struct("<8sHH")

FRAME_HEAD = b"\x7E\x23"


def main():
    ap = argparse.ArgumentParser(
        description="Record raw serial IMU bytes with arrival stamps to a .imudump file.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--port", required=True, help="Serial device, e.g. /dev/ttyUSB0 or /dev/cu.wchusbserial*")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--out", default="serial_imu_capture.imudump")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.02)
    except Exception as exc:
        print(f"error: could not open {args.port} at {args.baud}: {exc}", file=sys.stderr)
        return 1

    print(f"Opened {args.port} at {args.baud}. Recording for {args.seconds:.0f} s. Never writing to the port.")
    n_records = 0
    n_bytes = 0
    n_frames = 0
    last_data = None
    gaps = []  # (t_offset_s, gap_s) for gaps > 100 ms, for the operator's benefit
    t_start = time.monotonic()
    t0_ns = time.time_ns()
    last_print = 0.0
    try:
        with open(args.out, "wb") as f:
            f.write(FILE_HDR_FIXED.pack(MAGIC, VERSION, 1))
            f.write(struct.pack("<I", args.baud))
            while True:
                elapsed = time.monotonic() - t_start
                if elapsed >= args.seconds:
                    break
                data = ser.read(4096)  # returns on timeout (20 ms) or when data is there
                if not data:
                    continue
                t_ns = time.time_ns()
                f.write(RECORD_HDR.pack(t_ns, 0, len(data)))
                f.write(data)
                n_records += 1
                n_bytes += len(data)
                n_frames += data.count(FRAME_HEAD)
                if last_data is not None and (t_ns - last_data) > 100_000_000:
                    gaps.append(((last_data - t0_ns) / 1e9, (t_ns - last_data) / 1e9))
                last_data = t_ns
                if elapsed - last_print > 0.5:
                    print(f"\r  {elapsed:6.1f}s  {n_bytes} B  ~{n_frames} frames  gaps>100ms: {len(gaps)}   ",
                          end="", flush=True)
                    last_print = elapsed
    except KeyboardInterrupt:
        print("\ninterrupted -- file is valid up to the last complete record")
    finally:
        ser.close()

    print()
    dur = max(1e-9, time.monotonic() - t_start)
    print(f"records {n_records}  bytes {n_bytes}  ({n_bytes/dur:.0f} B/s)  frame headers ~{n_frames} ({n_frames/dur:.1f}/s)")
    if gaps:
        print(f"gaps > 100 ms: {len(gaps)}")
        for t, g in gaps[:10]:
            print(f"   at t={t:6.1f}s  gap {g:.3f}s")
        if len(gaps) >= 2:
            periods = [gaps[i + 1][0] - gaps[i][0] for i in range(len(gaps) - 1)]
            print(f"   gap-to-gap periods: {' '.join(f'{p:.1f}' for p in periods)} s")
    else:
        print("gaps > 100 ms: none (capture longer than 35 s to be sure)")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
