/* Our shadow-caster pass on Vulkan (decision 0024): draws a ShadowPass (shadow.h) into the
 * cascade layer the game's own commands left bound, with our shader
 * runtime/shaders/shadow_static.vert, shadow_cutout.vert/.frag for cut-out casters or
 * shadow_skinned.vert for skinned ones (compiled
 * by recomp/build.py into work/build/shaders, YW4_NATIVE_SHADER_DIR to move it). Depth only;
 * the pipeline state the game's shadow draws use (depth test and write, bias) comes from the
 * executor's state; culling per draw, as the game drew that submesh. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
#include "gpu_internal.h"
#include "host.h"
#include "lamp_shadow.h"
#include "shadow.h"

/* What our pass left out, for the log (shadow.c's report every 600 frames, the run's summary, a GPU crash): passes
 * (the whole layer's draws: its shadow map no longer bound, or not found again) and single draws, by why. Since the
 * last report, and since the start. The first few of each kind are also logged when they happen, with the frame, so
 * a player's "the shadows flickered at ..." can be matched to one (owner, 2026-11-16: enough in the log for a report). */
static long passes_skipped, draws_skipped[SHADOW_SKIP_N];
static long passes_skipped_total, draws_skipped_total[SHADOW_SKIP_N];
const char *const shadow_skip_name[SHADOW_SKIP_N] = {"other", "vertices not in GPU memory",
                                                     "indices not in GPU memory", "cut-out texture",
                                                     "skin layout", "dither texture", "wind textures"};

void gpu_shadow_counts(long *p, long d[SHADOW_SKIP_N]) {
    *p = __atomic_exchange_n(&passes_skipped, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < SHADOW_SKIP_N; i++) d[i] = __atomic_exchange_n(&draws_skipped[i], 0, __ATOMIC_RELAXED);
}

void gpu_shadow_totals(long *p, long d[SHADOW_SKIP_N]) {
    *p = __atomic_load_n(&passes_skipped_total, __ATOMIC_RELAXED);
    for (int i = 0; i < SHADOW_SKIP_N; i++) d[i] = __atomic_load_n(&draws_skipped_total[i], __ATOMIC_RELAXED);
}

/* a pass left out: counted, and the first 5 logged */
static void pass_skipped(const ShadowPass *p, const char *why) {
    __atomic_add_fetch(&passes_skipped, 1, __ATOMIC_RELAXED);
    long n = __atomic_add_fetch(&passes_skipped_total, 1, __ATOMIC_RELAXED);
    if (n <= 5)
        fprintf(stderr, "yw4: shadows: our pass for shadow map %#" PRIx64 " layer %u left out at GPU frame %" PRIu64
                " (%u draws): %s%s\n", p->depth_addr, p->depth_layer, gpu_frames(), p->n, why,
                n == 5 ? " (the last one logged; the count goes on in the shadows lines)" : "");
}

/* a draw left out: counted, and the first 3 of each reason logged */
static void draw_skipped(const ShadowDraw *d, int why) {
    __atomic_add_fetch(&draws_skipped[why], 1, __ATOMIC_RELAXED);
    long n = __atomic_add_fetch(&draws_skipped_total[why], 1, __ATOMIC_RELAXED);
    if (n <= 3)
        fprintf(stderr, "yw4: shadows: one of our draws left out at GPU frame %" PRIu64 ": %s (vertices %#" PRIx64
                " x %u, stride %u, indices %#" PRIx64 " x %u, texture %#" PRIx64 ")%s\n", gpu_frames(),
                shadow_skip_name[why], d->vertices, d->vertex_count, d->stride, d->indices, d->index_count, d->texture,
                n == 3 ? " (the last one logged for this reason)" : "");
}

static VkShaderModule module(const char *name) {
    const char *dir = getenv("YW4_NATIVE_SHADER_DIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.spv", dir && *dir ? dir : "work/build/shaders", name);
    FILE *f = fopen(path, "rb");
    if (!f) gpu_stop("no native shader %s (python -m recomp.build compiles runtime/shaders)", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n);
    if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) gpu_stop("cannot read %s", path);
    fclose(f);
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n,
                                    .pCode = code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(vk.device, &mci, NULL, &m));
    free(code);
    return m;
}

/* Kinds of caster, each its own shaders and pipeline layout. The instanced ones draw all of a
 * submesh's copies at once, their matrices in the copies ring. */
enum {
    KIND_STATIC, KIND_CUTOUT, KIND_SKINNED, KIND_SKINNED4, KIND_INST, KIND_INST_CUTOUT, KIND_INST_FADE,
    KIND_INST_FADE_CUTOUT, KIND_WIND, KINDS
};
/* Push constants: the matrix (64 bytes), for cut-out casters also the threshold; instanced ones
 * only the threshold (their matrices come per copy); wind-swayed ones projection x view, the
 * threshold and their header's index in the copies ring. */
