#!/usr/bin/env python3
"""Build a reproducible public MBPK image for ESP32-S31/P4 benchmarks.

All entries come from public procedural/generated assets or checked-in legacy
compatibility samples. The script never reads private source material.
"""

from __future__ import annotations

import json
import hashlib
import math
import re
import struct
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from encoder.eaf4 import decode as decode_eaf4, encode as encode_eaf4, load_bgra  # noqa: E402
from encoder.eaf565 import (  # noqa: E402
    convert_to_eaf,
    encode_btc_block,
    reconstruct_btc_block,
    rgb565_components,
    rgb_to_565_arr,
)

OUT_DIR = ROOT / "build" / "s31_bench_pack"

# Container type constants for the C benchmark runner
CONT_EAF4_8888 = 1
CONT_EAF_565 = 2
CONT_EAF_8888_OUT = 3


def compute_quality_bgra(src_frames: list[np.ndarray], recon_frames: list[np.ndarray]) -> dict:
    sse = 0.0
    sse_rgb = 0.0
    sse_a = 0.0
    max_err = 0
    max_err_rgb = 0
    total_px = 0
    diff_px = 0
    for s, r in zip(src_frames, recon_frames):
        d = np.abs(s.astype(np.int32) - r.astype(np.int32))
        max_err = max(max_err, int(d.max()))
        max_err_rgb = max(max_err_rgb, int(d[..., :3].max()))
        d2 = d.astype(np.float64) ** 2
        sse += float(np.sum(d2))
        sse_rgb += float(np.sum(d2[..., :3]))
        sse_a += float(np.sum(d2[..., 3]))
        total_px += s.shape[0] * s.shape[1]
        diff_px += int(np.any(d != 0, axis=2).sum())

    def _psnr(err_sum: float, count: int) -> float | str:
        if err_sum == 0:
            return "Inf"
        mse = err_sum / count
        return round(10.0 * math.log10((255.0**2) / mse), 2)

    return {
        "psnr_rgba_db": _psnr(sse, total_px * 4),
        "psnr_rgb_db": _psnr(sse_rgb, total_px * 3),
        "psnr_alpha_db": _psnr(sse_a, total_px),
        "max_err": max_err,
        "max_err_rgb": max_err_rgb,
        "diff_pixel_ratio": round(diff_px / total_px, 4),
    }


def simulate_565_lossless_recon(src_frames: list[np.ndarray]) -> list[np.ndarray]:
    """Reconstruct BGRA8888 equivalent of 0x04 Lossless 565+A8."""
    out = []
    for bgra in src_frames:
        rgb = bgra[..., [2, 1, 0]].astype(np.int32)
        c565 = rgb_to_565_arr(rgb).astype(np.uint16)
        comp = rgb565_components(c565)  # R8, G8, B8
        rec = np.empty_like(bgra)
        rec[..., 0] = comp[..., 2]
        rec[..., 1] = comp[..., 1]
        rec[..., 2] = comp[..., 0]
        rec[..., 3] = bgra[..., 3]
        out.append(rec)
    return out


