/* NVN on the host (decisions 0005, 0016, 0017): what nvn*.c share.
 *
 * Calls arrive in the AArch64 calling convention on the guest CPUState: integer and pointer
 * arguments in x0.., floats in s0.. (v[i].f[0]), the result in x0 (or s0). The first argument
 * is the NVN object. Its memory belongs to the game (sizes fixed by the NVN ABI), so:
 *   - real objects (memory pool, texture, sampler, program, window, command buffer, sync,
 *     texture/sampler pool) keep a pointer to our host object in their first 8 bytes;
 *   - builders and small state objects (blend, depth-stencil...) keep their fields directly
 *     in guest memory, at offsets inside their ABI size, because the game copies them by value.
 *
 * Builder offsets and state packing are adapted from LAS Recomp (runtime/nvn/nvn.h,
 * objects.c; MIT, see third_party/lasrecomp).
 */
#ifndef NVN_H
#define NVN_H

#include <stddef.h>
#include <stdint.h>

#include "hle.h"
#include "tegra.h"

typedef struct {
    const char *name;
    GuestFn fn;
} NvnImpl;

/* Each nvn_*.c file's functions, ended by {NULL, NULL}. */
extern const NvnImpl nvn_object_impls[];
extern const NvnImpl nvn_cmdbuf_impls[];

#define NVN(name) static void name(CPUState *c)
#define IMPL(name) {#name, name}

#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (c->v[(i)].f[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define G(T, base, off) (*(T *)(uintptr_t)((uint64_t)(base) + (off)))
#define HOST(T, guest) ((T *)(uintptr_t)G(uint64_t, guest, 0))
#define SET_HOST(guest, p) (G(uint64_t, guest, 0) = (uint64_t)(uintptr_t)(p))

/* Stops: something this NVN can't do yet. */
_Noreturn void nvn_fatal(const CPUState *c, const char *fmt, ...);
/* Calls a guest function (a game callback) on this thread; x0.. are already set. */
void nvn_guest_call(CPUState *c, uint64_t fn);

/* ---- host objects */

typedef struct {
    uint64_t storage, size;  /* guest address of the game's memory */
    uint32_t flags;
} NvnPool;

typedef struct {
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  /* guest address of the texels */
    TexLayout layout;
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
} NvnSamplerDesc;  /* also stored inside the guest NVNsamplerBuilder, at +8 */

/* Our layout of the guest NVNtextureView (0x28 bytes): which fields are set, and them. */
typedef struct {
    uint32_t setmask;
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    int has_view;
} NvnTexEntry;

typedef struct {
    uint64_t addr;
    int count;
    void *entries;  /* NvnTexEntry[] or NvnSamplerDesc[] */
} NvnDescPool;

typedef struct {
    uint64_t code;     /* guest address of the stage's code (NVN header + Maxwell program) */
    uint64_t control;  /* guest address of its control section */
} NvnShaderStage;

typedef struct {
    NvnShaderStage stages[6];  /* vertex, fragment, geometry, tess control, tess eval, compute */
    uint32_t stage_mask;
} NvnProgram;

typedef struct {
    uint64_t textures[8];  /* guest NVNtexture* */
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

/* ---- command recording: records live in the game's command memory (nvn_cmdbuf.c) */

typedef struct {
    uint64_t mem, size, used;  /* current command memory chunk (guest address) */
    uint64_t start;            /* first record of the list being recorded */
    uint64_t callback, callback_data;
    int recording;
    uint64_t guest;            /* the guest NVNcommandBuffer* */
} NvnCmdBuf;

/* One recorded call: header, the call's x1..x7 and s0..s3, then copied data. A command
 * handle is the guest address of a list's first record. */
typedef struct {
    uint32_t op, size;  /* size includes the header, a multiple of 8 */
    uint64_t x[7];
    float f[4];
    uint8_t extra[];
} CmdRec;

enum {
    CMD_END, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_BIND_TRANSFORM_FEEDBACK, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL,
    CMD_SET_RENDER_TARGETS, CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR,
    CMD_SET_STENCIL, CMD_SET_DEPTH_CLAMP, CMD_SET_POLYGON_OFFSET, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_SET_RASTERIZER_DISCARD, CMD_SET_CONSERVATIVE_RASTER, CMD_SET_SCRATCH_MEMORY,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DRAW_TRANSFORM_FEEDBACK, CMD_DISPATCH,
    CMD_BEGIN_TRANSFORM_FEEDBACK, CMD_END_TRANSFORM_FEEDBACK,
    CMD_BARRIER, CMD_REPORT_COUNTER, CMD_RESET_COUNTER,
    CMD_COUNT
};

/* Walks a submitted command list (headless: counts what it would run). */
void nvn_execute(uint64_t handle);
/* Draw and dispatch counts since the last call (for the frame log). */
uint64_t nvn_take_draw_count(void);

#endif
