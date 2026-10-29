/* Frame generation with AMD FSR (FidelityFX SDK 2.4.0, amd_fidelityfx_framegeneration_vk.dll, loaded through
 * amd_fidelityfx_loader_vk.dll like the upscaler in gpu_fsr.c; MIT, fetched by tools/yw4/deps.py).
 *
 * Without the SDK's own swapchain (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY): the window keeps
 * presenting as before, so our HUD and settings panel are drawn sharp onto every frame shown.
 * - After the anti-aliasing (gpu_aa.c) the 3D's depth and the game's motion go to FSR's prepare pass.
 * - At the present (gpu.c) the game's finished image is copied into one of two slots, and FSR makes the frame
 *   between the last one and it into the same slot.
 * - A pacing thread presents the generated frame at once and the game's half a frame later (half the measured
 *   time between the game's frames), so the main thread never waits for that half frame.
 * The generated frame is made from the whole picture, menus included (the game draws them into the 3D's image).
 * YW4_FG=0 off / 1 FSR / 2 DLSS (gpu_dlssg.cpp, through Streamline) at start; the settings panel cycles it.
 * Windows only for now. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_dlssg.h"
#include "gpu_fg.h"
#include "gpu_fsr.h"
#include "gpu_internal.h"
#include "host.h"
#include "window.h"

static int fg_mode = -1;

int gpu_fg(void) {
    int m = __atomic_load_n(&fg_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("YW4_FG");
        m = e ? atoi(e) : 0;
        if (m < 0 || m > 2) m = 0;
        __atomic_store_n(&fg_mode, m, __ATOMIC_RELEASE);
    }
    return m;
}

/* YW4_FG_AT=<frame>:<mode>,...: tests switch frame generation at those frames, as the panel would */
void gpu_fg_test_switch(uint64_t frame) {
    const char *e = getenv("YW4_FG_AT");
    for (const char *p = e; p && *p;) {
        char *end;
        unsigned long long f = strtoull(p, &end, 10);
        if (*end != ':') return;
        int m = atoi(end + 1);
        if (f == frame && m >= 0 && m <= 2) {
            __atomic_store_n(&fg_mode, m, __ATOMIC_RELEASE);
            fprintf(stderr, "yw4: gpu: frame generation %d at frame %llu (YW4_FG_AT)\n", m, f);
        }
        p = strchr(end, ',');
        if (p) p++;
    }
}

