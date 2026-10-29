/* Anti-aliasing between the 3D scene and the UI: FXAA or SMAA on the game's scene image right after its
 * bloom composite (fragment shader 58b87f44, the last 3D pass; the UI is drawn into the same image next),
 * so edges get smoothed and the UI stays sharp (an emulator only has the finished picture, UI included).
 *
 * The scene is linear HDR colour; both methods work on colour as seen, so it is first copied into an
 * RGBA16F image through a reversible curve (runtime/shaders/aa.glsl), and the last pass writes the result
 * back into the game's image through the inverse. SMAA (third_party/smaa, MIT): edges (RG8), blending
 * weights (RGBA8, with its area and search lookup textures), neighbourhood blending. FXAA: one pass.
 * TAA: the 3D draws shifted by a fraction of a pixel per frame (gpu_taa_jitter, applied to the viewport in
 * gpu_dynamic_state), each frame blended into a history moved by the game's own motion vectors (the
 * G-buffer's first target, noted by gpu_taa_note_targets); runtime/shaders/taa_resolve.frag.
 * FSR (mode 4, gpu_fsr.c): AMD's upscaler in place of TAA's blend, from the same jitter, motion and the scene's
 * depth; at the scene's own size for now (native anti-aliasing), falling back to TAA when FSR can't run.
 * YW4_AA=0 off / 1 FXAA / 2 SMAA / 3 TAA / 4 FSR at start; the settings panel (F1) cycles it. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_fsr.h"
#include "gpu_internal.h"
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"

static int aa_mode = -1;

int gpu_aa(void) {
    int m = __atomic_load_n(&aa_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("YW4_AA");
        m = e ? atoi(e) : 0;
        if (m < 0 || m > 4) m = 0;
        __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    }
    return m;
}

void gpu_aa_cycle(void) {
    int m = (gpu_aa() + 1) % 5;
    __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: anti-aliasing %s\n",
            m == 4 ? "FSR" : m == 3 ? "TAA" : m == 2 ? "SMAA" : m == 1 ? "FXAA" : "off");
}

VkShaderModule gpu_native_shader(const char *name) {
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

/* Freed only once the GPU is done with every submission (a size change is rare: the settings panel). */
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

static AaImage encoded, edges, weights, area, search, history[2], fsr_out;
static int history_valid;  /* history[taa_cur] holds last frame's result */

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
    /* row 0 first: Vulkan's texture coordinates start at the top like Direct3D's, which SMAA's tables assume
     * (flipping either made no difference to the weights, 2026-10-27) */
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
        image_free(&history[0]), image_free(&history[1]);
    }
    history_valid = 0;
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_make(&encoded, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, u);
    image_make(&edges, VK_FORMAT_R8G8_UNORM, w, h, u);
    image_make(&weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, u);
    image_make(&history[0], VK_FORMAT_R16G16B16A16_SFLOAT, w, h, u);
    image_make(&history[1], VK_FORMAT_R16G16B16A16_SFLOAT, w, h, u);
    to_general(cb, encoded.image), to_general(cb, edges.image), to_general(cb, weights.image);
    to_general(cb, history[0].image), to_general(cb, history[1].image);
}

/* FSR's result at the output's size (made again when it changes). */
static void output_image(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (fsr_out.image && fsr_out.w == w && fsr_out.h == h) return;
    if (fsr_out.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&fsr_out);
    image_make(&fsr_out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    to_general(cb, fsr_out.image);
}

/* ---- pipelines: one layout (5 sampled images, the size as a push constant), one pipeline per pass */

enum { P_ENCODE, P_FXAA, P_EDGE, P_WEIGHT, P_BLEND, P_SHOW_EDGES, P_SHOW_WEIGHTS, P_TEST, P_TAA, P_TAA_OUT, P_UPSCALE, P_DEPTH, PASSES };

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
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
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
                                                "smaa_weight.vert",   "smaa_blend.vert",    "aa_fullscreen.vert",
                                                "aa_fullscreen.vert", "aa_fullscreen.vert", "aa_fullscreen.vert",
                                                "aa_fullscreen.vert", "aa_fullscreen.vert", "aa_fullscreen.vert"};
    static const char *const fs_name[PASSES] = {"aa_encode.frag",   "fxaa.frag",           "smaa_edge.frag",
                                                "smaa_weight.frag", "smaa_blend.frag",     "aa_show_edges.frag",
                                                "aa_show_weights.frag", "aa_test.frag", "taa_resolve.frag",
                                                "taa_output.frag", "aa_upscale.frag", "aa_depth_upscale.frag"};
    static VkShaderModule vs[PASSES], fs[PASSES];
    if (!vs[pass]) vs[pass] = gpu_native_shader(vs_name[pass]), fs[pass] = gpu_native_shader(fs_name[pass]);
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
    int depth = pass == P_DEPTH;  /* `out` is then a depth format: no colour, depth always written */
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = depth, .depthWriteEnable = depth,
                                                .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = depth ? 0 : 1, .pAttachments = &cba};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = depth ? 0 : 1, .pColorAttachmentFormats = &out};
    if (depth) {
        ri.depthAttachmentFormat = out;
        if (out == VK_FORMAT_D24_UNORM_S8_UINT || out == VK_FORMAT_D32_SFLOAT_S8_UINT) ri.stencilAttachmentFormat = out;
    }
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy,
        .layout = pipe_layout};
    CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &pipes[n].pipe));
    pipes[n].pass = pass, pipes[n].out = out;
    return pipes[n++].pipe;
}

