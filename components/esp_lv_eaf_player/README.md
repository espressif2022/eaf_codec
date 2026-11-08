[![Component Registry](https://components.espressif.com/components/espressif/esp_lv_eaf_player/badge.svg)](https://components.espressif.com/components/espressif/esp_lv_eaf_player)

# EAF Player Core and LVGL Adapter

`esp_lv_eaf_player` is an ESP-IDF component that decodes legacy EAF/AAF and EAF4
animations through a GUI-independent player API. An optional adapter exposes the
same player as an LVGL 8 or LVGL 9 image widget.

## Architecture

The component has four independent layers:

1. `esp_eaf_player.c` owns stream state, frame buffers, seeking and container
   selection. Its public API is `esp_eaf_player.h` and contains no LVGL types.
2. Container readers under `container/eaf` and `container/eaf4` validate
   headers and indexes, then submit a normalized request. They contain no pixel
   reconstruction code.
3. All EAF/AAF modes `0x00..0x07` and all EAF4 frame sub-modes are selected by
   `esp_eaf_codec_registry.c`. Bounded uses one callback and one tile loop for
   both containers; only its two wire-payload profiles differ. Other genuinely
   different payload implementations remain under `codec/eaf` or `codec/eaf4`.
4. `lv_eaf.c` adapts decoded frames to an LVGL image descriptor and handles only
   timers, events and invalidation.

The implementation mirrors those boundaries on disk: `src/core`,
`src/container/{eaf,eaf4}`, `src/codec/{common,eaf,eaf4}`, and
`src/adapter/lvgl`. Public headers remain flat under `include/` so existing
include statements and the published API stay compatible.

This boundary lets another GUI consume `esp_eaf_frame_t.pixels`, optional A8
alpha, stride and dirty rectangle without including or linking LVGL.

## Requirements

- ESP-IDF 5.5 or newer;
- LVGL 8.x or 9.x only when using `lv_eaf.h`;
- PSRAM is recommended for large animations, but small assets can fall back to
  internal RAM;
- software JPEG decoding is enabled by default. ESP32-P4 and ESP32-S31 use the
  hardware JPEG decoder when available.

## Codec selection

| ID | Public name | Alpha | Recommended use |
|---:|---|---|---|
| `0x00`, `0x01`, `0x03` | Palette RLE/Huffman | Palette A8 | Existing assets and low-color icons |
| `0x02` | JPEG Keyframe | No | Internal keyframes and compatibility |
| `0x04` | EAF Lossless | Full A8 | Exact UI, shadows, and translucent animation |
| `0x05` | EAF Bounded | Full A8 in EAF4 | UI/cartoon animation when a bounded error is acceptable |
| `0x06` | EAF JPEG Delta | No | Photos, scenery, gradients, and opaque animation |
| `0x07` | EAF Video | No | Long opaque full-screen sequences |

The decoder also accepts historical pre-convergence bytecodes `0x08`, `0x09`,
and `0x0C`. Existing legacy EAF/AAF assets do not need to be re-encoded.

H.264 support is optional. Add `espressif/esp_h264` to the application manifest
when codec `0x07` is required.

## Add to a project

```bash
idf.py add-dependency "espressif/esp_lv_eaf_player:*"
```

Or add it to the application's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lv_eaf_player: "*"
```

LVGL is intentionally not a transitive dependency. An LVGL application also
adds `lvgl/lvgl: ">=8,<10"`; a non-LVGL application does not.

## Use from another GUI

```c
#include "esp_eaf_player.h"

esp_eaf_player_t *player = NULL;
esp_eaf_player_config_t config = ESP_EAF_PLAYER_CONFIG_DEFAULT();

/* Optional for classic EAF: only codecs with a direct BGRA8888 path accept it. */
config.output_format = ESP_EAF_OUT_BGRA8888;

ESP_ERROR_CHECK(esp_eaf_player_open_memory(eaf_data, eaf_size, &config, &player));
const esp_eaf_stream_info_t *info = esp_eaf_player_get_info(player);
const esp_eaf_frame_t *frame = esp_eaf_player_get_frame(player);

/* Submit frame->pixels, frame->stride and frame->alpha to your GUI. */
ESP_ERROR_CHECK(esp_eaf_player_next(player));
esp_eaf_player_close(player);
```

The input buffer is borrowed until `esp_eaf_player_close()`. A decoded frame is
owned by the player and remains valid until the next seek/next call or close.

`output_format` is a per-player runtime choice, not a Kconfig option. Classic
EAF defaults to RGB565 for compatibility. Its 8-bit palette, 24-bit BGR and
H.264 codecs can write BGRA8888 directly; codecs backed by RGB565 payloads or
RGB565-only JPEG return `ESP_ERR_NOT_SUPPORTED`. The player never expands a
decoded RGB565 frame into BGRA8888. EAF4 always exposes its native BGRA8888
pixels. `ESP_EAF_OUT_RGB888` remains reserved and is rejected.

## Load an embedded asset

The data descriptor must contain the complete EAF/EAF4 file, not decoded pixels.

```c
#include "lv_eaf.h"

extern const lv_image_dsc_t my_animation_eaf;

#if LVGL_VERSION_MAJOR >= 9
lv_obj_t *screen = lv_screen_active();
#else
lv_obj_t *screen = lv_scr_act();
#endif

lv_obj_t *animation = lv_eaf_create(screen);
lv_eaf_set_frame_delay(animation, 50);  // milliseconds; set before or after load
lv_eaf_set_loop_count(animation, -1);   // infinite
lv_eaf_set_src(animation, &my_animation_eaf);
lv_obj_center(animation);

if (!lv_eaf_is_loaded(animation)) {
    // Reject the asset or show a fallback image.
}
```

To load an existing memory buffer without wrapping it in an LVGL descriptor:

```c
lv_eaf_set_src_data(animation, eaf_bytes, eaf_size);
```

The buffer passed to `lv_eaf_set_src_data()` is borrowed and must remain valid
until another source is selected or the widget is deleted.

## Load from LVGL FS

```c
lv_eaf_set_src(animation, "E:/animations/intro.eaf");
```

Mount the LVGL drive before loading the source. Data loaded from a file is owned
by the widget and released when the source changes or the widget is deleted.

## Playback controls

```c
lv_eaf_pause(animation);
lv_eaf_resume(animation);
lv_eaf_restart(animation);
lv_eaf_set_loop_enabled(animation, true);
lv_eaf_set_loop_count(animation, 2);
```

The default frame delay is `0`, which advances whenever LVGL services the player
timer. Set a positive delay for paced playback.

For RGB565 assets without continuous alpha, select a one-bit transparent color
with `lv_eaf_set_color_key()` before loading the source. Pass `-1` to disable it.

## Configuration

`Component config > ESP LV EAF Player > Enable Software JPEG Decoding` controls
`CONFIG_ESP_LV_EAF_ENABLE_SW_JPEG`. Disable it only if every deployed asset is
free of JPEG and JPEG Delta blocks. Disabling it saves decoder working memory,
but codec `0x02`/`0x06` assets will fail on targets without a hardware decoder.

## Public headers

- `lv_eaf.h`: supported LVGL widget API;
- `esp_eaf_player.h`: GUI-independent player, stream metadata and frame views;
- `esp_eaf_codec.h`: container probing, codec IDs, and output format types;
- `eaf4.h`: compatibility API for direct EAF4 container access. New GUI
  integrations should prefer `esp_eaf_player.h`.

GUI adapters should normally use `esp_eaf_player.h`; LVGL applications may use
only `lv_eaf.h`. Structures and codec hooks under `src/` are private and may
evolve without an API compatibility guarantee.

## Asset conversion

The online [EAF converter](https://esp32-gif.espressif.com/) can generate legacy
EAF assets. Use the encoder shipped with the source repository when selecting
the newer Lossless, Bounded, JPEG Delta, or Video modes.

## Tests

`test_apps/lvgl8` and `test_apps/lvgl9` are self-contained ESP-IDF Unity test
projects. Their tiny legacy EAF and EAF4 fixtures are generated locally during
CMake configuration; no customer data or network download is used.

See [test_apps/README.md](test_apps/README.md) for commands and acceptance criteria.

## License

Apache-2.0. See [LICENSE](LICENSE).
