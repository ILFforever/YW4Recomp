/* AMD FSR upscaling through the FidelityFX SDK's own Vulkan DLLs (decision 0039; MIT, fetched and checked by
 * tools/yw4/deps.py, never committed): amd_fidelityfx_loader_vk.dll next to yw4.exe, which loads
 * amd_fidelityfx_upscaler_vk.dll from the same folder. The loader picks the upscaler the GPU supports: FSR 3.1 on
 * any GPU, FSR 4 on the AMD ones it names.
 *
 * Used by gpu_aa.c in place of TAA's blend: the same jitter point (3D draws shifted in the viewport), the game's
 * motion (copied at the end of the G-buffer pass) and the scene's depth. The colour goes in through the
 * anti-aliasing's reversible curve (perceptual, 0..1), so FSR is told the colour is non-linear.
 * Windows only for now: elsewhere gpu_fsr_available() says no and TAA is used. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "gpu_fsr.h"
#include "host.h"

#ifdef _WIN32
#include <windows.h>
#ifndef _WINDOWS
#define _WINDOWS  /* ffx_api_loader.h looks for MSBuild's name for Windows; without it ffxLoadFunctions is empty */
#endif

#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_api_types.h"
#include "vk/ffx_api_vk.h"
#include "../../upscalers/include/ffx_upscale.h"

static ffxFunctions fx;
static int loaded = -1;  /* -1 not tried, 0 failed, 1 loaded */
static ffxContext ctx;
static uint32_t ctx_rw, ctx_rh, ctx_ow, ctx_oh;
static int ctx_failed;   /* creation failed for this size: don't retry every frame */

static void message(uint32_t type, const wchar_t *text) {
    fprintf(stderr, "yw4: fsr: %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

int gpu_fsr_available(void) {
    if (loaded >= 0) return loaded;
    HMODULE m = LoadLibraryA("amd_fidelityfx_loader_vk.dll");
    if (!m) {
        fprintf(stderr, "yw4: fsr: amd_fidelityfx_loader_vk.dll not found next to yw4.exe (python -m recomp.build "
                        "fetches it): TAA instead\n");
        return loaded = 0;
    }
    ffxLoadFunctions(&fx, m);
    if (!fx.CreateContext || !fx.Dispatch || !fx.Query || !fx.DestroyContext) {
        fprintf(stderr, "yw4: fsr: the loader DLL lacks its functions: TAA instead\n");
        return loaded = 0;
    }
    return loaded = 1;
}

static void destroy(void) {
    if (!ctx) return;
    gpu_wait(gpu_submitted(), UINT64_MAX);  /* FSR's resources may still be in use */
    fx.DestroyContext(&ctx, NULL);
    ctx = NULL;
}

static int create(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (ctx && ctx_rw == rw && ctx_rh == rh && ctx_ow == ow && ctx_oh == oh) return 1;
    if (!ctx && ctx_failed && ctx_rw == rw && ctx_rh == rh && ctx_ow == ow && ctx_oh == oh) return 0;
    destroy();
    ctx_rw = rw, ctx_rh = rh, ctx_ow = ow, ctx_oh = oh, ctx_failed = 0;
    struct ffxCreateBackendVKDesc backend = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
        .deviceContext = {.vkDevice = vk.device, .vkPhysicalDevice = vk.gpu, .vkDeviceProcAddr = vkGetDeviceProcAddr}};
    struct ffxCreateContextDescUpscale desc = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .pNext = &backend.header},
        .flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE,
        .maxRenderSize = {rw, rh}, .maxUpscaleSize = {ow, oh}, .fpMessage = message};
    if (getenv("YW4_FSR_DEBUG")) desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (getenv("YW4_FSR_DEBUG_VIEW")) desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    /* The upscalers the DLL offers on this GPU, logged once; YW4_FSR_VERSION=<name> (e.g. 2.3.4) picks one */
    struct ffxOverrideVersion over = {.header = {.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION}};
    {
        uint64_t ids[16], count = 16;
        const char *names[16] = {0};
        struct ffxQueryDescGetVersions qv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS},
                                             .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                             .device = &backend.deviceContext, .outputCount = &count, .versionIds = ids,
                                             .versionNames = names};
        fx.Query(NULL, &qv.header);
        static int listed;
        const char *want = getenv("YW4_FSR_VERSION");
        for (uint64_t i = 0; i < count && i < 16; i++) {
            if (!listed) fprintf(stderr, "yw4: fsr: offers %s\n", names[i] ? names[i] : "?");
            if (want && names[i] && !strcmp(want, names[i])) over.versionId = ids[i];
        }
        listed = 1;
        if (over.versionId) backend.header.pNext = &over.header;
        else if (want) fprintf(stderr, "yw4: fsr: no version %s: the default instead\n", want);
    }
    int64_t t0 = host_now_ns();
    ffxReturnCode_t r = fx.CreateContext(&ctx, &desc.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        fprintf(stderr, "yw4: fsr: context for %ux%u -> %ux%u failed (%u): TAA instead\n", rw, rh, ow, oh, (unsigned)r);
        ctx = NULL, ctx_failed = 1;
        return 0;
    }
    struct ffxQueryGetProviderVersion qv = {.header = {.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION}};
    fx.Query(&ctx, &qv.header);
    fprintf(stderr, "yw4: fsr: %s for %ux%u -> %ux%u ready in %.0f ms\n", qv.versionName ? qv.versionName : "upscaler",
            rw, rh, ow, oh, (double)(host_now_ns() - t0) / 1e6);
    return 1;
}

