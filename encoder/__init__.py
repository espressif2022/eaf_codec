"""Unified EAF codec package:
- EAF RGB565/RGB565A8 container (0x89 "EAF", codecs 0x00..0x07) in
  `eaf565` and `eaf565_lossless_rgba`
- EAF4 BGRA8888 container ("EAF4", frame submodes 0..6) in `eaf4` and
  `btc8888`; submodes 0..5 implement codec 0x04 and submode 6 implements 0x05
"""

from .eaf4 import (
    MODE_565A8_JOINT,
    MODE_565A8_TILES,
    MODE_BTC565_SAFE,
    MODE_BTC8888_SAFE,
    MODE_JOINT_DELTA,
    MODE_NAMES,
    MODE_PLANAR,
    MODE_RAW,
    MODE_TILE_PACKED,
    MODE_TILE_PLANAR,
    MODE_XOR,
    PIXFMT_BGRA8888,
    PIXFMT_RGB565,
    PIXFMT_RGB565A8,
    decode,
    encode,
    encode_565,
    encode_paths,
    load_bgra,
)

__all__ = [
    "MODE_RAW",
    "MODE_XOR",
    "MODE_PLANAR",
    "MODE_TILE_PLANAR",
    "MODE_TILE_PACKED",
    "MODE_JOINT_DELTA",
    "MODE_BTC8888_SAFE",
    "MODE_565A8_TILES",
    "MODE_565A8_JOINT",
    "MODE_BTC565_SAFE",
    "PIXFMT_BGRA8888",
    "PIXFMT_RGB565",
    "PIXFMT_RGB565A8",
    "MODE_NAMES",
    "decode",
    "encode",
    "encode_565",
    "encode_paths",
    "load_bgra",
]
