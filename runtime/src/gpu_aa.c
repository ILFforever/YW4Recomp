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
 * DLSS (mode 5, gpu_dlss.c): NVIDIA's upscaler in the same place, on NVIDIA RTX GPUs; FSR, then TAA, when it can't.
 * YW4_AA=0 off / 1 FXAA / 2 SMAA / 3 TAA / 4 FSR / 5 DLSS at start; the settings panel (F1) cycles it.
 * SHARPENING (gpu_sharpen, decision 0076) comes after any of them, OFF included. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_dlss.h"
#include "gpu_dlssg.h"
#include "gpu_fg.h"
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
        if (m < 0 || m > 5) m = 0;
        if (m == 5 && !gpu_dlss_built()) {
            m = gpu_fsr_built() ? 4 : 3;
            fprintf(stderr, "yw4: aa: DLSS is not in this build: %s instead\n", m == 4 ? "FSR" : "TAA");
        }
        if (m == 4 && !gpu_fsr_built()) {
            m = 3;
            fprintf(stderr, "yw4: aa: FSR is not in this build: TAA instead\n");
        }
        __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    }
    if (m == 5 && __atomic_load_n(&vk.up, __ATOMIC_ACQUIRE) && (!gpu_dlss_built() || !gpu_dlss_available())) {
        m = gpu_fsr_built() ? 4 : 3;
        fprintf(stderr, "yw4: aa: DLSS is not available on this GPU: %s instead\n", m == 4 ? "FSR" : "TAA");
        __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    }
    return m;
}

/* One step forward (dir 1) or back (dir -1), skipping FSR and DLSS where they can't run: one switch per step */
void gpu_aa_step(int dir) {
    int m = (gpu_aa() + 6 + dir) % 6;
    while ((m == 5 && (!gpu_dlss_built() || !gpu_dlss_available())) || (m == 4 && !gpu_fsr_built()))
        m = (m + 6 + dir) % 6;
    __atomic_store_n(&aa_mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: anti-aliasing %s\n",
            m == 5 ? "DLSS" : m == 4 ? "FSR" : m == 3 ? "TAA" : m == 2 ? "SMAA" : m == 1 ? "FXAA" : "off");
}

void gpu_aa_cycle(void) { gpu_aa_step(1); }

/* SHARPENING (decision 0076): a slider (issue #11: a player asked for one) that stops at five levels, OFF / LOW /
 * MEDIUM / HIGH / MAX at 0 / 25 / 50 / 75 / 100%, MEDIUM the default; it shows the level's name, no number (owner,
 * 2026-11-16). Kept in percent: sharpen_amount blends between the levels, should finer steps come back. One choice
 * for every anti-aliasing: TAA and DLSS get it in their last pass (taa_output.frag), FSR as its own sharpener's
 * amount (RCAS, so nothing is sharpened twice), OFF / FXAA / SMAA as a pass of its own. Saved as YW4_SHARPEN, the
 * slider's fraction ("0.50"); gpu_sharpen_parse reads that and the files written before the slider (0..4, the
 * levels). */
static int sharpen_pct = -1;
enum { SHARPEN_STEP = 25 };  /* percent between two levels */

