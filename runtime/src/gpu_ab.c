/* Same-frame A/B of one of our passes (YW4_AB_AT=<GPU frame>[,...], YW4_AB_PASS=gbuffer|material):
 * in those frames the commands of the pass's layer are run twice from the same starting images,
 * once with our draws and once with the game's, and their colour and depth targets are compared
 * texel by texel. Nothing moves between the two, unlike alternating frames (YW4_GBUFFER_ALTERNATE),
 * where moving things differ anyway and can hide a fault.
 *
 * gbuffer.c records a marker (CMD_NATIVE with gpu_ab_native) when the pass's layer starts and
 * another when the next layer starts on that thread. gpu_execute hands markers to gpu_ab_marker:
 *   begin: remember where the commands go on and the executor's state; copy the bound targets, and
 *   later the ones the range binds (gpu_ab_targets).
 *   end, first time: copy the targets out (ours), put the copies back, restore the state, and run
 *   the commands again with the game's draws (gpu_ab_game); end, second time: copy them out
 *   (the game's) and go on. The game's result is what the frame keeps.
 * At present the two copies are compared and a line per target goes to the log. gpu_lock held. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_internal.h"

int gpu_ab_game;  /* the second run: the game's draws (gbuffer.c asks) */

enum { TARGETS = 9 };  /* up to 8 colour targets and depth */

typedef struct {
    GpuImage *g;
    VkImage saved;  /* the target as the layer found it */
    VkDeviceMemory saved_mem;
    VkBuffer out[2];  /* ours, the game's */
    VkDeviceMemory out_mem[2];
    uint32_t bytes, bpt;  /* size of a copy, bytes per texel */
    int depth;
} Target;

static struct {
    uint64_t exec;    /* the command list run the begin marker was in */
    int active, run;  /* a range is being compared; 0 ours, 1 the game's */
    uint64_t resume;  /* the record after the begin marker */
    GpuState *state;  /* the executor's state at the begin marker */
    Target t[TARGETS];
    int n;
    uint64_t frame;
    char pass[16];
} ab;

/* At present: 1 compare and free, 2 only free (a range that didn't finish) */
static int pending;

static int ab_frame(uint64_t f) {
    static uint64_t at[32];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("YW4_AB_AT");
        for (char *end; e && *e && n < 32; e = *end ? end + 1 : end) {
            at[n] = strtoull(e, &end, 10);
            if (end == e) break;
            n++;
        }
    }
    for (int i = 0; i < n; i++)
        if (at[i] == f) return 1;
    return 0;
}

int gpu_ab_wanted(const char *pass) {
    const char *p = getenv("YW4_AB_PASS");
    return getenv("YW4_AB_AT") && !strcmp(p ? p : "gbuffer", pass);
}

static uint32_t texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R8G8_UNORM: return 2;
    default: return 4;  /* RGBA8, R11G11B10, R16G16, R32 */
    }
}

static VkBuffer host_buffer(VkDeviceSize size, VkDeviceMemory *mem) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b;
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, b, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, mem));
    CHECK(vkBindBufferMemory(vk.device, b, *mem, 0));
    return b;
}

static void copy_image(VkCommandBuffer cb, const GpuImage *g, VkImage from, VkImage to) {
    VkImageCopy c = {.srcSubresource = {g->aspect, 0, 0, g->layers}, .dstSubresource = {g->aspect, 0, 0, g->layers},
                     .extent = {g->width, g->height, 1}};
    vkCmdCopyImage(cb, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cb);
}

static void copy_out(VkCommandBuffer cb, const Target *t, VkBuffer to) {
    VkImageAspectFlags a = t->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkBufferImageCopy c = {.imageSubresource = {a, 0, 0, t->g->layers}, .imageExtent = {t->g->width, t->g->height, 1}};
    vkCmdCopyImageToBuffer(cb, t->g->image, VK_IMAGE_LAYOUT_GENERAL, to, 1, &c);
    full_barrier(cb);
}

static void add_target(VkCommandBuffer cb, NvnTexture *tex, int depth) {
    if (!tex || ab.n >= TARGETS) return;
    GpuImage *g = image_of(cb, tex);
    for (int i = 0; i < ab.n; i++)
        if (ab.t[i].g == g) return;
    if (g->depth != 1) return;  /* 2D (array) targets only */
    Target *t = &ab.t[ab.n++];
    memset(t, 0, sizeof *t);
    t->g = g, t->depth = depth;
    t->bpt = depth ? (g->format == VK_FORMAT_D16_UNORM ? 2 : 4) : texel_bytes(g->format);
    t->bytes = g->width * g->height * g->layers * t->bpt;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = g->format, .extent = {g->width, g->height, 1}, .mipLevels = 1,
                             .arrayLayers = g->layers, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &t->saved));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, t->saved, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &t->saved_mem));
    CHECK(vkBindImageMemory(vk.device, t->saved, t->saved_mem, 0));
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = t->saved, .subresourceRange = {g->aspect, 0, 1, 0, g->layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
    for (int k = 0; k < 2; k++) t->out[k] = host_buffer(t->bytes, &t->out_mem[k]);
    copy_image(cb, g, g->image, t->saved);
}

