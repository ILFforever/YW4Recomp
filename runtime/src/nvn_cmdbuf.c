/* NVN command buffers (decision 0017).
 *
 * Commands are recorded as CmdRec records into the command memory the game gives the
 * command buffer (AddCommandMemory), so the game's own memory management works as designed;
 * a list that outgrows a chunk continues in the next through a JUMP record. A command handle
 * is the guest address of a list's first record. Each record keeps the call's arguments and
 * copies of what its pointers point at (state objects, colours), because the game may reuse
 * that memory before the list runs.
 *
 * Headless: QueueSubmitCommands walks the lists and counts draws; nothing executes.
 *
 * Adapted from LAS Recomp (runtime/nvn/cmdbuf.c; MIT, see third_party/lasrecomp).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)  /* room always kept for the record that leaves a chunk */

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {  /* (cmd, device) */
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    if (!cb) nvn_fatal(c, "out of host memory");
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

NVN(nvnCommandBufferAddCommandMemory) {  /* (cmd, memory pool, offset, size) */
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t mem = HOST(NvnPool, A(1))->storage + A(2);
    if (cb->recording && cb->mem) raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec))->x[0] = mem;
    cb->mem = mem;
    cb->size = A(3);
    cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; }  /* (cmd, memory, size): the driver's; ours needs none */

/* Room for one record of `extra` bytes after the header. No memory callback is set by this
 * game, so running out is a stop. */
static CmdRec *rec(const CPUState *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    if (!cb->mem || cb->used + size + JUMP_RESERVE > cb->size)
        nvn_fatal(c, "out of command memory (%u bytes needed, %llu of %llu used)", size,
                  (unsigned long long)cb->used, (unsigned long long)cb->size);
    CmdRec *r = raw_rec(cb->mem + cb->used, op, size);
    cb->used += size;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = 1;
    rec(c, cb, CMD_BARRIER, 0);  /* the list starts with a harmless record, so it starts in memory that exists */
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = 0;
    RET(cb->start);
}

/* ---- generic recording: x1..x7 and s0..s3 as they were at the call */

static CmdRec *record(CPUState *c, uint32_t op, uint32_t extra) {
    CmdRec *r = rec(c, cb_of(A(0)), op, extra);
    memcpy(r->x, &c->x[1], sizeof r->x);
    for (int i = 0; i < 4; i++) r->f[i] = AF(i);
    return r;
}
/* ... with a copy of `len` bytes from the pointer in argument `arg` */
static CmdRec *record_copy(CPUState *c, uint32_t op, int arg, uint32_t len) {
    CmdRec *r = record(c, op, len);
    if (A(arg) && len) memcpy(r->extra, AP(arg), len);
    return r;
}
#define PLAIN(name, op) NVN(name) { record(c, op, 0); }

/* ---- state */
NVN(nvnCommandBufferBindProgram) {  /* (cmd, program, stage bits) */
    record(c, CMD_BIND_PROGRAM, 0)->x[0] = (uint64_t)(uintptr_t)HOST(NvnProgram, A(1));
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
/* (cmd, count, const state*) */
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }
PLAIN(nvnCommandBufferBindVertexBuffer, CMD_BIND_VERTEX_BUFFERS)      /* (cmd, index, address, size) */
PLAIN(nvnCommandBufferBindUniformBuffer, CMD_BIND_UNIFORM_BUFFERS)    /* (cmd, stage, index, address, size) */
PLAIN(nvnCommandBufferBindStorageBuffer, CMD_BIND_STORAGE_BUFFERS)    /* (cmd, stage, index, address, size) */
PLAIN(nvnCommandBufferBindTexture, CMD_BIND_TEXTURES)                 /* (cmd, stage, index, handle) */
PLAIN(nvnCommandBufferBindImage, CMD_BIND_IMAGES)                     /* (cmd, stage, index, handle) */
PLAIN(nvnCommandBufferBindTransformFeedbackBuffer, CMD_BIND_TRANSFORM_FEEDBACK)  /* (cmd, index, address, size) */
NVN(nvnCommandBufferSetTexturePool) { record(c, CMD_SET_TEXTURE_POOL, 0)->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, A(1)); }
NVN(nvnCommandBufferSetSamplerPool) { record(c, CMD_SET_SAMPLER_POOL, 0)->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, A(1)); }

/* SetRenderTargets(cmd, count, const NVNtexture *const *colours, const NVNtextureView *const *views,
 * const NVNtexture *depth, const NVNtextureView *depth view): host textures and copies of the views */
