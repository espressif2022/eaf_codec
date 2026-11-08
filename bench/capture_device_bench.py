#!/usr/bin/env python3
"""Reset a serial-connected target and capture one complete EAF benchmark run."""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()

    chunks: list[bytes] = []
    deadline = time.monotonic() + args.timeout
    complete = False
    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        while time.monotonic() < deadline:
            data = port.read(port.in_waiting or 1)
            if not data:
                continue
            chunks.append(data)
            text = data.decode("utf-8", errors="replace")
            print(text, end="", flush=True)
            if b"EAF_BENCH_SUITE_END" in b"".join(chunks[-4:]):
                complete = True
                break

    output = b"".join(chunks).decode("utf-8", errors="replace")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")
    if not complete:
        raise SystemExit(f"benchmark did not finish within {args.timeout:.0f}s; partial log saved to {args.output}")
    print(f"\nSaved complete device log to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
