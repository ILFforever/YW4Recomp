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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "host.h"
#include "kernel.h"
#include "nvn.h"
#include "pixels.h"

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
    cb_of(A(0))->program = HOST(NvnProgram, A(1));
    cb_of(A(0))->program_guest = A(1);
}

/* YW4_TRACE_DRAW_CALLERS=1: each different guest call stack that records a draw, once,
 * with the bound program's shaders: where the engine's renderers draw from. */
static void trace_draw_caller(const CPUState *c) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_TRACE_DRAW_CALLERS") && *getenv("YW4_TRACE_DRAW_CALLERS") != '0';
    if (!on) return;
    enum { DEPTH = 10, SEEN = 4096 };
    static uint64_t seen[SEEN];
    static int nseen;
    uint64_t st[DEPTH];
    int n = yw4_call_stack(c, st, DEPTH);
    const NvnCmdBuf *cb = cb_of(A(0));
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < n; i++) h = (h ^ st[i]) * 0x100000001B3ull;
    h = (h ^ ((uint64_t)cb->target_w << 32 | cb->target_h)) * 0x100000001B3ull;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == h) return;
    if (nseen < SEEN) seen[nseen++] = h;
    const NvnProgram *p = cb->program;
    char where[48];
    fprintf(stderr, "yw4: draw caller: vs %016llx fs %016llx target %ux%u (%u colour, %s depth)\n",
            p ? (unsigned long long)p->stages[0].key : 0ull, p ? (unsigned long long)p->stages[1].key : 0ull,
            cb->target_w, cb->target_h, cb->target_colors, cb->target_depth ? "with" : "no");
    for (int i = 0; i < n; i++) fprintf(stderr, "       %s\n", yw4_describe(st[i] - 4, where));
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) {
    record_copy(c, CMD_BIND_POLYGON, 1, 4);
    if (A(1)) memcpy(cb_of(A(0))->polygon, AP(1), 4);
}
/* (cmd, count, const state*) */
NVN(nvnCommandBufferBindVertexAttribState) {
    record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4);
    NvnCmdBuf *cb = cb_of(A(0));
    cb->trace_nattribs = A(1) < 16 ? (uint32_t)A(1) : 16;
    if (A(2)) memcpy(cb->trace_attribs, AP(2), cb->trace_nattribs * 4);
}
NVN(nvnCommandBufferBindVertexStreamState) {  /* (cmd, count, states: stride, divisor) */
    record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8);
    NvnCmdBuf *cb = cb_of(A(0));
    if (A(2)) memcpy(cb->trace_streams, AP(2), (A(1) < 4 ? (uint32_t)A(1) : 4) * 8);
}
NVN(nvnCommandBufferBindVertexBuffer) {  /* (cmd, index, address, size) */
    record(c, CMD_BIND_VERTEX_BUFFERS, 0);
    if (A(1) == 0) cb_of(A(0))->trace_vb0 = A(2);
    if (A(1) < 4) cb_of(A(0))->trace_vb[A(1)] = A(2);
}
NVN(nvnCommandBufferBindUniformBuffer) {  /* (cmd, stage, index, address, size) */
    record(c, CMD_BIND_UNIFORM_BUFFERS, 0);
    if (A(1) < 2 && A(2) < 16) cb_of(A(0))->trace_ub[A(1)][A(2)] = A(3);
}
PLAIN(nvnCommandBufferBindStorageBuffer, CMD_BIND_STORAGE_BUFFERS)    /* (cmd, stage, index, address, size) */
NVN(nvnCommandBufferBindTexture) {  /* (cmd, stage, index, handle) */
    record(c, CMD_BIND_TEXTURES, 0);
    if (A(1) < 2 && A(2) < 4) cb_of(A(0))->trace_tex[A(1)][A(2)] = A(3);
    if (nvn_trace_texpool((uint32_t)(A(3) & 0xFFFFF)) > 0) {  /* once per (stage, index, texture id, fragment shader) */
        enum { SEEN = 1024 };
        static uint64_t seen[SEEN];
        static int nseen;
        const NvnProgram *p = cb_of(A(0))->program;
        uint64_t k = (A(1) << 40 | A(2) << 20 | (A(3) & 0xFFFFF)) ^ (p ? p->stages[1].key : 0);
        int i = 0;
        while (i < nseen && seen[i] != k) i++;
        if (i == nseen && nseen < SEEN) {
            seen[nseen++] = k;
            fprintf(stderr, "yw4: texpool: BindTexture(stage %u, %u, %#" PRIx64 ") in cmdbuf %#" PRIx64 ", program fs %016llx\n",
                    (unsigned)A(1), (unsigned)A(2), A(3), A(0), p ? (unsigned long long)p->stages[1].key : 0ull);
        }
    }
}
PLAIN(nvnCommandBufferBindImage, CMD_BIND_IMAGES)                     /* (cmd, stage, index, handle) */
PLAIN(nvnCommandBufferBindTransformFeedbackBuffer, CMD_BIND_TRANSFORM_FEEDBACK)  /* (cmd, index, address, size) */
static void trace_set_pool(const CPUState *c, const char *kind) {
    static long n;
    if (nvn_trace_texpool(UINT32_MAX) > 0 && n++ < 100)
        fprintf(stderr, "yw4: texpool: record Set%sPool(%p, guest %#" PRIx64 ") in cmdbuf %#" PRIx64 "\n", kind,
                (void *)HOST(NvnDescPool, A(1)), A(1), A(0));
}
NVN(nvnCommandBufferSetTexturePool) {
    record(c, CMD_SET_TEXTURE_POOL, 0)->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, A(1));
    trace_set_pool(c, "Texture");
}
NVN(nvnCommandBufferSetSamplerPool) {
    record(c, CMD_SET_SAMPLER_POOL, 0)->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, A(1));
    trace_set_pool(c, "Sampler");
}

