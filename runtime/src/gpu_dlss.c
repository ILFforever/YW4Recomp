/* NVIDIA DLSS upscaling (decision 0040). NVIDIA's NGX SDK (fetched by tools/yw4/deps.py under NVIDIA's own
 * licence, never committed): its static library, linked with runtime/src/msvc_shim.c, finds NGX in the NVIDIA
 * driver, which loads nvngx_dlss.dll from yw4.exe's folder. DLSS needs a few device extensions, which window.c
 * enables on NVIDIA GPUs (gpu_dlss_device_extensions).
 *
 * Used by gpu_aa.c like FSR: the same inputs (the encoded scene, its depth, the game's motion, the jitter), the
 * same output image and the same upscaling plumbing (gpu.c). DLSS is told the colour is not HDR (the encoded
 * curve is 0..1) and works out its own exposure. Upscaling modes map to DLSS's: none = DLAA (DLSS's
 * anti-aliasing at the same size), quality, balanced, performance. NVIDIA DLSS is credited where it is offered
 * (the settings panel's option text, README). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "gpu_dlss.h"
#include "gpu_dlssg.h"
#include "host.h"

#ifdef YW4_DLSS
#include <SDL3/SDL_vulkan.h>

#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"

int gpu_dlss_built(void) { return 1; }

int gpu_dlss_device_extensions(uint32_t vendor, const char ***exts, unsigned *count) {
    *exts = NULL, *count = 0;
    if (vendor != 0x10de) return 0;  /* NVIDIA */
    unsigned ni = 0;
    const char **ie = NULL;
    if (NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, count, exts) != NVSDK_NGX_Result_Success) *exts = NULL, *count = 0;
    return (int)*count;
}

/* Models (2026-10-29, 4K 21:9 output, RTX 4070 laptop, performance mode): DLSS's defaults, the transformers K
 * (quality, balanced) and M (performance), cost 5.3 / 7.4 ms; the CNN preset E 2.6 ms (FSR 3.7). The CNN was the
 * default until 2026-10-31, when it turned out to make shadow edges flicker (the character's shadow, standing
 * still: 4-6% of its edge pixels over 16 frames, the transformers 0%; the same in the build that brought DLSS in).
 * So the transformers only, no panel row; YW4_DLSS_MODEL=0 still picks the CNN for tests. */
int gpu_dlss_model(void) {
    static int m = -1;
    if (m < 0) m = !(getenv("YW4_DLSS_MODEL") && atoi(getenv("YW4_DLSS_MODEL")) == 0);
    return m;
}

static int state = -1;  /* -1 not tried, 0 unavailable, 1 ready */
static NVSDK_NGX_Parameter *params;
static NVSDK_NGX_Handle *feature;
static uint32_t f_rw, f_rh, f_ow, f_oh;
static int f_quality, f_model, f_failed;

int gpu_dlss_available(void) {
    if (state >= 0) return state;
    state = 0;
    if (vk.props.vendorID != 0x10de) return 0;
    PFN_vkGetInstanceProcAddr gipa = dlssg_loader();  /* Streamline's while it runs (DLSS frame generation) */
    if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        "4f6c2e1a-8b3d-4c7e-9a52-1d0e7b6c3f88", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", L".", vk.instance, vk.gpu,
        vk.device, gipa, vkGetDeviceProcAddr, NULL, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        fprintf(stderr, "yw4: dlss: NGX did not start (%#x): FSR or TAA instead\n", (unsigned)r);
        return 0;
    }
    NVSDK_NGX_Parameter *caps = NULL;
    int avail = 0, driver = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps)) && caps) {
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &driver);
    }
    if (!avail) {
        fprintf(stderr, "yw4: dlss: not available on this GPU%s: FSR or TAA instead\n",
                driver ? " (the driver needs an update)" : "");
        return 0;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&params))) return 0;
    fprintf(stderr, "yw4: dlss: NVIDIA DLSS ready\n");
    return state = 1;
}

static void release(void) {
    if (!feature) return;
    gpu_wait(gpu_submitted(), UINT64_MAX);  /* DLSS's resources may still be in use */
    NVSDK_NGX_VULKAN_ReleaseFeature(feature);
    feature = NULL;
}

