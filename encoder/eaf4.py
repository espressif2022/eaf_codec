"""EAF4 BGRA8888 codec using frame submodes 0..6.

Submodes 0..5 are representations of unified codec 0x04 (LOSSLESS); submode 6
is unified codec 0x05 (BTC_SAFE). They are not top-level codec IDs.

  - Mode 0 (MODE_RAW)          : Full-frame BGRA8888 Deflate (Keyframe, lossless)
  - Mode 1 (MODE_XOR)          : Full-frame BGRA8888 32-bit XOR Deflate (lossless)
  - Mode 2 (MODE_PLANAR)       : 4-plane (B,G,R,A) XOR Deflate (lossless)
  - Mode 3 (MODE_TILE_PLANAR)  : 4x4 tile mask + 4-plane XOR Deflate (lossless)
  - Mode 4 (MODE_TILE_PACKED)  : 4x4 tile mask + interleaved 32-bit BGRA word XOR Deflate (lossless, fast)
  - Mode 5 (MODE_JOINT_DELTA)  : joint ternary symbol (-1..1)^4 + 4-plane escape Deflate (lossless)
  - Mode 6 (MODE_BTC8888_SAFE) : 4x4 error-bounded BTC with 64B raw escape
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

from .btc8888 import encode_btc8888_frame, restore_btc8888_frame

HEADER_SIZE = 64
INDEX_SIZE = 24
HEADER = struct.Struct("<4sHHHHIIIIIIHHIII12s")
INDEX = struct.Struct("<IIIIBBHI")

MODE_RAW = 0
MODE_XOR = 1
MODE_PLANAR = 2
MODE_TILE_PLANAR = 3
MODE_TILE_PACKED = 4
MODE_JOINT_DELTA = 5
MODE_BTC8888_SAFE = 6

MODE_NAMES = {
    MODE_RAW: "raw",
    MODE_XOR: "xor",
    MODE_PLANAR: "planar_8888",
    MODE_TILE_PLANAR: "tile_planar_8888",
    MODE_TILE_PACKED: "tile_packed_8888",
    MODE_JOINT_DELTA: "joint_delta_8888",
    MODE_BTC8888_SAFE: "btc8888_safe",
}


def load_bgra(path: str | Path) -> np.ndarray:
    with Image.open(path) as image:
        rgba = np.array(image.convert("RGBA"), dtype=np.uint8)
    return np.ascontiguousarray(rgba[..., [2, 1, 0, 3]])


def encode_joint_delta(current: np.ndarray, previous: np.ndarray) -> tuple[bytes, int]:
    """Mode 5: 4x4 changed-tile bitmap + ternary joint delta symbol (0..80, 255=escape) + 4-plane escapes."""
    height, width, _ = current.shape
    cur_tiles = current.reshape(height // 4, 4, width // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    old_tiles = previous.reshape(height // 4, 4, width // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    changed = np.any(cur_tiles != old_tiles, axis=(1, 2))
    mask_bytes = np.packbits(changed, bitorder="little").tobytes()
    now = cur_tiles[changed].reshape(-1, 4)
    old = old_tiles[changed].reshape(-1, 4)
    if len(now) == 0:
        raw = mask_bytes
        return zlib.compress(raw, 6), len(raw)
    delta = now.astype(np.int16) - old.astype(np.int16)
    small = np.all((delta >= -1) & (delta <= 1), axis=1)
    symbols = np.full(len(now), 255, dtype=np.uint8)
    d = delta[small] + 1
    symbols[small] = (d[:, 0] * 27 + d[:, 1] * 9 + d[:, 2] * 3 + d[:, 3]).astype(np.uint8)
    escapes = np.ascontiguousarray(now[~small].T).tobytes()
    raw = mask_bytes + symbols.tobytes() + escapes
    return zlib.compress(raw, 6), len(raw)


def candidates(current: np.ndarray, previous: np.ndarray, include_v2: bool = False) -> list[tuple[bytes, int]]:
    """Return (deflate_blob, uncompressed_size) for lossless modes 0..3 (or 0..5 if include_v2=True)."""
    height, width, _ = current.shape
    difference = current ^ previous
    tiles = difference.reshape(height // 4, 4, width // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    changed = np.any(tiles != 0, axis=(1, 2))
    mask_bytes = np.packbits(changed, bitorder="little").tobytes()
    changed_tiles = tiles[changed]
    tile_planar_blob = mask_bytes + changed_tiles.transpose(2, 0, 1).tobytes()
    raw_list = [
        current.tobytes(),
        difference.tobytes(),
        difference.transpose(2, 0, 1).tobytes(),
        tile_planar_blob,
    ]
    out = [(zlib.compress(payload, 6), len(payload)) for payload in raw_list]
    if include_v2:
        tile_packed_blob = mask_bytes + np.ascontiguousarray(changed_tiles).tobytes()
        out.append((zlib.compress(tile_packed_blob, 6), len(tile_packed_blob)))
        out.append(encode_joint_delta(current, previous))
    return out


def restore(blob: bytes, mode: int, previous: np.ndarray) -> np.ndarray:
    height, width, _ = previous.shape
    if mode == MODE_BTC8888_SAFE:
        return restore_btc8888_frame(blob, previous)
    data = zlib.decompress(blob)
    if mode == MODE_RAW:
        return np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4).copy()
    if mode == MODE_XOR:
        return np.frombuffer(data, dtype=np.uint8).reshape(height, width, 4) ^ previous
    if mode == MODE_PLANAR:
        return np.frombuffer(data, dtype=np.uint8).reshape(4, height, width).transpose(1, 2, 0) ^ previous
    tile_count = (height // 4) * (width // 4)
    mask_bytes = (tile_count + 7) // 8
    mask = np.unpackbits(np.frombuffer(data[:mask_bytes], dtype=np.uint8), bitorder="little")[:tile_count].astype(bool)
    changed = int(mask.sum())
    if mode == MODE_TILE_PLANAR:
        tiles = np.zeros((tile_count, 16, 4), dtype=np.uint8)
        if changed:
            tiles[mask] = np.frombuffer(data[mask_bytes:], dtype=np.uint8).reshape(4, changed, 16).transpose(1, 2, 0)
        restored = tiles.reshape(height // 4, width // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4)
        return restored ^ previous
    if mode == MODE_TILE_PACKED:
        tiles = np.zeros((tile_count, 16, 4), dtype=np.uint8)
        if changed:
            tiles[mask] = np.frombuffer(data[mask_bytes:], dtype=np.uint8).reshape(changed, 16, 4)
        restored = tiles.reshape(height // 4, width // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4)
        return restored ^ previous
    if mode == MODE_JOINT_DELTA:
        old_tiles = previous.reshape(height // 4, 4, width // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4).copy()
        if changed == 0:
            return previous.copy()
        sym_end = mask_bytes + changed * 16
        sym = np.frombuffer(data[mask_bytes:sym_end], dtype=np.uint8)
        baseline = old_tiles[mask].reshape(-1, 4).astype(np.int16)
        sm = sym != 255
        for c_idx, weight in enumerate((27, 9, 3, 1)):
            baseline[sm, c_idx] += ((sym[sm] // weight) % 3).astype(np.int16) - 1
        escaped = int((~sm).sum())
        if escaped:
            esc = np.frombuffer(data[sym_end:], dtype=np.uint8).reshape(4, escaped).T
            baseline[~sm] = esc
        old_tiles[mask] = (baseline & 0xFF).astype(np.uint8).reshape(changed, 16, 4)
        return np.ascontiguousarray(
            old_tiles.reshape(height // 4, width // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4)
        )
    raise ValueError(f"unknown EAF4 mode {mode}")


def _pack(
    records: list[tuple[int, int, int, int, int]],
    payloads: list[bytes],
    width: int,
    height: int,
    period_ms: int,
) -> bytes:
    index_offset = HEADER_SIZE
    data_offset = HEADER_SIZE + len(records) * INDEX_SIZE
    offset = data_offset
    index = bytearray()
    for length, decoded_size, frame_crc, mode, payload_crc in records:
        index.extend(INDEX.pack(offset, length, decoded_size, frame_crc, mode, 0, 0, payload_crc))
        offset += length
    header = bytearray(
        HEADER.pack(
            b"EAF4",
            1,
            HEADER_SIZE,
            width,
            height,
            len(records),
            period_ms,
            width * height * 4,
            index_offset,
            data_offset,
            offset,
            4,
            4,
            0,
            zlib.crc32(index) & 0xFFFFFFFF,
            1,
            bytes(12),
        )
    )
    struct.pack_into("<I", header, 40, zlib.crc32(header) & 0xFFFFFFFF)
    return bytes(header) + bytes(index) + b"".join(payloads)


def encode(
    frames: list[np.ndarray],
    period_ms: int = 67,
    key_interval: int = 30,
    profile: str = "v1",
    forced_mode: int | None = None,
    btc_max_error: int = 8,
) -> tuple[bytes, dict]:
    """Encode BGRA8888 frames into an EAF4 container.

    profiles:
      - "v1"          : adaptive over legacy-compatible lossless modes 0..3
      - "v2"          : adaptive over lossless modes 0..5 (includes Tile-Packed & Joint-Delta)
      - "tile_packed" : force Mode 0 on keyframes and Mode 4 (Tile-Packed 32-bit XOR) on inter-frames
      - "btc8888"     : Mode 6 (BTC-8888 Safe error-bounded 4x4 blocks with 64B raw escape)
    """
    if not frames or not 10 <= period_ms <= 10000 or key_interval < 1:
        raise ValueError("invalid EAF4 parameters")
    payloads: list[bytes] = []
    records: list[tuple[int, int, int, int, int]] = []
    previous = None
    counts = [0] * 7
    frame_crcs: list[int] = []
    width = height = 0
    is_lossless = profile != "btc8888" and forced_mode != MODE_BTC8888_SAFE

    for index, current in enumerate(frames):
        if current.dtype != np.uint8 or current.ndim != 3 or current.shape[2] != 4:
            raise ValueError(f"frame {index} is not uint8 BGRA")
        height, width, _ = current.shape
        if width % 4 or height % 4 or not 0 < width <= 1024 or not 0 < height <= 1024:
            raise ValueError(f"invalid dimensions {width}x{height}")
        current = np.ascontiguousarray(current)
        if previous is None:
            previous = np.zeros_like(current)
        if current.shape != previous.shape:
            raise ValueError("frame dimensions differ")

        is_key = index % key_interval == 0
        if profile == "btc8888" or forced_mode == MODE_BTC8888_SAFE:
            mode = MODE_BTC8888_SAFE
            blob, decoded_size, decoded = encode_btc8888_frame(
                current, previous, keyframe=is_key, max_error=btc_max_error
            )
            verify = restore(blob, mode, previous)
            if not np.array_equal(decoded, verify):
                raise ValueError(f"BTC8888 decoder mismatch at frame {index}")
            max_err = int(np.max(np.abs(current.astype(np.int16) - decoded.astype(np.int16))))
            if max_err > btc_max_error:
                raise ValueError(f"BTC8888 error bound {max_err} > {btc_max_error} at frame {index}")
        else:
            include_v2 = profile in ("v2", "tile_packed") or (forced_mode is not None and forced_mode >= 4)
            choices = candidates(current, previous, include_v2=include_v2)
            if is_key:
                mode = MODE_RAW
            elif forced_mode is not None:
                mode = forced_mode
            elif profile == "tile_packed":
                mode = MODE_TILE_PACKED
            elif profile == "v2":
                # Speed-Aware RDO: prefer fast Tile-Packed (mode 4) or Raw (mode 0);
                # only pick Joint-Delta (mode 5) when it saves >= 8% and >= 256 bytes over Tile-Packed.
                best_fast = min((MODE_RAW, MODE_TILE_PACKED), key=lambda m: len(choices[m][0]))
                fast_len = len(choices[best_fast][0])
                joint_len = len(choices[MODE_JOINT_DELTA][0])
                if joint_len + 256 <= fast_len and joint_len <= int(fast_len * 0.92):
                    mode = MODE_JOINT_DELTA
                else:
                    mode = best_fast
            else:
                mode = min(range(4), key=lambda m: len(choices[m][0]))
            blob, decoded_size = choices[mode]
            decoded = restore(blob, mode, previous)
            if not np.array_equal(current, decoded):
                raise ValueError(f"roundtrip failed at frame {index} (mode {mode})")

        frame_crc = zlib.crc32(decoded.tobytes()) & 0xFFFFFFFF
        records.append((len(blob), decoded_size, frame_crc, mode, zlib.crc32(blob) & 0xFFFFFFFF))
        payloads.append(blob)
        counts[mode] += 1
        frame_crcs.append(frame_crc)
        previous = decoded

    blob = _pack(records, payloads, width, height, period_ms)
    summary = {
        "width": width,
        "height": height,
        "frames": len(records),
        "period_ms": period_ms,
        "key_interval": key_interval,
        "profile": profile,
        "bytes": len(blob),
        "mode_counts": counts if any(counts[4:]) else counts[:4],
        "mode_counts_all": counts,
        "frame_crc32": frame_crcs,
        "roundtrip_exact": is_lossless,
    }
    return blob, summary


def encode_paths(
    paths: list[str | Path],
    output: str | Path,
    period_ms: int = 67,
    key_interval: int = 30,
    profile: str = "v1",
    forced_mode: int | None = None,
    btc_max_error: int = 8,
) -> dict:
    frames = [load_bgra(path) for path in paths]
    blob, summary = encode(
        frames,
        period_ms=period_ms,
        key_interval=key_interval,
        profile=profile,
        forced_mode=forced_mode,
        btc_max_error=btc_max_error,
    )
    destination = Path(output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as handle:
        handle.write(blob)
    return summary


def decode(data: bytes) -> list[np.ndarray]:
    """Reference decoder for Modes 0..6. Checks header, index, payload, and frame CRCs."""
    if len(data) < HEADER_SIZE or data[:4] != b"EAF4":
        raise ValueError("not an EAF4 file")
    header = bytearray(data[:HEADER_SIZE])
    expected = struct.unpack_from("<I", header, 40)[0]
    header[40:44] = b"\x00\x00\x00\x00"
    if (zlib.crc32(header) & 0xFFFFFFFF) != expected:
        raise ValueError("header crc mismatch")
    version, header_size, width, height = struct.unpack_from("<HHHH", data, 4)
    frames, _period_ms, frame_bytes, index_offset, data_offset, file_size = struct.unpack_from("<IIIIII", data, 12)
    tile_w, tile_h, _header_crc, index_crc, flags = struct.unpack_from("<HHIII", data, 36)
    if (version, header_size, tile_w, tile_h, flags) != (1, 64, 4, 4, 1):
        raise ValueError("unsupported EAF4 header")
    if file_size != len(data) or index_offset != 64 or data_offset != 64 + frames * INDEX_SIZE:
        raise ValueError("bad EAF4 layout")
    if frame_bytes != width * height * 4 or data[52:64] != bytes(12):
        raise ValueError("bad EAF4 geometry")
    index = data[HEADER_SIZE:data_offset]
    if (zlib.crc32(index) & 0xFFFFFFFF) != index_crc:
        raise ValueError("index crc mismatch")
    previous = np.zeros((height, width, 4), dtype=np.uint8)
    output: list[np.ndarray] = []
    for frame in range(frames):
        offset, length, decoded_size, frame_crc, mode, _pad, _reserved16, payload_crc = INDEX.unpack_from(
            index, frame * INDEX_SIZE
        )
        blob = data[offset : offset + length]
        if (zlib.crc32(blob) & 0xFFFFFFFF) != payload_crc:
            raise ValueError(f"payload crc mismatch at frame {frame}")
        if frame == 0 and mode not in (MODE_RAW, MODE_BTC8888_SAFE):
            raise ValueError("frame 0 must be a keyframe")
        restored = restore(blob, mode, previous)
        if restored.shape != previous.shape or decoded_size != len(zlib.decompress(blob)):
            raise ValueError(f"bad frame {frame}")
        if (zlib.crc32(restored.tobytes()) & 0xFFFFFFFF) != frame_crc:
            raise ValueError(f"frame crc mismatch at frame {frame}")
        output.append(np.ascontiguousarray(restored))
        previous = output[-1]
    return output
