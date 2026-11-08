/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "esp_eaf_player.h"
#include "esp_log.h"

extern const uint8_t legacy_eaf_start[] asm("_binary_legacy_eaf_start");
extern const uint8_t legacy_eaf_end[] asm("_binary_legacy_eaf_end");
extern const uint8_t lossless_eaf4_start[] asm("_binary_lossless_eaf4_start");
extern const uint8_t lossless_eaf4_end[] asm("_binary_lossless_eaf4_end");

static void verify(const uint8_t *start, const uint8_t *end)
{
    esp_eaf_player_t *player = NULL;
    esp_eaf_player_config_t config = ESP_EAF_PLAYER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_eaf_player_open_memory(start, end - start, &config, &player));
    const esp_eaf_stream_info_t *info = esp_eaf_player_get_info(player);
    const esp_eaf_frame_t *frame = esp_eaf_player_get_frame(player);
    assert(info && info->frame_count > 0 && info->width > 0 && info->height > 0);
    assert(frame && frame->pixels && frame->pixel_bytes > 0);
    ESP_ERROR_CHECK(esp_eaf_player_next(player));
    esp_eaf_player_close(player);
}

static void verify_legacy_bgra8888(void)
{
    esp_eaf_player_t *player = NULL;
    esp_eaf_player_config_t config = ESP_EAF_PLAYER_CONFIG_DEFAULT();
    config.output_format = ESP_EAF_OUT_BGRA8888;
    ESP_ERROR_CHECK(esp_eaf_player_open_memory(legacy_eaf_start,
                                               legacy_eaf_end - legacy_eaf_start,
                                               &config, &player));
    const esp_eaf_stream_info_t *info = esp_eaf_player_get_info(player);
    const esp_eaf_frame_t *frame = esp_eaf_player_get_frame(player);
    assert(info->pixel_format == ESP_EAF_OUT_BGRA8888);
    assert(info->has_alpha);
    assert(frame->stride == (size_t)info->width * 4);
    assert(frame->pixel_bytes == (size_t)info->width * info->height * 4);
    assert(frame->alpha == NULL);
    esp_eaf_player_close(player);
}

void app_main(void)
{
    verify(legacy_eaf_start, legacy_eaf_end);
    verify_legacy_bgra8888();
    verify(lossless_eaf4_start, lossless_eaf4_end);
    ESP_LOGI("eaf_core_test", "GUI-independent EAF core test passed");
}
