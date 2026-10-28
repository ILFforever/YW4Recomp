/* Our post-processing passes, the executor side: bloom (decision 0037) and the distance blur (decision 0038).
 * The engine sides and the data: bloom.c, dof.c, post.h.
 *
 * Each runs where its hook's CMD_NATIVE record sits, in place of the engine's draws, with the same quad,
 * viewport, samplers, blend and operations as those (runtime/shaders/bloom*, dof*), so the result is identical.
 * Bloom: the bright pass into the half-size image, each level blurred across and down, the composite onto the
 * scene image the game has bound (s's first colour target), then anti-aliasing as after the game's composite.
 * Distance blur: the scene shrunk into the half-size image, the blur rounds (across, down) between that and its
 * pair, the composite onto the scene by depth (the panel's DISTANCE BLUR setting: half its amount, or none).
 *
 * Check mode (YW4_BLOOM=check, YW4_DOF_PASS=check): the same passes into copies of the images (the scene copied
 * first); after the game's own composite both versions are copied out and compared once the GPU is done
 * (gpu_post_flush). Every 300th frame, or the frames in YW4_BLOOM_CHECK_AT / YW4_DOF_CHECK_AT. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_internal.h"
#include "post.h"

enum { P_BRIGHT, P_BLUR, P_COMPOSITE, P_DOF_COPY, P_DOF_BLUR, P_DOF_COMPOSITE, PASSES };
enum { BLEND_NONE, BLEND_BLOOM, BLEND_DOF };
static const struct { const char *vs, *fs; int blend; } programs[PASSES] = {
    {"bloom.vert", "bloom_bright.frag", BLEND_NONE},
    {"bloom.vert", "bloom_blur.frag", BLEND_NONE},
    {"bloom.vert", "bloom_composite.frag", BLEND_BLOOM},  /* colour + scene x (1 - alpha), the scene's alpha kept */
    {"dof.vert", "dof_copy.frag", BLEND_NONE},
    {"dof.vert", "dof_blur.frag", BLEND_NONE},
    {"dof.vert", "dof_composite.frag", BLEND_DOF},  /* colour x alpha + scene x (1 - alpha), our alpha written */
};
enum { PUSH = 128 };

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipe_layout;
/* The game's samplers for these images (as its draws bind them), made through gpu_sampler so the texture
 * filtering setting (YW4_ANISO) changes ours as it changes the game's: the colour images' (min filter 3, mag 1,
 * clamped, levels 0-15) and the distance blur's depth (min 2, mag 0). */
static const NvnSamplerDesc color_sampler = {.min_filter = 3, .mag_filter = 1, .wrap = {7, 7, 7}, .compare_func = 8,
                                             .lod_min = 0.0f, .lod_max = 15.0f, .max_aniso = 1.0f};
static const NvnSamplerDesc depth_sampler = {.min_filter = 2, .mag_filter = 0, .wrap = {7, 7, 7}, .compare_func = 8,
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
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PUSH};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &pipe_layout));
}

static VkShaderModule shader(const char *name) {
    enum { MAX = 16 };
    static struct { const char *name; VkShaderModule m; } loaded[MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(loaded[i].name, name)) return loaded[i].m;
    if (n == MAX) gpu_stop("post-processing: too many shaders");
    loaded[n].name = name, loaded[n].m = gpu_native_shader(name);
    return loaded[n++].m;
}

static VkPipeline pipeline(int pass, VkFormat out) {
    enum { MAX = 32 };
    static struct { int pass; VkFormat out; VkPipeline pipe; } pipes[MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (pipes[i].pass == pass && pipes[i].out == out) return pipes[i].pipe;
    if (n == MAX) gpu_stop("post-processing: too many pipelines");
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = shader(programs[pass].vs), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = shader(programs[pass].fs), .pName = "main"}};
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
    int blend = programs[pass].blend;
    VkPipelineColorBlendAttachmentState cba = {.blendEnable = (VkBool32)(blend != BLEND_NONE),
                                               .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD,
                                               .colorWriteMask = 0xF};
    if (blend == BLEND_BLOOM) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE, cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    } else if (blend == BLEND_DOF) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
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

/* One full-screen pass reading `in` (sampler per input: NULL = the colour images'). The viewport as
 * gpu_dynamic_state makes the game's (its 0, 0, width, height scaled like the image, flipped for the
 * upper-left origin), so the quad covers the same pixels the same way. */
