# ESP32-S3-LCD-EV-Board-SUB2 480x480 benchmark

Dataset: `public-generated-480x480-v1`. Pack size: `4187.0 KiB`. Entries: `10`.
Dataset pixel SHA-256: `4c9418a28fd69374ef6da5e6a0337d0dc09a4eac6926f3f63f2b7427a848ab76`.
Board: `ESP32-S3-LCD-EV-Board-SUB2`. Device: `esp32s3` rev `1`, `2` cores, CPU `240 MHz`, ESP-IDF `v6.1-dev-5824-gfa8039b5cad-dirt`, optimization `performance`.
Sampling: `20` timed frames per entry after warm-up.

> Generated from public procedural sources or deterministic derivatives of checked-in generated masters. Host timings and historical
> non-public results are not mixed into this table.

## Results

| Scene | Codec | Format | Size KiB | Ratio | Decode avg/P95/max ms | Decode FPS | Pipeline ms/FPS | SRAM/PSRAM KiB | PSNR* | MaxErr* |
| :--- | :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `landscape_pan_480` | `0x04 EAF Lossless (8888 v2)` | `BGRA8888` | 1159.2 | 1.55× | 309.03/353.27/353.51 | 3.2 | 364.50/2.7 | 8.3/3082.0 | Inf | 0 |
| `landscape_pan_480` | `0x05 EAF Bounded (8888 MaxErr<=8)` | `BGRA8888` | 663.5 | 2.71× | 176.73/180.33/180.78 | 5.7 | 232.30/4.3 | 8.3/2129.8 | 42.97 | 8 |
| `landscape_pan_480` | `0x06 EAF JPEG Delta (software decode on S3)` | `RGB565` | 110.0 | 16.36× | 74.88/87.61/87.74 | 13.4 | 74.88/13.4 | 0.0/676.1 | 30.54 | 128 |
| `landscape_water_480` | `0x04 EAF Lossless (8888 v2)` | `BGRA8888` | 838.3 | 2.15× | 197.07/264.79/264.90 | 5.1 | 252.58/4.0 | 8.3/2332.8 | Inf | 0 |
| `landscape_water_480` | `0x05 EAF Bounded (8888 MaxErr<=8)` | `BGRA8888` | 471.0 | 3.82× | 126.10/172.59/172.84 | 7.9 | 181.64/5.5 | 8.3/2085.6 | 42.81 | 8 |
| `landscape_water_480` | `0x06 EAF JPEG Delta (software decode on S3)` | `RGB565` | 79.3 | 22.70× | 49.10/60.89/61.10 | 20.4 | 49.10/20.4 | 0.0/676.1 | 30.61 | 93 |
| `robot_alpha_480` | `0x04 EAF Lossless (8888 v2)` | `BGRA8888` | 345.3 | 5.21× | 111.32/112.92/113.24 | 9.0 | 166.80/6.0 | 8.3/1525.9 | Inf | 0 |
| `robot_alpha_480` | `0x05 EAF Bounded (8888 MaxErr<=8)` | `BGRA8888` | 196.9 | 9.14× | 67.20/77.10/77.27 | 14.9 | 122.81/8.1 | 8.3/1631.8 | 50.19 | 8 |
| `robot_alpha_480` | `0x04 EAF Lossless (565+A8)` | `RGB565A8` | 228.3 | 7.88× | 87.62/104.62/104.63 | 11.4 | 87.62/11.4 | 0.0/1364.1 | 35.13 | 7 |
| `robot_alpha_480` | `0x05 EAF Bounded (565 bound=8)` | `RGB565` | 93.8 | 19.20× | 47.73/61.63/61.72 | 21.0 | 47.73/21.0 | 0.0/676.1 | 7.29 | 15 |

## Acceptance

- Device suite: **PASS** (`10/10` results).
- No `BENCH_ERROR` records.
- SUB2 LCD initialized and received **10/10** decoded preview frames; the final green border marks PASS.
- Quality metrics are encoder-side source comparisons calculated by the public packer; device CRC and timings come from the flashed decoder.
- `*` EAF4 quality uses reference decode. RGB565 includes RGB888→RGB565 quantization; JPEG Delta is an encoder-side approximation.
- Decode FPS is codec throughput, not measured LCD refresh FPS. RGB scanout stays active, but preview draw calls run outside timed samples. For EAF4 on ESP32-S3, Pipeline includes CPU Alpha blend and BGRA8888→RGB565 conversion. Opaque RGB565 is display-ready; the RGB565+A8 row measures decode only and still needs Alpha composition in an actual renderer.

Reproduce with [DEVICE_TESTING.md](DEVICE_TESTING.md).
