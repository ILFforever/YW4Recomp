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
/* YW4_GBUFFER=check: nvn_cmdbuf.c calls gbuffer_check_draw for every draw while this is set. */
extern int gbuffer_checking;
void gbuffer_check_draw(const NvnCmdBuf *cb, uint64_t index_addr);

#endif