static void pass(VkCommandBuffer cb, int p, const Dest *d, const VkImageView *in, int nin,
                 const NvnSamplerDesc *const *samplers, const float push[PUSH / 4]) {
    VkDescriptorSet set = gpu_alloc_set(set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet wr[3];
    for (int i = 0; i < 3; i++) {
        int k = i < nin ? i : 0;
        const NvnSamplerDesc *sd = samplers && samplers[k] ? samplers[k] : &color_sampler;
        ii[i] = (VkDescriptorImageInfo){gpu_sampler(sd), in[k], VK_IMAGE_LAYOUT_GENERAL};
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
    vkCmdPushConstants(cb, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PUSH, push);
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

static uint32_t texel_bytes(VkFormat f) {
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : f == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : 4;
}

enum { CHECK_MAX = 8 };
typedef struct {
    const char *name, *env;      /* for the log line; the variable with the frames to check */
    int n;
    const char *image[CHECK_MAX];
    int open;                    /* our copies are drawn, waiting for the game's composite */
    uint64_t frame;
    Copy ours[CHECK_MAX];
    NvnTexture game[CHECK_MAX];  /* the game's images, compared with ours */
    VkBuffer buf[2][CHECK_MAX];  /* ours, the game's, read back */
    VkDeviceMemory mem[2][CHECK_MAX];
    uint32_t bytes[CHECK_MAX];
    int pending;
    uint64_t at[16];
    int nat;                     /* -1: the frames not read yet */
} Check;

static Check bloom_chk = {"bloom", "YW4_BLOOM_CHECK_AT", 8,
                          {"bright", "level 0 across", "level 0", "level 1 across", "level 1", "level 2 across",
                           "level 2", "scene"}, .nat = -1};
static Check dof_chk = {"distance blur", "YW4_DOF_CHECK_AT", 3, {"half", "half across", "scene"}, .nat = -1};
static Check blur_chk = {"blur rounds", "YW4_BLUR_CHECK_AT", 2, {"a", "b"}, .nat = -1};

static int check_now(Check *k) {
    if (k->open || k->pending) return 0;
    if (k->nat < 0) {
        k->nat = 0;
        for (const char *e = getenv(k->env); e && *e && k->nat < 16; e += strcspn(e, ","), e += *e == ',')
            k->at[k->nat++] = strtoull(e, NULL, 10);
    }
    uint64_t frame = gpu_frames();
    if (!k->nat) return frame % 300 == 0;
    for (int i = 0; i < k->nat; i++)
        if (k->at[i] == frame) return 1;
    return 0;
}

/* After the game's own pass: copy both versions of every image out. */
static void check_copy_out(Check *k, VkCommandBuffer cb) {
    if (!k->open) return;
    k->open = 0;
    full_barrier(cb);
    for (int i = 0; i < k->n; i++) {
        GpuImage *g = image_find(&k->game[i]);
        Copy *c = &k->ours[i];
        k->bytes[i] = 0;
        if (!g || g->width != c->w || g->height != c->h) continue;
        k->bytes[i] = c->w * c->h * texel_bytes(c->format);
        for (int j = 0; j < 2; j++) {
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = k->bytes[i],
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            CHECK(vkCreateBuffer(vk.device, &bci, NULL, &k->buf[j][i]));
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(vk.device, k->buf[j][i], &mr);
            /* cached, so the CPU's compare reads it at memory speed: uncached reads of the ~300 MB of a 4K check
             * took 13 s and stalled the game (2026-10-28) */
            VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            uint32_t type = vk_memory_type(mr.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (type == UINT32_MAX) type = vk_memory_type(mr.memoryTypeBits, host);
            VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                        .memoryTypeIndex = type};
            CHECK(vkAllocateMemory(vk.device, &mai, NULL, &k->mem[j][i]));
            CHECK(vkBindBufferMemory(vk.device, k->buf[j][i], k->mem[j][i], 0));
            VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {c->w, c->h, 1}};
            vkCmdCopyImageToBuffer(cb, j ? g->image : c->image, VK_IMAGE_LAYOUT_GENERAL, k->buf[j][i], 1, &r);
        }
    }
    full_barrier(cb);
    k->pending = 1;
}

static void check_flush(Check *k) {
    if (!k->pending) return;
    k->pending = 0;
    char line[512];
    int at = snprintf(line, sizeof line, "yw4: %s: check frame %" PRIu64 ":", k->name, k->frame);
    for (int i = 0; i < k->n; i++) {
        if (!k->bytes[i]) {
            at += snprintf(line + at, sizeof line - (size_t)at, " %s not compared;", k->image[i]);
            continue;
        }
        const uint8_t *a, *b;
        CHECK(vkMapMemory(vk.device, k->mem[0][i], 0, VK_WHOLE_SIZE, 0, (void **)&a));
        CHECK(vkMapMemory(vk.device, k->mem[1][i], 0, VK_WHOLE_SIZE, 0, (void **)&b));
        uint32_t tb = texel_bytes(k->ours[i].format), n = k->bytes[i] / tb, bad = 0;
        for (uint32_t t = 0; t < n; t++) bad += memcmp(a + (size_t)t * tb, b + (size_t)t * tb, tb) != 0;
        at += snprintf(line + at, sizeof line - (size_t)at, " %s %u of %u differ;", k->image[i], bad, n);
        for (int j = 0; j < 2; j++) {
            vkUnmapMemory(vk.device, k->mem[j][i]);
            vkDestroyBuffer(vk.device, k->buf[j][i], NULL), vkFreeMemory(vk.device, k->mem[j][i], NULL);
        }
        if (at >= (int)sizeof line - 64) break;
    }
    fprintf(stderr, "%s\n", line);
}

int gpu_post_pending(void) { return bloom_chk.pending || dof_chk.pending || blur_chk.pending; }

void gpu_post_flush(void) { check_flush(&bloom_chk), check_flush(&dof_chk), check_flush(&blur_chk); }

/* An image the record names: made, and a view to sample it. NULL if not. */
static GpuImage *record_image(VkCommandBuffer cb, NvnTexture *t, VkImageView *sample) {
    GpuImage *g = image_of(cb, t);
    *sample = g ? gpu_sample_view(cb, t, NULL, 0, VK_IMAGE_VIEW_TYPE_2D) : VK_NULL_HANDLE;
    return *sample ? g : NULL;
}

/* ---- bloom */

void gpu_bloom_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    BloomPass p;
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    if (p.check && !check_now(&bloom_chk)) return;
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
    for (int i = 0; i < BLOOM_IMAGES; i++)
        if (!(g[i] = record_image(cb, &p.tex[i], &sv[i]))) return;
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
            Copy *c = &bloom_chk.ours[i - BLOOM_BRIGHT];
            copy_make(c, g[i]);
            dst[i].view = rd[i] = c->view;
        }
    }
    full_barrier(cb);
    if (p.check) {
        Copy *c = &bloom_chk.ours[bloom_chk.n - 1];
        copy_make(c, sg);
        image_copy(cb, sg->image, c->image, sg->width, sg->height);
        full_barrier(cb);
        scene.view = c->view, scene.format = sg->format;
    }
    float push[PUSH / 4];
    memset(push, 0, sizeof push);
    memcpy(push, p.bright, sizeof p.bright);
    gpu_passes_other(cb, "our bloom: bright pass and blur", 0);  /* the per-pass timer (F5) */
    VkImageView in[3] = {rd[BLOOM_IN_SCENE], rd[BLOOM_IN_HALF]};
    pass(cb, P_BRIGHT, &dst[BLOOM_BRIGHT], in, 2, NULL, push);
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
        pass(cb, P_BLUR, &dst[across], in, 1, NULL, push);
        push[0] = 0.0f, push[1] = p.step[l] / (float)p.tex[across].height;
        in[0] = rd[across];
        pass(cb, P_BLUR, &dst[down], in, 1, NULL, push);
        src = down;
    }
    memset(push, 0, sizeof push);
    memcpy(push, p.composite, sizeof p.composite);
    VkImageView lv[3] = {rd[BLOOM_L0], rd[BLOOM_L1], rd[BLOOM_L2]};
    gpu_passes_other(cb, "our bloom: composite", 0);
    pass(cb, P_COMPOSITE, &scene, lv, 3, NULL, push);
    gpu_passes_targets(cb, s);  /* what follows is the game's again */
    gpu_dynamic_forget();  /* our pipelines set viewport and scissor: the game's next draw sets its own again */
    if (p.check) {
        bloom_chk.open = 1, bloom_chk.frame = gpu_frames();
        for (int i = BLOOM_BRIGHT; i < BLOOM_IMAGES; i++) bloom_chk.game[i - BLOOM_BRIGHT] = p.tex[i];
        bloom_chk.game[bloom_chk.n - 1] = *scene_t;
        return;
    }
    s->after_composite = 1;
    if (gpu_aa()) gpu_aa_apply(cb, s);
}

