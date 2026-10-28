/* Our bloom pass, the executor side (decision 0037; the engine side and the data: bloom.c, bloom.h).
 *
 * Run where the hook's CMD_NATIVE record sits, in place of the engine's 8 draws: the bright pass into the
 * half-size image, each level blurred across and down, and the composite onto the scene image the game has
 * bound (s's first colour target), then anti-aliasing as after the game's composite. Same quad, viewport,
 * sampler, blend and operations as the game's draws (runtime/shaders/bloom*), so the result is identical.
 *
 * Check mode: the same passes into copies of the images (the scene copied first), then, after the game's
 * own composite, both are copied out and compared once the GPU is done (gpu_bloom_flush). Every 300th
 * frame, or the frames in YW4_BLOOM_CHECK_AT. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bloom.h"
#include "gpu_internal.h"

enum { P_BRIGHT, P_BLUR, P_COMPOSITE, PASSES };

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipe_layout;
/* The game's sampler for these images (as its draws bind it: min filter 3, mag 1, clamped, levels 0-15), made
 * through gpu_sampler so the texture filtering setting (YW4_ANISO) changes ours as it changes the game's. */
static const NvnSamplerDesc game_sampler = {.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 7}, .compare_func = 8,
                                            .lod_min = 0.0f, .lod_max = 15.0f, .max_aniso = 1.0f};

static void layouts(void) {
    if (set_layout) return;
    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                              NULL};
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3,
                                           .pBindings = b};
    CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &set_layout));
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &pipe_layout));
}

static VkPipeline pipeline(int pass, VkFormat out) {
    enum { MAX = 16 };
    static struct { int pass; VkFormat out; VkPipeline pipe; } pipes[MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (pipes[i].pass == pass && pipes[i].out == out) return pipes[i].pipe;
    if (n == MAX) gpu_stop("bloom: too many pipelines");
    static const char *const fs_name[PASSES] = {"bloom_bright.frag", "bloom_blur.frag", "bloom_composite.frag"};
    static VkShaderModule vs, fs[PASSES];
    if (!vs) vs = gpu_native_shader("bloom.vert");
    if (!fs[pass]) fs[pass] = gpu_native_shader(fs_name[pass]);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
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
    /* the composite as the game blends it: colour + scene x (1 - alpha), the scene's alpha kept */
    int blend = pass == P_COMPOSITE;
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = (VkBool32)blend, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF};
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

/* Where a pass draws: a view, its format and size, and the game's size of the image (for the viewport). */
typedef struct {
    VkImageView view;
    VkFormat format;
    uint32_t w, h;
    const GpuImage *g;  /* the game's image (its scale and width factor) */
} Dest;

/* One full-screen pass. The viewport as gpu_dynamic_state makes the game's (its 0, 0, width, height scaled
 * like the image, flipped for the upper-left origin), so the quad covers the same pixels the same way. */
static void pass(VkCommandBuffer cb, int p, const Dest *d, const VkImageView *in, int nin, const float push[16]) {
    VkDescriptorSet set = gpu_alloc_set(set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet wr[3];
    for (int i = 0; i < 3; i++) {
        ii[i] = (VkDescriptorImageInfo){gpu_sampler(&game_sampler), in[i < nin ? i : 0], VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                       .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(vk.device, 3, wr, 0, NULL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = d->view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {d->w, d->h}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(p, d->format));
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &set, 0, NULL);
    int64_t kn = d->g->scale_num, kd = d->g->scale_den, xn = kn * d->g->wide_num, xd = kd * d->g->wide_den;
    float kf = (float)kn / (float)kd, xf = (float)xn / (float)xd;
    float vw = (float)d->g->key.width * xf, vh = (float)d->g->key.height * kf;
    int flip = nvn_window_origin == 1;
    VkViewport vpt = {0.0f, flip ? vh : 0.0f, vw, flip ? -vh : vh, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {d->w, d->h}};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, push);
    vkCmdDraw(cb, 6, 1, 0, 0);
    vkCmdEndRendering(cb);
    full_barrier(cb);
}

/* ---- check mode: our results in copies, compared with the game's */

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkFormat format;
    uint32_t w, h;
} Copy;

static void copy_make(Copy *c, const GpuImage *g) {
    if (c->image && c->format == g->format && c->w == g->width && c->h == g->height) return;
    if (c->image) {
        gpu_wait(gpu_submitted(), UINT64_MAX);
        vkDestroyImageView(vk.device, c->view, NULL), vkDestroyImage(vk.device, c->image, NULL);
        vkFreeMemory(vk.device, c->mem, NULL);
    }
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = g->format, .extent = {g->width, g->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &c->image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, c->image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &c->mem));
    CHECK(vkBindImageMemory(vk.device, c->image, c->mem, 0));
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = c->image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g->format,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(vk.device, &vci, NULL, &c->view));
    c->format = g->format, c->w = g->width, c->h = g->height;
}

