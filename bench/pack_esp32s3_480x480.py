#!/usr/bin/env python3
"""Build a compact public 480x480 benchmark pack for ESP32-S3 LCD boards."""

from __future__ import annotations

import hashlib
import json
import math
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from bench.generate_visual_assets import _fit_crop  # noqa: E402
from bench.pack_s31_bench import (  # noqa: E402
    CONT_EAF4_8888,
    CONT_EAF_565,
    compute_quality_bgra,
    simulate_565_encoder_reconstruction,
)
from encoder.eaf4 import decode as decode_eaf4, encode as encode_eaf4  # noqa: E402
from encoder.eaf565 import convert_to_eaf  # noqa: E402


OUT_DIR = ROOT / "build" / "esp32s3_480x480"
WIDTH = 480
HEIGHT = 480
FRAMES = 2
PACK_LIMIT = 0xEE0000


def make_landscape_pan(master: Image.Image) -> list[Image.Image]:
    return [
        _fit_crop(master, WIDTH, HEIGHT, 1.08 + i * 0.015, 0.30 + i * 0.08, 0.50).convert("RGBA")
        for i in range(FRAMES)
    ]


def make_landscape_water(master: Image.Image) -> list[Image.Image]:
    base = np.asarray(_fit_crop(master, WIDTH, HEIGHT, 1.05, 0.54, 0.58).convert("RGB"), dtype=np.uint8)
    waterline = 270
    result = []
    for index in range(FRAMES):
        frame = base.copy()
        for y in range(waterline, HEIGHT):
            amplitude = 1.0 + 5.0 * (y - waterline) / (HEIGHT - waterline)
            frame[y] = np.roll(frame[y], int(round(amplitude * math.sin(index * 0.8 + y * 0.11))), axis=0)
        alpha = np.full((HEIGHT, WIDTH, 1), 255, dtype=np.uint8)
        result.append(Image.fromarray(np.concatenate((frame, alpha), axis=2), "RGBA"))
    return result


