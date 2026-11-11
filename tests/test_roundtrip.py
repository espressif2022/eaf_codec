#!/usr/bin/env python3
"""Verify EAF4 submodes 0..6 across Python/C plus unified CLI roundtrips."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

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


def synthetic_frames() -> list[np.ndarray]:
    frames = []
    for index in range(6):
        image = np.zeros((16, 32, 4), dtype=np.uint8)
        image[..., 0] = np.arange(32, dtype=np.uint8) * 4
        image[..., 1] = np.arange(16, dtype=np.uint8)[:, None] * 12
        image[..., 2] = 40 + index * 10
        image[..., 3] = 255
        if index:
            image[4:12, 8:20, 0] = (image[4:12, 8:20, 0] + index) & 0xFF
            image[4:12, 8:20, 1] = 10 * index
            image[4:12, 8:20, 3] = 80 + index * 15
        frames.append(image)
    return frames


def write_pngs(frames: list[np.ndarray], directory: Path) -> list[Path]:
    paths = []
    for index, bgra in enumerate(frames):
        rgba = bgra[..., [2, 1, 0, 3]]
        path = directory / f"frame_{index:02d}.png"
        Image.fromarray(rgba, "RGBA").save(path)
        paths.append(path)
    return paths


def verify_with_c(verifier: Path, blob: bytes, tmp_dir: Path, name: str) -> None:
    path = tmp_dir / f"{name}.eaf4"
    path.write_bytes(blob)
    subprocess.run([str(verifier), str(path)], check=True)


def main() -> int:
    subprocess.run(["make", "-C", str(ROOT), "all"], check=True)
    verifier = ROOT / "build" / "eaf4_verify"
    frames = synthetic_frames()

    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)

        # 1. Test every single mode (0..6) in both Python and C decoders
        for mode in (
            MODE_RAW,
            MODE_XOR,
            MODE_PLANAR,
            MODE_TILE_PLANAR,
            MODE_TILE_PACKED,
            MODE_JOINT_DELTA,
            MODE_BTC8888_SAFE,
        ):
            blob, _ = encode(frames, period_ms=67, key_interval=3, forced_mode=mode, btc_max_error=8)
            restored = decode(blob)
            for idx, (src, out) in enumerate(zip(frames, restored)):
                err = int(np.max(np.abs(src.astype(np.int16) - out.astype(np.int16))))
                if mode == MODE_BTC8888_SAFE:
                    if err > 8:
                        raise SystemExit(f"mode 6 error {err} > 8 at frame {idx}")
                elif err != 0:
                    raise SystemExit(f"lossless mode {mode} mismatch at frame {idx}")
            verify_with_c(verifier, blob, directory, f"mode_{mode}")

        # 2. Verify unified CLI encoding & dual-header inspection across 0x04..0x06
        paths = write_pngs(frames[:4], directory)
        v1_out = directory / "clip_lossless_8888.eaf4"
        btc8888_out = directory / "clip_btc_8888.eaf4"
        eaf565_lossless = directory / "clip_lossless_565.eaf"
        eaf565_btc = directory / "clip_btc_565.eaf"
        eaf565_quad = directory / "clip_quad_jpeg.eaf"
        cli = str(ROOT / "tools" / "eaf_cli.py")
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(v1_out), "--format", "8888", "--codec", "lossless", "--key-interval", "2", *[str(p) for p in paths]],
            check=True,
        )
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(btc8888_out), "--format", "8888", "--codec", "bounded", *[str(p) for p in paths]],
            check=True,
        )
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(eaf565_lossless), "--format", "565", "--codec", "lossless", *[str(p) for p in paths]],
            check=True,
        )
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(eaf565_btc), "--format", "565", "--codec", "bounded", *[str(p) for p in paths]],
            check=True,
        )
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(eaf565_quad), "--format", "565", "--codec", "jpeg-delta", *[str(p) for p in paths]],
            check=True,
        )
        for p_out in (v1_out, btc8888_out, eaf565_lossless, eaf565_btc, eaf565_quad):
            subprocess.run([sys.executable, cli, "inspect", str(p_out)], check=True)
        # Also verify legacy 0x00..0x03 sample inspection
        for sample in sorted((ROOT / "assets" / "eaf_samples").glob("*.eaf")):
            subprocess.run([sys.executable, cli, "inspect", str(sample)], check=True)
        # Verify C header generation and batch flash packager
        c_hdr = directory / "clip_asset.h"
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(directory / "hdr_test.eaf4"), "--c-header", str(c_hdr), str(directory)],
            check=True,
        )
        if not c_hdr.is_file() or "hdr_test_data" not in c_hdr.read_text():
            raise SystemExit("C header emission failed")
        batch_out = directory / "batch_out"
        subprocess.run(
            [sys.executable, cli, "batch", str(ROOT / "assets" / "synthetic"), "-o", str(batch_out), "--format", "8888", "--codec", "lossless"],
            check=True,
        )
        if not (batch_out / "eaf_assets.bin").is_file() or not (batch_out / "eaf_assets_table.h").is_file():
            raise SystemExit("Batch flash packager failed")
        verify_with_c(verifier, v1_out.read_bytes(), directory, "cli_v1")
        verify_with_c(verifier, btc8888_out.read_bytes(), directory, "cli_btc8888")
        verify_with_c(verifier, eaf565_lossless.read_bytes(), directory, "cli_565_lossless")
        verify_with_c(verifier, eaf565_btc.read_bytes(), directory, "cli_565_btc")

        # Also verify --legacy-container for 565
        legacy_565 = directory / "clip_legacy_565.eaf"
        subprocess.run(
            [sys.executable, cli, "encode", "-o", str(legacy_565), "--format", "565", "--codec", "lossless", "--legacy-container", *[str(p) for p in paths]],
            check=True,
        )
        subprocess.run([sys.executable, cli, "inspect", str(legacy_565)], check=True)

        # 3. Verify representative public scenes across all EAF4 profiles.
        for clip_name in ("alpha_glow", "micro_motion", "natural_motion"):
            public_dir = ROOT / "assets" / "synthetic" / clip_name
            real_paths = sorted(public_dir.glob("frame_*.png"))[:8]
            real_frames = [load_bgra(p) for p in real_paths]
            for prof in ("v1", "v2", "tile_packed", "btc8888"):
                blob, summary = encode(real_frames, profile=prof, btc_max_error=8)
                verify_with_c(verifier, blob, directory, f"{clip_name}_{prof}")
                print(
                    f"asset {clip_name:<18} ({len(real_frames):2d}f) profile={prof:<11} "
                    f"bytes={summary['bytes']:>7} modes={summary['mode_counts_all']}"
                )

    print("all modes (0..6) and public synthetic assets verified OK in Python and C")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
