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

#include "gpu_internal.h"
#include "host.h"
#include "shadow.h"

static long passes_skipped, draws_skipped;

void gpu_shadow_counts(long *p, long *d) {
    *p = __atomic_exchange_n(&passes_skipped, 0, __ATOMIC_RELAXED);
    *d = __atomic_exchange_n(&draws_skipped, 0, __ATOMIC_RELAXED);
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
enum { KIND_STATIC, KIND_CUTOUT, KIND_SKINNED, KIND_INST, KIND_INST_CUTOUT, KIND_INST_FADE, KIND_INST_FADE_CUTOUT, KINDS };
/* Push constants: the matrix (64 bytes), for cut-out casters also the threshold; instanced ones
 * only the threshold (their matrices come per copy). */
static const uint32_t push_size[KINDS] = {64, 68, 64, 4, 4, 4, 4};
static int kind_instanced(int kind) { return kind >= KIND_INST; }

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

static VkDescriptorSetLayout copies_set_layout(void) {
    if (!cps.layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 1, .pBindings = &b};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &cps.layout));
    }
    return cps.layout;
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
    VkDescriptorSetLayout l = copies_set_layout();
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

static VkDescriptorSetLayout palette_set_layout(void) {
    if (!pal.layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 1, .pBindings = &b};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &pal.layout));
    }
    return pal.layout;
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
    VkDescriptorSetLayout l = palette_set_layout();
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
static uint32_t kind_textures(int kind) {
    return kind == KIND_INST_FADE_CUTOUT ? 2 : kind == KIND_CUTOUT || kind == KIND_INST_CUTOUT || kind == KIND_INST_FADE;
}

static VkPipelineLayout layout(int kind) {
    static VkPipelineLayout l[KINDS];
    if (!l[kind]) {
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind]};
        VkDescriptorSetLayout set[2] = {kind == KIND_SKINNED ? palette_set_layout() : textures_set_layout(kind_textures(kind)),
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
static int kind_has_uv(int kind) { return kind == KIND_CUTOUT || kind == KIND_INST_CUTOUT || kind == KIND_INST_FADE_CUTOUT; }
static VkPipeline pipeline(VkFormat depth, int kind, VkFormat uv) {
    enum { MAX = 32 };
    static struct { VkFormat depth, uv; int kind; VkPipeline pipe; } pipes[MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (pipes[i].depth == depth && pipes[i].kind == kind && pipes[i].uv == uv) return pipes[i].pipe;
    if (n == MAX) gpu_stop("shadow pass: too many pipelines");
    static VkShaderModule vs[KINDS], fs[KINDS];
    static const char *const vs_name[KINDS] = {"shadow_static.vert",          "shadow_cutout.vert",
                                               "shadow_skinned.vert",         "shadow_instanced.vert",
                                               "shadow_instanced_cutout.vert", "shadow_instanced.vert",
                                               "shadow_instanced_cutout.vert"};
    static const char *const fs_name[KINDS] = {NULL, "shadow_cutout.frag", NULL, NULL, "shadow_cutout.frag",
                                               "shadow_fade.frag", "shadow_fade_cutout.frag"};
    if (!vs[kind]) vs[kind] = module(vs_name[kind]);
    if (fs_name[kind] && !fs[kind]) fs[kind] = module(fs_name[kind]);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs[kind], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs[kind], .pName = "main"}};
    VkVertexInputBindingDescription bind[2] = {{0, 12, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 12, VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attr[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 1, uv, 0}};
    if (kind == KIND_SKINNED) {
        attr[1] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32B32A32_UINT, 28};
        attr[2] = (VkVertexInputAttributeDescription){2, 0, VK_FORMAT_R16G16B16A16_UINT, 44};
    }
    uint32_t nattr = kind == KIND_SKINNED ? 3 : kind_has_uv(kind) ? 2 : 1;
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = kind_has_uv(kind) ? 2 : 1,
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
    CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &pipes[n].pipe));
    pipes[n].depth = depth, pipes[n].kind = kind, pipes[n].uv = uv;
    return pipes[n++].pipe;
}

