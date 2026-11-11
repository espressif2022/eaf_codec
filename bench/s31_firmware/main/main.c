#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zlib.h"

#include "soc/soc_caps.h"

#if CONFIG_IDF_TARGET_ESP32S3
#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_ops.h"
#endif

#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
typedef ppa_client_handle_t bench_post_handle_t;
#else
typedef void *bench_post_handle_t;
#endif

#include "esp_eaf_codec.h"
#include "esp_eaf_internal.h"

#define CONT_EAF4_8888    1
#define CONT_EAF_565      2
#define CONT_EAF_8888_OUT 3
#define BENCH_LOOPS       10

#if CONFIG_IDF_TARGET_ESP32S3
#define BENCH_LCD_WIDTH   480
#define BENCH_LCD_HEIGHT  480
static esp_lcd_panel_handle_t s_lcd_panel;

static void display_draw_splash(void)
{
    const size_t pixels = (size_t)BENCH_LCD_WIDTH * BENCH_LCD_HEIGHT;
    uint16_t *frame = heap_caps_malloc(pixels * sizeof(*frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        ESP_LOGE("eaf_display", "Unable to allocate splash frame");
        return;
    }
    for (int y = 0; y < BENCH_LCD_HEIGHT; y++) {
        for (int x = 0; x < BENCH_LCD_WIDTH; x++) {
            unsigned r = (unsigned)x * 31U / (BENCH_LCD_WIDTH - 1);
            unsigned g = (unsigned)y * 63U / (BENCH_LCD_HEIGHT - 1);
            unsigned b = (unsigned)(x + y) * 31U / (BENCH_LCD_WIDTH + BENCH_LCD_HEIGHT - 2);
            if (((x / 60) + (y / 60)) & 1) {
                r = (r + 6U > 31U) ? 31U : r + 6U;
                b = (b + 6U > 31U) ? 31U : b + 6U;
            }
            frame[(size_t)y * BENCH_LCD_WIDTH + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, BENCH_LCD_WIDTH, BENCH_LCD_HEIGHT, frame));
    free(frame);
}

static void display_init(void)
{
    ESP_ERROR_CHECK(bsp_display_new(NULL, &s_lcd_panel, NULL));
    ESP_ERROR_CHECK(s_lcd_panel ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(bsp_display_get_h_res() == BENCH_LCD_WIDTH && bsp_display_get_v_res() == BENCH_LCD_HEIGHT
                    ? ESP_OK : ESP_ERR_INVALID_SIZE);
    /* GC9503 initialization already sends the display-on command and the BSP
     * then releases its temporary 3-wire command IO. Calling disp_on_off()
     * here would address that released IO and fail on SUB2. */
    display_draw_splash();
    printf("EAF_DISPLAY_READY panel=480x480 sub_board=SUB2 driver=GC9503CV\n");
}

static void display_show_frame(const void *rgb565, uint32_t width, uint32_t height,
                               int idx, const char *asset, const char *codec)
{
    if (!s_lcd_panel || !rgb565 || width != BENCH_LCD_WIDTH || height != BENCH_LCD_HEIGHT) {
        return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, width, height, rgb565));
    printf("EAF_DISPLAY_FRAME idx=%d asset=%s codec=%s\n", idx, asset, codec);
}

static void display_mark_result(bool ok)
{
    if (!s_lcd_panel) return;
    enum { BORDER = 12 };
    uint16_t *horizontal = heap_caps_malloc(BENCH_LCD_WIDTH * BORDER * sizeof(*horizontal), MALLOC_CAP_8BIT);
    uint16_t *vertical = heap_caps_malloc(BORDER * (BENCH_LCD_HEIGHT - 2 * BORDER) * sizeof(*vertical), MALLOC_CAP_8BIT);
    if (!horizontal || !vertical) {
        free(horizontal);
        free(vertical);
        return;
    }
    uint16_t color = ok ? 0x07E0 : 0xF800;
    for (size_t i = 0; i < BENCH_LCD_WIDTH * BORDER; i++) horizontal[i] = color;
    for (size_t i = 0; i < BORDER * (BENCH_LCD_HEIGHT - 2 * BORDER); i++) vertical[i] = color;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, BENCH_LCD_WIDTH, BORDER, horizontal));
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, BENCH_LCD_HEIGHT - BORDER,
                                              BENCH_LCD_WIDTH, BENCH_LCD_HEIGHT, horizontal));
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, BORDER, BORDER,
                                              BENCH_LCD_HEIGHT - BORDER, vertical));
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_lcd_panel, BENCH_LCD_WIDTH - BORDER, BORDER,
                                              BENCH_LCD_WIDTH, BENCH_LCD_HEIGHT - BORDER, vertical));
    free(horizontal);
    free(vertical);
}
#else
static void display_init(void) {}
static void display_show_frame(const void *rgb565, uint32_t width, uint32_t height,
                               int idx, const char *asset, const char *codec)
{
    (void)rgb565; (void)width; (void)height; (void)idx; (void)asset; (void)codec;
}
static void display_mark_result(bool ok) { (void)ok; }
#endif

