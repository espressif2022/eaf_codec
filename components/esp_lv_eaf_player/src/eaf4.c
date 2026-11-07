#define _POSIX_C_SOURCE 200809L
#include "eaf4.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_log.h"
#define NOW_US() esp_timer_get_time()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* PSRAM is write-back with a 64-byte line. A short tail shares that line with
 * the next heap block, so the write-back corrupts its header. */
static size_t psram_bytes(size_t n)
{
    const size_t line = 64;
    return (n + line - 1) & ~(line - 1);
}
static void *allocate(size_t n)
{
    size_t bytes = psram_bytes(n);
    if (!n || bytes < n)
        return NULL;
    return heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void release_psram(void *p, size_t n)
{
    if (!p)
        return;
    if (n)
        esp_cache_msync(p, psram_bytes(n), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    free(p);
}
#else
static void *allocate(size_t n) { return malloc(n); }
static inline int64_t host_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
#define NOW_US() host_now_us()
#endif

struct eaf4 {
    FILE *file;
    const uint8_t *memory;
    eaf4_info_t info;
    uint8_t *index, *compressed, *scratch, *frame, *cached, *seek_snapshot;
    uint32_t cached_offset;
    uint32_t seek_snapshot_frame;
    bool has_seek_snapshot;
    size_t cached_bytes;
    eaf4_timing_t timing;
    uint32_t crc_table[8][256];
    uint32_t next, max_compressed, max_decoded;
};

static uint16_t u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Slicing-by-eight IEEE CRC-32. Tables belong to the decoder and reside
 * in internal RAM on ESP, avoiding flash-table fetches on every input byte. */
static void crc_init(eaf4_t *p)
{
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320U : 0);
        p->crc_table[0][i] = c;
    }
    for (unsigned t = 1; t < 8; ++t)
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t c = p->crc_table[t - 1][i];
            p->crc_table[t][i] = (c >> 8) ^ p->crc_table[0][c & 255];
        }
}

static uint32_t crc(eaf4_t *p, const void *data, size_t n)
{
    const uint8_t *b = data;
    const uint32_t (*t)[256] = p->crc_table;
    uint32_t c = 0xffffffffU;
    while (n >= 8) {
        uint32_t a = u32(b) ^ c, d = u32(b + 4);
        c = t[7][a & 255] ^ t[6][(a >> 8) & 255] ^
            t[5][(a >> 16) & 255] ^ t[4][a >> 24] ^
            t[3][d & 255] ^ t[2][(d >> 8) & 255] ^
            t[1][(d >> 16) & 255] ^ t[0][d >> 24];
        b += 8;
        n -= 8;
    }
    while (n--)
        c = (c >> 8) ^ t[0][(c ^ *b++) & 255];
    return ~c;
}

void eaf4_release_cache(eaf4_t *p)
{
    if (!p)
        return;
    if (p->file) {
        fclose(p->file);
        p->file = NULL;
    }
#ifdef ESP_PLATFORM
    release_psram(p->index, (size_t)p->info.frames * 24);
    release_psram(p->compressed, p->max_compressed);
    release_psram(p->cached, p->cached_bytes);
    release_psram(p->scratch, p->max_decoded);
    release_psram(p->seek_snapshot, p->info.frame_bytes);
#else
    free(p->index);
    free(p->compressed);
    free(p->cached);
    free(p->scratch);
    free(p->seek_snapshot);
#endif
    p->index = NULL;
    p->compressed = NULL;
    p->cached = NULL;
    p->scratch = NULL;
    p->seek_snapshot = NULL;
    p->cached_bytes = 0;
    p->has_seek_snapshot = false;
}

void eaf4_close(eaf4_t *p)
{
    if (!p)
        return;
    eaf4_release_cache(p);
#ifdef ESP_PLATFORM
    release_psram(p->frame, p->info.frame_bytes);
#else
    free(p->frame);
#endif
    free(p);
}

const eaf4_info_t *eaf4_info(const eaf4_t *p) { return p ? &p->info : NULL; }
const eaf4_timing_t *eaf4_timing(const eaf4_t *p) { return p ? &p->timing : NULL; }

