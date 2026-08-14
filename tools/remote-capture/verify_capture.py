#!/usr/bin/env python3
"""verify_capture.py -- validate capture files returned from the remote site.

Runs at the DEV MACHINE end (not the remote site) on files produced by
capture_d6.py, capture_mid360.py, or capture_gnss.py. Prints a clear
PASS/WARN/FAIL verdict per check plus an overall verdict.

Python 3.7+, standard library only, EXCEPT: the D6 check will also try to
shell out to the S1 spike's `d6cli` binary if it has been built (an
independent, more authoritative decode using the real parser); if that
binary doesn't exist, the D6 check still runs a self-contained lightweight
Python check instead of failing.

Usage:
    python3 verify_capture.py bench_d6_30s.bin
    python3 verify_capture.py bench_mid360_60s.livoxdump
    python3 verify_capture.py bench_gnss_120s.nmea
    python3 verify_capture.py FILE --type d6|mid360|nmea      (override auto-detect)
    python3 verify_capture.py bench_d6_30s.bin --seconds 30   (for an accurate rate check)
    python3 verify_capture.py bench_d6_30s.bin --d6cli /path/to/d6cli
"""

import argparse
import os
import re
import struct
import subprocess
import sys

DEFAULT_D6CLI = "/Users/admin/lidarscan/spikes/s1-d6-parser/build/d6cli"

# D6 rate expectations, mirrored from capture_d6.py.
D6_EXPECTED_RATE_MIN_BPS = 8_000
D6_EXPECTED_RATE_MAX_BPS = 16_000

MID360_MAGIC = b"LX360CAP"


class Result:
    """One PASS/WARN/FAIL line."""

    def __init__(self, status, text):
        assert status in ("PASS", "WARN", "FAIL")
        self.status = status
        self.text = text

    def __str__(self):
        return f"[{self.status}] {self.text}"


