#!/usr/bin/env python3
"""capture_d6.py -- raw COIN-D6 serial capture for the LidarScan remote-capture kit.

Records RAW BYTES ONLY from the D6's CH340 USB-serial link, verbatim, to a
file. No parsing happens here on purpose: the raw bytes are the gold data
that closes spike S1 (settles a checksum-variant ambiguity and measures
range noise for S6). Parsing/decoding happens back at the dev machine with
the S1 parser (see verify_capture.py).

Requires: Python 3.7+, pyserial (`pip install pyserial`). No other
dependencies.

Usage:
    python3 capture_d6.py --list
    python3 capture_d6.py --port /dev/tty.usbserial-1140 --seconds 30 --out bench_d6_30s.bin
    python3 capture_d6.py --port COM5 --seconds 30 --out bench_d6_30s.bin        (Windows)

Protocol facts baked in (see LidarScan Tech Spec §2.1):
    - UART 230400 baud, 8N1
    - DTR is cleared after opening (some CH340 boards wire DTR to a reset line)
    - Start command: AA 55 F0 0F   (sent once, right after opening)
    - Stop  command: AA 55 F5 0A   (sent on exit, whether normal or Ctrl-C)
    - Expected steady-state throughput: ~11.5 KB/s (device idles between
      packets even though the wire rate is 230400 baud = 23040 B/s). A
      healthy 30 s capture is therefore about 345 KB.
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

BAUD = 230400
CMD_START = bytes([0xAA, 0x55, 0xF0, 0x0F])
CMD_STOP = bytes([0xAA, 0x55, 0xF5, 0x0A])

# Expected steady-state byte rate band for a healthy D6 stream. Center is
# ~11.5 KB/s (4000 pts/s * ~13 bytes/pt averaged over packet framing); the
# band is wide because packet cadence is bursty, not because the estimate
# is loose.
EXPECTED_RATE_MIN_BPS = 8_000
EXPECTED_RATE_MAX_BPS = 16_000
EXPECTED_RATE_CENTER_BPS = 11_500

NO_DATA_WARN_SECONDS = 3.0


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        print("If the D6/CH340 adapter is plugged in, see INSTRUCTIONS.md troubleshooting.")
        return
    print(f"Found {len(ports)} serial port(s):")
    for p in ports:
        hint = ""
        vid_pid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None and p.pid is not None else None
        if vid_pid == "1A86:7523":
            hint = "  <-- looks like a CH340 (this is very likely the D6 adapter)"
        print(f"  {p.device:20s} {p.description or '(no description)'}{hint}")
    print()
    print("Pick the CH340 device with --port PORTNAME")


def friendly_open_error(exc, port):
    """Turn a pyserial open failure into OS-specific, actionable guidance."""
    system = platform.system()
    msg = str(exc)
    lines = [f"Could not open port {port!r}: {msg}"]

    busy = any(s in msg for s in ("Access is denied", "Resource busy", "Device or resource busy", "in use"))
    if busy:
        lines.append("")
        lines.append("This usually means another program has the port open.")
        lines.append("  - Close any vendor tool, Arduino IDE serial monitor, or a previous")
        lines.append("    run of this script that is still running.")
        lines.append("  - Unplug and replug the USB adapter, then try again.")

    if system == "Windows":
        lines.append("")
        lines.append("Windows fix: install the CH340 driver.")
        lines.append(r"  1. Find CH340_WINDOWS.zip (vendor kit: 'english coind6\4 Windows Host")
        lines.append(r"     Software\CH340 Driver\CH340_WINDOWS.zip').")
        lines.append("  2. Unzip it and run the installer inside (e.g. SETUP.EXE) as Administrator.")
        lines.append("  3. Reboot if prompted, then re-plug the adapter.")
        lines.append("  4. Check Device Manager -> Ports (COM & LPT) for 'USB-SERIAL CH340 (COMx)'.")
        lines.append("     Use that COMx as --port, e.g. --port COM5")
    elif system == "Darwin":
        lines.append("")
        lines.append("macOS: no driver install should be needed (CH340 support is built in on")
        lines.append("macOS 13+). If the port still won't open:")
        lines.append("  - Unplug/replug the adapter and run --list again.")
        lines.append("  - Try the /dev/cu.usbserial-* device, not /dev/tty.usbserial-*.")
    elif system == "Linux":
        lines.append("")
        lines.append("Linux fix (permission denied on /dev/ttyUSB*):")
        lines.append("  - Quick one-off:  sudo python3 capture_d6.py --port /dev/ttyUSB0 ...")
        lines.append("  - Permanent fix:  sudo usermod -aG dialout $USER   (then log out and back in)")

    return "\n".join(lines)


def format_rate(nbytes, seconds):
    if seconds <= 0:
        return "0.0 KB/s"
    return f"{nbytes / seconds / 1000.0:.1f} KB/s"


def run_capture(port, seconds, out_path):
    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD
        ser.bytesize = serial.EIGHTBITS
        ser.parity = serial.PARITY_NONE
        ser.stopbits = serial.STOPBITS_ONE
        ser.timeout = 0.2
        ser.open()
    except serial.SerialException as exc:
        print(friendly_open_error(exc, port), file=sys.stderr)
        return 1
    except OSError as exc:
        print(friendly_open_error(exc, port), file=sys.stderr)
        return 1

    # The vendor SDK clears DTR after opening -- some CH340 boards wire DTR
    # to a reset line, so leaving it set can hold the D6 in reset.
    try:
        ser.dtr = False
    except Exception:
        pass

    print(f"Opened {port} @ {BAUD} 8N1")

    warned_no_data = False
    total_bytes = 0
    t_start = time.monotonic()
    t_first_byte = None

    try:
        with open(out_path, "wb") as f:
            ser.write(CMD_START)
            print("Sent start command: AA 55 F0 0F")
            print(f"Capturing for {seconds} s -- do not unplug the adapter...")
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

                if not warned_no_data and total_bytes == 0 and elapsed > NO_DATA_WARN_SECONDS:
                    warned_no_data = True
                    sys.stdout.write("\n")
                    print(f"WARNING: no data received in the first {NO_DATA_WARN_SECONDS:.0f} s.")
                    print("  Check: D6 power (spinning + LED?), CH340 wiring (DTX->RXD, DRX->TXD,")
                    print("  swap and retry if unsure), and that --port is the right device.")
                    print("  Still listening for the rest of the capture window...")
                    print()

                if now - last_print > 0.2:
                    remaining = max(0.0, seconds - elapsed)
                    rate = format_rate(total_bytes, elapsed) if elapsed > 0.5 else "-- KB/s"
                    sys.stdout.write(
                        f"\r  {total_bytes:>9,} bytes captured | {rate:>10s} | "
                        f"{elapsed:5.1f}s elapsed | {remaining:5.1f}s remaining   "
                    )
                    sys.stdout.flush()
                    last_print = now

            sys.stdout.write("\n\n")

    except KeyboardInterrupt:
        sys.stdout.write("\n\nInterrupted by user -- stopping early.\n")
    finally:
        try:
            ser.write(CMD_STOP)
            print("Sent stop command: AA 55 F5 0A")
        except Exception:
            pass
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
    print(f"Duration      : {elapsed_total:.1f} s")
    print(f"Average rate  : {rate_bps / 1000.0:.2f} KB/s")
    if t_first_byte is not None:
        print(f"First byte at : {t_first_byte - t_start:.2f} s after start command")

    if total_bytes == 0:
        verdict = "FAIL -- no data received at all. Check wiring/power and retry."
    elif EXPECTED_RATE_MIN_BPS <= rate_bps <= EXPECTED_RATE_MAX_BPS:
        verdict = f"PASS -- rate is in the expected range (~{EXPECTED_RATE_CENTER_BPS / 1000.0:.1f} KB/s for a healthy D6)."
    else:
        verdict = (
            f"WARN -- rate is outside the expected {EXPECTED_RATE_MIN_BPS / 1000.0:.0f}"
            f"-{EXPECTED_RATE_MAX_BPS / 1000.0:.0f} KB/s range. The file may still be usable -- "
            "send it back and we'll check it with verify_capture.py."
        )
    print(f"Sanity verdict: {verdict}")
    print()
    print("Success looks like: a file a few hundred KB in size for a 30 s run,")
    print("with a PASS verdict above. Send the file back (see INSTRUCTIONS.md).")

    return 0 if total_bytes > 0 else 2


def main():
    ap = argparse.ArgumentParser(
        description="Capture raw COIN-D6 serial data to a file (no parsing -- raw bytes only).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--port", help="Serial port, e.g. /dev/tty.usbserial-1140 (macOS), "
                                    "/dev/ttyUSB0 (Linux), COM5 (Windows)")
    ap.add_argument("--list", action="store_true", help="List candidate serial ports and exit")
    ap.add_argument("--seconds", type=float, default=30, help="Capture duration in seconds")
    ap.add_argument("--out", default="bench_d6_30s.bin", help="Output file for raw captured bytes")
    args = ap.parse_args()

    if args.list:
        list_ports()
        return 0

    if not args.port:
        print("error: --port is required (use --list to see available ports)", file=sys.stderr)
        return 1

    return run_capture(args.port, args.seconds, args.out)


if __name__ == "__main__":
    sys.exit(main())
