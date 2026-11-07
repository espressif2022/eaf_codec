#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zlib.h"

#include "esp_eaf_codec.h"
#include "esp_eaf_dec.h"

#define CONT_EAF4_8888    1
#define CONT_EAF_565      2
#define CONT_EAF_8888_OUT 3

static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void run_eaf4_item(int idx, const char *asset, const char *codec, const uint8_t *blob, uint32_t size,
                          ppa_client_handle_t ppa)
{
    size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    eaf4_t *player = NULL;
    if (eaf4_open_memory(blob, size, &player) != 0 || !player) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=open\n", idx, asset, codec);
        return;
    }

    size_t sram_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t sram_used = (sram_before > sram_after) ? (uint32_t)(sram_before - sram_after) : 0;
    uint32_t psram_used = (psram_before > psram_after) ? (uint32_t)(psram_before - psram_after) : 0;

    const eaf4_info_t *info = eaf4_info(player);
    uint32_t w = info->width, h = info->height, n = info->frames;
    size_t rgb_bytes = ((size_t)w * h * 3 + 63U) & ~63U;
    uint8_t *bg = heap_caps_aligned_alloc(64, rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *out_rgb = heap_caps_aligned_alloc(64, rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *dec_samples = calloc(n * 3, sizeof(uint32_t));
    uint32_t *pipe_samples = calloc(n * 3, sizeof(uint32_t));
    memset(bg, 0x40, rgb_bytes);
    memset(out_rgb, 0x00, rgb_bytes);
    esp_cache_msync(bg, rgb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(out_rgb, rgb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Warmup pass */
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *px = NULL;
        eaf4_next(player, &px, NULL);
    }

    uint64_t total_dec_us = 0, total_inf_us = 0, total_rec_us = 0, total_ppa_us = 0;
    uint32_t last_crc = 0;
    uint32_t sample_count = n * 3;

    for (uint32_t loop = 0; loop < 3; loop++) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *px = NULL;
            int64_t t0 = esp_timer_get_time();
            int rc = eaf4_next(player, &px, NULL);
            uint32_t dec_us = (uint32_t)(esp_timer_get_time() - t0);
            if (rc != 0 || !px) {
                printf("BENCH_ERROR idx=%d asset=%s codec=%s frame=%lu\n", idx, asset, codec, (unsigned long)i);
                goto cleanup;
            }
            const eaf4_timing_t *tm = eaf4_timing(player);
            total_dec_us += dec_us;
            total_inf_us += tm->inflate_us;
            total_rec_us += tm->reconstruct_us;
            dec_samples[loop * n + i] = dec_us;

            /* PPA hardware ARGB8888 over RGB888 blend */
            int64_t t_ppa = esp_timer_get_time();
            esp_cache_msync((void *)px, (info->frame_bytes + 63U) & ~63U, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            ppa_blend_oper_config_t bc = {
                .in_bg = {.buffer = bg, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h, .blend_cm = PPA_BLEND_COLOR_MODE_RGB888},
                .in_fg = {.buffer = px, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h, .blend_cm = PPA_BLEND_COLOR_MODE_ARGB8888},
                .out = {.buffer = out_rgb, .buffer_size = rgb_bytes, .pic_w = w, .pic_h = h, .blend_cm = PPA_BLEND_COLOR_MODE_RGB888},
                .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
                .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            ppa_do_blend(ppa, &bc);
            uint32_t ppa_us = (uint32_t)(esp_timer_get_time() - t_ppa);
            total_ppa_us += ppa_us;
            pipe_samples[loop * n + i] = dec_us + ppa_us;

            if (loop == 0 && i == n - 1) {
                last_crc = crc32(0, px, info->frame_bytes);
            }
            if ((i & 7U) == 7U) {
                vTaskDelay(1);
            }
        }
    }

    qsort(dec_samples, sample_count, sizeof(uint32_t), cmp_u32);
    qsort(pipe_samples, sample_count, sizeof(uint32_t), cmp_u32);

    printf("BENCH_RESULT idx=%d asset=%s codec=%s fmt=BGRA8888 w=%lu h=%lu frames=%lu "
           "comp_bytes=%lu sram_B=%lu psram_B=%lu "
           "dec_avg_us=%lu dec_p95_us=%lu dec_max_us=%lu inf_avg_us=%lu rec_avg_us=%lu "
           "ppa_avg_us=%lu pipe_avg_us=%lu pipe_p95_us=%lu crc=%08lx\n",
           idx, asset, codec, (unsigned long)w, (unsigned long)h, (unsigned long)n,
           (unsigned long)size, (unsigned long)sram_used, (unsigned long)psram_used,
           (unsigned long)(total_dec_us / sample_count),
           (unsigned long)dec_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)dec_samples[sample_count - 1],
           (unsigned long)(total_inf_us / sample_count),
           (unsigned long)(total_rec_us / sample_count),
           (unsigned long)(total_ppa_us / sample_count),
           (unsigned long)((total_dec_us + total_ppa_us) / sample_count),
           (unsigned long)pipe_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)last_crc);

cleanup:
    free(dec_samples);
    free(pipe_samples);
    free(bg);
    free(out_rgb);
    eaf4_close(player);
}

