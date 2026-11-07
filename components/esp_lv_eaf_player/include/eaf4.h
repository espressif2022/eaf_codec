#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EAF4_MODE_RAW          = 0, /* Full-frame BGRA8888 Deflate (Keyframe, lossless) */
    EAF4_MODE_XOR          = 1, /* Full-frame BGRA8888 32-bit XOR Deflate (lossless) */
    EAF4_MODE_PLANAR       = 2, /* 0x0A 8888: 4-plane B/G/R/A XOR Deflate (lossless) */
    EAF4_MODE_TILE_PLANAR  = 3, /* 0x0C 8888: 4x4 tile mask + 4-plane XOR Deflate (lossless) */
    EAF4_MODE_TILE_PACKED  = 4, /* 4x4 tile mask + interleaved 32-bit BGRA word XOR Deflate (lossless, fast) */
    EAF4_MODE_JOINT_DELTA  = 5, /* 0x0C joint ternary (-1..1)^4 symbol + 4-plane escape Deflate (lossless) */
    EAF4_MODE_BTC8888_SAFE = 6, /* 0x09 8888: 4x4 error-bounded BTC with 64B raw BGRA escape */
} eaf4_mode_t;

typedef struct eaf4 eaf4_t;
typedef struct {
    uint16_t width, height;
    uint32_t frames, period_ms, frame_bytes;
} eaf4_info_t;
typedef struct {
    uint32_t read_us, inflate_us, reconstruct_us, crc_us;
} eaf4_timing_t;

const eaf4_timing_t *eaf4_timing(const eaf4_t *player);

/* Streaming file decoder. Returned pixels are straight-alpha B,G,R,A bytes.
 * Each next() verifies the compressed CRC. Host builds also verify the
 * reconstructed frame CRC. No LVGL dependency; the buffer is owned by the decoder. */
int eaf4_open(const char *path, eaf4_t **out);
/* Borrow immutable bytes; caller keeps the mapping alive until close. */
int eaf4_open_memory(const void *data, size_t size, eaf4_t **out);
const eaf4_info_t *eaf4_info(const eaf4_t *player);
int eaf4_next(eaf4_t *player, const uint8_t **pixels, uint32_t *frame_index);
int eaf4_seek(eaf4_t *player, uint32_t target_frame, const uint8_t **pixels);
void eaf4_release_cache(eaf4_t *player);
void eaf4_close(eaf4_t *player);

#ifdef __cplusplus
}
#endif