def detect_type(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".bin":
        return "d6"
    if ext == ".livoxdump":
        return "mid360"
    if ext in (".nmea", ".txt", ".log"):
        return "nmea"
    # Content sniff fallback.
    with open(path, "rb") as f:
        head = f.read(16)
    if head[:8] == MID360_MAGIC:
        return "mid360"
    if head[:1] in (b"$", b"!"):
        return "nmea"
    return "d6"


def guess_seconds_from_name(path):
    m = re.search(r"(\d+)s\b", os.path.basename(path))
    return float(m.group(1)) if m else None


# ---------------------------------------------------------------------------
# D6
# ---------------------------------------------------------------------------

def verify_d6(path, seconds, d6cli_path):
    results = []
    size = os.path.getsize(path)
    results.append(Result("PASS" if size > 0 else "FAIL", f"file size: {size:,} bytes"))
    if size == 0:
        return results

    # --- Independent lightweight Python check -----------------------------
    with open(path, "rb") as f:
        data = f.read()

    # Wire order per spec/REPORT: header bytes on the wire are AA 55 (the
    # spec's PH=0x55AA read little-endian). Count candidate header sites.
    header_count = data.count(b"\xAA\x55")
    density = header_count / len(data) if data else 0.0
    # A healthy D6 packet is ~10 + 3*LSN bytes; with LSN commonly ~40 that's
    # ~130 bytes/packet, so one header roughly every 100-140 bytes is
    # plausible. This is a coarse sanity check only -- the real check is the
    # d6cli replay below.
    if header_count == 0:
        results.append(Result("FAIL", "0x55AA header density: no header bytes found at all"))
    elif density < 1 / 400:
        results.append(Result("WARN", f"0x55AA header density low: {header_count} headers in {len(data)} bytes "
                                        f"(1 per {len(data)/header_count:.0f} bytes) -- expected roughly 1 per ~100-140 bytes"))
    else:
        results.append(Result("PASS", f"0x55AA header density: {header_count} candidate headers "
                                        f"(1 per {len(data)/header_count:.0f} bytes)"))

    secs = seconds or guess_seconds_from_name(path)
    if secs:
        rate = len(data) / secs
        if D6_EXPECTED_RATE_MIN_BPS <= rate <= D6_EXPECTED_RATE_MAX_BPS:
            results.append(Result("PASS", f"byte rate: {rate/1000:.2f} KB/s over {secs:.0f}s (expected "
                                            f"{D6_EXPECTED_RATE_MIN_BPS/1000:.0f}-{D6_EXPECTED_RATE_MAX_BPS/1000:.0f} KB/s)"))
        else:
            results.append(Result("WARN", f"byte rate: {rate/1000:.2f} KB/s over {secs:.0f}s is outside the "
                                            f"expected {D6_EXPECTED_RATE_MIN_BPS/1000:.0f}-{D6_EXPECTED_RATE_MAX_BPS/1000:.0f} KB/s range"))
    else:
        results.append(Result("WARN", "byte rate: unknown capture duration (pass --seconds, or name the file "
                                        "like bench_d6_30s.bin) -- skipped rate check"))

    # --- Authoritative check via the S1 parser, if built -------------------
    if os.path.isfile(d6cli_path) and os.access(d6cli_path, os.X_OK):
        cmd = [d6cli_path, "--replay", path, "--plot", "none", "--quiet"]
        if secs:
            cmd += ["--replay-duration", str(secs)]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            out = proc.stdout + proc.stderr
            results.append(_parse_d6cli_output(out))
        except Exception as exc:
            results.append(Result("WARN", f"d6cli replay failed to run: {exc}"))
    else:
        results.append(Result("WARN", f"d6cli not found/executable at {d6cli_path} -- skipping the authoritative "
                                        "S1-parser replay check (build it: cmake -S spikes/s1-d6-parser -B "
                                        "spikes/s1-d6-parser/build && cmake --build spikes/s1-d6-parser/build)"))

    return results


def _parse_d6cli_output(out):
    def find_float(pattern):
        m = re.search(pattern, out)
        return float(m.group(1)) if m else None

    def find_int(pattern):
        m = re.search(pattern, out)
        return int(m.group(1)) if m else None

    pass_rate = find_float(r"checksum pass rate\s*:\s*([\d.]+)\s*%")
    pkts_ok = find_int(r"packets ok\s*:\s*(\d+)")
    accepted_vendor = find_int(r"accepted by vendor\s*:\s*(\d+)")
    accepted_spec = find_int(r"accepted by spec\s*:\s*(\d+)")
    starts = find_int(r"start packets\s*:\s*(\d+)")
    rotations = find_int(r"rotations\s*:\s*(\d+)")

    if pass_rate is None:
        return Result("WARN", "d6cli replay produced no parseable summary (check the binary / file are compatible)")

    detail = f"checksum pass rate={pass_rate:.4f}%"
    if accepted_vendor is not None and accepted_spec is not None:
        winner = "vendor" if accepted_vendor >= accepted_spec else "spec"
        detail += f", vendor-variant={accepted_vendor}, spec-variant={accepted_spec} (S1 checksum question: {winner})"
    if starts is not None:
        detail += f", start-packets={starts}"
    if rotations is not None:
        detail += f", rotations={rotations}"
    if pkts_ok is not None:
        detail += f", packets_ok={pkts_ok}"

    status = "PASS" if pass_rate > 99.5 else "FAIL"
    return Result(status, f"d6cli replay (S1 parser): {detail} -- S1 exit criterion is >99.5%")


# ---------------------------------------------------------------------------
# Mid-360
# ---------------------------------------------------------------------------

def verify_mid360(path):
    results = []
    size = os.path.getsize(path)
    results.append(Result("PASS" if size > 0 else "FAIL", f"file size: {size:,} bytes"))
    if size == 0:
        return results

    with open(path, "rb") as f:
        fixed_hdr = f.read(struct.calcsize("<8sHH"))
        if len(fixed_hdr) < struct.calcsize("<8sHH"):
            results.append(Result("FAIL", "truncated before container header"))
            return results
        magic, version, num_ports = struct.unpack("<8sHH", fixed_hdr)
        if magic != MID360_MAGIC:
            results.append(Result("FAIL", f"bad magic: {magic!r} (expected {MID360_MAGIC!r})"))
            return results
        results.append(Result("PASS", f"container header OK: version={version}, num_ports={num_ports}"))

        port_table = []
        for _ in range(num_ports):
            raw = f.read(4)
            if len(raw) < 4:
                results.append(Result("FAIL", "truncated port table"))
                return results
            port_table.append(struct.unpack("<I", raw)[0])
        results.append(Result("PASS", f"port table: {port_table}"))

        pkt_counts = [0] * num_ports
        byte_counts = [0] * num_ports
        record_hdr = struct.Struct("<QHI")
        record_hdr_size = record_hdr.size
        t_min = None
        t_max = None
        truncated = False
        n_records = 0

        while True:
            hdr = f.read(record_hdr_size)
            if len(hdr) == 0:
                break
            if len(hdr) < record_hdr_size:
                truncated = True
                break
            t_ns, port_idx, length = record_hdr.unpack(hdr)
            payload = f.read(length)
            if len(payload) < length:
                truncated = True
                break
            n_records += 1
            if port_idx >= num_ports:
                results.append(Result("WARN", f"record {n_records}: port_idx {port_idx} out of range "
                                                f"(num_ports={num_ports}) -- container may be corrupt"))
                continue
            pkt_counts[port_idx] += 1
            byte_counts[port_idx] += length
            t_min = t_ns if t_min is None else min(t_min, t_ns)
            t_max = t_ns if t_max is None else max(t_max, t_ns)

        if truncated:
            results.append(Result("WARN", f"file ends with a truncated record after {n_records} good records "
                                            "(expected if the capture was killed mid-write)"))
        else:
            results.append(Result("PASS", f"{n_records} records parsed cleanly to EOF"))

        if n_records == 0:
            results.append(Result("FAIL", "no datagrams captured"))
            return results

        duration_s = (t_max - t_min) / 1e9 if t_min is not None and t_max is not None and t_max > t_min else None
        for i, p in enumerate(port_table):
            if pkt_counts[i] == 0:
                results.append(Result("WARN", f"port {p}: 0 packets"))
            else:
                rate_txt = ""
                if duration_s:
                    rate_txt = f", {byte_counts[i]/duration_s/1e6:.2f} MB/s"
                results.append(Result("PASS", f"port {p}: {pkt_counts[i]:,} pkts, {byte_counts[i]:,} bytes{rate_txt}"))

    return results


# ---------------------------------------------------------------------------
# NMEA / GNSS
# ---------------------------------------------------------------------------

def nmea_checksum_ok(sentence):
    """sentence like '$GPGGA,...*4B' (no CR/LF). Returns True/False, or None if unchecksummable."""
    if not sentence or sentence[0] not in "$!":
        return None
    star = sentence.rfind("*")
    if star == -1 or star + 3 > len(sentence):
        return None
    body = sentence[1:star]
    claimed = sentence[star + 1:star + 3]
    try:
        claimed_val = int(claimed, 16)
    except ValueError:
        return None
    computed = 0
    for ch in body:
        computed ^= ord(ch)
    return computed == claimed_val


def verify_nmea(path):
    results = []
    size = os.path.getsize(path)
    results.append(Result("PASS" if size > 0 else "FAIL", f"file size: {size:,} bytes"))
    if size == 0:
        return results

    with open(path, "rb") as f:
        raw = f.read()
    text = raw.decode("ascii", errors="replace")
    lines = [ln.strip("\r\n") for ln in text.splitlines() if ln.strip()]

    n_total = 0
    n_checksum_ok = 0
    n_checksum_bad = 0
    n_no_checksum = 0
    fix_hist = {}
    talkers = {}

    for ln in lines:
        if not ln.startswith(("$", "!")):
            continue
        n_total += 1
        ok = nmea_checksum_ok(ln)
        if ok is None:
            n_no_checksum += 1
        elif ok:
            n_checksum_ok += 1
        else:
            n_checksum_bad += 1

        star = ln.find("*")
        body = ln[:star] if star != -1 else ln
        fields = body.split(",")
        sentence_id = fields[0][1:] if fields and len(fields[0]) > 1 else ""
        talkers[sentence_id] = talkers.get(sentence_id, 0) + 1

        if sentence_id.endswith("GGA") and len(fields) > 6:
            fix_q = fields[6]
            fix_hist[fix_q] = fix_hist.get(fix_q, 0) + 1

    results.append(Result("PASS" if n_total > 0 else "FAIL", f"NMEA-looking lines: {n_total}"))
    if n_total == 0:
        return results

    if n_checksum_bad == 0 and n_checksum_ok > 0:
        results.append(Result("PASS", f"checksums: {n_checksum_ok} valid, 0 invalid ({n_no_checksum} lines had no checksum field)"))
    elif n_checksum_ok > 0:
        bad_frac = n_checksum_bad / (n_checksum_ok + n_checksum_bad)
        status = "WARN" if bad_frac < 0.01 else "FAIL"
        results.append(Result(status, f"checksums: {n_checksum_ok} valid, {n_checksum_bad} INVALID "
                                        f"({bad_frac*100:.2f}% bad)"))
    else:
        results.append(Result("WARN", "checksums: no checksummable sentences found"))

    top_talkers = sorted(talkers.items(), key=lambda kv: -kv[1])[:8]
    results.append(Result("PASS", "sentence types: " + ", ".join(f"{k}={v}" for k, v in top_talkers)))

    fix_names = {"0": "no fix", "1": "GPS (single)", "2": "DGPS", "4": "RTK Fixed", "5": "RTK Float"}
    if fix_hist:
        hist_txt = ", ".join(f"{fix_names.get(k, k)}={v}" for k, v in sorted(fix_hist.items()))
        has_fix = any(k != "0" and int(k) > 0 for k in fix_hist if k.isdigit())
        status = "PASS" if has_fix else "WARN"
        results.append(Result(status, f"GGA fix-quality histogram: {hist_txt}"))
    else:
        results.append(Result("WARN", "no $GxGGA sentences found -- can't build a fix-quality histogram"))

    return results


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def overall_status(results):
    if any(r.status == "FAIL" for r in results):
        return "FAIL"
    if any(r.status == "WARN" for r in results):
        return "WARN"
    return "PASS"


def main():
    ap = argparse.ArgumentParser(description="Verify a capture file returned from the remote-capture kit.")
    ap.add_argument("file", help="Path to the returned capture file")
    ap.add_argument("--type", choices=["d6", "mid360", "nmea"], help="Override auto-detected file type")
    ap.add_argument("--seconds", type=float, help="Known capture duration (for D6 rate check); "
                                                     "inferred from a filename like bench_d6_30s.bin otherwise")
    ap.add_argument("--d6cli", default=DEFAULT_D6CLI, help="Path to the S1 spike's d6cli binary")
    args = ap.parse_args()

    if not os.path.isfile(args.file):
        print(f"error: no such file: {args.file}", file=sys.stderr)
        return 1

    ftype = args.type or detect_type(args.file)
    print(f"File: {args.file}")
    print(f"Type: {ftype}")
    print()

    if ftype == "d6":
        results = verify_d6(args.file, args.seconds, args.d6cli)
    elif ftype == "mid360":
        results = verify_mid360(args.file)
    elif ftype == "nmea":
        results = verify_nmea(args.file)
    else:
        print(f"error: unknown type {ftype}", file=sys.stderr)
        return 1

    for r in results:
        print(str(r))

    overall = overall_status(results)
    print()
    print(f"OVERALL: {overall}")
    return {"PASS": 0, "WARN": 0, "FAIL": 1}[overall]


if __name__ == "__main__":
    sys.exit(main())