static int open_source(const char *path, const void *memory, size_t memory_size, eaf4_t **out)
{
    if ((!path && !memory) || !out)
        return -1;
    *out = NULL;
#ifdef ESP_PLATFORM
    eaf4_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    eaf4_t *p = calloc(1, sizeof(*p));
#endif
    if (!p)
        return -2;
    crc_init(p);
    uint8_t h[64];
    long size;
    p->memory = memory;
    if (memory) {
        if (memory_size < 64 || memory_size > 32 * 1024 * 1024) goto bad;
        size = memory_size;
        memcpy(h, memory, 64);
    } else {
        p->file = fopen(path, "rb");
        if (!p->file || fseek(p->file, 0, SEEK_END)) goto bad;
        size = ftell(p->file);
        if (size < 64 || size > 32 * 1024 * 1024 || fseek(p->file, 0, SEEK_SET) || fread(h, 1, 64, p->file) != 64) goto bad;
    }
    if (memcmp(h, "EAF4", 4) || u16(h + 4) != 1 || u16(h + 6) != 64 || u16(h + 36) != 4 || u16(h + 38) != 4 ||
        u32(h + 48) != 1)
        goto bad;
    uint32_t expected = u32(h + 40);
    memset(h + 40, 0, 4);
    if (crc(p, h, 64) != expected)
        goto bad;
    for (unsigned i = 52; i < 64; i++)
        if (h[i])
            goto bad;
    p->info = (eaf4_info_t){u16(h + 8), u16(h + 10), u32(h + 12), u32(h + 16), u32(h + 20)};
    eaf4_info_t *v = &p->info;
    if (!v->width || !v->height || v->width > 1024 || v->height > 1024 || v->width % 4 || v->height % 4 ||
        !v->frames || v->frames > 10000 || v->period_ms < 10 || v->period_ms > 10000 ||
        v->frame_bytes != (uint32_t)v->width * v->height * 4 || u32(h + 24) != 64 ||
        u32(h + 28) != 64 + v->frames * 24 || u32(h + 32) != (uint32_t)size)
        goto bad;
    uint32_t index_size = v->frames * 24, offset = u32(h + 28);
    if (offset > (uint32_t)size)
        goto bad;
    p->index = allocate(index_size);
    if (!p->index)
        goto bad;
    if (memory) memcpy(p->index, p->memory + 64, index_size);
    if ((!memory && fread(p->index, 1, index_size, p->file) != index_size) || crc(p, p->index, index_size) != u32(h + 44))
        goto bad;
    uint32_t num_tiles = (uint32_t)v->width * v->height / 16;
    uint32_t mask_bytes = (num_tiles + 7) / 8;
    uint32_t max_possible_decoded = v->frame_bytes + num_tiles + 4096;
    for (uint32_t i = 0; i < v->frames; i++) {
        const uint8_t *r = p->index + i * 24;
        uint32_t length = u32(r + 4), decoded = u32(r + 8);
        uint8_t mode = r[16];
        if (u32(r) != offset || !length || length > max_possible_decoded || mode > 6 || r[17] ||
            u16(r + 18) || (!i && mode != EAF4_MODE_RAW && mode != EAF4_MODE_BTC8888_SAFE) ||
            offset > (uint32_t)size || length > (uint32_t)size - offset)
            goto bad;
        if (mode <= EAF4_MODE_PLANAR && decoded != v->frame_bytes)
            goto bad;
        if ((mode == EAF4_MODE_TILE_PLANAR || mode == EAF4_MODE_TILE_PACKED) &&
            (decoded < mask_bytes || decoded > v->frame_bytes + mask_bytes || (decoded - mask_bytes) % 64))
            goto bad;
        if (mode == EAF4_MODE_JOINT_DELTA &&
            (decoded < mask_bytes || decoded > v->frame_bytes + num_tiles * 16 + mask_bytes))
            goto bad;
        if (mode == EAF4_MODE_BTC8888_SAFE &&
            (decoded < num_tiles || decoded > v->frame_bytes + num_tiles))
            goto bad;
        offset += length;
        if (length > p->max_compressed)
            p->max_compressed = length;
        if (decoded > p->max_decoded)
            p->max_decoded = decoded;
    }
    if (offset != (uint32_t)size)
        goto bad;
    p->compressed = allocate(p->max_compressed);
    p->scratch = allocate(p->max_decoded);
    p->frame = allocate(v->frame_bytes);
    if (!p->compressed || !p->scratch || !p->frame)
        goto bad;
    memset(p->frame, 0, v->frame_bytes);
    /* Optional compressed-data cache. Keep the streaming buffers so allocation
     * failure still permits playback without changing the on-disk format. */
    p->cached_offset = 64 + index_size;
    size_t payload_size = (size_t)size - p->cached_offset;
    if (!memory && payload_size <= 4 * 1024 * 1024) {
        p->cached = allocate(payload_size);
        if (p->cached) {
            p->cached_bytes = payload_size;
            if (fseek(p->file, p->cached_offset, SEEK_SET) ||
                fread(p->cached, 1, payload_size, p->file) != payload_size) {
#ifdef ESP_PLATFORM
                release_psram(p->cached, p->cached_bytes);
#else
                free(p->cached);
#endif
                p->cached = NULL;
                p->cached_bytes = 0;
                clearerr(p->file);
            }
        }
    }
#ifdef ESP_PLATFORM
    ESP_LOGI("eaf4", "compressed cache: %s, payload=%lu bytes",
             p->cached ? "PSRAM" : "streaming", (unsigned long)payload_size);
#endif
    *out = p;
    return 0;
bad:
    eaf4_close(p);
    return -1;
}