void gpu_fg_cycle(void) {
    int m = (gpu_fg() + 1) % 3;
    if (m == 2 && !dlssg_available()) m = 0;  /* DLSS's needs an RTX 40 or newer (gpu_dlssg.cpp) */
    __atomic_store_n(&fg_mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: frame generation %s\n", m == 2 ? "DLSS" : m == 1 ? "FSR" : "off");
}

#ifdef _WIN32
#include <windows.h>
#ifndef _WINDOWS
#define _WINDOWS  /* ffx_api_loader.h looks for MSBuild's name for Windows (as gpu_fsr.c) */
#endif

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_api_types.h"
#include "vk/ffx_api_vk.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"  /* MSVC's warning pragmas in the header */
#include "../../framegeneration/include/ffx_framegeneration.h"
#pragma clang diagnostic pop

static ffxFunctions fx;
static int loaded = -1;  /* -1 not tried, 0 failed, 1 loaded */
static ffxContext ctx;
static uint32_t ctx_dw, ctx_dh, ctx_rw, ctx_rh;
static VkFormat ctx_fmt;
static int ctx_failed;

static int available(void) {
    if (loaded >= 0) return loaded;
    HMODULE m = LoadLibraryA("amd_fidelityfx_loader_vk.dll");
    if (!m) {
        fprintf(stderr, "yw4: fg: amd_fidelityfx_loader_vk.dll not found next to yw4.exe: no frame generation\n");
        return loaded = 0;
    }
    ffxLoadFunctions(&fx, m);
    if (!fx.CreateContext || !fx.Dispatch || !fx.Configure || !fx.DestroyContext) {
        fprintf(stderr, "yw4: fg: the loader DLL lacks its functions: no frame generation\n");
        return loaded = 0;
    }
    return loaded = 1;
}

int gpu_fg_active(void) { return gpu_fg() == 1 && gpu_aa() >= 3 && available() == 1; }

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R32_UINT;  /* as ffx_api_vk.h maps them */
    case VK_FORMAT_D16_UNORM: return FFX_API_SURFACE_FORMAT_R16_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

/* Our images and the game's stay in layout GENERAL: FSR's unordered-access state (as gpu_fsr.c). */
static struct FfxApiResource resource(VkImage image, VkFormat f, uint32_t w, uint32_t h, uint32_t usage) {
    struct FfxApiResource r = {.resource = (void *)image, .state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS};
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = surface_format(f);
    r.description.width = w, r.description.height = h, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = usage;
    return r;
}

static void destroy(void) {
    if (!ctx) return;
    gpu_wait(gpu_submitted(), UINT64_MAX);
    fx.DestroyContext(&ctx, NULL);
    ctx = NULL;
}

/* A context for frames of dw x dh in fmt from a 3D of up to rw x rh. */
static int create(uint32_t dw, uint32_t dh, uint32_t rw, uint32_t rh, VkFormat fmt) {
    if (ctx && ctx_dw == dw && ctx_dh == dh && ctx_rw == rw && ctx_rh == rh && ctx_fmt == fmt) return 1;
    if (!ctx && ctx_failed && ctx_dw == dw && ctx_dh == dh && ctx_rw == rw && ctx_rh == rh && ctx_fmt == fmt) return 0;
    destroy();
    ctx_dw = dw, ctx_dh = dh, ctx_rw = rw, ctx_rh = rh, ctx_fmt = fmt, ctx_failed = 0;
    if (surface_format(fmt) == FFX_API_SURFACE_FORMAT_UNKNOWN) {
        fprintf(stderr, "yw4: fg: presented format %d not handled: no frame generation\n", (int)fmt);
        ctx_failed = 1;
        return 0;
    }
    struct ffxCreateBackendVKDesc backend = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
        .deviceContext = {.vkDevice = vk.device, .vkPhysicalDevice = vk.gpu, .vkDeviceProcAddr = vkGetDeviceProcAddr}};
    struct ffxCreateContextDescFrameGeneration desc = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, .pNext = &backend.header},
        .displaySize = {dw, dh}, .maxRenderSize = {rw, rh}, .backBufferFormat = surface_format(fmt)};
    struct ffxCreateContextDescFrameGenerationHudless hl = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS}, .hudlessBackBufferFormat = surface_format(fmt)};
    backend.header.pNext = &hl.header;  /* the HUD-less copy (prepare): FSR keeps the UI still */
    if (getenv("YW4_FG_DEBUG")) desc.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    if (getenv("YW4_FG_FLAGS")) desc.flags |= (uint32_t)atoi(getenv("YW4_FG_FLAGS"));  /* tests: e.g. 8 depth inverted */
    int64_t t0 = host_now_ns();
    ffxReturnCode_t r = fx.CreateContext(&ctx, &desc.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        fprintf(stderr, "yw4: fg: context for %ux%u (3D %ux%u) failed (%u): no frame generation\n", dw, dh, rw, rh,
                (unsigned)r);
        ctx = NULL, ctx_failed = 1;
        return 0;
    }
    struct ffxQueryGetProviderVersion qv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    fx.Query(&ctx, &qv.header);
    fprintf(stderr, "yw4: fg: %s for %ux%u (3D up to %ux%u) ready in %.0f ms\n",
            qv.versionName ? qv.versionName : "frame generation", dw, dh, rw, rh, (double)(host_now_ns() - t0) / 1e6);
    return 1;
}

static uint32_t debug_flags(void) {
    static int d = -1;  /* YW4_FG_DEBUG=1: FSR's tear and reset marks on the generated frames; 2: its debug view */
    if (d < 0) d = getenv("YW4_FG_DEBUG") ? atoi(getenv("YW4_FG_DEBUG")) : 0;
    return (d == 1 ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_RESET_INDICATORS
                   : 0) |
           (d == 2 ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0);
}

