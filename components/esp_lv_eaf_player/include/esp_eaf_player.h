/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_eaf_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GUI-independent EAF player. The source memory is borrowed until close. */
typedef struct esp_eaf_player esp_eaf_player_t;

typedef struct {
    bool swap_rgb565_bytes;    /*!< Swap bytes in RGB565 output. */
    bool use_reference_planar; /*!< Use the reference legacy planar decoder. */
    int32_t color_key;         /*!< RGB565 transparent key, or -1 to disable. */
    esp_eaf_output_format_t output_format; /*!< Requested classic EAF output. EAF4 remains BGRA8888. */
} esp_eaf_player_config_t;

#define ESP_EAF_PLAYER_CONFIG_DEFAULT() { \
    .swap_rgb565_bytes = false,          \
    .use_reference_planar = false,       \
    .color_key = -1,                     \
    .output_format = ESP_EAF_OUT_RGB565, \
}

typedef struct {
    esp_eaf_container_t container;
    esp_eaf_output_format_t pixel_format;
    uint16_t width;
    uint16_t height;
    uint32_t frame_count;
    bool has_alpha;
} esp_eaf_stream_info_t;

typedef struct {
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
    bool valid;
} esp_eaf_rect_t;

/**
 * Decoded frame view. Memory is owned by the player and remains valid until
 * the next seek/next call or esp_eaf_player_close(). RGB565 alpha, when
 * present, is a separate tightly packed A8 plane.
 */
typedef struct {
    const uint8_t *pixels;
    const uint8_t *alpha;
    size_t pixel_bytes;
    size_t stride;
    uint32_t index;
    esp_eaf_rect_t dirty;
} esp_eaf_frame_t;

esp_err_t esp_eaf_player_open_memory(const void *data, size_t size,
                                     const esp_eaf_player_config_t *config,
                                     esp_eaf_player_t **out_player);
esp_err_t esp_eaf_player_open_file(const char *path,
                                   const esp_eaf_player_config_t *config,
                                   esp_eaf_player_t **out_player);
void esp_eaf_player_release_cache(esp_eaf_player_t *player);
void esp_eaf_player_close(esp_eaf_player_t *player);

const esp_eaf_stream_info_t *esp_eaf_player_get_info(const esp_eaf_player_t *player);
const esp_eaf_frame_t *esp_eaf_player_get_frame(const esp_eaf_player_t *player);

esp_err_t esp_eaf_player_seek(esp_eaf_player_t *player, uint32_t frame_index);
esp_err_t esp_eaf_player_next(esp_eaf_player_t *player);

void esp_eaf_player_set_reference_planar(esp_eaf_player_t *player, bool enabled);
void esp_eaf_player_get_planar_stats(const esp_eaf_player_t *player,
                                     uint64_t *inflate_us, uint64_t *restore_us,
                                     uint32_t *frames);

#ifdef __cplusplus
}
#endif
