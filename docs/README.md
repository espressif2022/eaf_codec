# Documentation

Start here instead of reading every historical implementation note.

## User documentation

- [Codec guide](EAF_COMPRESSION_MODES_GUIDE.md): public names, content selection,
  color depth, Alpha and implementation details.
- [Format specification](FORMAT.md): EAF4 binary layout and frame submodes.
- [Compression matrix](COMPRESSION_SUMMARY.md): compact `0x00..0x07` capability table.

## Validation and performance

- [Testing guide](TESTING.md): canonical Host and device commands and acceptance rules.
- [Host quick baseline](HOST_QUICK_BENCHMARK.md): reproducible public smoke data.
- [Device testing](DEVICE_TESTING.md): LVGL tests and the validated ESP32-S3 SUB2 480×480 workflow.
- [Device benchmark reports](BENCHMARK_REPORT.md): public hardware baselines.
- [ESP32-S3 480×480 result](ESP32S3_480X480_BENCHMARK.md): current real-board measurements.

## Naming

The public production names are **EAF Lossless**, **EAF Bounded**,
**EAF JPEG Delta** and **EAF Video**. Protocol IDs and implementation enums remain
`0x04 LOSSLESS`, `0x05 BTC_SAFE`, `0x06 QUAD_JPEG` and `0x07 H264` for binary
compatibility. Old CLI spellings remain aliases and should not be used in new examples.

These names describe the stable user-visible promise instead of the current implementation:

- **Lossless**: exact reconstruction is the defining property across 565+A8 and BGRA8888.
- **Bounded**: the useful guarantee is a configured maximum error; “BTC” is only one block implementation, and “Safe” does not explain the guarantee.
- **JPEG Delta**: only changed regions are carried as JPEG data, without exposing the internal quadtree layout.
- **Video**: selects the inter-frame path while allowing its underlying bitstream implementation to evolve.

For that reason, protocol enums remain technical and stable while new UI, CLI and
documentation use the capability names.
