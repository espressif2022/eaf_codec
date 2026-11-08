/* EAF H.264 codec adapter. */
/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_eaf_codec_internal.h"

#if ESP_LV_EAF_ENABLE_H264
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_h264_dec_sw.h"

static const char *TAG = "eaf_h264";

esp_err_t esp_eaf_codec_video_decode(const esp_eaf_codec_request_t *r)
{
    esp_eaf_decoder_t *decoder = r->decoder;
    if (decoder->out_fmt != ESP_EAF_OUT_RGB565 && decoder->out_fmt != ESP_EAF_OUT_BGRA8888) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_eaf_header_t *header = r->header;
    const uint8_t *block_data = r->block_data;
    size_t block_len = r->block_len;
    int width = r->width;
    int block_height = r->block_height;
    uint8_t *decode_buffer = r->decode_buffer;
    bool swap_color = r->swap_color;
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
            if (decoder->out_fmt == ESP_EAF_OUT_BGRA8888) {
                esp_eaf_i420_to_bgra8888(out_frame.outbuf, width, block_height, (uint32_t *)decode_buffer);
            } else {
                esp_eaf_i420_to_rgb565(out_frame.outbuf, width, block_height, (uint16_t *)decode_buffer, swap_color);
            }
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
#else
esp_err_t esp_eaf_codec_video_decode(const esp_eaf_codec_request_t *request)
{
    (void)request;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