static float pass_params[4];  /* the passes' second push constant (TAA's) */

/* One full-screen pass into `view` (w x h, format out), reading the given images: binding 0 scene_in (NULL:
 * the encoded scene), 1 in1 (NULL: edges), 2 and 3 SMAA's tables, 4 in4 (NULL: weights). */
static void pass_with(VkCommandBuffer cb, int p, VkImageView view, VkFormat out, uint32_t w, uint32_t h, int clear,
                      VkImageView scene_in, VkImageView in1, VkImageView in4) {
    VkImageView in[5] = {scene_in ? scene_in : encoded.view, in1 ? in1 : edges.view, area.view, search.view,
                         in4 ? in4 : weights.view};
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
    float push[8] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h,
                     pass_params[0], pass_params[1], pass_params[2], pass_params[3]};
    vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    full_barrier(cb);
}

static void pass(VkCommandBuffer cb, int p, VkImageView view, VkFormat out, uint32_t w, uint32_t h, int clear,
                 VkImageView scene_in) {
    pass_with(cb, p, view, out, w, h, clear, scene_in, NULL, NULL);
}

/* Upscaling: the 3D's depth (sampled through `src`) stretched into the output-sized depth image `view`. The
 * stencil starts at 0 (the UI makes its own). */
static void depth_pass(VkCommandBuffer cb, VkImageView view, VkFormat fmt, uint32_t w, uint32_t h, VkImageView src) {
    VkDescriptorSet set = gpu_alloc_set(set_layout);
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet wr[5];
    VkImageView in[5] = {src, src, src, src, src};  /* only binding 0 is read; the others must be valid views */
    for (int i = 0; i < 5; i++) {
        ii[i] = (VkDescriptorImageInfo){point, in[i], VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                       .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(vk.device, 5, wr, 0, NULL);
    int stencil = fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .pDepthAttachment = &da, .pStencilAttachment = stencil ? &da : NULL};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(P_DEPTH, fmt));
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &set, 0, NULL);
    VkViewport vpt = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float push[8] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h, 0, 0, 0, 0};
    vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    full_barrier(cb);
}

/* ---- TAA: the jitter and the game's motion target */

static uint32_t scene_w, scene_h;  /* the scene's size at the last anti-aliasing (0: none yet) */
static uint32_t scene_ow;          /* ... and the output's width (upscaling: larger) */
/* The game's motion target is used for other things later in the frame, so it is copied into our own image
 * as soon as the G-buffer pass ends (gpu_taa_targets_end). */
static NvnTexture *gbuffer_motion;  /* the open G-buffer pass's motion target (NULL: none open) */
static AaImage motion;
static uint64_t motion_frame = ~0ull;  /* the frame it was copied in */
static int taa_cur;

static float halton(int i, int base) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) f /= (float)base, r += f * (float)(i % base);
    return r;
}

/* The shift, in pixels, for a 3D draw into a w x h target this frame: TAA on and the target the scene's
 * size (the caller leaves out what follows the composite); else none (0). 8 positions (Halton 2, 3),
 * one per frame. */