static void run_eaf565_item(int idx, const char *asset, const char *codec, const uint8_t *blob, uint32_t size,
                            bool out_8888)
{
    size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    esp_eaf_format_handle_t handle = NULL;
    if (esp_eaf_format_init(blob, size, &handle) != ESP_OK || !handle) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=eaf_init\n", idx, asset, codec);
        return;
    }
    esp_eaf_decoder_t *dec = esp_eaf_decoder_create();
    esp_eaf_decoder_set_output_format(dec, out_8888 ? ESP_EAF_OUT_BGRA8888 : ESP_EAF_OUT_RGB565);

    int total_frames = esp_eaf_format_get_total_frames(handle);
    const uint8_t *f0 = esp_eaf_format_get_frame_data(handle, 0);
    int f0_sz = esp_eaf_format_get_frame_size(handle, 0);
    esp_eaf_header_t hdr0 = {0};
    if (esp_eaf_header_parse(f0, f0_sz, &hdr0) != ESP_EAF_FORMAT_VALID) {
        esp_eaf_decoder_destroy(dec);
        esp_eaf_format_deinit(handle);
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=hdr0\n", idx, asset, codec);
        return;
    }
    uint32_t w = hdr0.width, h = hdr0.height;
    esp_eaf_free_header(&hdr0);

    size_t px_bytes = (size_t)w * h * (out_8888 ? 4 : 2);
    size_t a_bytes = (size_t)w * h;
    uint8_t *fb = heap_caps_aligned_alloc(64, (px_bytes + 63U) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *ab = heap_caps_aligned_alloc(64, (a_bytes + 63U) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memset(fb, 0, px_bytes);
    memset(ab, 0xFF, a_bytes);

    /* Do 1 warmup frame to trigger internal decoder scratch allocation before measuring RAM */
    int image_frames = 0;
    for (int f = 0; f < total_frames; f++) {
        const uint8_t *fdata = esp_eaf_format_get_frame_data(handle, f);
        int fsz = esp_eaf_format_get_frame_size(handle, f);
        if (!fdata || fsz < 2 || memcmp(fdata, "_S", 2) != 0) continue;
        esp_eaf_header_t hdr = {0};
        if (esp_eaf_header_parse(fdata, fsz, &hdr) == ESP_EAF_FORMAT_VALID) {
            if (image_frames == 0) esp_eaf_decoder_reset(dec);
            esp_eaf_palette_cache_t cache;
            memset(cache.color, 0xFF, sizeof(cache.color));
            size_t b_stride = (size_t)hdr.block_height * w * (out_8888 ? 4 : 2);
            for (int b = 0; b < hdr.blocks; b++) {
                esp_eaf_block_decode_ctx(dec, &hdr, fdata, b, fb + b * b_stride, ab, false, &cache);
            }
            image_frames++;
        }
        esp_eaf_free_header(&hdr);
    }

    size_t sram_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t sram_used = (sram_before > sram_after) ? (uint32_t)(sram_before - sram_after) : 0;
    uint32_t psram_used = (psram_before > psram_after) ? (uint32_t)(psram_before - psram_after) : 0;

    uint32_t sample_count = (uint32_t)image_frames * 3;
    uint32_t *dec_samples = calloc(sample_count, sizeof(uint32_t));
    uint64_t total_dec_us = 0;
    uint32_t s_idx = 0;
    uint32_t last_crc = 0;

    for (int loop = 0; loop < 3; loop++) {
        int img_f = 0;
        for (int f = 0; f < total_frames; f++) {
            const uint8_t *fdata = esp_eaf_format_get_frame_data(handle, f);
            int fsz = esp_eaf_format_get_frame_size(handle, f);
            if (!fdata || fsz < 2 || memcmp(fdata, "_S", 2) != 0) continue;
            esp_eaf_header_t hdr = {0};
            if (esp_eaf_header_parse(fdata, fsz, &hdr) == ESP_EAF_FORMAT_VALID) {
                int64_t t0 = esp_timer_get_time();
                if (img_f == 0) esp_eaf_decoder_reset(dec);
                esp_eaf_palette_cache_t cache;
                memset(cache.color, 0xFF, sizeof(cache.color));
                size_t b_stride = (size_t)hdr.block_height * w * (out_8888 ? 4 : 2);
                for (int b = 0; b < hdr.blocks; b++) {
                    esp_eaf_block_decode_ctx(dec, &hdr, fdata, b, fb + b * b_stride, ab, false, &cache);
                }
                uint32_t du = (uint32_t)(esp_timer_get_time() - t0);
                total_dec_us += du;
                if (s_idx < sample_count) dec_samples[s_idx++] = du;
                img_f++;
            }
            esp_eaf_free_header(&hdr);
            if ((img_f & 7) == 7) vTaskDelay(1);
        }
        if (loop == 0) {
            last_crc = crc32(0, fb, px_bytes);
        }
    }

    qsort(dec_samples, sample_count, sizeof(uint32_t), cmp_u32);
    printf("BENCH_RESULT idx=%d asset=%s codec=%s fmt=%s w=%lu h=%lu frames=%lu "
           "comp_bytes=%lu sram_B=%lu psram_B=%lu "
           "dec_avg_us=%lu dec_p95_us=%lu dec_max_us=%lu inf_avg_us=0 rec_avg_us=0 "
           "ppa_avg_us=0 pipe_avg_us=%lu pipe_p95_us=%lu crc=%08lx\n",
           idx, asset, codec, out_8888 ? "BGRA8888" : "RGB565",
           (unsigned long)w, (unsigned long)h, (unsigned long)image_frames,
           (unsigned long)size, (unsigned long)sram_used, (unsigned long)psram_used,
           (unsigned long)(total_dec_us / sample_count),
           (unsigned long)dec_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)dec_samples[sample_count - 1],
           (unsigned long)(total_dec_us / sample_count),
           (unsigned long)dec_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)last_crc);

    free(dec_samples);
    free(fb);
    free(ab);
    esp_eaf_decoder_destroy(dec);
    esp_eaf_format_deinit(handle);
}

