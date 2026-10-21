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

/* ---- gpu.c */

void full_barrier(VkCommandBuffer cb);
uint64_t gpu_frames(void);  /* frames presented so far */

/* The imported pool buffer holding [addr, addr + size); 0 if no pool does. */
int gpu_buffer_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);

/* Per submission, freed when it completes: descriptor sets. */
VkDescriptorSet gpu_alloc_set(VkDescriptorSetLayout layout);

typedef struct GpuView GpuView;
typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t width, height, depth, layers, levels;
    GpuView *views;  /* cached image views */
    uint64_t uploaded_frame;  /* YW4_GPU_REUPLOAD: the frame it was last uploaded in */
    uint32_t upload_gen;      /* write generation (gpu.c) when it was last uploaded */
    uint32_t checked_gen;     /* ... when its pages were last found unchanged */
} GpuImage;

/* t's image: made on first use, uploaded from the game's memory unless the GPU owns it. */
GpuImage *image_of(VkCommandBuffer cb, NvnTexture *t);
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

typedef struct {
    RenderTargets rt;
    int ncolor;
    int rendering;  /* inside vkCmdBeginRendering for rt */
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
} GpuState;

void gpu_state_init(GpuState *s);
void gpu_end_rendering(VkCommandBuffer cb, GpuState *s);
/* A draw record: CMD_DRAW_ARRAYS or CMD_DRAW_ELEMENTS. */
void gpu_draw(VkCommandBuffer cb, GpuState *s, const CmdRec *r);

#endif
