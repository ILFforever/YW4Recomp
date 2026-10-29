/* NVIDIA DLSS frame generation through Streamline 2.14.16 (decision 0041; the SDK is fetched by
 * tools/yw4/deps.py and never committed: Streamline is MIT, its DLSS-G and Reflex DLLs are under NVIDIA's RTX SDK
 * licence). C++ because Streamline's headers are; the rest of the runtime calls the C functions in gpu_dlssg.h.
 *
 * Streamline is started before the Vulkan instance when frame generation is set to DLSS at start (YW4_FG=2), and
 * every Vulkan function then comes through sl.interposer.dll's vkGetInstanceProcAddr: Vulkan's own, except the
 * few Streamline intercepts (instance and device creation, where it adds what DLSS-G needs; the surface; the
 * swapchain and the present, where DLSS-G puts its generated frame in and paces it). Its manual hooking mode
 * (only those few from it, the rest from SDL's loader) crashed in sl.common during vkCreateDevice: a null
 * function pointer (2026-10-29). sl.interposer.dll is loaded at run
 * time and its core functions are reached through pointers: the forwarders below give Streamline's own inline
 * helpers (slDLSSGSetOptions, slReflexSleep...) the functions they call.
 *
 * Per frame, on the presenting thread: a frame token, Reflex's sleep and the simulation-start marker after the
 * last present; after the anti-aliasing the 3D's depth and the game's motion tagged with the camera constants
 * (the game's matrices aren't known: a projection from the field of view, the camera motion taken as part of the
 * motion vectors); before the present the game's rectangle in the window image tagged as the backbuffer's
 * subrect (the black bars stay as they are), the options and the present markers. */
#ifdef _WIN32
#include <windows.h>
#include <dxgi.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"

extern "C" {
#include "gpu_dlssg.h"
#include "gpu_fg.h"
#include "host.h"
#include "vk.h"
}

/* ---- Streamline's core functions, from sl.interposer.dll */

static decltype(&slInit) p_slInit;
static decltype(&slShutdown) p_slShutdown;
static decltype(&slIsFeatureSupported) p_slIsFeatureSupported;
static decltype(&slSetTagForFrame) p_slSetTagForFrame;
static decltype(&slSetConstants) p_slSetConstants;
static decltype(&slGetFeatureFunction) p_slGetFeatureFunction;
static decltype(&slGetNewFrameToken) p_slGetNewFrameToken;
static decltype(&slGetFeatureRequirements) p_slGetFeatureRequirements;

sl::Result slInit(const sl::Preferences &pref, uint64_t sdkVersion) { return p_slInit(pref, sdkVersion); }
sl::Result slShutdown() { return p_slShutdown(); }
sl::Result slIsFeatureSupported(sl::Feature f, const sl::AdapterInfo &a) { return p_slIsFeatureSupported(f, a); }
sl::Result slSetTagForFrame(const sl::FrameToken &frame, const sl::ViewportHandle &viewport, const sl::ResourceTag *tags,
                            uint32_t n, sl::CommandBuffer *cb) {
    return p_slSetTagForFrame(frame, viewport, tags, n, cb);
}
sl::Result slSetConstants(const sl::Constants &c, const sl::FrameToken &frame, const sl::ViewportHandle &viewport) {
    return p_slSetConstants(c, frame, viewport);
}
sl::Result slGetFeatureFunction(sl::Feature f, const char *name, void *&fn) { return p_slGetFeatureFunction(f, name, fn); }
sl::Result slGetNewFrameToken(sl::FrameToken *&token, const uint32_t *index) { return p_slGetNewFrameToken(token, index); }
sl::Result slGetFeatureRequirements(sl::Feature f, sl::FeatureRequirements &r) { return p_slGetFeatureRequirements(f, r); }

static HMODULE mod;
static PFN_vkGetInstanceProcAddr sl_gipa;
static PFN_vkGetDeviceProcAddr sl_gdpa;
static int started = -1;  /* -1 not tried, 0 not running, 1 running */
static int avail = -1;
static const sl::ViewportHandle viewport(0);

