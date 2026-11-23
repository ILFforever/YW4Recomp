/* Intel XeSS upscaling (decision 0089) through Intel's own libxess.dll (XeSS SDK 3.0.2: the headers MIT, the DLL
 * under Intel's Simplified Software Licence, fetched and checked by tools/yw4/deps.py, never committed), copied next
 * to yw4.exe with its licence text. Windows only: Intel ships no Linux library.
 *
 * XeSS runs on any GPU: on Intel Arc with its matrix units (XMX), elsewhere a slower general path (DP4a). Unlike FSR
 * it needs instance and device extensions and features of its own, so the DLL is loaded before the Vulkan instance
 * is made (window.c asks gpu_xess_instance_extensions, gpu_xess_device_extensions, gpu_xess_device_features).
 * YW4_XESS=0 leaves it out completely: the device is made exactly as without XeSS.
 *
 * Used by gpu_aa.c like DLSS: the same inputs (the encoded scene, a depth-only copy of its depth, the game's motion,
 * the jitter), the same output image and the same upscaling plumbing (gpu.c). The colour is told to be LDR (the
 * encoded curve is 0..1). Upscaling modes map to XeSS's with its older scale factors (1.5 / 1.7 / 2.0, the same as
 * FSR's and DLSS's); none = XeSS's anti-aliasing at the same size. XeSS wants its inputs in
 * SHADER_READ_ONLY_OPTIMAL: ours stay in GENERAL, so they are moved there and back around its pass. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "gpu_xess.h"
#include "host.h"

#if defined(YW4_XESS) && defined(_WIN32)
#include <windows.h>

#include "xess_vk.h"

#define XESS_FNS(X)                                                                                                    \
    X(xessGetVersion) X(xessVKGetRequiredInstanceExtensions) X(xessVKGetRequiredDeviceExtensions)                      \
    X(xessVKGetRequiredDeviceFeatures) X(xessVKCreateContext) X(xessVKBuildPipelines) X(xessVKInit) X(xessVKExecute)   \
    X(xessDestroyContext) X(xessSetVelocityScale) X(xessSetJitterScale) X(xessForceLegacyScaleFactors)                 \
    X(xessGetOptimalInputResolution) X(xessSetLoggingCallback) X(xessIsOptimalDriver)
#define DECLARE(f) static __typeof__(&f) p_##f;
XESS_FNS(DECLARE)

static int loaded = -1;  /* -1 not tried, 0 failed, 1 loaded */
static int state = -1;   /* -1 not tried, 0 unavailable, 1 ready */
static xess_context_handle_t ctx;
static uint32_t i_rw, i_rh, i_ow, i_oh;
static int i_quality, i_ready, i_failed;
static const uint32_t FLAGS = XESS_INIT_FLAG_LDR_INPUT_COLOR;  /* the encoded scene: 0..1 */

int gpu_xess_built(void) { return 1; }

