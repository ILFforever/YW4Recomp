/* Our bloom pass (decision 0037): what the hook on the engine's bloom (bloom.c, render thread) hands the
 * executor (gpu_bloom.c) in its CMD_NATIVE record. */
#ifndef YW4_BLOOM_H
#define YW4_BLOOM_H

#include "nvn.h"

enum {
    BLOOM_IN_SCENE, BLOOM_IN_HALF,  /* read by the bright pass: renderer image records 0 and 11 */
    BLOOM_BRIGHT,                   /* its output, record 9 (half size) */
    BLOOM_L0_ACROSS, BLOOM_L0,      /* level 0 (quarter size): records 20 and 19 */
    BLOOM_L1_ACROSS, BLOOM_L1,      /* level 1: 22, 21 */
    BLOOM_L2_ACROSS, BLOOM_L2,      /* level 2: 24, 23 */
    BLOOM_IMAGES
};

typedef struct {
    NvnTexture tex[BLOOM_IMAGES];  /* copies taken at the hook (render targets: they stay the same) */
    float bright[8];               /* bright pass: scale and threshold for the scene, then for the half image */
    float step[3];                 /* blur step per level, in texture coordinates of the level's width/height */
    float composite[8];            /* brightness and weight per level: (l0, w0, l1, w1), (l2, w2, -, -) */
    int check;                     /* YW4_BLOOM=check: draw into copies and compare with the game's */
} BloomPass;

/* bloom.c: ours drawing (YW4_BLOOM, the settings panel)? */
int bloom_native(void);
void bloom_toggle(void);

void gpu_bloom_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
/* Check mode, after the game's composite draw (gpu_draw.c): copy the game's results out for comparison. */
void gpu_bloom_after_composite(void *vk_cb, void *gpu_state);
int gpu_bloom_pending(void);
void gpu_bloom_flush(void);  /* after the GPU finished: compare and log */

#endif
