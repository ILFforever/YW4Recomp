/* Our shadow-caster pass (decision 0024): what the render thread gathers (shadow.c) for the
 * executor to draw (gpu_shadow.c). */
#ifndef YW4_SHADOW_H
#define YW4_SHADOW_H

#include <stdint.h>

typedef struct {
    uint64_t vertices, indices;          /* guest addresses (in the imported memory pools) */
    uint32_t vertex_count, index_count;  /* index_count 0: not indexed */
    uint32_t stride, index_type;         /* NVN index type: 1 = 16-bit, 2 = 32-bit */
    uint8_t cull, front, pad[2];         /* NVN polygon state bytes 0 and 1, as the game drew it */
    float mvp[16];                       /* projection x view x world, row-major */
    float threshold;                     /* cut-out casters (texture != 0): the material's alpha threshold, */
    uint64_t texture;                    /* texture handle, and the texture coordinates' NVN format and */
    uint32_t uv_format, uv_offset;       /* offset in the vertex */
    uint32_t palette_offset, palette_size;  /* skinned casters (size != 0): the bones in the palette ring */
} ShadowDraw;

typedef struct {
    uint64_t depth_addr;   /* the cascade's layer of the shadow map: the executor checks that the */
    uint32_t depth_layer;  /* game's commands left exactly this target bound */
    uint32_t n;
    ShadowDraw d[];
} ShadowPass;

/* A skinned caster's bone palette (size bytes, 3x4 per bone) copied into the executor's ring,
 * on the thread that gathers the pass; its offset there. 0 if it can't be. */
int gpu_shadow_palette(const void *src, uint32_t size, uint32_t *offset);
/* CMD_NATIVE callback: `data` holds a ShadowPass pointer. */
void gpu_shadow_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
/* F9: switch between the game's shadow casters and ours (shadow.c) */
void shadow_toggle(void);
/* Passes skipped because another target was bound, and draws skipped (memory outside the pools). */
void gpu_shadow_counts(long *passes_skipped, long *draws_skipped);

#endif