def make_robot_float(master: Image.Image) -> list[Image.Image]:
    alpha = np.asarray(master.getchannel("A"))
    ys, xs = np.where(alpha > 8)
    crop = master.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))
    result = []
    for index in range(FRAMES):
        target_h = 390 + index * 8
        target_w = int(round(crop.width * target_h / crop.height))
        sprite = crop.resize((target_w, target_h), Image.Resampling.LANCZOS)
        sprite = sprite.rotate(-2.0 + index * 4.0, Image.Resampling.BICUBIC, expand=True)
        canvas = Image.new("RGBA", (WIDTH, HEIGHT), (23, 27, 38, 0))
        cx = WIDTH // 2 + (-24 if index == 0 else 24)
        cy = HEIGHT // 2
        canvas.alpha_composite(sprite, (cx - sprite.width // 2, cy - sprite.height // 2))
        result.append(canvas)
    return result


def pixel_hash(scenes: list[tuple[str, list[Image.Image], list[str]]]) -> str:
    digest = hashlib.sha256()
    for name, frames, _modes in scenes:
        digest.update(name.encode())
        for frame in frames:
            digest.update(frame.convert("RGBA").tobytes())
    return digest.hexdigest()


def main() -> int:
    landscape = Image.open(ROOT / "assets" / "generated_sources" / "mountain_lake.png").convert("RGB")
    robot = Image.open(ROOT / "assets" / "generated_sources" / "robot_mascot.png").convert("RGBA")
    scenes = [
        ("landscape_pan_480", make_landscape_pan(landscape), ["eaf4-lossless", "eaf4-bounded", "jpeg-delta"]),
        ("landscape_water_480", make_landscape_water(landscape), ["eaf4-lossless", "eaf4-bounded", "jpeg-delta"]),
        ("robot_alpha_480", make_robot_float(robot), ["eaf4-lossless", "eaf4-bounded", "565a8-lossless", "565-bounded"]),
    ]

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    source_dir = OUT_DIR / "sources"
    source_dir.mkdir(exist_ok=True)
    entries: list[dict] = []
    blobs: list[bytes] = []

    with tempfile.TemporaryDirectory() as tmp_name:
        tmp = Path(tmp_name)
        for scene_name, frames_pil, modes in scenes:
            scene_dir = source_dir / scene_name
            scene_dir.mkdir(exist_ok=True)
            paths = []
            bgra = []
            for index, image in enumerate(frames_pil):
                path = scene_dir / f"frame_{index:03d}.png"
                image.save(path, optimize=True)
                paths.append(path)
                bgra.append(np.asarray(image.convert("RGBA"), dtype=np.uint8)[..., [2, 1, 0, 3]].copy())

            raw_8888 = WIDTH * HEIGHT * 4 * len(bgra)
            for mode in modes:
                if mode.startswith("eaf4-"):
                    profile = "v2" if mode == "eaf4-lossless" else "btc8888"
                    codec_key = "0x04_Lossless_8888_v2" if profile == "v2" else "0x05_Bounded_8888"
                    codec_title = "0x04 EAF Lossless (8888 v2)" if profile == "v2" else "0x05 EAF Bounded (8888 MaxErr<=8)"
                    blob, _summary = encode_eaf4(bgra, profile=profile, btc_max_error=8)
                    quality = compute_quality_bgra(bgra, decode_eaf4(blob))
                    fmt = "BGRA8888"
                    ctype = CONT_EAF4_8888
                    quality_method = "eaf4_reference_decode"
                    quality_max_error = quality["max_err"]
                    native_raw = raw_8888
                else:
                    if mode == "jpeg-delta":
                        encoder_mode, codec_key = "quad-jpeg", "0x06_JPEG_Delta_565"
                        codec_title = "0x06 EAF JPEG Delta (software decode on S3)"
                    elif mode == "565a8-lossless":
                        encoder_mode, codec_key = "lossless", "0x04_Lossless_565A8"
                        codec_title = "0x04 EAF Lossless (565+A8)"
                    else:
                        encoder_mode, codec_key = "btc-safe", "0x05_Bounded_565"
                        codec_title = "0x05 EAF Bounded (565 bound=8)"
                    out = tmp / f"{scene_name}_{codec_key}.eaf"
                    convert_to_eaf(
                        [str(path) for path in paths], str(out), mode=encoder_mode,
                        quality=82, keyframe_interval=30, jpeg_subsampling=2, btc_max_error=8,
                    )
                    blob = out.read_bytes()
                    quality = compute_quality_bgra(bgra, simulate_565_encoder_reconstruction(bgra, encoder_mode))
                    fmt = "RGB565A8" if encoder_mode == "lossless" else "RGB565"
                    ctype = CONT_EAF_565
                    quality_method = "jpeg_encoder_reconstruction_approximation" if encoder_mode == "quad-jpeg" else "encoder_reconstruction_reference"
                    quality_max_error = quality["max_err_rgb"]
                    native_raw = WIDTH * HEIGHT * (3 if encoder_mode == "lossless" else 2) * len(bgra)

                entries.append({
                    "asset": scene_name,
                    "dataset": "generated-480x480-v1",
                    "desc": "Public generated full-screen ESP32-S3 benchmark scene",
                    "codec_key": codec_key,
                    "codec_title": codec_title,
                    "format": fmt,
                    "container_type": ctype,
                    "width": WIDTH,
                    "height": HEIGHT,
                    "frames": len(bgra),
                    "raw_bytes": raw_8888,
                    "native_raw_bytes": native_raw,
                    "compressed_bytes": len(blob),
                    "ratio_vs_8888": round(raw_8888 / len(blob), 2),
                    "ratio_vs_native": round(native_raw / len(blob), 2),
                    "quality_method": quality_method,
                    "quality_max_error": quality_max_error,
                    **quality,
                })
                blobs.append(blob)

    count = len(blobs)
    data_start = (64 + count * 96 + 63) & ~63
    records = bytearray()
    payload = bytearray()
    for index, (entry, blob) in enumerate(zip(entries, blobs)):
        offset = data_start + len(payload)
        entry["pack_index"] = index
        entry["pack_offset"] = offset
        records += struct.pack(
            "<IIIHHI32s32s12s", offset, len(blob), entry["container_type"], WIDTH, HEIGHT, FRAMES,
            entry["asset"].encode()[:31], entry["codec_key"].encode()[:31], bytes(12),
        )
        payload += blob
        payload += bytes((64 - len(payload) % 64) % 64)
    total_size = data_start + len(payload)
    if total_size > PACK_LIMIT:
        raise ValueError(f"pack is {total_size} bytes; limit is {PACK_LIMIT}")
    pack = struct.pack("<4sIII48s", b"MBPK", 1, count, total_size, bytes(48))
    pack += records + bytes(data_start - 64 - len(records)) + payload
    pack_path = OUT_DIR / "esp32s3_480x480.bin"
    pack_path.write_bytes(pack)
    manifest = {
        "schema": 1,
        "dataset": "public-generated-480x480-v1",
        "board": "ESP32-S3-LCD-EV-Board-SUB2",
        "resolution": "480x480",
        "timed_passes": 10,
        "samples_per_entry": FRAMES * 10,
        "dataset_pixel_sha256": pixel_hash(scenes),
        "pack_file": pack_path.name,
        "pack_bytes": total_size,
        "partition_limit_bytes": PACK_LIMIT,
        "entries": entries,
    }
    (OUT_DIR / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"Packed {count} full-screen items: {total_size / 1048576:.2f} MiB; headroom {(PACK_LIMIT-total_size)/1048576:.2f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