int gpu_taa_jitter(uint32_t w, uint32_t h, float *jx, float *jy) {
    if (gpu_aa() < 3 || !scene_w || w != scene_w || h != scene_h) return 0;
    static int nshift;  /* YW4_TAA_LOG: counted per frame, logged every 300 */
    static uint64_t counted = ~0ull;
    if (counted != gpu_frames()) {
        if (counted % 300 == 0 && getenv("YW4_TAA_LOG"))
            fprintf(stderr, "yw4: taa: frame %llu: %d draws shifted\n", (unsigned long long)counted, nshift);
        counted = gpu_frames(), nshift = 0;
    }
    nshift++;
    if (gpu_aa() == 4 && gpu_fsr_available() == 1) {  /* FSR's own sequence */
        gpu_fsr_jitter(w, scene_ow ? scene_ow : w, gpu_frames(), jx, jy);
        return 1;
    }
    int i = (int)(gpu_frames() % 8) + 1;
    *jx = halton(i, 2) - 0.5f, *jy = halton(i, 3) - 0.5f;
    return 1;
}

/* A draw's targets: the G-buffer's (3 colour targets, the first RG16F: the game's motion) is noted. */
void gpu_taa_note_targets(const GpuState *s) {
    if (gpu_aa() < 3 || s->ncolor != 3 || !s->rt.color[0]) return;
    const GpuImage *g = image_find(s->rt.color[0]);
    if (g && g->format == VK_FORMAT_R16G16_SFLOAT) gbuffer_motion = s->rt.color[0];
}

/* The render targets change (rendering has ended): if the G-buffer was drawn into, copy its motion. */
void gpu_taa_targets_end(VkCommandBuffer cb) {
    NvnTexture *t = gbuffer_motion;
    gbuffer_motion = NULL;
    if (!t) return;
    GpuImage *g = image_of(cb, t);
    if (!g || g->format != VK_FORMAT_R16G16_SFLOAT) return;
    if (!motion.image || motion.w != g->width || motion.h != g->height) {
        if (motion.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&motion);
        image_make(&motion, VK_FORMAT_R16G16_SFLOAT, g->width, g->height,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        to_general(cb, motion.image);
    }
    full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {g->width, g->height, 1}};
    vkCmdCopyImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, motion.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cb);
    motion_frame = gpu_frames();
}

static void taa(VkCommandBuffer cb, VkImageView target, VkFormat fmt, uint32_t w, uint32_t h, int debug) {
    /* the G-buffer and the composite run on different render threads: a frame apart at most */
    uint64_t now = gpu_frames();
    VkImageView mv = motion.image && motion_frame + 1 >= now && motion_frame <= now + 1 ? motion.view : NULL;
    static uint64_t last = ~0ull;  /* a frame without TAA in between (a menu without the 3D view): start over */
    if (last + 1 != gpu_frames()) history_valid = 0;
    last = gpu_frames();
    float sy = nvn_window_origin == 1 ? -1.0f : 1.0f;  /* the game's screen y runs up when the origin is flipped */
    pass_params[0] = history_valid && mv ? 1.0f : 0.0f, pass_params[1] = 0.0f;
    pass_params[2] = 1.0f, pass_params[3] = sy;
    const char *e = getenv("YW4_TAA_SIGN");  /* testing: "x,y" overrides the motion's signs */
    if (e) sscanf(e, "%f,%f", &pass_params[2], &pass_params[3]);
    int next = !taa_cur;
    pass_with(cb, P_TAA, history[next].view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, NULL,
              mv ? mv : encoded.view, history[taa_cur].view);
    VkImageView result = history[next].view;
    if (debug == 4 || debug == 5) {  /* the motion, or the moved history's difference: into a spare image */
        pass_params[1] = (float)debug;
        pass_with(cb, P_TAA, weights.view, VK_FORMAT_R8G8B8A8_UNORM, w, h, 0, NULL, mv ? mv : encoded.view,
                  history[taa_cur].view);
        result = weights.view;
    }
    memset(pass_params, 0, sizeof pass_params);
    /* sharpening after the blend (YW4_TAA_SHARPEN, 0 none; the debug views: none) */
    static float sharpen = -1.0f;
    if (sharpen < 0.0f) sharpen = getenv("YW4_TAA_SHARPEN") ? (float)atof(getenv("YW4_TAA_SHARPEN")) : 0.5f;
    pass_params[0] = result == history[next].view ? sharpen : 0.0f;
    pass(cb, P_TAA_OUT, target, fmt, w, h, 0, result);
    pass_params[0] = 0.0f;
    taa_cur = next, history_valid = 1;
}