static void log_message(sl::LogType type, const char *msg) {
    if (type == sl::LogType::eInfo && !getenv("YW4_SL_LOG")) return;
    size_t n = strlen(msg);
    fprintf(stderr, "yw4: streamline: %s: %.*s\n", type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warning" : "info",
            (int)(n && msg[n - 1] == '\n' ? n - 1 : n), msg);
}

/* An NVIDIA GPU in this PC, asked through DXGI: Streamline must start before any Vulkan call, so Vulkan can't be
 * asked. dxgi.dll is loaded here, not linked. */
static int nvidia_gpu(void) {
    HMODULE d = LoadLibraryW(L"dxgi.dll");
    if (!d) return 0;
    auto create = (HRESULT(WINAPI *)(REFIID, void **))(void *)GetProcAddress(d, "CreateDXGIFactory1");
    IDXGIFactory1 *f = NULL;
    int found = 0;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void **)&f)) && f) {
        IDXGIAdapter1 *a = NULL;
        for (UINT i = 0; !found && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            if (SUCCEEDED(a->GetDesc1(&desc)) && desc.VendorId == 0x10de) found = 1;
            a->Release();
        }
        f->Release();
    }
    FreeLibrary(d);
    return found;
}

extern "C" int dlssg_start(void) {
    if (started >= 0) return started;
    started = 0;
    const char *e = getenv("YW4_STREAMLINE");  /* 0: never (Vulkan as without it) */
    if (e && *e == '0') return 0;
    if (!nvidia_gpu()) return 0;
    mod = LoadLibraryW(L"sl.interposer.dll");
    if (!mod) {
        fprintf(stderr, "yw4: dlssg: sl.interposer.dll not found next to yw4.exe (python -m recomp.build fetches it)\n");
        return 0;
    }
#define LOAD(f) p_##f = (decltype(p_##f))(void *)GetProcAddress(mod, #f);
    LOAD(slInit) LOAD(slShutdown) LOAD(slIsFeatureSupported) LOAD(slSetTagForFrame) LOAD(slSetConstants)
    LOAD(slGetFeatureFunction) LOAD(slGetNewFrameToken) LOAD(slGetFeatureRequirements)
#undef LOAD
    sl_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(mod, "vkGetInstanceProcAddr");
    sl_gdpa = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(mod, "vkGetDeviceProcAddr");
    if (!p_slInit || !p_slSetTagForFrame || !p_slGetFeatureFunction || !p_slGetNewFrameToken || !sl_gipa || !sl_gdpa) {
        fprintf(stderr, "yw4: dlssg: sl.interposer.dll lacks its functions\n");
        return 0;
    }
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = getenv("YW4_SL_LOG") ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = log_message;
    static wchar_t logdir[512];  /* YW4_SL_LOG=<folder>: Streamline's own log file there too */
    const char *ld = getenv("YW4_SL_LOG");
    if (ld && strlen(ld) > 1 && MultiByteToWideChar(CP_UTF8, 0, ld, -1, logdir, 512)) pref.pathToLogsAndData = logdir;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;  /* no over-the-air plugin downloads */
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "4f6c2e1a-8b3d-4c7e-9a52-1d0e7b6c3f88";  /* the same as DLSS's (gpu_dlss.c) */
    pref.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        fprintf(stderr, "yw4: dlssg: Streamline did not start (%d)\n", (int)r);
        return 0;
    }
    fprintf(stderr, "yw4: dlssg: Streamline started\n");
    return started = 1;
}

extern "C" PFN_vkGetInstanceProcAddr dlssg_loader(void) { return started == 1 ? sl_gipa : NULL; }

/* VK_KHR_win32_surface's create info (the repo's Vulkan headers leave out the platform headers) */
struct Win32SurfaceInfo {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    HINSTANCE hinstance;
    HWND hwnd;
};
typedef VkResult(VKAPI_PTR *CreateWin32Surface)(VkInstance, const Win32SurfaceInfo *, const VkAllocationCallbacks *,
                                                VkSurfaceKHR *);

