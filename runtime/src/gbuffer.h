/* Our G-buffer pass (decision 0026): the model renderer's G-buffer layers (kind 2). */
#ifndef YW4_GBUFFER_H
#define YW4_GBUFFER_H

#include <stdint.h>

#include "nvn.h"
#include "yw4rt.h"

/* From the model renderer hook (shadow.c), at the start of every layer. */
void gbuffer_layer_start(const CPUState *ctx);
/* From the submesh-draw hook (shadow.c), before every submesh draw. */
void gbuffer_submesh(const CPUState *ctx);
/* The shadow pass drew the submesh noted last itself (the game's draw is skipped): forget it. */
void gbuffer_disarm(void);
/* The renderer's world for the submesh about to be drawn ([sp+0x30]+0x40 at the submesh-draw
 * hook, 3 rows of 4): 1 if found. The shadow pass's too. */
int gbuffer_hook_world(const CPUState *ctx, float out[12]);
/* YW4_GBUFFER=native (our draws and programs) or =programs (our programs in the game's draws,
 * step 2a): gpu_draw.c asks per draw whether our programs replace the game's. F6 switches
 * between native and the game's. */
int gbuffer_native(void);
int gbuffer_native_frame(uint64_t gpu_frame);  /* the same, with YW4_GBUFFER_ALTERNATE applied */
void gbuffer_toggle(void);
/* Step 2b, YW4_GBUFFER=native: a G-buffer draw of ours, recorded in place of the game's
 * DrawElements. Our buffers and object constants; the rest (material constants, textures,
 * vertex formats, targets, pipeline state) is what the game bound before it. */
enum { GBUFFER_OBJECT_FLOATS = 80 };  /* 20 rows: the programs read up to row 19 */
typedef struct {
    uint64_t vertices, indices;
    uint32_t vertex_bytes, vertex_count, index_type, index_count, primitive;  /* index_type, primitive: NVN's;
                                                                  * no indices: index_count 0 */
    uint64_t game[7];  /* the game's draw (x1..x7), drawn instead on the game's frames */
    int game_indexed;  /* it was DrawElements, else DrawArrays */
    int game_instanced;  /* ...Instanced: game[] has its base vertex (indexed), base instance, instances */
    uint32_t base_instance, instances;  /* ours: the game's, so the instanced programs read the same
                                         * copies (vertex constants 11, bound by the game) */
    float object[GBUFFER_OBJECT_FLOATS];  /* vertex constants 2: world, projection x view x world,
                                           * last frame's world, in the program's rows (gbuffer.c) */
} GBufferDraw;
/* nvn_cmdbuf.c, DrawElements(cmd, primitive, type, count, address) (indexed) or DrawArrays(cmd,
 * primitive, first, count), or their Instanced forms (then base vertex if indexed, base instance,
 * instances follow): 1 if our draw went in its place (then the game's is not recorded). */
int gbuffer_take_draw(CPUState *c, const NvnCmdBuf *cb, int indexed, int instanced);
/* CMD_NATIVE callback (gpu_draw.c): copies the bound colour targets in the frames of
 * YW4_CAPTURE_LAYER_AT; `data` is a name. */
void gpu_capture_layer(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
/* Same-frame A/B (gpu_ab.c, YW4_AB_AT): its marker's CMD_NATIVE function ('b' or 'e', then the
 * pass's name), whether the pass is the one compared, and the second run (the game's draws). */
void gpu_ab_native(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
int gpu_ab_wanted(const char *pass);
void gbuffer_end_recording(const CPUState *ctx, uint64_t cmd);  /* nvn_cmdbuf.c: EndRecording(cmd) */
extern int gpu_ab_game;
/* CMD_NATIVE callback (gpu_draw.c): `data` holds a GBufferDraw. */
void gpu_gbuffer_draw(void *vk_cb, void *gpu_state, const void *data, uint32_t size);

/* YW4_GBUFFER=check: nvn_cmdbuf.c calls gbuffer_check_draw for every draw while this is set. */
extern int gbuffer_checking;
/* index_addr 0: not indexed; type and count are the draw's (count: indices or vertices) */
void gbuffer_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, uint32_t type, uint32_t count);

#endif
