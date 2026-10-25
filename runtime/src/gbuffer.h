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
/* YW4_GBUFFER=native (our draws and programs) or =programs (our programs in the game's draws,
 * step 2a): gpu_draw.c asks per draw whether our programs replace the game's. F6 switches
 * between native and the game's. */
int gbuffer_native(void);
int gbuffer_native_frame(uint64_t gpu_frame);  /* the same, with YW4_GBUFFER_ALTERNATE applied */
void gbuffer_toggle(void);
/* Step 2b, YW4_GBUFFER=native: a G-buffer draw of ours, recorded in place of the game's
 * DrawElements. Our buffers and object constants; the rest (material constants, textures,
 * vertex formats, targets, pipeline state) is what the game bound before it. */
typedef struct {
    uint64_t vertices, indices;
    uint32_t vertex_bytes, index_type, index_count, primitive;  /* index_type, primitive: NVN's */
    uint64_t game[7];  /* the game's DrawElements (x1..x7), drawn instead on the game's frames */
    float object[64];  /* vertex constants 2: rows 0-2 world, 8-11 projection x view x world,
                        * 12-15 last frame's world */
} GBufferDraw;
/* nvn_cmdbuf.c, DrawElements(cmd, primitive, type, count, address): 1 if our draw went in its
 * place (then the game's is not recorded). */
int gbuffer_take_draw(CPUState *c, const NvnCmdBuf *cb);
/* CMD_NATIVE callback (gpu_draw.c): `data` holds a GBufferDraw. */
void gpu_gbuffer_draw(void *vk_cb, void *gpu_state, const void *data, uint32_t size);

/* YW4_GBUFFER=check: nvn_cmdbuf.c calls gbuffer_check_draw for every draw while this is set. */
extern int gbuffer_checking;
/* index_addr 0: not indexed; type and count are the draw's (count: indices or vertices) */
void gbuffer_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, uint32_t type, uint32_t count);

#endif