static const uint32_t push_size[KINDS] = {64, 68, 64, 64, 4, 4, 4, 4, 72};
static int kind_instanced(int kind) { return kind >= KIND_INST; }
static int kind_skinned(int kind) { return kind == KIND_SKINNED || kind == KIND_SKINNED4; }
/* kinds with three vertex bindings (the same vertices, at the second and third attributes' offsets) */
static int kind_three(int kind) { return kind == KIND_WIND || kind == KIND_SKINNED4; }

/* Instanced casters' copies: a ring in host-visible memory, as the palettes'. The render thread
 * that gathers a pass writes each draw's copies in (gpu_shadow_copies); the draw's first instance
 * is the first one's index, read by shadow_instanced.vert through one storage-buffer descriptor.
 * 16 MiB is 200,000 copies: hundreds of frames. */
enum { COPIES_RING = 16 << 20 };
static struct {
    VkBuffer buf;
    ShadowCopy *map;
    uint32_t next, size;  /* in copies */
    VkDescriptorSetLayout layout;
    VkDescriptorSet set;
} cps;
static HostLock cps_lock = HOST_LOCK_INIT;

/* cps_lock held. */
static VkDescriptorSetLayout copies_layout_locked(void) {
    if (!cps.layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 1, .pBindings = &b};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &cps.layout));
    }
    return cps.layout;
}

/* Called by the GPU worker too, while guest render threads may be making it: same lock. */
static VkDescriptorSetLayout copies_set_layout(void) {
    host_lock(&cps_lock);
    VkDescriptorSetLayout l = copies_layout_locked();
    host_unlock(&cps_lock);
    return l;
}

/* The ring, its memory and its one descriptor set (cps_lock held). */
static void copies_ring(void) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = COPIES_RING,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &cps.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, cps.buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(vk.device, cps.buf, mem, 0));
    CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&cps.map));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(vk.device, &pci, NULL, &pool));
    VkDescriptorSetLayout l = copies_layout_locked();
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &l};
    CHECK(vkAllocateDescriptorSets(vk.device, &ai, &cps.set));
    VkDescriptorBufferInfo bi = {cps.buf, 0, COPIES_RING};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = cps.set, .dstBinding = 0,
                              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .pBufferInfo = &bi};
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
    cps.size = COPIES_RING / sizeof(ShadowCopy);
}

ShadowCopy *gpu_shadow_copies(uint32_t n, uint32_t *first) {
    host_lock(&cps_lock);
    if (!cps.buf) copies_ring();
    if (!n || n > cps.size) {
        host_unlock(&cps_lock);
        return NULL;
    }
    uint32_t at = cps.next + n > cps.size ? 0 : cps.next;
    cps.next = at + n;
    host_unlock(&cps_lock);
    *first = at;
    return cps.map + at;
}

/* Skinned casters' bone palettes: a ring in host-visible memory. The render thread that gathers
 * a pass copies each palette in (gpu_shadow_palette), the pass's draws read it through one
 * descriptor with a dynamic offset. 16 MiB is about 100 frames of palettes: reused long after
 * the GPU has read them. */
enum { PALETTE_RING = 16 << 20, PALETTE_MAX = 3 * 256 * 16 };  /* the shader's array: 256 bones */
static struct {
    VkBuffer buf;
    uint8_t *map;
    uint64_t next;
    VkDescriptorSetLayout layout;
    VkDescriptorSet set;
} pal;
static HostLock pal_lock = HOST_LOCK_INIT;

/* pal_lock held. */
static VkDescriptorSetLayout palette_layout_locked(void) {
    if (!pal.layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 1, .pBindings = &b};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &pal.layout));
    }
    return pal.layout;
}

/* Called by the GPU worker too, while guest render threads may be making it: same lock. */
static VkDescriptorSetLayout palette_set_layout(void) {
    host_lock(&pal_lock);
    VkDescriptorSetLayout l = palette_layout_locked();
    host_unlock(&pal_lock);
    return l;
}

/* The ring, its memory and its one descriptor set (pal_lock held). */
static void palette_ring(void) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PALETTE_RING + PALETTE_MAX,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &pal.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, pal.buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(vk.device, pal.buf, mem, 0));
    CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&pal.map));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(vk.device, &pci, NULL, &pool));
    VkDescriptorSetLayout l = palette_layout_locked();
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &l};
    CHECK(vkAllocateDescriptorSets(vk.device, &ai, &pal.set));
    VkDescriptorBufferInfo bi = {pal.buf, 0, PALETTE_MAX};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = pal.set, .dstBinding = 0,
                              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                              .pBufferInfo = &bi};
    vkUpdateDescriptorSets(vk.device, 1, &w, 0, NULL);
}

int gpu_shadow_palette(const void *src, uint32_t size, uint32_t *offset) {
    if (!size || size > PALETTE_MAX) return 0;
    VkDeviceSize align = vk.props.limits.minUniformBufferOffsetAlignment;
    host_lock(&pal_lock);
    if (!pal.buf) palette_ring();
    uint64_t at = (pal.next + align - 1) / align * align;
    if (at + size > PALETTE_RING) at = 0;
    pal.next = at + size;
    host_unlock(&pal_lock);
    memcpy(pal.map + at, src, size);
    *offset = (uint32_t)at;
    return 1;
}