static int sharpen_snap(int p) {
    p = (p + SHARPEN_STEP / 2) / SHARPEN_STEP * SHARPEN_STEP;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

int gpu_sharpen_parse(const char *e) {
    if (!e || !*e) return 50;
    int p;
    if (strchr(e, '.')) p = (int)(atof(e) * 100.0 + 0.5);  /* the slider's fraction */
    else if ((p = atoi(e)) >= 1 && p <= 4) p *= 25;          /* LOW, MEDIUM, HIGH, MAX before the slider */
    return sharpen_snap(p);
}

int gpu_sharpen(void) {
    int p = __atomic_load_n(&sharpen_pct, __ATOMIC_ACQUIRE);
    if (p < 0) {
        p = gpu_sharpen_parse(getenv("YW4_SHARPEN"));
        __atomic_store_n(&sharpen_pct, p, __ATOMIC_RELEASE);
    }
    return p;
}

float gpu_sharpen_get(void) { return (float)gpu_sharpen() / 100.0f; }

void gpu_sharpen_set(float f) {
    int p = sharpen_snap((int)(f * 100.0f + 0.5f));
    __atomic_store_n(&sharpen_pct, p, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: sharpening %d%%\n", p);
}

/* The amount, 0..1, as FSR's sharpness: FSR's own sharpener (RCAS) takes it, and our pass (sharpen.glsl) is the
 * same method. The steps are FSR's measured ones (decision 0076: RCAS grows fast with its amount, so MAX is 0.7,
 * not 1.0). DLSS has its own, stronger steps: its picture starts much softer than FSR's (1080p school scene: 15-30%
 * less detail with SHARPENING OFF), so on FSR's steps DLSS's MAX only looked like FSR's MEDIUM (issue #11). Its steps
 * are measured so each looks about as sharp as FSR's at the same step; 0.85 is still clean, 1.0 rings.
 * Testing: YW4_SHARPEN_MAP / YW4_DLSS_SHARPEN_MAP=<low>,<medium>,<high>,<max> replace a table; YW4_TAA_SHARPEN /
 * YW4_FSR_SHARPNESS override the amount with any value for our pass (TAA, DLSS, off, FXAA, SMAA, text and menus) /
 * for FSR. */
enum { SHARPEN_OURS, SHARPEN_FSR, SHARPEN_DLSS };
static float sharpen_amount(int which) {
    static float taa_env = -2.0f, fsr_env = -2.0f;  /* -1: not set */
    static float level_map[5] = {0.0f, 0.25f, 0.5f, 0.6f, 0.7f};
    static float dlss_map[5] = {0.0f, 0.6f, 0.75f, 0.8f, 0.85f};
    if (taa_env < -1.5f) {
        const char *e = getenv("YW4_TAA_SHARPEN");
        taa_env = e && *e ? (float)atof(e) : -1.0f;
        e = getenv("YW4_FSR_SHARPNESS");
        fsr_env = e && *e ? (float)atof(e) : -1.0f;
        if ((e = getenv("YW4_SHARPEN_MAP")) != NULL)
            sscanf(e, "%f,%f,%f,%f", &level_map[1], &level_map[2], &level_map[3], &level_map[4]);
        if ((e = getenv("YW4_DLSS_SHARPEN_MAP")) != NULL)
            sscanf(e, "%f,%f,%f,%f", &dlss_map[1], &dlss_map[2], &dlss_map[3], &dlss_map[4]);
    }
    float env = which == SHARPEN_FSR ? fsr_env : taa_env;
    if (env >= 0.0f) return env;
    /* between two levels (none now: the slider stops only at them) a straight line from one's amount to the next */
    const float *map = which == SHARPEN_DLSS ? dlss_map : level_map;
    int p = gpu_sharpen();
    if (p <= 0) return 0.0f;
    int i = p / 25;
    if (i >= 4) return map[4];
    float t = (float)(p - i * 25) / 25.0f;
    return map[i] + (map[i + 1] - map[i]) * t;
}

/* SHARPEN TEXT AND MENUS (decision 0076): 0 only the 3D is sharpened (the default), 1 the game's UI too, at
 * the present (gpu_sharpen_ui_present); YW4_SHARPEN_UI at start, the settings panel switches it. */
static int sharpen_ui = -1;

int gpu_sharpen_ui(void) {
    int on = __atomic_load_n(&sharpen_ui, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("YW4_SHARPEN_UI");
        on = e && atoi(e) == 1;
        __atomic_store_n(&sharpen_ui, on, __ATOMIC_RELEASE);
    }
    return on;
}

void gpu_sharpen_ui_toggle(void) {
    int on = !gpu_sharpen_ui();
    __atomic_store_n(&sharpen_ui, on, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: sharpen text and menus %s\n", on ? "on" : "off");
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
                                 .subresourceRange = {fmt == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                  : VK_IMAGE_ASPECT_COLOR_BIT,
                                                      0, 1, 0, 1}};
    CHECK(vkCreateImageView(vk.device, &vci, NULL, &im->view));
    im->w = w, im->h = h;
}

/* Freed only once the GPU is done with every submission (a size change is rare: the settings panel). */
static void image_free(AaImage *im) {
    if (!im->image) return;
    if (getenv("YW4_VULKAN_VALIDATION"))  /* to match "destroyed while in use" errors to an image */
        fprintf(stderr, "yw4: aa: freeing a %ux%u image, view %#" PRIx64 "\n", im->w, im->h, (uint64_t)im->view);
    vkDestroyImageView(vk.device, im->view, NULL);
    vkDestroyImage(vk.device, im->image, NULL);
    vkFreeMemory(vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static void to_general_aspect(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image, .subresourceRange = {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

static void to_general(VkCommandBuffer cb, VkImage image) { to_general_aspect(cb, image, VK_IMAGE_ASPECT_COLOR_BIT); }

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
    image_make(&fsr_out, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);  /* DLSS clears it */
    to_general(cb, fsr_out.image);
}

/* ---- pipelines: one layout (5 sampled images, the size as a push constant), one pipeline per pass */

enum { P_ENCODE, P_FXAA, P_EDGE, P_WEIGHT, P_BLEND, P_SHOW_EDGES, P_SHOW_WEIGHTS, P_TEST, P_TAA, P_TAA_OUT, P_UPSCALE, P_DEPTH, P_UI_SHARPEN, PASSES };

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
                                                "aa_fullscreen.vert", "aa_fullscreen.vert", "aa_fullscreen.vert",
                                                "aa_fullscreen.vert"};
    static const char *const fs_name[PASSES] = {"aa_encode.frag",   "fxaa.frag",           "smaa_edge.frag",
                                                "smaa_weight.frag", "smaa_blend.frag",     "aa_show_edges.frag",
                                                "aa_show_weights.frag", "aa_test.frag", "taa_resolve.frag",
                                                "taa_output.frag", "aa_upscale.frag", "aa_depth_upscale.frag",
                                                "ui_sharpen.frag"};
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
    CHECK(vkCreateGraphicsPipelines(vk.device, gpu_pcache(), 1, &pci, NULL, &pipes[n].pipe));
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
static uint64_t scene_frame;       /* the GPU frame of the last 3D object's draw (gpu_draw.c) */

void gpu_note_model_draw(void) { __atomic_store_n(&scene_frame, gpu_frames(), __ATOMIC_RELAXED); }

/* A 3D object was drawn in the last 30 GPU frames. Without one (the title menu, videos, loading screens: 2D,
 * though the title still runs the lighting and post passes) FSR frame generation steps aside (gpu_fg_active),
 * so the frames are paced by FRAME RATE, VSYNC included; the margin keeps it from switching on and off around a
 * loading screen's odd 3D frame. */
int gpu_scene_recent(void) {
    uint64_t f = __atomic_load_n(&scene_frame, __ATOMIC_RELAXED);
    return f && gpu_frames() - f < 30;
}

/* A full-screen video (CRI Sofdec: the title menu's background, the opening movie) is on screen: the movie program
 * drew in the last 10 GPU frames and no 3D object in the last 30. framerate_wait then holds 30 frames/s, the
 * videos' own rate: at 60 on a 165 Hz display (2.75 refreshes a frame, no variable refresh) each picture stayed
 * 5 or 6 refreshes in an uneven order and the video stuttered; at 30 it alternates 5, 6, 5, 6 (PresentMon,
 * owner's laptop, 2026-11-18). */
static uint64_t movie_frame;  /* the GPU frame of the movie program's last draw (gpu_draw.c) */

void gpu_note_movie_draw(void) { __atomic_store_n(&movie_frame, gpu_frames(), __ATOMIC_RELAXED); }

int gpu_movie_screen(void) {
    uint64_t f = __atomic_load_n(&movie_frame, __ATOMIC_RELAXED);
    return f && gpu_frames() - f < 10 && !gpu_scene_recent();
}
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

static void dlss_jitter(uint32_t w, uint32_t ow, uint64_t n, float *jx, float *jy);  /* below */

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
    if (gpu_aa() == 5) {  /* DLSS: Halton 2, 3, as many positions as NVIDIA suggests: 8 x the upscale ratio squared */
        dlss_jitter(w, scene_ow ? scene_ow : w, gpu_frames(), jx, jy);
        return 1;
    }
    int i = (int)(gpu_frames() % 8) + 1;
    *jx = halton(i, 2) - 0.5f, *jy = halton(i, 3) - 0.5f;
    return 1;
}

/* This frame's 3D shift as an offset in the game's clip space (x right, y up; 2 = the scene's width / height),
 * for the full-screen passes that work out positions from the depth with the camera's unshifted inverse
 * projection (gpu_draw.c): without it their results (the shadow mask) stay where an unshifted picture would
 * have them, and the upscaler, taking the shift out of everything, moves them back and forth (shadows
 * vibrating sideways with DLSS's fast model, owner 2026-10-30). 0: no shift this frame. YW4_UNJITTER=0: off;
 * YW4_UNJITTER_SIGN=x,y (testing) flips the axes. */
int gpu_taa_clip_jitter(float *cx, float *cy) {
    static int on = -1;
    static float sx = 1.0f, sy = 1.0f;
    if (on < 0) {
        const char *e = getenv("YW4_UNJITTER");
        on = !e || atoi(e);
        if ((e = getenv("YW4_UNJITTER_SIGN")) != NULL) sscanf(e, "%f,%f", &sx, &sy);
    }
    if (!on || gpu_aa() < 3 || !scene_w || !scene_h) return 0;
    float jx, jy;
    if (gpu_aa() == 4 && gpu_fsr_available() == 1) {
        gpu_fsr_jitter(scene_w, scene_ow ? scene_ow : scene_w, gpu_frames(), &jx, &jy);
    } else if (gpu_aa() == 5) {
        dlss_jitter(scene_w, scene_ow ? scene_ow : scene_w, gpu_frames(), &jx, &jy);
    } else {
        int i = (int)(gpu_frames() % 8) + 1;
        jx = halton(i, 2) - 0.5f, jy = halton(i, 3) - 0.5f;
    }
    /* the viewport moved the 3D right by jx and down by jy pixels */
    *cx = sx * 2.0f * jx / (float)scene_w, *cy = sy * -2.0f * jy / (float)scene_h;
    return 1;
}

/* The view's camera block (vertex cbuf 1, CBUSE_UB_WORLD_MATRIX_IDX) as a G-buffer draw had it bound, for frame
 * generation's camera (fg_camera). Read like the other blocks we copy at bind time, from guest memory. Its rows
 * (YW4_CAMERA_LOG, night street 2026-11-19), each matrix for column vectors, OpenGL's clip (depth -1..1, the view
 * looking down -z), without the anti-aliasing's shift (that is a viewport shift):
 *   0-3 view, 4-7 projection (45 degrees, near 0.05, far 4000), 8-11 projection x view, 12-15 the view's inverse
 *   (column 3: the camera's position), 16-19 the projection's inverse, 20-23 (projection x view)'s inverse,
 *   24-35 last frame's view, projection, projection x view. */
enum { CAM_ROWS = 36 };
static float gbuf_cam[CAM_ROWS * 4], motion_cam[CAM_ROWS * 4];
static int gbuf_cam_valid, motion_cam_valid;  /* motion_cam: latched with the motion (gpu_taa_targets_end) */

/* A draw's targets: the G-buffer's (3 colour targets, the first RG16F: the game's motion) is noted. */
void gpu_taa_note_targets(const GpuState *s) {
    if (gpu_aa() < 3 || s->ncolor != 3 || !s->rt.color[0]) return;
    const GpuImage *g = image_find(s->rt.color[0]);
    if (!g || g->format != VK_FORMAT_R16G16_SFLOAT) return;
    gbuffer_motion = s->rt.color[0];
    const GpuRange *cam = &s->ubo[0][1];
    if (cam->addr && cam->size >= sizeof gbuf_cam) {
        memcpy(gbuf_cam, (const void *)(uintptr_t)cam->addr, sizeof gbuf_cam);
        gbuf_cam_valid = 1;
        static int logged = -1;  /* YW4_CAMERA_LOG: the block's rows once, to tell them apart */
        if (logged < 0) logged = getenv("YW4_CAMERA_LOG") ? 0 : 1;
        if (!logged && gpu_frames() > 3000) {
            logged = 1;
            for (int r = 0; r < CAM_ROWS; r++)
                fprintf(stderr, "yw4: camera: cbuf 1 row %2d: %12.6f %12.6f %12.6f %12.6f\n", r, gbuf_cam[r * 4],
                        gbuf_cam[r * 4 + 1], gbuf_cam[r * 4 + 2], gbuf_cam[r * 4 + 3]);
        }
    }
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
    motion_cam_valid = gbuf_cam_valid;  /* the camera that goes with this motion */
    if (gbuf_cam_valid) memcpy(motion_cam, gbuf_cam, sizeof motion_cam);
    gbuf_cam_valid = 0;
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
    /* SHARPENING after the blend (the debug views: none) */
    pass_params[0] = result == history[next].view ? sharpen_amount(SHARPEN_OURS) : 0.0f;
    pass(cb, P_TAA_OUT, target, fmt, w, h, 0, result);
    pass_params[0] = 0.0f;
    taa_cur = next, history_valid = 1;
}

static void dlss_jitter(uint32_t w, uint32_t ow, uint64_t n, float *jx, float *jy) {
    float r = w ? (float)ow / (float)w : 1.0f;
    int phases = (int)(8.0f * r * r + 0.5f);
    int i = (int)(n % (uint64_t)(phases > 0 ? phases : 8)) + 1;
    *jx = halton(i, 2) - 0.5f, *jy = halton(i, 3) - 0.5f;
}

/* The depth as FSR gets it (upscaler and frame generation). FSR's Vulkan backend reads the depth through a view
 * it makes itself in a depth-only format (D32_SFLOAT for D32S8, X8_D24 for D24S8), which Vulkan does not allow
 * on an image that also holds stencil (VUID-VkImageViewCreateInfo-image-01761). With D32S8, the stand-in for
 * D24S8 on AMD GPUs, that lost the device on NVIDIA (issue #8). A D32S8 depth goes to FSR as a plain D32_SFLOAT
 * copy instead: one full-screen pass, made once per depth image and frame. D24S8 (NVIDIA) keeps going in as it is:
 * as invalid, but it never showed a fault, and most players are there. */
static AaImage fsr_depth;
static void fsr_depth_of(VkCommandBuffer cb, GpuImage *d, VkImageView src, uint32_t w, uint32_t h, VkImage *image,
                         VkFormat *format) {
    *image = d->image, *format = d->format;
    /* D24S8 (NVIDIA): FSR's own X8_D24 view of it is invalid too, and it is the depth FSR's frame generation reads
     * as R32_UINT raw bits (ffx_api_vk.h): the copy makes it a plain float depth like on AMD. 2026-11-22.
     * YW4_FSR_D24_COPY=0: the old way (A/B). */
    static int d24_copy = -1;
    if (d24_copy < 0) d24_copy = !(getenv("YW4_FSR_D24_COPY") && *getenv("YW4_FSR_D24_COPY") == '0');
    int copy = d->format == VK_FORMAT_D32_SFLOAT_S8_UINT || (d24_copy && d->format == VK_FORMAT_D24_UNORM_S8_UINT);
    if (!src || !copy) return;
    static uint64_t copied_frame = ~0ull;
    static VkImage copied_from;
    if (!fsr_depth.image || fsr_depth.w != w || fsr_depth.h != h) {
        if (fsr_depth.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&fsr_depth);
        image_make(&fsr_depth, VK_FORMAT_D32_SFLOAT, w, h,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        to_general_aspect(cb, fsr_depth.image, VK_IMAGE_ASPECT_DEPTH_BIT);
        copied_frame = ~0ull;
    }
    uint64_t now = gpu_frames();
    if (copied_frame != now || copied_from != d->image) {
        full_barrier(cb);
        depth_pass(cb, fsr_depth.view, VK_FORMAT_D32_SFLOAT, w, h, src);  /* ends with a full barrier */
        copied_frame = now, copied_from = d->image;
    }
    *image = fsr_depth.image, *format = VK_FORMAT_D32_SFLOAT;
}

/* FSR in place of TAA's blend: the encoded scene, the scene's depth and the game's motion in (w x h), FSR's
 * result out (ow x oh: larger when upscaling) through the curve's inverse (no extra sharpening: FSR has its
 * own). 0: FSR could not run. */
static int fsr(VkCommandBuffer cb, GpuImage *d, VkImageView depth_in, VkImageView target, VkFormat fmt, uint32_t w,
               uint32_t h, uint32_t ow, uint32_t oh) {
    uint64_t now = gpu_frames();
    gpu_fsr_prepare(w, h, ow, oh);  /* made now if it isn't: before the first frame FSR can run (gpu_fsr.c) */
    if (!motion.image || motion_frame + 1 < now || motion_frame > now + 1) return 0;
    if (!d || d->width != w || d->height != h || motion.w != w || motion.h != h) return 0;
    output_image(cb, ow, oh);
    VkImage depth_image;
    VkFormat depth_format;
    fsr_depth_of(cb, d, depth_in, w, h, &depth_image, &depth_format);
    static uint64_t last = ~0ull;
    static uint32_t last_ow;
    static int64_t last_ns;
    int64_t t = host_now_ns();
    GpuFsrInputs in = {.color = encoded.image, .depth = depth_image, .motion = motion.image, .output = fsr_out.image,
                       .color_format = VK_FORMAT_R16G16B16A16_SFLOAT, .depth_format = depth_format,
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
    static float cnear = -1.0f, cfar = -1.0f;
    if (cnear < 0.0f) {
        cnear = getenv("YW4_FSR_NEAR") ? (float)atof(getenv("YW4_FSR_NEAR")) : 0.1f;
        cfar = getenv("YW4_FSR_FAR") ? (float)atof(getenv("YW4_FSR_FAR")) : 1000.0f;
    }
    /* SHARPENING as FSR's own sharpener's amount (0 turns it off, gpu_fsr.c) */
    in.sharpness = sharpen_amount(SHARPEN_FSR), in.cam_near = cnear, in.cam_far = cfar, in.fov_y = 0.785398f;  /* 45 degrees, town's */
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
    pass(cb, P_TAA_OUT, target, fmt, ow, oh, 0, fsr_out.view);  /* not sharpened again (pass_params cleared) */
    last = now, last_ow = ow, last_ns = t;
    history_valid = 0;  /* TAA's history is stale when it takes over again */
    return 1;
}

/* DLSS like FSR: the same inputs and output (gpu_dlss.c). 0: it could not run. */
static int dlss(VkCommandBuffer cb, GpuImage *d, VkImageView depth_view, VkImageView target, VkFormat fmt, uint32_t w,
                uint32_t h, uint32_t ow, uint32_t oh) {
    uint64_t now = gpu_frames();
    if (!motion.image || motion_frame + 1 < now || motion_frame > now + 1) return 0;
    if (!d || d->width != w || d->height != h || motion.w != w || motion.h != h || !depth_view) return 0;
    output_image(cb, ow, oh);
    static uint64_t last = ~0ull;
    static uint32_t last_ow;
    static int64_t last_ns;
    int64_t t = host_now_ns();
    GpuDlssInputs in = {.color = encoded.image, .depth = d->image, .motion = motion.image, .output = fsr_out.image,
                        .color_view = encoded.view, .depth_view = depth_view, .motion_view = motion.view,
                        .output_view = fsr_out.view, .color_format = VK_FORMAT_R16G16B16A16_SFLOAT,
                        .depth_format = d->format, .output_format = VK_FORMAT_R16G16B16A16_SFLOAT, .render_w = w,
                        .render_h = h, .output_w = ow, .output_h = oh, .quality = gpu_upscale(),
                        .reset = last + 1 != now || last_ow != ow,
                        .frame_ms = last_ns ? (float)(t - last_ns) / 1e6f : 16.7f};
    dlss_jitter(w, ow, now, &in.jitter[0], &in.jitter[1]);
    float sy = nvn_window_origin == 1 ? -1.0f : 1.0f, mx = 1.0f, my = 1.0f, jsx = 1.0f, jsy = 1.0f;
    const char *e = getenv("YW4_DLSS_MV_SIGN");  /* testing: "x,y" flips the motion */
    if (e) sscanf(e, "%f,%f", &mx, &my);
    e = getenv("YW4_DLSS_JITTER_SIGN");  /* testing: "x,y" flips the jitter told to DLSS */
    if (e) sscanf(e, "%f,%f", &jsx, &jsy);
    in.motion_scale[0] = -(float)w * mx, in.motion_scale[1] = -(float)h * sy * my;
    in.jitter[0] *= jsx, in.jitter[1] *= jsy;
    full_barrier(cb);
    gpu_passes_other(cb, "DLSS itself", 0);  /* the per-pass timer (F5): DLSS apart from our passes around it */
    if (!gpu_dlss_dispatch(cb, &in)) return 0;
    full_barrier(cb);
    gpu_passes_other(cb, "DLSS: its output written back (SHARPENING)", 0);
    pass_params[0] = sharpen_amount(SHARPEN_DLSS);  /* DLSS has none of its own any more: ours, on DLSS's steps */
    pass(cb, P_TAA_OUT, target, fmt, ow, oh, 0, fsr_out.view);
    pass_params[0] = 0.0f;
    last = now, last_ow = ow, last_ns = t;
    history_valid = 0;
    return 1;
}

/* Frame generation's prepare pass (gpu_fg.c): the 3D's depth and the game's motion, with the jitter this frame
 * was drawn with (as gpu_taa_jitter chose it) and the motion's scale as FSR's upscaler gets them. */
/* 4x4 matrices stored row by row, for column vectors: o = a x b. */
static void m4_mul(const float *a, const float *b, float *o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
static void m4_transpose(const float *m, float *o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[c * 4 + r] = m[r * 4 + c];
}
static int m4_invert(const float *m, float *o) {  /* Gauss-Jordan with pivoting; 0 when singular */
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return 0;
        for (int k = 0; k < 8; k++) {
            double t = a[c][k];
            a[c][k] = a[p][k], a[p][k] = t;
        }
        double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[r * 4 + c] = (float)a[r][c + 4];
    return 1;
}

/* Frame generation's camera from the block latched with the motion (motion_cam), instead of placeholders (near 0.1,
 * far 1000, no camera motion at all: DLSS's frame generation got identity clip-to-last-clip and a camera at the
 * origin; owner, 2026-11-19: a noisy outline around the characters in generated frames as the camera turns).
 * Streamline wants row vectors (the transposes), clip depth 0..1 and the view looking down +z: so the game's clip
 * goes through C = (y flipped with the upper-left window origin, as the motion's scale) x (z: 0.5 z + 0.5 w), and
 * its view through S = z flipped. YW4_FG_CAMERA=0: the placeholders (A/B). */
static void fg_camera(GpuFgPrepare *p) {
    static int on = -1;
    if (on < 0) on = !(getenv("YW4_FG_CAMERA") && *getenv("YW4_FG_CAMERA") == '0');
    static int why;  /* once: why the placeholders stayed */
    if (!on || !motion_cam_valid) {
        if (!why++) fprintf(stderr, "yw4: fg: no game camera (%s)\n", on ? "none with the motion" : "YW4_FG_CAMERA=0");
        return;
    }
    const float *cam = motion_cam, *view = cam, *proj = cam + 16, *inv_vp = cam + 80, *prev_vp = cam + 128;
    const float *inv_view = cam + 48;
    float a = proj[10], b = proj[11];  /* row 2: z' = a z + b w; near and far where z' / -z is -1 and 1 */
    if (a == 1.0f || a == -1.0f || proj[5] == 0.0f) return;
    float sy = nvn_window_origin == 1 ? -1.0f : 1.0f;
    const float C[16] = {1, 0, 0, 0, 0, sy, 0, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0, 1};
    const float S[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
    float c_inv[16], cp[16], v2c[16], c2v[16], t[16], t2[16], c2p[16], p2c[16];
    if (!m4_invert(C, c_inv)) return;
    m4_mul(C, proj, cp), m4_mul(cp, S, v2c);
    if (!m4_invert(v2c, c2v)) return;
    m4_mul(C, prev_vp, t), m4_mul(t, inv_vp, t2), m4_mul(t2, c_inv, c2p);  /* C x last VP x VP^-1 x C^-1 */
    if (!m4_invert(c2p, p2c)) return;
    m4_transpose(v2c, p->view_to_clip), m4_transpose(c2v, p->clip_to_view);
    m4_transpose(c2p, p->clip_to_prev_clip), m4_transpose(p2c, p->prev_clip_to_clip);
    for (int i = 0; i < 3; i++) {
        p->cam_pos[i] = inv_view[i * 4 + 3];
        p->cam_right[i] = view[i], p->cam_up[i] = view[4 + i], p->cam_fwd[i] = -view[8 + i];
    }
    p->cam_near = b / (a - 1.0f), p->cam_far = b / (a + 1.0f);
    p->fov_y = 2.0f * atanf(1.0f / fabsf(proj[5]));
    p->has_cam = 1;
    static int logged;
    if (!logged++)
        fprintf(stderr, "yw4: fg: the game's camera for frame generation: near %.3f, far %.1f, %.1f degrees\n",
                (double)p->cam_near, (double)p->cam_far, (double)(p->fov_y * 57.29578f));
}

static void fg_prepare(VkCommandBuffer cb, GpuImage *d, VkImageView dv, uint32_t w, uint32_t h, uint32_t ow,
                       uint32_t oh, GpuImage *scene) {
    int dlssg = gpu_fg() == 2 && dlssg_available();  /* DLSS's: tagged for Streamline instead */
    if (!gpu_fg_active() && !dlssg) return;
    uint64_t now = gpu_frames();
    if (!motion.image || motion_frame + 1 < now || motion_frame > now + 1) {
        gpu_fg_skip(FG_SKIP_MOTION);
        return;
    }
    if (!d || d->width != w || d->height != h || motion.w != w || motion.h != h) {
        gpu_fg_skip(FG_SKIP_SIZE);
        return;
    }
    static int64_t last_ns;
    int64_t t = host_now_ns();
    GpuFgPrepare p = {.depth = d->image, .motion = motion.image, .depth_view = dv, .motion_view = motion.view,
                      .scene = scene ? scene->image : VK_NULL_HANDLE, .scene_format = scene ? scene->format : 0,
                      .scene_w = scene ? scene->width : 0, .scene_h = scene ? scene->height : 0,
                      .depth_format = d->format, .render_w = w,
                      .render_h = h, .frame_ms = last_ns ? (float)(t - last_ns) / 1e6f : 16.7f, .cam_near = 0.1f,
                      .cam_far = 1000.0f, .fov_y = 0.785398f};
    fg_camera(&p);
    last_ns = t;
    int mode = gpu_aa();
    if (mode == 4 && gpu_fsr_available() == 1) {
        gpu_fsr_jitter(w, ow, now, &p.jitter[0], &p.jitter[1]);
    } else if (mode == 5) {
        dlss_jitter(w, ow, now, &p.jitter[0], &p.jitter[1]);
    } else {
        int i = (int)(now % 8) + 1;
        p.jitter[0] = halton(i, 2) - 0.5f, p.jitter[1] = halton(i, 3) - 0.5f;
    }
    float sy = nvn_window_origin == 1 ? -1.0f : 1.0f;
    p.motion_scale[0] = -(float)w, p.motion_scale[1] = -(float)h * sy;
    if (dlssg) {
        gpu_fg_hudless_copy(cb, &p), dlssg_tag(cb, &p, ow, oh);
    } else {  /* FSR's: a depth with stencil as a plain copy (fsr_depth_of) */
        fsr_depth_of(cb, d, dv, w, h, &p.depth, &p.depth_format);
        gpu_fg_prepare(cb, &p);
    }
}

/* ---- SHARPEN TEXT AND MENUS (decision 0076). The game draws its UI into the scene image after the anti-aliasing
 * point, then copies scene and UI into the window image that is presented. With the option on, the scene as it is at
 * the anti-aliasing point (sharpened already) is copied into an image like the window image, where the window shows
 * it (as frame generation's HUD-less copy, gpu_fg.c); at the present, the pixels of the window image that differ from
 * it (the UI) are sharpened, the others left alone, so the 3D isn't sharpened twice. Frames with no 3D (the title,
 * full-screen menus, videos) have no copy: all of them is sharpened. Our own overlay is drawn later (window.c). */
static AaImage ui_before, ui_copy;
static VkFormat ui_before_fmt, ui_copy_fmt;
static uint64_t ui_before_frame = ~0ull;  /* gpu_frames() when it was made: valid for the present that follows */
static VkFormat shown_fmt;                /* the last present's window image and the part of it the window shows */
static uint32_t shown_w, shown_h;
static VkRect2D shown_rect;

void gpu_aa_note_present(VkFormat format, uint32_t w, uint32_t h, VkRect2D rect) {
    shown_fmt = format, shown_w = w, shown_h = h, shown_rect = rect;
}

static int ui_sharpening(void) { return gpu_sharpen_ui() && gpu_sharpen(); }

static void ui_before_copy(VkCommandBuffer cb, GpuImage *scene) {
    VkRect2D r = shown_rect;
    if (!ui_sharpening() || !scene || !shown_fmt || scene->width != r.extent.width || scene->height != r.extent.height ||
        r.offset.x < 0 || r.offset.y < 0 || r.offset.x + r.extent.width > shown_w || r.offset.y + r.extent.height > shown_h)
        return;
    if (!ui_before.image || ui_before.w != shown_w || ui_before.h != shown_h || ui_before_fmt != shown_fmt) {
        if (ui_before.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&ui_before);
        image_make(&ui_before, shown_fmt, shown_w, shown_h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        to_general(cb, ui_before.image);
        ui_before_fmt = shown_fmt;
    }
    full_barrier(cb);
    int32_t x = r.offset.x, y = r.offset.y;
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)scene->width, (int32_t)scene->height, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{x, y, 0}, {x + (int32_t)scene->width, y + (int32_t)scene->height, 1}}};
    vkCmdBlitImage(cb, scene->image, VK_IMAGE_LAYOUT_GENERAL, ui_before.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   VK_FILTER_NEAREST);
    full_barrier(cb);
    ui_before_frame = gpu_frames();
}

void gpu_sharpen_ui_present(VkCommandBuffer cb, NvnTexture *t) {
    if (!ui_sharpening()) return;
    GpuImage *g = image_of(cb, t);
    uint32_t w, h;
    NvnTextureView none = {0};
    VkImageView target = g ? gpu_render_view(cb, t, &none, 0, &w, &h) : NULL;
    if (!target || !w || !h) return;
    layouts();
    lookup_textures(cb);
    if (!ui_copy.image || ui_copy.w != w || ui_copy.h != h || ui_copy_fmt != g->format) {
        if (ui_copy.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&ui_copy);
        image_make(&ui_copy, g->format, w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        to_general(cb, ui_copy.image);
        ui_copy_fmt = g->format;
    }
    gpu_passes_other(cb, "SHARPEN TEXT AND MENUS", 0);
    full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, ui_copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cb);
    int before = ui_before.image && ui_before_frame + 1 == gpu_frames() && ui_before.w == w && ui_before.h == h &&
                 ui_before_fmt == g->format;
    pass_params[0] = sharpen_amount(SHARPEN_OURS), pass_params[1] = before ? 1.0f : 0.0f;
    pass_with(cb, P_UI_SHARPEN, target, g->format, w, h, 0, ui_copy.view, before ? ui_before.view : ui_copy.view,
              ui_copy.view);
    memset(pass_params, 0, sizeof pass_params);
    gpu_passes_other(cb, "after SHARPEN TEXT AND MENUS (the present)", 0);  /* F5: its time ends here, not next frame */
    gpu_dynamic_forget();
}

void gpu_aa_apply(VkCommandBuffer cb, GpuState *s) {
    int mode = gpu_aa(), up = gpu_upscale() != 0, sharpen = gpu_sharpen() != 0;
    NvnTexture *t = s->ncolor ? s->rt.color[0] : NULL;
    if ((!mode && !up && !sharpen) || !t || gpu_upscaled(t)) return;
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
    static const char *const label[6] = {"anti-aliasing off: SHARPENING", "anti-aliasing: FXAA", "anti-aliasing: SMAA",
                                         "anti-aliasing: TAA", "anti-aliasing: FSR", "anti-aliasing: DLSS"};
    gpu_passes_other(cb, label[mode], 0);  /* its own line in the per-pass timer (F5) */
    pass(cb, debug == 3 ? P_TEST : P_ENCODE, encoded.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, scene);
    /* The swirl into a battle grows the view in nine steps of 32 x 18 (gpu.c zoom_chain), each a new size: DLSS and
     * FSR are made again for every size (~33 ms each, a stutter at every battle's start), so during those ~1 s our
     * TAA stands in; it only resizes its images. The settled battle view is DLSS's / FSR's again. */
    int zoom = g && gpu_zoom_step(g->key.width, g->key.height);
    if (mode == 5 && !debug && !zoom && depth && dlss(cb, depth, depth_in, out, fmt, w, h, ow, oh)) {
        /* done: DLSS wrote the output's size */
    } else if (mode >= 4 && !debug && !zoom && depth && fsr(cb, depth, depth_in, out, fmt, w, h, ow, oh)) {
        /* done: FSR wrote the output's size (also when DLSS can't run) */
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
        if (mode < 3 && sharpen && !debug) {  /* SHARPENING without TAA's pass: one of its own, TAA's last pass */
            gpu_passes_other(cb, "SHARPENING (anti-aliasing off, FXAA or SMAA)", 0);
            /* off: the scene is already encoded; FXAA / SMAA wrote theirs into the target, so encoded again */
            if (mode) pass(cb, P_ENCODE, encoded.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, scene);
            pass_params[0] = sharpen_amount(SHARPEN_OURS);
            pass(cb, P_TAA_OUT, target, fmt, w, h, 0, encoded.view);
            pass_params[0] = 0.0f;
        }
        if (out != target) {  /* upscaling without FSR: the anti-aliased 3D stretched to the output's size */
            pass(cb, P_ENCODE, encoded.view, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 0, scene);
            pass(cb, P_UPSCALE, out, fmt, ow, oh, 0, encoded.view);  /* bilinear over the whole image */
        }
    }
    if (mode >= 3 && !debug) {
        gpu_passes_other(cb, "frame generation: prepare (HUD-less copy)", 0);
        GpuImage *sc = image_of(cb, t);  /* the scene image now: output-sized when upscaling */
        fg_prepare(cb, depth, depth_in, w, h, ow, oh, sc);
    }
    if (!debug) ui_before_copy(cb, image_of(cb, t));  /* SHARPEN TEXT AND MENUS: the 3D before the UI */
    gpu_dynamic_forget();  /* our pipelines set viewport and scissor: the game's next draw sets its own again */
    gpu_passes_other(cb, "after anti-aliasing (the UI)", 0);
}
