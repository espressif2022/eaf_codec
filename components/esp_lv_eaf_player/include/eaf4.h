#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internal frame sub-modes inside EAF4 (0x04 LOSSLESS & 0x05 BTC_SAFE).
 *
 * Under the unified codec table:
 *   - Codec 0x04 (LOSSLESS) uses frame sub-modes 0..5 automatically per frame:
 *       0 = Raw keyframe Deflate
 *       1 = Full-frame 32-bit XOR Deflate
 *       2 = 4-plane B/G/R/A XOR Deflate
 *       3 = 4x4 tile mask + 4-plane XOR Deflate
 *       4 = 4x4 tile mask + 32-bit word XOR Deflate (fast tile-packed)
 *       5 = 4x4 tile mask + 81-state ternary joint delta + escapes
 *   - Codec 0x05 (BTC_SAFE) uses frame sub-mode 6:
 *       6 = 4x4 error-bounded BTC with 64B raw BGRA8888 escape
 */
typedef enum {
    EAF4_SUBMODE_RAW          = 0,
    EAF4_SUBMODE_XOR          = 1,
    EAF4_SUBMODE_PLANAR       = 2,
    EAF4_SUBMODE_TILE_PLANAR  = 3,
    EAF4_SUBMODE_TILE_PACKED  = 4,
    EAF4_SUBMODE_JOINT_DELTA  = 5,
    EAF4_SUBMODE_BTC8888_SAFE = 6,
} eaf4_submode_t;

/* Backward-compatible aliases */
#define EAF4_MODE_RAW          EAF4_SUBMODE_RAW
#define EAF4_MODE_XOR          EAF4_SUBMODE_XOR
#define EAF4_MODE_PLANAR       EAF4_SUBMODE_PLANAR
#define EAF4_MODE_TILE_PLANAR  EAF4_SUBMODE_TILE_PLANAR
#define EAF4_MODE_TILE_PACKED  EAF4_SUBMODE_TILE_PACKED
#define EAF4_MODE_JOINT_DELTA  EAF4_SUBMODE_JOINT_DELTA
#define EAF4_MODE_BTC8888_SAFE EAF4_SUBMODE_BTC8888_SAFE

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