/* Cut-out and fading casters: their textures at set 0, bindings 0 .. n-1 (cut-out: the texture;
 * fading: the dither; fading cut-out: the texture, then the dither). n = 0: an empty set 0 for
 * the instanced kinds without textures (their copies are set 1). */
static VkDescriptorSetLayout textures_set_layout(uint32_t n) {
    static VkDescriptorSetLayout l[3];
    if (!l[n]) {
        VkDescriptorSetLayoutBinding b[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = n, .pBindings = b};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &l[n]));
    }
    return l[n];
}
/* Wind-swayed casters: the leaf texture (fragment stage), then the noise and the wind map (vertex). */
static VkDescriptorSetLayout wind_set_layout(void) {
    static VkDescriptorSetLayout l;
    if (!l) {
        VkDescriptorSetLayoutBinding b[3] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 3, .pBindings = b};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &l));
    }
    return l;
}
static uint32_t kind_textures(int kind) {
    return kind == KIND_WIND ? 3 : kind == KIND_INST_FADE_CUTOUT ? 2 : kind == KIND_CUTOUT || kind == KIND_INST_CUTOUT || kind == KIND_INST_FADE;
}

static VkPipelineLayout layout(int kind) {
    static VkPipelineLayout l[KINDS];
    if (!l[kind]) {
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind]};
        VkDescriptorSetLayout set[2] = {kind_skinned(kind) ? palette_set_layout()
                                        : kind == KIND_WIND  ? wind_set_layout()
                                                             : textures_set_layout(kind_textures(kind)),
                                        copies_set_layout()};
        uint32_t nsets = kind_instanced(kind) ? 2 : kind == KIND_STATIC ? 0 : 1;
        VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = nsets, .pSetLayouts = set,
                                          .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        CHECK(vkCreatePipelineLayout(vk.device, &lci, NULL, &l[kind]));
    }
    return l[kind];
}

/* One pipeline per depth format and kind: triangle lists, the position (R32G32B32 at offset 0,
 * the stride set per draw), no fragment stage. Cut-out casters also read their texture
 * coordinates (`uv`, a Vulkan format) through a second binding of the same vertices and have a
 * fragment stage; skinned ones read weights (R32G32B32A32_UINT at 28) and bone indices
 * (R16G16B16A16_UINT at 44) from the same binding. Instanced ones take their matrices per copy;
 * fading ones have the dither in their fragment stage. */
static int kind_has_uv(int kind) {
    return kind == KIND_CUTOUT || kind == KIND_INST_CUTOUT || kind == KIND_INST_FADE_CUTOUT || kind == KIND_WIND;
}
static VkPipeline pipeline(VkFormat depth, int kind, VkFormat uv, VkFormat wind) {
    enum { MAX = 48 };
    static struct { VkFormat depth, uv, wind; int kind; VkPipeline pipe; } pipes[MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (pipes[i].depth == depth && pipes[i].kind == kind && pipes[i].uv == uv && pipes[i].wind == wind)
            return pipes[i].pipe;
    if (n == MAX) gpu_stop("shadow pass: too many pipelines");
    static VkShaderModule vs[KINDS], fs[KINDS];
    static const char *const vs_name[KINDS] = {"shadow_static.vert",          "shadow_cutout.vert",
                                               "shadow_skinned.vert",         "shadow_skinned4.vert",
                                               "shadow_instanced.vert",
                                               "shadow_instanced_cutout.vert", "shadow_instanced.vert",
                                               "shadow_instanced_cutout.vert", "shadow_wind.vert"};
    static const char *const fs_name[KINDS] = {NULL, "shadow_cutout.frag", NULL, NULL, NULL, "shadow_cutout.frag",
                                               "shadow_fade.frag", "shadow_fade_cutout.frag", "shadow_cutout.frag"};
    if (!vs[kind]) vs[kind] = module(vs_name[kind]);
    if (fs_name[kind] && !fs[kind]) fs[kind] = module(fs_name[kind]);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs[kind], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs[kind], .pName = "main"}};
    VkVertexInputBindingDescription bind[3] = {{0, 12, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 12, VK_VERTEX_INPUT_RATE_VERTEX},
                                               {2, 12, VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attr[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 1, uv, 0}, {2, 2, wind, 0}};
    if (kind == KIND_SKINNED) {
        attr[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_UINT, 28};
        attr[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R16G16B16A16_UINT, 44};
    }
    uint32_t nattr = kind == KIND_SKINNED || kind_three(kind) ? 3 : kind_has_uv(kind) ? 2 : 1;
    for (uint32_t i = 0; i < nattr; i++) gpu_vertex_format_ok(attr[i].format, vs_name[kind], i);
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = kind_three(kind) ? 3 : kind_has_uv(kind) ? 2 : 1,
                                               .pVertexBindingDescriptions = bind,
                                               .vertexAttributeDescriptionCount = nattr,
                                               .pVertexAttributeDescriptions = attr};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    /* the same dynamic state as the game's pipelines (gpu_dynamic_state), plus the stride */
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    int stencil = depth == VK_FORMAT_D24_UNORM_S8_UINT || depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .depthAttachmentFormat = depth,
                                        .stencilAttachmentFormat = stencil ? depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = fs_name[kind] ? 2 : 1,
        .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs,
        .pDynamicState = &dy, .layout = layout(kind)};
    CHECK(vkCreateGraphicsPipelines(vk.device, gpu_pcache(), 1, &pci, NULL, &pipes[n].pipe));
    pipes[n].depth = depth, pipes[n].kind = kind, pipes[n].uv = uv, pipes[n].wind = wind;
    return pipes[n++].pipe;
}