static int load(void) {
    if (loaded >= 0) return loaded;
    loaded = 0;
    const char *e = getenv("YW4_XESS");
    if (e && *e == '0') return 0;
    HMODULE m = LoadLibraryA("libxess.dll");
    if (!m) {
        fprintf(stderr, "yw4: xess: libxess.dll not found next to yw4.exe (python -m recomp.build fetches it)\n");
        return 0;
    }
#define LOAD(f)                                                                                                        \
    if (!(p_##f = (__typeof__(&f))(void (*)(void))GetProcAddress(m, #f))) {                                            \
        fprintf(stderr, "yw4: xess: libxess.dll lacks %s\n", #f);                                                      \
        return 0;                                                                                                      \
    }
    XESS_FNS(LOAD)
    xess_version_t v = {0};
    p_xessGetVersion(&v);
    fprintf(stderr, "yw4: xess: libxess.dll %u.%u.%u loaded\n", v.major, v.minor, v.patch);
    return loaded = 1;
}

int gpu_xess_instance_extensions(const char *const **exts, uint32_t *count) {
    *exts = NULL, *count = 0;
    if (!load()) return 0;
    uint32_t api = 0;
    if (p_xessVKGetRequiredInstanceExtensions(count, exts, &api) != XESS_RESULT_SUCCESS) *exts = NULL, *count = 0;
    if (api > VK_API_VERSION_1_3)
        fprintf(stderr, "yw4: xess: wants Vulkan %u.%u, the game asks for 1.3\n", VK_API_VERSION_MAJOR(api),
                VK_API_VERSION_MINOR(api));
    return (int)*count;
}

int gpu_xess_device_extensions(VkInstance instance, VkPhysicalDevice gpu, const char *const **exts, uint32_t *count) {
    *exts = NULL, *count = 0;
    if (loaded != 1) return 0;
    if (p_xessVKGetRequiredDeviceExtensions(instance, gpu, count, exts) != XESS_RESULT_SUCCESS) *exts = NULL, *count = 0;
    return (int)*count;
}

void gpu_xess_device_features(VkInstance instance, VkPhysicalDevice gpu, VkPhysicalDeviceFeatures2 *features) {
    if (loaded != 1) return;
    void *chain = features;
    xess_result_t r = p_xessVKGetRequiredDeviceFeatures(instance, gpu, &chain);
    if (r != XESS_RESULT_SUCCESS) fprintf(stderr, "yw4: xess: no device features (%d)\n", (int)r);
}

static void log_message(const char *msg, xess_logging_level_t level) {
    fprintf(stderr, "yw4: xess: %s: %s\n", level >= XESS_LOGGING_LEVEL_ERROR ? "error" : "warning", msg);
}

int gpu_xess_available(void) {
    if (state >= 0) return state;
    if (!vk.device) return 0;
    state = 0;
    if (loaded != 1) return 0;
    xess_result_t r = p_xessVKCreateContext(vk.instance, vk.gpu, vk.device, &ctx);
    if (r != XESS_RESULT_SUCCESS) {
        fprintf(stderr, "yw4: xess: not available on this GPU (%d): FSR or TAA instead\n", (int)r);
        ctx = NULL;
        return 0;
    }
    p_xessSetLoggingCallback(ctx, XESS_LOGGING_LEVEL_WARNING, log_message);
    if (p_xessIsOptimalDriver(ctx) == XESS_RESULT_WARNING_OLD_DRIVER)
        fprintf(stderr, "yw4: xess: the graphics driver is older than XeSS would like\n");
    p_xessForceLegacyScaleFactors(ctx, true);  /* 1.5 / 1.7 / 2.0, as our upscaling modes */
    /* its shaders built in the background now; xessVKInit waits for them */
    int64_t t0 = host_now_ns();
    r = p_xessVKBuildPipelines(ctx, VK_NULL_HANDLE, false, FLAGS);
    if (r != XESS_RESULT_SUCCESS) {
        fprintf(stderr, "yw4: xess: pipelines not started (%d): FSR or TAA instead\n", (int)r);
        p_xessDestroyContext(ctx), ctx = NULL;
        return 0;
    }
    fprintf(stderr, "yw4: xess: Intel XeSS ready (pipelines building, %.0f ms so far)\n",
            (double)(host_now_ns() - t0) / 1e6);
    return state = 1;
}

/* Our mode as XeSS's setting; the input size must fit the setting's range, else the nearest one that takes it */
static int init(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int quality) {
    int same = i_rw == rw && i_rh == rh && i_ow == ow && i_oh == oh && i_quality == quality;
    if (same && i_ready) return 1;
    if (same && i_failed) return 0;
    i_rw = rw, i_rh = rh, i_ow = ow, i_oh = oh, i_quality = quality, i_ready = 0, i_failed = 1;
    static const xess_quality_settings_t want[4] = {XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_QUALITY,
                                                     XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE};
    static const xess_quality_settings_t order[] = {
        XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, XESS_QUALITY_SETTING_ULTRA_QUALITY,
        XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE,
        XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
    xess_2d_t out = {ow, oh}, opt, lo, hi;
    xess_quality_settings_t q = want[quality < 0 || quality > 3 ? 0 : quality];
    int fits = 0;
    for (int k = -1; k < (int)(sizeof order / sizeof *order) && !fits; k++) {
        xess_quality_settings_t t = k < 0 ? q : order[k];
        if (p_xessGetOptimalInputResolution(ctx, &out, t, &opt, &lo, &hi) != XESS_RESULT_SUCCESS) continue;
        if (rw >= lo.x && rw <= hi.x && rh >= lo.y && rh <= hi.y) q = t, fits = 1;
    }
    if (!fits) {
        fprintf(stderr, "yw4: xess: no setting takes %ux%u -> %ux%u: FSR or TAA instead\n", rw, rh, ow, oh);
        return 0;
    }
    gpu_wait(gpu_submitted(), UINT64_MAX);  /* XeSS's resources may still be in use */
    xess_vk_init_params_t p = {.outputResolution = out, .qualitySetting = q, .initFlags = FLAGS};
    int64_t t0 = host_now_ns();
    xess_result_t r = p_xessVKInit(ctx, &p);
    if (r != XESS_RESULT_SUCCESS) {
        fprintf(stderr, "yw4: xess: init for %ux%u -> %ux%u failed (%d): FSR or TAA instead\n", rw, rh, ow, oh, (int)r);
        return 0;
    }
    fprintf(stderr, "yw4: xess: %ux%u -> %ux%u, setting %d, ready in %.0f ms\n", rw, rh, ow, oh, (int)q,
            (double)(host_now_ns() - t0) / 1e6);
    i_ready = 1, i_failed = 0;
    return 1;
}

void gpu_xess_prepare(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int quality) {
    if (gpu_xess_available() == 1 && rw && rh && ow && oh) init(rw, rh, ow, oh, quality);
}

/* XeSS's inputs from GENERAL to SHADER_READ_ONLY_OPTIMAL (to_read) and back */
static void inputs_layout(VkCommandBuffer cb, const GpuXessInputs *in, int to_read) {
    VkImageLayout a = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout b = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImage images[3] = {in->color, in->motion, in->depth};
    VkImageMemoryBarrier bar[3];
    for (int i = 0; i < 3; i++)
        bar[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = a, .newLayout = b,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i],
            .subresourceRange = {i == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 3, bar);
}

static xess_vk_image_view_info view_info(VkImage image, VkImageView view, VkFormat f, uint32_t w, uint32_t h,
                                         VkImageAspectFlags aspect) {
    return (xess_vk_image_view_info){.imageView = view, .image = image, .subresourceRange = {aspect, 0, 1, 0, 1},
                                     .format = f, .width = w, .height = h};
}

int gpu_xess_dispatch(VkCommandBuffer cb, const GpuXessInputs *in) {
    if (gpu_xess_available() != 1 || !init(in->render_w, in->render_h, in->output_w, in->output_h, in->quality))
        return 0;
    /* XeSS's motion in render pixels from this frame's position to the last, like DLSS's scale */
    p_xessSetVelocityScale(ctx, in->motion_scale[0], in->motion_scale[1]);
    xess_vk_execute_params_t e;
    memset(&e, 0, sizeof e);
    e.colorTexture = view_info(in->color, in->color_view, in->color_format, in->render_w, in->render_h,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    e.velocityTexture = view_info(in->motion, in->motion_view, VK_FORMAT_R16G16_SFLOAT, in->render_w, in->render_h,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
    e.depthTexture = view_info(in->depth, in->depth_view, in->depth_format, in->render_w, in->render_h,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
    e.outputTexture = view_info(in->output, in->output_view, in->output_format, in->output_w, in->output_h,
                                VK_IMAGE_ASPECT_COLOR_BIT);
    e.jitterOffsetX = in->jitter[0], e.jitterOffsetY = in->jitter[1];
    e.exposureScale = 1.0f, e.resetHistory = in->reset != 0;
    e.inputWidth = in->render_w, e.inputHeight = in->render_h;
    inputs_layout(cb, in, 1);
    xess_result_t r = p_xessVKExecute(ctx, cb, &e);
    inputs_layout(cb, in, 0);
    if (r != XESS_RESULT_SUCCESS) {
        static int logged;
        if (logged++ < 4) fprintf(stderr, "yw4: xess: execute failed (%d)\n", (int)r);
        return 0;
    }
    return 1;
}

#else  /* no XeSS in this build (or not Windows) */

int gpu_xess_built(void) { return 0; }
int gpu_xess_instance_extensions(const char *const **exts, uint32_t *count) {
    *exts = NULL, *count = 0;
    return 0;
}
int gpu_xess_device_extensions(VkInstance instance, VkPhysicalDevice gpu, const char *const **exts, uint32_t *count) {
    (void)instance, (void)gpu;
    *exts = NULL, *count = 0;
    return 0;
}
void gpu_xess_device_features(VkInstance instance, VkPhysicalDevice gpu, VkPhysicalDeviceFeatures2 *features) {
    (void)instance, (void)gpu, (void)features;
}
int gpu_xess_available(void) { return 0; }
void gpu_xess_prepare(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int quality) {
    (void)rw, (void)rh, (void)ow, (void)oh, (void)quality;
}
int gpu_xess_dispatch(VkCommandBuffer cb, const GpuXessInputs *in) {
    (void)cb, (void)in;
    return 0;
}

#endif
