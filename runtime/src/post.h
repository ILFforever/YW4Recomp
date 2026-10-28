/* Our post-processing passes (decisions 0037 bloom, 0038 distance blur): what the hooks on the engine's passes
 * (bloom.c, dof.c, render thread) hand the executor (gpu_post.c) in their CMD_NATIVE records. */
#ifndef YW4_POST_H
#define YW4_POST_H

#include "nvn.h"

/* ---- bloom (bloom.c) */

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

/* ---- distance blur (dof.c) */

enum {
    DOF_SCENE,   /* record 0: shrunk, and the composite's target */
    DOF_A,       /* record 9: the shrunk scene, then each round's result (half size) */
    DOF_B,       /* record 10: each round's across pass */
    DOF_DEPTH,   /* record 42: the G-buffer's depth, read by the composite */
    DOF_IMAGES
};

typedef struct {
    NvnTexture tex[DOF_IMAGES];
    uint32_t rounds;     /* blur rounds (renderer +0x868, at most 32) */
    float weights[8];    /* Gaussian weights, centre first */
    float spread;        /* +0x860: the blur step in pixels of the half-size image, before / its width or height */
    float texel[2];      /* the composite's neighbour distance: +0x86c / the depth's width, height */
    float focus[8];      /* the composite's fragment constants, rows 0 and 1 */
    float far;           /* the camera's far distance (renderer +0x5c4), if the camera block isn't bound */
    int check;           /* YW4_DOF_PASS=check */
} DofPass;

/* dof.c: ours drawing (YW4_DOF_PASS, the settings panel)? */
int dof_native(void);
void dof_toggle(void);

void gpu_dof_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
void gpu_dof_after_composite(void *vk_cb, void *gpu_state);

/* ---- the engine's shared blur rounds f_7f3620 (dof.c): the distance blur's, the battle effects' (f_7f1bf0) and
 * two more callers; the distance blur pass of ours above draws its own rounds instead */

typedef struct {
    NvnTexture a, b;     /* read across into b, then down into a, per round */
    uint32_t rounds;
    float weights[8];
    float step[2];       /* across, down, in texture coordinates */
    int check;           /* YW4_BLUR_PASS=check: ours into copies; the game's runs after it (dof.c) */
} BlurPass;

int blur_native(void);
/* The fast blur rounds (YW4_BLUR_FAST, the settings panel's BATTLE BLUR): fewer, wider rounds; ours draw them. */
int blur_fast(void);
void blur_fast_toggle(void);
void gpu_blur_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
void gpu_blur_check_end(void *vk_cb, void *gpu_state, const void *data, uint32_t size);  /* after the game's rounds */

/* All checks: results waiting to be compared; compare and log (after the GPU finished). */
int gpu_post_pending(void);
void gpu_post_flush(void);

#endif