static uint32_t depth_layer_of(const RenderTargets *rt) {
    return rt->has_depth_view && rt->depth_view.setmask & 2 ? rt->depth_view.min_layer : 0;
}

/* A depth-only target (a shadow map layer) and the state the game's draws into it ended with:
 * what a pass drawn after the game has moved on needs (the next target's viewport can differ). */
typedef struct {
    RenderTargets rt;
    int32_t viewport[4], scissor[4];
    int have_viewport, have_scissor;
    float depth_range[2], polygon_offset[3];
    uint8_t depth_stencil[8], polygon[4];
} TargetState;

static void target_state_get(TargetState *t, const GpuState *s) {
    memcpy(&t->rt, &s->rt, sizeof t->rt);
    render_targets_fix(&t->rt);
    memcpy(t->viewport, s->viewport, sizeof t->viewport), memcpy(t->scissor, s->scissor, sizeof t->scissor);
    t->have_viewport = s->have_viewport, t->have_scissor = s->have_scissor;
    memcpy(t->depth_range, s->depth_range, sizeof t->depth_range);
    memcpy(t->polygon_offset, s->polygon_offset, sizeof t->polygon_offset);
    memcpy(t->depth_stencil, s->depth_stencil, sizeof t->depth_stencil), memcpy(t->polygon, s->polygon, sizeof t->polygon);
}

static void target_state_set(GpuState *s, const TargetState *t) {
    memcpy(&s->rt, &t->rt, sizeof s->rt);
    render_targets_fix(&s->rt);
    memcpy(s->viewport, t->viewport, sizeof s->viewport), memcpy(s->scissor, t->scissor, sizeof s->scissor);
    s->have_viewport = t->have_viewport, s->have_scissor = t->have_scissor;
    memcpy(s->depth_range, t->depth_range, sizeof s->depth_range);
    memcpy(s->polygon_offset, t->polygon_offset, sizeof s->polygon_offset);
    memcpy(s->depth_stencil, t->depth_stencil, sizeof s->depth_stencil), memcpy(s->polygon, t->polygon, sizeof s->polygon);
}

/* The depth-only targets left last, by texels' address and layer. */
enum { NOTED = 8 };
static HostLock noted_lock = HOST_LOCK_INIT;
static TargetState noted[NOTED];
static int nnoted, noted_next;

void gpu_shadow_note_targets(const GpuState *s) {
    if (s->ncolor || !s->rt.depth) return;
    uint64_t addr = s->rt.depth->addr;
    uint32_t layer = depth_layer_of(&s->rt);
    host_lock(&noted_lock);
    int i = 0;
    while (i < nnoted && (noted[i].rt.depth->addr != addr || depth_layer_of(&noted[i].rt) != layer)) i++;
    if (i == nnoted) {
        if (nnoted < NOTED) i = nnoted++;
        else i = noted_next, noted_next = (noted_next + 1) % NOTED;
    }
    target_state_get(&noted[i], s);
    host_unlock(&noted_lock);
}