static uint32_t u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

#if !SOC_PPA_SUPPORTED
static void bgra8888_to_rgb565_over_bg(const uint8_t *src, uint16_t *dst, size_t pixels)
{
    const unsigned bg_r = 32, bg_g = 40, bg_b = 48;
    for (size_t i = 0; i < pixels; i++) {
        unsigned b = src[i * 4 + 0], g = src[i * 4 + 1];
        unsigned r = src[i * 4 + 2], a = src[i * 4 + 3];
        unsigned inv = 255 - a;
        r = (r * a + bg_r * inv + 127) / 255;
        g = (g * a + bg_g * inv + 127) / 255;
        b = (b * a + bg_b * inv + 127) / 255;
        dst[i] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    }
}
#endif

static bool run_eaf4_item(int idx, const char *asset, const char *codec, const uint8_t *blob, uint32_t size,
                          bench_post_handle_t post_handle)
{
    size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    eaf4_t *player = NULL;
    if (eaf4_open_memory(blob, size, &player) != 0 || !player) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=open\n", idx, asset, codec);
        return false;
    }

    const eaf4_info_t *info = eaf4_info(player);
    uint32_t w = info->width, h = info->height, n = info->frames;
    size_t pixels = (size_t)w * h;
#if SOC_PPA_SUPPORTED
    size_t rgb_bytes = (pixels * 3 + 63U) & ~63U;
#else
    size_t rgb_bytes = (pixels * 2 + 63U) & ~63U;
#endif
#if SOC_PPA_SUPPORTED
    uint8_t *bg = heap_caps_aligned_alloc(64, rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    uint8_t *bg = NULL;
#endif
    uint8_t *out_rgb = heap_caps_aligned_alloc(64, rgb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (n == 0 || !out_rgb
#if SOC_PPA_SUPPORTED
        || !bg
#endif
    ) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=alloc_eaf4\n", idx, asset, codec);
        free(bg);
        free(out_rgb);
        eaf4_close(player);
        return false;
    }
#if SOC_PPA_SUPPORTED
    memset(bg, 0x40, rgb_bytes);
    esp_cache_msync(bg, rgb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
    memset(out_rgb, 0x00, rgb_bytes);
    esp_cache_msync(out_rgb, rgb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Warmup pass */
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *px = NULL;
        eaf4_next(player, &px, NULL);
    }

    size_t sram_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t sram_used = (sram_before > sram_after) ? (uint32_t)(sram_before - sram_after) : 0;
    uint32_t psram_used = (psram_before > psram_after) ? (uint32_t)(psram_before - psram_after) : 0;
    uint32_t *dec_samples = calloc(n * BENCH_LOOPS, sizeof(uint32_t));
    uint32_t *pipe_samples = calloc(n * BENCH_LOOPS, sizeof(uint32_t));
    if (!dec_samples || !pipe_samples) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=samples_alloc\n", idx, asset, codec);
        free(dec_samples);
        free(pipe_samples);
        free(bg);
        free(out_rgb);
        eaf4_close(player);
        return false;
    }

    uint64_t total_dec_us = 0, total_inf_us = 0, total_rec_us = 0, total_post_us = 0;
    uint32_t last_crc = 0;
    uint32_t sample_count = n * BENCH_LOOPS;
    bool ok = true;

    for (uint32_t loop = 0; loop < BENCH_LOOPS; loop++) {
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *px = NULL;
            int64_t t0 = esp_timer_get_time();
            int rc = eaf4_next(player, &px, NULL);
            uint32_t dec_us = (uint32_t)(esp_timer_get_time() - t0);
            if (rc != 0 || !px) {
                printf("BENCH_ERROR idx=%d asset=%s codec=%s frame=%lu\n", idx, asset, codec, (unsigned long)i);
                ok = false;
                goto cleanup;
            }
            const eaf4_timing_t *tm = eaf4_timing(player);
            total_dec_us += dec_us;
            total_inf_us += tm->inflate_us;
            total_rec_us += tm->reconstruct_us;
            dec_samples[loop * n + i] = dec_us;

            int64_t t_post = esp_timer_get_time();
            if (info->pixel_format == EAF4_PIXFMT_BGRA8888) {
#if SOC_PPA_SUPPORTED
                ppa_client_handle_t ppa = (ppa_client_handle_t)post_handle;
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
#else
                (void)post_handle;
                bgra8888_to_rgb565_over_bg(px, (uint16_t *)out_rgb, pixels);
#endif
            }
            uint32_t post_us = (info->pixel_format == EAF4_PIXFMT_BGRA8888)
                                   ? (uint32_t)(esp_timer_get_time() - t_post)
                                   : 0;
            total_post_us += post_us;
            pipe_samples[loop * n + i] = dec_us + post_us;

            if (loop == 0 && i == n - 1) {
                last_crc = crc32(0, px, info->frame_bytes);
            }
            /* Yield outside the timed region so long full-screen runs do not
             * starve IDLE0 and trip the task watchdog. */
            vTaskDelay(1);
        }
    }

    qsort(dec_samples, sample_count, sizeof(uint32_t), cmp_u32);
    qsort(pipe_samples, sample_count, sizeof(uint32_t), cmp_u32);

    const char *fmt_str = (info->pixel_format == EAF4_PIXFMT_BGRA8888) ? "BGRA8888" :
                          (info->pixel_format == EAF4_PIXFMT_RGB565A8) ? "RGB565A8" : "RGB565";
    printf("BENCH_RESULT idx=%d asset=%s codec=%s fmt=%s w=%lu h=%lu frames=%lu "
           "comp_bytes=%lu sram_B=%lu psram_B=%lu "
           "dec_avg_us=%lu dec_p95_us=%lu dec_max_us=%lu inf_avg_us=%lu rec_avg_us=%lu "
           "post_avg_us=%lu pipe_avg_us=%lu pipe_p95_us=%lu crc=%08lx post_mode=%s\n",
           idx, asset, codec, fmt_str, (unsigned long)w, (unsigned long)h, (unsigned long)n,
           (unsigned long)size, (unsigned long)sram_used, (unsigned long)psram_used,
           (unsigned long)(total_dec_us / sample_count),
           (unsigned long)dec_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)dec_samples[sample_count - 1],
           (unsigned long)(total_inf_us / sample_count),
           (unsigned long)(total_rec_us / sample_count),
           (unsigned long)(total_post_us / sample_count),
           (unsigned long)((total_dec_us + total_post_us) / sample_count),
           (unsigned long)pipe_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)last_crc,
