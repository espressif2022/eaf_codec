/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/** @file esp_eaf_codec_support.c
 *  @brief Shared entropy, JPEG, color-conversion and allocation support for codecs.
 */
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"

#include "esp_eaf_internal.h"
#include "esp_eaf_codec_internal.h"
#include "esp_lv_eaf_player_config.h"

#if ESP_LV_EAF_ENABLE_SW_JPEG
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#endif

#if ESP_LV_EAF_ENABLE_H264
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#endif

static const char *TAG = "eaf_codec_support";

#if ESP_LV_EAF_ENABLE_H264
static bool s_yuv_lut_inited = false;
static int16_t s_y_lut[256];
static int16_t s_rv_lut[256];
static int16_t s_gu_lut[256];
static int16_t s_gv_lut[256];
static int16_t s_bu_lut[256];
static uint16_t s_r565_clamp[1024];
static uint16_t s_g565_clamp[1024];
static uint16_t s_b565_clamp[1024];
static uint8_t s_u8_clamp[1024];

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
        s_u8_clamp[i] = (uint8_t)v;
        s_r565_clamp[i] = (uint16_t)((v & 0xF8) << 8);
        s_g565_clamp[i] = (uint16_t)((v & 0xFC) << 3);
        s_b565_clamp[i] = (uint16_t)(v >> 3);
    }
    s_yuv_lut_inited = true;
}

static inline uint32_t esp_eaf_yuv_lut_bgra8888(int y, int rd, int gd, int bd)
{
    y = s_y_lut[y];
    uint8_t b = s_u8_clamp[y + bd + 384];
    uint8_t g = s_u8_clamp[y - gd + 384];
    uint8_t r = s_u8_clamp[y + rd + 384];
    return (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000U;
}

void esp_eaf_i420_to_bgra8888(const uint8_t *i420, int width, int height, uint32_t *bgra)
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
        uint32_t *dst0 = bgra + y * width;
        uint32_t *dst1 = bgra + (y + 1) * width;

        for (int x = 0; x < width; x += 2) {
            uint8_t u = u_row[x >> 1];
            uint8_t v = v_row[x >> 1];
            int rd = s_rv_lut[v];
            int gd = s_gu_lut[u] + s_gv_lut[v];
            int bd = s_bu_lut[u];

            dst0[x]     = esp_eaf_yuv_lut_bgra8888(y_row0[x],     rd, gd, bd);
            dst0[x + 1] = esp_eaf_yuv_lut_bgra8888(y_row0[x + 1], rd, gd, bd);
            dst1[x]     = esp_eaf_yuv_lut_bgra8888(y_row1[x],     rd, gd, bd);
            dst1[x + 1] = esp_eaf_yuv_lut_bgra8888(y_row1[x + 1], rd, gd, bd);
        }
    }
}

static inline uint16_t esp_eaf_yuv_lut_px(int y, int rd, int gd, int bd)
{
    y = s_y_lut[y]; /* Generated H.264 is explicitly BT.601 limited range. */
    return s_r565_clamp[y + rd + 384] | s_g565_clamp[y - gd + 384] | s_b565_clamp[y + bd + 384];
}

void esp_eaf_i420_to_rgb565(const uint8_t *i420, int width, int height, uint16_t *rgb565, bool swap_color)
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
void esp_eaf_decoder_cache_jpeg_tables(esp_eaf_decoder_t *decoder, const uint8_t *jpg, size_t len)
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
void *esp_eaf_malloc_prefer_psram(size_t size)
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
void *esp_eaf_malloc_aligned_prefer_psram(size_t size, size_t alignment, size_t *allocated_size)
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

    /* Query JPEG picture info and enforce S31 / P4 2D-DMA horizontal macroblock alignment (hb) */
    uint32_t w = 0, h = 0, hb = 0;
    jpeg_decode_picture_info_t pic_info = {0};
    if (jpeg_decoder_get_info(jpeg_data, jpeg_size, &pic_info) != ESP_OK) {
        return ESP_FAIL;
    }
    w = pic_info.width;
    h = pic_info.height;
    if (pic_info.sample_method == JPEG_DOWN_SAMPLING_YUV444) {
        hb = 48; /* RGB565 YUV444 horizontal DMA block is 48 pixels */
    } else if (pic_info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ||
               pic_info.sample_method == JPEG_DOWN_SAMPLING_YUV420) {
        hb = 32; /* RGB565 YUV422/420 horizontal DMA block is 32 pixels */
    }
    if (hb == 0 || (w % hb != 0) || (h % 16 != 0)) {
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
    /* Hardware state belongs to the JPEG backend and is initialized lazily. */
    if (esp_eaf_hw_jpeg_init() == ESP_OK && s_hw_jpeg_handle != NULL) {
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