static void draw_pass(VkCommandBuffer cb, GpuState *s, const ShadowPass *p) {
    /* cut-out casters' textures, the dither and the wind textures: images made and uploaded before
     * rendering, as for the game's draws */
    uint64_t uploaded[3] = {0};  /* dither, wind noise, wind map: one each in a pass */
    for (uint32_t i = 0; i < p->n; i++) {
        const ShadowDraw *d = &p->d[i];
        uint64_t once[3] = {d->dither, d->wind_tex[0], d->wind_tex[1]};
        if (d->texture) {
            if (s->rendering) gpu_end_rendering(cb, s);
            gpu_handle_texture(cb, s, d->texture, 1, NULL);
        }
        for (int t = 0; t < 3; t++)
            if (once[t] && once[t] != uploaded[t]) {
                if (s->rendering) gpu_end_rendering(cb, s);
                gpu_handle_texture(cb, s, once[t], 1, NULL);
                uploaded[t] = once[t];
            }
    }
    /* the casters' geometry in video memory (gpu.c, YW4_VRAM_GEOMETRY): copies first, outside rendering */
    int copies = 0;
    for (uint32_t i = 0; i < p->n && !copies; i++) {
        const ShadowDraw *d = &p->d[i];
        copies = gpu_geometry_needs_copy(d->vertices, (uint64_t)d->vertex_count * d->stride) ||
                 (d->index_count && gpu_geometry_needs_copy(d->indices, (uint64_t)d->index_count * (d->index_type == 2 ? 4 : 2)));
    }
    if (copies) {
        gpu_end_rendering(cb, s);
        for (uint32_t i = 0; i < p->n; i++) {
            const ShadowDraw *d = &p->d[i];
            gpu_geometry_copy(cb, d->vertices, (uint64_t)d->vertex_count * d->stride);
            if (d->index_count) gpu_geometry_copy(cb, d->indices, (uint64_t)d->index_count * (d->index_type == 2 ? 4 : 2));
        }
        full_barrier(cb);
    }
    VkFormat color[8], depth;
    uint32_t w, h;
    if (!gpu_begin_targets(cb, s, color, &depth, &w, &h)) {
        pass_skipped(p, "its shadow map could not be bound");
        return;
    }
    VkPipeline bound = VK_NULL_HANDLE;
    int cull = -1, front = -1;
    long skipped = 0;
    long skip_why[7] = {0};  /* 1 vertices, 2 indices, 3 cut-out texture, 4 skin layout, 5 dither, 6 wind textures */
    /* YW4_AB_SELFTEST=1: leave out every second draw of ours, a fault the same-frame A/B must report (checks the tool) */
    static int selftest = -1;
    if (selftest < 0) selftest = getenv("YW4_AB_SELFTEST") && atoi(getenv("YW4_AB_SELFTEST")) > 0;
    static int no_alpha = -1;  /* YW4_SHADOW_NO_ALPHA=1 (tests): cut-out casters drawn solid, no alpha test */
    if (no_alpha < 0) no_alpha = getenv("YW4_SHADOW_NO_ALPHA") && atoi(getenv("YW4_SHADOW_NO_ALPHA")) > 0;
    ShadowDraw solid;
    for (uint32_t i = 0; i < p->n; i += 1 + (uint32_t)selftest) {
        const ShadowDraw *d = &p->d[i];
        if (no_alpha && d->texture) solid = *d, solid.threshold = -1.0f, d = &solid;
        VkBuffer vb[3], ib;
        VkDeviceSize voff[3], ioff, stride[3] = {d->stride, d->stride, d->stride};
        uint32_t isize = d->index_type == 2 ? 4 : 2;
        VkDescriptorImageInfo ii[4];  /* the cut-out texture, the dither, the wind noise, the wind map */
        /* 4-bone skinned casters: the weights' format in uv_format, the bone indices' in wind_format */
        VkFormat uv = d->texture || d->skin4 ? gpu_format(d->uv_format) : VK_FORMAT_UNDEFINED;
        VkFormat wind = d->wind_tex[0] || d->skin4 ? gpu_format(d->wind_format) : VK_FORMAT_UNDEFINED;
        /* why a draw is skipped, counted per reason for YW4_SHADOW_SKIP_LOG=<from>-<to> (GPU frames) */
        int why = !gpu_geometry_at(d->vertices, (uint64_t)d->vertex_count * d->stride, &vb[0], &voff[0]) ? 1
                  : d->index_count && !gpu_geometry_at(d->indices, (uint64_t)d->index_count * isize, &ib, &ioff) ? 2
                  : d->texture && (uv == VK_FORMAT_UNDEFINED || !gpu_handle_texture(cb, s, d->texture, 0, &ii[0])) ? 3
                  : d->skin4 && (uv == VK_FORMAT_UNDEFINED || wind == VK_FORMAT_UNDEFINED) ? 4
                  : d->dither && !gpu_handle_texture(cb, s, d->dither, 0, &ii[1]) ? 5
                  : d->wind_tex[0] && (wind == VK_FORMAT_UNDEFINED || !gpu_handle_texture(cb, s, d->wind_tex[0], 0, &ii[2]) ||
                                       !gpu_handle_texture(cb, s, d->wind_tex[1], 0, &ii[3])) ? 6
                  : 0;
        if (why) {
            skipped++;
            skip_why[why]++;
            draw_skipped(d, why);
            continue;
        }
        if (d->texture && d->has_sampler) {  /* the sampler settings learned with the caster, not the ring slot's now */
            NvnSamplerDesc sd;
            memcpy(&sd, d->sampler, sizeof sd);
            sd.compare_mode = 0;
            ii[0].sampler = gpu_sampler(&sd);
        }
        int kind = d->palette_size ? (d->skin4 ? KIND_SKINNED4 : KIND_SKINNED)
                   : d->wind_tex[0] ? KIND_WIND
                   : d->copies     ? (d->dither ? (d->texture ? KIND_INST_FADE_CUTOUT : KIND_INST_FADE)
                                                : (d->texture ? KIND_INST_CUTOUT : KIND_INST))
                   : d->texture    ? KIND_CUTOUT
                                   : KIND_STATIC;
        if (kind_skinned(kind) && !pal.buf) {  /* can't happen: the gather made the ring */
            skipped++;
            draw_skipped(d, 0);
            continue;
        }
        VkPipeline pipe = pipeline(depth, kind, uv, wind);
        if (pipe != bound) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            if (bound == VK_NULL_HANDLE) gpu_dynamic_state(cb, s, depth, w, h);
            bound = pipe;
            cull = front = -1;
        }
        if (d->cull != cull || d->front != front) gpu_cull(cb, d->cull, d->front), cull = d->cull, front = d->front;
        vb[1] = vb[2] = vb[0], voff[1] = voff[0] + d->uv_offset, voff[2] = voff[0] + d->wind_offset;
        vkCmdBindVertexBuffers2(cb, 0, kind_three(kind) ? 3 : kind_has_uv(kind) ? 2 : 1, vb, voff, NULL, stride);
        if (kind_skinned(kind))
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 0, 1, &pal.set, 1,
                                    &d->palette_offset);
        uint32_t ntex = kind_textures(kind);
        if (ntex) {  /* the cut-out texture at binding 0 if any, then the dither, or the wind textures */
            const VkDescriptorImageInfo *tex[3] = {d->texture ? &ii[0] : &ii[1], kind == KIND_WIND ? &ii[2] : &ii[1], &ii[3]};
            VkDescriptorSet set = gpu_alloc_set(kind == KIND_WIND ? wind_set_layout() : textures_set_layout(ntex));
            VkWriteDescriptorSet wr[3];
            for (uint32_t t = 0; t < ntex; t++)
                wr[t] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = t,
                                               .descriptorCount = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                               .pImageInfo = tex[t]};
            vkUpdateDescriptorSets(vk.device, ntex, wr, 0, NULL);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 0, 1, &set, 0, NULL);
        }
        if (kind == KIND_WIND) {  /* the copies; projection x view, the threshold, the header */
            struct { float vp[16], threshold; uint32_t header; } push;
            memcpy(push.vp, d->mvp, sizeof push.vp);
            push.threshold = d->threshold, push.header = d->wind_header;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 1, 1, &cps.set, 0, NULL);
            vkCmdPushConstants(cb, layout(kind), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind], &push);
        } else if (kind_instanced(kind)) {  /* the copies; only the threshold in push constants */
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 1, 1, &cps.set, 0, NULL);
            vkCmdPushConstants(cb, layout(kind), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind], &d->threshold);
        } else {  /* the matrix, and for cut-out casters the threshold right after it */
            vkCmdPushConstants(cb, layout(kind), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind], d->mvp);
        }
        uint32_t instances = d->copies ? d->copies : 1, first = d->copies ? d->first_copy : 0;
        gpu_query_condition(cb, (int)d->cond_mode, d->cond_addr);  /* skipped when the game's draw would be */
        if (d->index_count) {
            vkCmdBindIndexBuffer(cb, ib, ioff, d->index_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, d->index_count, instances, 0, 0, first);
        } else {
            vkCmdDraw(cb, d->vertex_count, instances, 0, first);
        }
        gpu_query_after_draw(cb);
    }
    {
        static uint64_t from = 1, to;
        if (from == 1) {
            from = 0;
            const char *e = getenv("YW4_SHADOW_SKIP_LOG");
            if (e) sscanf(e, "%" SCNu64 "-%" SCNu64, &from, &to);
        }
        uint64_t f = gpu_frames();
        if (to && f >= from && f <= to) {
            fprintf(stderr, "yw4: shadowskip: frame %" PRIu64 " layer %u: %u draws, skipped %ld (vertices %ld, indices %ld, "
                    "cut-out texture %ld, skin %ld, dither %ld, wind %ld); texture pool %p\n", f, p->depth_layer, p->n,
                    skipped, skip_why[1], skip_why[2], skip_why[3], skip_why[4], skip_why[5], skip_why[6],
                    (const void *)s->tex_pool);
            /* the first cut-out draw's sampler, as the bound sampler pool names it */
            for (uint32_t i = 0; i < p->n; i++) {
                if (!p->d[i].texture) continue;
                NvnSamplerDesc sd;
                if (gpu_handle_sampler(s, p->d[i].texture, &sd))
                    fprintf(stderr, "yw4: shadowskip:   cut-out handle %#" PRIx64 ": sampler filters %u/%u wrap %u/%u/%u "
                            "compare %u lod %g..%g bias %g aniso %g reduction %u\n", p->d[i].texture, sd.min_filter,
                            sd.mag_filter, sd.wrap[0], sd.wrap[1], sd.wrap[2], sd.compare_mode, sd.lod_min, sd.lod_max,
                            sd.lod_bias, sd.max_aniso, sd.reduction);
                else
                    fprintf(stderr, "yw4: shadowskip:   cut-out handle %#" PRIx64 ": no sampler in the bound pool\n",
                            p->d[i].texture);
                break;
            }
        }
    }
}

