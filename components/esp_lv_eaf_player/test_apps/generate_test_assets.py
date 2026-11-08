#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Generate tiny public fixtures for the LVGL 8/9 component tests."""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


EAF4_HEADER = struct.Struct("<4sHHHHIIIIIIHHIII12s")
EAF4_INDEX = struct.Struct("<IIIIBBHI")


def write_legacy(path: Path) -> None:
    width = height = 16
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            pixels.append(1 + ((x // 4 + y // 4) & 1))

    rle = bytearray()
    run_value = pixels[0]
    run_length = 0
    for value in pixels:
        if value == run_value and run_length < 255:
            run_length += 1
        else:
            rle.extend((run_length, run_value))
            run_value = value
            run_length = 1
    rle.extend((run_length, run_value))

    palette = bytearray(256 * 4)
    palette[4:8] = bytes((0x20, 0x90, 0xF0, 0xFF))
    palette[8:12] = bytes((0xF0, 0x80, 0x20, 0xA0))
    block = b"\x00" + bytes(rle)  # codec 0x00: Palette RLE
    image = (
        b"_S\x00" + b"00V1.0" + bytes((8,))
        + struct.pack("<HHHHI", width, height, 1, height, len(block))
        + bytes(palette) + block
    )
    frame = b"\x5A\x5A" + image
    table = struct.pack("<II", len(frame), 0)
    table_and_data = table + frame
    outer = b"\x89EAF" + struct.pack(
        "<III", 1, sum(table_and_data) & 0xFFFFFFFF, len(table_and_data)
    )
    path.write_bytes(outer + table_and_data)


def write_eaf4(path: Path) -> None:
    width = height = 16
    frame = bytearray()
    for y in range(height):
        for x in range(width):
            frame.extend((x * 12, y * 12, 0xD0, 0x60 + ((x + y) & 0x7F)))

    payload = zlib.compress(bytes(frame), 6)
    index = EAF4_INDEX.pack(
        EAF4_HEADER.size + EAF4_INDEX.size,
        len(payload),
        len(frame),
        zlib.crc32(frame) & 0xFFFFFFFF,
        0,  # EAF4 raw lossless keyframe
        0,
        0,
        zlib.crc32(payload) & 0xFFFFFFFF,
    )
    total_size = EAF4_HEADER.size + len(index) + len(payload)
    header = bytearray(EAF4_HEADER.pack(
        b"EAF4", 1, EAF4_HEADER.size, width, height, 1, 67, len(frame),
        EAF4_HEADER.size, EAF4_HEADER.size + len(index), total_size,
        4, 4, 0, zlib.crc32(index) & 0xFFFFFFFF, 1, bytes(12),
    ))
    struct.pack_into("<I", header, 40, zlib.crc32(header) & 0xFFFFFFFF)
    path.write_bytes(header + index + payload)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} LEGACY_EAF LOSSLESS_EAF4", file=sys.stderr)
        return 2
    legacy_path, eaf4_path = map(Path, sys.argv[1:])
    legacy_path.parent.mkdir(parents=True, exist_ok=True)
    eaf4_path.parent.mkdir(parents=True, exist_ok=True)
    write_legacy(legacy_path)
    write_eaf4(eaf4_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
