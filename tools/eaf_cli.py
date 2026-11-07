#!/usr/bin/env python3
"""Unified EAF / EAF4 CLI exposing the 4 converged production modes (0x04..0x07):

  - 0x04 lossless  : 100% Bit-Exact Lossless (BGRA8888 in EAF4, RGB565+A8 in EAF)
  - 0x05 btc-safe  : 4x4 Error-Bounded BTC + Raw Escape (maxErr <= 8, BGRA8888 & RGB565)
  - 0x06 quad-jpeg : 16x16+8x8 Quadtree Delta Strip JPEG (HW JPEG aligned, RGB888 & RGB565, no alpha)
  - 0x07 h264      : Constrained Baseline I/P Video Stream (RGB888 & RGB565, no alpha)
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
    if args.format == "565" or args.codec in ("quad-jpeg", "h264"):
        convert_to_eaf(
            files=args.frames,
            out_path=args.output,
            mode=args.codec,
            btc_max_error=args.btc_max_error,
            keyframe_interval=args.key_interval,
        )
        size = Path(args.output).stat().st_size
        print(
            f"wrote {args.output} container=EAF(0x89) format={args.format} "
            f"codec={args.codec} frames={len(args.frames)} bytes={size}"
        )
        return 0

    profile = "btc8888" if args.codec == "btc-safe" else args.lossless_profile
    summary = encode_paths(
        args.frames,
        args.output,
        period_ms=args.period,
        key_interval=args.key_interval,
        profile=profile,
        btc_max_error=args.btc_max_error,
    )
    counts = summary["mode_counts_all"]
    print(
        f"wrote {args.output} container=EAF4(8888) codec={args.codec} "
        f"{summary['width']}x{summary['height']} frames={summary['frames']} "
        f"bytes={summary['bytes']} submodes={counts}"
    )
    return 0


def _inspect(args: argparse.Namespace) -> int:
    path = Path(args.file)
    data = path.read_bytes()
    if len(data) >= 4 and data[:4] == b"EAF4":
        frames = decode(data)
        version, _header_size, width, height = struct.unpack_from("<HHHH", data, 4)
        n_frames, period_ms, frame_bytes, index_offset, _data_offset, file_size = struct.unpack_from("<IIIIII", data, 12)
        counts = [0] * 7
        for i in range(n_frames):
            rec = INDEX.unpack_from(data, index_offset + i * INDEX_SIZE)
            m = rec[4]
            if 0 <= m < len(counts):
                counts[m] += 1
        codec_id = "0x05 BTC_SAFE" if counts[6] > 0 else "0x04 LOSSLESS"
        sub_str = ", ".join(f"{MODE_NAMES[m]}:{c}" for m, c in enumerate(counts) if c > 0)
        print(
            f"OK {args.file}\n"
            f"  container         : EAF4 (BGRA8888 v{version})\n"
            f"  codec             : {codec_id}\n"
            f"  bytes             : {file_size}\n"
            f"  size              : {width}x{height}\n"
            f"  frames            : {len(frames)} (period={period_ms} ms, raw_frame={frame_bytes} B)\n"
            f"  frame_submodes    : {sub_str}"
        )
        return 0
    if len(data) >= 4 and data[0] == 0x89 and data[1:4] in (b"EAF", b"AAF"):
        info = parse_file(path, verify_blocks=False)
        print_text(info, verbose=False, print_frames=False)
        return 0 if info.ok else 1
    print(f"Unsupported file header in {args.file}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Unified EAF Codec CLI (0x04 lossless, 0x05 btc-safe, 0x06 quad-jpeg, 0x07 h264)")
    sub = parser.add_subparsers(dest="command", required=True)

    enc = sub.add_parser("encode", help="encode PNG frames using one of the 4 converged codecs (0x04..0x07)")
    enc.add_argument("frames", nargs="+")
    enc.add_argument("-o", "--output", required=True)
    enc.add_argument(
        "--format",
        choices=["8888", "565"],
        default="8888",
        help="8888=BGRA8888/RGB888, 565=RGB565/RGB565A8",
    )
    enc.add_argument(
        "--codec",
        choices=["lossless", "btc-safe", "quad-jpeg", "h264"],
        default="lossless",
        help="0x04=lossless, 0x05=btc-safe, 0x06=quad-jpeg (no alpha), 0x07=h264 (no alpha)",
    )
    enc.add_argument(
        "--lossless-profile",
        choices=["v1", "v2", "tile_packed"],
        default="v2",
        help="Sub-mode selection inside 0x04 lossless (v1=0..3 compat, v2=0..5 smallest, tile_packed=fastest)",
    )
    enc.add_argument("--period", type=int, default=67)
    enc.add_argument("--key-interval", type=int, default=30)
    enc.add_argument("--btc-max-error", type=int, default=8, help="max channel error for 0x05 btc-safe")
    enc.set_defaults(func=_encode)

    for cmd in ("info", "inspect"):
        p = sub.add_parser(cmd, help="auto-detect EAF(0x89) or EAF4 header and inspect contents")
        p.add_argument("file")
        p.set_defaults(func=_inspect)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
