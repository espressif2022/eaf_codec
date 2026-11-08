#!/usr/bin/env python3
"""Derive deterministic animation sequences from checked-in generated masters."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageEnhance, __version__ as PILLOW_VERSION


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "assets" / "generated_sources"
DEFAULT_OUTPUT = ROOT / "assets" / "visual"
GENERATOR_VERSION = 1
FRAMES = 24


def _pixel_hash(images: list[Image.Image]) -> str:
    digest = hashlib.sha256()
    for index, image in enumerate(images):
        rgba = image.convert("RGBA")
        digest.update(index.to_bytes(4, "little"))
        digest.update(rgba.width.to_bytes(4, "little"))
        digest.update(rgba.height.to_bytes(4, "little"))
        digest.update(rgba.tobytes())
    return digest.hexdigest()


def _save_scene(output: Path, name: str, images: list[Image.Image]) -> str:
    scene_dir = output / name
    scene_dir.mkdir(parents=True, exist_ok=True)
    for stale in scene_dir.glob("frame_*.png"):
        stale.unlink()
    for index, image in enumerate(images):
        image.save(scene_dir / f"frame_{index:03d}.png", optimize=True)
    return _pixel_hash(images)


def _fit_crop(image: Image.Image, width: int, height: int, zoom: float, x_phase: float, y_phase: float) -> Image.Image:
    source_w, source_h = image.size
    target_ratio = width / height
    crop_w = int(source_w / zoom)
    crop_h = int(crop_w / target_ratio)
    if crop_h > source_h:
        crop_h = int(source_h / zoom)
        crop_w = int(crop_h * target_ratio)
    max_x = source_w - crop_w
    max_y = source_h - crop_h
    left = int(round(max_x * max(0.0, min(1.0, x_phase))))
    top = int(round(max_y * max(0.0, min(1.0, y_phase))))
    return image.crop((left, top, left + crop_w, top + crop_h)).resize(
        (width, height), Image.Resampling.LANCZOS
    )


def robot_float(master: Image.Image, frames: int) -> list[Image.Image]:
    alpha = np.asarray(master.getchannel("A"))
    ys, xs = np.where(alpha > 8)
    crop = master.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))
    result = []
    for index in range(frames):
        phase = 2.0 * math.pi * index / frames
        scale = 0.86 + 0.025 * math.sin(phase * 2.0)
        target_h = int(round(138 * scale))
        target_w = max(1, int(round(crop.width * target_h / crop.height)))
        sprite = crop.resize((target_w, target_h), Image.Resampling.LANCZOS)
        sprite = sprite.rotate(2.5 * math.sin(phase), Image.Resampling.BICUBIC, expand=True)
        canvas = Image.new("RGBA", (160, 160), (23, 27, 38, 0))
        center_x = 80 + int(round(10 * math.sin(phase)))
        center_y = 82 + int(round(5 * math.sin(phase * 2.0)))
        canvas.alpha_composite(sprite, (center_x - sprite.width // 2, center_y - sprite.height // 2))
        result.append(canvas)
    return result


def landscape_pan(master: Image.Image, frames: int) -> list[Image.Image]:
    result = []
    for index in range(frames):
        progress = index / (frames - 1)
        eased = 0.5 - 0.5 * math.cos(math.pi * progress)
        image = _fit_crop(master, 320, 180, 1.10 + 0.025 * eased, 0.12 + 0.70 * eased, 0.50)
        image = ImageEnhance.Brightness(image).enhance(0.98 + 0.035 * math.sin(math.pi * progress))
        result.append(image.convert("RGBA"))
    return result


def landscape_water(master: Image.Image, frames: int) -> list[Image.Image]:
    base = _fit_crop(master, 320, 180, 1.06, 0.54, 0.58).convert("RGB")
    source = np.asarray(base, dtype=np.uint8)
    result = []
    waterline = 100
    for index in range(frames):
        frame = source.copy()
        for y in range(waterline, frame.shape[0]):
            amplitude = 1.0 + 3.0 * (y - waterline) / (frame.shape[0] - waterline)
            shift = int(round(amplitude * math.sin(index * 0.55 + y * 0.27)))
            frame[y] = np.roll(frame[y], shift, axis=0)
        wave = 3.0 * math.sin(index * 2.0 * math.pi / frames)
        frame[waterline:, :, :] = np.clip(frame[waterline:, :, :].astype(np.int16) + wave, 0, 255)
        alpha = np.full((*frame.shape[:2], 1), 255, dtype=np.uint8)
        result.append(Image.fromarray(np.concatenate((frame.astype(np.uint8), alpha), axis=2), "RGBA"))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--frames", type=int, default=FRAMES)
    args = parser.parse_args()
    if not 2 <= args.frames <= 120:
        parser.error("--frames must be in 2..120")

    robot_path = SOURCE_DIR / "robot_mascot.png"
    landscape_path = SOURCE_DIR / "mountain_lake.png"
    if not robot_path.is_file() or not landscape_path.is_file():
        parser.error("missing generated masters under assets/generated_sources")

    robot = Image.open(robot_path).convert("RGBA")
    landscape = Image.open(landscape_path).convert("RGB")
    scenes = [
        ("robot_float", "Transparent generated mascot with translation, rotation and scale motion", robot_float(robot, args.frames)),
        ("landscape_pan", "Photorealistic mountain-lake scene with slow pan, zoom and exposure change", landscape_pan(landscape, args.frames)),
        ("landscape_water", "Photorealistic landscape with localized water ripple motion", landscape_water(landscape, args.frames)),
    ]

    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {
        "generator_version": GENERATOR_VERSION,
        "frames_per_scene": args.frames,
        "pillow_version": PILLOW_VERSION,
        "masters": {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (robot_path, landscape_path)
        },
        "scenes": [],
    }
    dataset_hash = hashlib.sha256()
    for name, description, images in scenes:
        pixel_sha256 = _save_scene(args.output, name, images)
        width, height = images[0].size
        manifest["scenes"].append(
            {
                "name": name,
                "width": width,
                "height": height,
                "frames": len(images),
                "description": description,
                "pixel_sha256": pixel_sha256,
            }
        )
        dataset_hash.update(name.encode("utf-8") + bytes.fromhex(pixel_sha256))
    manifest["dataset_pixel_sha256"] = dataset_hash.hexdigest()
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(scenes)} visual scenes x {args.frames} frames in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
