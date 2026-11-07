## Unreleased (2026-10-27)

* Added encoding 0x0C (Lossless-RGBA): bit-exact RGB565 + A8 with 4x4 change masks,
  joint-symbol inter frames and ROM inflate. See docs/EAF_0x0C_FORMAT.md in the demo.
* Added per-stream decoder contexts (`esp_eaf_decoder_create()`, `esp_eaf_block_decode_ctx()`,
  `esp_eaf_decoder_destroy()`) and an optional internal-RAM scratch buffer.
* Added `esp_eaf_get_dirty_bands_ctx()`: up to `ESP_EAF_MAX_DIRTY_BANDS` dirty rectangles per frame.
* Added bounds checks in the frame and block parsers.

# ChangeLog

## v0.3.0 (2026-05-06)

* Added ESP32-S31 support.

## v0.2.0

* Add EAF transparency support.

## v0.1.0

* Implement the EAF LVGL decoder.