/* FSR in place of TAA's blend: the encoded scene, the scene's depth and the game's motion in (w x h), FSR's
 * result out (ow x oh: larger when upscaling) through the curve's inverse (no extra sharpening: FSR has its
 * own). 0: FSR could not run. */
static int fsr(VkCommandBuffer cb, GpuImage *d, VkImageView target, VkFormat fmt, uint32_t w, uint32_t h, uint32_t ow,
               uint32_t oh) {
    uint64_t now = gpu_frames();
    if (!motion.image || motion_frame + 1 < now || motion_frame > now + 1) return 0;
    if (!d || d->width != w || d->height != h || motion.w != w || motion.h != h) return 0;
    output_image(cb, ow, oh);
    static uint64_t last = ~0ull;
    static uint32_t last_ow;
    static int64_t last_ns;
    int64_t t = host_now_ns();
    GpuFsrInputs in = {.color = encoded.image, .depth = d->image, .motion = motion.image, .output = fsr_out.image,
                       .color_format = VK_FORMAT_R16G16B16A16_SFLOAT, .depth_format = d->format,
                       .output_format = VK_FORMAT_R16G16B16A16_SFLOAT, .render_w = w, .render_h = h, .output_w = ow,
                       .output_h = oh, .reset = last + 1 != now || last_ow != ow,
                       .frame_ms = last_ns ? (float)(t - last_ns) / 1e6f : 16.7f};
    /* the jitter as it was applied (gpu_taa_jitter); the motion from this frame's position to last frame's in
     * render pixels: TAA reads last frame at uv - motion x (1, sy) */
    gpu_fsr_jitter(w, ow, now, &in.jitter[0], &in.jitter[1]);
    float sy = nvn_window_origin == 1 ? -1.0f : 1.0f, mx = 1.0f, my = 1.0f, jsx = 1.0f, jsy = 1.0f;
    const char *e = getenv("YW4_FSR_MV_SIGN");  /* testing: "x,y" flips the motion */
    if (e) sscanf(e, "%f,%f", &mx, &my);
    e = getenv("YW4_FSR_JITTER_SIGN");  /* testing: "x,y" flips the jitter told to FSR */
    if (e) sscanf(e, "%f,%f", &jsx, &jsy);
    in.motion_scale[0] = -(float)w * mx, in.motion_scale[1] = -(float)h * sy * my;
    in.jitter[0] *= jsx, in.jitter[1] *= jsy;
    static float sharp = -1.0f, cnear = -1.0f, cfar = -1.0f;
    if (sharp < 0.0f) {
        sharp = getenv("YW4_FSR_SHARPNESS") ? (float)atof(getenv("YW4_FSR_SHARPNESS")) : 0.5f;
        cnear = getenv("YW4_FSR_NEAR") ? (float)atof(getenv("YW4_FSR_NEAR")) : 0.1f;
        cfar = getenv("YW4_FSR_FAR") ? (float)atof(getenv("YW4_FSR_FAR")) : 1000.0f;
    }
    in.sharpness = sharp, in.cam_near = cnear, in.cam_far = cfar, in.fov_y = 0.785398f;  /* 45 degrees, town's */
    {   /* YW4_FSR_LOG: every 300 frames, how often FSR was asked and how often it started over */
        static int calls, resets, on = -1;
        if (on < 0) on = getenv("YW4_FSR_LOG") != NULL;
        calls++, resets += in.reset;
        if (on && calls % 300 == 0)
            fprintf(stderr, "yw4: fsr: %d calls, %d resets; frame %llu jitter %.3f %.3f\n", calls, resets,
                    (unsigned long long)now, in.jitter[0], in.jitter[1]);
    }
    full_barrier(cb);
    if (!gpu_fsr_dispatch(cb, &in)) return 0;
    full_barrier(cb);
    pass(cb, P_TAA_OUT, target, fmt, ow, oh, 0, fsr_out.view);  /* sharpen 0 (pass_params cleared) */
    last = now, last_ow = ow, last_ns = t;
    history_valid = 0;  /* TAA's history is stale when it takes over again */
    return 1;
}