/* ---- prepare (after the anti-aliasing) */

static uint32_t want_rw, want_rh;  /* the 3D's size last prepared (or asked for: the context follows at present) */
static uint64_t next_id = 1, prepared_id;  /* FSR's frame identifier; prepared_id: this frame's, 0 if not */
static int prepared_reset;

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
} FgImage;

static void image_make(FgImage *im, VkFormat fmt, uint32_t w, uint32_t h);  /* below */
static void image_free(FgImage *im);
static void to_general(VkCommandBuffer cb, VkImage image);
static VkFormat raw_format(VkFormat f);

/* The HUD-less copy: the picture before the UI, blitted into the presented format (the conversion the game's own
 * copy into the window image makes), so FSR can tell the UI from the 3D and keep it still. */
static FgImage hudless;
static VkFormat hudless_fmt, presented_fmt;
static uint32_t hudless_w, hudless_h;
static int hudless_ok;  /* made this frame */
static VkRect2D shown_rect;  /* the part of the presented image the window shows (the game's crop), last frame */

static void hudless_copy(VkCommandBuffer cb, const GpuFgPrepare *p) {
    hudless_ok = 0;
    /* the presented image is the window texture: the picture sits in it where the crop takes it from */
    VkRect2D r = shown_rect;
    if (!p->scene || !presented_fmt || p->scene_w != r.extent.width || p->scene_h != r.extent.height ||
        r.offset.x < 0 || r.offset.y < 0 || r.offset.x + r.extent.width > ctx_dw || r.offset.y + r.extent.height > ctx_dh)
        return;
    static int off = -1;
    if (off < 0) off = getenv("YW4_FG_HUDLESS") && *getenv("YW4_FG_HUDLESS") == '0';
    if (off) return;
    if (!hudless.image || hudless_fmt != presented_fmt || hudless_w != ctx_dw || hudless_h != ctx_dh) {
        if (hudless.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&hudless);
        image_make(&hudless, presented_fmt, ctx_dw, ctx_dh);
        to_general(cb, hudless.image);
        hudless_fmt = presented_fmt, hudless_w = ctx_dw, hudless_h = ctx_dh;
    }
    full_barrier(cb);
    int32_t x = r.offset.x, y = r.offset.y;
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)p->scene_w, (int32_t)p->scene_h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{x, y, 0}, {x + (int32_t)p->scene_w, y + (int32_t)p->scene_h, 1}}};
    vkCmdBlitImage(cb, p->scene, VK_IMAGE_LAYOUT_GENERAL, hudless.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);
    hudless_ok = 1;
}

static int configure(uint64_t id, uint32_t w, uint32_t h) {
    struct ffxConfigureDescFrameGeneration c = {.header = {.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION}};
    c.frameGenerationEnabled = true;
    if (hudless_ok) c.HUDLessColor = resource(hudless.image, raw_format(hudless_fmt), w, h, FFX_API_RESOURCE_USAGE_READ_ONLY);
    c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY | debug_flags();
    c.generationRect.left = 0, c.generationRect.top = 0;
    c.generationRect.width = (int32_t)w, c.generationRect.height = (int32_t)h;
    c.frameID = id;
    return fx.Configure(&ctx, &c.header) == FFX_API_RETURN_OK;
}