extern "C" VkSurfaceKHR dlssg_create_surface(void *hwnd) {
    if (started != 1 || !hwnd) return VK_NULL_HANDLE;
    auto create = (CreateWin32Surface)sl_gipa(vk.instance, "vkCreateWin32SurfaceKHR");
    if (!create) return VK_NULL_HANDLE;
    Win32SurfaceInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = GetModuleHandleW(NULL);
    ci.hwnd = (HWND)hwnd;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    return create(vk.instance, &ci, NULL, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

static int reflex_ok = -1;  /* Reflex runs on this GPU (GeForce 900 and newer) */

static void check_features(void) {
    if (avail >= 0) return;
    avail = reflex_ok = 0;
    if (started != 1 || !vk.device) return;
    sl::AdapterInfo a{};
    a.vkPhysicalDevice = vk.gpu;
    sl::Result r = slIsFeatureSupported(sl::kFeatureReflex, a);
    reflex_ok = r == sl::Result::eOk;
    if (!reflex_ok) fprintf(stderr, "yw4: dlssg: Reflex not supported here (%d)\n", (int)r);
    r = slIsFeatureSupported(sl::kFeatureDLSS_G, a);
    avail = reflex_ok && r == sl::Result::eOk;
    if (r != sl::Result::eOk)
        fprintf(stderr, "yw4: dlssg: DLSS frame generation not supported here (%d): needs an RTX 40 or newer, a recent "
                        "driver and hardware-accelerated GPU scheduling\n", (int)r);
    else fprintf(stderr, "yw4: dlssg: DLSS frame generation available\n");
}

extern "C" int dlssg_available(void) {
    check_features();
    return avail;
}

extern "C" int dlssg_reflex_available(void) {
    check_features();
    return reflex_ok;
}

/* The REFLEX setting: 0 off, 1 on, 2 on + boost (YW4_REFLEX at start) */
static int reflex_setting = -1;

extern "C" int dlssg_reflex_setting(void) {
    int m = __atomic_load_n(&reflex_setting, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("YW4_REFLEX");
        m = e ? atoi(e) : 0;
        if (m < 0 || m > 2) m = 0;
        __atomic_store_n(&reflex_setting, m, __ATOMIC_RELEASE);
    }
    return m;
}

extern "C" void dlssg_reflex_cycle(void) {
    int m = (dlssg_reflex_setting() + 1) % 3;
    __atomic_store_n(&reflex_setting, m, __ATOMIC_RELEASE);
}

/* What runs: the setting, at least on while DLSS frame generation is chosen (NVIDIA requires it) */
extern "C" int dlssg_reflex(void) {
    if (!dlssg_reflex_available()) return 0;
    int m = dlssg_reflex_setting();
    return gpu_fg() == 2 && avail && m == 0 ? 1 : m;
}

/* ---- per frame */

static sl::FrameToken *token;
static uint32_t frame_index;
static int tagged, mode_on = -1;
static float shown_per_frame;  /* frames on screen per game frame, from DLSS-G's state; 0 while it is off */
static VkImage hudless_image;  /* this present's HUD-less copy at the window's size (window.c), or none */
static VkImageView hudless_view;
static VkFormat hudless_format;
static uint32_t hudless_w, hudless_h;

extern "C" void dlssg_hudless(VkImage image, VkImageView view, VkFormat format, uint32_t w, uint32_t h) {
    hudless_image = image, hudless_view = view, hudless_format = format, hudless_w = w, hudless_h = h;
}

static int ready(void) { return started == 1 && reflex_ok == 1; }  /* the frame tokens and markers */

extern "C" void dlssg_frame_begin(void) {
    if (!ready()) return;
    frame_index++;
    if (slGetNewFrameToken(token, &frame_index) != sl::Result::eOk) {
        token = NULL;
        return;
    }
    static int applied = -1;  /* Reflex's mode, set again when the setting or frame generation changes it */
    int want = dlssg_reflex();
    if (want != applied) {
        sl::ReflexOptions ro{};
        ro.mode = want == 2 ? sl::ReflexMode::eLowLatencyWithBoost : want == 1 ? sl::ReflexMode::eLowLatency
                                                                               : sl::ReflexMode::eOff;
        sl::Result r = slReflexSetOptions(ro);
        fprintf(stderr, "yw4: dlssg: Reflex %s%s\n", want == 2 ? "on + boost" : want ? "on" : "off",
                r == sl::Result::eOk ? "" : " (not taken)");
        applied = want;
    }
    slReflexSleep(*token);  /* also with Reflex off: NVIDIA asks for the sleep and markers always */
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
    tagged = 0;
}

/* Row-major, row vectors (as Streamline's own helpers): a 4x4 inverse by Gauss-Jordan elimination. */
static void invert(const sl::float4x4 &m, sl::float4x4 &out) {
    float a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) a[r][c] = (&m[r].x)[c], a[r][c + 4] = r == c ? 1.0f : 0.0f;
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabsf(a[r][c]) > fabsf(a[p][c])) p = r;
        for (int k = 0; k < 8; k++) {
            float t = a[c][k];
            a[c][k] = a[p][k], a[p][k] = t;
        }
        float d = a[c][c];
        if (d == 0.0f) return;
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                float f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) (&out[r].x)[c] = a[r][c + 4];
}