static int create(VkCommandBuffer cb, const GpuDlssInputs *in) {
    int model = gpu_dlss_model();
    int same = f_rw == in->render_w && f_rh == in->render_h && f_ow == in->output_w && f_oh == in->output_h &&
               f_quality == in->quality && f_model == model;
    if (feature && same) return 1;
    if (!feature && f_failed && same) return 0;
    release();
    f_rw = in->render_w, f_rh = in->render_h, f_ow = in->output_w, f_oh = in->output_h, f_quality = in->quality;
    f_model = model;
    f_failed = 0;
    static const int perf[4] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
                                NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf};
    NVSDK_NGX_DLSS_Create_Params cp;
    memset(&cp, 0, sizeof cp);
    cp.Feature.InWidth = in->render_w, cp.Feature.InHeight = in->render_h;
    cp.Feature.InTargetWidth = in->output_w, cp.Feature.InTargetHeight = in->output_h;
    cp.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)perf[in->quality & 3];
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    /* the model for every mode: fast = preset E, best = DLSS's own defaults (0); YW4_DLSS_PRESET=<letter> forces one
     * (K for DLAA/quality/balanced and M for performance are the transformer defaults; E/F the CNN, deprecated) */
    const char *pe = getenv("YW4_DLSS_PRESET");
    unsigned v = model ? 0 : NVSDK_NGX_DLSS_Hint_Render_Preset_E;
    if (pe && *pe >= 'A' && *pe <= 'Z') v = (unsigned)(*pe - 'A' + 1);
    {
        static const char *const hints[] = {
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
        for (size_t i = 0; i < sizeof hints / sizeof *hints; i++) NVSDK_NGX_Parameter_SetUI(params, hints[i], v);
    }
    int64_t t0 = host_now_ns();
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(vk.device, cb, 1, 1, &feature, params, &cp);
    if (NVSDK_NGX_FAILED(r) || !feature) {
        fprintf(stderr, "yw4: dlss: feature for %ux%u -> %ux%u failed (%#x)\n", in->render_w, in->render_h, in->output_w,
                in->output_h, (unsigned)r);
        feature = NULL, f_failed = 1;
        return 0;
    }
    fprintf(stderr, "yw4: dlss: %ux%u -> %ux%u, %s model, ready in %.0f ms\n", in->render_w, in->render_h,
            in->output_w, in->output_h, model ? "best" : "fast", (double)(host_now_ns() - t0) / 1e6);
    return 1;
}

static NVSDK_NGX_Resource_VK resource(VkImageView v, VkImage im, VkFormat f, uint32_t w, uint32_t h, int depth,
                                      int write) {
    VkImageSubresourceRange range = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(v, im, range, f, w, h, write != 0);
}

int gpu_dlss_dispatch(VkCommandBuffer cb, const GpuDlssInputs *in) {
    if (gpu_dlss_available() != 1 || !create(cb, in)) return 0;
    NVSDK_NGX_Resource_VK color = resource(in->color_view, in->color, in->color_format, in->render_w, in->render_h, 0, 0);
    NVSDK_NGX_Resource_VK depth = resource(in->depth_view, in->depth, in->depth_format, in->render_w, in->render_h, 1, 0);
    NVSDK_NGX_Resource_VK motion = resource(in->motion_view, in->motion, VK_FORMAT_R16G16_SFLOAT, in->render_w,
                                            in->render_h, 0, 0);
    NVSDK_NGX_Resource_VK out = resource(in->output_view, in->output, in->output_format, in->output_w, in->output_h, 0, 1);
    NVSDK_NGX_VK_DLSS_Eval_Params e;
    memset(&e, 0, sizeof e);
    e.Feature.pInColor = &color, e.Feature.pInOutput = &out;
    e.pInDepth = &depth, e.pInMotionVectors = &motion;
    e.InJitterOffsetX = in->jitter[0], e.InJitterOffsetY = in->jitter[1];
    e.InRenderSubrectDimensions.Width = in->render_w, e.InRenderSubrectDimensions.Height = in->render_h;
    e.InReset = in->reset;
    e.InMVScaleX = in->motion_scale[0], e.InMVScaleY = in->motion_scale[1];
    e.InFrameTimeDeltaInMsec = in->frame_ms;
    NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, feature, params, &e);
    if (NVSDK_NGX_FAILED(r)) {
        static int logged;
        if (logged++ < 4) fprintf(stderr, "yw4: dlss: evaluate failed (%#x)\n", (unsigned)r);
        return 0;
    }
    return 1;
}

#else  /* no DLSS in this build */

int gpu_dlss_built(void) { return 0; }
int gpu_dlss_device_extensions(uint32_t vendor, const char ***exts, unsigned *count) {
    (void)vendor;
    *exts = NULL, *count = 0;
    return 0;
}
int gpu_dlss_available(void) { return 0; }
int gpu_dlss_model(void) { return 1; }
int gpu_dlss_dispatch(VkCommandBuffer cb, const GpuDlssInputs *in) {
    (void)cb, (void)in;
    return 0;
}

#endif
