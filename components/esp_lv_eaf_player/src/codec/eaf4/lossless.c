/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Lossless EAF4 submodes. */
#include <string.h>

#include "esp_eaf_codec_internal.h"

static uint32_t read_le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t channel_word(uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8) | a;
#else
    return b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
#endif
}

static int reconstruct_joint_delta(const esp_eaf_codec_request_t *request)
{
    uint32_t width = request->frame_width;
    uint32_t tiles = (request->frame_bytes / 4) / 16;
    uint32_t columns = width / 4;
    uint32_t mask_size = (tiles + 7) / 8;
    uint32_t decoded = request->decoded_size;
    const uint8_t *scratch = request->scratch;
    if (decoded < mask_size) return -1;

    uint32_t changed = 0;
    for (uint32_t i = 0; i < tiles; i++) {
        if (scratch[i / 8] & (1U << (i % 8))) changed++;
    }
    if (changed == 0) return decoded == mask_size ? 0 : -1;

    uint32_t symbol_count = changed * 16;
    if (decoded < mask_size + symbol_count) return -1;
    const uint8_t *symbols = scratch + mask_size;
    uint32_t escaped = 0;
    for (uint32_t i = 0; i < symbol_count; i++) {
        if (symbols[i] == 255) escaped++;
    }
    if (decoded != mask_size + symbol_count + escaped * 4) return -1;

    const uint8_t *escape = symbols + symbol_count;
    uint32_t ordinal = 0;
    uint32_t escape_index = 0;
    for (uint32_t tile = 0; tile < tiles; tile++) {
        if (!(scratch[tile / 8] & (1U << (tile % 8)))) continue;
        uint32_t y = (tile / columns) * 4;
        uint32_t x = (tile % columns) * 4;
        for (unsigned pixel = 0; pixel < 16; pixel++) {
            uint8_t *dst = request->frame + (((y + pixel / 4) * width + x + pixel % 4) * 4);
            uint8_t symbol = symbols[ordinal++];
            if (symbol == 255) {
                dst[0] = escape[0 * escaped + escape_index];
                dst[1] = escape[1 * escaped + escape_index];
                dst[2] = escape[2 * escaped + escape_index];
                dst[3] = escape[3 * escaped + escape_index];
                escape_index++;
            } else {
                dst[0] = (uint8_t)(dst[0] + (int)(symbol / 27U) % 3 - 1);
                dst[1] = (uint8_t)(dst[1] + (int)(symbol / 9U) % 3 - 1);
                dst[2] = (uint8_t)(dst[2] + (int)(symbol / 3U) % 3 - 1);
                dst[3] = (uint8_t)(dst[3] + (int)(symbol % 3U) - 1);
            }
        }
    }
    return 0;
}

static int reconstruct_tiles(const esp_eaf_codec_request_t *request)
{
    uint32_t count = request->frame_bytes / 4;
    uint32_t tiles = count / 16;
    uint32_t mask_size = (tiles + 7) / 8;
    uint32_t changed = 0;
    for (uint32_t i = 0; i < tiles; i++) {
        if (request->scratch[i / 8] & (1U << (i % 8))) changed++;
    }
    if (mask_size + changed * 64 != request->decoded_size) return -1;

    uint32_t ordinal = 0;
    uint32_t columns = request->frame_width / 4;
    uint32_t *output = (uint32_t *)request->frame;
    const uint8_t *payload = request->scratch + mask_size;
    for (uint32_t tile = 0; tile < tiles; tile++) {
        if (!(request->scratch[tile / 8] & (1U << (tile % 8)))) continue;
        uint32_t y = (tile / columns) * 4;
        uint32_t x = (tile % columns) * 4;
        if (request->mode == EAF4_SUBMODE_TILE_PACKED) {
            const uint8_t *tile_src = payload + ordinal * 64;
            for (unsigned row = 0; row < 4; row++) {
                uint32_t *dst = output + (y + row) * request->frame_width + x;
                const uint8_t *src = tile_src + row * 16;
                dst[0] ^= read_le32(src);
                dst[1] ^= read_le32(src + 4);
                dst[2] ^= read_le32(src + 8);
                dst[3] ^= read_le32(src + 12);
            }
        } else {
            const uint8_t *b = payload + ordinal * 16;
            const uint8_t *g = b + changed * 16;
            const uint8_t *r = g + changed * 16;
            const uint8_t *a = r + changed * 16;
            for (unsigned row = 0; row < 4; row++) {
                uint32_t *dst = output + (y + row) * request->frame_width + x;
                for (unsigned col = 0; col < 4; col++) {
                    unsigned pixel = row * 4 + col;
                    dst[col] ^= channel_word(b[pixel], g[pixel], r[pixel], a[pixel]);
                }
            }
        }
        ordinal++;
    }
    return 0;
}

int esp_eaf_codec_eaf4_lossless_decode(const esp_eaf_codec_request_t *request)
{
    if (!request || !request->frame || !request->scratch || !request->frame_width ||
        !request->frame_height || request->frame_bytes == 0) {
        return -1;
    }
    uint32_t count = request->frame_bytes / 4;
    switch (request->mode) {
    case EAF4_SUBMODE_RAW:
        return request->decoded_size == request->frame_bytes ? 0 : -1;
    case EAF4_SUBMODE_XOR: {
        if (request->decoded_size != request->frame_bytes) return -1;
        uint32_t *output = (uint32_t *)request->frame;
        const uint32_t *difference = (const uint32_t *)request->scratch;
        for (uint32_t i = 0; i < count; i++) output[i] ^= difference[i];
        return 0;
    }
    case EAF4_SUBMODE_PLANAR: {
        if (request->decoded_size != request->frame_bytes) return -1;
        uint32_t *output = (uint32_t *)request->frame;
        const uint8_t *b = request->scratch;
        const uint8_t *g = b + count;
        const uint8_t *r = g + count;
        const uint8_t *a = r + count;
        for (uint32_t i = 0; i < count; i++) {
            output[i] ^= channel_word(b[i], g[i], r[i], a[i]);
        }
        return 0;
    }
    case EAF4_SUBMODE_TILE_PLANAR:
    case EAF4_SUBMODE_TILE_PACKED:
        return reconstruct_tiles(request);
    case EAF4_SUBMODE_JOINT_DELTA:
        return reconstruct_joint_delta(request);
    default:
        return -1;
    }
}
