/* Codec registry shared by all EAF containers. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_eaf_codec_internal.h"

static const esp_eaf_codec_ops_t s_codecs[] = {
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_RAW,          "eaf4-raw",          esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_XOR,          "eaf4-xor",          esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_PLANAR,       "eaf4-planar",       esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_TILE_PLANAR,  "eaf4-tile-planar",  esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_TILE_PACKED,  "eaf4-tile-packed",  esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_JOINT_DELTA,  "eaf4-joint-delta",  esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_BTC8888_SAFE, "eaf4-bounded",      esp_eaf_codec_bounded_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_565A8_TILES,  "eaf4-565a8-tiles",  esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_565A8_JOINT,  "eaf4-565a8-joint",  esp_eaf_codec_eaf4_lossless_decode },
    { ESP_EAF_CONTAINER_EAF8888, EAF4_SUBMODE_BTC565_SAFE,  "eaf4-565-bounded",  esp_eaf_codec_bounded_decode },
#ifdef ESP_PLATFORM
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_PALETTE_RLE,      "palette-rle",      esp_eaf_codec_palette_rle_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_PALETTE_HUFF_RLE, "palette-huff-rle", esp_eaf_codec_palette_huff_rle_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_JPEG_KEYFRAME,    "jpeg-keyframe",    esp_eaf_codec_jpeg_keyframe_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_PALETTE_HUFF,     "palette-huff",     esp_eaf_codec_palette_huff_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_LOSSLESS,         "lossless",         esp_eaf_codec_eaf_lossless_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_BTC_SAFE,         "bounded",          esp_eaf_codec_bounded_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_QUAD_JPEG,        "jpeg-delta",       esp_eaf_codec_jpeg_delta_decode },
    { ESP_EAF_CONTAINER_EAF565, ESP_EAF_MODE_H264,             "video",            esp_eaf_codec_video_decode },
#endif
};

const esp_eaf_codec_ops_t *esp_eaf_codec_find(esp_eaf_container_t container, uint8_t mode)
{
    for (size_t i = 0; i < sizeof(s_codecs) / sizeof(s_codecs[0]); i++) {
        if (s_codecs[i].container == container && s_codecs[i].mode == mode) {
            return &s_codecs[i];
        }
    }
    return NULL;
}
