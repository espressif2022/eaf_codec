#!/usr/bin/env python3
"""Full 8888 benchmark matrix on xiaobai animations in assets/xiaobai/.

Measures:
  - Compression size & ratio across Modes 0..6 (v1, v2, tile_packed, btc8888)
  - C decoder timing (inflate_us + reconstruct_us) via build/eaf4_verify
  - Quality metrics (PSNR dB, Max channel error)
"""

from __future__ import annotations

import json
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

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
    decode,
    encode,
    load_bgra,
)

XIAOBAI_DIR = ROOT / "assets" / "xiaobai"
VERIFIER = ROOT / "build" / "eaf4_verify"


def compute_psnr_and_max_err(src_frames: list[np.ndarray], dec_frames: list[np.ndarray]) -> tuple[str, int]:
    sse = 0.0
    max_err = 0
    total_values = 0
    for s, d in zip(src_frames, dec_frames):
        diff = np.abs(s.astype(np.int32) - d.astype(np.int32))
        max_err = max(max_err, int(diff.max()))
        sse += float(np.sum(diff.astype(np.float64) ** 2))
        total_values += s.size
    if sse == 0:
        return "Inf", 0
    mse = sse / total_values
    psnr = 10.0 * math.log10((255.0**2) / mse)
    return f"{psnr:.2f}", max_err


def measure_c_decode_us(blob: bytes, tmp_dir: Path) -> tuple[int, int]:
    path = tmp_dir / "bench.eaf4"
    path.write_bytes(blob)
    inflate_total = 0
    recon_total = 0
    runs = 5
    for _ in range(runs):
        proc = subprocess.run([str(VERIFIER), str(path)], capture_output=True, text=True, check=True)
        m = re.search(r"inflate=(\d+)\s+reconstruct=(\d+)", proc.stdout)
        if m:
            inflate_total += int(m.group(1))
            recon_total += int(m.group(2))
    return inflate_total // runs, recon_total // runs


def main() -> int:
    subprocess.run(["make", "-C", str(ROOT), "all"], check=True)
    report: dict = {"animations": []}

    print("=" * 112)
    print(f"{'Animation':<22} {'Codec / Mode':<20} {'Size (KB)':>10} {'Ratio':>8} {'Inflate(us)':>12} {'Recon(us)':>10} {'PSNR(dB)':>9} {'MaxErr':>6}")
    print("=" * 112)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        for folder_path in sorted(XIAOBAI_DIR.iterdir()):
            if not folder_path.is_dir():
                continue
            paths = sorted(folder_path.glob("frame_*.png"))
            if not paths:
                continue
            frames = [load_bgra(p) for p in paths]
            h, w, _ = frames[0].shape
            raw_bytes = sum(f.nbytes for f in frames)
            anim_name = f"{folder_path.name} ({w}x{h})"
            anim_entry = {"name": folder_path.name, "width": w, "height": h, "frames": len(frames), "raw_bytes": raw_bytes, "modes": {}}

            configs = [
                ("Mode 0 Raw-Deflate", dict(forced_mode=MODE_RAW)),
                ("Mode 1 Frame-XOR", dict(forced_mode=MODE_XOR)),
                ("Mode 2 Planar-8888", dict(forced_mode=MODE_PLANAR)),
                ("Mode 3 Tile-Planar", dict(forced_mode=MODE_TILE_PLANAR)),
                ("Mode 4 Tile-Packed", dict(forced_mode=MODE_TILE_PACKED)),
                ("Mode 5 Joint-Delta", dict(forced_mode=MODE_JOINT_DELTA)),
                ("EAF4 v1 Adaptive", dict(profile="v1")),
                ("EAF4 v2 Adaptive", dict(profile="v2")),
                ("Mode 6 BTC8888-Safe", dict(profile="btc8888", btc_max_error=8)),
            ]

            for label, kwargs in configs:
                blob, summary = encode(frames, **kwargs)
                decoded = decode(blob)
                psnr, max_err = compute_psnr_and_max_err(frames, decoded)
                inf_us, rec_us = measure_c_decode_us(blob, tmp_dir)
                ratio = raw_bytes / len(blob)
                print(
                    f"{anim_name:<22} {label:<20} {len(blob)/1024:10.1f} {ratio:7.2f}x "
                    f"{inf_us:12d} {rec_us:10d} {psnr:>9} {max_err:6d}"
                )
                anim_entry["modes"][label] = {
                    "bytes": len(blob),
                    "ratio": round(ratio, 2),
                    "inflate_us": inf_us,
                    "reconstruct_us": rec_us,
                    "psnr_db": psnr,
                    "max_err": max_err,
                    "mode_counts": summary["mode_counts_all"],
                }
            print("-" * 112)
            report["animations"].append(anim_entry)

    out_json = ROOT / "build" / "matrix_8888_report.json"
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"Saved report to {out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
