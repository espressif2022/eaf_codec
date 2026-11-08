/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/** @file esp_eaf_decoder.c
 *  @brief Per-stream decoder state, scratch buffers and dirty-region tracking.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "zlib.h"

#include "esp_eaf_internal.h"
#include "esp_eaf_codec_internal.h"

#if ESP_LV_EAF_ENABLE_H264
#include "esp_h264_dec.h"
#endif

esp_eaf_decoder_t *esp_eaf_decoder_create(void)
{
    return calloc(1, sizeof(esp_eaf_decoder_t));
}
void esp_eaf_decoder_reset(esp_eaf_decoder_t *decoder)
{
    if (!decoder) return;
    bool rom = decoder->predictor_rom;
    uint8_t *scratch = decoder->predictor_scratch;
    size_t capacity = decoder->predictor_capacity;
    uint8_t *int_scratch = decoder->lossless_int_scratch;
    size_t int_capacity = decoder->lossless_int_capacity;
    bool int_tried = decoder->lossless_int_tried;
    esp_eaf_planar_stats_t stats = decoder->predictor_stats;
    const uint16_t *fused_bg = decoder->fused_bg_rgb565;
    uint16_t *fused_comp = decoder->fused_comp_rgb565;
    esp_eaf_output_format_t out_fmt = decoder->out_fmt;
    if (decoder->predictor_z_ready) inflateEnd(&decoder->predictor_z);
#if ESP_LV_EAF_ENABLE_H264
    if (decoder->h264) {
        esp_h264_dec_close(decoder->h264);
        esp_h264_dec_del(decoder->h264);
    }
#endif
    memset(decoder, 0, sizeof(*decoder));
    decoder->predictor_rom = rom;
    decoder->predictor_scratch = scratch;
    decoder->predictor_capacity = capacity;
    decoder->lossless_int_scratch = int_scratch;
    decoder->lossless_int_capacity = int_capacity;
    decoder->lossless_int_tried = int_tried;
    decoder->predictor_stats = stats;
    decoder->fused_bg_rgb565 = fused_bg;
    decoder->fused_comp_rgb565 = fused_comp;
    decoder->out_fmt = out_fmt;
}

void esp_eaf_decoder_set_output_format(esp_eaf_decoder_t *decoder, esp_eaf_output_format_t out_fmt)
{
    if (decoder) {
        decoder->out_fmt = out_fmt;
    }
}

esp_eaf_output_format_t esp_eaf_decoder_get_output_format(const esp_eaf_decoder_t *decoder)
{
    return decoder ? decoder->out_fmt : ESP_EAF_OUT_RGB565;
}

void esp_eaf_decoder_mark_dirty(esp_eaf_decoder_t *decoder, uint16_t x1, uint16_t y1,
                                uint16_t x2, uint16_t y2)
{
    if (!decoder) return;
    if (!decoder->dirty.valid) {
        decoder->dirty = (esp_eaf_dirty_rect_t){x1, y1, x2, y2, true};
        return;
    }
    if (x1 < decoder->dirty.x1) decoder->dirty.x1 = x1;
    if (y1 < decoder->dirty.y1) decoder->dirty.y1 = y1;
    if (x2 > decoder->dirty.x2) decoder->dirty.x2 = x2;
    if (y2 > decoder->dirty.y2) decoder->dirty.y2 = y2;
}

size_t esp_eaf_decoder_reserve_internal_scratch(esp_eaf_decoder_t *decoder, size_t size)
{
    if (!decoder) return 0;
    if (decoder->lossless_int_scratch && decoder->lossless_int_capacity >= size) {
        return decoder->lossless_int_capacity;
    }
    free(decoder->lossless_int_scratch);
    decoder->lossless_int_scratch = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    decoder->lossless_int_capacity = decoder->lossless_int_scratch ? size : 0;
    decoder->lossless_int_tried = true;
    return decoder->lossless_int_capacity;
}

void esp_eaf_decoder_set_fused_blend(esp_eaf_decoder_t *decoder, const uint16_t *bg_rgb565, uint16_t *comp_rgb565)
{
    if (!decoder) return;
    decoder->fused_bg_rgb565 = bg_rgb565;
    decoder->fused_comp_rgb565 = comp_rgb565;
}

void esp_eaf_decoder_use_planar_rom(esp_eaf_decoder_t *decoder, bool enabled)
{
    if (decoder) decoder->predictor_rom = enabled;
}

void esp_eaf_decoder_get_planar_stats(const esp_eaf_decoder_t *decoder, esp_eaf_planar_stats_t *stats)
{
    if (stats) *stats = decoder ? decoder->predictor_stats : (esp_eaf_planar_stats_t){0};
}

void esp_eaf_decoder_destroy(esp_eaf_decoder_t *decoder)
{
    if (!decoder) return;
    esp_eaf_decoder_reset(decoder);
    free(decoder->predictor_scratch);
    free(decoder->lossless_int_scratch);
    free(decoder);
}

void esp_eaf_get_dirty_rect_ctx(const esp_eaf_decoder_t *decoder, esp_eaf_dirty_rect_t *rect)
{
    if (decoder && rect) *rect = decoder->dirty;
}

uint8_t esp_eaf_get_dirty_bands_ctx(const esp_eaf_decoder_t *decoder, esp_eaf_dirty_rect_t *bands, uint8_t max_bands)
{
    if (!decoder || !bands || max_bands == 0) {
        return 0;
    }
    uint8_t n = decoder->dirty_band_count;
    if (n > max_bands) {
        n = max_bands;
    }
    for (uint8_t i = 0; i < n; i++) {
        bands[i] = decoder->dirty_bands[i];
    }
    return n;
}

static inline uint32_t eaf_rect_area(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
    return (x2 - x1 + 1U) * (y2 - y1 + 1U);
}

#define EAF_DIRTY_CELL_TILES 8U     /* 32 px cells */
#define EAF_DIRTY_MAX_CELLS  32U
#define EAF_DIRTY_GAP_CELLS  0U     /* bridge up to this many empty cells inside a cell row */
/* Each extra LVGL refresh area costs roughly as much as redrawing this many pixels
 * (per-area setup plus PSRAM row-miss latency), so cheaper merges are always taken. */
