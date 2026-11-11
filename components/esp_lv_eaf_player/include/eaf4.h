#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Internal frame sub-modes inside the unified EAF4 container.
 *
 * Header offset 52..63 (previously reserved zeros) carries container metadata:
 *   - h[52] = pixel_format:
 *       0 = EAF4_PIXFMT_BGRA8888 (4 bytes/pixel, default for v1 compatibility)
 *       1 = EAF4_PIXFMT_RGB565   (2 bytes/pixel)
 *       2 = EAF4_PIXFMT_RGB565A8 (3 bytes/pixel: 2*W*H RGB565 followed by W*H A8)
 *   - h[53] = top_codec_id:
 *       0 = auto from frame sub-mode (v1 compatibility), or 0x04..0x07
 *
 * Frame sub-modes stored in Index Entry byte 16 (`mode`):
 *   - BGRA8888 (pixel_format == 0):
 *       0..5 = Codec 0x04 (LOSSLESS: Raw, XOR, Planar, Tile-Planar, Tile-Packed, Joint-Delta)
 *       6    = Codec 0x05 (BTC_SAFE: 4x4 error-bounded BTC with 64B raw escape)
 *   - RGB565 / RGB565A8 (pixel_format == 1 or 2):
 *       0    = Raw keyframe Deflate (2*W*H or 3*W*H bytes)
 *       7    = Codec 0x04 RGB565+A8 4x4 tile mask + planar XOR + A8 delta Deflate
 *       8    = Codec 0x04 RGB565+A8 4x4 tile mask + 81-state joint symbol + escapes Deflate
 *       9    = Codec 0x05 RGB565 4x4 error-bounded BTC with 32B raw escape Deflate
 */
typedef enum {
    EAF4_PIXFMT_BGRA8888 = 0,
    EAF4_PIXFMT_RGB565   = 1,
    EAF4_PIXFMT_RGB565A8 = 2,
} eaf4_pixfmt_t;

typedef enum {
    EAF4_SUBMODE_RAW            = 0,
    EAF4_SUBMODE_XOR            = 1,
    EAF4_SUBMODE_PLANAR         = 2,
    EAF4_SUBMODE_TILE_PLANAR    = 3,
    EAF4_SUBMODE_TILE_PACKED    = 4,
    EAF4_SUBMODE_JOINT_DELTA    = 5,
    EAF4_SUBMODE_BTC8888_SAFE   = 6,
    EAF4_SUBMODE_565A8_TILES    = 7,
    EAF4_SUBMODE_565A8_JOINT    = 8,
    EAF4_SUBMODE_BTC565_SAFE    = 9,
    EAF4_SUBMODE_MAX            = 10,
} eaf4_submode_t;

/* Backward-compatible aliases */
#define EAF4_MODE_RAW            EAF4_SUBMODE_RAW
#define EAF4_MODE_XOR            EAF4_SUBMODE_XOR
#define EAF4_MODE_PLANAR         EAF4_SUBMODE_PLANAR
#define EAF4_MODE_TILE_PLANAR    EAF4_SUBMODE_TILE_PLANAR
#define EAF4_MODE_TILE_PACKED    EAF4_SUBMODE_TILE_PACKED
#define EAF4_MODE_JOINT_DELTA    EAF4_SUBMODE_JOINT_DELTA
#define EAF4_MODE_BTC8888_SAFE   EAF4_SUBMODE_BTC8888_SAFE
#define EAF4_MODE_565A8_TILES    EAF4_SUBMODE_565A8_TILES
#define EAF4_MODE_565A8_JOINT    EAF4_SUBMODE_565A8_JOINT
#define EAF4_MODE_BTC565_SAFE    EAF4_SUBMODE_BTC565_SAFE

typedef struct eaf4 eaf4_t;
typedef struct {
    uint16_t width, height;
    uint32_t frames, period_ms, frame_bytes;
    uint8_t pixel_format; /*!< eaf4_pixfmt_t (0=BGRA8888, 1=RGB565, 2=RGB565A8) */
    uint8_t codec_id;     /*!< Top-level codec ID (0x04..0x05, or 0 for v1 default) */
    bool has_alpha;       /*!< True for BGRA8888 and RGB565A8 */
} eaf4_info_t;
typedef struct {
    uint32_t read_us, inflate_us, reconstruct_us, crc_us;
} eaf4_timing_t;

typedef struct {
    uint16_t x1, y1, x2, y2;
    bool valid;
} eaf4_dirty_rect_t;

const eaf4_timing_t *eaf4_timing(const eaf4_t *player);
void eaf4_get_dirty_rect(const eaf4_t *player, eaf4_dirty_rect_t *rect);

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
