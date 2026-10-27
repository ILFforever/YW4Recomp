/* Anti-aliasing between the 3D scene and the UI: FXAA or SMAA on the game's scene image right after its
 * bloom composite (fragment shader 58b87f44, the last 3D pass; the UI is drawn into the same image next),
 * so edges get smoothed and the UI stays sharp (an emulator only has the finished picture, UI included).
 *
 * The scene is linear HDR colour; both methods work on colour as seen, so it is first copied into an
 * RGBA16F image through a reversible curve (runtime/shaders/aa.glsl), and the last pass writes the result
 * back into the game's image through the inverse. SMAA (third_party/smaa, MIT): edges (RG8), blending
 * weights (RGBA8, with its area and search lookup textures), neighbourhood blending. FXAA: one pass.
 * YW4_AA=0 off / 1 FXAA / 2 SMAA at start; the settings panel (F1) cycles it. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_internal.h"
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"

static int aa_mode = -1;

int gpu_aa(void) {
    int m = __atomic_load_n(&aa_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("YW4_AA");
        m = e ? atoi(e) : 0;
        if (m < 0 || m > 2) m = 0;
        __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    }
    return m;
}

void gpu_aa_cycle(void) {
    int m = (gpu_aa() + 1) % 3;
    __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: anti-aliasing %s\n", m == 2 ? "SMAA" : m == 1 ? "FXAA" : "off");
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

/* ---- our images (layout GENERAL, like the game's) */

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t w, h;
} AaImage;

static void image_make(AaImage *im, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                             .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &im->image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, im->image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &im->mem));
    CHECK(vkBindImageMemory(vk.device, im->image, im->mem, 0));
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(vk.device, &vci, NULL, &im->view));
    im->w = w, im->h = h;
}

/* Freed only once the GPU is done with every submission (a size change is rare: F4, F1). */
static void image_free(AaImage *im) {
    if (!im->image) return;
    vkDestroyImageView(vk.device, im->view, NULL);
    vkDestroyImage(vk.device, im->image, NULL);
    vkFreeMemory(vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static void to_general(VkCommandBuffer cb, VkImage image) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

static AaImage encoded, edges, weights, area, search;

/* The SMAA lookup textures, uploaded once (in cb, outside rendering). */
static void lookup_textures(VkCommandBuffer cb) {
    if (area.image) return;
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_make(&area, VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, u);
    image_make(&search, VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, u);
    static VkBuffer buf;  /* kept: the copy may still be running when this returns */
    static VkDeviceMemory mem;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = AREATEX_SIZE + SEARCHTEX_SIZE,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(vk.device, buf, mem, 0));
    uint8_t *p;
    CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    memcpy(p, areaTexBytes, AREATEX_SIZE);
    memcpy(p + AREATEX_SIZE, searchTexBytes, SEARCHTEX_SIZE);
    to_general(cb, area.image), to_general(cb, search.image);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {AREATEX_WIDTH, AREATEX_HEIGHT, 1}};
    vkCmdCopyBufferToImage(cb, buf, area.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    c.bufferOffset = AREATEX_SIZE, c.imageExtent = (VkExtent3D){SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1};
    vkCmdCopyBufferToImage(cb, buf, search.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cb);
}

/* The working images at the scene's size (made again when it changes). */
static void working_images(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (encoded.image && encoded.w == w && encoded.h == h) return;
    if (encoded.image) {
        gpu_wait(gpu_submitted(), UINT64_MAX);
        image_free(&encoded), image_free(&edges), image_free(&weights);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_make(&encoded, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, u);
    image_make(&edges, VK_FORMAT_R8G8_UNORM, w, h, u);
    image_make(&weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, u);
    to_general(cb, encoded.image), to_general(cb, edges.image), to_general(cb, weights.image);
}

/* ---- pipelines: one layout (5 sampled images, the size as a push constant), one pipeline per pass */

enum { P_ENCODE, P_FXAA, P_EDGE, P_WEIGHT, P_BLEND, PASSES };

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipe_layout;
static VkSampler linear, point;

static void layouts(void) {
    if (set_layout) return;
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                              NULL};
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5,
                                           .pBindings = b};
    CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &set_layout));
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &pipe_layout));
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
    CHECK(vkCreateSampler(vk.device, &si, NULL, &linear));
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;  /* SMAA's search texture is read texel for texel */
    CHECK(vkCreateSampler(vk.device, &si, NULL, &point));
}

