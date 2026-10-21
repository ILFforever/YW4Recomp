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

/* Linear rows (pitch wb * bpb) into a block-linear level. */
void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t block_height_log2, uint32_t block_depth_log2);

#endif
