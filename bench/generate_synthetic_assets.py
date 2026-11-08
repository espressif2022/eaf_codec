#!/usr/bin/env python3
"""Generate deterministic, redistributable RGBA benchmark sequences.

The generator intentionally uses only the Python standard library.  Every scene
is synthetic, has no external copyright dependency, and is stable for a given
GENERATOR_VERSION.  PNGs use RGBA8 with filter type 0 so source pixels are exact.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import zlib
from pathlib import Path


GENERATOR_VERSION = 1
FRAMES = 24


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def write_rgba_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    if len(pixels) != width * height * 4:
        raise ValueError("invalid RGBA buffer size")
    rows = b"".join(b"\x00" + pixels[y * width * 4 : (y + 1) * width * 4] for y in range(height))
    png = (
        b"\x89PNG\r\n\x1a\n"
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(rows, 9))
        + _chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def _clamp(value: float) -> int:
    return max(0, min(255, int(round(value))))


def _hash(x: int, y: int, seed: int = 0) -> int:
    n = (x * 0x1F123BB5) ^ (y * 0x5F356495) ^ (seed * 0x6C8E9CF5)
    n = (n ^ (n >> 15)) * 0x2C1B3C6D
    n = (n ^ (n >> 12)) * 0x297A2D39
    return (n ^ (n >> 15)) & 0xFFFFFFFF


def _render(width: int, height: int, fn) -> bytes:
    out = bytearray(width * height * 4)
    pos = 0
    for y in range(height):
        for x in range(width):
            r, g, b, a = fn(x, y)
            out[pos : pos + 4] = bytes((_clamp(r), _clamp(g), _clamp(b), _clamp(a)))
            pos += 4
    return bytes(out)


def flat_ui(frame: int, width: int, height: int) -> bytes:
    cx = 28 + (frame * 5) % (width - 56)
    pulse = 8 + (frame % 8)

    def pixel(x: int, y: int):
        if 16 <= x < width - 16 and 18 <= y < height - 18:
            color = (22, 28, 38, 255)
        else:
            color = (7, 9, 14, 255)
        if 28 <= y < 44 and 28 <= x < width - 28:
            color = (45, 58, 78, 255)
        if 64 <= y < 96 and 20 <= x < width - 20:
            color = (18, 105, 150, 255)
        if abs(x - cx) <= pulse and abs(y - 80) <= pulse:
            color = (255, 190, 36, 255)
        if 112 <= y < 126 and 30 <= x < 30 + (frame + 1) * (width - 60) // FRAMES:
            color = (44, 220, 135, 255)
        return color

    return _render(width, height, pixel)


def alpha_glow(frame: int, width: int, height: int) -> bytes:
    phase = 2.0 * math.pi * frame / FRAMES
    cx = width * (0.5 + 0.22 * math.sin(phase))
    cy = height * (0.5 + 0.18 * math.cos(phase))

    def pixel(x: int, y: int):
        d = math.hypot(x - cx, y - cy)
        a = 255.0 * max(0.0, 1.0 - d / 55.0) ** 2
        # Non-zero hidden RGB is deliberate, including where alpha reaches zero.
        return 45 + x * 120 / width, 95 + y * 120 / height, 255 - x * 55 / width, a

    return _render(width, height, pixel)


def dark_gradient(frame: int, width: int, height: int) -> bytes:
    phase = 2.0 * math.pi * frame / FRAMES

    def pixel(x: int, y: int):
        radial = math.hypot(x - width / 2, y - height / 2) / math.hypot(width / 2, height / 2)
        wave = math.sin(x * 0.075 + phase) + math.cos(y * 0.06 - phase * 0.7)
        base = 8.0 + 15.0 * (1.0 - radial) + 2.5 * wave
        return base * 0.70, base * 0.88, base * 1.15, 255

    return _render(width, height, pixel)


def fine_lines(frame: int, width: int, height: int) -> bytes:
    offset = frame % 8

    def pixel(x: int, y: int):
        color = (10, 10, 12, 255)
        if (x + offset) % 11 == 0 or (y - offset) % 13 == 0:
            color = (225, 230, 238, 255)
        if (x - y + frame * 2) % 29 == 0:
            color = (255, 35, 75, 255)
        if (x + y - frame * 3) % 31 == 0:
            color = (20, 210, 255, 255)
        if width // 2 - 1 <= x <= width // 2 + 1:
            color = (120, 255, 80, 255)
        return color

    return _render(width, height, pixel)


def natural_motion(frame: int, width: int, height: int) -> bytes:
    phase = 2.0 * math.pi * frame / FRAMES

    def pixel(x: int, y: int):
        nx = x / width
        ny = y / height
        cloud = (
            math.sin(x * 0.032 + phase)
            + 0.55 * math.sin(x * 0.071 + y * 0.026 - phase * 1.7)
            + 0.35 * math.cos(y * 0.093 + phase * 0.6)
        )
        haze = 24.0 * cloud
        r = 35 + 105 * ny + haze
        g = 75 + 105 * ny + haze
        b = 125 + 90 * (1.0 - nx) + haze
        return r, g, b, 255

    return _render(width, height, pixel)


def high_frequency(frame: int, width: int, height: int) -> bytes:
    def pixel(x: int, y: int):
        noise = _hash(x + frame * 3, y - frame * 2, 71)
        grain = noise & 0xFF
        spark = 255 if noise % 113 == 0 else 0
        return 55 + grain * 0.65 + spark * 0.2, 38 + grain * 0.48, 18 + grain * 0.27, 255

    return _render(width, height, pixel)


def pan_scroll(frame: int, width: int, height: int) -> bytes:
    shift = frame * 7

    def pixel(x: int, y: int):
        sx = x + shift
        cell = ((sx // 16) + (y // 16)) & 1
        n = (_hash(sx // 3, y // 3, 19) & 31) - 15
        if cell:
            return 42 + n, 115 + n, 70 + n, 255
        return 150 + n, 92 + n, 45 + n, 255

    return _render(width, height, pixel)


def micro_motion(frame: int, width: int, height: int) -> bytes:
    dot_x = 28 + frame * (width - 56) // (FRAMES - 1)

    def pixel(x: int, y: int):
        band = 18 * math.sin(x * 0.035) + 12 * math.cos(y * 0.047)
        color = (55 + band, 70 + band, 92 + band, 255)
        if (x - dot_x) ** 2 + (y - height // 2) ** 2 <= 16:
            color = (255, 245, 90, 255)
        return color

    return _render(width, height, pixel)


def color_key(frame: int, width: int, height: int) -> bytes:
    cx = 26 + frame * (width - 52) // (FRAMES - 1)
    cy = height // 2

    def pixel(x: int, y: int):
        # Exact RGB565 black background, intended for color key 0x0000.
        if (x - cx) ** 2 + (y - cy) ** 2 < 24**2:
            return 248, 128 + (x & 7) * 4, 24, 255
        return 0, 0, 0, 255

    return _render(width, height, pixel)


SCENES = {
    "flat_ui": (160, 160, flat_ui, "Opaque flat colors, hard edges and local motion"),
    "alpha_glow": (160, 160, alpha_glow, "Continuous straight alpha with non-zero hidden RGB"),
    "dark_gradient": (160, 160, dark_gradient, "Subtle near-black gradients and low-amplitude motion"),
    "fine_lines": (160, 160, fine_lines, "One-pixel lines and high-contrast colored edges"),
    "natural_motion": (160, 160, natural_motion, "Smooth multi-frequency natural-looking motion"),
    "high_frequency": (160, 160, high_frequency, "Frame-varying deterministic grain and sparse sparks"),
    "pan_scroll": (320, 160, pan_scroll, "Full-frame horizontal translation of textured content"),
    "micro_motion": (160, 160, micro_motion, "Static detailed background with a tiny moving region"),
    "color_key": (160, 160, color_key, "Exact RGB565 black key background with a hard-edge object"),
}


def generate(output: Path, frames: int) -> None:
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"generator_version": GENERATOR_VERSION, "frames_per_scene": frames, "scenes": []}
    dataset_hash = hashlib.sha256()
    for name, (width, height, renderer, description) in SCENES.items():
        scene_dir = output / name
        scene_dir.mkdir(exist_ok=True)
        scene_hash = hashlib.sha256()
        for stale_frame in scene_dir.glob("frame_*.png"):
            stale_frame.unlink()
        for frame in range(frames):
            pixels = renderer(frame * FRAMES // frames, width, height)
            write_rgba_png(scene_dir / f"frame_{frame:03d}.png", width, height, pixels)
            scene_hash.update(struct.pack("<III", frame, width, height))
            scene_hash.update(pixels)
        pixel_sha256 = scene_hash.hexdigest()
        dataset_hash.update(name.encode("utf-8") + bytes.fromhex(pixel_sha256))
        manifest["scenes"].append(
            {
                "name": name,
                "width": width,
                "height": height,
                "frames": frames,
                "description": description,
                "pixel_sha256": pixel_sha256,
            }
        )
    manifest["dataset_pixel_sha256"] = dataset_hash.hexdigest()
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("assets/synthetic"))
    parser.add_argument("--frames", type=int, default=FRAMES)
    args = parser.parse_args()
    if not 2 <= args.frames <= 240:
        parser.error("--frames must be in 2..240")
    generate(args.output, args.frames)
    print(f"Generated {len(SCENES)} scenes x {args.frames} frames in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
