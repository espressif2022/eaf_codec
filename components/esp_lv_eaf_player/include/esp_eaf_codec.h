/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "eaf4.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_EAF_CONTAINER_INVALID = 0,
    ESP_EAF_CONTAINER_EAF565  = 1, /*!< 0x89 "EAF" / 0x89 "AAF" (RGB565 / RGB565A8, modes 0x00..0x0C) */
    ESP_EAF_CONTAINER_EAF8888 = 2, /*!< "EAF4" (BGRA8888 straight-alpha, modes 0..6) */
} esp_eaf_container_t;

/**
 * @brief Detect EAF container type from the first 4 bytes of a buffer.
 */
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

#ifdef __cplusplus
}
#endif