static void image_copy(VkCommandBuffer cb, VkImage from, VkImage to, uint32_t w, uint32_t h) {
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

enum { CHECKED = BLOOM_IMAGES - 2 + 1 };  /* the 7 images the passes write, and the scene */
static const char *const checked_name[CHECKED] = {"bright", "level 0 across", "level 0", "level 1 across", "level 1",
                                                  "level 2 across", "level 2", "scene"};
static struct {
    int open;                    /* our copies are drawn, waiting for the game's composite */
    uint64_t frame;
    Copy ours[CHECKED];
    NvnTexture game[CHECKED];    /* the game's images, compared with ours */
    VkBuffer buf[2][CHECKED];    /* ours, the game's, read back */
    VkDeviceMemory mem[2][CHECKED];
    uint32_t bytes[CHECKED];
    int pending;
} chk;

static int check_frame(uint64_t frame) {
    static uint64_t at[16];
    static int nat = -1;
    if (nat < 0) {
        nat = 0;
        for (const char *e = getenv("YW4_BLOOM_CHECK_AT"); e && *e && nat < 16; e += strcspn(e, ","), e += *e == ',')
            at[nat++] = strtoull(e, NULL, 10);
    }
    if (!nat) return frame % 300 == 0;
    for (int i = 0; i < nat; i++)
        if (at[i] == frame) return 1;
    return 0;
}

static uint32_t texel_bytes(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : f == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : 4;
}

/* ---- the pass */

void gpu_bloom_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    BloomPass p;
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    if (p.check && (chk.open || chk.pending || !check_frame(gpu_frames()))) return;
    NvnTexture *scene_t = s->ncolor ? s->rt.color[0] : NULL;
    if (!scene_t) {
        static int logged;
        if (!logged++) fprintf(stderr, "yw4: bloom: no scene target bound at our pass\n");
        return;
    }
    gpu_end_rendering(cb, s);
    layouts();
    GpuImage *g[BLOOM_IMAGES];
    VkImageView sv[BLOOM_IMAGES];
    for (int i = 0; i < BLOOM_IMAGES; i++) {
        g[i] = image_of(cb, &p.tex[i]);
        sv[i] = g[i] ? gpu_sample_view(cb, &p.tex[i], NULL, 0, VK_IMAGE_VIEW_TYPE_2D) : VK_NULL_HANDLE;
        if (!sv[i]) return;
    }
    GpuImage *sg = image_of(cb, scene_t);
    uint32_t sw, sh;
    VkImageView scene_rv = gpu_render_view(cb, scene_t, &s->rt.views[0], s->rt.has_view[0], &sw, &sh);
    if (!sg || !scene_rv) return;
    const NvnTextureView *v = &s->rt.views[0];  /* the format the scene renders in (as begin_rendering) */
    VkFormat scene_fmt = s->rt.has_view[0] && v->setmask & 4 ? gpu_format(v->format) : sg->format;
    /* where each pass draws, and what the next one reads: the game's images, or in check mode our copies */
    Dest dst[BLOOM_IMAGES], scene = {scene_rv, scene_fmt, sw, sh, sg};
    VkImageView rd[BLOOM_IMAGES];
    for (int i = 0; i < BLOOM_IMAGES; i++) rd[i] = sv[i];
    for (int i = BLOOM_BRIGHT; i < BLOOM_IMAGES; i++) {
        uint32_t w, h;
        VkImageView rv = gpu_render_view(cb, &p.tex[i], NULL, 0, &w, &h);
        dst[i] = (Dest){rv, g[i]->format, w, h, g[i]};
        if (p.check) {
            Copy *c = &chk.ours[i - BLOOM_BRIGHT];
            copy_make(c, g[i]);
            dst[i].view = rd[i] = c->view;
        }
    }
    full_barrier(cb);
    if (p.check) {
        Copy *c = &chk.ours[CHECKED - 1];
        copy_make(c, sg);
        image_copy(cb, sg->image, c->image, sg->width, sg->height);
        full_barrier(cb);
        scene.view = c->view, scene.format = sg->format;
    }
    float push[16];
    memset(push, 0, sizeof push);
    memcpy(push, p.bright, sizeof p.bright);
    VkImageView in[3] = {rd[BLOOM_IN_SCENE], rd[BLOOM_IN_HALF]};
    pass(cb, P_BRIGHT, &dst[BLOOM_BRIGHT], in, 2, push);
    static const float weights[4] = {0.15021657943725586f, 0.13866737484931946f, 0.1181645467877388f,
                                     0.09295152127742767f};
    int src = BLOOM_BRIGHT;
    for (int l = 0; l < 3; l++) {
        int across = BLOOM_L0_ACROSS + 2 * l, down = across + 1;
        memset(push, 0, sizeof push);
        memcpy(push + 4, weights, sizeof weights);
        push[12] = 1.0f;  /* blur taps */
        push[0] = p.step[l] / (float)p.tex[across].width;
        in[0] = rd[src];
        pass(cb, P_BLUR, &dst[across], in, 1, push);
        push[0] = 0.0f, push[1] = p.step[l] / (float)p.tex[across].height;
        in[0] = rd[across];
        pass(cb, P_BLUR, &dst[down], in, 1, push);
        src = down;
    }
    memset(push, 0, sizeof push);
    memcpy(push, p.composite, sizeof p.composite);
    VkImageView lv[3] = {rd[BLOOM_L0], rd[BLOOM_L1], rd[BLOOM_L2]};
    pass(cb, P_COMPOSITE, &scene, lv, 3, push);
    gpu_dynamic_forget();  /* our pipelines set viewport and scissor: the game's next draw sets its own again */
    if (p.check) {
        chk.open = 1, chk.frame = gpu_frames();
        for (int i = BLOOM_BRIGHT; i < BLOOM_IMAGES; i++) chk.game[i - BLOOM_BRIGHT] = p.tex[i];
        chk.game[CHECKED - 1] = *scene_t;
        return;
    }
    s->after_composite = 1;
    if (gpu_aa()) gpu_aa_apply(cb, s);
}