def simulate_565_encoder_reconstruction(src_frames: list[np.ndarray], mode: str) -> list[np.ndarray]:
    """Reproduce the encoder's RGB565 reference frames for source-quality metrics."""
    if mode == "lossless":
        return simulate_565_lossless_recon(src_frames)

    if mode == "btc-safe":
        out = []
        height, width, _ = src_frames[0].shape
        ref = np.zeros((height, width), dtype=np.uint16)
        tiles_x = width // 4
        for index, frame in enumerate(src_frames):
            src_rgb = frame[..., [2, 1, 0]].astype(np.int32)
            src = rgb_to_565_arr(src_rgb).astype(np.uint16)
            blocks = src.reshape(height // 4, 4, tiles_x, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
            refs = ref.reshape(height // 4, 4, tiles_x, 4).transpose(0, 2, 1, 3).reshape(-1, 16).copy()
            keyframe = index == 0
            for tile, block in enumerate(blocks):
                target = rgb565_components(block)
                error = np.abs(target - rgb565_components(refs[tile]))
                if not keyframe and error.max() <= 8 and error.mean() <= 0.8:
                    continue
                block_mode, endpoints, mask = encode_btc_block(target, 8)
                rebuilt = reconstruct_btc_block(block_mode, endpoints, mask)
                if np.max(np.abs(target - rgb565_components(rebuilt))) > 8:
                    rebuilt = block
                refs[tile] = rebuilt
            ref = refs.reshape(height // 4, tiles_x, 4, 4).transpose(0, 2, 1, 3).reshape(height, width)
            comp = rgb565_components(ref)
            reconstructed = np.empty_like(frame)
            reconstructed[..., 0] = comp[..., 2]
            reconstructed[..., 1] = comp[..., 1]
            reconstructed[..., 2] = comp[..., 0]
            reconstructed[..., 3] = 255
            out.append(reconstructed)
        return out

    # JPEG Delta quality is an encoder-side approximation; device CRC and timing
    # still come from decoding the exact final EAF blob.
    from encoder.quad_jpeg import encode_quad_jpeg_frame

    out = []
    prev = np.zeros_like(src_frames[0])
    hdr_state: dict = {}
    for i, f in enumerate(src_frames):
        # Quantize to 565 first to reflect 565 pipeline fidelity
        rgb = f[..., [2, 1, 0]].astype(np.int32)
        c565 = rgb_to_565_arr(rgb).astype(np.uint16)
        comp = rgb565_components(c565)
        f565 = np.empty_like(f)
        f565[..., 0] = comp[..., 2]
        f565[..., 1] = comp[..., 1]
        f565[..., 2] = comp[..., 0]
        f565[..., 3] = 255
        _, _, rec = encode_quad_jpeg_frame(
            f565, prev, hdr_state, keyframe=(i % 30 == 0), quality=82, subsampling=2
        )
        # Re-quantize output to 565
        rgb_r = rec[..., [2, 1, 0]].astype(np.int32)
        c565_r = rgb_to_565_arr(rgb_r).astype(np.uint16)
        comp_r = rgb565_components(c565_r)
        rec565 = np.empty_like(f)
        rec565[..., 0] = comp_r[..., 2]
        rec565[..., 1] = comp_r[..., 1]
        rec565[..., 2] = comp_r[..., 0]
        rec565[..., 3] = 255
        out.append(rec565)
        prev = rec565
    return out


def load_frame_sequence(folder: Path, prefix: str = "frame_", max_frames: int = 45) -> tuple[list[Path], list[np.ndarray]]:
    paths = sorted(
        folder.glob(f"{prefix}*.png"),
        key=lambda p: int(re.search(r"(\d+)$", p.stem).group(1)),
    )[:max_frames]
    frames = [load_bgra(p) for p in paths]
    return paths, frames


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    datasets: list[tuple[str, str, str, list[Path], list[np.ndarray]]] = []

    # Procedural scenes use six frames; denser generated visuals use three. This
    # keeps the combined pack within the default 6.875 MiB benchmark partition.
    dataset_specs = [
        ("synthetic", ROOT / "assets" / "synthetic", 6),
        ("visual", ROOT / "assets" / "visual", 3),
    ]
    dataset_manifests = []
    for dataset_name, dataset_root, max_frames in dataset_specs:
        source_manifest = json.loads((dataset_root / "manifest.json").read_text(encoding="utf-8"))
        dataset_manifests.append((dataset_name, source_manifest, max_frames))
        descriptions = {entry["name"]: entry["description"] for entry in source_manifest["scenes"]}
        for scene_dir in sorted(path for path in dataset_root.iterdir() if path.is_dir()):
            paths, frames = load_frame_sequence(scene_dir, "frame_", max_frames)
            if frames:
                description = descriptions.get(scene_dir.name, "public benchmark scene")
                datasets.append((dataset_name, scene_dir.name, f"{dataset_name}: {description}", paths, frames))

    entries_meta = []
    blobs: list[bytes] = []

    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        for dataset_name, name, desc, paths, frames in datasets:
            h, w, _ = frames[0].shape
            n_frames = len(frames)
            raw_8888_bytes = w * h * 4 * n_frames
            raw_565_bytes = w * h * 2 * n_frames
            raw_565a8_bytes = w * h * 3 * n_frames

            # --- 8888 Modes (EAF4 container) ---
            eaf4_modes = [
                ("0x04_Lossless_8888_v1", "v1", "0x04 EAF Lossless (8888 v1)"),
                ("0x04_Lossless_8888_v2", "v2", "0x04 EAF Lossless (8888 v2)"),
                ("0x04_Lossless_8888_Fast", "tile_packed", "0x04 EAF Lossless (Tile-Packed)"),
                ("0x05_Bounded_8888", "btc8888", "0x05 EAF Bounded (8888 MaxErr<=8)"),
            ]
            if dataset_name == "visual":
                eaf4_modes = [eaf4_modes[1], eaf4_modes[3]]
            for codec_label, profile_arg, codec_id_str in eaf4_modes:
                blob, summary = encode_eaf4(frames, profile=profile_arg, btc_max_error=8)
                recon = decode_eaf4(blob)
                q = compute_quality_bgra(frames, recon)
                q["quality_method"] = "eaf4_reference_decode"
                q["quality_max_error"] = q["max_err"]
                entries_meta.append(
                    {
                        "asset": name,
                        "dataset": dataset_name,
                        "desc": desc,
                        "codec_key": codec_label,
                        "codec_title": codec_id_str,
                        "format": "BGRA8888",
                        "container_type": CONT_EAF4_8888,
                        "width": w,
                        "height": h,
                        "frames": n_frames,
                        "raw_bytes": raw_8888_bytes,
                        "compressed_bytes": len(blob),
                        "ratio_vs_8888": round(raw_8888_bytes / len(blob), 2),
                        "ratio_vs_native": round(raw_8888_bytes / len(blob), 2),
                        "submodes": summary["mode_counts_all"],
                        **q,
                    }
                )
                blobs.append(blob)

            # --- 565 Modes (0x89 EAF container) ---
            five_modes = [
                ("0x04_Lossless_565A8", "lossless", "0x04 EAF Lossless (565+A8)", raw_565a8_bytes),
                ("0x05_Bounded_565", "btc-safe", "0x05 EAF Bounded (565 bound=8)", raw_565_bytes),
            ]
            if w % 16 == 0 and h % 16 == 0:
                five_modes.append(("0x06_JPEG_Delta_565", "quad-jpeg", "0x06 EAF JPEG Delta (HW-Aligned)", raw_565_bytes))

            for codec_label, mode565, codec_id_str, native_raw in five_modes:
                out_eaf = tmp_dir / f"{name}_{codec_label}.eaf"
                convert_to_eaf(
                    [str(p) for p in paths],
                    str(out_eaf),
                    mode=mode565,
                    quality=82,
                    keyframe_interval=30,
                    jpeg_subsampling=2,
                    btc_max_error=8,
                )
                blob = out_eaf.read_bytes()
                recon = simulate_565_encoder_reconstruction(frames, mode565)
                q = compute_quality_bgra(frames, recon)
                q["quality_method"] = (
                    "jpeg_encoder_reconstruction_approximation"
                    if mode565 == "quad-jpeg"
                    else "encoder_reconstruction_reference"
                )
                q["quality_max_error"] = q["max_err_rgb"]
                entries_meta.append(
                    {
                        "asset": name,
                        "dataset": dataset_name,
                        "desc": desc,
                        "codec_key": codec_label,
                        "codec_title": codec_id_str,
                        "format": "RGB565A8" if mode565 == "lossless" else "RGB565",
                        "container_type": CONT_EAF_565,
                        "width": w,
                        "height": h,
                        "frames": n_frames,
                        "raw_bytes": raw_8888_bytes,
                        "native_raw_bytes": native_raw,
                        "compressed_bytes": len(blob),
                        "ratio_vs_8888": round(raw_8888_bytes / len(blob), 2),
                        "ratio_vs_native": round(native_raw / len(blob), 2),
                        **q,
                    }
                )
                blobs.append(blob)

    # 3. Legacy 0x00..0x03 Palette EAF sample tested in both 565 and native 8888 output modes
    for sample_name, w, h, n_f, desc in [
        ("transparent.eaf", 150, 150, 37, "Legacy 0x00 Palette RLE (150x150, 37f)"),
        ("smile_05s.eaf", 360, 360, 25, "Legacy 0x01/0x03 Palette Huff (360x360, 25f)"),
    ]:
        sp = ROOT / "assets" / "eaf_samples" / sample_name
        if sp.is_file():
            blob = sp.read_bytes()
            for fmt_label, ctype, bpp in [
                ("RGB565", CONT_EAF_565, 2),
                ("BGRA8888", CONT_EAF_8888_OUT, 4),
            ]:
                entries_meta.append(
                    {
                        "asset": sample_name.replace(".eaf", ""),
                        "desc": desc,
                        "codec_key": f"0x00_Palette_{fmt_label}",
                        "codec_title": f"0x00/0x01 PALETTE ({fmt_label} out)",
                        "format": fmt_label,
                        "container_type": ctype,
                        "width": w,
                        "height": h,
                        "frames": n_f,
                        "raw_bytes": w * h * 4 * n_f,
                        "native_raw_bytes": w * h * bpp * n_f,
                        "compressed_bytes": len(blob),
                        "ratio_vs_8888": round((w * h * 4 * n_f) / len(blob), 2),
                        "ratio_vs_native": round((w * h * bpp * n_f) / len(blob), 2),
                        "psnr_rgba_db": "-",
                        "psnr_rgb_db": "-",
                        "psnr_alpha_db": "-",
                        "max_err": "-",
                        "max_err_rgb": "-",
                        "quality_max_error": "-",
                        "quality_method": "unavailable_without_source_frames",
                        "diff_pixel_ratio": "-",
                    }
                )
                blobs.append(blob)

    # Pack into MBPK binary image:
    # Header (64B): "MBPK" (4s), version=1 (I), count (I), total_size (I), reserved (48s)
    # Entry Record (96B each):
    #   offset (I), size (I), container_type (I), width (H), height (H), frames (I),
    #   asset_name (32s), codec_key (32s), reserved (12s)
    count = len(blobs)
    data_start = (64 + count * 96 + 63) & ~63
    records_buf = bytearray()
    payload_buf = bytearray()

    for idx, (meta, blob) in enumerate(zip(entries_meta, blobs)):
        aligned_off = data_start + len(payload_buf)
        meta["pack_index"] = idx
        meta["pack_offset"] = aligned_off
        records_buf += struct.pack(
            "<IIIHHI32s32s12s",
            aligned_off,
            len(blob),
            meta["container_type"],
            meta["width"],
            meta["height"],
            meta["frames"],
            meta["asset"].encode("utf-8")[:31],
            meta["codec_key"].encode("utf-8")[:31],
            bytes(12),
        )
        payload_buf += blob
        pad = (64 - (len(payload_buf) % 64)) % 64
        if pad:
            payload_buf += bytes(pad)

    total_size = data_start + len(payload_buf)
    if total_size > 0x6E0000:
        raise ValueError(f"Pack size {total_size} exceeds 6.875MB partition limit!")

    header = struct.pack("<4sIII48s", b"MBPK", 1, count, total_size, bytes(48))
    pad_hdr = bytes(data_start - 64 - len(records_buf))
    pack_bytes = header + records_buf + pad_hdr + payload_buf

    pack_path = OUT_DIR / "s31_bench_pack.bin"
    pack_path.write_bytes(pack_bytes)
    manifest_path = OUT_DIR / "manifest.json"
    combined_hash = hashlib.sha256()
    for dataset_name, source_manifest, _max_frames in dataset_manifests:
        combined_hash.update(dataset_name.encode("utf-8"))
        combined_hash.update(bytes.fromhex(source_manifest["dataset_pixel_sha256"]))
    manifest = {
        "schema": 1,
        "dataset": "public-synthetic-and-visual-v1",
        "dataset_pixel_sha256": combined_hash.hexdigest(),
        "datasets": [
            {
                "name": dataset_name,
                "generator_version": source_manifest["generator_version"],
                "dataset_pixel_sha256": source_manifest["dataset_pixel_sha256"],
                "frames_per_scene_in_pack": max_frames,
            }
            for dataset_name, source_manifest, max_frames in dataset_manifests
        ],
        "pack_file": pack_path.name,
        "pack_bytes": total_size,
        "entries": entries_meta,
    }
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"Packed {count} benchmark items into {pack_path} ({total_size / (1024*1024):.2f} MiB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
