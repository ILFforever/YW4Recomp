/* Lamp shadows (decision 0081): the characters' shadows from the game's lamp lights.
 *
 * The game's lamps (point lights drawn as volumes, lamp_light.frag) cast no shadow. For the few lamps nearest the
 * characters we draw the characters into a small cube of depth images per lamp (six 90-degree views), the way a point
 * light's shadow is usually made, and the lamp program looks each pixel up in its lamp's cube.
 *
 * Three threads take part:
 * - the GPU executor, at the lamp draw (gpu_draw.c): the lamps of that frame, read from the draw's vertices
 *   (lamp_shadow_collect), and the table the lamp program reads (lamp_shadow_fill);
 * - the render thread drawing the sun's nearest shadow cascade (shadow.c): which lamps get shadows this frame
 *   (lamp_shadow_begin: the lit, still lamps nearest them, faded in and out), the characters' draws copied with each lamp view's matrix (lamp_shadow_add), and the pass
 *   queued into its command buffer (lamp_shadow_end);
 * - the executor again, running that pass (gpu_shadow.c gpu_lamp_shadow_pass) before the lamp draw. */
#ifndef YW4_LAMP_SHADOW_H
#define YW4_LAMP_SHADOW_H

#include <stdint.h>

#include "shadow.h"

enum {
    LAMP_SHADOW_LAMPS = 6,                       /* lamps with shadows at once (4 at first: swaps showed as blinks) */
    LAMP_SHADOW_FACES = 6,                       /* a cube: +X -X +Y -Y +Z -Z */
    LAMP_SHADOW_LAYERS = LAMP_SHADOW_LAMPS * LAMP_SHADOW_FACES,
    LAMP_SHADOW_DRAWS = 2048,
};

/* What the lamp program reads (binding 94, std140): the count and settings, the camera's view-to-world matrix (for
 * this draw's camera), each lamp's world position and reach, and each view's world-to-clip matrix. */
typedef struct {
    float info[4];                               /* lamps, texel size (1 / lamp_shadow_size()), fade start, end (m) */
    float view_to_world[16];                     /* row-major rows, as the shader's dot products use them */
    float lamp[LAMP_SHADOW_LAMPS][4];            /* world x y z, radius */
    float fade[8];                               /* each lamp's shadows faded in, 0..1 (2 vec4) */
    float face_vp[LAMP_SHADOW_LAYERS][16];       /* row-major */
} LampShadowTable;

typedef struct {
    ShadowDraw d;
    uint32_t layer;                              /* lamp x 6 + face */
    uint32_t carried;                            /* passes this draw has been carried over (0: drawn this frame) */
    uint64_t object;                             /* the character's object */
} LampShadowDraw;

/* One frame's lamp shadow pass: its lamps and views, and the characters' draws into them. */
typedef struct {
    uint32_t lamps, n;
    float lamp[LAMP_SHADOW_LAMPS][4];
    float fade[LAMP_SHADOW_LAMPS];
    float face_vp[LAMP_SHADOW_LAYERS][16];
    LampShadowDraw d[LAMP_SHADOW_DRAWS];
} LampShadowPass;

/* Texels per side of each view: 512, or YW4_LAMP_SHADOW_SIZE at start (256-2048). */
uint32_t lamp_shadow_size(void);

/* Executor, at the lamp draw: the lamps in its vertices (`count` vertices of `stride` bytes; colour at byte 16,
 * world position and radius at byte 32), for the next frames' choice. */
void lamp_shadow_collect(const uint8_t *vertices, uint32_t count, uint32_t stride);

/* Render thread, the sun's nearest cascade, once a frame (its other cascades add to the pass): choose the lamps nearest the characters, whose world
 * positions are given (x y z each), and every object in the layer (`present`). 0: no lamp shadows this frame. */
int lamp_shadow_begin(const float *characters, int ncharacters, const uint64_t *present, int npresent);
/* A character's draw as the sun's pass would make it (its mvp is replaced), its world matrix (row-major) and its
 * object's box (local space, min and max): copied for each view of each chosen lamp that can see it. */
void lamp_shadow_add(const ShadowDraw *d, const float world[16], const float box_lo[3], const float box_hi[3],
                     uint64_t object, uint64_t submesh);
/* A pass is being built (the nearest cascade's first layer began it; the frame's other shadow layers add to it). */
int lamp_shadow_open(void);
/* Debugging: a pass was queued only when the next frame's shadows began (its farthest cascade's end never came). */
void lamp_shadow_note_late(void);
/* Debugging (blinks): a character's submesh the shadow pass saw in the lamps' layer but could not copy (why: 0 not a
 * learned character, 1 our draw of it failed), and a character's submesh in a farther cascade's layer (object). */
void lamp_shadow_note_skip(int why);
void lamp_shadow_note_far(uint64_t object);
/* The pass, to queue in the cascade's command buffer (NULL: nothing to draw this frame). */
LampShadowPass *lamp_shadow_end(void);

/* Executor, the lamp draw's binding 94: the table for the pass drawn last (lamp_shadow_drawn), with this draw's view
 * (camera constant rows 4-6, world to view). */
void lamp_shadow_fill(LampShadowTable *t, const float *camera_rows);
/* gpu_shadow.c, after drawing a pass: its lamps and views are what the images now hold. */
void lamp_shadow_drawn(const LampShadowPass *p);

/* gpu_shadow.c: the CMD_NATIVE callback (data: a LampShadowPass pointer), and the images for the lamp draw: made
 * (outside rendering: gpu_lamp_maps_prepare before the draw's rendering begins) and bound. */
void gpu_lamp_shadow_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
struct GpuState;
void gpu_lamp_maps_prepare(void *vk_cb, struct GpuState *s);
/* VkImageView of the 2D array, its VkSampler with depth compare and the one for raw depths (0: not made yet) */
int gpu_lamp_maps(void **view, void **sampler, void **raw);

#endif