/* Check mode: the game's composite has been drawn; copy both versions of every image out. */
void gpu_bloom_after_composite(void *vk_cb, void *gpu_state) {
    (void)gpu_state;
    if (!chk.open) return;
    VkCommandBuffer cb = vk_cb;
    chk.open = 0;
    full_barrier(cb);
    for (int i = 0; i < CHECKED; i++) {
        GpuImage *g = image_find(&chk.game[i]);
        Copy *c = &chk.ours[i];
        chk.bytes[i] = 0;
        if (!g || g->width != c->w || g->height != c->h) continue;
        chk.bytes[i] = c->w * c->h * texel_bytes(c->format);
        for (int k = 0; k < 2; k++) {
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = chk.bytes[i],
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            CHECK(vkCreateBuffer(vk.device, &bci, NULL, &chk.buf[k][i]));
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(vk.device, chk.buf[k][i], &mr);
            /* cached, so the CPU's compare reads it at memory speed: uncached reads of the ~300 MB of a 4K check
             * took 13 s and stalled the game (2026-10-28) */
            VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            uint32_t type = vk_memory_type(mr.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (type == UINT32_MAX) type = vk_memory_type(mr.memoryTypeBits, host);
            VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                        .memoryTypeIndex = type};
            CHECK(vkAllocateMemory(vk.device, &mai, NULL, &chk.mem[k][i]));
            CHECK(vkBindBufferMemory(vk.device, chk.buf[k][i], chk.mem[k][i], 0));
            VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {c->w, c->h, 1}};
            vkCmdCopyImageToBuffer(cb, k ? g->image : c->image, VK_IMAGE_LAYOUT_GENERAL, chk.buf[k][i], 1, &r);
        }
    }
    full_barrier(cb);
    chk.pending = 1;
}

int gpu_bloom_pending(void) { return chk.pending; }

void gpu_bloom_flush(void) {
    if (!chk.pending) return;
    chk.pending = 0;
    char line[512];
    int at = snprintf(line, sizeof line, "yw4: bloom: check frame %" PRIu64 ":", chk.frame);
    for (int i = 0; i < CHECKED; i++) {
        if (!chk.bytes[i]) {
            at += snprintf(line + at, sizeof line - (size_t)at, " %s not compared;", checked_name[i]);
            continue;
        }
        const uint8_t *a, *b;
        CHECK(vkMapMemory(vk.device, chk.mem[0][i], 0, VK_WHOLE_SIZE, 0, (void **)&a));
        CHECK(vkMapMemory(vk.device, chk.mem[1][i], 0, VK_WHOLE_SIZE, 0, (void **)&b));
        uint32_t tb = texel_bytes(chk.ours[i].format), n = chk.bytes[i] / tb, bad = 0;
        for (uint32_t t = 0; t < n; t++) bad += memcmp(a + (size_t)t * tb, b + (size_t)t * tb, tb) != 0;
        at += snprintf(line + at, sizeof line - (size_t)at, " %s %u of %u differ;", checked_name[i], bad, n);
        for (int k = 0; k < 2; k++) {
            vkUnmapMemory(vk.device, chk.mem[k][i]);
            vkDestroyBuffer(vk.device, chk.buf[k][i], NULL), vkFreeMemory(vk.device, chk.mem[k][i], NULL);
        }
        if (at >= (int)sizeof line - 64) break;
    }
    fprintf(stderr, "%s\n", line);
}