static void identity(sl::float4x4 &m) {
    memset(&m, 0, sizeof m);
    m[0].x = m[1].y = m[2].z = m[3].w = 1.0f;
}

extern "C" void dlssg_tag(VkCommandBuffer cb, const GpuFgPrepare *p, uint32_t ow, uint32_t oh) {
    (void)ow, (void)oh;
    if (!ready() || !token) return;
    slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
    if (!avail || gpu_fg() != 2) return;
    sl::Resource depth(sl::ResourceType::eTex2d, (void *)p->depth, nullptr, (void *)p->depth_view, VK_IMAGE_LAYOUT_GENERAL);
    depth.width = p->render_w, depth.height = p->render_h, depth.nativeFormat = p->depth_format;
    depth.mipLevels = 1, depth.arrayLayers = 1, depth.flags = 0;
    depth.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sl::Resource mvec(sl::ResourceType::eTex2d, (void *)p->motion, nullptr, (void *)p->motion_view, VK_IMAGE_LAYOUT_GENERAL);
    mvec.width = p->render_w, mvec.height = p->render_h, mvec.nativeFormat = VK_FORMAT_R16G16_SFLOAT;
    mvec.mipLevels = 1, mvec.arrayLayers = 1, mvec.flags = 0;
    mvec.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sl::Extent full{0, 0, p->render_w, p->render_h};
    sl::ResourceTag tags[2] = {sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &full),
                               sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &full)};
    sl::Result r = slSetTagForFrame(*token, viewport, tags, 2, (sl::CommandBuffer *)cb);
    sl::Constants c{};
    float aspect = (float)p->render_w / (float)p->render_h, n = p->cam_near, f = p->cam_far;
    float ys = 1.0f / tanf(p->fov_y * 0.5f), xs = ys / aspect;
    memset(&c.cameraViewToClip, 0, sizeof c.cameraViewToClip);
    c.cameraViewToClip[0].x = xs, c.cameraViewToClip[1].y = ys;
    c.cameraViewToClip[2].z = f / (f - n), c.cameraViewToClip[2].w = 1.0f, c.cameraViewToClip[3].z = -n * f / (f - n);
    invert(c.cameraViewToClip, c.clipToCameraView);
    identity(c.clipToLensClip), identity(c.clipToPrevClip), identity(c.prevClipToClip);
    c.jitterOffset = {p->jitter[0], p->jitter[1]};
    c.mvecScale = {p->motion_scale[0] / (float)p->render_w, p->motion_scale[1] / (float)p->render_h};  /* to [-1, 1] */
    c.cameraPinholeOffset = {0.0f, 0.0f};
    c.cameraPos = {0.0f, 0.0f, 0.0f}, c.cameraUp = {0.0f, 1.0f, 0.0f}, c.cameraRight = {1.0f, 0.0f, 0.0f};
    c.cameraFwd = {0.0f, 0.0f, 1.0f};
    c.cameraNear = n, c.cameraFar = f, c.cameraFOV = p->fov_y, c.cameraAspectRatio = aspect;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = sl::Boolean::eFalse;
    sl::Result rc = slSetConstants(c, *token, viewport);
    if (r != sl::Result::eOk || rc != sl::Result::eOk) {
        static int logged;
        if (logged++ < 4) fprintf(stderr, "yw4: dlssg: tags (%d) or constants (%d) not taken\n", (int)r, (int)rc);
        return;
    }
    tagged = 1;
}

