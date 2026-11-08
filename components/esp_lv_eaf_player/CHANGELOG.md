# ChangeLog

## Unreleased

* Added per-player classic EAF output selection through
  `esp_eaf_player_config_t.output_format`. Direct BGRA8888 output is available
  for compatible codecs without introducing RGB565-to-BGRA8888 expansion.
* Reject reserved RGB888 and unsupported codec/output combinations explicitly.

## v0.4.0 (2026-11-08)

* Added the public, GUI-independent `esp_eaf_player.h` API.
* Made LVGL an optional adapter and removed it from transitive component
  dependencies; LVGL applications now declare `lvgl/lvgl` directly.
* Added a codec ops registry and moved Lossless, Bounded, JPEG Delta and H.264
  decoding into independent source files.
* Routed both legacy EAF/AAF and EAF4 through the same container-aware codec
  registry. EAF4 container parsing is now separate from Lossless/Bounded pixel
  reconstruction.
* Split parser, decoder runtime, codec and GUI adapter responsibilities into
  explicit directories; removed the old monolithic legacy decoder module.
* Unified the RGB565 and BGRA8888 Bounded implementations behind one callback
  and one 4x4 tile loop while retaining their distinct wire-payload adapters.
* Removed the ineffective RGB565-to-BGRA8888 expansion path. Codecs now reject
  output formats they cannot produce directly instead of silently up-converting.
* Kept all existing `lv_eaf_*` APIs source-compatible.

* Converged the production encoding table to contiguous IDs `0x00..0x07`.
  Lossless-RGBA is now codec `0x04`; the decoder still accepts legacy bytecode
  `0x0C` for existing assets.
* Added bit-exact RGB565 + A8 lossless coding with 4x4 change masks,
  joint-symbol inter frames and ROM inflate.
* Added per-stream decoder contexts (`esp_eaf_decoder_create()`, `esp_eaf_block_decode_ctx()`,
  `esp_eaf_decoder_destroy()`) and an optional internal-RAM scratch buffer.
* Added `esp_eaf_get_dirty_bands_ctx()`: up to `ESP_EAF_MAX_DIRTY_BANDS` dirty rectangles per frame.
* Added bounds checks in the frame and block parsers.
* Made LVGL 8/9 tests self-contained with locally generated public fixtures.
* Let EAF4 decoding fall back to internal RAM when PSRAM is unavailable.
* Allowed playback delay and loop settings to be configured and queried before
  loading a source.
* Documented codec selection, alpha support, dependencies, memory ownership and
  legacy compatibility in the component README.

## v0.3.0 (2026-05-06)

* Added ESP32-S31 support.

## v0.2.0

* Add EAF transparency support.

## v0.1.0

* Implement the EAF LVGL decoder.
