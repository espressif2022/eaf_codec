/* Internal codec ABI shared by container readers and codec implementations. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_eaf_codec.h"

#ifdef ESP_PLATFORM
#include "esp_eaf_internal.h"
#include "esp_lv_eaf_player_config.h"
#include "zlib.h"
#if ESP_LV_EAF_ENABLE_H264
#include "esp_h264_dec.h"
#endif

struct esp_eaf_decoder {
    esp_eaf_dirty_rect_t dirty;
    esp_eaf_dirty_rect_t dirty_bands[ESP_EAF_MAX_DIRTY_BANDS + 1];
    uint8_t dirty_band_count;
    uint8_t jpeg_hdr[640];
    size_t jpeg_hdr_len;
    int sof0_offset;
    uint8_t *predictor_scratch;
    size_t predictor_capacity;
    uint8_t *lossless_int_scratch;
    size_t lossless_int_capacity;
    bool lossless_int_tried;
    int predictor_width, predictor_height;
    bool predictor_ready, predictor_swap;
    bool predictor_rom, predictor_z_ready;
    z_stream predictor_z;
    esp_eaf_planar_stats_t predictor_stats;
    const uint16_t *fused_bg_rgb565;
    uint16_t *fused_comp_rgb565;
    esp_eaf_output_format_t out_fmt;
#if ESP_LV_EAF_ENABLE_H264
    esp_h264_dec_handle_t h264;
#endif
};
#endif

typedef struct {
    esp_eaf_container_t container;
#ifdef ESP_PLATFORM
    esp_eaf_decoder_t *decoder;
    const esp_eaf_header_t *header;
    const uint8_t *block_data;
    size_t block_len;
    int block_index;
    int width;
    int block_height;
    uint8_t *decode_buffer;
    uint8_t *alpha_buffer;
    bool swap_color;
    esp_eaf_palette_cache_t *palette_cache;
#endif
    uint8_t mode;
    uint8_t *frame;
    const uint8_t *scratch;
    uint32_t decoded_size;
    uint32_t frame_bytes;
    uint16_t frame_width;
    uint16_t frame_height;
} esp_eaf_codec_request_t;

typedef int (*esp_eaf_codec_decode_cb_t)(const esp_eaf_codec_request_t *request);

typedef struct {
    esp_eaf_container_t container;
    uint8_t mode;
    const char *name;
    esp_eaf_codec_decode_cb_t decode;
} esp_eaf_codec_ops_t;

const esp_eaf_codec_ops_t *esp_eaf_codec_find(esp_eaf_container_t container, uint8_t mode);

int esp_eaf_codec_eaf4_lossless_decode(const esp_eaf_codec_request_t *request);
int esp_eaf_codec_bounded_decode(const esp_eaf_codec_request_t *request);

#ifdef ESP_PLATFORM
esp_err_t esp_eaf_codec_palette_rle_decode(const esp_eaf_codec_request_t *request);
esp_err_t esp_eaf_codec_palette_huff_rle_decode(const esp_eaf_codec_request_t *request);
esp_err_t esp_eaf_codec_jpeg_keyframe_decode(const esp_eaf_codec_request_t *request);
esp_err_t esp_eaf_codec_palette_huff_decode(const esp_eaf_codec_request_t *request);
esp_err_t esp_eaf_codec_eaf_lossless_decode(const esp_eaf_codec_request_t *request);
esp_err_t esp_eaf_codec_jpeg_delta_decode(const esp_eaf_codec_request_t *request);
esp_err_t esp_eaf_codec_video_decode(const esp_eaf_codec_request_t *request);

void *esp_eaf_malloc_prefer_psram(size_t size);
void *esp_eaf_malloc_aligned_prefer_psram(size_t size, size_t alignment, size_t *allocated_size);
void esp_eaf_decoder_emit_tile_dirty_bands(esp_eaf_decoder_t *decoder, const uint8_t *mask,
                                           int tiles_x, int tiles_y, uint16_t y_offset);
esp_eaf_output_format_t esp_eaf_decoder_get_output_format(const esp_eaf_decoder_t *decoder);
void esp_eaf_decoder_mark_dirty(esp_eaf_decoder_t *decoder, uint16_t x1, uint16_t y1,
                                uint16_t x2, uint16_t y2);
void esp_eaf_decoder_cache_jpeg_tables(esp_eaf_decoder_t *decoder,
                                       const uint8_t *jpeg, size_t size);

#if ESP_LV_EAF_ENABLE_H264
void esp_eaf_i420_to_bgra8888(const uint8_t *i420, int width, int height, uint32_t *bgra);
void esp_eaf_i420_to_rgb565(const uint8_t *i420, int width, int height,
                            uint16_t *rgb565, bool swap_color);
#endif
#endif