typedef struct {
    NvnTexture *color[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    NvnTexture *depth;
    NvnTextureView depth_view;
    uint8_t has_depth_view;
} RenderTargets;
NVN(nvnCommandBufferSetRenderTargets) {
    int n = (int)A(1);
    if (n < 0 || n > 8) nvn_fatal(c, "%d render targets", n);
    RenderTargets rt = {0};
    const uint64_t *colors = AP(2), *views = AP(3);
    for (int i = 0; i < n; i++) {
        rt.color[i] = colors && colors[i] ? HOST(NvnTexture, colors[i]) : NULL;
        if (views && views[i]) rt.has_view[i] = 1, memcpy(&rt.views[i], (const void *)(uintptr_t)views[i], sizeof rt.views[i]);
    }
    rt.depth = A(4) ? HOST(NvnTexture, A(4)) : NULL;
    if (A(5)) rt.has_depth_view = 1, memcpy(&rt.depth_view, AP(5), sizeof rt.depth_view);
    memcpy(record(c, CMD_SET_RENDER_TARGETS, sizeof rt)->extra, &rt, sizeof rt);
}
PLAIN(nvnCommandBufferSetViewport, CMD_SET_VIEWPORTS)        /* (cmd, x, y, w, h) */
PLAIN(nvnCommandBufferSetScissor, CMD_SET_SCISSORS)          /* (cmd, x, y, w, h) */
PLAIN(nvnCommandBufferSetDepthRange, CMD_SET_DEPTH_RANGES)   /* (cmd, near, far): floats */
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }  /* (cmd, const float[4]) */
/* (cmd, faces, value); x[6] says which */
NVN(nvnCommandBufferSetStencilRef) { record(c, CMD_SET_STENCIL, 0)->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { record(c, CMD_SET_STENCIL, 0)->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { record(c, CMD_SET_STENCIL, 0)->x[6] = 2; }
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)                   /* (cmd, enable) */
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)        /* (cmd, factor, units, clamp) */
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)                   /* (cmd, mask) */
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)               /* (cmd, enable) */
NVN(nvnCommandBufferSetRenderEnableConditional) { record(c, CMD_SET_RENDER_ENABLE, 0)->x[6] = 1; }  /* (cmd, mode, counter address) */
PLAIN(nvnCommandBufferSetRasterizerDiscard, CMD_SET_RASTERIZER_DISCARD)     /* (cmd, discard) */
PLAIN(nvnCommandBufferSetConservativeRasterEnable, CMD_SET_CONSERVATIVE_RASTER)
PLAIN(nvnCommandBufferSetShaderScratchMemory, CMD_SET_SCRATCH_MEMORY)       /* (cmd, pool, offset, size) */

/* ---- work */
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }  /* (cmd, index, const float[4], mask) */
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)  /* (cmd, depth, depth mask, stencil, stencil mask) */
/* DrawArrays(cmd, primitive, first, count) is DrawArraysInstanced with one instance */
NVN(nvnCommandBufferDrawArrays) { CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0); r->x[3] = 0, r->x[4] = 1; }
PLAIN(nvnCommandBufferDrawArraysInstanced, CMD_DRAW_ARRAYS)  /* (cmd, primitive, first, count, base instance, instances) */
/* DrawElements(cmd, primitive, index type, count, index address) is DrawElementsInstanced
 * (..., base vertex, base instance, instances) with 0, 0, 1 */
NVN(nvnCommandBufferDrawElements) { CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0); r->x[4] = 0, r->x[5] = 0, r->x[6] = 1; }
PLAIN(nvnCommandBufferDrawElementsInstanced, CMD_DRAW_ELEMENTS)
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)        /* (cmd, primitive, indirect address) */
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)    /* (cmd, primitive, type, index address, indirect address) */
PLAIN(nvnCommandBufferDrawTransformFeedback, CMD_DRAW_TRANSFORM_FEEDBACK)  /* (cmd, primitive, feedback address) */
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)                       /* (cmd, x, y, z) */
PLAIN(nvnCommandBufferBeginTransformFeedback, CMD_BEGIN_TRANSFORM_FEEDBACK)  /* (cmd, state address) */
PLAIN(nvnCommandBufferEndTransformFeedback, CMD_END_TRANSFORM_FEEDBACK)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)                                /* (cmd, barrier bits) */
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)                   /* (cmd, counter, address) */
PLAIN(nvnCommandBufferResetCounter, CMD_RESET_COUNTER)                     /* (cmd, counter) */

/* ---- running a list (headless: walking it) */

static uint64_t draws;

