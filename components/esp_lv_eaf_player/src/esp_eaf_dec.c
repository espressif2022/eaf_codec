/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file esp_eaf_dec.c
 * @brief ESP EAF (Emote Animation Format) decoder implementation
 */

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/param.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "miniz.h"
#include "zlib.h"
#include "esp_timer.h"
#include "esp_eaf_dec.h"
#include "esp_lv_eaf_player_config.h"

#if ESP_LV_EAF_ENABLE_SW_JPEG
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#endif

#if ESP_LV_EAF_ENABLE_H264
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"

static bool s_yuv_lut_inited = false;
static int16_t s_y_lut[256];
static int16_t s_rv_lut[256];
static int16_t s_gu_lut[256];
static int16_t s_gv_lut[256];
static int16_t s_bu_lut[256];
static uint16_t s_r565_clamp[1024];
static uint16_t s_g565_clamp[1024];
static uint16_t s_b565_clamp[1024];

static void esp_eaf_init_yuv_luts(void)
{
    if (s_yuv_lut_inited) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        int d = i - 128;
        s_y_lut[i] = (298 * (i - 16) + 128) >> 8;
        s_rv_lut[i] = (int16_t)((409 * d) >> 8);
        s_gu_lut[i] = (int16_t)((100 * d) >> 8);
        s_gv_lut[i] = (int16_t)((208 * d) >> 8);
        s_bu_lut[i] = (int16_t)((516 * d) >> 8);
    }
    for (int i = 0; i < 1024; i++) {
        int v = i - 384;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        s_r565_clamp[i] = (uint16_t)((v & 0xF8) << 8);
        s_g565_clamp[i] = (uint16_t)((v & 0xFC) << 3);
        s_b565_clamp[i] = (uint16_t)(v >> 3);
    }
    s_yuv_lut_inited = true;
}

static inline uint16_t esp_eaf_yuv_lut_px(int y, int rd, int gd, int bd)
{
    y = s_y_lut[y]; /* Generated H.264 is explicitly BT.601 limited range. */
    return s_r565_clamp[y + rd + 384] | s_g565_clamp[y - gd + 384] | s_b565_clamp[y + bd + 384];
}

static void esp_eaf_i420_to_rgb565(const uint8_t *i420, int width, int height, uint16_t *rgb565, bool swap_color)
{
    esp_eaf_init_yuv_luts();
    const uint8_t *y_plane = i420;
    const uint8_t *u_plane = i420 + width * height;
    const uint8_t *v_plane = u_plane + (width * height >> 2);
    int uv_stride = width >> 1;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row0 = y_plane + y * width;
        const uint8_t *y_row1 = y_row0 + width;
        const uint8_t *u_row  = u_plane + (y >> 1) * uv_stride;
        const uint8_t *v_row  = v_plane + (y >> 1) * uv_stride;
        uint32_t *dst32_0 = (uint32_t *)(rgb565 + y * width);
        uint32_t *dst32_1 = (uint32_t *)(rgb565 + (y + 1) * width);

        for (int x = 0; x < width; x += 2) {
            uint8_t u = u_row[x >> 1];
            uint8_t v = v_row[x >> 1];
            int rd = s_rv_lut[v];
            int gd = s_gu_lut[u] + s_gv_lut[v];
            int bd = s_bu_lut[u];

            uint16_t p00 = esp_eaf_yuv_lut_px(y_row0[x],     rd, gd, bd);
            uint16_t p01 = esp_eaf_yuv_lut_px(y_row0[x + 1], rd, gd, bd);
            uint16_t p10 = esp_eaf_yuv_lut_px(y_row1[x],     rd, gd, bd);
            uint16_t p11 = esp_eaf_yuv_lut_px(y_row1[x + 1], rd, gd, bd);
            if (swap_color) {
                p00 = __builtin_bswap16(p00);
                p01 = __builtin_bswap16(p01);
                p10 = __builtin_bswap16(p10);
                p11 = __builtin_bswap16(p11);
            }
            dst32_0[x >> 1] = ((uint32_t)p01 << 16) | p00;
            dst32_1[x >> 1] = ((uint32_t)p11 << 16) | p10;
        }
    }
}
#endif

/* Hardware JPEG decoder support detection */
#if ESP_LV_EAF_ENABLE_HW_JPEG
#define ESP_EAF_HW_JPEG_ENABLED 1
#include "driver/jpeg_decode.h"
#else
#define ESP_EAF_HW_JPEG_ENABLED 0
#endif

static const char *TAG = "esp_eaf_dec";

/**********************
 *  STATIC VARIABLES
 **********************/
static esp_eaf_block_decoder_t s_esp_eaf_decoder[ESP_EAF_ENCODING_MAX] = {0};
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
#if ESP_LV_EAF_ENABLE_H264
    esp_h264_dec_handle_t h264;
#endif
};
/* Compatibility API is single-stream. LVGL and the benchmark use explicit contexts. */
static esp_eaf_decoder_t s_legacy_decoder;

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

static voidpf planar_zalloc(voidpf opaque, uInt count, uInt size)
{
    (void)opaque;
    if (size && count > SIZE_MAX / size) return NULL;
    size_t bytes = (size_t)count * size;
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, bytes);
    return p;
}

static void planar_zfree(voidpf opaque, voidpf pointer)
{
    (void)opaque;
    free(pointer);
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
static void eaf_emit_tile_dirty_bands(esp_eaf_decoder_t *decoder, const uint8_t *mask,
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

void esp_eaf_get_last_dirty_rect(esp_eaf_dirty_rect_t *rect)
{
    esp_eaf_get_dirty_rect_ctx(&s_legacy_decoder, rect);
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)read_le16(p) | ((uint32_t)read_le16(p + 2) << 16);
}

static void esp_eaf_cache_jpeg_tables(esp_eaf_decoder_t *decoder, const uint8_t *jpg, size_t len)
{
    decoder->jpeg_hdr_len = 0;
    if (len < 16 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        return;
    }
    size_t pos = 2;
    size_t out = 0;
    decoder->jpeg_hdr[out++] = 0xFF;
    decoder->jpeg_hdr[out++] = 0xD8;
    decoder->sof0_offset = -1;
    while (pos + 4 <= len && jpg[pos] == 0xFF) {
        uint8_t m = jpg[pos + 1];
        uint16_t seg_len = ((uint16_t)jpg[pos + 2] << 8) | jpg[pos + 3];
        if (seg_len < 2 || pos + 2 + seg_len > len) {
            break;
        }
        if (m == 0xDB || m == 0xC0 || m == 0xC4 || m == 0xDA) {
            if (out + 2 + seg_len <= sizeof(decoder->jpeg_hdr)) {
                if (m == 0xC0) {
                    decoder->sof0_offset = (int)out;
                }
                memcpy(&decoder->jpeg_hdr[out], &jpg[pos], 2 + seg_len);
                out += 2 + seg_len;
            }
            if (m == 0xDA) {
                decoder->jpeg_hdr_len = out;
                return;
            }
        }
        pos += 2 + seg_len;
    }
}

#if ESP_EAF_HW_JPEG_ENABLED
static jpeg_decoder_handle_t s_hw_jpeg_handle = NULL;
#endif

/**********************
 *  STATIC HELPER FUNCTIONS
 **********************/

/**
 * @brief Align size up to alignment boundary
 */
#define ESP_EAF_ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

/**
 * @brief Get cache alignment for PSRAM
 */
static inline size_t esp_eaf_get_psram_cache_align(void)
{
    size_t cache_align = 0;
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_align);
#endif
    /* Fallback to 4-byte alignment if cache alignment not available */
    if (cache_align == 0) {
        cache_align = 4;
    }
    return cache_align;
}

/**
 * @brief Allocate memory with PSRAM priority
 *
 * Tries to allocate from PSRAM first, falls back to internal RAM if unavailable.
 * This function prioritizes PSRAM usage to save SRAM.
 */
static inline void *esp_eaf_malloc_prefer_psram(size_t size)
{
    void *ptr = NULL;

    /* Try PSRAM first if available */
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
    ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif

    /* Fallback to default memory (internal RAM) */
    if (ptr == NULL) {
        ptr = malloc(size);
    }

    return ptr;
}

/**
 * @brief Allocate aligned memory with PSRAM priority
 *
 * Tries to allocate aligned memory from PSRAM first, falls back to internal RAM if unavailable.
 * This is required for hardware JPEG decoder output buffers.
 *
 * @param size Size to allocate
 * @param alignment Alignment requirement (0 means use cache line alignment)
 * @param allocated_size Pointer to store actual allocated size (may be larger due to alignment)
 * @return Pointer to allocated memory, or NULL on failure
 */
static inline void *esp_eaf_malloc_aligned_prefer_psram(size_t size, size_t alignment, size_t *allocated_size)
{
    void *ptr = NULL;
    size_t aligned_size = size;

    /* Use cache alignment if not specified */
    if (alignment == 0) {
        alignment = esp_eaf_get_psram_cache_align();
    }

    /* Align size up to alignment boundary */
    aligned_size = ESP_EAF_ALIGN_UP(size, alignment);

    /* Try PSRAM first if available */
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
    ptr = heap_caps_aligned_alloc(alignment, aligned_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif

    /* Fallback to default memory (internal RAM) with alignment */
    if (ptr == NULL) {
        ptr = heap_caps_aligned_alloc(alignment, aligned_size, MALLOC_CAP_DEFAULT);
    }

    if (allocated_size != NULL) {
        *allocated_size = (ptr != NULL) ? aligned_size : 0;
    }

    return ptr;
}

/**
 * @brief Allocate and zero-initialize memory with PSRAM priority
 *
 * Tries to allocate from PSRAM first, falls back to internal RAM if unavailable.
 */
static inline void *esp_eaf_calloc_prefer_psram(size_t num, size_t size)
{
    void *ptr = NULL;

    /* Try PSRAM first if available */
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM
    size_t total_size = num * size;
    ptr = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != NULL) {
        memset(ptr, 0, total_size);
    }
#endif

    /* Fallback to default memory (internal RAM) */
    if (ptr == NULL) {
        ptr = calloc(num, size);
    }

    return ptr;
}

/**
 * @brief Allocate and zero-initialize aligned memory with PSRAM priority
 *
 * Tries to allocate aligned memory from PSRAM first, falls back to internal RAM if unavailable.
 * This is required for hardware JPEG decoder output buffers.
 *
 * @param num Number of elements
 * @param size Size of each element
 * @param alignment Alignment requirement (0 means use cache line alignment)
 * @param allocated_size Pointer to store actual allocated size (may be larger due to alignment)
 * @return Pointer to allocated memory, or NULL on failure
 */
static inline void *esp_eaf_calloc_aligned_prefer_psram(size_t num, size_t size, size_t alignment, size_t *allocated_size)
{
    size_t total_size = num * size;
    void *ptr = esp_eaf_malloc_aligned_prefer_psram(total_size, alignment, allocated_size);
    if (ptr != NULL) {
        memset(ptr, 0, *allocated_size);
    }
    return ptr;
}

/**
 * @brief Calculate checksum for EAF data
 */
static uint32_t esp_eaf_format_calc_checksum(const uint8_t *data, uint32_t length)
{
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < length; i++) {
        checksum += data[i];
    }
    return checksum;
}

/* Huffman Tree Helper Functions */
static esp_eaf_huffman_node_t* esp_eaf_huffman_node_create()
{
    esp_eaf_huffman_node_t* node = (esp_eaf_huffman_node_t*)esp_eaf_calloc_prefer_psram(1, sizeof(esp_eaf_huffman_node_t));
    return node;
}

static void esp_eaf_huffman_tree_free(esp_eaf_huffman_node_t* node)
{
    if (!node) {
        return;
    }
    esp_eaf_huffman_tree_free(node->left);
    esp_eaf_huffman_tree_free(node->right);
    free(node);
}