void app_main(void)
{
    esp_eaf_hw_jpeg_init();

    ppa_client_handle_t ppa = NULL;
    ppa_client_config_t cc = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ESP_ERROR_CHECK(ppa_register_client(&cc, &ppa));

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "bench");
    ESP_ERROR_CHECK(part ? ESP_OK : ESP_ERR_NOT_FOUND);

    const void *mapped = NULL;
    esp_partition_mmap_handle_t mh;
    ESP_ERROR_CHECK(esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &mh));

    const uint8_t *base = (const uint8_t *)mapped;
    ESP_ERROR_CHECK(memcmp(base, "MBPK", 4) == 0 ? ESP_OK : ESP_FAIL);
    uint32_t count = u32(base + 8);
    uint32_t total_size = u32(base + 12);

    printf("S31_BENCH_SUITE_BEGIN items=%lu total_bytes=%lu free_sram=%u free_psram=%u\n",
           (unsigned long)count, (unsigned long)total_size,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = base + 64 + i * 96;
        uint32_t off = u32(rec + 0);
        uint32_t sz = u32(rec + 4);
        uint32_t ctype = u32(rec + 8);
        char asset[33] = {0}, codec[33] = {0};
        memcpy(asset, rec + 20, 32);
        memcpy(codec, rec + 52, 32);

        /* Copy compressed blob into PSRAM so we measure pure PSRAM/CPU/DMA decode speed */
        uint8_t *ram_blob = heap_caps_aligned_alloc(64, (sz + 63U) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(ram_blob ? ESP_OK : ESP_ERR_NO_MEM);
        memcpy(ram_blob, base + off, sz);
        esp_cache_msync(ram_blob, (sz + 63U) & ~63U, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        if (ctype == CONT_EAF4_8888) {
            run_eaf4_item((int)i, asset, codec, ram_blob, sz, ppa);
        } else {
            run_eaf565_item((int)i, asset, codec, ram_blob, sz, ctype == CONT_EAF_8888_OUT);
        }

        free(ram_blob);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    printf("S31_BENCH_SUITE_END items=%lu status=0\n", (unsigned long)count);
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
