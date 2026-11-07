"""Mode 7 (QUAD_JPEG): Two-level Quadtree (16x16 + 8x8 quadrant mask) Abbreviated-JPEG for 888/565.

Extracts only changed 8x8 quadrants between consecutive frames, packs them into a 1D
horizontal strip of 16x16 MCUs (height = 16, width = 16 * M), and strips the redundant
JPEG DQT/DHT headers on inter-frames.

Hardware JPEG 2D-DMA alignment (ESP32-S31 / ESP32-P4):
  - YUV420 / YUV422 -> hb = 32 pixels (M must be a multiple of 2 MCUs)
  - YUV444          -> hb = 40 pixels for RGB888, 48 pixels for RGB565
  By padding M to a multiple of 2 (or 15 for YUV444), the 1D strip width is guaranteed
  to be a multiple of the hardware DMA burst block size!
"""

from __future__ import annotations

import io
import math
import struct
import numpy as np
from PIL import Image

SUB_KEYFRAME_FULL = 0
SUB_QUAD_DELTA = 1


def _bgr_to_rgb(bgra: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(bgra[..., [2, 1, 0]])


def _rgb_to_bgra(rgb: np.ndarray, alpha: int = 255) -> np.ndarray:
    h, w, _ = rgb.shape
    out = np.empty((h, w, 4), dtype=np.uint8)
    out[..., 0] = rgb[..., 2]
    out[..., 1] = rgb[..., 1]
    out[..., 2] = rgb[..., 0]
    out[..., 3] = alpha
    return out


def _encode_jpeg_bytes(rgb: np.ndarray, quality: int = 85, subsampling: int = 2) -> bytes:
    buf = io.BytesIO()
    Image.fromarray(rgb, "RGB").save(buf, format="JPEG", quality=quality, optimize=False, subsampling=subsampling)
    return buf.getvalue()


def _decode_jpeg_rgb(jpeg_bytes: bytes) -> np.ndarray:
    with Image.open(io.BytesIO(jpeg_bytes)) as im:
        return np.array(im.convert("RGB"), dtype=np.uint8)


def split_jpeg_header_and_scan(jb: bytes) -> tuple[bytes, int, bytes]:
    """Return (header_up_to_sos_data, sof0_offset, entropy_scan_without_eoi)."""
    if len(jb) < 4 or jb[0] != 0xFF or jb[1] != 0xD8:
        raise ValueError("Not a valid JPEG")
    pos = 2
    sof0_offset = -1
    n = len(jb)
    while pos + 4 <= n:
        if jb[pos] != 0xFF:
            pos += 1
            continue
        while pos < n and jb[pos] == 0xFF:
            pos += 1
        if pos >= n:
            break
        marker = jb[pos]
        pos += 1
        if marker in (0xD8, 0xD9) or (0xD0 <= marker <= 0xD7):
            continue
        seg_len = (jb[pos] << 8) | jb[pos + 1]
        if marker == 0xC0:
            sof0_offset = pos - 2
        if marker == 0xDA:
            hdr_end = pos + seg_len
            scan = jb[hdr_end:]
            if len(scan) >= 2 and scan[-2:] == b"\xFF\xD9":
                scan = scan[:-2]
            if sof0_offset < 0:
                raise ValueError("SOF0 not found before SOS")
            return jb[:hdr_end], sof0_offset, scan
        pos += seg_len
    raise ValueError("SOS marker not found in JPEG")


def rebuild_abbreviated_jpeg(hdr_template: bytes, sof0_offset: int, width: int, height: int, scan: bytes) -> bytes:
    hdr = bytearray(hdr_template)
    hdr[sof0_offset + 5] = (height >> 8) & 0xFF
    hdr[sof0_offset + 6] = height & 0xFF
    hdr[sof0_offset + 7] = (width >> 8) & 0xFF
    hdr[sof0_offset + 8] = width & 0xFF
    return bytes(hdr) + scan + b"\xFF\xD9"


def encode_quad_jpeg_frame(
    current_bgra: np.ndarray,
    prev_recon_bgra: np.ndarray,
    hdr_state: dict,
    keyframe: bool = False,
    quality: int = 85,
    subsampling: int = 2,
    mean_th: float = 1.5,
    max_th: int = 8,
    hw_dma_align: bool = True,
) -> tuple[bytes, int, np.ndarray]:
    """Encode one BGRA8888 frame as Mode 7 (QUAD_JPEG).

    Payload format (stored directly, not double-deflated):
      - byte 0: submode
          0 = Full-frame standard JPEG keyframe (updates shared header template)
              followed by raw JPEG file bytes.
          1 = Quadtree Abbreviated-JPEG delta frame:
              * uint16 mcus (number of 16x16 MCUs in the 1D strip, height=16, width=mcus*16)
              * outer_mask[(tiles_x * tiles_y + 7) // 8] (1 bit per 16x16 macroblock)
              * nibbles[(changed_16 + 1) // 2]           (4-bit quadrant mask per changed 16x16)
              * scan_bytes[...]                          (abbreviated JPEG entropy scan without header/EOI)
    """
    h, w, _ = current_bgra.shape
    if w % 16 != 0 or h % 16 != 0:
        raise ValueError(f"QUAD_JPEG requires width/height divisible by 16 (got {w}x{h})")

    cur_rgb = _bgr_to_rgb(current_bgra)
    prev_rgb = _bgr_to_rgb(prev_recon_bgra)

    # Always prepare full-frame keyframe candidate
    full_jb = _encode_jpeg_bytes(cur_rgb, quality=quality, subsampling=subsampling)
    full_hdr, full_sof0, _ = split_jpeg_header_and_scan(full_jb)

    if keyframe or "hdr" not in hdr_state:
        hdr_state["hdr"] = full_hdr
        hdr_state["sof0"] = full_sof0
        hdr_state["quality"] = quality
        hdr_state["subsampling"] = subsampling
        dec_rgb = _decode_jpeg_rgb(full_jb)
        rebuilt = _rgb_to_bgra(dec_rgb)
        payload = bytes([SUB_KEYFRAME_FULL]) + full_jb
        return payload, w * h * 4, rebuilt

    tiles_x = w // 16
    tiles_y = h // 16
    total_tiles = tiles_x * tiles_y
    mask_len = (total_tiles + 7) // 8
    mask_bits = bytearray(mask_len)
    nibbles_list: list[int] = []
    blocks_8x8: list[np.ndarray] = []

    cur_i = cur_rgb.astype(np.int16)
    prev_i = prev_rgb.astype(np.int16)

    for idx_t in range(total_tiles):
        ty = (idx_t // tiles_x) * 16
        tx = (idx_t % tiles_x) * 16
        sub_cur = cur_i[ty : ty + 16, tx : tx + 16]
        sub_prev = prev_i[ty : ty + 16, tx : tx + 16]
        q_mask = 0
        for q in range(4):
            qy, qx = (q >> 1) * 8, (q & 1) * 8
            diff = np.abs(sub_cur[qy : qy + 8, qx : qx + 8] - sub_prev[qy : qy + 8, qx : qx + 8])
            if float(diff.mean()) > mean_th or int(diff.max()) > max_th:
                q_mask |= 1 << q
                blocks_8x8.append(cur_rgb[ty + qy : ty + qy + 8, tx + qx : tx + qx + 8])
        if q_mask != 0:
            mask_bits[idx_t >> 3] |= 1 << (idx_t & 7)
            nibbles_list.append(q_mask)

    packed_nibbles = bytearray((len(nibbles_list) + 1) // 2)
    for i, nib in enumerate(nibbles_list):
        if i & 1:
            packed_nibbles[i >> 1] |= (nib & 0x0F) << 4
        else:
            packed_nibbles[i >> 1] = nib & 0x0F

    if len(blocks_8x8) == 0:
        payload = bytes([SUB_QUAD_DELTA]) + struct.pack("<H", 0) + bytes(mask_bits)
        return payload, w * h * 4, prev_recon_bgra.copy()

    mcus = (len(blocks_8x8) + 3) // 4
    if hw_dma_align:
        # Align strip width (16 * mcus) to 32 pixels (even MCU count) for S31/P4 2D-DMA YUV420/422
        mcu_align = 2 if subsampling in (1, 2) else 5
        mcus = ((mcus + mcu_align - 1) // mcu_align) * mcu_align

    atlas_w = mcus * 16
    atlas_h = 16
    atlas = np.zeros((atlas_h, atlas_w, 3), dtype=np.uint8)
    for idx_q, blk in enumerate(blocks_8x8):
        mcu_idx = idx_q >> 2
        q_sub = idx_q & 3
        ay = (q_sub >> 1) * 8
        ax = mcu_idx * 16 + (q_sub & 1) * 8
        atlas[ay : ay + 8, ax : ax + 8] = blk
    # Pad unused quadrants in the last MCU with the last block to avoid high-frequency DCT edges
    for idx_q in range(len(blocks_8x8), mcus * 4):
        mcu_idx = idx_q >> 2
        q_sub = idx_q & 3
        ay = (q_sub >> 1) * 8
        ax = mcu_idx * 16 + (q_sub & 1) * 8
        atlas[ay : ay + 8, ax : ax + 8] = blocks_8x8[-1]

    atlas_jb = _encode_jpeg_bytes(atlas, quality=hdr_state["quality"], subsampling=hdr_state["subsampling"])
    _, _, scan = split_jpeg_header_and_scan(atlas_jb)
    delta_payload = (
        bytes([SUB_QUAD_DELTA])
        + struct.pack("<H", mcus)
        + bytes(mask_bits)
        + bytes(packed_nibbles)
        + scan
    )

    # Auto fallback to full-frame JPEG if delta strip is larger than full-frame JPEG
    if len(full_jb) + 1 <= len(delta_payload):
        hdr_state["hdr"] = full_hdr
        hdr_state["sof0"] = full_sof0
        dec_rgb = _decode_jpeg_rgb(full_jb)
        return bytes([SUB_KEYFRAME_FULL]) + full_jb, w * h * 4, _rgb_to_bgra(dec_rgb)

    # Reconstruct exact decoder state using shared header template
    rebuilt_jb = rebuild_abbreviated_jpeg(hdr_state["hdr"], hdr_state["sof0"], atlas_w, atlas_h, scan)
    dec_atlas = _decode_jpeg_rgb(rebuilt_jb)
    out_rgb = prev_rgb.copy()
    idx8 = 0
    c = 0
    for idx_t in range(total_tiles):
        if not (mask_bits[idx_t >> 3] & (1 << (idx_t & 7))):
            continue
        ty = (idx_t // tiles_x) * 16
        tx = (idx_t % tiles_x) * 16
        nib = (packed_nibbles[c >> 1] >> 4) & 0x0F if (c & 1) else (packed_nibbles[c >> 1] & 0x0F)
        c += 1
        for q in range(4):
            if not (nib & (1 << q)):
                continue
            mcu_idx = idx8 >> 2
            q_sub = idx8 & 3
            ay = (q_sub >> 1) * 8
            ax = mcu_idx * 16 + (q_sub & 1) * 8
            idx8 += 1
            qy = ty + (q >> 1) * 8
            qx = tx + (q & 1) * 8
            out_rgb[qy : qy + 8, qx : qx + 8] = dec_atlas[ay : ay + 8, ax : ax + 8]

    return delta_payload, w * h * 4, _rgb_to_bgra(out_rgb)


def restore_quad_jpeg_frame(blob: bytes, previous_bgra: np.ndarray, hdr_state: dict) -> np.ndarray:
    """Reference decoder for Mode 7 (QUAD_JPEG)."""
    h, w, _ = previous_bgra.shape
    submode = blob[0]
    if submode == SUB_KEYFRAME_FULL:
        jb = blob[1:]
        hdr, sof0, _ = split_jpeg_header_and_scan(jb)
        hdr_state["hdr"] = hdr
        hdr_state["sof0"] = sof0
        return _rgb_to_bgra(_decode_jpeg_rgb(jb))
    if submode != SUB_QUAD_DELTA:
        raise ValueError(f"Unknown QUAD_JPEG submode {submode}")

    mcus = struct.unpack_from("<H", blob, 1)[0]
    tiles_x = w // 16
    tiles_y = h // 16
    total_tiles = tiles_x * tiles_y
    mask_len = (total_tiles + 7) // 8
    mask = blob[3 : 3 + mask_len]
    changed_16 = sum(bin(b).count("1") for b in mask)
    if changed_16 == 0 or mcus == 0:
        return previous_bgra.copy()

    nibble_len = (changed_16 + 1) // 2
    nibbles = blob[3 + mask_len : 3 + mask_len + nibble_len]
    scan = blob[3 + mask_len + nibble_len :]
    atlas_w = mcus * 16
    atlas_h = 16
    rebuilt_jb = rebuild_abbreviated_jpeg(hdr_state["hdr"], hdr_state["sof0"], atlas_w, atlas_h, scan)
    dec_atlas = _decode_jpeg_rgb(rebuilt_jb)

    out_bgra = previous_bgra.copy()
    dec_atlas_bgra = _rgb_to_bgra(dec_atlas)
    idx8 = 0
    c = 0
    for idx_t in range(total_tiles):
        if not (mask[idx_t >> 3] & (1 << (idx_t & 7))):
            continue
        ty = (idx_t // tiles_x) * 16
        tx = (idx_t % tiles_x) * 16
        nib = (nibbles[c >> 1] >> 4) & 0x0F if (c & 1) else (nibbles[c >> 1] & 0x0F)
        c += 1
        for q in range(4):
            if not (nib & (1 << q)):
                continue
            mcu_idx = idx8 >> 2
            q_sub = idx8 & 3
            ay = (q_sub >> 1) * 8
            ax = mcu_idx * 16 + (q_sub & 1) * 8
            idx8 += 1
            qy = ty + (q >> 1) * 8
            qx = tx + (q & 1) * 8
            out_bgra[qy : qy + 8, qx : qx + 8] = dec_atlas_bgra[ay : ay + 8, ax : ax + 8]
    return out_bgra
