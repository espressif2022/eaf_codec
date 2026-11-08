# Device benchmark reports

The reproducible public hardware baseline is available for:

- [ESP32-S3-LCD-EV-Board-SUB2, 480×480](ESP32S3_480X480_BENCHMARK.md):
  10/10 codec entries and 10/10 LCD preview updates passed at 240 MHz with
  8 MB octal PSRAM at 80 MHz.

Historical non-public measurements were intentionally removed because their source assets
cannot be redistributed or independently reproduced. They must not be used as the public
performance baseline for this repository.

## Reproducible inputs

- source scenes: `assets/synthetic` and `assets/visual`;
- generators: `bench/generate_synthetic_assets.py` and `bench/generate_visual_assets.py`, version 1;
- ESP32-S3 SUB2 480×480 packer: `bench/pack_esp32s3_480x480.py`;
- general device packer: `bench/pack_s31_bench.py`;
- firmware: `bench/s31_firmware`;
- Host reference: [HOST_QUICK_BENCHMARK.md](HOST_QUICK_BENCHMARK.md).

## Produce this report

Follow [DEVICE_TESTING.md](DEVICE_TESTING.md) to create and flash the public MBPK pack,
capture the serial log, and run:

```bash
make device-s3-report
```

The generated table includes compressed size, ratio, average/P95/max decode latency,
decode FPS, display-ready pipeline time, encoder-side PSNR/MaxErr references and measured
SRAM/PSRAM usage. The firmware records target revision, CPU clock, ESP-IDF version and
compiler optimization mode; board/module and PSRAM clock must be added to the run notes.
RGB565 quality values include RGB888-to-RGB565 quantization; JPEG Delta quality is marked
as an encoder-side approximation, while device timing and CRC come from the final blob.
