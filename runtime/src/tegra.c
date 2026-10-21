/* Tegra X1 texture layout (tegra.h).
 *
 * Block-linear: a GOB is 64 bytes x 8 rows (512 bytes); a block stacks 2^bh GOBs vertically
 * and 2^bd in depth; a level is whole blocks. Smaller mip levels shrink the block height so
 * a block is never much taller than the level.
 *
 * Adapted from LAS Recomp (runtime/nvn/tegra.c, formats.c; MIT, see third_party/lasrecomp).
 */
#include <string.h>

#include "tegra.h"

#define C(bpb) {bpb, 1, 1, FMT_COLOR}
#define B(bpb, w, h) {bpb, w, h, FMT_COMPRESSED}

/* NVNformat -> size. Zero entries are formats we don't know (nvn_format_info: NULL). */
static const FormatInfo formats[] = {
    [0x01] = C(1), [0x02] = C(1), [0x03] = C(1), [0x04] = C(1),                 /* R8 */
    [0x05] = C(2), [0x06] = C(2), [0x07] = C(2), [0x08] = C(2), [0x09] = C(2),  /* R16 */
    [0x0A] = C(4), [0x0B] = C(4), [0x0C] = C(4),                                /* R32 */
    [0x0D] = C(2), [0x0E] = C(2), [0x0F] = C(2), [0x10] = C(2),                 /* RG8 */
    [0x11] = C(4), [0x12] = C(4), [0x13] = C(4), [0x14] = C(4), [0x15] = C(4),  /* RG16 */
    [0x16] = C(8), [0x17] = C(8), [0x18] = C(8),                                /* RG32 */
    [0x19] = C(3), [0x1A] = C(3), [0x1B] = C(3), [0x1C] = C(3),                 /* RGB8 */
    [0x1D] = C(6), [0x1E] = C(6), [0x1F] = C(6), [0x20] = C(6), [0x21] = C(6),  /* RGB16 */
    [0x22] = C(12), [0x23] = C(12), [0x24] = C(12),                             /* RGB32 */
    [0x25] = C(4), [0x26] = C(4), [0x27] = C(4), [0x28] = C(4),                 /* RGBA8 */
    [0x29] = C(8), [0x2A] = C(8), [0x2B] = C(8), [0x2C] = C(8), [0x2D] = C(8),  /* RGBA16 */
    [0x2E] = C(16), [0x2F] = C(16), [0x30] = C(16),                             /* RGBA32 */
    [0x31] = {1, 1, 1, FMT_STENCIL},                                            /* STENCIL8 */
    [0x32] = {2, 1, 1, FMT_DEPTH}, [0x33] = {4, 1, 1, FMT_DEPTH}, [0x34] = {4, 1, 1, FMT_DEPTH},  /* DEPTH16/24/32F */
    [0x35] = {4, 1, 1, FMT_DEPTH_STENCIL}, [0x36] = {8, 1, 1, FMT_DEPTH_STENCIL},  /* DEPTH24_STENCIL8, DEPTH32F_STENCIL8 */
    [0x37] = C(4), [0x38] = C(4),                                               /* RGBX8 / RGBA8 sRGB */
    [0x39] = C(2), [0x3A] = C(2), [0x3B] = C(2), [0x3C] = C(2),                 /* RGBA4, RGB5, RGB5A1, RGB565 */
    [0x3D] = C(4), [0x3E] = C(4), [0x3F] = C(4), [0x40] = C(4),                 /* RGB10A2(UI), R11G11B10F, RGB9E5 */
    [0x41] = B(8, 4, 4), [0x42] = B(8, 4, 4), [0x43] = B(16, 4, 4), [0x44] = B(16, 4, 4),  /* BC1-3 */
    [0x45] = B(8, 4, 4), [0x46] = B(8, 4, 4), [0x47] = B(16, 4, 4), [0x48] = B(16, 4, 4),  /* BC1-3 sRGB */
    [0x49] = B(8, 4, 4), [0x4A] = B(8, 4, 4), [0x4B] = B(16, 4, 4), [0x4C] = B(16, 4, 4),  /* BC4, BC5 */
    [0x4D] = B(16, 4, 4), [0x4E] = B(16, 4, 4), [0x4F] = B(16, 4, 4), [0x50] = B(16, 4, 4),  /* BC6H, BC7 */
    /* vertex formats: integers read as floats */
    [0x51] = C(1), [0x52] = C(1), [0x53] = C(2), [0x54] = C(2), [0x55] = C(4), [0x56] = C(4),
    [0x57] = C(2), [0x58] = C(2), [0x59] = C(4), [0x5A] = C(4), [0x5B] = C(8), [0x5C] = C(8),
    [0x5D] = C(3), [0x5E] = C(3), [0x5F] = C(6), [0x60] = C(6), [0x61] = C(12), [0x62] = C(12),
    [0x63] = C(4), [0x64] = C(4), [0x65] = C(8), [0x66] = C(8), [0x67] = C(16), [0x68] = C(16),
    [0x69] = C(4), [0x6A] = C(4), [0x6B] = C(4), [0x6C] = C(4),
    [0x6D] = C(4), [0x6E] = C(4), [0x6F] = C(4), [0x70] = C(4),                 /* RGBX8 */
    [0x71] = C(8), [0x72] = C(8), [0x73] = C(8), [0x74] = C(8), [0x75] = C(8),  /* RGBX16 */
    [0x76] = C(16), [0x77] = C(16), [0x78] = C(16),                             /* RGBX32 */
    /* ASTC, linear then sRGB */
    [0x79] = B(16, 4, 4), [0x7A] = B(16, 5, 4), [0x7B] = B(16, 5, 5), [0x7C] = B(16, 6, 5),
    [0x7D] = B(16, 6, 6), [0x7E] = B(16, 8, 5), [0x7F] = B(16, 8, 6), [0x80] = B(16, 8, 8),
    [0x81] = B(16, 10, 5), [0x82] = B(16, 10, 6), [0x83] = B(16, 10, 8), [0x84] = B(16, 10, 10),
    [0x85] = B(16, 12, 10), [0x86] = B(16, 12, 12),
    [0x87] = B(16, 4, 4), [0x88] = B(16, 5, 4), [0x89] = B(16, 5, 5), [0x8A] = B(16, 6, 5),
    [0x8B] = B(16, 6, 6), [0x8C] = B(16, 8, 5), [0x8D] = B(16, 8, 6), [0x8E] = B(16, 8, 8),
    [0x8F] = B(16, 10, 5), [0x90] = B(16, 10, 6), [0x91] = B(16, 10, 8), [0x92] = B(16, 10, 10),
    [0x93] = B(16, 12, 10), [0x94] = B(16, 12, 12),
    [0x95] = C(2), [0x96] = C(2), [0x97] = C(2),                                /* BGR5, BGR5A1, A1BGR5 */
    [0x98] = C(4), [0x99] = C(4), [0x9A] = C(4), [0x9B] = C(4),                 /* BGRX8, BGRA8 (+ sRGB) */
};