/* Assemble the native word whose memory bytes are B,G,R,A. */
static inline uint32_t channel_word(uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return ((uint32_t)b << 24) | ((uint32_t)g << 16) | ((uint32_t)r << 8) | a;
#else
    return b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | ((uint32_t)a << 24);
#endif
}

static int reconstruct_btc8888(eaf4_t *p, uint32_t decoded)
{
    uint32_t width = p->info.width;
    uint32_t columns = width / 4;
    uint32_t tiles = (p->info.frame_bytes / 4) / 16;
    if (decoded < tiles)
        return -1;
    const uint8_t *modes = p->scratch;
    const uint8_t *src = p->scratch + tiles;
    const uint8_t *end = p->scratch + decoded;
    uint32_t *output = (uint32_t *)p->frame;

    for (uint32_t t = 0; t < tiles; ++t) {
        uint8_t m = modes[t];
        if (m == 0)
            continue;
        uint32_t y = (t / columns) * 4, x = (t % columns) * 4;
        if (m == 1) {
            if (end - src < 4) return -1;
            uint32_t word = u32(src);
            src += 4;
            for (unsigned row = 0; row < 4; ++row) {
                uint32_t *dst = output + (y + row) * width + x;
                dst[0] = word;
                dst[1] = word;
                dst[2] = word;
                dst[3] = word;
            }
        } else if (m == 2) {
            if (end - src < 10) return -1;
            uint32_t c0 = u32(src), c1 = u32(src + 4);
            uint16_t mask = u16(src + 8);
            src += 10;
            for (unsigned row = 0; row < 4; ++row) {
                uint32_t *dst = output + (y + row) * width + x;
                for (unsigned col = 0; col < 4; ++col) {
                    dst[col] = ((mask >> (row * 4 + col)) & 1U) ? c1 : c0;
                }
            }
        } else if (m == 3) {
            if (end - src < 12) return -1;
            const uint8_t *c0 = src, *c3 = src + 4;
            uint32_t mask = u32(src + 8);
            src += 12;
            uint32_t pal[4];
            pal[0] = channel_word(c0[0], c0[1], c0[2], c0[3]);
            pal[1] = channel_word(
                (uint8_t)((2U * c0[0] + c3[0]) / 3U),
                (uint8_t)((2U * c0[1] + c3[1]) / 3U),
                (uint8_t)((2U * c0[2] + c3[2]) / 3U),
                (uint8_t)((2U * c0[3] + c3[3]) / 3U)
            );
            pal[2] = channel_word(
                (uint8_t)((c0[0] + 2U * c3[0]) / 3U),
                (uint8_t)((c0[1] + 2U * c3[1]) / 3U),
                (uint8_t)((c0[2] + 2U * c3[2]) / 3U),
                (uint8_t)((c0[3] + 2U * c3[3]) / 3U)
            );
            pal[3] = channel_word(c3[0], c3[1], c3[2], c3[3]);
            for (unsigned row = 0; row < 4; ++row) {
                uint32_t *dst = output + (y + row) * width + x;
                for (unsigned col = 0; col < 4; ++col) {
                    unsigned k = row * 4 + col;
                    dst[col] = pal[(mask >> (2U * k)) & 3U];
                }
            }
        } else if (m == 4) {
            if (end - src < 64) return -1;
            for (unsigned row = 0; row < 4; ++row) {
                uint32_t *dst = output + (y + row) * width + x;
                memcpy(dst, src + row * 16, 16);
            }
            src += 64;
        } else {
            return -1;
        }
    }
    return (src == end) ? 0 : -1;
}

