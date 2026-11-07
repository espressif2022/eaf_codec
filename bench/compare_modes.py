#!/usr/bin/env python3
"""Compare all EAF4 (Modes 0..6) sizes on a PNG frame sequence.

Usage: python3 bench/compare_modes.py frame_000.png frame_001.png ...
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from encoder.eaf4 import (  # noqa: E402
    MODE_BTC8888_SAFE,
    MODE_JOINT_DELTA,
    MODE_PLANAR,
    MODE_RAW,
    MODE_TILE_PACKED,
    MODE_TILE_PLANAR,
    MODE_XOR,
    encode,
    load_bgra,
)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: compare_modes.py frame.png ...", file=sys.stderr)
        return 2
    frames = [load_bgra(path) for path in sys.argv[1:]]
    height, width, _ = frames[0].shape
    raw = sum(frame.nbytes for frame in frames)

    configs = [
        ("mode0_raw", dict(forced_mode=MODE_RAW)),
        ("mode1_xor", dict(forced_mode=MODE_XOR)),
        ("mode2_planar", dict(forced_mode=MODE_PLANAR)),
        ("mode3_tile_planar", dict(forced_mode=MODE_TILE_PLANAR)),
        ("mode4_tile_packed", dict(forced_mode=MODE_TILE_PACKED)),
        ("mode5_joint_delta", dict(forced_mode=MODE_JOINT_DELTA)),
        ("adaptive_v1 (0..3)", dict(profile="v1")),
        ("adaptive_v2 (0..5)", dict(profile="v2")),
        ("mode6_btc8888_safe", dict(profile="btc8888", btc_max_error=8)),
    ]

    print(f"{width}x{height} frames={len(frames)} raw_bgra={raw}")
    print(f"{'mode':<20}{'bytes':>12}{'ratio':>10}")
    for name, kwargs in configs:
        blob, _ = encode(frames, **kwargs)
        print(f"{name:<20}{len(blob):12d}{raw / len(blob):10.2f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