void gpu_shadow_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    const ShadowPass *p;
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    if (!p || !p->n) return;
    uint32_t layer = s->rt.has_depth_view && s->rt.depth_view.setmask & 2 ? s->rt.depth_view.min_layer : 0;
    if (!s->rt.depth || s->rt.depth->addr != p->depth_addr || layer != p->depth_layer || s->ncolor) {
        __atomic_add_fetch(&passes_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    /* cut-out casters' textures and the dither: images made and uploaded before rendering, as for
     * the game's draws */
    uint64_t dither_uploaded = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        if (p->d[i].texture) {
            if (s->rendering) gpu_end_rendering(cb, s);
            gpu_handle_texture(cb, s, p->d[i].texture, 1, NULL);
        }
        if (p->d[i].dither && p->d[i].dither != dither_uploaded) {
            if (s->rendering) gpu_end_rendering(cb, s);
            gpu_handle_texture(cb, s, p->d[i].dither, 1, NULL);
            dither_uploaded = p->d[i].dither;
        }
    }
    VkFormat color[8], depth;
    uint32_t w, h;
    if (!gpu_begin_targets(cb, s, color, &depth, &w, &h)) {
        __atomic_add_fetch(&passes_skipped, 1, __ATOMIC_RELAXED);
        return;
    }
    VkPipeline bound = VK_NULL_HANDLE;
    int cull = -1, front = -1;
    long skipped = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        const ShadowDraw *d = &p->d[i];
        VkBuffer vb[2], ib;
        VkDeviceSize voff[2], ioff, stride[2] = {d->stride, d->stride};
        uint32_t isize = d->index_type == 2 ? 4 : 2;
        VkDescriptorImageInfo ii[2];  /* the cut-out texture, the dither */
        VkFormat uv = d->texture ? gpu_format(d->uv_format) : VK_FORMAT_UNDEFINED;
        if (!gpu_buffer_at(d->vertices, (uint64_t)d->vertex_count * d->stride, &vb[0], &voff[0]) ||
            (d->index_count && !gpu_buffer_at(d->indices, (uint64_t)d->index_count * isize, &ib, &ioff)) ||
            (d->texture && (uv == VK_FORMAT_UNDEFINED || !gpu_handle_texture(cb, s, d->texture, 0, &ii[0]))) ||
            (d->dither && !gpu_handle_texture(cb, s, d->dither, 0, &ii[1]))) {
            skipped++;
            continue;
        }
        int kind = d->palette_size ? KIND_SKINNED
                   : d->copies     ? (d->dither ? (d->texture ? KIND_INST_FADE_CUTOUT : KIND_INST_FADE)
                                                : (d->texture ? KIND_INST_CUTOUT : KIND_INST))
                   : d->texture    ? KIND_CUTOUT
                                   : KIND_STATIC;
        if (kind == KIND_SKINNED && !pal.buf) {  /* can't happen: the gather made the ring */
            skipped++;
            continue;
        }
        VkPipeline pipe = pipeline(depth, kind, uv);
        if (pipe != bound) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            if (bound == VK_NULL_HANDLE) gpu_dynamic_state(cb, s, depth, w, h);
            bound = pipe;
            cull = front = -1;
        }
        if (d->cull != cull || d->front != front) gpu_cull(cb, d->cull, d->front), cull = d->cull, front = d->front;
        vb[1] = vb[0], voff[1] = voff[0] + d->uv_offset;
        vkCmdBindVertexBuffers2(cb, 0, kind_has_uv(kind) ? 2 : 1, vb, voff, NULL, stride);
        if (kind == KIND_SKINNED)
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(KIND_SKINNED), 0, 1, &pal.set, 1,
                                    &d->palette_offset);
        uint32_t ntex = kind_textures(kind);
        if (ntex) {  /* the cut-out texture at binding 0 if any, then the dither */
            const VkDescriptorImageInfo *first = d->texture ? &ii[0] : &ii[1];
            VkDescriptorSet set = gpu_alloc_set(textures_set_layout(ntex));
            VkWriteDescriptorSet wr[2] = {
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = first},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1]}};
            vkUpdateDescriptorSets(vk.device, ntex, wr, 0, NULL);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 0, 1, &set, 0, NULL);
        }
        if (kind_instanced(kind)) {  /* the copies; only the threshold in push constants */
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout(kind), 1, 1, &cps.set, 0, NULL);
            vkCmdPushConstants(cb, layout(kind), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind], &d->threshold);
        } else {  /* the matrix, and for cut-out casters the threshold right after it */
            vkCmdPushConstants(cb, layout(kind), VK_SHADER_STAGE_VERTEX_BIT, 0, push_size[kind], d->mvp);
        }
        uint32_t instances = d->copies ? d->copies : 1, first = d->copies ? d->first_copy : 0;
        if (d->index_count) {
            vkCmdBindIndexBuffer(cb, ib, ioff, d->index_type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, d->index_count, instances, 0, 0, first);
        } else {
            vkCmdDraw(cb, d->vertex_count, instances, 0, first);
        }
    }
    if (skipped) __atomic_add_fetch(&draws_skipped, skipped, __ATOMIC_RELAXED);
}
