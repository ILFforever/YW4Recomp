/* The draw path: NVN command lists run on Vulkan (decision 0021).
 *
 * Memory: every memory pool is imported with VK_EXT_external_memory_host and covered by
 * one VkBuffer, so an NVN buffer address is (that buffer, offset) and the GPU reads and
 * writes the game's memory in place. Textures are VkImages, one per NVN texture, made at
 * first use; those in CPU-no-access pools (render targets) are only ever written by the GPU.
 *
 * Execution: gpu_execute records one Vulkan command buffer per NVN command list and submits
 * it, signalling a timeline semaphore with the submission's number; NVN syncs wait for
 * numbers. Every image stays in layout GENERAL and a full memory barrier separates the
 * operations: simple and correct; finer barriers when profiling asks for them.
 *
 * Step 1 (0021): clears and present. Draws and dispatches are counted, not run yet.
 *
 * The NVN -> Vulkan format table follows LAS Recomp (runtime/nvn/vulkan/textures.c; MIT, see
 * third_party/lasrecomp).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "host.h"
#include "vk.h"
#include "window.h"

static int ready;
static HostLock gpu_lock = HOST_LOCK_INIT;  /* recording and submitting */
static VkCommandPool cmd_pool;
static VkSemaphore timeline;
static uint64_t submitted;  /* number of the last submission */

enum { RING = 16 };
static struct {
    VkCommandBuffer cb;
    uint64_t seq;  /* the submission that last used it */
} ring[RING];
static int ring_next;

static _Noreturn void gpu_fatal(const char *what, VkResult r) {
    fprintf(stderr, "yw4: gpu: %s failed (VkResult %d)\n", what, (int)r);
    exit(3);
}
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) gpu_fatal(#x, r_); } while (0)

int gpu_ready(void) { return ready; }

void gpu_init(void) {
    if (!vk.host_import) return;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = vk.queue_family};
    CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &cmd_pool));
    for (int i = 0; i < RING; i++) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(vk.device, &ai, &ring[i].cb));
    }
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    CHECK(vkCreateSemaphore(vk.device, &sci, NULL, &timeline));
    ready = 1;
    fprintf(stderr, "yw4: gpu: draw path on (decision 0021)\n");
}

/* ================================================================== submissions */

uint64_t gpu_submitted(void) { return __atomic_load_n(&submitted, __ATOMIC_ACQUIRE); }

static uint64_t completed(void) {
    uint64_t v = 0;
    CHECK(vkGetSemaphoreCounterValue(vk.device, timeline, &v));
    return v;
}

int gpu_wait(uint64_t seq, uint64_t timeout_ns) {
    if (!ready || completed() >= seq) return 0;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &timeline, .pValues = &seq};
    VkResult r = vkWaitSemaphores(vk.device, &wi, timeout_ns);
    if (r == VK_TIMEOUT) return 2;
    if (r != VK_SUCCESS) gpu_fatal("vkWaitSemaphores", r);
    return 1;
}

/* The next command buffer of the ring, begun; waits if the GPU still runs it. gpu_lock held. */
static VkCommandBuffer begin(void) {
    int i = ring_next;
    ring_next = (ring_next + 1) % RING;
    gpu_wait(ring[i].seq, UINT64_MAX);
    CHECK(vkResetCommandBuffer(ring[i].cb, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(ring[i].cb, &bi));
    ring[i].seq = submitted + 1;
    return ring[i].cb;
}

static void submit(VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    uint64_t seq = submitted + 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &seq};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = 1,
                       .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    host_lock(&vk.queue_lock);
    VkResult r = vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    host_unlock(&vk.queue_lock);
    if (r != VK_SUCCESS) gpu_fatal("vkQueueSubmit", r);
    __atomic_store_n(&submitted, seq, __ATOMIC_RELEASE);
}

/* Everything before it, in any stage, finishes and is visible to everything after. */
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                         0, NULL);
}

/* ================================================================== memory pools */