const FormatInfo *nvn_format_info(uint32_t f) {
    return f < sizeof formats / sizeof *formats && formats[f].bpb ? &formats[f] : NULL;
}

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

void tegra_layout_init(TexLayout *t) {
    uint32_t gobs = div_up(div_up(t->height, t->fmt->bh), 8), bh = 0, bd = 0;
    while ((1u << bh) < gobs && bh < 4) bh++;  /* up to 16 GOBs per block */
    while (t->depth > 1 && (1u << bd) < t->depth && bd < 5) bd++;
    t->block_height_log2 = bh;
    t->block_depth_log2 = bd;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = t->block_height_log2, bd = t->block_depth_log2;
    while (bh > 0 && (8u << (bh - 1)) >= hb) bh--;
    while (bd > 0 && (1u << (bd - 1)) >= d) bd--;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, 8u << bh) * div_up(d, 1u << bd);
    if (out) *out = (LevelLayout){0, w, h, d, wb, hb, bh, bd};
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l;
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t layer = 0;
    for (uint32_t i = 0; i < t->levels; i++) layer += level_size(t, i, NULL);
    if (t->layers > 1) layer = align_up(layer, 512ull << t->block_height_log2);  /* layers start on a block */
    return layer;
}

uint64_t tegra_total_size(const TexLayout *t) { return tegra_layer_size(t) * (t->layers ? t->layers : 1); }

