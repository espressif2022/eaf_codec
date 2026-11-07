/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "eaf4.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Unified contiguous EAF encoding ID table (0x00..0x07, zero gaps).
 *
 *   - 0x00..0x03 : Legacy compatibility (Palette RLE / Huffman / Full-Frame JPEG keyframe)
 *   - 0x04 LOSSLESS  : [Core 1] 100% Bit-Exact Lossless (565+A8 & BGRA8888, adaptive sub-modes)
 *   - 0x05 BTC_SAFE  : [Core 2] 4x4 Error-Bounded BTC + Raw Escape (maxErr <= 8, 565 & BGRA8888)
 *   - 0x06 QUAD_JPEG : [Core 3] 16x16+8x8 Quadtree Delta Strip JPEG (HW/SW JPEG, 565 & RGB888, no alpha)
 *   - 0x07 H264      : [Core 4] Constrained Baseline I/P Video Stream (565 & RGB888, no alpha)
 */
typedef enum {
    /* Legacy compatibility & internal keyframe IDs (0x00 .. 0x03) */
    ESP_EAF_MODE_PALETTE_RLE      = 0x00,
    ESP_EAF_MODE_PALETTE_HUFF_RLE = 0x01,
    ESP_EAF_MODE_JPEG_KEYFRAME    = 0x02,
    ESP_EAF_MODE_PALETTE_HUFF     = 0x03,

    /* 4 Converged Production Modes (0x04 .. 0x07) */
    ESP_EAF_MODE_LOSSLESS         = 0x04,
    ESP_EAF_MODE_BTC_SAFE         = 0x05,
    ESP_EAF_MODE_QUAD_JPEG        = 0x06,
    ESP_EAF_MODE_H264             = 0x07,
    ESP_EAF_MODE_MAX              = 0x08,
} esp_eaf_mode_t;

typedef enum {
    ESP_EAF_CONTAINER_INVALID = 0,
    ESP_EAF_CONTAINER_EAF565  = 1, /*!< 0x89 "EAF" / 0x89 "AAF" */
    ESP_EAF_CONTAINER_EAF8888 = 2, /*!< "EAF4" (BGRA8888 straight-alpha) */
} esp_eaf_container_t;

typedef enum {
    ESP_EAF_OUT_RGB565   = 0, /*!< 16-bit RGB565 (+ optional A8 plane) */
    ESP_EAF_OUT_RGB888   = 1, /*!< 24-bit RGB888 / BGR888 (opaque modes 0x02, 0x06, 0x07) */
    ESP_EAF_OUT_BGRA8888 = 2, /*!< 32-bit BGRA8888 (all modes 0x00..0x07) */
} esp_eaf_output_format_t;

static inline esp_eaf_container_t esp_eaf_container_probe(const void *data, size_t size)
{
    if (!data || size < 4) {
        return ESP_EAF_CONTAINER_INVALID;
    }
    const uint8_t *b = (const uint8_t *)data;
    if (b[0] == 0x89 && (memcmp(b + 1, "EAF", 3) == 0 || memcmp(b + 1, "AAF", 3) == 0)) {
        return ESP_EAF_CONTAINER_EAF565;
    }
    if (memcmp(b, "EAF4", 4) == 0) {
        return ESP_EAF_CONTAINER_EAF8888;
    }
    return ESP_EAF_CONTAINER_INVALID;
}

static inline bool esp_eaf_mode_supports_alpha(esp_eaf_mode_t mode)
{
    return (mode != ESP_EAF_MODE_JPEG_KEYFRAME &&
            mode != ESP_EAF_MODE_QUAD_JPEG &&
            mode != ESP_EAF_MODE_H264);
}

#ifdef __cplusplus
}
#endif
