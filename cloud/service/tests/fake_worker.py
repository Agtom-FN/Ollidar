"""A stand-in for `engine_cli --post`, used by every non-integration test.

It speaks the same shape the real CLI does (INT-34 §6): progress on stderr as
`post: NN%  message`, product on stdout, exit 0/1/2/3. It also *reads* the
extracted input directory, so a test that asserts a result also proves the
upload survived the chunking byte-for-byte.

    python fake_worker.py [--mode ok|crash|usage|selfcancel|hang]
                          [--steps N] [--step-sleep S] <input> <output>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path


def emit(pct: int, message: str) -> None:
    print(f"post: {pct:3d}%  {message}", file=sys.stderr, flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="ok")
    ap.add_argument("--steps", type=int, default=4)
    ap.add_argument("--step-sleep", type=float, default=0.0)
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args()

    src, dst = Path(args.input), Path(args.output)
    dst.mkdir(parents=True, exist_ok=True)

    if args.mode == "usage":
        print("fake_worker: bad arguments", file=sys.stderr)
        return 2

    emit(0, "reading input")
    if not src.is_dir():
        print(f"post: FAILED: '{src}' is not a directory", file=sys.stderr)
        return 1

    files = sorted(p for p in src.rglob("*") if p.is_file())
    digest = hashlib.sha256()
    for path in files:
        digest.update(str(path.relative_to(src)).encode())
        digest.update(path.read_bytes())

    if args.mode == "hang":
        emit(10, "hanging on purpose")
        while True:
            time.sleep(0.05)

    for step in range(1, args.steps + 1):
        if args.step_sleep:
            time.sleep(args.step_sleep)
        pct = int(step * 100 / (args.steps + 1))
        emit(pct, f"stage-{step}")
        if args.mode == "crash" and step == 2:
            print("post: FAILED (kIoError): synthetic worker crash", file=sys.stderr)
            return 1
        if args.mode == "selfcancel" and step == 2:
            print("post: cancelled", file=sys.stderr)
            return 3

    summary = {
        "input": str(src),
        "manifest_present": (src / "manifest.json").is_file(),
        "file_count": len(files),
        "sha256": digest.hexdigest(),
        "pid": os.getpid(),
    }
    (dst / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True))
    (dst / "cloud.ply").write_bytes(b"ply\nformat binary_little_endian 1.0\nend_header\n")

    emit(100, "exporting")
    print(f"post: {src}")
    print("post: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