/* Byte offset of (x bytes, row y, slice z) in a block-linear surface. */
static uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh, uint32_t bd,
                           uint64_t block_rows) {
    uint32_t gob_y = y >> 3;
    uint64_t block_size = 512ull << (bh + bd);
    uint64_t off = ((uint64_t)(z >> bd) * block_rows + (gob_y >> bh)) * width_gobs * block_size;
    off += (uint64_t)(xb >> 6) * block_size;
    off += ((uint64_t)(z & ((1u << bd) - 1)) << bh | (gob_y & ((1u << bh) - 1))) * 512;
    uint32_t xi = xb & 63, yi = y & 7;  /* inside the GOB: 16-byte sectors */
    return off + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 15));
}

/* Copies a region between block-linear storage and tightly packed linear rows. */
static void copy_region(const TexLayout *t, uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint8_t *linear, int to_linear) {
    LevelLayout l = tegra_level(t, level);
    uint8_t *surface = base + (uint64_t)layer * tegra_layer_size(t) + l.offset;
    uint32_t bpb = t->fmt->bpb, bw = t->fmt->bw, bh = t->fmt->bh;
    uint32_t x0 = x / bw, y0 = y / bh, wb = div_up(w, bw), hb = div_up(h, bh);
    uint32_t width_gobs = div_up(l.wb * bpb, 64);
    uint64_t block_rows = div_up(l.hb, 8u << l.block_height_log2);
    if ((x0 * bpb) % 16 == 0 && 16 % bpb == 0) {  /* 16-byte runs are contiguous in both layouts */
        uint32_t row = wb * bpb;
        for (uint32_t k = 0; k < d; k++)
            for (uint32_t j = 0; j < hb; j++)
                for (uint32_t xb = 0; xb < row; xb += 16) {
                    uint8_t *b = surface + gob_offset(x0 * bpb + xb, y0 + j, z + k, width_gobs, l.block_height_log2,
                                                      l.block_depth_log2, block_rows);
                    uint8_t *lin = linear + ((uint64_t)k * hb + j) * row + xb;
                    size_t n = row - xb < 16 ? row - xb : 16;
                    if (to_linear) memcpy(lin, b, n);
                    else memcpy(b, lin, n);
                }
        return;
    }
    for (uint32_t k = 0; k < d; k++)
        for (uint32_t j = 0; j < hb; j++)
            for (uint32_t i = 0; i < wb; i++) {
                uint8_t *b = surface + gob_offset((x0 + i) * bpb, y0 + j, z + k, width_gobs, l.block_height_log2,
                                                  l.block_depth_log2, block_rows);
                uint8_t *lin = linear + (((uint64_t)k * hb + j) * wb + i) * bpb;
                if (to_linear) memcpy(lin, b, bpb);
                else memcpy(b, lin, bpb);
            }
}

void tegra_write_region(const TexLayout *t, uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d, const uint8_t *src) {
    copy_region(t, base, level, layer, x, y, z, w, h, d, (uint8_t *)src, 0);
}

void tegra_read_region(const TexLayout *t, const uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                       uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst) {
    copy_region(t, (uint8_t *)base, level, layer, x, y, z, w, h, d, dst, 1);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb, uint32_t bh,
                   uint32_t bd) {
    uint32_t row = wb * bpb, width_gobs = div_up(row, 64);
    uint64_t block_rows = div_up(hb, 8u << bh);
    for (uint32_t z = 0; z < d; z++)
        for (uint32_t y = 0; y < hb; y++)
            for (uint32_t x = 0; x < row; x += 16)  /* 16-byte runs are contiguous in both layouts */
                memcpy(dst + gob_offset(x, y, z, width_gobs, bh, bd, block_rows),
                       src + ((uint64_t)z * hb + y) * row + x, row - x < 16 ? row - x : 16);
}