/* SetRenderTargets(cmd, count, const NVNtexture *const *colours, const NVNtextureView *const *views,
 * const NVNtexture *depth, const NVNtextureView *depth view): host textures and copies of the views */
NVN(nvnCommandBufferSetRenderTargets) {
    int n = (int)A(1);
    if (n < 0 || n > 8) nvn_fatal(c, "%d render targets", n);
    RenderTargets rt = {0};
    const uint64_t *colors = AP(2), *views = AP(3);
    for (int i = 0; i < n; i++) {
        if (colors && colors[i]) {  /* copies: the texture objects may be gone when the list runs */
            rt.color_snap[i] = *HOST(NvnTexture, colors[i]);
            rt.color_snap[i].gpu = NULL;
            rt.color[i] = &rt.color_snap[i];
        }
        if (views && views[i]) rt.has_view[i] = 1, memcpy(&rt.views[i], (const void *)(uintptr_t)views[i], sizeof rt.views[i]);
    }
    if (A(4)) {
        rt.depth_snap = *HOST(NvnTexture, A(4));
        rt.depth_snap.gpu = NULL;
        rt.depth = &rt.depth_snap;
    }
    if (A(5)) rt.has_depth_view = 1, memcpy(&rt.depth_view, AP(5), sizeof rt.depth_view);
    memcpy(record(c, CMD_SET_RENDER_TARGETS, sizeof rt)->extra, &rt, sizeof rt);
    NvnCmdBuf *cb = cb_of(A(0));
    cb->depth_addr = rt.depth ? rt.depth->addr : 0;
    cb->depth_guest = A(4);
    cb->depth_layer = rt.has_depth_view && rt.depth_view.setmask & 2 ? rt.depth_view.min_layer : 0;
    const NvnTexture *first = rt.color[0] ? rt.color[0] : rt.depth;
    cb->target_w = first ? first->width : 0, cb->target_h = first ? first->height : 0;
    cb->target_colors = 0;
    for (int i = 0; i < n; i++) cb->target_colors += rt.color[i] != NULL;
    cb->target_depth = rt.depth != NULL;
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
NVN(nvnCommandBufferClearDepthStencil) {  /* (cmd, depth, depth mask, stencil, stencil mask) */
    record(c, CMD_CLEAR_DEPTH_STENCIL, 0);
}
/* DrawArrays(cmd, primitive, first, count) is DrawArraysInstanced with one instance */
static void trace_draw(CPUState *c, uint64_t index_addr, const ShadowDrawCall *call) {
    trace_draw_caller(c);
    if (trace_drawlist_on()) trace_drawlist_draw(cb_of(A(0)), index_addr);
    if (shadow_checking) shadow_check_draw(cb_of(A(0)), index_addr, call);
}
static const ShadowDrawCall one = {0, 0, 1};
NVN(nvnCommandBufferDrawArrays) { trace_draw(c, 0, &one); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0); r->x[3] = 0, r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) {  /* (cmd, primitive, first, count, base instance, instances) */
    trace_draw(c, 0, &(ShadowDrawCall){0, (uint32_t)A(4), (uint32_t)A(5)});
    record(c, CMD_DRAW_ARRAYS, 0);
}
/* DrawElements(cmd, primitive, index type, count, index address) is DrawElementsInstanced
 * (..., base vertex, base instance, instances) with 0, 0, 1 */
