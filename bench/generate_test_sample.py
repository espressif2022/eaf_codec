#!/usr/bin/env python3
"""Regenerate the small public EAF4 asset embedded by the LVGL test apps."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from encoder.eaf4 import encode, load_bgra  # noqa: E402


DEFAULT_OUTPUT = ROOT / "assets" / "eaf_samples" / "synthetic_lossless.eaf4"
SOURCE_INDICES = (0, 4, 8, 12, 16, 20)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    source_dir = ROOT / "assets" / "synthetic" / "micro_motion"
    paths = [source_dir / f"frame_{index:03d}.png" for index in SOURCE_INDICES]
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        parser.error("missing generated source frames; run bench/generate_synthetic_assets.py first")

    frames = [load_bgra(path) for path in paths]
    blob, summary = encode(frames, period_ms=67, key_interval=3, profile="tile_packed")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    digest = hashlib.sha256(blob).hexdigest()
    print(
        f"Generated {args.output} ({summary['width']}x{summary['height']}, "
        f"{summary['frames']} frames, {len(blob)} bytes, sha256={digest})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