/* CMD_NATIVE (shadow.c, only with YW4_AB_PASS=shadow): the game's draw that follows is one ours replaces. It is
 * kept in the command list so the same-frame A/B's second run (the game's) has it; every other run skips it. */
void gpu_shadow_game_draw(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    (void)vk_cb, (void)data, (void)size;
    ((GpuState *)gpu_state)->skip_next_draw = !gpu_ab_game;
}

void gpu_shadow_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    const ShadowPass *p;
    if (gpu_ab_game) return;  /* the same-frame A/B's second run: the game's draws instead */
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    if (!p || !p->n) return;
    if (s->rt.depth && s->rt.depth->addr == p->depth_addr && depth_layer_of(&s->rt) == p->depth_layer && !s->ncolor) {
        draw_pass(cb, s, p);
        return;
    }
    /* The game has moved on to another target before the pass's turn (cutscenes: a cascade's second
     * layer goes into a second shadow map, bound before that layer starts, with its own viewport).
     * Draw into the pass's layer with the target and state the game's draws there ended with, then
     * put the game's back. */
    TargetState *game = malloc(sizeof *game), *ours = malloc(sizeof *ours);
    int found = 0;
    if (game && ours) {
        host_lock(&noted_lock);
        for (int i = 0; i < nnoted && !found; i++)
            if (noted[i].rt.depth->addr == p->depth_addr && depth_layer_of(&noted[i].rt) == p->depth_layer)
                *ours = noted[i], render_targets_fix(&ours->rt), found = 1;
        host_unlock(&noted_lock);
    }
    if (!found) {
        free(game), free(ours);
        pass_skipped(p, game && ours ? "the game had moved to another target and its shadow map was not seen before"
                                     : "out of memory");
        return;
    }
    int ncolor = s->ncolor;
    gpu_end_rendering(cb, s);
    target_state_get(game, s);
    target_state_set(s, ours);
    s->ncolor = 0;
    draw_pass(cb, s, p);
    gpu_end_rendering(cb, s);
    target_state_set(s, game);
    s->ncolor = ncolor;
    free(game), free(ours);
}