void gpu_bloom_after_composite(void *vk_cb, void *gpu_state) {
    (void)gpu_state;
    check_copy_out(&bloom_chk, vk_cb);
}

/* ---- distance blur */

/* The camera's far distance as the game's composite reads it: row 3 .y of the camera block (fragment constant
 * buffer 8) the game has bound, else the renderer's. */
static float camera_far(const GpuState *s, float fallback) {
    const GpuRange *r = &s->ubo[1][8];  /* stage 1: the fragment stage (gpu_draw.c nvn_stage) */
    return r->addr && r->size >= 56 ? *(const float *)(uintptr_t)(r->addr + 52) : fallback;
}

void gpu_dof_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    DofPass p;
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    int mode = gpu_dof();  /* the panel's DISTANCE BLUR: 0 off (no composite), 1 half its amount, 2 the game's */
    if (p.check && (!mode || !check_now(&dof_chk))) return;
    gpu_end_rendering(cb, s);
    layouts();
    /* with full-size effects these keep the game's size ratio, as when the game's own draws are seen (gpu_draw.c) */
    gpu_keep_game_ratio(&p.tex[DOF_A]), gpu_keep_game_ratio(&p.tex[DOF_B]);
    GpuImage *g[DOF_IMAGES];
    VkImageView sv[DOF_IMAGES];
    for (int i = 0; i < DOF_IMAGES; i++)
        if (!(g[i] = record_image(cb, &p.tex[i], &sv[i]))) return;
    Dest dst[DOF_IMAGES];
    VkImageView rd[DOF_IMAGES];
    for (int i = 0; i < DOF_IMAGES; i++) {
        uint32_t w = 0, h = 0;
        VkImageView rv = i == DOF_DEPTH ? VK_NULL_HANDLE : gpu_render_view(cb, &p.tex[i], NULL, 0, &w, &h);
        dst[i] = (Dest){rv, g[i]->format, w, h, g[i]};
        rd[i] = sv[i];
    }
    if (p.check) {  /* copies of the two half-size images and of the scene (copied first: the composite blends) */
        static const int img[3] = {DOF_A, DOF_B, DOF_SCENE};
        for (int k = 0; k < 3; k++) {
            Copy *c = &dof_chk.ours[k];
            copy_make(c, g[img[k]]);
            dst[img[k]].view = c->view;
            if (img[k] != DOF_SCENE) rd[img[k]] = c->view;
        }
        full_barrier(cb);
        image_copy(cb, g[DOF_SCENE]->image, dof_chk.ours[2].image, g[DOF_SCENE]->width, g[DOF_SCENE]->height);
    }
    full_barrier(cb);
    float push[PUSH / 4];
    memset(push, 0, sizeof push);
    gpu_passes_other(cb, "our distance blur: shrink and blur", 0);
    VkImageView in[2] = {rd[DOF_SCENE]};
    pass(cb, P_DOF_COPY, &dst[DOF_A], in, 1, NULL, push);
    memcpy(push + 12, p.weights, sizeof p.weights);
    push[20] = 1.0f;  /* blur taps */
    for (uint32_t r = 0; r < p.rounds; r++) {
        push[8] = p.spread / (float)p.tex[DOF_A].width, push[9] = 0.0f;
        in[0] = rd[DOF_A];
        pass(cb, P_DOF_BLUR, &dst[DOF_B], in, 1, NULL, push);
        push[8] = 0.0f, push[9] = p.spread / (float)p.tex[DOF_A].height;
        in[0] = rd[DOF_B];
        pass(cb, P_DOF_BLUR, &dst[DOF_A], in, 1, NULL, push);
    }
    if (mode) {
        memset(push, 0, sizeof push);
        memcpy(push, p.focus, sizeof p.focus);
        if (mode == 1) push[3] *= 0.5f;  /* half the amount (the game's draws get the same in gpu_draw.c) */
        push[8] = p.texel[0], push[9] = p.texel[1];
        push[20] = 2.0f, push[21] = camera_far(s, p.far);
        VkImageView cv[2] = {rd[DOF_A], rd[DOF_DEPTH]};
        const NvnSamplerDesc *samp[2] = {&color_sampler, &depth_sampler};
        gpu_passes_other(cb, "our distance blur: composite", 0);
        pass(cb, P_DOF_COMPOSITE, &dst[DOF_SCENE], cv, 2, samp, push);
    }
    gpu_passes_targets(cb, s);
    gpu_dynamic_forget();
    if (p.check) {
        dof_chk.open = 1, dof_chk.frame = gpu_frames();
        dof_chk.game[0] = p.tex[DOF_A], dof_chk.game[1] = p.tex[DOF_B], dof_chk.game[2] = p.tex[DOF_SCENE];
    }
}

