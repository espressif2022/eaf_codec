#!/usr/bin/env python3
"""Fast, reproducible Host acceptance benchmark for the public synthetic assets.

The default run samples six frames from every scene and checks the two primary
BGRA8888 codecs: EAF Lossless v2 and EAF Bounded. It reports encode/decode throughput,
compression ratio, C decoder timing, PSNR and separate RGB/Alpha maximum error.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from encoder.eaf4 import decode, encode, load_bgra  # noqa: E402


DEFAULT_ASSET_DIRS = (ROOT / "assets" / "synthetic", ROOT / "assets" / "visual")
DEFAULT_OUTPUT = ROOT / "build" / "host_quick_report.json"
VERIFIER = ROOT / "build" / "eaf4_verify"
CONFIGS = (
    ("lossless_v2", {"profile": "v2"}),
    ("bounded_8", {"profile": "btc8888", "btc_max_error": 8}),
)


def sampled_paths(scene_dir: Path, count: int) -> list[Path]:
    paths = sorted(scene_dir.glob("frame_*.png"))
    if not paths:
        return []
    if len(paths) <= count:
        return paths
    indices = [round(i * (len(paths) - 1) / (count - 1)) for i in range(count)]
    return [paths[i] for i in indices]


def quality(source: list[np.ndarray], restored: list[np.ndarray]) -> dict:
    squared_error = 0.0
    values = 0
    rgb_max = 0
    alpha_max = 0
    for src, dst in zip(source, restored):
        delta = np.abs(src.astype(np.int16) - dst.astype(np.int16))
        rgb_max = max(rgb_max, int(delta[..., :3].max()))
        alpha_max = max(alpha_max, int(delta[..., 3].max()))
        squared_error += float(np.square(delta.astype(np.float64)).sum())
        values += delta.size
    if squared_error == 0:
        psnr = "Inf"
    else:
        mse = squared_error / values
        psnr = round(10.0 * math.log10((255.0**2) / mse), 2)
    return {"psnr_db": psnr, "rgb_max_error": rgb_max, "alpha_max_error": alpha_max}


def c_decode(blob: bytes, directory: Path, repetitions: int) -> dict:
    path = directory / "quick.eaf4"
    path.write_bytes(blob)
    totals = []
    for _ in range(repetitions):
        proc = subprocess.run([str(VERIFIER), str(path)], capture_output=True, text=True, check=True)
        match = re.search(r"timing_us read=(\d+) inflate=(\d+) reconstruct=(\d+)", proc.stdout)
        if not match:
            raise RuntimeError(f"unexpected verifier output: {proc.stdout}")
        read_us, inflate_us, reconstruct_us = map(int, match.groups())
        totals.append(read_us + inflate_us + reconstruct_us)
    totals.sort()
    return {"total_us": totals[len(totals) // 2], "repetitions": repetitions}


def run(assets_dirs: list[Path], sample_frames: int, repetitions: int) -> dict:
    subprocess.run(["make", "-C", str(ROOT), "all"], check=True, stdout=subprocess.DEVNULL)
    sources = []
    for assets_dir in assets_dirs:
        manifest_path = assets_dir / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
        sources.append(
            {
                "assets_dir": str(assets_dir),
                "generator_version": manifest.get("generator_version"),
                "dataset_pixel_sha256": manifest.get("dataset_pixel_sha256"),
            }
        )
    report = {
        "schema": 1,
        "sources": sources,
        "sample_frames": sample_frames,
        "scenes": [],
    }
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        scene_dirs = [
            scene_dir
            for assets_dir in assets_dirs
            for scene_dir in sorted(path for path in assets_dir.iterdir() if path.is_dir())
        ]
        for scene_dir in scene_dirs:
            paths = sampled_paths(scene_dir, sample_frames)
            if not paths:
                continue
            frames = [load_bgra(path) for path in paths]
            height, width, _ = frames[0].shape
            raw_bytes = sum(frame.nbytes for frame in frames)
            scene_result = {"name": scene_dir.name, "width": width, "height": height, "frames": len(frames), "codecs": {}}
            for name, kwargs in CONFIGS:
                start = time.perf_counter()
                blob, summary = encode(frames, key_interval=max(2, len(frames)), **kwargs)
                encode_s = time.perf_counter() - start
                start = time.perf_counter()
                restored = decode(blob)
                python_decode_s = time.perf_counter() - start
                metrics = quality(frames, restored)
                c_result = c_decode(blob, tmp_dir, repetitions)
                c_frame_us = c_result["total_us"] / len(frames)
                scene_result["codecs"][name] = {
                    "bytes": len(blob),
                    "ratio": round(raw_bytes / len(blob), 3),
                    "encode_ms": round(encode_s * 1000.0, 3),
                    "encode_fps": round(len(frames) / encode_s, 2),
                    "python_decode_ms": round(python_decode_s * 1000.0, 3),
                    "python_decode_fps": round(len(frames) / python_decode_s, 2),
                    "c_decode_us_per_frame": round(c_frame_us, 2),
                    "c_decode_fps": round(1_000_000.0 / c_frame_us, 2) if c_frame_us else None,
                    "mode_counts": summary["mode_counts_all"],
                    **metrics,
                }
            report["scenes"].append(scene_result)
    return report


def print_report(report: dict) -> None:
    print(
        f"Host quick benchmark: scenes={len(report['scenes'])} "
        f"sample_frames={report['sample_frames']} sources={len(report['sources'])}"
    )
    print(
        f"{'scene':<16} {'codec':<13} {'KiB':>8} {'ratio':>8} {'enc fps':>9} "
        f"{'C us/f':>9} {'C fps':>9} {'PSNR':>8} {'RGB/A err':>10}"
    )
    for scene in report["scenes"]:
        for codec, result in scene["codecs"].items():
            print(
                f"{scene['name']:<16} {codec:<13} {result['bytes']/1024:8.1f} "
                f"{result['ratio']:7.2f}x {result['encode_fps']:9.1f} "
                f"{result['c_decode_us_per_frame']:9.1f} {result['c_decode_fps']:9.1f} "
                f"{str(result['psnr_db']):>8} "
                f"{result['rgb_max_error']:>4}/{result['alpha_max_error']:<4}"
            )


def assess(report: dict) -> dict:
    failures = []
    for scene in report["scenes"]:
        lossless = scene["codecs"].get("lossless_v2")
        bounded = scene["codecs"].get("bounded_8")
        if not lossless or lossless["rgb_max_error"] != 0 or lossless["alpha_max_error"] != 0:
            failures.append(f"{scene['name']}: lossless roundtrip is not exact")
        if not bounded or bounded["rgb_max_error"] > 8 or bounded["alpha_max_error"] > 8:
            failures.append(f"{scene['name']}: bounded error limit exceeded")
        for codec, result in scene["codecs"].items():
            if not result["c_decode_fps"] or result["bytes"] <= 0:
                failures.append(f"{scene['name']}/{codec}: invalid C decode or payload result")
    return {"passed": not failures, "checks": len(report["scenes"]) * 4, "failures": failures}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--assets-dir",
        action="append",
        type=Path,
        dest="assets_dirs",
        help="scene root; repeat for several datasets (default: synthetic + visual)",
    )
    parser.add_argument("--sample-frames", type=int, default=6)
    parser.add_argument("--c-repetitions", type=int, default=3)
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    if args.sample_frames < 2 or args.c_repetitions < 1:
        parser.error("sample frames must be >=2 and repetitions >=1")
    assets_dirs = [path.resolve() for path in (args.assets_dirs or DEFAULT_ASSET_DIRS)]
    for assets_dir in assets_dirs:
        if not assets_dir.is_dir():
            parser.error(f"asset directory not found: {assets_dir}")
    report = run(assets_dirs, args.sample_frames, args.c_repetitions)
    report["acceptance"] = assess(report)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print_report(report)
    status = "PASS" if report["acceptance"]["passed"] else "FAIL"
    print(f"Acceptance: {status} ({report['acceptance']['checks']} checks)")
    for failure in report["acceptance"]["failures"]:
        print(f"  - {failure}")
    print(f"Saved JSON: {args.output}")
    return 0 if report["acceptance"]["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