NVN(nvnCommandBufferDrawElements) { trace_draw(c, A(4), &one); CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0); r->x[4] = 0, r->x[5] = 0, r->x[6] = 1; }
NVN(nvnCommandBufferDrawElementsInstanced) {
    trace_draw(c, A(4), &(ShadowDrawCall){(int32_t)A(5), (uint32_t)A(6), (uint32_t)A(7)});
    record(c, CMD_DRAW_ELEMENTS, 0);
}
void nvn_record_native(const CPUState *c, uint64_t cmd, NvnNativePass fn, const void *data, uint32_t size) {
    CmdRec *r = rec(c, cb_of(cmd), CMD_NATIVE, size);
    r->x[0] = (uint64_t)(uintptr_t)fn;
    r->x[1] = size;
    if (size) memcpy(r->extra, data, size);
}
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

/* ---- what runs on the CPU for now (decision 0019): colour clears, written into the
 * texture's storage in the game's memory, so the presented frame shows them */

static NvnTexture *render_targets[8];  /* bound by the last SetRenderTargets */

/* Level 0 of layer 0 of t, every texel set to the clear colour in the channels of `mask`. */
static void clear_color(NvnTexture *t, const float color[4], uint32_t mask) {
    static uint32_t unknown_logged[8];
    uint8_t texel[16];
    int n = pixel_encode(t->format, color, texel);
    if (!n || n != t->layout.fmt->bpb) {
        for (int i = 0; i < 8; i++)
            if (unknown_logged[i] == t->format) return;
            else if (!unknown_logged[i]) { unknown_logged[i] = t->format; break; }
        fprintf(stderr, "yw4: NVN: clear of format %#x not done on the CPU yet\n", t->format);
        return;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)t->addr;
    uint64_t level0 = t->layout.levels > 1 ? tegra_level(&t->layout, 1).offset : tegra_layer_size(&t->layout);
    if ((mask & 0xF) == 0xF && 16 % n == 0) {  /* every texel equal: the layout doesn't matter */
        for (uint64_t i = 0; i < level0; i += (uint64_t)n) memcpy(base + i, texel, (size_t)n);
        return;
    }
    fprintf(stderr, "yw4: NVN: clear with channel mask %#x not done on the CPU yet\n", mask);
}

void nvn_execute(uint64_t handle) {
    int gpu = gpu_ready();
    if (gpu) gpu_execute(handle);  /* the draw path (0021); the walk below then only counts */
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
            nvn_stats_draw(r->op == CMD_DRAW_ARRAYS ? 0 : r->op == CMD_DRAW_ELEMENTS ? 1 : r->op == CMD_DISPATCH ? 3 : 2);
            break;
        case CMD_BIND_PROGRAM:
            nvn_stats_program((const void *)(uintptr_t)r->x[0]);
            break;
        case CMD_SET_RENDER_TARGETS: {
            static RenderTargets rt;  /* the record's copies; its pointers must be re-pointed */
            memcpy(&rt, r->extra, sizeof rt);
            render_targets_fix(&rt);
            for (int i = 0; i < 8; i++) render_targets[i] = i < (int)r->x[0] ? rt.color[i] : NULL;
            break;
        }
        case CMD_CLEAR_COLOR: {  /* (index, colour, mask) */
            NvnTexture *t = r->x[0] < 8 ? render_targets[r->x[0]] : NULL;
            if (!gpu && t && t->addr) {
                float color[4];
                memcpy(color, r->extra, 16);
                clear_color(t, color, (uint32_t)r->x[2]);
            }
            break;
        }
        case CMD_REPORT_COUNTER: {  /* (counter, address): NVNcounterData {u64 value, u64 GPU ticks at 614.4 MHz} */
            if (gpu) break;
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
