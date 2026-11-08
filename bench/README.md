# Benchmark tooling

| Script | Role |
| :--- | :--- |
| `generate_synthetic_assets.py` | Deterministic RGBA8 source generation; no external assets |
| `generate_visual_assets.py` | Derive character and photorealistic animations from checked-in generated masters |
| `generate_test_sample.py` | Rebuild the small EAF4 file embedded by LVGL tests |
| `quick_host_bench.py` | Fast EAF Lossless/Bounded Host acceptance and JSON output |
| `run_matrix.py` | Full EAF4 submode size, quality and Host C timing matrix |
| `compare_modes.py` | Ad-hoc comparison for a supplied PNG sequence |
| `pack_s31_bench.py` | Public procedural/visual/legacy MBPK pack for device runs |
| `pack_esp32s3_480x480.py` | Full-screen visual pack for ESP32-S3 SUB2 480×480 boards |
| `capture_device_bench.py` | Reset a serial target and capture a complete benchmark run |
| `generate_report.py` | Merge the MBPK manifest with captured device serial results |
| `s31_firmware/` | Cross-target runner; ESP32-S3 uses CPU RGB565 postprocess, P4/S31 uses PPA when available |

Canonical commands and acceptance criteria live in [docs/TESTING.md](../docs/TESTING.md).
Benchmark outputs belong under ignored `build/`; do not commit generated packs or logs.