/* gpu_execute, after SetRenderTargets: targets first bound inside the range are copied too (a layer
 * binds its own), before anything draws into them. */
void gpu_ab_targets(VkCommandBuffer cb, GpuState *s) {
    if (!ab.active || ab.run) return;
    for (int i = 0; i < s->ncolor && i < 8; i++) add_target(cb, s->rt.color[i], 0);
    add_target(cb, s->rt.depth, 1);
}

/* A marker's CMD_NATIVE function: gpu_execute hands markers to gpu_ab_marker instead. */
void gpu_ab_native(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    (void)vk_cb, (void)gpu_state, (void)data, (void)size;
}

/* exec: which command list run this is; next: the record after the marker. Returns 1 when the
 * executor must go on at *resume instead (the second run). */
int gpu_ab_marker(VkCommandBuffer cb, GpuState *s, const void *data, uint32_t size, uint64_t exec, uint64_t next,
                  uint64_t *resume) {
    char what = size ? *(const char *)data : 0;  /* 'b' begin, 'e' end; then the pass's name */
    if (ab.active && exec != ab.exec) {  /* the range ran into another command list: give up on it */
        fprintf(stderr, "yw4: ab: the %s layer of frame %" PRIu64 " ended in another command list: not compared\n",
                ab.pass, ab.frame);
        free(ab.state);
        ab.active = 0, gpu_ab_game = 0;
        pending = 2;  /* the copies are freed after the GPU, at present */
        return 0;
    }
    if (what == 'b') {
        if (ab.active || pending || !ab_frame(gpu_frames())) return 0;
        gpu_end_rendering(cb, s);
        memset(&ab, 0, sizeof ab);
        ab.active = 1, ab.resume = next, ab.frame = gpu_frames(), ab.exec = exec;
        snprintf(ab.pass, sizeof ab.pass, "%.*s", (int)(size > 1 ? size - 1 : 0), (const char *)data + 1);
        if (!(ab.state = malloc(sizeof *ab.state))) return ab.active = 0, 0;
        *ab.state = *s;
        for (int i = 0; i < s->ncolor && i < 8; i++) add_target(cb, s->rt.color[i], 0);
        add_target(cb, s->rt.depth, 1);
        return 0;
    }
    if (what != 'e' || !ab.active) return 0;
    gpu_end_rendering(cb, s);
    for (int i = 0; i < ab.n; i++) copy_out(cb, &ab.t[i], ab.t[i].out[ab.run]);
    if (ab.run == 0) {  /* again, from the same images and state, with the game's draws */
        for (int i = 0; i < ab.n; i++) copy_image(cb, ab.t[i].g, ab.t[i].saved, ab.t[i].g->image);
        *s = *ab.state;
        ab.run = 1;
        gpu_ab_game = 1;
        *resume = ab.resume;
        return 1;
    }
    gpu_ab_game = 0;
    ab.active = 0;
    free(ab.state);
    pending = 1;
    return 0;
}

int gpu_ab_pending(void) { return pending; }

/* At present, after the GPU is done: compare and free. */
void gpu_ab_flush(void) {
    if (!pending) return;
    for (int i = 0; i < ab.n; i++) {
        Target *t = &ab.t[i];
        if (pending == 1) {
            uint8_t *a, *b;
            CHECK(vkMapMemory(vk.device, t->out_mem[0], 0, VK_WHOLE_SIZE, 0, (void **)&a));
            CHECK(vkMapMemory(vk.device, t->out_mem[1], 0, VK_WHOLE_SIZE, 0, (void **)&b));
            long differ = 0, first = -1;
            /* 24-bit depth comes out in 32 bits whose top 8 are undefined */
            int d24 = t->depth && (t->g->format == VK_FORMAT_D24_UNORM_S8_UINT || t->g->format == VK_FORMAT_X8_D24_UNORM_PACK32);
            for (uint32_t k = 0; k < t->bytes / t->bpt; k++)
                if (d24 ? (((const uint32_t *)a)[k] ^ ((const uint32_t *)b)[k]) & 0xFFFFFF
                        : memcmp(a + (size_t)k * t->bpt, b + (size_t)k * t->bpt, t->bpt)) {
                    if (first < 0) first = k;
                    differ++;
                }
            fprintf(stderr, "yw4: ab: %s frame %" PRIu64 " %s %d (%ux%u, format %d): %ld of %u texels differ between"
                    " ours and the game's", ab.pass, ab.frame, t->depth ? "depth" : "colour", i, t->g->width,
                    t->g->height, (int)t->g->format, differ, t->g->width * t->g->height * t->g->layers);
            if (first >= 0) fprintf(stderr, " (first at %ld, %ld)", first % t->g->width, first / t->g->width);
            fprintf(stderr, "\n");
            vkUnmapMemory(vk.device, t->out_mem[0]), vkUnmapMemory(vk.device, t->out_mem[1]);
        }
        for (int k = 0; k < 2; k++) vkDestroyBuffer(vk.device, t->out[k], NULL), vkFreeMemory(vk.device, t->out_mem[k], NULL);
        vkDestroyImage(vk.device, t->saved, NULL), vkFreeMemory(vk.device, t->saved_mem, NULL);
    }
    ab.n = 0;
    pending = 0;
}