static VkPipeline pipeline(int pass, VkFormat out) {
    enum { MAX = 16 };
    static struct { int pass; VkFormat out; VkPipeline pipe; } pipes[MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (pipes[i].pass == pass && pipes[i].out == out) return pipes[i].pipe;
    if (n == MAX) gpu_stop("anti-aliasing: too many pipelines");
    static const char *const vs_name[PASSES] = {"aa_fullscreen.vert", "aa_fullscreen.vert", "smaa_edge.vert",
                                                "smaa_weight.vert", "smaa_blend.vert"};
    static const char *const fs_name[PASSES] = {"aa_encode.frag", "fxaa.frag", "smaa_edge.frag", "smaa_weight.frag",
                                                "smaa_blend.frag"};
    static VkShaderModule vs[PASSES], fs[PASSES];
    if (!vs[pass]) vs[pass] = module(vs_name[pass]), fs[pass] = module(fs_name[pass]);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs[pass], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs[pass], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &out};
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy,
        .layout = pipe_layout};
    CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &pipes[n].pipe));
    pipes[n].pass = pass, pipes[n].out = out;
    return pipes[n++].pipe;
}

/* One full-screen pass into `view` (w x h, format out), reading the given images (NULL: a stand-in). */
static void pass(VkCommandBuffer cb, int p, VkImageView view, VkFormat out, uint32_t w, uint32_t h, int clear,
                 VkImageView scene_in) {
    VkImageView in[5] = {scene_in ? scene_in : encoded.view, edges.view, area.view, search.view, weights.view};
    VkDescriptorSet set = gpu_alloc_set(set_layout);
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet wr[5];
    for (int i = 0; i < 5; i++) {
        ii[i] = (VkDescriptorImageInfo){i == 3 ? point : linear, in[i], VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                       .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(vk.device, 5, wr, 0, NULL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(p, out));
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &set, 0, NULL);
    VkViewport vpt = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float metrics[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, metrics);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    full_barrier(cb);
}

void gpu_aa_apply(VkCommandBuffer cb, GpuState *s) {
    int mode = gpu_aa();
    NvnTexture *t = s->ncolor ? s->rt.color[0] : NULL;
    if (!mode || !t) return;
    gpu_end_rendering(cb, s);
    GpuImage *g = image_of(cb, t);
    uint32_t w, h;
    VkImageView target = gpu_render_view(cb, t, &s->rt.views[0], s->rt.has_view[0], &w, &h);
    VkImageView scene = gpu_sample_view(cb, t, &s->rt.views[0], s->rt.has_view[0], VK_IMAGE_VIEW_TYPE_2D);
    if (!target || !scene || !w || !h) return;
    const NvnTextureView *v = &s->rt.views[0];  /* the format the view renders in (as begin_rendering) */
    VkFormat fmt = s->rt.has_view[0] && v->setmask & 4 ? gpu_format(v->format) : g->format;
    layouts();
    lookup_textures(cb);
    working_images(cb, w, h);
    full_barrier(cb);
    pass(cb, P_ENCODE, encoded.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, scene);
    if (mode == 1) {
        pass(cb, P_FXAA, target, fmt, w, h, 0, NULL);
    } else {
        pass(cb, P_EDGE, edges.view, VK_FORMAT_R8G8_UNORM, w, h, 1, NULL);
        pass(cb, P_WEIGHT, weights.view, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, NULL);
        pass(cb, P_BLEND, target, fmt, w, h, 0, NULL);
    }
    gpu_dynamic_forget();  /* our pipelines set viewport and scissor: the game's next draw sets its own again */
}