void gpu_fg_prepare(VkCommandBuffer cb, const GpuFgPrepare *p) {
    prepared_id = 0;
    if (!gpu_fg_active()) return;
    want_rw = p->render_w, want_rh = p->render_h;
    if (!ctx || ctx_rw != p->render_w || ctx_rh != p->render_h) return;  /* made at the next present */
    uint64_t id = next_id;
    hudless_copy(cb, p);
    if (!configure(id, ctx_dw, ctx_dh)) return;
    struct ffxDispatchDescFrameGenerationPrepareV2 d = {.header = {.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2}};
    d.frameID = id, d.flags = debug_flags(), d.commandList = cb;
    d.renderSize.width = p->render_w, d.renderSize.height = p->render_h;
    float mx = 1.0f, my = 1.0f, jx = 1.0f, jy = 1.0f;  /* tests: YW4_FG_MV_SIGN / YW4_FG_JITTER_SIGN=x,y scale them */
    if (getenv("YW4_FG_MV_SIGN")) sscanf(getenv("YW4_FG_MV_SIGN"), "%f,%f", &mx, &my);
    if (getenv("YW4_FG_JITTER_SIGN")) sscanf(getenv("YW4_FG_JITTER_SIGN"), "%f,%f", &jx, &jy);
    d.jitterOffset.x = p->jitter[0] * jx, d.jitterOffset.y = p->jitter[1] * jy;
    d.motionVectorScale.x = p->motion_scale[0] * mx, d.motionVectorScale.y = p->motion_scale[1] * my;
    d.frameTimeDelta = p->frame_ms;
    d.reset = prepared_reset;
    d.cameraNear = p->cam_near, d.cameraFar = p->cam_far, d.cameraFovAngleVertical = p->fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.depth = resource(p->depth, p->depth_format, p->render_w, p->render_h, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors = resource(p->motion, VK_FORMAT_R16G16_SFLOAT, p->render_w, p->render_h, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.cameraForward[2] = 1.0f, d.cameraUp[1] = 1.0f, d.cameraRight[0] = 1.0f;  /* not known: a plain frame */
    full_barrier(cb);
    ffxReturnCode_t r = fx.Dispatch(&ctx, &d.header);
    full_barrier(cb);
    if (r != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 4) fprintf(stderr, "yw4: fg: prepare failed (%u)\n", (unsigned)r);
        return;
    }
    prepared_id = id;
}

/* ---- slots: the game's frame and the generated one, until the pacing thread has shown them */

typedef struct {
    FgImage real, gen;
    VkFormat fmt;
    uint32_t w, h;
    int busy, has_gen;
} Slot;

static Slot slots[2];
static int next_slot;
static HostLock plock;
static HostCond pcond;
static int queued = -1, showing = -1;  /* the slot waiting for the pacing thread, the one it shows */
static VkRect2D queued_rect;
static uint64_t queued_frame;
static int64_t queued_ns;
static int pacer_started;

/* The format FSR is told and views our images in: an sRGB one's UNORM twin, so FSR reads and writes the
 * encoded bytes (its sRGB transfer function) and can write them at all (sRGB formats have no storage use). */
static VkFormat raw_format(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f;
}

/* In the presented format (the window's copy decodes it as it does the game's), viewable in raw_format too. */
static void image_make(FgImage *im, VkFormat fmt, uint32_t w, uint32_t h) {
    VkFormat raw = raw_format(fmt), both[2] = {fmt, raw};
    VkImageFormatListCreateInfo list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2,
                                        .pViewFormats = both};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                             .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    if (raw != fmt)  /* storage use through the UNORM view only */
        ici.pNext = &list, ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, raw, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) ici.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
    CHECK(vkCreateImage(vk.device, &ici, NULL, &im->image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, im->image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &im->mem));
    CHECK(vkBindImageMemory(vk.device, im->image, im->mem, 0));
}

