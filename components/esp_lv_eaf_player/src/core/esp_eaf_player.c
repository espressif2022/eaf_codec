/* Core player implementation.
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_eaf_player.h"

#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_eaf_internal.h"

#define ESP_EAF_ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

struct esp_eaf_player {
    esp_eaf_player_config_t config;
    esp_eaf_stream_info_t info;
    esp_eaf_frame_t frame;
    eaf4_t *eaf4;
    esp_eaf_format_handle_t legacy;
    esp_eaf_decoder_t *decoder;
    esp_eaf_header_t header;
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    size_t color_buffer_size;
    bool has_palette_alpha;
};

static size_t cache_alignment(void)
{
    size_t alignment = 0;
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment);
#endif
    return alignment ? alignment : 4;
}

static void *aligned_alloc_prefer_psram(size_t size, size_t *allocated_size)
{
    size_t alignment = cache_alignment();
    size_t aligned_size = ESP_EAF_ALIGN_UP(size, alignment);
    void *ptr = NULL;
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
    ptr = heap_caps_aligned_alloc(alignment, aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!ptr) {
        ptr = heap_caps_aligned_alloc(alignment, aligned_size, MALLOC_CAP_DEFAULT);
    }
    if (allocated_size) {
        *allocated_size = ptr ? aligned_size : 0;
    }
    return ptr;
}

static bool legacy_has_alpha(esp_eaf_format_handle_t handle)
{
    int total = esp_eaf_format_get_total_frames(handle);
    for (int frame = 0; frame < total; frame++) {
        const uint8_t *data = esp_eaf_format_get_frame_data(handle, frame);
        int size = esp_eaf_format_get_frame_size(handle, frame);
        esp_eaf_header_t header = {0};
        if (!data || size <= 0 || esp_eaf_header_parse(data, size, &header) != ESP_EAF_FORMAT_VALID) {
            continue;
        }
        bool found = false;
        if ((header.bit_depth == 4 || header.bit_depth == 8) && header.palette) {
            for (int i = 0; i < header.num_colors; i++) {
                if (header.palette[i * 4 + 3] < 0xFF) {
                    found = true;
                    break;
                }
            }
        } else if (header.bit_depth == 24 && header.blocks > 0 && header.block_len[0] > 0) {
            uint8_t mode = data[header.data_offset];
            found = mode == ESP_EAF_MODE_LOSSLESS || mode == 0x0C;
        }
        esp_eaf_free_header(&header);
        if (found) {
            return true;
        }
    }
    return false;
}

static esp_err_t decode_legacy(esp_eaf_player_t *player, uint32_t index)
{
    const uint8_t *data = esp_eaf_format_get_frame_data(player->legacy, (int)index);
    int frame_size = esp_eaf_format_get_frame_size(player->legacy, (int)index);
    if (!data || frame_size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_eaf_free_header(&player->header);
    if (esp_eaf_header_parse(data, frame_size, &player->header) != ESP_EAF_FORMAT_VALID) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (player->header.width != player->info.width || player->header.height != player->info.height) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (index == 0) {
        esp_eaf_decoder_reset(player->decoder);
    }

    bool delta_alpha = player->header.bit_depth == 24 && player->header.blocks > 0 &&
                       player->header.block_len[0] > 0 &&
                       (data[player->header.data_offset] == ESP_EAF_MODE_LOSSLESS ||
                        data[player->header.data_offset] == 0x0C);
    if (index == 0 || (player->has_palette_alpha && !delta_alpha)) {
        memset(player->frame_buffer, 0, player->frame_buffer_size);
    }

    size_t pixel_size = player->info.pixel_format == ESP_EAF_OUT_BGRA8888 ? 4 : 2;
    size_t block_stride = (size_t)player->header.block_height * player->header.width * pixel_size;
    uint8_t *alpha = player->info.pixel_format == ESP_EAF_OUT_RGB565 && player->info.has_alpha
                         ? player->frame_buffer + player->color_buffer_size
                         : NULL;
    esp_eaf_palette_cache_t cache = {0};
    memset(cache.color, 0xFF, sizeof(cache.color));
    for (int block = 0; block < player->header.blocks; block++) {
        size_t offset = (size_t)block * block_stride;
        if (offset >= player->color_buffer_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        esp_err_t err = esp_eaf_block_decode_ctx(player->decoder, &player->header, data, block,
                                                 player->frame_buffer + offset, alpha,
                                                 player->config.swap_rgb565_bytes, &cache);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (player->info.pixel_format == ESP_EAF_OUT_RGB565 &&
        player->config.color_key >= 0 && !player->has_palette_alpha) {
        uint16_t key = (uint16_t)player->config.color_key;
        const uint16_t *pixels = (const uint16_t *)player->frame_buffer;
        esp_eaf_dirty_rect_t dirty = {0};
        esp_eaf_get_dirty_rect_ctx(player->decoder, &dirty);
        int x0 = index > 0 && dirty.valid ? dirty.x1 : 0;
        int y0 = index > 0 && dirty.valid ? dirty.y1 : 0;
        int x1 = index > 0 && dirty.valid ? dirty.x2 : player->info.width - 1;
        int y1 = index > 0 && dirty.valid ? dirty.y2 : player->info.height - 1;
        if (x1 >= player->info.width) x1 = player->info.width - 1;
        if (y1 >= player->info.height) y1 = player->info.height - 1;
        for (int y = y0; y <= y1; y++) {
            size_t row = (size_t)y * player->info.width;
            for (int x = x0; x <= x1; x++) {
                alpha[row + x] = pixels[row + x] == key ? 0 : 0xFF;
            }
        }
    }

    esp_eaf_dirty_rect_t dirty = {0};
    esp_eaf_get_dirty_rect_ctx(player->decoder, &dirty);
    player->frame.dirty = (esp_eaf_rect_t){dirty.x1, dirty.y1, dirty.x2, dirty.y2, dirty.valid};
    player->frame.pixels = player->frame_buffer;
    player->frame.alpha = alpha;
    player->frame.index = index;
    return ESP_OK;
}

esp_err_t esp_eaf_player_open_memory(const void *data, size_t size,
                                     const esp_eaf_player_config_t *config,
                                     esp_eaf_player_t **out_player)
{
    if (!data || size == 0 || !out_player) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_player = NULL;
    esp_eaf_container_t container = esp_eaf_container_probe(data, size);
    if (container == ESP_EAF_CONTAINER_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_eaf_player_t *player = calloc(1, sizeof(*player));
    if (!player) {
        return ESP_ERR_NO_MEM;
    }
    player->config = config ? *config : (esp_eaf_player_config_t)ESP_EAF_PLAYER_CONFIG_DEFAULT();
    player->info.container = container;
    esp_err_t err = ESP_OK;

    if (player->config.output_format != ESP_EAF_OUT_RGB565 &&
        player->config.output_format != ESP_EAF_OUT_BGRA8888) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    if (container == ESP_EAF_CONTAINER_EAF8888) {
        if (eaf4_open_memory(data, size, &player->eaf4) != 0) {
            err = ESP_FAIL;
            goto fail;
        }
        const eaf4_info_t *info = eaf4_info(player->eaf4);
        player->info.width = info->width;
        player->info.height = info->height;
        player->info.frame_count = info->frames;
        player->info.pixel_format = ESP_EAF_OUT_BGRA8888;
        player->info.has_alpha = true;
        player->frame.pixel_bytes = info->frame_bytes;
        player->frame.stride = (size_t)info->width * 4;
    } else {
        if (player->config.output_format == ESP_EAF_OUT_BGRA8888 &&
            player->config.color_key >= 0) {
            err = ESP_ERR_NOT_SUPPORTED;
            goto fail;
        }
        err = esp_eaf_format_init(data, size, &player->legacy);
        if (err != ESP_OK) goto fail;
        int total = esp_eaf_format_get_total_frames(player->legacy);
        const uint8_t *first = total > 0 ? esp_eaf_format_get_frame_data(player->legacy, 0) : NULL;
        int first_size = total > 0 ? esp_eaf_format_get_frame_size(player->legacy, 0) : 0;
        if (!first || total <= 0 ||
            esp_eaf_header_parse(first, first_size, &player->header) != ESP_EAF_FORMAT_VALID) {
            err = ESP_ERR_INVALID_RESPONSE;
            goto fail;
        }
        player->decoder = esp_eaf_decoder_create();
        if (!player->decoder) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        esp_eaf_decoder_use_planar_rom(player->decoder, player->config.use_reference_planar);
        esp_eaf_decoder_set_output_format(player->decoder, player->config.output_format);
        player->info.width = player->header.width;
        player->info.height = player->header.height;
        player->info.frame_count = (uint32_t)total;
        player->info.pixel_format = player->config.output_format;
        player->has_palette_alpha = legacy_has_alpha(player->legacy);
        player->info.has_alpha = player->has_palette_alpha ||
                                 (player->info.pixel_format == ESP_EAF_OUT_RGB565 &&
                                  player->config.color_key >= 0);
        size_t pixel_size = player->info.pixel_format == ESP_EAF_OUT_BGRA8888 ? 4 : 2;
        player->color_buffer_size = (size_t)player->info.width * player->info.height * pixel_size;
        size_t alpha_size = player->info.pixel_format == ESP_EAF_OUT_RGB565 && player->info.has_alpha
                                ? (size_t)player->info.width * player->info.height
                                : 0;
        player->frame_buffer = aligned_alloc_prefer_psram(player->color_buffer_size + alpha_size,
                                                          &player->frame_buffer_size);
        if (!player->frame_buffer) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        player->frame.pixel_bytes = player->color_buffer_size;
        player->frame.stride = (size_t)player->info.width * pixel_size;
    }

    err = esp_eaf_player_seek(player, 0);
    if (err != ESP_OK) goto fail;
    *out_player = player;
    return ESP_OK;

fail:
    esp_eaf_player_close(player);
    return err;
}

void esp_eaf_player_close(esp_eaf_player_t *player)
{
    if (!player) return;
    if (player->eaf4) eaf4_close(player->eaf4);
    if (player->legacy) esp_eaf_format_deinit(player->legacy);
    esp_eaf_decoder_destroy(player->decoder);
    esp_eaf_free_header(&player->header);
    free(player->frame_buffer);
    free(player);
}

const esp_eaf_stream_info_t *esp_eaf_player_get_info(const esp_eaf_player_t *player)
{
    return player ? &player->info : NULL;
}

const esp_eaf_frame_t *esp_eaf_player_get_frame(const esp_eaf_player_t *player)
{
    return player ? &player->frame : NULL;
}

esp_err_t esp_eaf_player_seek(esp_eaf_player_t *player, uint32_t frame_index)
{
    if (!player || frame_index >= player->info.frame_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (player->eaf4) {
        const uint8_t *pixels = NULL;
        if (eaf4_seek(player->eaf4, frame_index, &pixels) != 0 || !pixels) {
            return ESP_FAIL;
        }
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
        esp_cache_msync((void *)pixels, ESP_EAF_ALIGN_UP(player->frame.pixel_bytes, 64),
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
        player->frame.pixels = pixels;
        player->frame.alpha = NULL;
        player->frame.index = frame_index;
        player->frame.dirty = (esp_eaf_rect_t){0, 0, player->info.width - 1,
                                               player->info.height - 1, true};
        return ESP_OK;
    }
    return decode_legacy(player, frame_index);
}

esp_err_t esp_eaf_player_next(esp_eaf_player_t *player)
{
    if (!player) return ESP_ERR_INVALID_ARG;
    uint32_t next = player->frame.index + 1;
    if (next >= player->info.frame_count) next = 0;
    return esp_eaf_player_seek(player, next);
}

void esp_eaf_player_set_reference_planar(esp_eaf_player_t *player, bool enabled)
{
    if (!player) return;
    player->config.use_reference_planar = enabled;
    esp_eaf_decoder_use_planar_rom(player->decoder, enabled);
}

void esp_eaf_player_get_planar_stats(const esp_eaf_player_t *player,
                                     uint64_t *inflate_us, uint64_t *restore_us,
                                     uint32_t *frames)
{
    esp_eaf_planar_stats_t stats = {0};
    if (player && player->decoder) {
        esp_eaf_decoder_get_planar_stats(player->decoder, &stats);
    }
    if (inflate_us) *inflate_us = stats.inflate_us;
    if (restore_us) *restore_us = stats.restore_us;
    if (frames) *frames = stats.frames;
}