#if SOC_PPA_SUPPORTED
           "ppa_bgra8888_to_rgb888"
#else
           "cpu_bgra8888_to_rgb565_alpha"
#endif
    );

    display_show_frame(out_rgb, w, h, idx, asset, codec);

cleanup:
    free(dec_samples);
    free(pipe_samples);
    free(bg);
    free(out_rgb);
    eaf4_close(player);
    return ok;
}

static bool run_eaf565_item(int idx, const char *asset, const char *codec, const uint8_t *blob, uint32_t size,
                            bool out_8888)
{
    size_t sram_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    esp_eaf_format_handle_t handle = NULL;
    if (esp_eaf_format_init(blob, size, &handle) != ESP_OK || !handle) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=eaf_init\n", idx, asset, codec);
        return false;
    }
    esp_eaf_decoder_t *dec = esp_eaf_decoder_create();
    if (!dec) {
        esp_eaf_format_deinit(handle);
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=decoder_alloc\n", idx, asset, codec);
        return false;
    }
    esp_eaf_decoder_set_output_format(dec, out_8888 ? ESP_EAF_OUT_BGRA8888 : ESP_EAF_OUT_RGB565);

    int total_frames = esp_eaf_format_get_total_frames(handle);
    const uint8_t *f0 = esp_eaf_format_get_frame_data(handle, 0);
    int f0_sz = esp_eaf_format_get_frame_size(handle, 0);
    esp_eaf_header_t hdr0 = {0};
    if (esp_eaf_header_parse(f0, f0_sz, &hdr0) != ESP_EAF_FORMAT_VALID) {
        esp_eaf_decoder_destroy(dec);
        esp_eaf_format_deinit(handle);
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=hdr0\n", idx, asset, codec);
        return false;
    }
    uint32_t w = hdr0.width, h = hdr0.height;
    esp_eaf_free_header(&hdr0);

    size_t px_bytes = (size_t)w * h * (out_8888 ? 4 : 2);
    size_t a_bytes = (size_t)w * h;
    uint8_t *fb = heap_caps_aligned_alloc(64, (px_bytes + 63U) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *ab = heap_caps_aligned_alloc(64, (a_bytes + 63U) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb || !ab) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=framebuffer_alloc\n", idx, asset, codec);
        free(fb);
        free(ab);
        esp_eaf_decoder_destroy(dec);
        esp_eaf_format_deinit(handle);
        return false;
    }
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

    uint32_t sample_count = (uint32_t)image_frames * BENCH_LOOPS;
    if (sample_count == 0) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=no_frames\n", idx, asset, codec);
        free(fb);
        free(ab);
        esp_eaf_decoder_destroy(dec);
        esp_eaf_format_deinit(handle);
        return false;
    }
    uint32_t *dec_samples = calloc(sample_count, sizeof(uint32_t));
    if (!dec_samples) {
        printf("BENCH_ERROR idx=%d asset=%s codec=%s err=samples_alloc\n", idx, asset, codec);
        free(fb);
        free(ab);
        esp_eaf_decoder_destroy(dec);
        esp_eaf_format_deinit(handle);
        return false;
    }
    uint64_t total_dec_us = 0;
    uint32_t s_idx = 0;
    uint32_t last_crc = 0;

    for (int loop = 0; loop < BENCH_LOOPS; loop++) {
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
            vTaskDelay(1);
        }
        if (loop == 0) {
            last_crc = crc32(0, fb, px_bytes);
        }
    }

    qsort(dec_samples, sample_count, sizeof(uint32_t), cmp_u32);
    printf("BENCH_RESULT idx=%d asset=%s codec=%s fmt=%s w=%lu h=%lu frames=%lu "
           "comp_bytes=%lu sram_B=%lu psram_B=%lu "
           "dec_avg_us=%lu dec_p95_us=%lu dec_max_us=%lu inf_avg_us=0 rec_avg_us=0 "
           "post_avg_us=0 pipe_avg_us=%lu pipe_p95_us=%lu crc=%08lx post_mode=direct_rgb565\n",
           idx, asset, codec, out_8888 ? "BGRA8888" : "RGB565",
           (unsigned long)w, (unsigned long)h, (unsigned long)image_frames,
           (unsigned long)size, (unsigned long)sram_used, (unsigned long)psram_used,
           (unsigned long)(total_dec_us / sample_count),
           (unsigned long)dec_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)dec_samples[sample_count - 1],
           (unsigned long)(total_dec_us / sample_count),
           (unsigned long)dec_samples[(sample_count - 1) * 95 / 100],
           (unsigned long)last_crc);

    if (!out_8888) {
        display_show_frame(fb, w, h, idx, asset, codec);
    }

    free(dec_samples);
    free(fb);
    free(ab);
    esp_eaf_decoder_destroy(dec);
    esp_eaf_format_deinit(handle);
    return true;
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
#if CONFIG_COMPILER_OPTIMIZATION_PERF
    const char *optimization = "performance";
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
    const char *optimization = "size";