static esp_err_t esp_eaf_huffman_decode_data(const uint8_t* encoded_data, size_t encoded_len,
                                             const uint8_t* dict_data, size_t dict_len,
                                             uint8_t* decoded_data, size_t* decoded_len)
{
    if (!encoded_data || !dict_data || encoded_len == 0 || dict_len == 0) {
        *decoded_len = 0;
        return ESP_OK;
    }

    /* Get padding bits from dictionary */
    uint8_t padding_bits = dict_data[0];
    size_t dict_pos = 1;

    /* Reconstruct Huffman Tree */
    esp_eaf_huffman_node_t* root = esp_eaf_huffman_node_create();
    if (!root) {
        ESP_LOGE(TAG, "Failed to allocate root node");
        return ESP_ERR_NO_MEM;
    }
    esp_eaf_huffman_node_t* current_node = NULL;

    while (dict_pos < dict_len) {
        if (dict_pos + 1 >= dict_len) {
            ESP_LOGE(TAG, "Dictionary truncated at position %zu", dict_pos);
            esp_eaf_huffman_tree_free(root);
            return ESP_FAIL;
        }
        uint8_t symbol = dict_data[dict_pos++];
        uint8_t code_len = dict_data[dict_pos++];

        if (code_len == 0 || code_len > 64) {
            ESP_LOGE(TAG, "Invalid code length: %d", code_len);
            esp_eaf_huffman_tree_free(root);
            return ESP_FAIL;
        }

        size_t code_byte_len = (code_len + 7) / 8;
        if (dict_pos + code_byte_len > dict_len) {
            ESP_LOGE(TAG, "Dictionary overflow at position %zu", dict_pos);
            esp_eaf_huffman_tree_free(root);
            return ESP_FAIL;
        }

        uint64_t code = 0;
        for (size_t i = 0; i < code_byte_len; ++i) {
            code = (code << 8) | dict_data[dict_pos++];
        }

        /* Insert symbol into tree */
        current_node = root;
        for (int bit_pos = code_len - 1; bit_pos >= 0; --bit_pos) {
            int bit_val = (code >> bit_pos) & 1;
            if (bit_val == 0) {
                if (!current_node->left) {
                    current_node->left = esp_eaf_huffman_node_create();
                    if (!current_node->left) {
                        ESP_LOGE(TAG, "Failed to allocate Huffman node");
                        esp_eaf_huffman_tree_free(root);
                        return ESP_ERR_NO_MEM;
                    }
                }
                current_node = current_node->left;
            } else {
                if (!current_node->right) {
                    current_node->right = esp_eaf_huffman_node_create();
                    if (!current_node->right) {
                        ESP_LOGE(TAG, "Failed to allocate Huffman node");
                        esp_eaf_huffman_tree_free(root);
                        return ESP_ERR_NO_MEM;
                    }
                }
                current_node = current_node->right;
            }
        }
        current_node->is_leaf = 1;
        current_node->symbol = symbol;
    }

    /* Calculate total bits to decode */
    size_t total_bits = encoded_len * 8;
    if (padding_bits > 0) {
        total_bits -= padding_bits;
    }

    current_node = root;
    size_t decoded_pos = 0;

    /* Process each bit in the encoded data */
    for (size_t bit_index = 0; bit_index < total_bits; bit_index++) {
        size_t byte_idx = bit_index / 8;
        int bit_offset = 7 - (bit_index % 8);  /* Most significant bit first */
        int bit_val = (encoded_data[byte_idx] >> bit_offset) & 1;

        if (bit_val == 0) {
            current_node = current_node->left;
        } else {
            current_node = current_node->right;
        }

        if (current_node == NULL) {
            ESP_LOGE(TAG, "Invalid path in Huffman tree at bit %zu", bit_index);
            esp_eaf_huffman_tree_free(root);
            return ESP_FAIL;
        }

        if (current_node->is_leaf) {
            decoded_data[decoded_pos++] = current_node->symbol;
            current_node = root;
        }
    }

    *decoded_len = decoded_pos;
    esp_eaf_huffman_tree_free(root);
    return ESP_OK;
}

/**********************
 *  HEADER FUNCTIONS
 **********************/

esp_eaf_format_t esp_eaf_header_parse(const uint8_t *file_data, size_t file_size, esp_eaf_header_t *header)
{
    if (!header) return ESP_EAF_FORMAT_INVALID;
    memset(header, 0, sizeof(*header));
    if (!file_data || file_size < 3) return ESP_EAF_FORMAT_INVALID;
    memcpy(header->format, file_data, 2);
    if (memcmp(file_data, "_R", 2) == 0) {
        size_t len = file_data[2];
        if (len > file_size - 3) return ESP_EAF_FORMAT_INVALID;
        header->palette = esp_eaf_malloc_prefer_psram(len + 1);
        if (!header->palette) return ESP_EAF_FORMAT_INVALID;
        memcpy(header->palette, file_data + 3, len);
        header->palette[len] = 0;
        header->num_colors = len + 1;
        return ESP_EAF_FORMAT_REDIRECT;
    }
    if (memcmp(file_data, "_S", 2) != 0 || file_size < 18) return ESP_EAF_FORMAT_INVALID;
    memcpy(header->version, file_data + 3, 6);
    header->bit_depth = file_data[9];
    if (header->bit_depth != 4 && header->bit_depth != 8 && header->bit_depth != 24) return ESP_EAF_FORMAT_INVALID;
    header->width = read_le16(file_data + 10);
    header->height = read_le16(file_data + 12);
    header->blocks = read_le16(file_data + 14);
    header->block_height = read_le16(file_data + 16);
    if (!header->width || !header->height || !header->blocks || !header->block_height ||
        header->blocks != (header->height + header->block_height - 1) / header->block_height) return ESP_EAF_FORMAT_INVALID;
    header->num_colors = header->bit_depth == 24 ? 0 : 1 << header->bit_depth;
    size_t offset = 18 + (size_t)header->blocks * 4 + (size_t)header->num_colors * 4;
    if (offset > file_size || offset > UINT16_MAX) return ESP_EAF_FORMAT_INVALID;
    size_t remaining = file_size - offset;
    for (int i = 0; i < header->blocks; i++) {
        uint32_t len = read_le32(file_data + 18 + i * 4);
        if (len > remaining) return ESP_EAF_FORMAT_INVALID;
        remaining -= len;
    }
    header->block_len = esp_eaf_malloc_prefer_psram(header->blocks * sizeof(uint32_t));
    if (!header->block_len) return ESP_EAF_FORMAT_INVALID;
    for (int i = 0; i < header->blocks; i++) header->block_len[i] = read_le32(file_data + 18 + i * 4);
    if (header->num_colors) {
        header->palette = esp_eaf_malloc_prefer_psram(header->num_colors * 4);
        if (!header->palette) {
            esp_eaf_free_header(header);
            return ESP_EAF_FORMAT_INVALID;
        }
        memcpy(header->palette, file_data + 18 + header->blocks * 4, header->num_colors * 4);
    }
    header->data_offset = offset;
    return ESP_EAF_FORMAT_VALID;
}

void esp_eaf_free_header(esp_eaf_header_t *header)
{
    if (header->block_len != NULL) {
        free(header->block_len);
        header->block_len = NULL;
    }
    if (header->palette != NULL) {
        free(header->palette);
        header->palette = NULL;
    }
}

void esp_eaf_calculate_offsets(const esp_eaf_header_t *header, uint32_t *offsets)
{
    offsets[0] = header->data_offset;
    for (int i = 1; i < header->blocks; i++) {
        offsets[i] = offsets[i - 1] + header->block_len[i - 1];
    }
}

/**********************
 *  PALETTE FUNCTIONS
 **********************/

esp_eaf_color_t esp_eaf_palette_get_color(const esp_eaf_header_t *header, uint8_t color_index, bool swap_bytes)
{
    const uint8_t *color_data = &header->palette[color_index * 4];
    /* RGB888: R=color[2], G=color[1], B=color[0] */
    /* RGB565: R: (color[2] & 0xF8) << 8, G: (color[1] & 0xFC) << 3, B: (color[0] & 0xF8) >> 3 */
    esp_eaf_color_t result;
    uint16_t rgb565_value = swap_bytes ? __builtin_bswap16(((color_data[2] & 0xF8) << 8) | ((color_data[1] & 0xFC) << 3) | ((color_data[0] & 0xF8) >> 3)) : \
                            ((color_data[2] & 0xF8) << 8) | ((color_data[1] & 0xFC) << 3) | ((color_data[0] & 0xF8) >> 3);
    result.full = rgb565_value;
    return result;
}

/**********************
 *  DECODING FUNCTIONS
 **********************/

/**
 * @brief Decode Huffman + RLE compressed data
 */
static esp_err_t esp_eaf_huffman_rle_decode(const uint8_t *input_data, size_t input_size,
                                            uint8_t *output_buffer, size_t *out_size,
                                            bool swap_color)
{
    if (out_size == NULL || *out_size == 0) {
        ESP_LOGE(TAG, "Output size is invalid");
        return ESP_FAIL;
    }

    uint8_t *huffman_buffer = esp_eaf_malloc_prefer_psram(*out_size);
    if (huffman_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for Huffman buffer");
        return ESP_FAIL;
    }

    size_t huffman_out_size = *out_size;
    esp_err_t ret = esp_eaf_huffman_decode(input_data, input_size, huffman_buffer, &huffman_out_size, swap_color);
    if (ret == ESP_OK) {
        ret = esp_eaf_rle_decode(huffman_buffer, huffman_out_size, output_buffer, out_size, swap_color);
    }

    free(huffman_buffer);
    return ret;
}

/**
 * @brief Register a decoder for a specific encoding type
 */
static esp_err_t esp_eaf_register_decoder(esp_eaf_encoding_t type, esp_eaf_block_decoder_t decoder)
{
    if (type >= ESP_EAF_ENCODING_MAX) {
        ESP_LOGE(TAG, "Invalid encoding type: %d", type);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_esp_eaf_decoder[type] != NULL) {
        ESP_LOGW(TAG, "Decoder already registered for type: %d", type);
    }

    s_esp_eaf_decoder[type] = decoder;
    return ESP_OK;
}

/**
 * @brief Initialize all decoders
 */
static esp_err_t esp_eaf_init_decoders(void)
{
    esp_err_t ret = esp_eaf_register_decoder(ESP_EAF_ENCODING_RLE, esp_eaf_rle_decode);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_eaf_register_decoder(ESP_EAF_ENCODING_HUFFMAN, esp_eaf_huffman_rle_decode);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_eaf_register_decoder(ESP_EAF_ENCODING_JPEG, esp_eaf_jpeg_decode);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_eaf_register_decoder(ESP_EAF_ENCODING_HUFFMAN_DIRECT, esp_eaf_huffman_decode);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Initialize hardware JPEG decoder if available */
    ret = esp_eaf_hw_jpeg_init();
    return ret;
}

/**
 * @brief Convert BGR to RGB565
 */
static inline uint16_t esp_eaf_rgb565_from_bgr(const uint8_t *bgr, bool swap_color)
{
    uint16_t value = ((bgr[2] & 0xF8) << 8) | ((bgr[1] & 0xFC) << 3) | ((bgr[0] & 0xF8) >> 3);
    return swap_color ? __builtin_bswap16(value) : value;
}

/**
 * @brief Get the number of rows in a block
 */
static inline int esp_eaf_get_block_rows(const esp_eaf_header_t *header, int block_index)
{
    int start_row = block_index * header->block_height;
    int remaining = header->height - start_row;
    if (remaining <= 0) {
        return 0;
    }
    return remaining > header->block_height ? header->block_height : remaining;
}

#define EAF_LOSSLESS_INT_SCRATCH (64U * 1024U)
#define EAF_LOSSLESS_INT_RESERVE (32U * 1024U)

#define EAF_JOINT_SYM_ZERO   40U
#define EAF_JOINT_SYM_ESCAPE 255U

static uint16_t s_joint_rgb_delta[81];
static uint8_t s_joint_a_delta[81];
static bool s_joint_lut_ready;

static void eaf_joint_lut_init(void)
{
    for (int s = 0; s < 81; s++) {
        int dr = s / 27 - 1, dg = (s / 9) % 3 - 1, db = (s / 3) % 3 - 1, da = s % 3 - 1;
        s_joint_rgb_delta[s] = (uint16_t)(((dr & 31) << 11) | ((dg & 63) << 5) | (db & 31));
        s_joint_a_delta[s] = (uint8_t)da;
    }
    s_joint_lut_ready = true;
}

static inline uint16_t eaf_rgb565_add_per_channel(uint16_t v, uint16_t d)
{
    return (uint16_t)((((uint32_t)v & 0xF800U) + (d & 0xF800U)) & 0xF800U) |
           (uint16_t)((((uint32_t)v & 0x07E0U) + (d & 0x07E0U)) & 0x07E0U) |
           (uint16_t)((((uint32_t)v & 0x001FU) + (d & 0x001FU)) & 0x001FU);
}

static inline uint32_t eaf_rgb565x2_add_per_channel(uint32_t w, uint32_t d)
{
    return (((w & 0xF800F800U) + (d & 0xF800F800U)) & 0xF800F800U) |
           (((w & 0x07E007E0U) + (d & 0x07E007E0U)) & 0x07E007E0U) |
           (((w & 0x001F001FU) + (d & 0x001F001FU)) & 0x001F001FU);
}

