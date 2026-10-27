/* Shared by gpu.c (submissions, memory, images) and gpu_draw.c (shaders, pipelines, draws):
 * the draw path of decision 0021. */
#ifndef YW4_GPU_INTERNAL_H
#define YW4_GPU_INTERNAL_H

#include "gpu.h"
#include "nvn.h"
#include "vk.h"

_Noreturn void gpu_fatal(const char *what, VkResult r);
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) gpu_fatal(#x, r_); } while (0)
_Noreturn void gpu_stop(const char *fmt, ...);  /* something the draw path can't do yet */

/* ---- gpu_ab.c: same-frame A/B of a pass (YW4_AB_AT) */
typedef struct GpuState GpuState;
int gpu_ab_marker(VkCommandBuffer cb, GpuState *s, const void *data, uint32_t size, uint64_t exec, uint64_t next,
                  uint64_t *resume);
void gpu_ab_targets(VkCommandBuffer cb, GpuState *s);
int gpu_ab_pending(void);
void gpu_ab_flush(void);

/* ---- gpu_passes.c: per-pass GPU timing (F5) */
void gpu_passes_begin(VkCommandBuffer cb, int slot);
void gpu_passes_end(VkCommandBuffer cb);
void gpu_passes_targets(VkCommandBuffer cb, const GpuState *s);
void gpu_passes_other(VkCommandBuffer cb, const char *what, uint64_t key);
void gpu_passes_draw(const NvnProgram *p, uint64_t vertices);  /* vertices (or indices) x instances */
void gpu_passes_collect(int slot);
void gpu_passes_note(int what);  /* 1 rendering ended, 2 a texture uploaded */

/* ---- gpu.c */

void full_barrier(VkCommandBuffer cb);
uint64_t gpu_frames(void);  /* frames presented so far */

/* The imported pool buffer holding [addr, addr + size); 0 if no pool does. */
int gpu_buffer_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
/* Geometry in video memory (YW4_VRAM_GEOMETRY): whether a vertex or index range needs copying,
 * the copy (outside rendering), and the range's copy if current, else the pool as gpu_buffer_at. */
int gpu_vram_geometry(void);
int gpu_geometry_needs_copy(uint64_t addr, uint64_t size);
void gpu_geometry_copy(VkCommandBuffer cb, uint64_t addr, uint64_t size);
int gpu_geometry_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);

/* Per submission, freed when it completes: descriptor sets. */
VkDescriptorSet gpu_alloc_set(VkDescriptorSetLayout layout);

typedef struct GpuView GpuView;

/* What identifies a texture's image: where its texels are and how they are laid out. Two
 * NVN texture objects (or copies of one) with the same key share one image, as they share
 * the memory on the Switch. */
typedef struct {
    uint64_t addr;
    uint32_t format, target, width, height, depth, layers, levels, block_height_log2, block_depth_log2, linear;
    /* made larger by scale_num / scale_den (0 / 0: the game's size): the shadow maps (YW4_SHADOW_RES / panel,
     * res / 1024) and the screen-sized targets (YW4_RES / panel, height / 900) */
    uint32_t scale_num, scale_den;
    /* the width also times wide_num / wide_den (0 / 0: not): the screen-sized targets in another picture
     * shape than 16:9 (widescreen.c) */
    uint32_t wide_num, wide_den;
} ImageKey;

/* v made num / den times larger, rounded to the nearest whole pixel */
static inline uint32_t gpu_scaled(uint32_t v, uint32_t num, uint32_t den) {
    return (uint32_t)(((uint64_t)v * num + den / 2) / den);
}

typedef struct GpuImage GpuImage;
struct GpuImage {
    GpuImage *next;           /* hash chain (gpu.c) */
    ImageKey key;
    uint64_t last_frame;      /* frame it was last used in, and the submission: for freeing */
    uint64_t last_seq;
    VkImage image;
    VkDeviceMemory mem;
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t width, height, depth, layers, levels;
    uint32_t scale_num, scale_den;  /* width and height are the texture's times this (key's; 1 / 1 if none) */
    uint32_t wide_num, wide_den;    /* ... and the width also times this (key's; 1 / 1 if none) */
    GpuView *views;  /* cached image views */
    uint64_t uploaded_frame;  /* YW4_GPU_REUPLOAD: the frame it was last uploaded in */
    uint32_t upload_gen;      /* write generation (gpu.c) when it was last uploaded */
    uint32_t checked_gen;     /* ... when its pages were last found unchanged */
};

/* Effects resolution: t (a distance-blur image) keeps the game's size ratio (gpu.c). */
void gpu_keep_game_ratio(const NvnTexture *t);
/* t's image: made on first use, uploaded from the game's memory unless the GPU owns it. */
GpuImage *image_of(VkCommandBuffer cb, NvnTexture *t);
/* t's image if it exists (no upload, no creation). */
GpuImage *image_find(const NvnTexture *t);
/* Would image_of record a copy for t (make it, or upload it)? Not allowed while rendering. */
int gpu_needs_transfer(const NvnTexture *t);
VkFormat gpu_format(uint32_t nvn_format);
int gpu_format_alpha_one(uint32_t nvn_format);  /* RGBX-style: alpha reads as 1 */

/* A view of t for rendering (one level, the layers of v) or sampling (as `type`, through
 * v's format, levels, layers and swizzle). */
