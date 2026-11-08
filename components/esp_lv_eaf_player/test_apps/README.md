# LVGL component test applications

These applications validate the GUI-independent core plus its LVGL 8 and LVGL 9
adapters. They are compatibility tests, not codec performance benchmarks.

Each application generates and embeds two tiny public fixtures at configure time:

- a 16×16 legacy Palette RLE EAF;
- a 16×16 EAF4 Lossless animation with continuous alpha.

The generator uses only the Python standard library. No customer data, repository-
external asset, or network download is required.

## Run

```bash
cd core                          # or lvgl8 / lvgl9
idf.py set-target esp32s3
idf.py build flash monitor
```

Choose “run all tests” from the Unity menu.

`core` contains no LVGL dependency and runs its checks immediately at boot.

## Acceptance

- both EAF and EAF4 sources load;
- reported frame count is positive;
- the virtual 240×240 display receives a flush;
- object creation/destruction stays within the configured memory-leak threshold.

For codec throughput, P95 latency, PPA and PSRAM measurements, use the separate workflow
in [docs/DEVICE_TESTING.md](../../../docs/DEVICE_TESTING.md).
