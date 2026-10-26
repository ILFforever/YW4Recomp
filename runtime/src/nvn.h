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
    void *gpu;               /* the draw path's import of it (gpu.c) */
} NvnPool;

#define POOL_CPU_NO_ACCESS 0x1u  /* NVN memory pool flag: only the GPU uses it (render targets) */

enum { TARGET_1D, TARGET_2D, TARGET_3D, TARGET_1D_ARRAY, TARGET_2D_ARRAY, TARGET_2D_MS, TARGET_2D_MS_ARRAY,
       TARGET_RECTANGLE, TARGET_CUBE, TARGET_CUBE_ARRAY, TARGET_BUFFER };
#define FLAG_LINEAR 0x10u

typedef struct {
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  /* guest address of the texels */
    TexLayout layout;
    uint32_t pool_flags;  /* of the memory pool it lives in */
    void *gpu;            /* the draw path's image (gpu.c) */
    int cpu_written;      /* WriteTexels changed its storage: upload before the GPU uses it */
    int registered;       /* times registered in a texture pool (diagnostic) */
    void *found;          /* the image gpu.c found for it last, valid while found_epoch is current */
    uint32_t found_epoch;
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

/* A texture pool entry. Like the hardware's descriptor, it keeps a copy of the texture as it
 * was registered (snap; tex points at it, NULL if the slot is empty): the game finalizes
 * and reuses texture objects that are still registered, so a pointer to the live object
 * would end up naming a different texture. */
typedef struct {
    NvnTexture *tex;
    NvnTexture snap;
    NvnTextureView view;
    int has_view;
    uint64_t guest;  /* the guest NVNtexture* it was registered from (diagnostic) */
} NvnTexEntry;

/* The data of a SetRenderTargets record (x[0] = colour count): copies of the textures as
 * they were when recorded (the objects may be finalized before the list runs) and of the
 * views. color[i] / depth point at the copies, or are NULL; whoever copies this struct
 * must point them at its own copies (gpu.c). */
typedef struct {
    NvnTexture *color[8];
    NvnTexture color_snap[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    NvnTexture *depth;
    NvnTexture depth_snap;
    NvnTextureView depth_view;
    uint8_t has_depth_view;
} RenderTargets;

/* Points rt's texture pointers at rt's own copies (after copying the struct). */
static inline void render_targets_fix(RenderTargets *rt) {
    for (int i = 0; i < 8; i++) rt->color[i] = rt->color[i] ? &rt->color_snap[i] : NULL;
    rt->depth = rt->depth ? &rt->depth_snap : NULL;
}

typedef struct {
    uint64_t addr;
    int count;
    void *entries;  /* NvnTexEntry[] or NvnSamplerDesc[] */
} NvnDescPool;

typedef struct {
    uint64_t code;     /* guest address of the stage's code (NVN header + Maxwell program) */
    uint64_t control;  /* guest address of its control section */
    uint64_t key;      /* the translated shader: FNV-1a 64 of its assembly (nvn_shader.c) */
} NvnShaderStage;

typedef struct {
    NvnShaderStage stages[6];  /* vertex, fragment, geometry, tess control, tess eval, compute */
    uint32_t stage_mask;
    void *gpu;                 /* the draw path's shaders and layouts (gpu_draw.c) */
} NvnProgram;

/* Device modes (nvnDeviceSetWindowOriginMode / SetDepthMode): 0 lower-left / near at -w
 * (OpenGL, the defaults), 1 upper-left / near at 0. */
extern uint32_t nvn_window_origin, nvn_depth_mode;

typedef struct {
    uint64_t textures[8];  /* guest NVNtexture* */
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

/* ---- command recording: records live in the game's command memory (nvn_cmdbuf.c) */

enum { TRACE_TEXTURES = 20 };  /* texture slots recorded per stage (the shadow fade programs read slot 18) */

typedef struct {
    uint64_t mem, size, used;  /* current command memory chunk (guest address, or an overflow chunk) */
    uint64_t base;             /* the command memory the game added last */
    int overflows;             /* overflow chunks used since (nvn_cmdbuf.c) */
    uint64_t start;            /* first record of the list being recorded */
    uint64_t callback, callback_data;
    int recording;
    uint64_t guest;            /* the guest NVNcommandBuffer* */
    const NvnProgram *program; /* bound last (YW4_TRACE_DRAW_CALLERS) */
    uint64_t program_guest;    /* ... its guest NVNprogram* */
    uint32_t target_w, target_h, target_colors, target_depth;  /* set last (YW4_TRACE_DRAW_CALLERS) */
    uint64_t trace_vb0, trace_ub[2][16];  /* bound last, vertex and fragment stage (YW4_TRACE_DRAWLIST) */
    uint64_t trace_vb[8], trace_streams[8];  /* vertex buffers 0-7 and stream states 0-7 bound last */
    uint64_t trace_tex[2][TRACE_TEXTURES];  /* texture handles bound last, vertex and fragment stage */
    const NvnDescPool *trace_texpool;       /* texture pool set last */
    const NvnDescPool *trace_samplerpool;   /* sampler pool set last */
    uint32_t bound_ub[2], bound_tex[2];     /* slots bound since the last draw (gbuffer.c clears them) */
    uint32_t trace_attribs[16], trace_nattribs;  /* vertex attribute state bound last */
    uint64_t depth_addr;    /* depth target set last: texels' address, first layer of its view */
    uint32_t depth_layer;
    uint64_t depth_guest;   /* ... and the guest NVNtexture* it was set with */
    uint8_t polygon[4];     /* polygon state bound last: cull, front face, mode, offset enables */
} NvnCmdBuf;

/* YW4_TRACE_DRAWLIST (trace_drawlist.c): on, and a recorded draw (index address or 0) */
int trace_drawlist_on(void);
void trace_drawlist_draw(const NvnCmdBuf *cb, uint64_t index_addr);
/* Our shadow pass (shadow.c), while learning from the game's draws: a draw was recorded with
 * this state bound, these index address and instancing */
typedef struct { int32_t base_vertex; uint32_t base_instance, instances; } ShadowDrawCall;
void shadow_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, const ShadowDrawCall *call);
extern int shadow_checking;

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
    CMD_NATIVE,  /* a pass of our own renderer (decision 0024): x[0] = NvnNativePass, data in extra */
    CMD_COUNT
};

/* A native pass, run by the executor where its CMD_NATIVE record sits in the command stream:
 * `vk_cb` is the VkCommandBuffer being recorded, `gpu_state` the executor's GpuState (the
 * targets the game bound, gpu_internal.h), `data` the record's copied data. */
typedef void (*NvnNativePass)(void *vk_cb, void *gpu_state, const void *data, uint32_t size);
/* Records a native pass into the NVN command buffer `cmd` (guest address); copies `size` bytes. */
void nvn_record_native(const CPUState *c, uint64_t cmd, NvnNativePass fn, const void *data, uint32_t size);

/* YW4_TRACE_TEXPOOL (nvn_objects.c): 0 off, 1 trace id in detail (UINT32_MAX: any), -1 only
 * summaries for it. */
int nvn_trace_texpool(uint32_t id);

/* Walks a submitted command list (headless: counts what it would run). */
void nvn_execute(uint64_t handle);
/* Draw and dispatch counts since the last call (for the frame log). */
uint64_t nvn_take_draw_count(void);
/* perf.c: one row of the performance log per presented frame (times from host_now_ns). */
void yw4_perf_frame(uint64_t frame, int64_t enter, int64_t shown, int64_t exit_, uint64_t draws);
/* Adds host time to this frame's row: PERF_SUBMIT (nvnQueueSubmitCommands) or PERF_SYNC (waits on the GPU). */
enum { PERF_SUBMIT, PERF_SYNC };
void yw4_perf_add(int what, int64_t ns);
void yw4_profile_thread(int role, uint64_t frame);  /* profile.c: YW4_PROFILE samples threads with that role */
extern _Thread_local int yw4_perf_presenter;  /* set on the thread that presents */

/* nvn_shader.c: shader keys (decision 0020) and YW4_NVN_STATS usage statistics */
uint64_t nvn_shader_key(uint64_t control);
int nvn_stats_enabled(void);
void nvn_stats_shader(uint64_t key, const char *stage);
void nvn_stats_pool(uint32_t flags, uint64_t size);
void nvn_stats_texels(uint64_t bytes);
void nvn_stats_draw(int kind);
void nvn_stats_program(const void *program);
void nvn_stats_frame(uint64_t frame);

#endif
