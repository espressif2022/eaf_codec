#!/usr/bin/env python3
"""Merge a public device-pack manifest and S31/P4 serial log into Markdown."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "build" / "s31_bench_pack" / "manifest.json"
DEFAULT_LOG = ROOT / "build" / "s31_bench_pack" / "s31_hardware.log"
DEFAULT_REPORT = ROOT / "docs" / "BENCHMARK_REPORT.md"

RESULT_RE = re.compile(
    r"BENCH_RESULT idx=(\d+) asset=(\S+) codec=(\S+) fmt=(\S+) w=(\d+) h=(\d+) frames=(\d+) "
    r"comp_bytes=(\d+) sram_B=(\d+) psram_B=(\d+) "
    r"dec_avg_us=(\d+) dec_p95_us=(\d+) dec_max_us=(\d+) inf_avg_us=(\d+) rec_avg_us=(\d+) "
    r"(?:ppa|post)_avg_us=(\d+) pipe_avg_us=(\d+) pipe_p95_us=(\d+) crc=([0-9a-fA-F]+)"
)
ENV_RE = re.compile(
    r"(?:S31|EAF)_BENCH_ENV idf=(\S+) target=(\S+) revision=(\d+) cores=(\d+) "
    r"cpu_mhz=(\d+) optimization=(\S+)"
)


def parse_results(log_text: str) -> dict[int, dict]:
    results = {}
    for line in log_text.splitlines():
        match = RESULT_RE.search(line)
        if not match:
            continue
        values = match.groups()
        idx = int(values[0])
        if idx in results:
            raise ValueError(f"duplicate BENCH_RESULT index: {idx}")
        results[idx] = {
            "asset_log": values[1],
            "codec_log": values[2],
            "format_log": values[3],
            "width_log": int(values[4]),
            "height_log": int(values[5]),
            "frames_log": int(values[6]),
            "compressed_bytes_log": int(values[7]),
            "sram_bytes": int(values[8]),
            "psram_bytes": int(values[9]),
            "dec_avg_us": int(values[10]),
            "dec_p95_us": int(values[11]),
            "dec_max_us": int(values[12]),
            "inflate_avg_us": int(values[13]),
            "reconstruct_avg_us": int(values[14]),
            "postprocess_avg_us": int(values[15]),
            "pipeline_avg_us": int(values[16]),
            "pipeline_p95_us": int(values[17]),
            "board_crc": values[18].lower(),
        }
    return results


def render(manifest: dict, log_text: str) -> str:
    entries = manifest["entries"]
    results = parse_results(log_text)
    errors = [line for line in log_text.splitlines() if "BENCH_ERROR" in line]
    suite_ok = bool(re.search(r"(?:S31|EAF)_BENCH_SUITE_END items=\d+ status=0", log_text))
    display_ready = bool(re.search(
        r"EAF_DISPLAY_READY panel=480x480 sub_board=SUB2 driver=GC9503CV", log_text
    ))
    display_frames = len(re.findall(r"EAF_DISPLAY_FRAME idx=\d+", log_text))
    missing = [entry["pack_index"] for entry in entries if entry["pack_index"] not in results]
    mismatched = []
    for entry in entries:
        result = results.get(entry["pack_index"])
        if result is None:
            continue
        expected = (
            entry["asset"][:31],
            entry["codec_key"][:31],
            entry["width"],
            entry["height"],
            entry["frames"],
            entry["compressed_bytes"],
        )
        actual = (
            result["asset_log"],
            result["codec_log"],
            result["width_log"],
            result["height_log"],
            result["frames_log"],
            result["compressed_bytes_log"],
        )
        if actual != expected:
            mismatched.append(entry["pack_index"])
    env_match = ENV_RE.search(log_text)
    if errors or not suite_ok or missing or mismatched or not env_match or not display_ready or display_frames != len(entries):
        details = []
        if errors:
            details.append(f"device errors: {len(errors)}")
        if not suite_ok:
            details.append("missing successful suite terminator")
        if missing:
            details.append(f"missing result indices: {missing}")
        if mismatched:
            details.append(f"manifest/log mismatch indices: {mismatched}")
        if not env_match:
            details.append("missing EAF_BENCH_ENV metadata")
        if not display_ready:
            details.append("missing SUB2 display-ready marker")
        if display_frames != len(entries):
            details.append(f"displayed frames: {display_frames}/{len(entries)}")
        raise ValueError("invalid hardware log: " + "; ".join(details))

    idf, target, revision, cores, cpu_mhz, optimization = env_match.groups()

    board = manifest.get("board", target)
    resolution = manifest.get("resolution")
    title = f"{board} {resolution} benchmark" if resolution else "Public device benchmark report"
    lines = [
        f"# {title}",
        "",
        f"Dataset: `{manifest.get('dataset', 'unknown')}`. Pack size: "
        f"`{manifest.get('pack_bytes', 0) / 1024:.1f} KiB`. Entries: `{len(entries)}`.",
        f"Dataset pixel SHA-256: `{manifest.get('dataset_pixel_sha256', 'unknown')}`.",
        f"Board: `{board}`. Device: `{target}` rev `{revision}`, `{cores}` cores, CPU `{cpu_mhz} MHz`, "
        f"ESP-IDF `{idf}`, optimization `{optimization}`.",
        f"Sampling: `{manifest.get('samples_per_entry', 'unknown')}` timed frames per entry after warm-up.",
        "",
        "> Generated from public procedural sources or deterministic derivatives of checked-in generated masters. Host timings and historical",
        "> non-public results are not mixed into this table.",
        "",
        "## Results",
        "",
        "| Scene | Codec | Format | Size KiB | Ratio | Decode avg/P95/max ms | Decode FPS | Pipeline ms/FPS | SRAM/PSRAM KiB | PSNR* | MaxErr* |",
        "| :--- | :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for entry in entries:
        result = results[entry["pack_index"]]
        dec_avg = result["dec_avg_us"] / 1000.0
        dec_p95 = result["dec_p95_us"] / 1000.0
        dec_max = result["dec_max_us"] / 1000.0
        pipe_avg = result["pipeline_avg_us"] / 1000.0
        fps = 1_000_000.0 / max(1, result["dec_avg_us"])
        pipe_fps = 1_000_000.0 / max(1, result["pipeline_avg_us"])
        psnr = entry.get("psnr_rgba_db", "-")
        max_err = entry.get("quality_max_error", "-")
        lines.append(
            f"| `{entry['asset']}` | `{entry['codec_title']}` | `{entry['format']}` | "
            f"{entry['compressed_bytes']/1024:.1f} | {entry['ratio_vs_8888']:.2f}× | "
            f"{dec_avg:.2f}/{dec_p95:.2f}/{dec_max:.2f} | {fps:.1f} | {pipe_avg:.2f}/{pipe_fps:.1f} | "
            f"{result['sram_bytes']/1024:.1f}/{result['psram_bytes']/1024:.1f} | {psnr} | {max_err} |"
        )
    lines.extend(
        [
            "",
            "## Acceptance",
            "",
            f"- Device suite: **PASS** (`{len(results)}/{len(entries)}` results).",
            "- No `BENCH_ERROR` records.",
            f"- SUB2 LCD initialized and received **{display_frames}/{len(entries)}** decoded preview frames; the final green border marks PASS.",
            "- Quality metrics are encoder-side source comparisons calculated by the public packer; device CRC and timings come from the flashed decoder.",
            "- `*` EAF4 quality uses reference decode. RGB565 includes RGB888→RGB565 quantization; JPEG Delta is an encoder-side approximation.",
            "- Decode FPS is codec throughput, not measured LCD refresh FPS. RGB scanout stays active, but preview draw calls run outside timed samples. For EAF4 on ESP32-S3, Pipeline includes CPU Alpha blend and BGRA8888→RGB565 conversion. Opaque RGB565 is display-ready; the RGB565+A8 row measures decode only and still needs Alpha composition in an actual renderer.",
            "",
            "Reproduce with [DEVICE_TESTING.md](DEVICE_TESTING.md).",
        ]
    )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_REPORT)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    report = render(manifest, args.log.read_text(encoding="utf-8", errors="replace"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8")
    print(f"Generated {args.output} with {len(manifest['entries'])} public device results")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