static void image_free(FgImage *im) {
    if (!im->image) return;
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

/* The slot's images for frames of w x h in fmt (made again on a change; only when nothing shows them). */
static void slot_images(VkCommandBuffer cb, Slot *s, VkFormat fmt, uint32_t w, uint32_t h) {
    if (s->real.image && s->fmt == fmt && s->w == w && s->h == h) return;
    if (s->real.image) gpu_wait(gpu_submitted(), UINT64_MAX), image_free(&s->real), image_free(&s->gen);
    image_make(&s->real, fmt, w, h);
    image_make(&s->gen, fmt, w, h);
    to_general(cb, s->real.image);
    to_general(cb, s->gen.image);
    s->fmt = fmt, s->w = w, s->h = h;
}

/* ---- pacing: the generated frame now, the game's half the game's frame time later */

static int gen_share_x1000;  /* the share of recent game frames shown with a generated one, x1000 (the HUD) */

float gpu_fg_shown_per_frame(void) {
    if (gpu_fg() == 2) return dlssg_shown_per_frame();
    if (!gpu_fg_active() || !pacer_started) return 0.0f;
    return 1.0f + (float)__atomic_load_n(&gen_share_x1000, __ATOMIC_RELAXED) / 1000.0f;
}

static void pacer(void *arg) {
    (void)arg;
    double interval_ms = 16.7;
    int64_t last_ns = 0;
    for (;;) {
        host_lock(&plock);
        while (queued < 0) host_cond_wait(&pcond, &plock, -1);
        int k = queued;
        VkRect2D rect = queued_rect;
        uint64_t frame = queued_frame;
        int64_t t = queued_ns;
        queued = -1, showing = k;
        host_cond_broadcast(&pcond);
        host_unlock(&plock);
        Slot *s = &slots[k];
        if (last_ns) {  /* the game's frame time, smoothed over ~10 frames */
            double d = (double)(t - last_ns) / 1e6;
            if (d > 0.0 && d < 100.0) interval_ms += (d - interval_ms) * 0.1;
        }
        last_ns = t;
        if (s->has_gen) {
            /* screenshots are of the game's frames; YW4_FG_SHOT_GEN=1: of the generated one shown before it instead,
             * 2: of both (the generated one as frame_N_gen.bmp) */
            static int shot_gen = -1;
            if (shot_gen < 0) shot_gen = getenv("YW4_FG_SHOT_GEN") ? atoi(getenv("YW4_FG_SHOT_GEN")) : 0;
            if (shot_gen == 2) window_screenshot_suffix("_gen");
            window_present_image(s->gen.image, s->fmt, rect, shot_gen ? frame : UINT64_MAX);
            if (shot_gen == 2) window_screenshot_suffix("");
            if (shot_gen == 1) frame = UINT64_MAX;
            int64_t due = t + (int64_t)(interval_ms * 0.5e6), now = host_now_ns();
            if (due > now && due - now < 50000000) host_sleep_ns(due - now);
        }
        window_present_image(s->real.image, s->fmt, rect, frame);
        static int shown, made;  /* every 600 of the game's frames: how many got a generated one */
        made += s->has_gen;
        __atomic_store_n(&gen_share_x1000, gen_share_x1000 + (s->has_gen * 1000 - gen_share_x1000) / 16, __ATOMIC_RELAXED);
        if (++shown == 600) {
            fprintf(stderr, "yw4: fg: %d of the last 600 frames had a generated frame before them (%.1f ms apart)\n",
                    made, interval_ms * 0.5);
            shown = made = 0;
        }
        host_lock(&plock);
        s->busy = 0, showing = -1;
        host_cond_broadcast(&pcond);
        host_unlock(&plock);
    }
}

int gpu_fg_record(VkCommandBuffer cb, VkImage image, VkFormat format, uint32_t w, uint32_t h) {
    uint64_t id = prepared_id;
    prepared_id = 0;
    if (!gpu_fg_active()) {
        if (ctx && gpu_fg() != 1) destroy();
        return -1;
    }
    if (!pacer_started) {
        host_lock_init(&plock);
        host_cond_init(&pcond);
        if (host_thread_start(pacer, NULL, 1 << 20)) {
            fprintf(stderr, "yw4: fg: no pacing thread: no frame generation\n");
            loaded = 0;
            return -1;
        }
        pacer_started = 1;
    }
    /* the context follows the presented size and the 3D's: made here, used from the next frame's prepare */
    presented_fmt = format;
    if (want_rw && want_rh && create(w, h, want_rw, want_rh, raw_format(format)) == 0) return -1;
    int k = next_slot;
    next_slot ^= 1;
    host_lock(&plock);
    while (slots[k].busy) host_cond_wait(&pcond, &plock, -1);  /* still to be shown: the pacing thread is behind */
    slots[k].busy = 1;
    host_unlock(&plock);
    Slot *s = &slots[k];
    slot_images(cb, s, format, w, h);
    full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, image, VK_IMAGE_LAYOUT_GENERAL, s->real.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    s->has_gen = 0;
    if (id && ctx && ctx_dw == w && ctx_dh == h) {
        struct ffxDispatchDescFrameGeneration d = {.header = {.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION}};
        d.commandList = cb;
        /* our copy of the game's frame: the game's image has no UNORM view */
        d.presentColor = resource(s->real.image, raw_format(format), w, h, FFX_API_RESOURCE_USAGE_READ_ONLY);
        d.outputs[0] = resource(s->gen.image, raw_format(format), w, h, FFX_API_RESOURCE_USAGE_UAV);
        d.numGeneratedFrames = 1;
        d.reset = prepared_reset;
        d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;  /* the game's bytes, shown as they are */
        d.minMaxLuminance[0] = 0.0f, d.minMaxLuminance[1] = 1000.0f;
        d.generationRect.width = (int32_t)w, d.generationRect.height = (int32_t)h;
        d.frameID = id;
        full_barrier(cb);
        ffxReturnCode_t r = fx.Dispatch(&ctx, &d.header);
        full_barrier(cb);
        static int show_hudless = -1;  /* tests: YW4_FG_SHOW_HUDLESS=1 shows the HUD-less copy in its place */
        if (show_hudless < 0) show_hudless = getenv("YW4_FG_SHOW_HUDLESS") != NULL;
        if (show_hudless && hudless_ok && hudless_w == w && hudless_h == h) {
            VkImageCopy hc = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
            vkCmdCopyImage(cb, hudless.image, VK_IMAGE_LAYOUT_GENERAL, s->gen.image, VK_IMAGE_LAYOUT_GENERAL, 1, &hc);
            full_barrier(cb);
        }
        if (r == FFX_API_RETURN_OK) s->has_gen = 1;
        else {
            static int logged;
            if (logged++ < 4) fprintf(stderr, "yw4: fg: generation failed (%u)\n", (unsigned)r);
        }
        next_id = id + 1;
        prepared_reset = 0;
    } else {
        prepared_reset = 1;  /* a frame without a prepare: FSR starts over */
    }
    full_barrier(cb);
    return k;
}

void gpu_fg_show(int k, VkRect2D rect, uint64_t frame) {
    host_lock(&plock);
    while (queued >= 0) host_cond_wait(&pcond, &plock, -1);
    queued = k, queued_rect = rect, queued_frame = frame, queued_ns = host_now_ns();
    if (rect.offset.x != shown_rect.offset.x || rect.offset.y != shown_rect.offset.y ||
        rect.extent.width != shown_rect.extent.width || rect.extent.height != shown_rect.extent.height) {
        fprintf(stderr, "yw4: fg: the window shows %ux%u at %d,%d of the presented image\n", rect.extent.width,
                rect.extent.height, rect.offset.x, rect.offset.y);
        shown_rect = rect;
    }
    host_cond_broadcast(&pcond);
    host_unlock(&plock);
}

void gpu_fg_drain(void) {
    if (!pacer_started) return;
    host_lock(&plock);
    while (queued >= 0 || showing >= 0) host_cond_wait(&pcond, &plock, -1);
    host_unlock(&plock);
}

#else  /* not Windows: no FSR DLLs yet */

int gpu_fg_active(void) { return 0; }
float gpu_fg_shown_per_frame(void) { return 0.0f; }
void gpu_fg_prepare(VkCommandBuffer cb, const GpuFgPrepare *p) { (void)cb, (void)p; }
int gpu_fg_record(VkCommandBuffer cb, VkImage image, VkFormat format, uint32_t w, uint32_t h) {
    (void)cb, (void)image, (void)format, (void)w, (void)h;
    return -1;
}
void gpu_fg_show(int slot, VkRect2D rect, uint64_t frame) { (void)slot, (void)rect, (void)frame; }
void gpu_fg_drain(void) {}

#endif
