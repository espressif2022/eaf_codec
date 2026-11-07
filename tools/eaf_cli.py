#!/usr/bin/env python3
"""Unified EAF / EAF4 CLI:
- Auto-detects 0x89"EAF" (RGB565/RGB565A8, 0x00..0x0C) and "EAF4" (BGRA8888, Modes 0..6) on `inspect`/`info`
- Supports encoding into either EAF4 (BGRA8888) or EAF (RGB565/565A8) container.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from encoder.eaf4 import INDEX, INDEX_SIZE, MODE_NAMES, decode, encode_paths  # noqa: E402
from encoder.eaf565 import convert_to_eaf  # noqa: E402
from eaf_inspector.parser import parse_file, print_text  # noqa: E402


def _encode(args: argparse.Namespace) -> int:
    if args.container == "eaf565":
        convert_to_eaf(
            files=args.frames,
            out_path=args.output,
            mode=args.mode565,
            btc_max_error=args.btc_max_error,
            keyframe_interval=args.key_interval,
        )
        size = Path(args.output).stat().st_size
        print(f"wrote {args.output} container=EAF(0x89) mode={args.mode565} frames={len(args.frames)} bytes={size}")
        return 0

    summary = encode_paths(
        args.frames,
        args.output,
        period_ms=args.period,
        key_interval=args.key_interval,
        profile=args.profile,
        forced_mode=args.mode,
        btc_max_error=args.btc_max_error,
    )
    counts = summary["mode_counts_all"]
    print(
        f"wrote {args.output} container=EAF4 {summary['width']}x{summary['height']} "
        f"frames={summary['frames']} bytes={summary['bytes']} "
        f"profile={summary['profile']} modes={counts}"
    )
    return 0


def _inspect(args: argparse.Namespace) -> int:
    path = Path(args.file)
    data = path.read_bytes()
    if len(data) >= 4 and data[:4] == b"EAF4":
        frames = decode(data)
        version, header_size, width, height = struct.unpack_from("<HHHH", data, 4)
        n_frames, period_ms, frame_bytes, index_offset, data_offset, file_size = struct.unpack_from("<IIIIII", data, 12)
        counts = [0] * 7
        for i in range(n_frames):
            rec = INDEX.unpack_from(data, index_offset + i * INDEX_SIZE)
            m = rec[4]
            if 0 <= m < len(counts):
                counts[m] += 1
        mode_str = ", ".join(f"{MODE_NAMES[m]}:{c}" for m, c in enumerate(counts) if c > 0)
        print(
            f"OK {args.file}\n"
            f"  container         : EAF4 (BGRA8888 v{version})\n"
            f"  bytes             : {file_size}\n"
            f"  size              : {width}x{height}\n"
            f"  frames            : {len(frames)} (period={period_ms} ms, raw_frame={frame_bytes} B)\n"
            f"  modes             : {mode_str}"
        )
        return 0
    elif len(data) >= 4 and data[0] == 0x89 and data[1:4] in (b"EAF", b"AAF"):
        info = parse_file(path, verify_blocks=False)
        print_text(info, verbose=False, print_frames=False)
        return 0 if info.ok else 1
    else:
        print(f"Unsupported file header in {args.file}", file=sys.stderr)
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Unified EAF (565) & EAF4 (8888) codec CLI")
    sub = parser.add_subparsers(dest="command", required=True)

    enc = sub.add_parser("encode", help="encode PNG frames into EAF4 (8888) or EAF (565)")
    enc.add_argument("frames", nargs="+")
    enc.add_argument("-o", "--output", required=True)
    enc.add_argument(
        "--container",
        choices=["eaf4", "eaf565"],
        default="eaf4",
        help="eaf4=BGRA8888 (Modes 0..6), eaf565=0x89 EAF (RGB565/565A8, 0x07..0x0C)",
    )
    enc.add_argument("--period", type=int, default=67)
    enc.add_argument("--key-interval", type=int, default=30)
    enc.add_argument(
        "--profile",
        choices=["v1", "v2", "tile_packed", "btc8888"],
        default="v1",
        help="[EAF4] v1=Modes 0..3, v2=Modes 0..5, tile_packed=Mode 4, btc8888=Mode 6",
    )
    enc.add_argument("--mode", type=int, choices=range(7), default=None, help="[EAF4] force mode 0..6")
    enc.add_argument(
        "--mode565",
        choices=["lossless-rgba", "btc-safe", "quad-abbrev", "h264", "btc-alpha", "jpeg", "strip-delta"],
        default="btc-safe",
        help="[EAF565] 0x0C lossless-rgba, 0x09 btc-safe, 0x08 quad-abbrev, 0x07 h264, 0x0B btc-alpha",
    )
    enc.add_argument("--btc-max-error", type=int, default=8, help="max channel error for BTC-Safe (565 or 8888)")
    enc.set_defaults(func=_encode)

    for cmd in ("info", "inspect"):
        p = sub.add_parser(cmd, help="auto-detect EAF(0x89) or EAF4 header and inspect contents")
        p.add_argument("file")
        p.set_defaults(func=_inspect)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