/* ---- lamp shadows (decision 0081, lamp_shadow.h): the characters drawn into a cube of depth images per chosen lamp,
 * read by the lamp program (lamp_light.frag) through one 2D array with depth compare. Kept in the GENERAL layout,
 * as the executor's other images. */

static struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView array, layer[LAMP_SHADOW_LAYERS];
    VkSampler sampler, raw;  /* depth compare (bilinear); the raw depths (nearest), for the blocker search */
    int ready;
} lm;

/* The images (made once, cleared to "nothing in the way"), outside rendering. */
static void lamp_maps_make(VkCommandBuffer cb) {
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_D32_SFLOAT, .extent = {lamp_shadow_size(), lamp_shadow_size(), 1},
                             .mipLevels = 1, .arrayLayers = LAMP_SHADOW_LAYERS, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &lm.image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, lm.image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &lm.mem));
    CHECK(vkBindImageMemory(vk.device, lm.image, lm.mem, 0));
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = lm.image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_D32_SFLOAT,
                                 .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, LAMP_SHADOW_LAYERS}};
    CHECK(vkCreateImageView(vk.device, &vci, NULL, &lm.array));
    for (uint32_t i = 0; i < LAMP_SHADOW_LAYERS; i++) {
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, i, 1};
        CHECK(vkCreateImageView(vk.device, &vci, NULL, &lm.layer[i]));
    }
    /* hardware compare with a bilinear blend of the four nearest results (the program adds its own taps) */
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .compareEnable = VK_TRUE,
                               .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL, .maxLod = 0.0f};
    CHECK(vkCreateSampler(vk.device, &sci, NULL, &lm.sampler));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST, sci.compareEnable = VK_FALSE;
    CHECK(vkCreateSampler(vk.device, &sci, NULL, &lm.raw));
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = lm.image,
                              .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, LAMP_SHADOW_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearDepthStencilValue one = {1.0f, 0};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, LAMP_SHADOW_LAYERS};
    vkCmdClearDepthStencilImage(cb, lm.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &all);
    full_barrier(cb);
    lm.ready = 1;
    fprintf(stderr, "yw4: lamp shadows: %d depth images of %ux%u made\n", LAMP_SHADOW_LAYERS, lamp_shadow_size(),
            lamp_shadow_size());
}

void gpu_lamp_maps_prepare(void *vk_cb, struct GpuState *s) {
    if (lm.ready) return;
    gpu_end_rendering(vk_cb, s);
    lamp_maps_make(vk_cb);
}

int gpu_lamp_maps(void **view, void **sampler, void **raw) {
    if (!lm.ready) return 0;
    *view = (void *)lm.array, *sampler = (void *)lm.sampler, *raw = (void *)lm.raw;
    return 1;
}

/* Our own state for the lamp views (all of the pipelines' dynamic state): no culling (a character is closed, and the
 * views' handedness differs from the game's), depth test and write, nothing else. */