#define EAF_DIRTY_MERGE_PX   4000U

static inline uint32_t eaf_band_merge_cost(const esp_eaf_dirty_rect_t *a, const esp_eaf_dirty_rect_t *b)
{
    uint32_t au = eaf_rect_area(MIN(a->x1, b->x1), MIN(a->y1, b->y1), MAX(a->x2, b->x2), MAX(a->y2, b->y2));
    uint32_t sum = eaf_rect_area(a->x1, a->y1, a->x2, a->y2) + eaf_rect_area(b->x1, b->y1, b->x2, b->y2);
    return au > sum ? au - sum : 0;
}

static void eaf_band_merge(esp_eaf_decoder_t *decoder, uint8_t keep, uint8_t drop)
{
    esp_eaf_dirty_rect_t *b = decoder->dirty_bands;
    b[keep].x1 = MIN(b[keep].x1, b[drop].x1);
    b[keep].y1 = MIN(b[keep].y1, b[drop].y1);
    b[keep].x2 = MAX(b[keep].x2, b[drop].x2);
    b[keep].y2 = MAX(b[keep].y2, b[drop].y2);
    b[drop] = b[--decoder->dirty_band_count];
}

/* Append a dirty rect, merging it while the union is cheaper than a separate refresh area;
 * past the cap, merge whichever pair grows the area least. */