typedef struct {
    VkDeviceMemory mem;
    VkBuffer buf;
} GpuPool;

void gpu_pool_init(NvnPool *p) {
    if (!ready || !p->size) return;
    void *host = (void *)(uintptr_t)p->storage;
    if (p->storage % vk.host_import_align || p->size % vk.host_import_align) {
        fprintf(stderr, "yw4: gpu: pool %#" PRIx64 " + %#" PRIx64 " is not aligned for import\n", p->storage, p->size);
        exit(3);
    }
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(vkGetMemoryHostPointerPropertiesEXT(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                              &hp));
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = p->size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
    GpuPool *g = calloc(1, sizeof *g);
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &g->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, g->buf, &mr);
    uint32_t type = vk_memory_type(mr.memoryTypeBits & hp.memoryTypeBits, 0);
    if (type == UINT32_MAX) {
        fprintf(stderr, "yw4: gpu: no memory type to import pool %#" PRIx64 "\n", p->storage);
        exit(3);
    }
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                .allocationSize = p->size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &g->mem));
    CHECK(vkBindBufferMemory(vk.device, g->buf, g->mem, 0));
    p->gpu = g;
}

void gpu_pool_finalize(NvnPool *p) {
    GpuPool *g = p->gpu;
    if (!g) return;
    gpu_wait(gpu_submitted(), UINT64_MAX);
    vkDestroyBuffer(vk.device, g->buf, NULL);
    vkFreeMemory(vk.device, g->mem, NULL);
    free(g);
    p->gpu = NULL;
}

/* ================================================================== textures */

static VkFormat vk_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;
    case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x33: return VK_FORMAT_D32_SFLOAT;  /* DEPTH24 */
    case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    /* RGBX / BGRX: alpha unused (reads as 1: to do with the sampler views) */
    case 0x37: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x71: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x76: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x98: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9A: return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9B: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkFormat format;
    VkImageAspectFlags aspect;
    uint32_t width, height, depth, layers, levels;
} GpuImage;

static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* t's image, made (and cleared to zero) on first use. cb: where its first barrier goes. */
static GpuImage *image_of(VkCommandBuffer cb, NvnTexture *t) {
    if (t->gpu) return t->gpu;
    VkFormat fmt = vk_format(t->format);
    if (fmt == VK_FORMAT_UNDEFINED || t->target == TARGET_BUFFER || t->samples > 1) {
        fprintf(stderr, "yw4: gpu: texture format %#x target %u samples %u not supported yet\n", t->format, t->target,
                t->samples);
        exit(3);
    }
    GpuImage *g = calloc(1, sizeof *g);
    g->format = fmt;
    g->aspect = aspect_of(fmt);
    g->width = t->layout.width, g->height = t->layout.height, g->depth = t->layout.depth;
    g->layers = t->layout.layers ? t->layout.layers : 1, g->levels = t->layout.levels ? t->layout.levels : 1;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, fmt, &fp);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    int cube = t->target == TARGET_CUBE || t->target == TARGET_CUBE_ARRAY;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
        .imageType = t->target == TARGET_3D ? VK_IMAGE_TYPE_3D
                     : t->target == TARGET_1D || t->target == TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D
                                                                                : VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = {g->width, g->height, g->depth}, .mipLevels = g->levels, .arrayLayers = g->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &g->image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, g->image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &g->mem));
    CHECK(vkBindImageMemory(vk.device, g->image, g->mem, 0));
    VkImageSubresourceRange all = {g->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g->image, .subresourceRange = all};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
    if (g->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue zero = {{0}};
        vkCmdClearColorImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    } else {
        VkClearDepthStencilValue zero = {0};
        vkCmdClearDepthStencilImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    }
    full_barrier(cb);
    t->gpu = g;
    return g;
}

void gpu_texture_finalize(NvnTexture *t) {
    GpuImage *g = t ? t->gpu : NULL;
    if (!g) return;
    gpu_wait(gpu_submitted(), UINT64_MAX);
    vkDestroyImage(vk.device, g->image, NULL);
    vkFreeMemory(vk.device, g->mem, NULL);
    free(g);
    t->gpu = NULL;
}