void nvn_execute(uint64_t handle) {
    uint64_t at = handle;
    for (int n = 0;; n++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)at;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size & 7 || n > 10000000) {
            fprintf(stderr, "yw4: NVN: command list %#llx is broken at %#llx (op %u, size %u)\n",
                    (unsigned long long)handle, (unsigned long long)at, r->op, r->size);
            exit(3);
        }
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: at = r->x[0]; continue;
        case CMD_DRAW_ARRAYS: case CMD_DRAW_ELEMENTS: case CMD_DRAW_ARRAYS_INDIRECT: case CMD_DRAW_ELEMENTS_INDIRECT:
        case CMD_DRAW_TRANSFORM_FEEDBACK: case CMD_DISPATCH:
            __atomic_add_fetch(&draws, 1, __ATOMIC_RELAXED);
            break;
        case CMD_REPORT_COUNTER: {  /* (counter, address): NVNcounterData {u64 value, u64 GPU ticks at 614.4 MHz} */
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            out[0] = 0;
            out[1] = (uint64_t)((unsigned __int128)(uint64_t)host_now_ns() * 384 / 625);
            break;
        }
        }
        at += r->size;
    }
}

uint64_t nvn_take_draw_count(void) { return __atomic_exchange_n(&draws, 0, __ATOMIC_RELAXED); }

const NvnImpl nvn_cmdbuf_impls[] = {
    IMPL(nvnCommandBufferInitialize), IMPL(nvnCommandBufferFinalize), IMPL(nvnCommandBufferAddCommandMemory),
    IMPL(nvnCommandBufferAddControlMemory), IMPL(nvnCommandBufferBeginRecording), IMPL(nvnCommandBufferEndRecording),
    IMPL(nvnCommandBufferBindProgram), IMPL(nvnCommandBufferBindBlendState), IMPL(nvnCommandBufferBindChannelMaskState),
    IMPL(nvnCommandBufferBindColorState), IMPL(nvnCommandBufferBindDepthStencilState),
    IMPL(nvnCommandBufferBindMultisampleState), IMPL(nvnCommandBufferBindPolygonState),
    IMPL(nvnCommandBufferBindVertexAttribState), IMPL(nvnCommandBufferBindVertexStreamState),
    IMPL(nvnCommandBufferBindVertexBuffer), IMPL(nvnCommandBufferBindUniformBuffer),
    IMPL(nvnCommandBufferBindStorageBuffer), IMPL(nvnCommandBufferBindTexture), IMPL(nvnCommandBufferBindImage),
    IMPL(nvnCommandBufferBindTransformFeedbackBuffer), IMPL(nvnCommandBufferSetTexturePool),
    IMPL(nvnCommandBufferSetSamplerPool), IMPL(nvnCommandBufferSetRenderTargets), IMPL(nvnCommandBufferSetViewport),
    IMPL(nvnCommandBufferSetScissor), IMPL(nvnCommandBufferSetDepthRange), IMPL(nvnCommandBufferSetBlendColor),
    IMPL(nvnCommandBufferSetStencilRef), IMPL(nvnCommandBufferSetStencilMask), IMPL(nvnCommandBufferSetStencilValueMask),
    IMPL(nvnCommandBufferSetDepthClamp), IMPL(nvnCommandBufferSetPolygonOffsetClamp), IMPL(nvnCommandBufferSetSampleMask),
    IMPL(nvnCommandBufferSetRenderEnable), IMPL(nvnCommandBufferSetRenderEnableConditional),
    IMPL(nvnCommandBufferSetRasterizerDiscard), IMPL(nvnCommandBufferSetConservativeRasterEnable),
    IMPL(nvnCommandBufferSetShaderScratchMemory), IMPL(nvnCommandBufferClearColor),
    IMPL(nvnCommandBufferClearDepthStencil), IMPL(nvnCommandBufferDrawArrays), IMPL(nvnCommandBufferDrawArraysInstanced),
    IMPL(nvnCommandBufferDrawElements), IMPL(nvnCommandBufferDrawElementsInstanced),
    IMPL(nvnCommandBufferDrawArraysIndirect), IMPL(nvnCommandBufferDrawElementsIndirect),
    IMPL(nvnCommandBufferDrawTransformFeedback), IMPL(nvnCommandBufferDispatchCompute),
    IMPL(nvnCommandBufferBeginTransformFeedback), IMPL(nvnCommandBufferEndTransformFeedback),
    IMPL(nvnCommandBufferBarrier), IMPL(nvnCommandBufferReportCounter), IMPL(nvnCommandBufferResetCounter),
    {NULL, NULL},
};
