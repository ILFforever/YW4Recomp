/* Tegra X1 texture layout: NVN format sizes and the block-linear (GOB) layout.
 *
 * Adapted from LAS Recomp (runtime/nvn/tegra.h, formats.c; MIT, see third_party/lasrecomp).
 */
#ifndef TEGRA_H
#define TEGRA_H

#include <stdint.h>

enum { FMT_COLOR, FMT_DEPTH, FMT_STENCIL, FMT_DEPTH_STENCIL, FMT_COMPRESSED };

typedef struct {
    uint8_t bpb;     /* bytes per block (per texel when uncompressed) */
    uint8_t bw, bh;  /* block size in texels */
    uint8_t kind;
} FormatInfo;

/* NULL for a format number we don't know. */
const FormatInfo *nvn_format_info(uint32_t nvn_format);

typedef struct {
    uint32_t width, height, depth, layers, levels;  /* depth: of a 3D texture; layers: array layers or cube faces */
    const FormatInfo *fmt;
    uint32_t block_height_log2;  /* GOBs per block, level 0 */
    uint32_t block_depth_log2;
} TexLayout;

typedef struct {
    uint64_t offset;   /* from the start of the layer */
    uint32_t w, h, d;  /* texels */
    uint32_t wb, hb;   /* blocks */
    uint32_t block_height_log2, block_depth_log2;
} LevelLayout;

/* The block height the NVN driver picks, and the whole storage layout from it. */
void tegra_layout_init(TexLayout *t);
LevelLayout tegra_level(const TexLayout *t, uint32_t level);
uint64_t tegra_total_size(const TexLayout *t);

/* Size of one array layer (or cube face) with all its levels, aligned to a block. */
uint64_t tegra_layer_size(const TexLayout *t);

/* Writes a region of tightly packed linear data (rows of whole blocks) into level `level`
 * of layer `layer` of block-linear storage at `base`. x, y, z, w, h, d are in texels (z, d:
 * slices of a 3D texture). */
void tegra_write_region(const TexLayout *t, uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                        uint32_t z, uint32_t w, uint32_t h, uint32_t d, const uint8_t *src);

/* The reverse: a region of block-linear storage into tightly packed linear rows. */
void tegra_read_region(const TexLayout *t, const uint8_t *base, uint32_t level, uint32_t layer, uint32_t x, uint32_t y,
                       uint32_t z, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst);

/* Linear rows (pitch wb * bpb) into a block-linear level. */
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2);

#endif