/* The level and layers a render-target view selects (all layers of level 0 without a view). */
static VkImageSubresourceRange target_range(const GpuImage *g, const NvnTextureView *v, int has_view) {
    VkImageSubresourceRange r = {g->aspect, 0, 1, 0, g->layers};
    if (has_view && v->setmask & 1) r.baseMipLevel = v->base_level;
    if (has_view && v->setmask & 2) r.baseArrayLayer = v->min_layer, r.layerCount = v->num_layers ? v->num_layers : 1;
    return r;
}

/* ================================================================== command lists */

typedef struct {
    RenderTargets rt;
    int ncolor;
} State;

void gpu_execute(uint64_t handle) {
    host_lock(&gpu_lock);
    VkCommandBuffer cb = begin();
    full_barrier(cb);  /* after the previous submission, and the CPU's writes before it */
    State s = {0};
    uint64_t at = handle;
    for (int n = 0;; n++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)at;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size & 7 || n > 10000000) {
            fprintf(stderr, "yw4: gpu: command list %#" PRIx64 " is broken at %#" PRIx64 " (op %u, size %u)\n", handle,
                    at, r->op, r->size);
            exit(3);
        }
        if (r->op == CMD_END) break;
        if (r->op == CMD_JUMP) {
            at = r->x[0];
            continue;
        }
        switch (r->op) {
        case CMD_SET_RENDER_TARGETS:
            memcpy(&s.rt, r->extra, sizeof s.rt);
            s.ncolor = (int)r->x[0];
            break;
        case CMD_CLEAR_COLOR: {  /* (index, colour, mask) */
            int i = (int)r->x[0];
            NvnTexture *t = i < s.ncolor ? s.rt.color[i] : NULL;
            if (!t) break;
            if ((r->x[2] & 0xF) != 0xF) {
                static int logged;
                if (!logged++) fprintf(stderr, "yw4: gpu: clear with channel mask %#x: whole texels for now\n",
                                       (unsigned)r->x[2]);
            }
            GpuImage *g = image_of(cb, t);
            VkClearColorValue color;
            memcpy(color.float32, r->extra, 16);
            VkImageSubresourceRange range = target_range(g, &s.rt.views[i], s.rt.has_view[i]);
            vkCmdClearColorImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
            full_barrier(cb);
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL: {  /* (depth in s0; depth mask, stencil, stencil mask in x1..x3) */
            NvnTexture *t = s.rt.depth;
            if (!t) break;
            GpuImage *g = image_of(cb, t);
            VkImageSubresourceRange range = target_range(g, &s.rt.depth_view, s.rt.has_depth_view);
            range.aspectMask = 0;
            if (r->x[0] && g->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if ((r->x[2] & 0xFF) && g->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (!range.aspectMask) break;
            VkClearDepthStencilValue v = {.depth = r->f[0], .stencil = (uint32_t)r->x[1] & 0xFF};
            vkCmdClearDepthStencilImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            full_barrier(cb);
            break;
        }
        case CMD_REPORT_COUNTER: {  /* (counter, address): NVNcounterData {u64 value, u64 GPU ticks at 614.4 MHz} */
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            out[0] = 0;
            out[1] = (uint64_t)((unsigned __int128)(uint64_t)host_now_ns() * 384 / 625);
            break;
        }
        }
        at += r->size;
    }
    submit(cb);
    host_unlock(&gpu_lock);
}

/* ================================================================== present */

void gpu_present(NvnTexture *t, uint64_t frame) {
    host_lock(&gpu_lock);
    GpuImage *g = t->gpu;
    if (!g) {  /* never drawn to: make it (black) */
        VkCommandBuffer cb = begin();
        g = image_of(cb, t);
        submit(cb);
    }
    host_unlock(&gpu_lock);
    window_present_image(g->image, g->format, g->width, g->height, frame);
}