VkImageView gpu_render_view(VkCommandBuffer cb, NvnTexture *t, const NvnTextureView *v, int has_view, uint32_t *w,
                            uint32_t *h);
VkImageView gpu_sample_view(VkCommandBuffer cb, NvnTexture *t, const NvnTextureView *v, int has_view,
                            VkImageViewType type);
VkImageView gpu_dummy_view(VkCommandBuffer cb, VkImageViewType type);
VkSampler gpu_sampler(const NvnSamplerDesc *d);

/* ---- gpu_draw.c: the state a command list builds up, and its draws */

typedef struct {
    uint64_t addr, size;
} GpuRange;

enum { STAGES = 6, UBOS = 16, SSBOS = 16, TEXTURES = 48 };

typedef struct GpuState {
    RenderTargets rt;
    int ncolor;
    int rendering;  /* inside vkCmdBeginRendering for rt */
    int targets_noted;  /* rt passed to gpu_shadow_note_targets since it was set */
    NvnProgram *program;
    uint8_t blend[8][8];
    uint8_t color[4];
    uint32_t channel_mask;
    uint8_t depth_stencil[8];
    uint8_t polygon[4];
    uint32_t attribs[16];
    uint32_t nattribs;
    uint32_t streams[16][2];  /* stride, divisor */
    uint32_t nstreams;
    GpuRange vbuf[16];
    GpuRange ubo[STAGES][UBOS], ssbo[STAGES][SSBOS];
    uint64_t tex[STAGES][TEXTURES], img[STAGES][TEXTURES];
    const NvnDescPool *tex_pool, *sampler_pool;
    int32_t viewport[4], scissor[4];
    int have_viewport, have_scissor;
    float depth_range[2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_value_mask[2];
    float polygon_offset[3];  /* factor, units, clamp */
    int rasterizer_discard, depth_clamp;
    int after_composite;  /* this command list drew the bloom composite: what follows (but the copy into the
                           * window's image) is the UI (gpu_draw.c) */
} GpuState;

void gpu_state_init(GpuState *s);
void gpu_end_rendering(VkCommandBuffer cb, GpuState *s);
/* For native passes (decision 0024): render into s's targets (their images made first,
 * rendering begun unless it is); the attachment formats and size; 0 if no target is set. */
int gpu_begin_targets(VkCommandBuffer cb, GpuState *s, VkFormat color[8], VkFormat *depth, uint32_t *w, uint32_t *h);
/* For native passes: the 2D texture a handle names in s's pools, sampled as a game draw's
 * binding would be. upload: make or upload its image now (outside rendering only), else fill
 * ii. 0: no such texture. */
int gpu_handle_texture(VkCommandBuffer cb, const GpuState *s, uint64_t handle, int upload, VkDescriptorImageInfo *ii);
/* Cull mode and front face from NVN polygon state bytes 0 and 1 (with the window origin's flip). */
void gpu_cull(VkCommandBuffer cb, uint8_t cull_mode, uint8_t front_face);
/* The pipelines' dynamic state from s (viewport, scissor, cull, depth, stencil, bias). */
void gpu_dynamic_state(VkCommandBuffer cb, const GpuState *s, VkFormat depth, uint32_t w, uint32_t h);
/* begin(): the command buffer has no dynamic state yet (gpu_dynamic_state sets only what changed). */
void gpu_dynamic_forget(void);
/* gpu_aa.c: anti-aliasing of the scene in s's first colour target, outside rendering (ends it). */
void gpu_aa_apply(VkCommandBuffer cb, GpuState *s);
/* TAA (gpu_aa.c): the shift in pixels for a 3D draw into a w x h target (0: none), and the draws' targets
 * (the G-buffer's motion target is noted, and copied when its targets change). */
int gpu_taa_jitter(uint32_t w, uint32_t h, float *jx, float *jy);  /* not for the UI: the caller checks */
void gpu_taa_note_targets(const GpuState *s);
/* One of the textures presented lately (the window's images; gpu.c). */
int gpu_is_presented(uint64_t addr);
void gpu_taa_targets_end(VkCommandBuffer cb);
/* gpu_shadow.c: the first draw into s's targets since they were set; depth-only ones are
 * remembered with the state the game draws with, so a shadow pass can draw into its layer after
 * the game has moved on to another target. */
void gpu_shadow_note_targets(const GpuState *s);
/* A draw record: CMD_DRAW_ARRAYS or CMD_DRAW_ELEMENTS. */
void gpu_draw(VkCommandBuffer cb, GpuState *s, const CmdRec *r);
/* A CMD_DISPATCH record: runs the bound program's compute shader, then a full barrier. */
void gpu_dispatch(VkCommandBuffer cb, GpuState *s, const CmdRec *r);
/* YW4_COMPUTE_TRACE=n: every n frames, the inputs and results of the last dispatch of each compute shader. */
void gpu_compute_trace(uint64_t frame);
/* YW4_CAPTURE_FS: colour targets copied after chosen draws (gpu_draw.c). Pending copies are written
 * out by gpu_capture_flush once the GPU has finished them. */
int gpu_capture_pending(void);
void gpu_capture_flush(void);
/* Builds the pipelines listed in gpu_draw.c on a background thread at start-up (YW4_PREBUILD=0: off). */
void gpu_prebuild_start(void);

#endif