void gpu_dof_after_composite(void *vk_cb, void *gpu_state) {
    (void)gpu_state;
    check_copy_out(&dof_chk, vk_cb);
}

/* ---- the engine's shared blur rounds (dof.c yw4_hook_blur) */

void gpu_blur_pass(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    VkCommandBuffer cb = vk_cb;
    GpuState *s = gpu_state;
    BlurPass p;
    if (size < sizeof p) return;
    memcpy(&p, data, sizeof p);
    if (p.check && !check_now(&blur_chk)) return;
    gpu_end_rendering(cb, s);
    layouts();
    NvnTexture *t[2] = {&p.a, &p.b};
    GpuImage *g[2];
    VkImageView rd[2];
    Dest dst[2];
    for (int i = 0; i < 2; i++) {
        if (!(g[i] = record_image(cb, t[i], &rd[i]))) return;
        uint32_t w, h;
        VkImageView rv = gpu_render_view(cb, t[i], NULL, 0, &w, &h);
        dst[i] = (Dest){rv, g[i]->format, w, h, g[i]};
    }
    if (p.check) {  /* our rounds on copies of both, starting from the same images */
        for (int i = 0; i < 2; i++) {
            Copy *c = &blur_chk.ours[i];
            copy_make(c, g[i]);
            full_barrier(cb);
            image_copy(cb, g[i]->image, c->image, g[i]->width, g[i]->height);
            dst[i].view = rd[i] = c->view;
        }
    }
    full_barrier(cb);
    /* YW4_BLUR_FAST=1: blurs add up (their widths squared), so n rounds equal one sqrt(n) times as wide; a
     * quarter of the rounds (at least 1) with the taps sqrt(n / m) times as far apart give the same total width
     * for a quarter of the passes. Not identical (the 8-tap kernel's ends are cut differently); not in check mode. */
    if (blur_fast() && !p.check && p.rounds > 2) {
        uint32_t m = (p.rounds + 3) / 4;
        float k = sqrtf((float)p.rounds / (float)m);
        p.step[0] *= k, p.step[1] *= k, p.rounds = m;
    }
    gpu_passes_other(cb, "our blur rounds", 0);
    float push[PUSH / 4];
    memset(push, 0, sizeof push);
    memcpy(push + 12, p.weights, sizeof p.weights);
    push[20] = 1.0f;  /* blur taps */
    VkImageView in[1];
    for (uint32_t r = 0; r < p.rounds; r++) {
        push[8] = p.step[0], push[9] = 0.0f;
        in[0] = rd[0];
        pass(cb, P_DOF_BLUR, &dst[1], in, 1, NULL, push);
        push[8] = 0.0f, push[9] = p.step[1];
        in[0] = rd[1];
        pass(cb, P_DOF_BLUR, &dst[0], in, 1, NULL, push);
    }
    gpu_passes_targets(cb, s);
    gpu_dynamic_forget();
    if (p.check) {
        blur_chk.open = 1, blur_chk.frame = gpu_frames();
        blur_chk.game[0] = p.a, blur_chk.game[1] = p.b;
    }
}

void gpu_blur_check_end(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    (void)data, (void)size;
    gpu_end_rendering(vk_cb, gpu_state);
    check_copy_out(&blur_chk, vk_cb);
}