void gpu_fsr_jitter(uint32_t w, uint32_t ow, uint64_t n, float *jx, float *jy) {
    *jx = *jy = 0.0f;
    if (gpu_fsr_available() != 1) return;
    int32_t phases = 8;
    struct ffxQueryDescUpscaleGetJitterPhaseCount qp = {
        .header = {.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT}, .renderWidth = w, .displayWidth = ow,
        .pOutPhaseCount = &phases};
    fx.Query(ctx ? &ctx : NULL, &qp.header);
    if (phases < 1) phases = 8;
    struct ffxQueryDescUpscaleGetJitterOffset qo = {
        .header = {.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET}, .index = (int32_t)(n % (uint64_t)phases),
        .phaseCount = phases, .pOutX = jx, .pOutY = jy};
    fx.Query(ctx ? &ctx : NULL, &qo.header);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
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

/* Our images stay in layout GENERAL, which FSR's Vulkan backend calls the unordered-access state: it moves them
 * to what it needs and back. */
static struct FfxApiResource resource(VkImage image, VkFormat f, uint32_t w, uint32_t h, uint32_t usage) {
    struct FfxApiResource r = {.resource = (void *)image, .state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS};
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = surface_format(f);
    r.description.width = w, r.description.height = h, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = usage;
    return r;
}

int gpu_fsr_dispatch(VkCommandBuffer cb, const GpuFsrInputs *in) {
    if (gpu_fsr_available() != 1 || !create(in->render_w, in->render_h, in->output_w, in->output_h)) return 0;
    struct ffxDispatchDescUpscale d = {.header = {.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE}, .commandList = cb};
    d.color = resource(in->color, in->color_format, in->render_w, in->render_h, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = resource(in->depth, in->depth_format, in->render_w, in->render_h, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors = resource(in->motion, VK_FORMAT_R16G16_SFLOAT, in->render_w, in->render_h,
                               FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = resource(in->output, in->output_format, in->output_w, in->output_h, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset.x = in->jitter[0], d.jitterOffset.y = in->jitter[1];
    d.motionVectorScale.x = in->motion_scale[0], d.motionVectorScale.y = in->motion_scale[1];
    d.renderSize.width = in->render_w, d.renderSize.height = in->render_h;
    static float test = -1.0f;  /* YW4_FSR_TEST_RATIO=1.5: timing only, FSR told the 3D used 1/ratio of the images */
    if (test < 0.0f) test = getenv("YW4_FSR_TEST_RATIO") ? (float)atof(getenv("YW4_FSR_TEST_RATIO")) : 0.0f;
    if (test > 1.0f)
        d.renderSize.width = (uint32_t)((float)in->render_w / test), d.renderSize.height = (uint32_t)((float)in->render_h / test);
    d.upscaleSize.width = in->output_w, d.upscaleSize.height = in->output_h;
    d.enableSharpening = in->sharpness > 0.0f, d.sharpness = in->sharpness;
    d.frameTimeDelta = in->frame_ms, d.preExposure = 1.0f, d.reset = in->reset != 0;
    d.cameraNear = in->cam_near, d.cameraFar = in->cam_far, d.cameraFovAngleVertical = in->fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    if (getenv("YW4_FSR_DEBUG_VIEW")) d.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;  /* FSR's own diagnostic picture */
    ffxReturnCode_t r = fx.Dispatch(&ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int logged;
        if (logged++ < 4) fprintf(stderr, "yw4: fsr: dispatch failed (%u)\n", (unsigned)r);
        return 0;
    }
    return 1;
}

#else  /* not Windows: no FSR DLLs yet */

int gpu_fsr_available(void) { return 0; }
void gpu_fsr_jitter(uint32_t w, uint32_t ow, uint64_t n, float *jx, float *jy) {
    (void)w, (void)ow, (void)n;
    *jx = *jy = 0.0f;
}
int gpu_fsr_dispatch(VkCommandBuffer cb, const GpuFsrInputs *in) {
    (void)cb, (void)in;
    return 0;
}

#endif