static void eaf_add_dirty_band(esp_eaf_decoder_t *decoder, uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    esp_eaf_dirty_rect_t *b = decoder->dirty_bands;
    uint8_t cur = decoder->dirty_band_count++;
    b[cur] = (esp_eaf_dirty_rect_t){ .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2, .valid = true };

    for (;;) {
        uint8_t best = UINT8_MAX;
        uint32_t best_cost = EAF_DIRTY_MERGE_PX;
        for (uint8_t j = 0; j < decoder->dirty_band_count; j++) {
            if (j == cur) continue;
            uint32_t cost = eaf_band_merge_cost(&b[cur], &b[j]);
            if (cost < best_cost) {
                best_cost = cost;
                best = j;
            }
        }
        if (best == UINT8_MAX) break;
        uint8_t keep = MIN(cur, best), drop = MAX(cur, best);
        eaf_band_merge(decoder, keep, drop);
        cur = keep;
    }

    while (decoder->dirty_band_count > ESP_EAF_MAX_DIRTY_BANDS) {
        uint8_t n = decoder->dirty_band_count, best_i = 0, best_j = 1;
        uint32_t best_cost = UINT32_MAX;
        for (uint8_t i = 0; i < n; i++) {
            for (uint8_t j = i + 1; j < n; j++) {
                uint32_t cost = eaf_band_merge_cost(&b[i], &b[j]);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_i = i;
                    best_j = j;
                }
            }
        }
        eaf_band_merge(decoder, best_i, best_j);
    }
}

/* Turn a 4x4 tile change mask into cell-row runs, each emitted as a tight dirty rect. */
void esp_eaf_decoder_emit_tile_dirty_bands(esp_eaf_decoder_t *decoder, const uint8_t *mask,
                                           int tiles_x, int tiles_y, uint16_t y_off)
{
    uint32_t cell_tiles = EAF_DIRTY_CELL_TILES;
    if ((uint32_t)tiles_x > cell_tiles * EAF_DIRTY_MAX_CELLS) {
        cell_tiles = ((uint32_t)tiles_x + EAF_DIRTY_MAX_CELLS - 1U) / EAF_DIRTY_MAX_CELLS;
    }
    uint32_t cells_x = ((uint32_t)tiles_x + cell_tiles - 1U) / cell_tiles;
    uint16_t cx1[EAF_DIRTY_MAX_CELLS], cx2[EAF_DIRTY_MAX_CELLS];
    uint16_t cy1[EAF_DIRTY_MAX_CELLS], cy2[EAF_DIRTY_MAX_CELLS];

    for (uint32_t tr0 = 0; tr0 < (uint32_t)tiles_y; tr0 += cell_tiles) {
        uint32_t tr1 = MIN(tr0 + cell_tiles, (uint32_t)tiles_y);
        uint32_t occ = 0;
        for (uint32_t tr = tr0; tr < tr1; tr++) {
            uint32_t t = tr * (uint32_t)tiles_x;
            for (uint32_t tc = 0; tc < (uint32_t)tiles_x; tc++, t++) {
                if (!(mask[t >> 3] & (1U << (t & 7U)))) {
                    continue;
                }
                uint32_t c = tc / cell_tiles;
                if (!(occ & (1U << c))) {
                    occ |= 1U << c;
                    cx1[c] = cx2[c] = (uint16_t)tc;
                    cy1[c] = cy2[c] = (uint16_t)tr;
                } else {
                    if (tc < cx1[c]) cx1[c] = (uint16_t)tc;
                    if (tc > cx2[c]) cx2[c] = (uint16_t)tc;
                    cy2[c] = (uint16_t)tr;
                }
            }
        }
        uint32_t c = 0;
        while (c < cells_x) {
            if (!(occ & (1U << c))) {
                c++;
                continue;
            }
            uint16_t x1 = cx1[c], x2 = cx2[c], y1 = cy1[c], y2 = cy2[c];
            uint32_t last = c;
            for (uint32_t n = c + 1; n < cells_x && n <= last + EAF_DIRTY_GAP_CELLS + 1U; n++) {
                if (occ & (1U << n)) {
                    x2 = cx2[n];
                    y1 = MIN(y1, cy1[n]);
                    y2 = MAX(y2, cy2[n]);
                    last = n;
                }
            }
            eaf_add_dirty_band(decoder, (uint16_t)(x1 * 4U), (uint16_t)(y_off + y1 * 4U),
                               (uint16_t)(x2 * 4U + 3U), (uint16_t)(y_off + y2 * 4U + 3U));
            c = last + 1U;
        }
    }
}
