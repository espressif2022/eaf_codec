"""Unified EAF codec package:
- EAF RGB565/RGB565A8 container (0x89 "EAF", codecs 0x00..0x07) in
  `eaf565` and `eaf565_lossless_rgba`
- EAF4 BGRA8888 container ("EAF4", frame submodes 0..6) in `eaf4` and
  `btc8888`; submodes 0..5 implement codec 0x04 and submode 6 implements 0x05
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