extern "C" void dlssg_present_begin(int32_t x, int32_t y, uint32_t w, uint32_t h, VkFormat format, uint32_t images) {
    if (!ready() || !token) return;
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
    int want = avail && gpu_fg() == 2 && tagged;
    if (want != mode_on && (avail || mode_on == 1)) {
        sl::DLSSGOptions o{};
        o.mode = want ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.numFramesToGenerate = 1;
        if (getenv("YW4_DLSSG_SHOW_GENERATED"))  /* tests: only the generated frames on screen (for screen captures) */
            o.flags = sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
        o.numBackBuffers = images;
        o.colorBufferFormat = (uint32_t)format;
        sl::Result r = slDLSSGSetOptions(viewport, o);
        fprintf(stderr, "yw4: dlssg: frame generation %s%s\n", want ? "on" : "off", r == sl::Result::eOk ? "" : " (not taken)");
        mode_on = want;
    }
    if (want) {
        sl::Extent sub{(uint32_t)y, (uint32_t)x, w, h};
        sl::ResourceTag tags[2] = {
            sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &sub),
            sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &sub)};
        /* the HUD-less copy at the window image's size (window.c), the same rectangle as the backbuffer's; without
         * it a null tag, so Streamline doesn't use an old one */
        sl::Resource hl(sl::ResourceType::eTex2d, (void *)hudless_image, nullptr, (void *)hudless_view,
                        VK_IMAGE_LAYOUT_GENERAL);
        hl.width = hudless_w, hl.height = hudless_h, hl.nativeFormat = (uint32_t)hudless_format;
        hl.mipLevels = 1, hl.arrayLayers = 1, hl.flags = 0;
        hl.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (hudless_image) tags[1].resource = &hl;
        slSetTagForFrame(*token, viewport, tags, 2, nullptr);
    }
    slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
}

extern "C" void dlssg_present_end(void) {
    if (!ready() || !token) return;
    slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
    static int n;  /* every 30 frames: how many frames DLSS-G showed per game frame (the HUD); every 600 logged */
    if (mode_on == 1 && ++n % 30 == 0) {
        sl::DLSSGState s{};
        if (slDLSSGGetState(viewport, s, nullptr) == sl::Result::eOk) {
            shown_per_frame = (float)s.numFramesActuallyPresented;
            if (n % 600 == 0)
                fprintf(stderr, "yw4: dlssg: status %#x, %u frames presented per frame\n", (unsigned)s.status,
                        s.numFramesActuallyPresented);
        }
    }
    if (mode_on != 1) shown_per_frame = 0.0f;
}

extern "C" float dlssg_shown_per_frame(void) { return shown_per_frame; }

extern "C" int dlssg_on(void) { return ready() && mode_on == 1; }

#else  /* not Windows: no Streamline */

extern "C" {
#include "gpu_dlssg.h"
}
extern "C" int dlssg_start(void) { return 0; }
extern "C" PFN_vkGetInstanceProcAddr dlssg_loader(void) { return NULL; }
extern "C" VkSurfaceKHR dlssg_create_surface(void *hwnd) { (void)hwnd; return VK_NULL_HANDLE; }
extern "C" int dlssg_available(void) { return 0; }
extern "C" int dlssg_reflex_available(void) { return 0; }
extern "C" int dlssg_reflex_setting(void) { return 0; }
extern "C" void dlssg_reflex_cycle(void) {}
extern "C" int dlssg_reflex(void) { return 0; }
extern "C" void dlssg_frame_begin(void) {}
extern "C" void dlssg_tag(VkCommandBuffer cb, const GpuFgPrepare *p, uint32_t ow, uint32_t oh) { (void)cb, (void)p, (void)ow, (void)oh; }
extern "C" void dlssg_present_begin(int32_t x, int32_t y, uint32_t w, uint32_t h, VkFormat f, uint32_t n) {
    (void)x, (void)y, (void)w, (void)h, (void)f, (void)n;
}
extern "C" void dlssg_present_end(void) {}
extern "C" float dlssg_shown_per_frame(void) { return 0.0f; }
extern "C" void dlssg_hudless(VkImage i, VkImageView v, VkFormat f, uint32_t w, uint32_t h) {
    (void)i, (void)v, (void)f, (void)w, (void)h;
}
extern "C" int dlssg_on(void) { return 0; }

#endif