static int reconstruct_joint_delta(eaf4_t *p, uint32_t decoded)
{
    uint32_t width = p->info.width;
    uint32_t columns = width / 4;
    uint32_t tiles = (p->info.frame_bytes / 4) / 16;
    uint32_t mask_size = (tiles + 7) / 8;
    if (decoded < mask_size)
        return -1;
    uint32_t changed = 0;
    for (uint32_t i = 0; i < tiles; i++)
        if (p->scratch[i / 8] & (1U << (i % 8)))
            changed++;
    if (changed == 0)
        return (decoded == mask_size) ? 0 : -1;
    uint32_t sym_count = changed * 16;
    if (decoded < mask_size + sym_count)
        return -1;
    const uint8_t *symbols = p->scratch + mask_size;
    uint32_t escaped = 0;
    for (uint32_t i = 0; i < sym_count; ++i)
        if (symbols[i] == 255)
            escaped++;
    if (decoded != mask_size + sym_count + escaped * 4)
        return -1;
    const uint8_t *esc = symbols + sym_count;
    uint32_t ord = 0, ex = 0;
    for (uint32_t t = 0; t < tiles; ++t) {
        if (!(p->scratch[t / 8] & (1U << (t % 8))))
            continue;
        uint32_t y = (t / columns) * 4, x = (t % columns) * 4;
        for (unsigned k = 0; k < 16; ++k) {
            uint8_t *q = p->frame + (((y + k / 4) * width + x + (k % 4)) * 4);
            uint8_t sym = symbols[ord++];
            if (sym == 255) {
                q[0] = esc[0 * escaped + ex];
                q[1] = esc[1 * escaped + ex];
                q[2] = esc[2 * escaped + ex];
                q[3] = esc[3 * escaped + ex];
                ex++;
            } else {
                q[0] = (uint8_t)(q[0] + (int)(sym / 27U) % 3 - 1);
                q[1] = (uint8_t)(q[1] + (int)(sym / 9U) % 3 - 1);
                q[2] = (uint8_t)(q[2] + (int)(sym / 3U) % 3 - 1);
                q[3] = (uint8_t)(q[3] + (int)(sym % 3U) - 1);
            }
        }
    }
    return 0;
}