static void lamp_dynamic_state(VkCommandBuffer cb) {
    uint32_t n = lamp_shadow_size();
    VkViewport vpt = {0, 0, (float)n, (float)n, 0, 1};
    VkRect2D sc = {{0, 0}, {n, n}};
    float blend[4] = {0, 0, 0, 0};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetBlendConstants(cb, blend);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, VK_TRUE);
    vkCmdSetDepthWriteEnable(cb, VK_TRUE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

/* One character draw (skinned, or static) into the bound lamp view; 0 if it can't be drawn. */
static int lamp_draw(VkCommandBuffer cb, const ShadowDraw *d, VkPipeline *bound) {
    if (d->texture || d->copies || d->wind_tex[0] || d->dither) return 0;  /* only what lamp_shadow_add is given */
    VkBuffer vb[3], ib;
    VkDeviceSize voff[3], ioff, stride[3] = {d->stride, d->stride, d->stride};
    uint32_t isize = d->index_type == 2 ? 4 : 2;
    if (!gpu_geometry_at(d->vertices, (uint64_t)d->vertex_count * d->stride, &vb[0], &voff[0])) return 0;
    if (d->index_count && !gpu_geometry_at(d->indices, (uint64_t)d->index_count * isize, &ib, &ioff)) return 0;
    VkFormat uv = d->skin4 ? gpu_format(d->uv_format) : VK_FORMAT_UNDEFINED;
    VkFormat wind = d->skin4 ? gpu_format(d->wind_format) : VK_FORMAT_UNDEFINED;
    if (d->skin4 && (uv == VK_FORMAT_UNDEFINED || wind == VK_FORMAT_UNDEFINED)) return 0;
    int kind = d->palette_size ? (d->skin4 ? KIND_SKINNED4 : KIND_SKINNED) : KIND_STATIC;
    if (kind_skinned(kind) && !pal.buf) return 0;
    VkPipeline pipe = pipeline(VK_FORMAT_D32_SFLOAT, kind, uv, wind);
    if (pipe != *bound) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe), *bound = pipe;
    vb[1] = vb[2] = vb[0], voff[1] = voff[0] + d->uv_offset, voff[2] = voff[0] + d->wind_offset;
    vkCmdBindVertexBuffers2(cb, 0, kind_three(kind) ? 3 : 1, vb, voff, NULL, stride);
    if (kind_skinned(kind))
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 0, 1, &pal.set, 1, &d->palette_offset);
    vkCmdPushConstants(cb, layout(kind), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind], d->mvp);
    if (d->index_count) {
        vkCmdBindIndexBuffer(cb, ib, ioff, d->index_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, d->index_count, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cb, d->vertex_count, 1, 0, 0);
    }
    return 1;
}

void gpu_lamp_shadow_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    const LampShadowPass *p;
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    if (!p || !p->lamps) return;
    gpu_end_rendering(cb, s);
    if (!lm.ready) lamp_maps_make(cb);
    /* the casters' geometry in video memory: copied first, outside rendering (as draw_pass) */
    int copies = 0;
    for (uint32_t i = 0; i < p->n && !copies; i++) {
        const ShadowDraw *d = &p->d[i].d;
        copies = gpu_geometry_needs_copy(d->vertices, (uint64_t)d->vertex_count * d->stride) ||
                 (d->index_count && gpu_geometry_needs_copy(d->indices, (uint64_t)d->index_count * (d->index_type == 2 ? 4 : 2)));
    }
    if (copies)
        for (uint32_t i = 0; i < p->n; i++) {
            const ShadowDraw *d = &p->d[i].d;
            gpu_geometry_copy(cb, d->vertices, (uint64_t)d->vertex_count * d->stride);
            if (d->index_count) gpu_geometry_copy(cb, d->indices, (uint64_t)d->index_count * (d->index_type == 2 ? 4 : 2));
        }
    full_barrier(cb);  /* the copies, and the last frame's lamp draw done reading the images */
    VkPipeline bound = VK_NULL_HANDLE;
    long skipped = 0;
    for (uint32_t layer = 0; layer < p->lamps * LAMP_SHADOW_FACES; layer++) {
        VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = lm.layer[layer],
                                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                        .clearValue = {.depthStencil = {1.0f, 0}}};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {lamp_shadow_size(), lamp_shadow_size()}}, .layerCount = 1,
                              .pDepthAttachment = &da};
        vkCmdBeginRendering(cb, &ri);
        if (layer == 0) lamp_dynamic_state(cb);
        for (uint32_t i = 0; i < p->n; i++)
            if (p->d[i].layer == layer && !lamp_draw(cb, &p->d[i].d, &bound)) skipped++;
        vkCmdEndRendering(cb);
    }
    full_barrier(cb);
    gpu_dynamic_forget();  /* the game's next draw sets its own state again */
    lamp_shadow_drawn(p);
    static long logged;
    if (skipped && logged++ < 3)
        fprintf(stderr, "yw4: lamp shadows: %ld of %u draws left out at GPU frame %" PRIu64 " (geometry or layout)\n",
                skipped, p->n, gpu_frames());
}
