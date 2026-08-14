#!/usr/bin/env python3
"""capture_gnss.py -- raw NMEA capture for the LidarScan remote-capture kit.

Logs raw NMEA-0183 bytes from an RTK rover's serial or Bluetooth-serial port
to a file, verbatim. This closes spike S5's "NMEA+RTCM logs captured for
tests" exit criterion. No parsing here on purpose -- verify_capture.py does
checksum validation and fix-quality histograms back at the dev machine.

Requires: Python 3.7+, pyserial (`pip install pyserial`). No other
dependencies.

Usage:
    python3 capture_gnss.py --list
    python3 capture_gnss.py --port /dev/tty.usbserial-1140 --seconds 120 --out bench_gnss_120s.nmea
    python3 capture_gnss.py --port COM6 --baud 9600 --seconds 120 --out bench_gnss_120s.nmea

Notes:
    - Bluetooth-SPP rover connections show up as a regular serial port once
      paired (a COM port on Windows, /dev/tty.* or /dev/rfcomm* elsewhere) --
      pair the rover first with your OS Bluetooth settings, then use --list.
    - Default baud is 9600 (common default for NMEA-only output on u-blox
      and Emlid receivers). If nothing is captured, try --baud 115200 or
      --baud 38400 -- some receivers/BT modules use a higher rate.
"""

import argparse
import platform
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.stderr.write(
        "error: pyserial is not installed.\n"
        "Fix: python3 -m pip install --user pyserial\n"
        "     (Windows: py -m pip install pyserial)\n"
    )
    sys.exit(1)

DEFAULT_BAUD = 9600
NO_DATA_WARN_SECONDS = 3.0


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        print("If using Bluetooth, pair the rover in OS Bluetooth settings first.")
        return
    print(f"Found {len(ports)} serial port(s):")
    for p in ports:
        print(f"  {p.device:20s} {p.description or '(no description)'}")
    print()
    print("Pick the rover's port with --port PORTNAME")


def friendly_open_error(exc, port):
    system = platform.system()
    msg = str(exc)
    lines = [f"Could not open port {port!r}: {msg}"]

    busy = any(s in msg for s in ("Access is denied", "Resource busy", "Device or resource busy", "in use"))
    if busy:
        lines.append("")
        lines.append("Another program may have the port open (e.g. u-center, a previous run")
        lines.append("of this script). Close it, unplug/re-pair, and try again.")

    if system == "Windows":
        lines.append("")
        lines.append("Windows: check Device Manager -> Ports (COM & LPT) for the rover's COM port.")
        lines.append("If it's a USB-serial rover and doesn't show up, it may need a driver from")
        lines.append("the receiver vendor (not CH340 -- that's D6-specific).")
    elif system == "Darwin":
        lines.append("")
        lines.append("macOS: for Bluetooth rovers, pair in System Settings -> Bluetooth first,")
        lines.append("then look for /dev/tty.* or /dev/cu.* with the rover's name in --list.")
    elif system == "Linux":
        lines.append("")
        lines.append("Linux fix (permission denied on /dev/ttyUSB* or /dev/rfcomm*):")
        lines.append("  - Quick one-off:  sudo python3 capture_gnss.py --port /dev/ttyUSB0 ...")
        lines.append("  - Permanent fix:  sudo usermod -aG dialout $USER   (then log out and back in)")

    return "\n".join(lines)


def format_rate(nbytes, seconds):
    if seconds <= 0:
        return "0.0 B/s"
    return f"{nbytes / seconds:.0f} B/s"