void gpu_aa_apply(VkCommandBuffer cb, GpuState *s) {
    int mode = gpu_aa(), up = gpu_upscale() != 0;
    NvnTexture *t = s->ncolor ? s->rt.color[0] : NULL;
    if ((!mode && !up) || !t || gpu_upscaled(t)) return;
    gpu_end_rendering(cb, s);
    GpuImage *g = image_of(cb, t);
    uint32_t w, h;
    VkImageView target = gpu_render_view(cb, t, &s->rt.views[0], s->rt.has_view[0], &w, &h);
    VkImageView scene = gpu_sample_view(cb, t, &s->rt.views[0], s->rt.has_view[0], VK_IMAGE_VIEW_TYPE_2D);
    if (!target || !scene || !w || !h) return;
    const NvnTextureView *v = &s->rt.views[0];  /* the format the view renders in (as begin_rendering) */
    VkFormat fmt = s->rt.has_view[0] && v->setmask & 4 ? gpu_format(v->format) : g->format;
    /* upscaling: from here to the present the scene image's address names the output-sized image (gpu.c);
     * `target` and `scene` stay the 3D's */
    /* the 3D's depth, for FSR and for the stretched copy (fetched before the redirect names another image) */
    NvnTexture *dt = s->rt.depth;
    GpuImage *depth = dt ? image_of(cb, dt) : NULL;
    VkImageView depth_in = depth && depth->width == w && depth->height == h
                               ? gpu_sample_view(cb, dt, &s->rt.depth_view, s->rt.has_depth_view, VK_IMAGE_VIEW_TYPE_2D)
                               : NULL;
    if (!depth_in) depth = NULL;
    VkImageView out = target;
    uint32_t ow = w, oh = h;
    if (up) {
        gpu_upscale_begin(t->addr, depth ? dt->addr : 0);
        image_of(cb, t);
        out = gpu_render_view(cb, t, &s->rt.views[0], s->rt.has_view[0], &ow, &oh);
        if (!out || !ow || !oh) {
            gpu_upscale_begin(0, 0);
            out = target, ow = w, oh = h;
        } else if (depth) {  /* the output-sized depth: the 3D's stretched into it */
            GpuImage *od = image_of(cb, dt);
            uint32_t dw, dh;
            VkImageView dv = gpu_render_view(cb, dt, &s->rt.depth_view, s->rt.has_depth_view, &dw, &dh);
            layouts();
            full_barrier(cb);
            if (od && dv && dw == ow && dh == oh) depth_pass(cb, dv, od->format, ow, oh, depth_in);
        }
    }
    layouts();
    lookup_textures(cb);
    working_images(cb, w, h);
    scene_w = w, scene_h = h, scene_ow = ow;
    full_barrier(cb);
    /* YW4_AA_DEBUG=1 / 2: SMAA's edges / blend weights in place of the picture; 3: a test pattern in place
     * of the scene, anti-aliased as usual */
    static int debug = -1;
    if (debug < 0) debug = getenv("YW4_AA_DEBUG") ? atoi(getenv("YW4_AA_DEBUG")) : 0;
    static const char *const label[5] = {"upscaling (plain)", "anti-aliasing: FXAA", "anti-aliasing: SMAA",
                                         "anti-aliasing: TAA", "anti-aliasing: FSR"};
    gpu_passes_other(cb, label[mode], 0);  /* its own line in the per-pass timer (F5) */
    pass(cb, debug == 3 ? P_TEST : P_ENCODE, encoded.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, scene);
    if (mode == 4 && !debug && depth && fsr(cb, depth, out, fmt, w, h, ow, oh)) {
        /* done: FSR wrote the output's size */
    } else {
        if (mode >= 3) {
            taa(cb, target, fmt, w, h, debug);
        } else if (mode == 1) {
            pass(cb, P_FXAA, target, fmt, w, h, 0, NULL);
        } else if (mode == 2) {
            pass(cb, P_EDGE, edges.view, VK_FORMAT_R8G8_UNORM, w, h, 1, NULL);
            pass(cb, P_WEIGHT, weights.view, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, NULL);
            pass(cb, debug == 1 ? P_SHOW_EDGES : debug == 2 ? P_SHOW_WEIGHTS : P_BLEND, target, fmt, w, h, 0, NULL);
        }
        if (out != target) {  /* upscaling without FSR: the anti-aliased 3D stretched to the output's size */
            pass(cb, P_ENCODE, encoded.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, scene);
            pass(cb, P_UPSCALE, out, fmt, ow, oh, 0, encoded.view);  /* bilinear over the whole image */
        }
    }
    gpu_dynamic_forget();  /* our pipelines set viewport and scissor: the game's next draw sets its own again */
    gpu_passes_other(cb, "after anti-aliasing (the UI)", 0);
}