#else
    const char *optimization = "debug";
#endif
    printf("EAF_BENCH_ENV idf=%s target=%s revision=%u cores=%u cpu_mhz=%u optimization=%s "
           "input=flash_mmap output=rgb565 panel=480x480\n",
           esp_get_idf_version(), CONFIG_IDF_TARGET, (unsigned)chip_info.revision,
           (unsigned)chip_info.cores, (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           optimization);

    display_init();

    esp_eaf_hw_jpeg_init();

    bench_post_handle_t post_handle = NULL;
#if SOC_PPA_SUPPORTED
    ppa_client_handle_t ppa = NULL;
    ppa_client_config_t cc = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    ESP_ERROR_CHECK(ppa_register_client(&cc, &ppa));
    post_handle = (bench_post_handle_t)ppa;
#endif

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

    printf("EAF_BENCH_SUITE_BEGIN items=%lu total_bytes=%lu free_sram=%u free_psram=%u\n",
           (unsigned long)count, (unsigned long)total_size,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    bool suite_ok = true;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *rec = base + 64 + i * 96;
        uint32_t off = u32(rec + 0);
        uint32_t sz = u32(rec + 4);
        uint32_t ctype = u32(rec + 8);
        char asset[33] = {0}, codec[33] = {0};
        memcpy(asset, rec + 20, 32);
        memcpy(codec, rec + 52, 32);

#if SOC_PPA_SUPPORTED
        /* P4/S31: preserve the historical pure-PSRAM input path. */
        uint8_t *ram_blob = heap_caps_aligned_alloc(64, (sz + 63U) & ~63U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(ram_blob ? ESP_OK : ESP_ERR_NO_MEM);
        memcpy(ram_blob, base + off, sz);
        esp_cache_msync(ram_blob, (sz + 63U) & ~63U, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#else
        /* ESP32-S3 has 8 MiB PSRAM; decode directly from mapped flash so a
         * full-screen EAF4 frame, LCD framebuffer and scratch buffers fit. */
        const uint8_t *ram_blob = base + off;
#endif

        if (ctype == CONT_EAF4_8888) {
            suite_ok &= run_eaf4_item((int)i, asset, codec, ram_blob, sz, post_handle);
        } else {
            suite_ok &= run_eaf565_item((int)i, asset, codec, ram_blob, sz, ctype == CONT_EAF_8888_OUT);
        }

#if SOC_PPA_SUPPORTED
        free(ram_blob);
#endif
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    printf("EAF_BENCH_SUITE_END items=%lu status=%d\n", (unsigned long)count, suite_ok ? 0 : 1);
    display_mark_result(suite_ok);
    fflush(stdout);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
