# Device testing

There are two separate device workflows. Do not mix component compatibility with codec throughput.

## 1. LVGL component compatibility tests

The applications under `components/esp_lv_eaf_player/test_apps` verify LVGL 8 and
LVGL 9 integration, object lifecycle, redraw and memory leaks. They embed two repository
assets and do not download anything during CMake configure:

- legacy palette EAF (`Happy.eaf`);
- EAF4 Lossless (`synthetic_lossless.eaf4`).

```bash
cd components/esp_lv_eaf_player/test_apps/lvgl9   # or lvgl8
idf.py set-target esp32s3
idf.py build flash monitor
```

From the Unity menu, run all tests. Acceptance requires both assets to load, at least one
display flush, a positive frame count and no leak above the configured threshold.

## 2. ESP32-S3-LCD-EV-Board-SUB2 480×480 benchmark

This workflow measures full-screen codec throughput and memory on the 480×480 SUB2 panel.
It uses the official BSP 4.1.0 raw-panel API (without LVGL). The pack is deterministically
derived from the public robot and mountain-lake masters and covers EAF Lossless,
EAF Bounded and EAF JPEG Delta.
EAF Video requires the target video component and is outside this firmware.

Two timing scopes are reported:

- `Decode`: codec work only;
- `Pipeline`: for EAF4, decode plus CPU Alpha blend and BGRA8888→RGB565 conversion;
  opaque RGB565 is already display-ready. RGB565+A8 is decode-only in this runner and
  still needs Alpha composition in an actual renderer.

Pipeline FPS is not LCD refresh FPS. The RGB scanout remains active, so PSRAM bandwidth
pressure is representative of an attached display, but each `esp_lcd_panel_draw_bitmap()`
call runs after the timed samples. The screen starts with a color pattern, shows the last
decoded frame after each item, and finishes with a green PASS or red FAIL border.

### Build the pack

```bash
make device-s3-pack
```

Outputs:

- `build/esp32s3_480x480/esp32s3_480x480.bin`;
- `build/esp32s3_480x480/manifest.json`.

### Build and flash the firmware

```bash
cd bench/s31_firmware
idf.py -B build-s3-480 \
  -DSDKCONFIG=sdkconfig.s3_480 \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults build
idf.py -B build-s3-480 -p PORT -b 1500000 flash
python -m esptool --chip esp32s3 --port PORT -b 1500000 \
  write-flash --flash-size 16MB 0x110000 \
  ../../build/esp32s3_480x480/esp32s3_480x480.bin
```

From the repository root, reset the target and capture one complete run:

```bash
python bench/capture_device_bench.py --port PORT \
  -o build/esp32s3_480x480/esp32s3_hardware.log
```

Then generate the public report:

```bash
make device-s3-report
```

The checked-in result is [ESP32S3_480X480_BENCHMARK.md](ESP32S3_480X480_BENCHMARK.md).

### Device acceptance

- no `BENCH_ERROR` line;
- final `EAF_BENCH_SUITE_END ... status=0`;
- `EAF_DISPLAY_READY panel=480x480 sub_board=SUB2 driver=GC9503CV` and visible LCD output;
- every manifest entry has one `BENCH_RESULT`;
- EAF Lossless CRC remains stable across repeated runs;
- EAF4 Bounded quality is checked by reference decode (`MaxErr <= 8` across BGRA);
- RGB565 Bounded enforces the bound against the quantized RGB565 source; a report
  comparing against original RGB888 also includes normal 5/6-bit quantization error;
- average, P95 and maximum decode time plus SRAM/PSRAM usage are recorded.

The firmware emits `EAF_BENCH_ENV` with target revision, CPU clock, ESP-IDF version,
optimization, input path, output format and panel resolution. The boot log must also
show 8 MB octal PSRAM at 80 MHz; hardware numbers without this metadata are not comparable.