int eaf4_next(eaf4_t *p, const uint8_t **pixels, uint32_t *frame_index)
{
    if (!p || !pixels)
        return -1;
    const uint8_t *r = p->index + p->next * 24;
    uint32_t length = u32(r + 4), decoded = u32(r + 8), count = p->info.frame_bytes / 4;
    uint8_t mode = r[16];
    int64_t start = NOW_US();
    const uint8_t *compressed = p->memory ? p->memory + u32(r) : p->cached ? p->cached + u32(r) - p->cached_offset : p->compressed;
    if ((!p->memory && !p->cached && (fseek(p->file, u32(r), SEEK_SET) ||
                       fread(p->compressed, 1, length, p->file) != length)) ||
        crc(p, compressed, length) != u32(r + 20))
        return -1;
    p->timing.read_us = (uint32_t)(NOW_US() - start);
    start = NOW_US();
    uLongf dest = decoded;
    uLong input = length;
    if (uncompress2(p->scratch, &dest, compressed, &input) != Z_OK || dest != decoded || input != length)
        return -1;
    p->timing.inflate_us = (uint32_t)(NOW_US() - start);
    start = NOW_US();
    if (mode == EAF4_MODE_RAW) {
        memcpy(p->frame, p->scratch, p->info.frame_bytes);
    } else if (mode == EAF4_MODE_XOR) {
        uint32_t *restrict output = (uint32_t *)p->frame;
        const uint32_t *restrict difference = (const uint32_t *)p->scratch;
        for (uint32_t i = 0; i < count; ++i) {
            output[i] ^= difference[i];
        }
    } else if (mode == EAF4_MODE_PLANAR) {
        uint32_t *restrict output = (uint32_t *)p->frame;
        const uint8_t *restrict b = p->scratch;
        const uint8_t *restrict g = b + count;
        const uint8_t *restrict r_plane = g + count;
        const uint8_t *restrict a = r_plane + count;
        for (uint32_t i = 0; i < count; ++i) {
            output[i] ^= channel_word(b[i], g[i], r_plane[i], a[i]);
        }
    } else if (mode == EAF4_MODE_TILE_PLANAR || mode == EAF4_MODE_TILE_PACKED) {
        uint32_t tiles = count / 16, mask_size = (tiles + 7) / 8, changed = 0;
        for (uint32_t i = 0; i < tiles; i++)
            if (p->scratch[i / 8] & (1U << (i % 8))) changed++;
        if (mask_size + changed * 64 != decoded) return -1;
        uint32_t ordinal = 0, columns = p->info.width / 4;
        uint32_t *restrict output = (uint32_t *)p->frame;
        const uint8_t *restrict payload = p->scratch + mask_size;
        if (mode == EAF4_MODE_TILE_PACKED) {
            for (uint32_t t = 0; t < tiles; ++t) {
                if (!(p->scratch[t / 8] & (1U << (t % 8)))) continue;
                uint32_t y = (t / columns) * 4, x = (t % columns) * 4;
                const uint8_t *tile_src = payload + ordinal * 64;
                for (unsigned row = 0; row < 4; ++row) {
                    uint32_t *destination = output + (y + row) * p->info.width + x;
                    const uint8_t *row_src = tile_src + row * 16;
                    destination[0] ^= u32(row_src + 0);
                    destination[1] ^= u32(row_src + 4);
                    destination[2] ^= u32(row_src + 8);
                    destination[3] ^= u32(row_src + 12);
                }
                ++ordinal;
            }
        } else {
            for (uint32_t t = 0; t < tiles; ++t) {
                if (!(p->scratch[t / 8] & (1U << (t % 8)))) continue;
                uint32_t y = (t / columns) * 4, x = (t % columns) * 4;
                const uint8_t *b = payload + ordinal * 16;
                const uint8_t *g = b + changed * 16;
                const uint8_t *r_plane = g + changed * 16;
                const uint8_t *a = r_plane + changed * 16;
                for (unsigned row = 0; row < 4; ++row) {
                    uint32_t *destination = output + (y + row) * p->info.width + x;
                    for (unsigned col = 0; col < 4; ++col) {
                        unsigned k = row * 4 + col;
                        destination[col] ^= channel_word(b[k], g[k], r_plane[k], a[k]);
                    }
                }
                ++ordinal;
            }
        }
    } else if (mode == EAF4_MODE_JOINT_DELTA) {
        if (reconstruct_joint_delta(p, decoded) != 0)
            return -1;
    } else if (mode == EAF4_MODE_BTC8888_SAFE) {
        if (reconstruct_btc8888(p, decoded) != 0)
            return -1;
    } else {
        return -1;
    }
    p->timing.reconstruct_us = (uint32_t)(NOW_US() - start);
#ifndef ESP_PLATFORM
    start = NOW_US();
    if (crc(p, p->frame, p->info.frame_bytes) != u32(r + 12))
        return -1;
    p->timing.crc_us = (uint32_t)(NOW_US() - start);
#else
    p->timing.crc_us = 0;
#endif
    *pixels = p->frame;
    if (frame_index)
        *frame_index = p->next;
    p->next = (p->next + 1) % p->info.frames;
    return 0;
}

int eaf4_seek(eaf4_t *p, uint32_t target_frame, const uint8_t **pixels)
{
    if (!p || target_frame >= p->info.frames)
        return -1;
    if (p->has_seek_snapshot && p->seek_snapshot && p->seek_snapshot_frame == target_frame) {
        memcpy(p->frame, p->seek_snapshot, p->info.frame_bytes);
        p->next = (target_frame + 1) % p->info.frames;
        if (pixels)
            *pixels = p->frame;
        return 0;
    }
    uint32_t key = target_frame;
    while (key > 0 && p->index[key * 24 + 16] != EAF4_MODE_RAW)
        key--;
    p->next = key;
    const uint8_t *px = NULL;
    for (uint32_t f = key; f <= target_frame; f++) {
        if (eaf4_next(p, &px, NULL) != 0)
            return -1;
    }
    if (key != target_frame) {
        if (!p->seek_snapshot)
            p->seek_snapshot = allocate(p->info.frame_bytes);
        if (p->seek_snapshot) {
            memcpy(p->seek_snapshot, p->frame, p->info.frame_bytes);
            p->seek_snapshot_frame = target_frame;
            p->has_seek_snapshot = true;
        }
    }
    if (pixels)
        *pixels = px;
    return 0;
}

int eaf4_open(const char *path, eaf4_t **out) { return open_source(path, NULL, 0, out); }
int eaf4_open_memory(const void *data, size_t size, eaf4_t **out) { return open_source(NULL, data, size, out); }