static esp_err_t eaf_lossless_restore_joint(const uint8_t *mask, size_t mask_bytes, int total_tiles, int tiles_x,
                                            int width, const uint8_t *sym, const uint8_t *esc, uint32_t n_esc,
                                            uint16_t *fb16, uint8_t *alpha_dst, bool swap_color,
                                            uint16_t *min_x, uint16_t *min_y, uint16_t *max_x, uint16_t *max_y)
{
    if (!s_joint_lut_ready) {
        eaf_joint_lut_init();
    }
    const uint8_t *esc_lo = esc;
    const uint8_t *esc_hi = esc + n_esc;
    const uint8_t *esc_a = esc + 2 * (size_t)n_esc;
    uint32_t esc_i = 0;
    for (size_t byte_idx = 0; byte_idx < mask_bytes; byte_idx++) {
        uint8_t mbyte = mask[byte_idx];
        while (mbyte != 0) {
            int bit = __builtin_ctz((unsigned)mbyte);
            mbyte &= (uint8_t)(mbyte - 1);
            int t = (int)(byte_idx << 3) + bit;
            if (t >= total_tiles) {
                break;
            }
            uint16_t ty = (t / tiles_x) * 4;
            uint16_t tx = (t % tiles_x) * 4;
            if (tx < *min_x) *min_x = tx;
            if (ty < *min_y) *min_y = ty;
            if (tx + 3 > *max_x) *max_x = tx + 3;
            if (ty + 3 > *max_y) *max_y = ty + 3;

            for (int r = 0; r < 4; r++, sym += 4) {
                if (read_le32(sym) == EAF_JOINT_SYM_ZERO * 0x01010101U) {
                    continue;
                }
                size_t row_off = (size_t)(ty + r) * width + tx;
                uint16_t *dst_c = fb16 + row_off;
                uint8_t *dst_a = alpha_dst ? (alpha_dst + row_off) : NULL;
                uint32_t s4 = read_le32(sym);
                /* Any byte >= 81 (escape or invalid) takes the per-pixel path */
                bool slow = ((((s4 & 0x7F7F7F7FU) + 0x2F2F2F2FU) | s4) & 0x80808080U) != 0;
                if (!slow && !swap_color) {
                    uint8_t s0 = (uint8_t)s4, s1 = (uint8_t)(s4 >> 8), s2 = (uint8_t)(s4 >> 16), s3 = (uint8_t)(s4 >> 24);
                    uint32_t *c32 = (uint32_t *)dst_c;
                    c32[0] = eaf_rgb565x2_add_per_channel(c32[0], (uint32_t)s_joint_rgb_delta[s0] |
                                                          ((uint32_t)s_joint_rgb_delta[s1] << 16));
                    c32[1] = eaf_rgb565x2_add_per_channel(c32[1], (uint32_t)s_joint_rgb_delta[s2] |
                                                          ((uint32_t)s_joint_rgb_delta[s3] << 16));
                    if (dst_a) {
                        uint32_t d = (uint32_t)s_joint_a_delta[s0] | ((uint32_t)s_joint_a_delta[s1] << 8) |
                                     ((uint32_t)s_joint_a_delta[s2] << 16) | ((uint32_t)s_joint_a_delta[s3] << 24);
                        uint32_t a = *(uint32_t *)dst_a;
                        *(uint32_t *)dst_a = ((a & 0x7F7F7F7FU) + (d & 0x7F7F7F7FU)) ^ ((a ^ d) & 0x80808080U);
                    }
                    continue;
                }
                for (int k = 0; k < 4; k++) {
                    uint8_t s = sym[k];
                    if (s == EAF_JOINT_SYM_ZERO) {
                        continue;
                    }
                    if (s == EAF_JOINT_SYM_ESCAPE) {
                        if (esc_i >= n_esc) {
                            return ESP_ERR_INVALID_SIZE;
                        }
                        uint16_t v = (uint16_t)esc_lo[esc_i] | ((uint16_t)esc_hi[esc_i] << 8);
                        dst_c[k] = swap_color ? __builtin_bswap16(v) : v;
                        if (dst_a) {
                            dst_a[k] = esc_a[esc_i];
                        }
                        esc_i++;
                    } else if (s < 81) {
                        uint16_t v = swap_color ? __builtin_bswap16(dst_c[k]) : dst_c[k];
                        v = eaf_rgb565_add_per_channel(v, s_joint_rgb_delta[s]);
                        dst_c[k] = swap_color ? __builtin_bswap16(v) : v;
                        if (dst_a) {
                            dst_a[k] = (uint8_t)(dst_a[k] + s_joint_a_delta[s]);
                        }
                    } else {
                        return ESP_ERR_INVALID_SIZE;
                    }
                }
            }
        }
    }
    return esc_i == n_esc ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t esp_eaf_block_decode_ctx(esp_eaf_decoder_t *decoder, const esp_eaf_header_t *header, const uint8_t *frame_data,
                               int block_index, uint8_t *decode_buffer, uint8_t *alpha_buffer,
                               bool swap_color, esp_eaf_palette_cache_t *cache)
{
    if (!decoder || !header || !frame_data || !decode_buffer || !header->block_len ||
        block_index < 0 || block_index >= header->blocks || !header->width || !header->height) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t offset = header->data_offset;
    for (int i = 0; i < block_index; i++) offset += header->block_len[i];
    const uint8_t *block_data = frame_data + offset;
    size_t block_len = header->block_len[block_index];
    int width = header->width;
    int block_height = esp_eaf_get_block_rows(header, block_index);
    if (block_index == 0) {
        decoder->dirty.x1 = width;
        decoder->dirty.y1 = header->height;
        decoder->dirty.x2 = 0;
        decoder->dirty.y2 = 0;
        decoder->dirty.valid = false;
        decoder->dirty_band_count = 0;
    }
    if (block_len == 0) {
        return ESP_OK;
    }
    uint8_t encoding_type = block_data[0];

    esp_err_t decode_result = ESP_FAIL;

    if ((encoding_type == ESP_EAF_ENCODING_TILE_JPEG || encoding_type == ESP_EAF_ENCODING_TILE_ABBREV ||
         encoding_type == ESP_EAF_ENCODING_QUAD_ABBREV) &&
        (width % 16 || block_height % 16 || header->blocks != 1)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (encoding_type == ESP_EAF_ENCODING_TILE_JPEG || encoding_type == ESP_EAF_ENCODING_TILE_ABBREV) {
        /* 16x16 Tile-Atlas Delta JPEG: first 29 bytes = 225-bit tile mask, followed by packed JPEG or abbreviated scan */
        int tiles_x = width / 16;
        int tiles_y = block_height / 16;
        int total_tiles = tiles_x * tiles_y;
        int mask_bytes = (total_tiles + 7) / 8;
        if (block_len < 1 + mask_bytes) {
            return ESP_FAIL;
        }
        const uint8_t *mask = block_data + 1;
        int changed_tiles = 0;
        uint16_t min_x = width, min_y = header->height, max_x = 0, max_y = 0;
        for (int t = 0; t < total_tiles; t++) {
            if (mask[t >> 3] & (1 << (t & 7))) {
                changed_tiles++;
                uint16_t ty = (t / tiles_x) * 16;
                uint16_t tx = (t % tiles_x) * 16;
                if (tx < min_x) min_x = tx;
                if (ty < min_y) min_y = ty;
                if (tx + 15 > max_x) max_x = tx + 15;
                if (ty + 15 > max_y) max_y = ty + 15;
            }
        }
        if (changed_tiles == 0) {
            return ESP_OK;
        }
        decoder->dirty.x1 = min_x;
        decoder->dirty.y1 = min_y;
        decoder->dirty.x2 = max_x;
        decoder->dirty.y2 = max_y;
        decoder->dirty.valid = true;

        const uint8_t *jpg_ptr = block_data + 1 + mask_bytes;
        size_t jpg_len = block_len - 1 - mask_bytes;
        int ncols = (encoding_type == ESP_EAF_ENCODING_TILE_ABBREV) ? changed_tiles : (changed_tiles < tiles_x ? changed_tiles : tiles_x);
        int nrows = (encoding_type == ESP_EAF_ENCODING_TILE_ABBREV) ? 1 : ((changed_tiles + ncols - 1) / ncols);
        int atlas_w = ncols * 16;
        int atlas_h = nrows * 16;
        size_t atlas_size = (size_t)atlas_w * atlas_h * 2;
        size_t alloc_sz = 0;
        uint8_t *atlas_buf = (uint8_t *)esp_eaf_malloc_aligned_prefer_psram(atlas_size, 0, &alloc_sz);
        if (!atlas_buf) {
            return ESP_ERR_NO_MEM;
        }

        uint8_t *rebuilt_jpg = NULL;
        if (encoding_type == ESP_EAF_ENCODING_TILE_ABBREV) {
            if (decoder->jpeg_hdr_len == 0 || decoder->sof0_offset < 0) {
                free(atlas_buf);
                return ESP_FAIL;
            }
            size_t full_len = decoder->jpeg_hdr_len + jpg_len + 2;
            rebuilt_jpg = (uint8_t *)esp_eaf_malloc_prefer_psram(full_len);
            if (!rebuilt_jpg) {
                free(atlas_buf);
                return ESP_ERR_NO_MEM;
            }
            memcpy(rebuilt_jpg, decoder->jpeg_hdr, decoder->jpeg_hdr_len);
            rebuilt_jpg[decoder->sof0_offset + 5] = (atlas_h >> 8) & 0xFF;
            rebuilt_jpg[decoder->sof0_offset + 6] = atlas_h & 0xFF;
            rebuilt_jpg[decoder->sof0_offset + 7] = (atlas_w >> 8) & 0xFF;
            rebuilt_jpg[decoder->sof0_offset + 8] = atlas_w & 0xFF;
            memcpy(rebuilt_jpg + decoder->jpeg_hdr_len, jpg_ptr, jpg_len);
            rebuilt_jpg[decoder->jpeg_hdr_len + jpg_len] = 0xFF;
            rebuilt_jpg[decoder->jpeg_hdr_len + jpg_len + 1] = 0xD9;
            jpg_ptr = rebuilt_jpg;
            jpg_len = full_len;
        }

        size_t dec_sz = alloc_sz;
        decode_result = esp_eaf_jpeg_decode(jpg_ptr, jpg_len, atlas_buf, &dec_sz, swap_color);
        if (rebuilt_jpg) {
            free(rebuilt_jpg);
        }
        if (decode_result == ESP_OK && dec_sz != atlas_size) decode_result = ESP_ERR_INVALID_SIZE;
        if (decode_result == ESP_OK) {
            int k = 0;
            for (int t = 0; t < total_tiles; t++) {
                if (mask[t >> 3] & (1 << (t & 7))) {
                    int ty = (t / tiles_x) * 16;
                    int tx = (t % tiles_x) * 16;
                    int ar = (k / ncols) * 16;
                    int ac = (k % ncols) * 16;
                    for (int row = 0; row < 16; row++) {
                        uint8_t *dst_row = decode_buffer + ((ty + row) * width + tx) * 2;
                        const uint8_t *src_row = atlas_buf + ((ar + row) * atlas_w + ac) * 2;
                        memcpy(dst_row, src_row, 16 * 2);
                    }
                    k++;
                }
            }
        }
        free(atlas_buf);
        return decode_result;
    }

    if (encoding_type == ESP_EAF_ENCODING_PLANAR_DELTA) {
        /* Full frame: type(1), flags(1), raw Deflate of low plane then high plane.
         * flags bit 0: XOR previous RGB565; bit 1: uint16 horizontal difference.
         * Only even widths are supported, so pairs can be restored with word copies.
         */
        if (block_len < 3 || header->blocks != 1 || width % 2 || block_data[1] > 3) {
            return ESP_ERR_INVALID_SIZE;
        }
        unsigned mode = block_data[1];
        size_t pixels = (size_t)width * block_height;
        if (pixels > SIZE_MAX / 2) return ESP_ERR_INVALID_SIZE;
        size_t raw_len = pixels * 2;
        if (raw_len > UINT_MAX || block_len - 2 > UINT_MAX) return ESP_ERR_INVALID_SIZE;
        bool previous_valid = decoder->predictor_ready && decoder->predictor_width == width &&
                              decoder->predictor_height == block_height && decoder->predictor_swap == swap_color;
        decoder->predictor_ready = false;
        if ((mode & 1) && !previous_valid) return ESP_ERR_INVALID_STATE;
        if (decoder->predictor_capacity < raw_len) {
            free(decoder->predictor_scratch);
            decoder->predictor_capacity = 0;
            decoder->predictor_scratch = heap_caps_malloc(raw_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            bool internal = decoder->predictor_scratch != NULL;
            if (!decoder->predictor_scratch) {
                decoder->predictor_scratch = esp_eaf_malloc_prefer_psram(raw_len);
            }
            if (!decoder->predictor_scratch) return ESP_ERR_NO_MEM;
            decoder->predictor_capacity = raw_len;
            ESP_LOGI(TAG, "PLANAR_BUFFER bytes=%u memory=%s", (unsigned)raw_len, internal ? "internal" : "fallback");
        }
        uint8_t *raw = decoder->predictor_scratch;
        int64_t inflate_start = esp_timer_get_time();
        if (decoder->predictor_rom) {
            if (tinfl_decompress_mem_to_mem(raw, raw_len, block_data + 2, block_len - 2, 0) != raw_len) return ESP_FAIL;
        } else {
            z_stream *z = &decoder->predictor_z;
            if (!decoder->predictor_z_ready) {
                z->zalloc = planar_zalloc;
                z->zfree = planar_zfree;
                if (inflateInit2(z, -15) != Z_OK) return ESP_ERR_NO_MEM;
                decoder->predictor_z_ready = true;
            } else if (inflateReset(z) != Z_OK) return ESP_FAIL;
            z->next_in = (Bytef *)(block_data + 2);
            z->avail_in = block_len - 2;
            z->next_out = raw;
            z->avail_out = raw_len;
            if (inflate(z, Z_FINISH) != Z_STREAM_END || z->total_out != raw_len || z->avail_in != 0) return ESP_FAIL;
        }
        int64_t restore_start = esp_timer_get_time();
        uint16_t *fb = (uint16_t *)decode_buffer;
        size_t pos = 0;
        if (!decoder->predictor_rom && !(mode & 2) && !swap_color && ((uintptr_t)fb & 3) == 0) {
            /* malloc scratch is aligned; even width keeps the high plane aligned.
             * may_alias permits word stores into the caller's RGB565 framebuffer.
             */
            typedef uint16_t alias16 __attribute__((__may_alias__));
            typedef uint32_t alias32 __attribute__((__may_alias__));
            const alias16 *low = (const alias16 *)raw;
            const alias16 *high = (const alias16 *)(raw + pixels);
            alias32 *dst = (alias32 *)fb;
            if (mode & 1) {
                for (size_t i = 0; i < pixels / 2; i++) {
                    uint32_t l = low[i], h = high[i];
                    dst[i] ^= (l & 255) | ((h & 255) << 8) | ((l & 0xff00) << 8) | ((h & 0xff00) << 16);
                }
            } else {
                for (size_t i = 0; i < pixels / 2; i++) {
                    uint32_t l = low[i], h = high[i];
                    dst[i] = (l & 255) | ((h & 255) << 8) | ((l & 0xff00) << 8) | ((h & 0xff00) << 16);
                }
            }
        } else {
        for (int y = 0; y < block_height; y++) {
            uint16_t acc = 0;
            for (int x = 0; x < width; x += 2, pos += 2) {
                uint16_t a = raw[pos] | ((uint16_t)raw[pixels + pos] << 8);
                uint16_t b = raw[pos + 1] | ((uint16_t)raw[pixels + pos + 1] << 8);
                if (mode & 2) { acc += a; a = acc; acc += b; b = acc; }
                uint32_t pair = (uint32_t)a | ((uint32_t)b << 16);
                if (swap_color) pair = ((pair & 0x00ff00ffu) << 8) | ((pair & 0xff00ff00u) >> 8);
                if (mode & 1) {
                    uint32_t previous;
                    memcpy(&previous, fb + pos, sizeof(previous));
                    pair ^= previous;
                }
                memcpy(fb + pos, &pair, sizeof(pair));
            }
        }
        }
        decoder->predictor_stats.inflate_us += restore_start - inflate_start;
        decoder->predictor_stats.restore_us += esp_timer_get_time() - restore_start;
        decoder->predictor_stats.frames++;
        decoder->predictor_ready = true;
        decoder->predictor_width = width;
        decoder->predictor_height = block_height;
        decoder->predictor_swap = swap_color;
        decoder->dirty.x1 = decoder->dirty.y1 = 0;
        decoder->dirty.x2 = width - 1;
        decoder->dirty.y2 = block_height - 1;
        decoder->dirty.valid = true;
        return ESP_OK;
    }

    if (encoding_type == ESP_EAF_ENCODING_BTC_SAFE) {
        if (block_len < 5 || width % 4 || block_height % 4) return ESP_ERR_INVALID_SIZE;
        size_t tiles = (size_t)(width / 4) * (block_height / 4);
        size_t raw_len = read_le32(block_data + 1);
        if (raw_len < tiles || raw_len > tiles * 33) return ESP_ERR_INVALID_SIZE;
        uint8_t *raw = heap_caps_malloc(raw_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!raw) raw = esp_eaf_malloc_prefer_psram(raw_len);
        if (!raw) return ESP_ERR_NO_MEM;
        if (tinfl_decompress_mem_to_mem(raw, raw_len, block_data + 5, block_len - 5, 0) != raw_len) {
            free(raw);
            return ESP_FAIL;
        }
        const size_t sizes[] = {0, 2, 6, 8, 32};
        size_t expected = tiles;
        for (size_t t = 0; t < tiles; t++) {
            if (raw[t] > 4) { free(raw); return ESP_ERR_INVALID_SIZE; }
            expected += sizes[raw[t]];
        }
        if (expected != raw_len) { free(raw); return ESP_ERR_INVALID_SIZE; }
        const uint8_t *p = raw + tiles;
        uint16_t *fb = (uint16_t *)decode_buffer;
        for (size_t t = 0; t < tiles; t++) {
            unsigned mode = raw[t];
            if (!mode) continue;
            int tx = (t % (width / 4)) * 4, ty = (t / (width / 4)) * 4;
            uint16_t pal[4] = {0};
            uint32_t mask = 0;
            if (mode < 4) {
                pal[0] = read_le16(p);
                if (mode == 2) { pal[1] = read_le16(p + 2); mask = read_le16(p + 4); }
                if (mode == 3) {
                    pal[3] = read_le16(p + 2); mask = read_le32(p + 4);
                    int r0 = pal[0] >> 11, g0 = (pal[0] >> 5) & 63, b0 = pal[0] & 31;
                    int r3 = pal[3] >> 11, g3 = (pal[3] >> 5) & 63, b3 = pal[3] & 31;
                    pal[1] = (((2*r0+r3)/3) << 11) | (((2*g0+g3)/3) << 5) | ((2*b0+b3)/3);
                    pal[2] = (((r0+2*r3)/3) << 11) | (((g0+2*g3)/3) << 5) | ((b0+2*b3)/3);
                }
            }
            for (int pixel = 0; pixel < 16; pixel++) {
                uint16_t color = mode == 4 ? read_le16(p + pixel * 2) :
                    pal[mode == 1 ? 0 : mode == 2 ? ((mask >> pixel) & 1) : ((mask >> (pixel * 2)) & 3)];
                fb[(ty + pixel / 4) * width + tx + pixel % 4] = swap_color ? __builtin_bswap16(color) : color;
            }
            p += sizes[mode];
            int frame_y = ty + block_index * header->block_height;
            if (tx < decoder->dirty.x1) decoder->dirty.x1 = tx;
            if (frame_y < decoder->dirty.y1) decoder->dirty.y1 = frame_y;
            if (tx + 3 > decoder->dirty.x2) decoder->dirty.x2 = tx + 3;
            if (frame_y + 3 > decoder->dirty.y2) decoder->dirty.y2 = frame_y + 3;
            decoder->dirty.valid = true;
        }
        free(raw);
        return ESP_OK;
    }

    if (encoding_type == ESP_EAF_ENCODING_BTC_ZLIB) {
        /* 4x4 Adaptive Delta-BTC + ESP32 ROM tinfl_decompress_mem_to_mem:
         * Header after encoding_type(1B):
         *   uint16_t raw_len, uint16_t n_flat, uint16_t n_btc1, uint16_t n_btc2
         * Followed by raw deflate payload of size (block_len - 9).
         * Decompressed layout:
         *   modes[(total_4x4 + 3)/4] (2 bits per 4x4 tile: 0=skip, 1=flat, 2=1bit-btc, 3=2bit-dxt1)
         *   endpoints[(n_flat + (n_btc1 + n_btc2)*2) * 2] (uint16_t RGB565)
         *   masks16[n_btc1 * 2] (uint16_t 16-bit mask)
         *   masks32[n_btc2 * 4] (uint32_t 32-bit 2bpp mask)
         */
        if (block_len < 9 || width % 4 || block_height % 4) {
            return ESP_FAIL;
        }
        uint16_t raw_len = read_le16(block_data + 1);
        uint16_t n_flat = read_le16(block_data + 3);
        uint16_t n_btc1 = read_le16(block_data + 5);
        uint16_t n_btc2 = read_le16(block_data + 7);
        int tiles_x = width / 4;
        int tiles_y = block_height / 4;
        int total_tiles = tiles_x * tiles_y;
        int modes_len = (total_tiles + 3) / 4;

        size_t expected = modes_len + (size_t)n_flat * 2 + (size_t)n_btc1 * 6 + (size_t)n_btc2 * 8;
        if (expected != raw_len || (size_t)n_flat + n_btc1 + n_btc2 > (size_t)total_tiles) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t *raw_buf = (uint8_t *)heap_caps_malloc(raw_len + 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!raw_buf) {
            raw_buf = (uint8_t *)esp_eaf_malloc_prefer_psram(raw_len + 4);
        }
        if (!raw_buf) {
            return ESP_ERR_NO_MEM;
        }
        size_t decomp_sz = tinfl_decompress_mem_to_mem(
            raw_buf, raw_len, block_data + 9, block_len - 9, 0
        );
        if (decomp_sz != raw_len) {
            free(raw_buf);
            return ESP_FAIL;
        }

        const uint8_t *modes = raw_buf;
        unsigned counts[4] = {0};
        for (int t = 0; t < total_tiles; t++) counts[(modes[t >> 2] >> ((t & 3) * 2)) & 3]++;
        if (counts[1] != n_flat || counts[2] != n_btc1 || counts[3] != n_btc2) {
            free(raw_buf);
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t *ep_ptr = raw_buf + modes_len;
        const uint8_t *mask16_ptr = ep_ptr + (n_flat + (size_t)(n_btc1 + n_btc2) * 2) * 2;
        const uint8_t *mask32_ptr = mask16_ptr + (size_t)n_btc1 * 2;
        uint16_t *fb16 = (uint16_t *)decode_buffer;
        uint16_t min_x = width, min_y = header->height, max_x = 0, max_y = 0;

        for (int t = 0; t < total_tiles; t++) {
            uint8_t mode = (modes[t >> 2] >> ((t & 3) * 2)) & 0x03;
            if (mode == 0) {
                continue;
            }
            uint16_t ty = (t / tiles_x) * 4;
            uint16_t tx = (t % tiles_x) * 4;
            if (tx < min_x) min_x = tx;
            if (ty < min_y) min_y = ty;
            if (tx + 3 > max_x) max_x = tx + 3;
            if (ty + 3 > max_y) max_y = ty + 3;

            if (mode == 1) {
                uint16_t c = read_le16(ep_ptr);
                ep_ptr += 2;
                if (swap_color) c = __builtin_bswap16(c);
                uint32_t c2 = ((uint32_t)c << 16) | c;
                for (int r = 0; r < 4; r++) {
                    uint32_t *row32 = (uint32_t *)(fb16 + (ty + r) * width + tx);
                    row32[0] = c2;
                    row32[1] = c2;
                }
            } else if (mode == 2) {
                uint16_t c0 = read_le16(ep_ptr);
                uint16_t c1 = read_le16(ep_ptr + 2);
                ep_ptr += 4;
                if (swap_color) {
                    c0 = __builtin_bswap16(c0);
                    c1 = __builtin_bswap16(c1);
                }
                uint16_t m16 = read_le16(mask16_ptr);
                mask16_ptr += 2;
                for (int r = 0; r < 4; r++) {
                    uint16_t *dst = fb16 + (ty + r) * width + tx;
                    dst[0] = (m16 & 1) ? c1 : c0;
                    dst[1] = (m16 & 2) ? c1 : c0;
                    dst[2] = (m16 & 4) ? c1 : c0;
                    dst[3] = (m16 & 8) ? c1 : c0;
                    m16 >>= 4;
                }
            } else if (mode == 3) {
                uint16_t c0 = read_le16(ep_ptr);
                uint16_t c3 = read_le16(ep_ptr + 2);
                ep_ptr += 4;
                int r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
                int r3 = (c3 >> 11) & 0x1F, g3 = (c3 >> 5) & 0x3F, b3 = c3 & 0x1F;
                uint16_t c1 = (uint16_t)((((2 * r0 + r3) / 3) << 11) | (((2 * g0 + g3) / 3) << 5) | ((2 * b0 + b3) / 3));
                uint16_t c2 = (uint16_t)((((r0 + 2 * r3) / 3) << 11) | (((g0 + 2 * g3) / 3) << 5) | ((b0 + 2 * b3) / 3));
                if (swap_color) {
                    c0 = __builtin_bswap16(c0);
                    c1 = __builtin_bswap16(c1);
                    c2 = __builtin_bswap16(c2);
                    c3 = __builtin_bswap16(c3);
                }
                uint16_t pal[4] = {c0, c1, c2, c3};
                uint32_t m32;
                memcpy(&m32, mask32_ptr, 4);
                mask32_ptr += 4;
                for (int r = 0; r < 4; r++) {
                    uint16_t *dst = fb16 + (ty + r) * width + tx;
                    dst[0] = pal[m32 & 3];
                    dst[1] = pal[(m32 >> 2) & 3];
                    dst[2] = pal[(m32 >> 4) & 3];
                    dst[3] = pal[(m32 >> 6) & 3];
                    m32 >>= 8;
                }
            }
        }
        if (max_x >= min_x) {
            min_y += block_index * header->block_height;
            max_y += block_index * header->block_height;
            if (min_x < decoder->dirty.x1) decoder->dirty.x1 = min_x;
            if (min_y < decoder->dirty.y1) decoder->dirty.y1 = min_y;
            if (max_x > decoder->dirty.x2) decoder->dirty.x2 = max_x;
            if (max_y > decoder->dirty.y2) decoder->dirty.y2 = max_y;
            decoder->dirty.valid = true;
        }
        free(raw_buf);
        return ESP_OK;
    }

    if (encoding_type == ESP_EAF_ENCODING_BTC_ALPHA) {
        /* 4x4 Delta-BTC + 8-bit BC4/DXT5 continuous Alpha (0x0B):
         * Header after encoding_type(1B):
         *   uint32_t raw_len, uint16_t n_flat, uint16_t n_interp
         * Followed by raw deflate payload of size (block_len - 9).
         * Decompressed layout:
         *   modes[(total_4x4 + 3)/4] (2 bits per 4x4 tile: 0=skip, 1=flat RGBA, 3=2bpp DXT5 RGBA)
         *   flat_c[n_flat] (uint16_t RGB565)
         *   flat_a[n_flat] (uint8_t A8)
         *   interp_c[n_interp * 2] (uint16_t c0, c3)
         *   interp_a[n_interp * 2] (uint8_t a0, a3)
         *   masks_c[n_interp] (uint32_t 2bpp RGB mask)
         *   masks_a[n_interp] (uint32_t 2bpp Alpha mask)
         */
        if (block_len < 9 || width % 4 || block_height % 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t raw_len = read_le32(block_data + 1);
        uint16_t n_flat = read_le16(block_data + 5);
        uint16_t n_interp = read_le16(block_data + 7);
        int tiles_x = width / 4;
        int tiles_y = block_height / 4;
        int total_tiles = tiles_x * tiles_y;
        size_t modes_len = (size_t)(total_tiles + 3) / 4;
        size_t expected = modes_len + (size_t)n_flat * 3 + (size_t)n_interp * 14;
        if (expected != raw_len || (size_t)n_flat + n_interp > (size_t)total_tiles) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t *raw_buf = (uint8_t *)heap_caps_malloc(raw_len + 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!raw_buf) {
            raw_buf = (uint8_t *)esp_eaf_malloc_prefer_psram(raw_len + 4);
        }
        if (!raw_buf) {
            return ESP_ERR_NO_MEM;
        }
        size_t decomp_sz = tinfl_decompress_mem_to_mem(raw_buf, raw_len, block_data + 9, block_len - 9, 0);
        if (decomp_sz != raw_len) {
            free(raw_buf);
            return ESP_FAIL;
        }
        const uint8_t *modes = raw_buf;
        unsigned counts[4] = {0};
        for (int t = 0; t < total_tiles; t++) {
            uint8_t m = (modes[t >> 2] >> ((t & 3) * 2)) & 0x03;
            if (m == 2) {
                free(raw_buf);
                return ESP_ERR_INVALID_SIZE;
            }
            counts[m]++;
        }
        if (counts[1] != n_flat || counts[3] != n_interp) {
            free(raw_buf);
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t *flat_c_ptr = modes + modes_len;
        const uint8_t *flat_a_ptr = flat_c_ptr + (size_t)n_flat * 2;
        const uint8_t *interp_c_ptr = flat_a_ptr + n_flat;
        const uint8_t *interp_a_ptr = interp_c_ptr + (size_t)n_interp * 4;
        const uint8_t *mask_c_ptr = interp_a_ptr + (size_t)n_interp * 2;
        const uint8_t *mask_a_ptr = mask_c_ptr + (size_t)n_interp * 4;

        uint16_t *fb16 = (uint16_t *)decode_buffer;
        uint8_t *alpha_dst = alpha_buffer ? (alpha_buffer + (size_t)block_index * header->block_height * width) : NULL;
        uint16_t min_x = width, min_y = header->height, max_x = 0, max_y = 0;

        for (int t = 0; t < total_tiles; t++) {
            uint8_t mode = (modes[t >> 2] >> ((t & 3) * 2)) & 0x03;
            if (mode == 0) {
                continue;
            }
            uint16_t ty = (t / tiles_x) * 4;
            uint16_t tx = (t % tiles_x) * 4;
            if (tx < min_x) min_x = tx;
            if (ty < min_y) min_y = ty;
            if (tx + 3 > max_x) max_x = tx + 3;
            if (ty + 3 > max_y) max_y = ty + 3;

            if (mode == 1) {
                uint16_t c = read_le16(flat_c_ptr);
                flat_c_ptr += 2;
                uint8_t a = *flat_a_ptr++;
                if (swap_color) c = __builtin_bswap16(c);
                uint32_t c2 = ((uint32_t)c << 16) | c;
                uint32_t a4 = (uint32_t)a * 0x01010101u;
                for (int r = 0; r < 4; r++) {
                    size_t row_off = (size_t)(ty + r) * width + tx;
                    uint32_t *row32 = (uint32_t *)(fb16 + row_off);
                    row32[0] = c2;
                    row32[1] = c2;
                    if (alpha_dst) {
                        *(uint32_t *)(alpha_dst + row_off) = a4;
                    }
                }
            } else if (mode == 3) {
                uint16_t c0 = read_le16(interp_c_ptr);
                uint16_t c3 = read_le16(interp_c_ptr + 2);
                interp_c_ptr += 4;
                int r0 = (c0 >> 11) & 0x1F, g0 = (c0 >> 5) & 0x3F, b0 = c0 & 0x1F;
                int r3 = (c3 >> 11) & 0x1F, g3 = (c3 >> 5) & 0x3F, b3 = c3 & 0x1F;
                uint16_t c1 = (uint16_t)((((2 * r0 + r3) / 3) << 11) | (((2 * g0 + g3) / 3) << 5) | ((2 * b0 + b3) / 3));
                uint16_t c2 = (uint16_t)((((r0 + 2 * r3) / 3) << 11) | (((g0 + 2 * g3) / 3) << 5) | ((b0 + 2 * b3) / 3));
                if (swap_color) {
                    c0 = __builtin_bswap16(c0);
                    c1 = __builtin_bswap16(c1);
                    c2 = __builtin_bswap16(c2);
                    c3 = __builtin_bswap16(c3);
                }
                uint16_t pal_c[4] = {c0, c1, c2, c3};

                uint8_t a0 = interp_a_ptr[0];
                uint8_t a3 = interp_a_ptr[1];
                interp_a_ptr += 2;
                uint8_t pal_a[4] = {
                    a0,
                    (uint8_t)((2 * a0 + a3) / 3),
                    (uint8_t)((a0 + 2 * a3) / 3),
                    a3,
                };

                uint32_t mc = read_le32(mask_c_ptr);
                mask_c_ptr += 4;
                uint32_t ma = read_le32(mask_a_ptr);
                mask_a_ptr += 4;

                for (int r = 0; r < 4; r++) {
                    size_t row_off = (size_t)(ty + r) * width + tx;
                    uint16_t *dst_c = fb16 + row_off;
                    dst_c[0] = pal_c[mc & 3];
                    dst_c[1] = pal_c[(mc >> 2) & 3];
                    dst_c[2] = pal_c[(mc >> 4) & 3];
                    dst_c[3] = pal_c[(mc >> 6) & 3];
                    mc >>= 8;
                    if (alpha_dst) {
                        uint8_t *dst_a = alpha_dst + row_off;
                        dst_a[0] = pal_a[ma & 3];
                        dst_a[1] = pal_a[(ma >> 2) & 3];
                        dst_a[2] = pal_a[(ma >> 4) & 3];
                        dst_a[3] = pal_a[(ma >> 6) & 3];
                        ma >>= 8;
                    }
                }
            }
        }
        if (max_x >= min_x) {
            min_y += block_index * header->block_height;
            max_y += block_index * header->block_height;
            if (min_x < decoder->dirty.x1) decoder->dirty.x1 = min_x;
            if (min_y < decoder->dirty.y1) decoder->dirty.y1 = min_y;
            if (max_x > decoder->dirty.x2) decoder->dirty.x2 = max_x;
            if (max_y > decoder->dirty.y2) decoder->dirty.y2 = max_y;
            decoder->dirty.valid = true;
        }
        free(raw_buf);
        return ESP_OK;
    }

    if (encoding_type == ESP_EAF_ENCODING_LOSSLESS_RGBA) {
        /* 100% Bit-Exact Lossless 4x4 Temporal-XOR RGB565 + A8 (0x0C):
         * Header after encoding_type(1B):
         *   uint8_t flags (bit 0: temporal XOR/delta from previous frame; 0 = keyframe overwrite)
         *   uint32_t raw_len
         *   uint16_t n_changed (number of changed 4x4 tiles)
         * Followed by raw deflate payload of size (block_len - 8).
         * Decompressed layout:
         *   mask[(total_4x4 + 7)/8] (1 bit per 4x4 tile: 1 = changed, 0 = unchanged)
         *   c_low[n_changed * 16]   (low byte of RGB565 or XOR delta)
         *   c_high[n_changed * 16]  (high byte of RGB565 or XOR delta)
         *   a_plane[n_changed * 16] (A8 byte or uint8 additive delta)
         *
         * flags == 3 (inter frame, joint-symbol layout) extends the header:
         *   uint32_t n_esc      (pixels stored as absolute values)
         *   uint32_t sym_comp   (size of the first deflate stream)
         * Followed by two raw deflate streams:
         *   1) mask[(total_4x4 + 7)/8] + sym[n_changed * 16]
         *      sym 0..80 = (dr+1)*27 + (dg+1)*9 + (db+1)*3 + (da+1), each delta in -1..1
         *      per channel (R5/G6/B5 wrap within the channel, A8 wraps mod 256);
         *      sym 255 = escape, pixel is taken from the escape stream
         *   2) esc_low[n_esc] + esc_high[n_esc] + esc_a[n_esc] (absolute RGB565 + A8)
         */
        if (block_len < 8 || width % 4 || block_height % 4) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t flags = block_data[1];
        if (flags != 0 && flags != 1 && flags != 3) {
            return ESP_ERR_INVALID_SIZE;
        }
        bool joint = (flags & 2) != 0;
        uint32_t raw_len = read_le32(block_data + 2);
        uint16_t n_changed = read_le16(block_data + 6);
        int tiles_x = width / 4;
        int tiles_y = block_height / 4;
        int total_tiles = tiles_x * tiles_y;
        size_t mask_bytes = (size_t)(total_tiles + 7) / 8;
        size_t hdr_len = 8;
        uint32_t n_esc = 0;
        uint32_t sym_comp = 0;
        if (joint) {
            if (block_len < 16) {
                return ESP_ERR_INVALID_SIZE;
            }
            n_esc = read_le32(block_data + 8);
            sym_comp = read_le32(block_data + 12);
            hdr_len = 16;
            if (sym_comp > block_len - hdr_len || n_esc > (uint32_t)n_changed * 16) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
        size_t expected = mask_bytes + (size_t)n_changed * (joint ? 16 : 48);
        if (expected != raw_len || n_changed > total_tiles) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t scratch_need = (size_t)raw_len + (size_t)n_esc * 3;
        uint8_t *raw_buf = NULL;
        /* Fixed internal-RAM scratch for typical inter blocks; larger blocks (keyframes) use PSRAM */
        if (!decoder->lossless_int_tried) {
            decoder->lossless_int_tried = true;
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >
                    EAF_LOSSLESS_INT_SCRATCH + EAF_LOSSLESS_INT_RESERVE) {
                esp_eaf_decoder_reserve_internal_scratch(decoder, EAF_LOSSLESS_INT_SCRATCH);
            }
        }
        if (scratch_need + 4 <= decoder->lossless_int_capacity) {
            raw_buf = decoder->lossless_int_scratch;
        }
        if (!raw_buf) {
            if (decoder->predictor_capacity < scratch_need + 4) {
                free(decoder->predictor_scratch);
                decoder->predictor_capacity = 0;
                decoder->predictor_scratch = esp_eaf_malloc_prefer_psram(scratch_need + 4);
                if (!decoder->predictor_scratch) {
                    return ESP_ERR_NO_MEM;
                }
                decoder->predictor_capacity = scratch_need + 4;
            }
            raw_buf = decoder->predictor_scratch;
        }
        int64_t t_inf0 = esp_timer_get_time();
        size_t sym_in_len = joint ? sym_comp : block_len - hdr_len;
        size_t decomp_sz = tinfl_decompress_mem_to_mem(raw_buf, raw_len, block_data + hdr_len, sym_in_len, 0);
        if (decomp_sz != raw_len) {
            return ESP_FAIL;
        }
        if (n_esc) {
            size_t esc_len = (size_t)n_esc * 3;
            decomp_sz = tinfl_decompress_mem_to_mem(raw_buf + raw_len, esc_len,
                                                    block_data + hdr_len + sym_comp,
                                                    block_len - hdr_len - sym_comp, 0);
            if (decomp_sz != esc_len) {
                return ESP_FAIL;
            }
        }
        int64_t t_rest0 = esp_timer_get_time();
        decoder->predictor_stats.inflate_us += (uint64_t)(t_rest0 - t_inf0);
        const uint8_t *mask = raw_buf;
        unsigned actual_changed = 0;
        for (int t = 0; t < total_tiles; t++) {
            if (mask[t >> 3] & (1u << (t & 7))) actual_changed++;
        }
        if (actual_changed != n_changed) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t *c_low = mask + mask_bytes;
        const uint8_t *c_high = c_low + (size_t)n_changed * 16;
        const uint8_t *a_src = c_high + (size_t)n_changed * 16;

        uint16_t *fb16 = (uint16_t *)decode_buffer;
        size_t blk_px_off = (size_t)block_index * header->block_height * width;
        uint8_t *alpha_dst = alpha_buffer ? (alpha_buffer + blk_px_off) : NULL;
        const uint16_t *fused_bg = decoder->fused_bg_rgb565 ? (decoder->fused_bg_rgb565 + blk_px_off) : NULL;
        uint16_t *fused_comp = decoder->fused_comp_rgb565 ? (decoder->fused_comp_rgb565 + blk_px_off) : NULL;
        uint16_t min_x = width, min_y = header->height, max_x = 0, max_y = 0;

        /* Pass 1: Fast sequential restore of RGB565 + A8 across changed 4x4 tiles */
        if (joint) {
            esp_err_t jerr = eaf_lossless_restore_joint(mask, mask_bytes, total_tiles, tiles_x, width,
                                                        mask + mask_bytes, raw_buf + raw_len, n_esc,
                                                        fb16, alpha_dst, swap_color,
                                                        &min_x, &min_y, &max_x, &max_y);
            if (jerr != ESP_OK) {
                return jerr;
            }
        } else {
            const uint8_t *c_low_p = c_low;
            const uint8_t *c_high_p = c_high;
            const uint8_t *a_src_p = a_src;
            for (size_t byte_idx = 0; byte_idx < mask_bytes; byte_idx++) {
                uint8_t mbyte = mask[byte_idx];
                if (mbyte == 0) {
                    continue;
                }
                while (mbyte != 0) {
                    int bit = __builtin_ctz((unsigned)mbyte);
                    mbyte &= (uint8_t)(mbyte - 1);
                    int t = (int)(byte_idx << 3) + bit;
                    if (t >= total_tiles) {
                        break;
                    }
                    uint16_t ty = (t / tiles_x) * 4;
                    uint16_t tx = (t % tiles_x) * 4;
                    if (tx < min_x) min_x = tx;
                    if (ty < min_y) min_y = ty;
                    if (tx + 3 > max_x) max_x = tx + 3;
                    if (ty + 3 > max_y) max_y = ty + 3;

                    for (int r = 0; r < 4; r++) {
                        size_t row_off = (size_t)(ty + r) * width + tx;
                        uint16_t *dst_c = fb16 + row_off;
                        uint8_t *dst_a = alpha_dst ? (alpha_dst + row_off) : NULL;
                        int p = r * 4;
                        uint32_t lo32 = read_le32(c_low_p + p);
                        uint32_t hi32 = read_le32(c_high_p + p);
                        uint32_t w0, w1;
                        if (!swap_color) {
                            w0 = (lo32 & 0x00FFU) | ((hi32 & 0x00FFU) << 8) |
                                 ((lo32 & 0xFF00U) << 8) | ((hi32 & 0xFF00U) << 16);
                            w1 = ((lo32 >> 16) & 0x00FFU) | (((hi32 >> 16) & 0x00FFU) << 8) |
                                 (((lo32 >> 16) & 0xFF00U) << 8) | (((hi32 >> 16) & 0xFF00U) << 16);
                        } else {
                            w0 = ((lo32 & 0x00FFU) << 8) | (hi32 & 0x00FFU) |
                                 ((lo32 & 0xFF00U) << 16) | ((hi32 & 0xFF00U) << 8);
                            w1 = (((lo32 >> 16) & 0x00FFU) << 8) | ((hi32 >> 16) & 0x00FFU) |
                                 (((lo32 >> 16) & 0xFF00U) << 16) | (((hi32 >> 16) & 0xFF00U) << 8);
                        }
                        uint32_t *dst_c32 = (uint32_t *)dst_c;
                        if (flags & 1) {
                            dst_c32[0] ^= w0;
                            dst_c32[1] ^= w1;
                            if (dst_a) {
                                dst_a[0] = (uint8_t)(dst_a[0] + a_src_p[p + 0]);
                                dst_a[1] = (uint8_t)(dst_a[1] + a_src_p[p + 1]);
                                dst_a[2] = (uint8_t)(dst_a[2] + a_src_p[p + 2]);
                                dst_a[3] = (uint8_t)(dst_a[3] + a_src_p[p + 3]);
                            }
                        } else {
                            dst_c32[0] = w0;
                            dst_c32[1] = w1;
                            if (dst_a) {
                                *(uint32_t *)dst_a = read_le32(a_src_p + p);
                            }
                        }
                    }
                    c_low_p += 16;
                    c_high_p += 16;
                    a_src_p += 16;
                }
            }
        }

        /* Pass 2: Per-stripe cache-hot fused blend for changed 4x4 tiles */
        if (fused_bg && fused_comp && alpha_dst) {
            for (size_t byte_idx = 0; byte_idx < mask_bytes; byte_idx++) {
                uint8_t mbyte = mask[byte_idx];
                if (mbyte == 0) {
                    continue;
                }
                while (mbyte != 0) {
                    int bit = __builtin_ctz((unsigned)mbyte);
                    mbyte &= (uint8_t)(mbyte - 1);
                    int t = (int)(byte_idx << 3) + bit;
                    if (t >= total_tiles) {
                        break;
                    }
                    uint16_t ty = (t / tiles_x) * 4;
                    uint16_t tx = (t % tiles_x) * 4;
                    for (int r = 0; r < 4; r++) {
                        size_t row_off = (size_t)(ty + r) * width + tx;
                        const uint32_t *fg32 = (const uint32_t *)(fb16 + row_off);
                        uint32_t a32 = *(const uint32_t *)(alpha_dst + row_off);
                        uint32_t *comp32 = (uint32_t *)(fused_comp + row_off);
                        if (a32 == 0xFFFFFFFFU) {
                            comp32[0] = fg32[0];
                            comp32[1] = fg32[1];
                        } else if (a32 == 0U) {
                            const uint32_t *bg32 = (const uint32_t *)(fused_bg + row_off);
                            comp32[0] = bg32[0];
                            comp32[1] = bg32[1];
                        } else {
                            const uint16_t *fg_row = fb16 + row_off;
                            const uint8_t *a_row = alpha_dst + row_off;
                            const uint16_t *bg_row = fused_bg + row_off;
                            uint16_t *comp_row = fused_comp + row_off;
                            for (int k = 0; k < 4; k++) {
                                uint8_t a = a_row[k];
                                uint16_t bg_px = bg_row[k];
                                uint16_t fg_c = fg_row[k];
                                if (a == 0) {
                                    comp_row[k] = bg_px;
                                } else if (a == 255 || fg_c == bg_px) {
                                    comp_row[k] = fg_c;
                                } else {
                                    uint32_t mix5 = ((uint32_t)a + 4U) >> 3;
                                    uint32_t bg_p = ((uint32_t)bg_px | ((uint32_t)bg_px << 16)) & 0x07E0F81FU;
                                    uint32_t fg_p = ((uint32_t)fg_c | ((uint32_t)fg_c << 16)) & 0x07E0F81FU;
                                    uint32_t res = ((((fg_p - bg_p) * mix5) >> 5) + bg_p) & 0x07E0F81FU;
                                    comp_row[k] = (uint16_t)((res >> 16) | res);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (max_x >= min_x) {
            min_y += block_index * header->block_height;
            max_y += block_index * header->block_height;
            if (min_x < decoder->dirty.x1) decoder->dirty.x1 = min_x;
            if (min_y < decoder->dirty.y1) decoder->dirty.y1 = min_y;
            if (max_x > decoder->dirty.x2) decoder->dirty.x2 = max_x;
            if (max_y > decoder->dirty.y2) decoder->dirty.y2 = max_y;
            decoder->dirty.valid = true;

            eaf_emit_tile_dirty_bands(decoder, mask, tiles_x, tiles_y,
                                      (uint16_t)(block_index * header->block_height));
        }
        decoder->predictor_stats.restore_us += (uint64_t)(esp_timer_get_time() - t_rest0);
        decoder->predictor_stats.frames++;
        return ESP_OK;
    }

    if (encoding_type == ESP_EAF_ENCODING_QUAD_ABBREV) {
        /* Quadtree 8x8/16x16 Multi-Res Abbreviated JPEG:
         * - 29 bytes: 225-bit outer 16x16 tile mask
         * - (changed_16 + 1)/2 bytes: 4-bit nibble per changed 16x16 tile:
         *     nibble == 0    : Smooth 16x16 tile (1 8x8 block upscaled 2x2 to 16x16)
         *     nibble in 1..15: 4-bit quadrant mask (each set bit is 1 sharp 8x8 quadrant)
         * - Abbreviated JPEG scan of 1D horizontal strip (width=16 * ceil(total_8x8 / 4), height=16)
         */
        int tiles_x = width / 16;
        int tiles_y = block_height / 16;
        int total_tiles = tiles_x * tiles_y;
        int mask_bytes = (total_tiles + 7) / 8;
        if (block_len < 1 + mask_bytes) {
            return ESP_FAIL;
        }
        const uint8_t *mask = block_data + 1;
        int changed_16 = 0;
        uint16_t min_x = width, min_y = header->height, max_x = 0, max_y = 0;
        for (int t = 0; t < total_tiles; t++) {
            if (mask[t >> 3] & (1 << (t & 7))) {
                changed_16++;
                uint16_t ty = (t / tiles_x) * 16;
                uint16_t tx = (t % tiles_x) * 16;
                if (tx < min_x) min_x = tx;
                if (ty < min_y) min_y = ty;
                if (tx + 15 > max_x) max_x = tx + 15;
                if (ty + 15 > max_y) max_y = ty + 15;
            }
        }
        if (changed_16 == 0) {
            return ESP_OK;
        }
        decoder->dirty.x1 = min_x;
        decoder->dirty.y1 = min_y;
        decoder->dirty.x2 = max_x;
        decoder->dirty.y2 = max_y;
        decoder->dirty.valid = true;

        int nibble_bytes = (changed_16 + 1) / 2;
        if (block_len < 1 + mask_bytes + nibble_bytes || decoder->jpeg_hdr_len == 0 || decoder->sof0_offset < 0) {
            return ESP_FAIL;
        }
        const uint8_t *nibbles = block_data + 1 + mask_bytes;
        int total_8x8 = 0;
        for (int c = 0; c < changed_16; c++) {
            uint8_t nib = (c & 1) ? ((nibbles[c >> 1] >> 4) & 0x0F) : (nibbles[c >> 1] & 0x0F);
            if (nib == 0) {
                /* This revision supports native-resolution quadrants only. */
                return ESP_ERR_NOT_SUPPORTED;
            } else {
                total_8x8 += __builtin_popcount(nib);
            }
        }
        int mcus = (total_8x8 + 3) / 4;
        if (mcus > 65535 / 16) {
            return ESP_ERR_INVALID_SIZE;
        }
        int atlas_w = mcus * 16;
        int atlas_h = 16;
        size_t atlas_size = (size_t)atlas_w * atlas_h * 2;
        size_t alloc_sz = 0;
        uint8_t *atlas_buf = (uint8_t *)esp_eaf_malloc_aligned_prefer_psram(atlas_size, 0, &alloc_sz);
        if (!atlas_buf) {
            return ESP_ERR_NO_MEM;
        }

        const uint8_t *scan_ptr = block_data + 1 + mask_bytes + nibble_bytes;
        size_t scan_len = block_len - 1 - mask_bytes - nibble_bytes;
        size_t full_len = decoder->jpeg_hdr_len + scan_len + 2;
        uint8_t *rebuilt_jpg = (uint8_t *)esp_eaf_malloc_prefer_psram(full_len);
        if (!rebuilt_jpg) {
            free(atlas_buf);
            return ESP_ERR_NO_MEM;
        }
        memcpy(rebuilt_jpg, decoder->jpeg_hdr, decoder->jpeg_hdr_len);
        rebuilt_jpg[decoder->sof0_offset + 5] = (atlas_h >> 8) & 0xFF;
        rebuilt_jpg[decoder->sof0_offset + 6] = atlas_h & 0xFF;
        rebuilt_jpg[decoder->sof0_offset + 7] = (atlas_w >> 8) & 0xFF;
        rebuilt_jpg[decoder->sof0_offset + 8] = atlas_w & 0xFF;
        memcpy(rebuilt_jpg + decoder->jpeg_hdr_len, scan_ptr, scan_len);
        rebuilt_jpg[decoder->jpeg_hdr_len + scan_len] = 0xFF;
        rebuilt_jpg[decoder->jpeg_hdr_len + scan_len + 1] = 0xD9;

        size_t dec_sz = alloc_sz;
        decode_result = esp_eaf_jpeg_decode(rebuilt_jpg, full_len, atlas_buf, &dec_sz, swap_color);
        free(rebuilt_jpg);

        if (decode_result == ESP_OK && dec_sz != atlas_size) decode_result = ESP_ERR_INVALID_SIZE;
        if (decode_result == ESP_OK) {
            uint16_t *fb16 = (uint16_t *)decode_buffer;
            const uint16_t *at16 = (const uint16_t *)atlas_buf;
            int idx8 = 0;
            int c = 0;
            for (int t = 0; t < total_tiles; t++) {
                if (!(mask[t >> 3] & (1 << (t & 7)))) {
                    continue;
                }
                int ty = (t / tiles_x) * 16;
                int tx = (t % tiles_x) * 16;
                uint8_t nib = (c & 1) ? ((nibbles[c >> 1] >> 4) & 0x0F) : (nibbles[c >> 1] & 0x0F);
                c++;

                /* Sharp 1:1 8x8 quadrants only (no 2x2 boxy upscale!) */
                for (int q = 0; q < 4; q++) {
                    if (!(nib & (1 << q))) {
                        continue;
                    }
                    int mcu_idx = idx8 >> 2;
                    int q_sub = idx8 & 3;
                    int ay = (q_sub >> 1) * 8;
                    int ax = mcu_idx * 16 + (q_sub & 1) * 8;
                    idx8++;
                    int qy = ty + (q >> 1) * 8;
                    int qx = tx + (q & 1) * 8;
                    for (int r = 0; r < 8; r++) {
                        memcpy(fb16 + (qy + r) * width + qx, at16 + (ay + r) * atlas_w + ax, 8 * 2);
                    }
                }
            }
        }
        free(atlas_buf);
        return decode_result;
    }

#if ESP_LV_EAF_ENABLE_H264
    if (encoding_type == ESP_EAF_ENCODING_H264) {
        if (width % 2 || block_height % 2 || header->blocks != 1) return ESP_ERR_INVALID_SIZE;
        if (decoder->h264 == NULL) {
            esp_h264_dec_cfg_sw_t cfg = {
                .pic_type = ESP_H264_RAW_FMT_I420,
            };
            if (esp_h264_dec_sw_new(&cfg, &decoder->h264) != ESP_H264_ERR_OK ||
                esp_h264_dec_open(decoder->h264) != ESP_H264_ERR_OK) {
                ESP_LOGE(TAG, "Failed to init H.264 decoder");
                return ESP_FAIL;
            }
        }
        size_t au_len = (size_t)(block_len - 1);
        uint8_t *ram_au = (uint8_t *)heap_caps_malloc(au_len + 16, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!ram_au) {
            ram_au = (uint8_t *)esp_eaf_malloc_prefer_psram(au_len + 16);
        }
        if (!ram_au) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(ram_au, block_data + 1, au_len);
        memset(ram_au + au_len, 0, 16);

        esp_h264_dec_in_frame_t in_frame = {
            .raw_data = {
                .buffer = ram_au,
                .len = (uint32_t)au_len,
            },
        };
        esp_h264_dec_out_frame_t out_frame = {0};
        bool got_pic = false;
        while (in_frame.raw_data.len > 0) {
            esp_h264_err_t hret = esp_h264_dec_process(decoder->h264, &in_frame, &out_frame);
            if (hret != ESP_H264_ERR_OK) {
                ESP_LOGE(TAG, "H.264 decode failed: %d", hret);
                free(ram_au);
                return ESP_FAIL;
            }
            if (in_frame.consume > in_frame.raw_data.len) {
                free(ram_au);
                return ESP_FAIL;
            }
            if (in_frame.consume == 0) {
                break;
            }
            in_frame.raw_data.buffer += in_frame.consume;
            in_frame.raw_data.len -= in_frame.consume;
            if (out_frame.out_size > 0 && out_frame.outbuf != NULL) {
                if (out_frame.out_size != (size_t)width * block_height * 3 / 2) {
                    free(ram_au);
                    return ESP_ERR_INVALID_SIZE;
                }
                esp_eaf_i420_to_rgb565(out_frame.outbuf, width, block_height, (uint16_t *)decode_buffer, swap_color);
                got_pic = true;
            }
        }
        free(ram_au);
        if (got_pic) {
            decoder->dirty.x1 = 0;
            decoder->dirty.y1 = 0;
            decoder->dirty.x2 = width - 1;
            decoder->dirty.y2 = block_height - 1;
            decoder->dirty.valid = true;
        }
        return got_pic ? ESP_OK : ESP_FAIL;
    }
#endif

    if (encoding_type >= sizeof(s_esp_eaf_decoder) / sizeof(s_esp_eaf_decoder[0])) {
        ESP_LOGE(TAG, "Unknown encoding type: 0x%02X", encoding_type);
        return ESP_FAIL;
    }

    esp_eaf_block_decoder_t block_decoder = s_esp_eaf_decoder[encoding_type];
    if (!block_decoder) {
        ESP_LOGE(TAG, "No decoder for encoding type: 0x%02X", encoding_type);
        return ESP_FAIL;
    }

    if (block_height <= 0) {
        return ESP_OK;
    }

    size_t pixel_count = (size_t)width * block_height;
    uint8_t *alpha_block = NULL;
    if (alpha_buffer != NULL) {
        size_t alpha_offset = (size_t)block_index * header->block_height * width;
        alpha_block = alpha_buffer + alpha_offset;
    }
    size_t decode_capacity = 0;
    bool is_jpeg = (encoding_type == ESP_EAF_ENCODING_JPEG);

    if (is_jpeg) {
        decode_capacity = pixel_count * 2;
    } else {
        switch (header->bit_depth) {
        case 24:
            decode_capacity = pixel_count * 3;
            break;
        case 8:
            decode_capacity = pixel_count;
            break;
        case 4:
            decode_capacity = (pixel_count + 1) / 2;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported bit depth: %d", header->bit_depth);
            return ESP_ERR_INVALID_ARG;
        }
    }

    uint8_t *work_buffer = NULL;
    uint8_t *decode_target = decode_buffer;

    if (!is_jpeg) {
        work_buffer = (uint8_t *)esp_eaf_malloc_prefer_psram(decode_capacity);
        if (!work_buffer) {
            ESP_LOGE(TAG, "Failed to allocate working buffer");
            return ESP_ERR_NO_MEM;
        }
        decode_target = work_buffer;
    }

    size_t out_size = decode_capacity;
    if (is_jpeg && block_index == 0) {
        esp_eaf_cache_jpeg_tables(decoder, block_data + 1, block_len - 1);
    }
    uint16_t by0 = (uint16_t)(block_index * header->block_height);
    uint16_t by1 = (uint16_t)(by0 + block_height - 1);
    if (0 < decoder->dirty.x1) decoder->dirty.x1 = 0;
    if (by0 < decoder->dirty.y1) decoder->dirty.y1 = by0;
    if ((uint16_t)(width - 1) > decoder->dirty.x2) decoder->dirty.x2 = (uint16_t)(width - 1);
    if (by1 > decoder->dirty.y2) decoder->dirty.y2 = by1;
    decoder->dirty.valid = true;

    decode_result = block_decoder(block_data + 1, block_len - 1, decode_target, &out_size, swap_color);

    if (decode_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decode block %d", block_index);
        if (work_buffer) {
            free(work_buffer);
        }
        return ESP_FAIL;
    }

    if (is_jpeg) {
        if (out_size != pixel_count * 2) return ESP_ERR_INVALID_SIZE;
        if (alpha_block != NULL) {
            memset(alpha_block, 0xFF, pixel_count);
        }
        return ESP_OK;
    }

    uint16_t *dest = (uint16_t *)decode_buffer;
    size_t dest_index = 0;

    switch (header->bit_depth) {
    case 24: {
        if (out_size < pixel_count * 3) {
            ESP_LOGE(TAG, "Insufficient data for 24-bit block %d", block_index);
            free(work_buffer);
            return ESP_FAIL;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            const uint8_t *bgr = &work_buffer[i * 3];
            dest[dest_index++] = esp_eaf_rgb565_from_bgr(bgr, swap_color);
            if (alpha_block != NULL) {
                alpha_block[i] = 0xFF;
            }
        }
        break;
    }
    case 8: {
        if (header->palette == NULL) {
            ESP_LOGE(TAG, "Palette missing for 8-bit block");
            free(work_buffer);
            return ESP_FAIL;
        }
        if (out_size < pixel_count) {
            ESP_LOGE(TAG, "Insufficient data for 8-bit block %d", block_index);
            free(work_buffer);
            return ESP_FAIL;
        }
        for (size_t i = 0; i < pixel_count; i++) {
            uint8_t ci = work_buffer[i];
            uint16_t color;
            uint8_t a;

            if (cache && cache->color[ci] != 0xFFFFFFFF) {
                /* Cache hit */
                color = (uint16_t)cache->color[ci];
                a = cache->alpha[ci];
            } else {
                /* Cache miss - compute color */
                const uint8_t *p = &header->palette[ci * 4];
                a = p[3];
                uint16_t rgb = ((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | ((p[0] & 0xF8) >> 3);
                color = swap_color ? __builtin_bswap16(rgb) : rgb;
                if (cache) {
                    cache->color[ci] = color;
                    cache->alpha[ci] = a;
                }
            }

            if (a) {
                dest[dest_index++] = color;
            } else {
                dest_index++;
            }
            if (alpha_block != NULL) {
                alpha_block[i] = a;
            }
        }
        break;
    }
    case 4: {
        if (header->palette == NULL) {
            ESP_LOGE(TAG, "Palette missing for 4-bit block");
            free(work_buffer);
            return ESP_FAIL;
        }
        size_t px = 0;
        for (size_t i = 0; i < out_size && px < pixel_count; i++) {
            uint8_t hi = (work_buffer[i] >> 4) & 0x0F;
            uint16_t color_hi;
            uint8_t a_hi;

            if (cache && cache->color[hi] != 0xFFFFFFFF) {
                color_hi = (uint16_t)cache->color[hi];
                a_hi = cache->alpha[hi];
            } else {
                const uint8_t *p = &header->palette[hi * 4];
                a_hi = p[3];
                uint16_t rgb = ((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | ((p[0] & 0xF8) >> 3);
                color_hi = swap_color ? __builtin_bswap16(rgb) : rgb;
                if (cache) {
                    cache->color[hi] = color_hi;
                    cache->alpha[hi] = a_hi;
                }
            }

            if (a_hi) {
                dest[px++] = color_hi;
            } else {
                px++;
            }
            if (alpha_block != NULL) {
                alpha_block[px - 1] = a_hi;
            }
            if (px >= pixel_count) {
                break;
            }

            uint8_t lo = work_buffer[i] & 0x0F;
            uint16_t color_lo;
            uint8_t a_lo;

            if (cache && cache->color[lo] != 0xFFFFFFFF) {
                color_lo = (uint16_t)cache->color[lo];
                a_lo = cache->alpha[lo];
            } else {
                const uint8_t *p = &header->palette[lo * 4];
                a_lo = p[3];
                uint16_t rgb = ((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | ((p[0] & 0xF8) >> 3);
                color_lo = swap_color ? __builtin_bswap16(rgb) : rgb;
                if (cache) {
                    cache->color[lo] = color_lo;
                    cache->alpha[lo] = a_lo;
                }
            }

            if (a_lo) {
                dest[px++] = color_lo;
            } else {
                px++;
            }
            if (alpha_block != NULL) {
                alpha_block[px - 1] = a_lo;
            }
        }
        break;
    }
    default:
        break;
    }

    free(work_buffer);
    return ESP_OK;
}

esp_err_t esp_eaf_rle_decode(const uint8_t *input_data, size_t input_size,
                             uint8_t *output_buffer, size_t *out_size,
                             bool swap_color)
{
    (void)swap_color; /* Unused parameter */

    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos + 1 <= input_size) {
        uint8_t repeat_count = input_data[in_pos++];
        uint8_t repeat_value = input_data[in_pos++];

        if (out_pos + repeat_count > *out_size) {
            ESP_LOGE(TAG, "Decompressed buffer overflow: %zu > %zu", out_pos + repeat_count, *out_size);
            return ESP_FAIL;
        }

        for (uint8_t i = 0; i < repeat_count; i++) {
            output_buffer[out_pos++] = repeat_value;
        }
    }

    *out_size = out_pos;
    return ESP_OK;
}

/**********************
 *  HARDWARE JPEG FUNCTIONS
 **********************/

#if ESP_EAF_HW_JPEG_ENABLED
/**
 * @brief Initialize hardware JPEG decoder
 */
esp_err_t esp_eaf_hw_jpeg_init(void)
{
    if (s_hw_jpeg_handle != NULL) {
        return ESP_OK;  /* Already initialized */
    }

    jpeg_decode_engine_cfg_t decode_eng_cfg = {
        .timeout_ms = 40,
    };

    esp_err_t ret = jpeg_new_decoder_engine(&decode_eng_cfg, &s_hw_jpeg_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create HW JPEG decoder engine: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Hardware JPEG decoder initialized");
    return ESP_OK;
}

/**
 * @brief Deinitialize hardware JPEG decoder
 */
esp_err_t esp_eaf_hw_jpeg_deinit(void)
{
    if (s_hw_jpeg_handle != NULL) {
        jpeg_del_decoder_engine(s_hw_jpeg_handle);
        s_hw_jpeg_handle = NULL;
        ESP_LOGI(TAG, "Hardware JPEG decoder deinitialized");
    }
    return ESP_OK;
}

/**
 * @brief Decode JPEG using hardware decoder
 */
static esp_err_t esp_eaf_jpeg_decode_hardware(const uint8_t *jpeg_data, size_t jpeg_size,
                                              uint8_t *decode_buffer, size_t *out_size,
                                              bool swap_color)
{
    if (s_hw_jpeg_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* First parse header to get dimensions using software decoder */
    uint32_t w, h;
    jpeg_dec_config_t sw_config = {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .rotate = JPEG_ROTATE_0D,
    };

    jpeg_dec_handle_t sw_dec;
    if (jpeg_dec_open(&sw_config, &sw_dec) != ESP_OK) {
        return ESP_FAIL;
    }

    jpeg_dec_io_t *jpeg_io = esp_eaf_malloc_prefer_psram(sizeof(jpeg_dec_io_t));
    jpeg_dec_header_info_t *header_info = esp_eaf_malloc_prefer_psram(sizeof(jpeg_dec_header_info_t));
    if (!jpeg_io || !header_info) {
        free(jpeg_io);
        free(header_info);
        jpeg_dec_close(sw_dec);
        return ESP_ERR_NO_MEM;
    }

    jpeg_io->inbuf = (unsigned char *)jpeg_data;
    jpeg_io->inbuf_len = jpeg_size;

    if (jpeg_dec_parse_header(sw_dec, jpeg_io, header_info) != JPEG_ERR_OK) {
        free(jpeg_io);
        free(header_info);
        jpeg_dec_close(sw_dec);
        return ESP_FAIL;
    }

    w = header_info->width;
    h = header_info->height;

    free(jpeg_io);
    free(header_info);
    jpeg_dec_close(sw_dec);

    /* Check if dimensions support hardware decoding (16-pixel alignment required) */
    if ((w % 16 != 0) || (h % 16 != 0)) {
        return ESP_ERR_NOT_SUPPORTED;  /* Fall back to software */
    }

    /* Validate output buffer size */
    size_t required_size = w * h * 2;  /* RGB565 = 2 bytes per pixel */
    if (*out_size < required_size) {
        ESP_LOGE(TAG, "Output buffer too small for HW decode: need %zu, got %zu", required_size, *out_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Check buffer alignment for hardware JPEG decoder */
    size_t cache_align = esp_eaf_get_psram_cache_align();
    uintptr_t buffer_addr = (uintptr_t)decode_buffer;
    if ((buffer_addr % cache_align) != 0) {
        ESP_LOGD(TAG, "Decode buffer address not aligned: %p (required: %zu)", decode_buffer, cache_align);
        return ESP_ERR_NOT_SUPPORTED;  /* Fall back to software */
    }
    if ((required_size % cache_align) != 0) {
        ESP_LOGD(TAG, "Decode buffer size not aligned: %zu (required: %zu)", required_size, cache_align);
        return ESP_ERR_NOT_SUPPORTED;  /* Fall back to software */
    }

    /* Configure hardware decoder */
    jpeg_decode_cfg_t hw_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = swap_color ? JPEG_DEC_RGB_ELEMENT_ORDER_RGB : JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    /* Perform hardware decode */
    uint32_t output_size = 0;
    esp_err_t ret = jpeg_decoder_process(
                        s_hw_jpeg_handle,
                        &hw_cfg,
                        (uint8_t *)jpeg_data,
                        jpeg_size,
                        decode_buffer,
                        required_size,
                        &output_size
                    );

    if (ret == ESP_OK) {
        *out_size = required_size;
        ESP_LOGD(TAG, "Hardware JPEG decode: %"PRIu32"x%"PRIu32" -> %"PRIu32" bytes", w, h, output_size);
    }

    return ret;
}
#else
esp_err_t esp_eaf_hw_jpeg_init(void)
{
    ESP_LOGD(TAG, "Hardware JPEG decoder not available on this chip");
    return ESP_OK;
}

esp_err_t esp_eaf_hw_jpeg_deinit(void)
{
    return ESP_OK;
}
#endif

esp_err_t esp_eaf_jpeg_decode(const uint8_t *jpeg_data, size_t jpeg_size,
                              uint8_t *decode_buffer, size_t *out_size, bool swap_color)
{
#if ESP_LV_EAF_ENABLE_SW_JPEG

#if ESP_EAF_HW_JPEG_ENABLED
    /* Try hardware decoding first if available */
    if (s_hw_jpeg_handle != NULL) {
        esp_err_t hw_ret = esp_eaf_jpeg_decode_hardware(jpeg_data, jpeg_size,
                                                        decode_buffer, out_size, swap_color);
        if (hw_ret == ESP_OK) {
            return ESP_OK;
        }
        /* If hardware decode fails, fall through to software */
        ESP_LOGD(TAG, "Hardware JPEG decode failed (0x%x), falling back to software", hw_ret);
    }
#endif

    /* Software decoding fallback */
    uint32_t w, h;
    jpeg_dec_config_t config = {
        .output_type = swap_color ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE,
        .rotate = JPEG_ROTATE_0D,
    };

    jpeg_dec_handle_t jpeg_dec;
    if (jpeg_dec_open(&config, &jpeg_dec) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open JPEG decoder");
        return ESP_FAIL;
    }

    jpeg_dec_io_t *jpeg_io = esp_eaf_malloc_prefer_psram(sizeof(jpeg_dec_io_t));
    jpeg_dec_header_info_t *out_info = esp_eaf_malloc_prefer_psram(sizeof(jpeg_dec_header_info_t));
    if (!jpeg_io || !out_info) {
        if (jpeg_io) {
            free(jpeg_io);
        }
        if (out_info) {
            free(out_info);
        }
        jpeg_dec_close(jpeg_dec);
        ESP_LOGE(TAG, "Failed to allocate memory for JPEG decoder");
        return ESP_FAIL;
    }

    jpeg_io->inbuf = (unsigned char *)jpeg_data;
    jpeg_io->inbuf_len = jpeg_size;

    jpeg_error_t ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);
    if (ret == JPEG_ERR_OK) {
        w = out_info->width;
        h = out_info->height;

        size_t required_size = w * h * 2; /* RGB565 = 2 bytes per pixel */
        if (*out_size < required_size) {
            ESP_LOGE(TAG, "Output buffer too small: need %zu, got %zu", required_size, *out_size);
            free(jpeg_io);
            free(out_info);
            jpeg_dec_close(jpeg_dec);
            return ESP_ERR_INVALID_SIZE;
        }

        jpeg_io->outbuf = decode_buffer;
        ret = jpeg_dec_process(jpeg_dec, jpeg_io);
        if (ret != JPEG_ERR_OK) {
            free(jpeg_io);
            free(out_info);
            jpeg_dec_close(jpeg_dec);
            ESP_LOGE(TAG, "Failed to decode JPEG: %d", ret);
            return ESP_FAIL;
        }
        *out_size = required_size;
    } else {
        free(jpeg_io);
        free(out_info);
        jpeg_dec_close(jpeg_dec);
        ESP_LOGE(TAG, "Failed to parse JPEG header");
        return ESP_FAIL;
    }

    free(jpeg_io);
    free(out_info);
    jpeg_dec_close(jpeg_dec);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "JPEG block detected but software JPEG decoder is DISABLED");
    ESP_LOGW(TAG, "Enable CONFIG_ESP_LV_EAF_ENABLE_SW_JPEG in menuconfig to decode JPEG blocks");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t esp_eaf_huffman_decode(const uint8_t *input_data, size_t input_size,
                                 uint8_t *output_buffer, size_t *out_size,
                                 bool swap_color)
{
    (void)swap_color; /* Unused parameter */
    size_t decoded_size = *out_size;

    if (!input_data || input_size < 3 || !output_buffer) {
        ESP_LOGE(TAG, "Invalid parameters: input_data=%p, input_size=%zu, output_buffer=%p",
                 input_data, input_size, output_buffer);
        return ESP_FAIL;
    }

    uint16_t dict_size = (input_data[1] << 8) | input_data[0];
    if (input_size < 2 + dict_size) {
        ESP_LOGE(TAG, "Compressed data too short for dictionary");
        return ESP_FAIL;
    }

    size_t encoded_size = input_size - 2 - dict_size;
    if (encoded_size == 0) {
        ESP_LOGE(TAG, "No data to decode");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_eaf_huffman_decode_data(input_data + 2 + dict_size, encoded_size,
                                                input_data + 2, dict_size,
                                                output_buffer, &decoded_size);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Huffman decoding failed: %d", ret);
        return ESP_FAIL;
    }

    if (decoded_size > *out_size) {
        ESP_LOGE(TAG, "Decoded data too large: %zu > %zu", decoded_size, *out_size);
        return ESP_FAIL;
    }
    *out_size = decoded_size;

    return ESP_OK;
}

/**********************
 *  FORMAT FUNCTIONS
 **********************/

esp_err_t esp_eaf_format_init(const uint8_t *data, size_t data_len, esp_eaf_format_handle_t *ret_parser)
{
    static bool decoders_initialized = false;

    if (!decoders_initialized) {
        esp_err_t ret = esp_eaf_init_decoders();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize decoders");
            return ret;
        }
        decoders_initialized = true;
    }

    esp_err_t ret = ESP_OK;
    esp_eaf_frame_entry_t *entries = NULL;
    esp_eaf_format_ctx_t *parser = NULL;

    ESP_GOTO_ON_FALSE(data != NULL && ret_parser != NULL, ESP_ERR_INVALID_ARG, err, TAG, "Invalid input");
    ESP_GOTO_ON_FALSE(data_len >= ESP_EAF_TABLE_OFFSET, ESP_ERR_INVALID_SIZE, err, TAG, "File too small");

    parser = (esp_eaf_format_ctx_t *)esp_eaf_calloc_prefer_psram(1, sizeof(esp_eaf_format_ctx_t));
    ESP_GOTO_ON_FALSE(parser, ESP_ERR_NO_MEM, err, TAG, "No memory for parser handle");

    /* Check file format magic number: 0x89 */
    ESP_GOTO_ON_FALSE(data[ESP_EAF_FORMAT_OFFSET] == ESP_EAF_FORMAT_MAGIC, ESP_ERR_INVALID_CRC, err, TAG, "Bad file format magic");

    /* Check for EAF/AAF format string */
    const char *format_str = (const char *)(data + ESP_EAF_STR_OFFSET);
    bool is_valid = (memcmp(format_str, ESP_EAF_FORMAT_STR, 3) == 0) || (memcmp(format_str, ESP_AAF_FORMAT_STR, 3) == 0);
    ESP_GOTO_ON_FALSE(is_valid, ESP_ERR_INVALID_CRC, err, TAG, "Bad file format string (expected EAF or AAF)");

    int total_frames = (int)read_le32(data + ESP_EAF_NUM_OFFSET);
    uint32_t stored_chk = read_le32(data + ESP_EAF_CHECKSUM_OFFSET);
    uint32_t stored_len = read_le32(data + ESP_EAF_TABLE_LEN);

    ESP_GOTO_ON_FALSE(total_frames > 0, ESP_ERR_INVALID_SIZE, err, TAG, "Invalid frame count %d", total_frames);
    ESP_GOTO_ON_FALSE((size_t)total_frames <= (SIZE_MAX / sizeof(esp_eaf_frame_table_entry_t)), ESP_ERR_INVALID_SIZE, err, TAG, "Frame count too large");

    const size_t payload_available = data_len - ESP_EAF_TABLE_OFFSET;
    const size_t table_bytes = (size_t)total_frames * sizeof(esp_eaf_frame_table_entry_t);
    ESP_GOTO_ON_FALSE(table_bytes <= payload_available, ESP_ERR_INVALID_SIZE, err, TAG, "Frame table exceeds file size");

    const size_t stored_len_sz = stored_len;
    ESP_GOTO_ON_FALSE(stored_len_sz <= payload_available, ESP_ERR_INVALID_SIZE, err, TAG, "Stored length beyond file");
    ESP_GOTO_ON_FALSE(stored_len_sz >= table_bytes, ESP_ERR_INVALID_SIZE, err, TAG, "Stored length smaller than table");
    ESP_GOTO_ON_FALSE((size_t)ESP_EAF_TABLE_OFFSET + stored_len_sz <= data_len, ESP_ERR_INVALID_SIZE, err, TAG, "Stored data exceeds file");

    const uint32_t calculated_chk = esp_eaf_format_calc_checksum((uint8_t *)(data + ESP_EAF_TABLE_OFFSET), stored_len);
    ESP_GOTO_ON_FALSE(calculated_chk == stored_chk, ESP_ERR_INVALID_CRC, err, TAG, "Bad full checksum");

    entries = (esp_eaf_frame_entry_t *)esp_eaf_malloc_prefer_psram(sizeof(esp_eaf_frame_entry_t) * total_frames);
    ESP_GOTO_ON_FALSE(entries, ESP_ERR_NO_MEM, err, TAG, "No memory for frame entries");

    esp_eaf_frame_table_entry_t *table = (esp_eaf_frame_table_entry_t *)(data + ESP_EAF_TABLE_OFFSET);
    const size_t frame_region_len = stored_len_sz - table_bytes;
    const size_t frame_region_offset = ESP_EAF_TABLE_OFFSET + table_bytes;
    const uint8_t *frame_region_base = data + frame_region_offset;

    for (int i = 0; i < total_frames; i++) {
        size_t frame_size = table[i].frame_size;
        size_t frame_offset = table[i].frame_offset;

        ESP_GOTO_ON_FALSE(frame_size >= ESP_EAF_MAGIC_LEN, ESP_ERR_INVALID_SIZE, err, TAG, "Frame %d size too small", i);
        ESP_GOTO_ON_FALSE(frame_offset <= frame_region_len, ESP_ERR_INVALID_SIZE, err, TAG, "Frame %d offset out of range", i);
        ESP_GOTO_ON_FALSE(frame_size <= (frame_region_len - frame_offset), ESP_ERR_INVALID_SIZE, err, TAG, "Frame %d size out of range", i);

        const uint8_t *frame_mem = frame_region_base + frame_offset;
        uint16_t magic = 0;
        memcpy(&magic, frame_mem, sizeof(magic));
        ESP_GOTO_ON_FALSE(magic == ESP_EAF_MAGIC_HEAD, ESP_ERR_INVALID_CRC, err, TAG, "Bad file magic header");

        (entries + i)->table = (table + i);
        (entries + i)->frame_mem = (const char *)frame_mem;
    }

    parser->entries = entries;
    parser->total_frames = total_frames;

    *ret_parser = (esp_eaf_format_handle_t)parser;

    return ESP_OK;

err:
    if (entries) {
        free(entries);
    }
    if (parser) {
        free(parser);
    }
    if (ret_parser) {
        *ret_parser = NULL;
    }

    return ret;
}

esp_err_t esp_eaf_format_deinit(esp_eaf_format_handle_t handle)
{
    if (handle == NULL) {
        ESP_LOGW(TAG, "Handle is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    esp_eaf_format_ctx_t *parser = (esp_eaf_format_ctx_t *)(handle);
    if (parser) {
        if (parser->entries) {
            free(parser->entries);
        }
        free(parser);
    }
    return ESP_OK;
}

int esp_eaf_format_get_total_frames(esp_eaf_format_handle_t handle)
{
    if (handle == NULL) {
        ESP_LOGE(TAG, "Handle is invalid");
        return -1;
    }

    esp_eaf_format_ctx_t *parser = (esp_eaf_format_ctx_t *)(handle);
    return parser->total_frames;
}

const uint8_t *esp_eaf_format_get_frame_data(esp_eaf_format_handle_t handle, int index)
{
    if (handle == NULL) {
        ESP_LOGE(TAG, "Handle is invalid");
        return NULL;
    }

    esp_eaf_format_ctx_t *parser = (esp_eaf_format_ctx_t *)(handle);

    if (parser->total_frames > index) {
        return (const uint8_t *)((parser->entries + index)->frame_mem + ESP_EAF_MAGIC_LEN);
    } else {
        ESP_LOGE(TAG, "Invalid index: %d. Maximum index is %d", index, parser->total_frames);
        return NULL;
    }
}

int esp_eaf_format_get_frame_size(esp_eaf_format_handle_t handle, int index)
{
    if (handle == NULL) {
        ESP_LOGE(TAG, "Handle is invalid");
        return -1;
    }

    esp_eaf_format_ctx_t *parser = (esp_eaf_format_ctx_t *)(handle);

    if (parser->total_frames > index) {
        return ((parser->entries + index)->table->frame_size - ESP_EAF_MAGIC_LEN);
    } else {
        ESP_LOGE(TAG, "Invalid index: %d. Maximum index is %d", index, parser->total_frames);
        return -1;
    }
}

esp_err_t esp_eaf_block_decode(const esp_eaf_header_t *header, const uint8_t *frame_data,
                               int block_index, uint8_t *decode_buffer, uint8_t *alpha_buffer,
                               bool swap_color, esp_eaf_palette_cache_t *cache)
{
    return esp_eaf_block_decode_ctx(&s_legacy_decoder, header, frame_data, block_index,
                                    decode_buffer, alpha_buffer, swap_color, cache);
}