def run_capture(port, baud, seconds, out_path):
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.bytesize = serial.EIGHTBITS
        ser.parity = serial.PARITY_NONE
        ser.stopbits = serial.STOPBITS_ONE
        ser.timeout = 0.2
        ser.open()
    except (serial.SerialException, OSError) as exc:
        print(friendly_open_error(exc, port), file=sys.stderr)
        return 1

    print(f"Opened {port} @ {baud} baud, 8N1")

    warned_no_data = False
    total_bytes = 0
    sentence_lines = 0
    t_start = time.monotonic()
    t_first_byte = None

    try:
        with open(out_path, "wb") as f:
            print(f"Logging NMEA for {seconds} s...")
            print()

            last_print = 0.0
            while True:
                now = time.monotonic()
                elapsed = now - t_start
                if elapsed >= seconds:
                    break

                chunk = ser.read(4096)
                if chunk:
                    if total_bytes == 0:
                        t_first_byte = now
                    f.write(chunk)
                    total_bytes += len(chunk)
                    sentence_lines += chunk.count(b"\n")

                if not warned_no_data and total_bytes == 0 and elapsed > NO_DATA_WARN_SECONDS:
                    warned_no_data = True
                    sys.stdout.write("\n")
                    print(f"WARNING: no data received in the first {NO_DATA_WARN_SECONDS:.0f} s.")
                    print("  Check: rover powered on and has a Bluetooth/serial link established,")
                    print("  and that --port / --baud match the rover's output settings.")
                    print("  Still listening for the rest of the capture window...")
                    print()

                if now - last_print > 0.2:
                    remaining = max(0.0, seconds - elapsed)
                    rate = format_rate(total_bytes, elapsed) if elapsed > 0.5 else "-- B/s"
                    sys.stdout.write(
                        f"\r  {total_bytes:>9,} bytes | ~{sentence_lines:>5,} lines | {rate:>10s} | "
                        f"{elapsed:5.1f}s elapsed | {remaining:5.1f}s remaining   "
                    )
                    sys.stdout.flush()
                    last_print = now

            sys.stdout.write("\n\n")

    except KeyboardInterrupt:
        sys.stdout.write("\n\nInterrupted by user -- stopping early.\n")
    finally:
        try:
            ser.close()
        except Exception:
            pass

    elapsed_total = time.monotonic() - t_start
    rate_bps = total_bytes / elapsed_total if elapsed_total > 0 else 0.0

    print()
    print("---- capture summary ----")
    print(f"File          : {out_path}")
    print(f"Size          : {total_bytes:,} bytes ({total_bytes / 1000.0:.1f} KB)")
    print(f"Lines (approx): {sentence_lines:,}")
    print(f"Duration      : {elapsed_total:.1f} s")
    print(f"Average rate  : {rate_bps:.0f} B/s")
    if t_first_byte is not None:
        print(f"First byte at : {t_first_byte - t_start:.2f} s after port open")

    if total_bytes == 0:
        verdict = "FAIL -- no data received at all. Check rover power/link and --baud, then retry."
    elif sentence_lines == 0:
        verdict = "WARN -- bytes arrived but no line breaks seen; check that the rover outputs NMEA (not raw UBX)."
    else:
        verdict = "PASS -- NMEA-looking data captured. Send the file back for checksum verification."
    print(f"Sanity verdict: {verdict}")
    print()
    print("Success looks like: hundreds of lines of $GxGGA/$GxRMC/... text.")
    print("Send the file back (see INSTRUCTIONS.md).")

    return 0 if total_bytes > 0 else 2


def main():
    ap = argparse.ArgumentParser(
        description="Log raw NMEA bytes from a GNSS/RTK rover serial port.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--port", help="Serial port, e.g. /dev/tty.usbserial-1140 (macOS), "
                                    "/dev/ttyUSB0 or /dev/rfcomm0 (Linux), COM6 (Windows)")
    ap.add_argument("--list", action="store_true", help="List candidate serial ports and exit")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Serial baud rate")
    ap.add_argument("--seconds", type=float, default=120, help="Capture duration in seconds")
    ap.add_argument("--out", default="bench_gnss_120s.nmea", help="Output file for raw NMEA bytes")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return 0

    if not args.port:
        print("error: --port is required (use --list to see available ports)", file=sys.stderr)
        return 1

    return run_capture(args.port, args.baud, args.seconds, args.out)


if __name__ == "__main__":
    sys.exit(main())
