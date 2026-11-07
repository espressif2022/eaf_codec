"""Unified EAF codec package:
- EAF 565 / 565A8 container (0x89 "EAF", modes 0x00..0x0C) in `eaf565` & `eaf565_lossless_rgba`
- EAF4 BGRA8888 container ("EAF4", modes 0..6) in `eaf4` & `btc8888`
"""

from .eaf4 import (
    MODE_BTC8888_SAFE,
    MODE_JOINT_DELTA,
    MODE_NAMES,
    MODE_PLANAR,
    MODE_RAW,
    MODE_TILE_PACKED,
    MODE_TILE_PLANAR,
    MODE_XOR,
    decode,
    encode,
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
    "MODE_NAMES",
    "decode",
    "encode",
    "encode_paths",
    "load_bgra",
]
