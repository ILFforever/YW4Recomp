/* The game's window (decision 0019): SDL3 for the window and its events, Vulkan to show frames.
 *
 * For now a frame arrives as RGBA8 pixels from the CPU (NVN's present reads the window
 * texture back from the game's memory): it is copied to a Vulkan image and scaled into the
 * window, letterboxed to keep 16:9. Vulkan is loaded at run time through SDL
 * (SDL_Vulkan_GetVkGetInstanceProcAddr), so nothing is linked against vulkan-1.
 * YW4_VULKAN_VALIDATION=1 turns on the Khronos validation layer if it is installed.
 *
 * Threads: the process's main thread owns the window and pumps its events; the guest runs on
 * its own host thread and presents from there (Vulkan only needs one thread at a time per
 * queue and swapchain, which present_lock gives).
 */
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "events.h"
#include "host.h"
#include "bindings.h"
#include "framerate.h"
#include "gpu_dlss.h"
#include "gpu_dlssg.h"
#include "gpu_fg.h"
#include "gbuffer.h"
#include "gpu.h"
#include "hud.h"
#include "gamemenu.h"
#include "pause.h"
#include "mousemenu.h"
#include "settings.h"
#include "textin.h"
#include "theme.h"
#include "ui.h"
#include "version.h"
#include "vk.h"
#include "window.h"

#define GUEST_HOST_STACK (64u << 20)  /* the guest main thread's host stack: reserved, not committed */

/* ---- Vulkan functions (vk.h), loaded at run time */

#define DEFINE(f) PFN_##f f;
VK_GLOBAL_FNS(DEFINE)
VK_INSTANCE_FNS(DEFINE)
VK_DEVICE_FNS(DEFINE)
VkContext vk = {.queue_lock = HOST_LOCK_INIT};

/* ---- state */

static SDL_Window *window;
static int64_t refresh_mhz;  /* the display's refresh in millihertz, 0 unknown (window_refresh_hz) */

double window_refresh_hz(void) { return (double)__atomic_load_n(&refresh_mhz, __ATOMIC_ACQUIRE) / 1000.0; }
static int have_vulkan;
static HostLock present_lock = HOST_LOCK_INIT;

static VkSurfaceKHR surface;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static void window_notice(int high, const char *title, const char *fmt, ...);  /* a fading card (below, with the HUD) */
static void window_notice_keys(void);  /* the same card for the controls, after the one showing (if any) */
static VkFence fence;
static VkSemaphore acquired;

static VkSwapchainKHR swapchain;
static VkExtent2D swap_extent;
static VkImage swap_images[8];
static VkImageView swap_views[8];  /* to draw into (the HUD, blended) */
static VkSemaphore rendered[8];  /* one per swapchain image: reused only after that image is presented */
static uint32_t nswap;
static VkFormat swap_format;
static int swap_readable;  /* the window's images can be copied from (the quick look's blur) */
static int swapchain_stale = 1;  /* (re)create before the next present: first use, resize */
/* the last presented picture's place in the window, as fractions (x, y, width, height): the present fits it
 * with black bars when the window's shape differs. Written by the present, read by the window's thread. */
static float picture_rect[4] = {0, 0, 1, 1};
static int swap_free;            /* the swapchain doesn't wait for the monitor (MAILBOX / IMMEDIATE wanted) */
static int swap_dlssg; /* made while DLSS frame generation was chosen: made again when the player switches it (NVIDIA),
                       * not when a menu pauses it (that rebuild was a 13-43 ms hitch per menu) */

static int free_running(void) {
    int hz = framerate_hz();
    return !hz || hz > 60 || gpu_fg_active() || dlssg_chosen();  /* frame generation paces its frames itself; vsync (-1): FIFO */
}

static VkBuffer staging;
static VkDeviceMemory staging_mem, frame_mem;
static void *staging_ptr;
static VkImage frame;
static uint32_t frame_w, frame_h;

/* Overlays drawn by the presenting thread on the CPU (hud.c) and copied onto the window: the performance
 * HUD (hud.h: F3 or YW4_HUD=1) and the settings panel (settings.h: F1). */
typedef struct {
    uint32_t w, h;  /* the image's size; what is drawn is at its top-left */
    VkImage image;
    VkBuffer buffer;  /* mapped staging copy (ptr) */
    VkDeviceMemory image_mem, buffer_mem;
    void *ptr;
} Overlay;
enum { HUD_W = 256 * 4, HUD_H = 128 * 4 };  /* the HUD drawn at its scale: up to 4x its 1x size */
/* (the "SWITCHING FRAME GENERATION..." notice is gone: owner, 2026-11-11; the switch's safe frames still run) */
static Overlay panel = {480, 480};
/* The performance HUD (F3; the in-game overlay's Performance page): hud_on 0 off, 1 detailed, 2 compact (-1:
 * not read yet, YW4_HUD); its corner (0 top left, 1 top right, 2 bottom left, 3 bottom right, YW4_HUD_CORNER)
 * and in the detailed one its parts (HUD_PART_*, YW4_HUD_PARTS). Saved with the settings. */
static int hud_on = -1, hud_last = 1, hud_corner = -1, hud_parts = -1;

void gpu_request_trace(int draws);  /* gpu_draw.c */
void gpu_request_dump(void);        /* gpu.c */
void shadow_toggle(void);           /* shadow.c */
static int screenshot_requested;  /* F12 */
static char screenshot_dir[512] = "work/screenshots";

static int vk_fail(const char *what, VkResult r) {
    fprintf(stderr, "yw4: window: %s failed (VkResult %d)\n", what, (int)r);
    return -1;
}

uint32_t vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if (bits & (1u << i) && (p.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int create_swapchain(uint64_t frame) {  /* frame: the game frame being presented, for the log */
    int64_t t0 = host_now_ns();
    vkDeviceWaitIdle(vk.device);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    if (!extent.width || !extent.height) return -1;  /* minimised: try again later */
    uint32_t nfmt = 0;
    VkSurfaceFormatKHR fmts[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, surface, &nfmt, NULL);
    if (nfmt > 64) nfmt = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, surface, &nfmt, fmts);
    /* UNORM in the plain sRGB colour space: the game's bytes are shown as they are, as on the Switch. The same
     * UNORM format can be listed again with other colour spaces (HDR, extended or linear ones: Linux compositors
     * with colour management); those make the screen read the bytes differently, and an _SRGB format would encode
     * them a second time: both look pale (issue #3). So the colour space must be SRGB_NONLINEAR too. */
    int pick = -1;
    for (uint32_t i = 0; i < nfmt && pick < 0; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            pick = (int)i;
    int second = 0;  /* without one: a UNORM format in any colour space, else the driver's first */
    for (uint32_t i = nfmt; i-- > 0;)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) second = (int)i;
    VkSurfaceFormatKHR fmt = fmts[pick < 0 ? second : pick];
    static int fmt_logged;
    if (!fmt_logged || pick < 0) {  /* once (the swapchain is made again on every resize), or each time it's wrong */
        fmt_logged = 1;
        char list[64 * 24] = "", *p = list;
        for (uint32_t i = 0; i < nfmt && p < list + sizeof list - 24; i++)
            p += snprintf(p, (size_t)(list + sizeof list - p), "%s%d/%d", i ? " " : "", (int)fmts[i].format,
                          (int)fmts[i].colorSpace);
        fprintf(stderr, "yw4: window: screen formats (format/colour space) %s; using %d/%d%s\n", list,
                (int)fmt.format, (int)fmt.colorSpace,
                pick < 0 ? ": WARNING no 8-bit UNORM in sRGB colour space, colours may look wrong" : "");
    }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    /* FIFO waits for the monitor's refresh. Above 60 frames/s or with no limit (framerate.c) MAILBOX, else
     * IMMEDIATE, when the driver has them, so the frame rate isn't capped at the refresh rate. */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    swap_free = free_running();
    swap_dlssg = dlssg_chosen();
    if (swap_free) {
        uint32_t nmodes = 0;
        VkPresentModeKHR modes[16];
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, surface, &nmodes, NULL);
        if (nmodes > 16) nmodes = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, surface, &nmodes, modes);
        for (uint32_t i = 0; i < nmodes; i++)
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = modes[i];
        for (uint32_t i = 0; i < nmodes; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    }
    swap_readable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        /* copies; the HUD drawn; read back where it can be (F2's quick look blurs the picture behind its card) */
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode,  /* FIFO is always available; the game paces itself to 30 */
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(vk.device, &ci, NULL, &swapchain);
    if (old) vkDestroySwapchainKHR(vk.device, old, NULL);
    if (r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return vk_fail("vkCreateSwapchainKHR", r);
    }
    /* the count first, then the images (Streamline's version, DLSS frame generation, wants it this way) */
    nswap = 0;
    vkGetSwapchainImagesKHR(vk.device, swapchain, &nswap, NULL);
    if (nswap > 8) nswap = 8;
    vkGetSwapchainImagesKHR(vk.device, swapchain, &nswap, swap_images);
    for (uint32_t i = 0; i < 8; i++) {  /* the views of the images, made again with them */
        if (swap_views[i]) vkDestroyImageView(vk.device, swap_views[i], NULL), swap_views[i] = VK_NULL_HANDLE;
        if (i >= nswap) continue;
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = swap_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCreateImageView(vk.device, &vi, NULL, &swap_views[i]);
    }
    for (uint32_t i = 0; i < nswap; i++) {
        if (rendered[i]) vkDestroySemaphore(vk.device, rendered[i], NULL);
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(vk.device, &si, NULL, &rendered[i]);
    }
    swap_extent = extent;
    swap_format = fmt.format;
    swapchain_stale = 0;
    fprintf(stderr, "yw4: window: swapchain %ux%u, %u images, %s%s, frame %" PRIu64 ", %.1f ms\n", extent.width,
            extent.height, nswap,
            mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (waits for the refresh)" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                                                                                                        : "IMMEDIATE",
            old ? " (made again)" : "", frame, (double)(host_now_ns() - t0) / 1e6);
    return 0;
}

/* Start-up order (2026-11-11). Streamline's start (slInit, ~1.4 s) needs nothing of ours, so it runs on a thread
 * of its own from the beginning of main(), beside the loading of the game's code, the RomFS scan and SDL's start;
 * vk_init waits for it. And the guest's thread starts as soon as the window exists, beside vk_init: the game boots
 * for ~2 s before its first command list (gpu.c, gpu_ready). YW4_BOOT_PARALLEL=0: one after the other, as before. */
static HostLock sl_lock = HOST_LOCK_INIT;
static HostCond sl_cond = HOST_COND_INIT;
static int sl_state;  /* 0 not on a thread, 1 starting, 2 done */

static int boot_parallel(void) {
    const char *e = getenv("YW4_BOOT_PARALLEL");
    return !(e && *e == '0');
}

static void sl_thread(void *arg) {
    (void)arg;
    dlssg_start();
    host_lock(&sl_lock);
    sl_state = 2;
    host_cond_broadcast(&sl_cond);
    host_unlock(&sl_lock);
}

void window_prestart(void) {
    const char *headless = getenv("YW4_HEADLESS");
    if ((headless && *headless && *headless != '0') || !boot_parallel()) return;
    sl_state = 1;
    if (host_thread_start(sl_thread, NULL, 4 << 20)) sl_state = 0;
}

static int vk_init(void) {
    /* DLSS frame generation (gpu_dlssg.cpp): Vulkan through Streamline, which adds what it needs */
    yw4_boot_mark("SDL and the window ready");
    host_lock(&sl_lock);  /* Streamline is starting on its own thread since main() (window_prestart) */
    while (sl_state == 1) host_cond_wait(&sl_cond, &sl_lock, -1);
    host_unlock(&sl_lock);
    int sl = dlssg_start();
    yw4_boot_mark("Streamline started (or left out)");
    PFN_vkGetInstanceProcAddr gipa =
        sl ? dlssg_loader() : (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) {
        fprintf(stderr, "yw4: window: no Vulkan loader (%s)\n", SDL_GetError());
        return -1;
    }
#define LOAD_GLOBAL(f) f = (PFN_##f)gipa(NULL, #f);
    VK_GLOBAL_FNS(LOAD_GLOBAL)
    Uint32 next = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&next);
    const char *layers[1];
    uint32_t nlayers = 0;
    const char *v = getenv("YW4_VULKAN_VALIDATION");
    if (v && *v && *v != '0') {
        uint32_t n = 0;
        VkLayerProperties props[128];
        vkEnumerateInstanceLayerProperties(&n, NULL);
        if (n > 128) n = 128;
        vkEnumerateInstanceLayerProperties(&n, props);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(props[i].layerName, "VK_LAYER_KHRONOS_validation")) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
        fprintf(stderr, "yw4: window: Vulkan validation %s\n", nlayers ? "on" : "asked for, but the layer isn't installed");
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "yw4",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = next, .ppEnabledExtensionNames = exts,
                                .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ici, NULL, &vk.instance);
    if (r != VK_SUCCESS) return vk_fail("vkCreateInstance", r);
    yw4_boot_mark("Vulkan instance");
#define LOAD_INSTANCE(f) f = (PFN_##f)gipa(vk.instance, #f);
    VK_INSTANCE_FNS(LOAD_INSTANCE)
    if (sl)  /* the surface through Streamline too (SDL would make it with its own loader) */
        surface = dlssg_create_surface(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL));
    if (!surface && !SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &surface)) {
        fprintf(stderr, "yw4: window: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return -1;
    }

    /* A GPU with a queue that draws and presents to this window; a discrete one if there is one. */
    uint32_t ngpu = 16;
    VkPhysicalDevice gpus[16];
    vkEnumeratePhysicalDevices(vk.instance, &ngpu, gpus);
    int best = -1;
    for (uint32_t i = 0; i < ngpu; i++) {
        uint32_t nq = 32;
        VkQueueFamilyProperties qs[32];
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, qs);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = 0;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, surface, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(gpus[i], &p);
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) best = score, vk.gpu = gpus[i], vk.queue_family = q;
            break;
        }
    }
    if (best < 0) {
        fprintf(stderr, "yw4: window: no Vulkan GPU can draw to this window\n");
        return -1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk.gpu, &props);
    vk.props = props;
    fprintf(stderr, "yw4: window: Vulkan on %s (API %u.%u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    if (props.apiVersion < VK_API_VERSION_1_3) {
        fprintf(stderr, "yw4: window: the draw path needs Vulkan 1.3 (decision 0021)\n");
        return -1;
    }
    {  /* the driver and the window system, for reports from other PCs (issue #3: RADV on Linux) */
        VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
        vkGetPhysicalDeviceProperties2(vk.gpu, &p2);
        const char *wsys = SDL_GetCurrentVideoDriver();
        fprintf(stderr, "yw4: window: driver %s, %s (id %d), window system %s\n", dp.driverName, dp.driverInfo,
                (int)dp.driverID, wsys ? wsys : "?");
    }
    static const char *dexts[24] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};  /* kept: vk.device_exts */
    uint32_t ndexts = 1;
    /* NVIDIA DLSS's device extensions (gpu_dlss.c, decision 0040), those the GPU has */
    const char **dlss_exts;
    unsigned ndlss = 0;
    gpu_dlss_device_extensions(props.vendorID, &dlss_exts, &ndlss);
    VkPhysicalDeviceVulkan12Features q12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q12};
    vkGetPhysicalDeviceFeatures2(vk.gpu, &q);
    int core_bda = q12.bufferDeviceAddress;
    /* DLSS lists the old VK_EXT_buffer_device_address, but allocates with the core feature's flag (validation,
     * 2026-10-29): the core feature is kept where the GPU has it (Vulkan 1.2), the EXT only without it */
    int bda_ext = 0, has_present_id = 0, has_derivatives = 0, has_conditional = 0;
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n, sizeof *e);
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, e);
        for (uint32_t i = 0; i < n; i++) {
            if (!strcmp(e[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) vk.host_import = 1;
            if (!strcmp(e[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) vk.memory_budget = 1;
            if (!strcmp(e[i].extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME)) has_present_id = 1;
            if (!strcmp(e[i].extensionName, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) has_derivatives = 1;
            if (!strcmp(e[i].extensionName, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)) has_conditional = 1;
        }
        for (unsigned k = 0; k < ndlss && ndexts < 20; k++)
            for (uint32_t i = 0; i < n; i++)
                if (!strcmp(e[i].extensionName, dlss_exts[k])) {
                    int bda = !strcmp(dlss_exts[k], "VK_EXT_buffer_device_address");
                    if (bda && core_bda) break;
                    dexts[ndexts++] = dlss_exts[k];
                    bda_ext |= bda;
                    break;
                }
        free(e);
    }
    if (vk.host_import) {
        dexts[ndexts++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp};
        vkGetPhysicalDeviceProperties2(vk.gpu, &p2);
        vk.host_import_align = hp.minImportedHostPointerAlignment;
    }
    fprintf(stderr, "yw4: window: host memory import: %s\n", vk.host_import ? "yes" : "no (no draw path)");
    if (vk.memory_budget && ndexts < 24) dexts[ndexts++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;  /* gpu_vram */
    else vk.memory_budget = 0;

    /* Features: what the translated shaders (0020) and NVN's state use, where the GPU has it. */
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    /* the extensions' own feature structs, asked about only where the GPU has the extension */
    VkPhysicalDevicePresentIdFeaturesKHR fpid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR fder = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT fcond = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
    if (has_conditional) fcond.pNext = have.pNext, have.pNext = &fcond;
    if (has_present_id) fpid.pNext = have.pNext, have.pNext = &fpid;
    if (has_derivatives) fder.pNext = have.pNext, have.pNext = &fder;
    vkGetPhysicalDeviceFeatures2(vk.gpu, &have);
    const VkPhysicalDeviceFeatures *h = &have.features;
    vk.sampler_anisotropy = h->samplerAnisotropy;
    /* 16-bit floats and subgroup sizes: not ours, AMD FSR's fast shader variants (gpu_fsr.c, decision 0039) */
    VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = f13.dynamicRendering,
                                            .synchronization2 = f13.synchronization2,
                                            .subgroupSizeControl = f13.subgroupSizeControl,
                                            .computeFullSubgroups = f13.computeFullSubgroups,
                                            /* shaders compiled for Vulkan 1.3 turn discard into demote */
                                            .shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation};
    VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13,
                                            .timelineSemaphore = f12.timelineSemaphore,
                                            .shaderFloat16 = f12.shaderFloat16, .shaderInt8 = f12.shaderInt8,
                                            .bufferDeviceAddress = f12.bufferDeviceAddress};
    VkPhysicalDeviceVulkan11Features e11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12,
                                            .shaderDrawParameters = f11.shaderDrawParameters,
                                            .storageBuffer16BitAccess = f11.storageBuffer16BitAccess,
                                            .uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess};
    /* the old EXT buffer-address extension (DLSS asks for it) may not be enabled alongside the core feature: its own
     * feature struct then */
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT ebda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT, .pNext = &e11,
        .bufferDeviceAddress = VK_TRUE};
    if (bda_ext) e12.bufferDeviceAddress = VK_FALSE;
    VkPhysicalDeviceFeatures2 enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = bda_ext ? (void *)&ebda : (void *)&e11,
        .features = {.geometryShader = h->geometryShader, .independentBlend = h->independentBlend,
                     .depthClamp = h->depthClamp, .depthBiasClamp = h->depthBiasClamp,
                     .fillModeNonSolid = h->fillModeNonSolid, .samplerAnisotropy = h->samplerAnisotropy,
                     .fragmentStoresAndAtomics = h->fragmentStoresAndAtomics,
                     .vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics,
                     .shaderStorageImageWriteWithoutFormat = h->shaderStorageImageWriteWithoutFormat,
                     .imageCubeArray = h->imageCubeArray, .dualSrcBlend = h->dualSrcBlend, .logicOp = h->logicOp,
                     .multiDrawIndirect = h->multiDrawIndirect, .textureCompressionBC = h->textureCompressionBC,
                     .occlusionQueryPrecise = h->occlusionQueryPrecise}};
    /* the game's occlusion queries and the draws it makes conditional on them (gpu_query.c) */
    vk.occlusion_precise = h->occlusionQueryPrecise;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT econd = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, .conditionalRendering = VK_TRUE};
    if (has_conditional && fcond.conditionalRendering && ndexts < 24) {
        dexts[ndexts++] = VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME;
        econd.pNext = (void *)enable.pNext, enable.pNext = &econd;
        vk.conditional_rendering = 1;
    }
    /* NVIDIA's libraries, not our shaders (validation, 2026-11-14). Streamline adds the extensions it lists itself
     * while it wraps vkCreateDevice, but not VK_KHR_present_id, which its VK_NV_low_latency2 (Reflex) requires; it
     * also asks for descriptor indexing and makes private-data slots. DLSS's compute shaders use 16-bit integers
     * and compute-shader derivatives. Each only where the GPU has it. */
    VkPhysicalDevicePresentIdFeaturesKHR epid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                                                 .presentId = VK_TRUE};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR eder = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,
        .computeDerivativeGroupLinear = VK_TRUE};
    if (sl) {
        e13.privateData = f13.privateData;
        e12.descriptorIndexing = f12.descriptorIndexing;
        if (has_present_id && fpid.presentId && ndexts < 24) {
            dexts[ndexts++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            epid.pNext = (void *)enable.pNext, enable.pNext = &epid;
        }
    }
    if (ndlss) {
        enable.features.shaderInt16 = h->shaderInt16;
        if (has_derivatives && fder.computeDerivativeGroupLinear && ndexts < 24) {
            dexts[ndexts++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
            eder.pNext = (void *)enable.pNext, enable.pNext = &eder;
        }
    }
    if (!f13.dynamicRendering || !f12.timelineSemaphore || !f11.shaderDrawParameters) {
        fprintf(stderr, "yw4: window: the GPU lacks dynamic rendering, timeline semaphores or draw parameters\n");
        return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk.queue_family,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = ndexts, .ppEnabledExtensionNames = dexts};
    {  /* for reports: what Streamline asks for (it adds these extensions itself while it wraps vkCreateDevice) */
        const char *se[32], *sf[32];
        unsigned nse = 0, nsf = 0;
        dlssg_device_requirements(se, 32, &nse, sf, 32, &nsf);
        if (nse) {
            fprintf(stderr, "yw4: window: Streamline asks for (%u):", nse);
            for (unsigned i = 0; i < nse; i++) fprintf(stderr, " %s", se[i]);
            fprintf(stderr, "; features (%u):", nsf);
            for (unsigned i = 0; i < nsf; i++) fprintf(stderr, " %s", sf[i]);
            fputc('\n', stderr);
        }
    }
    {  /* the device extensions, for reports (and what DLSS asked for that the GPU lacks or didn't fit) */
        fprintf(stderr, "yw4: window: device extensions (%u):", ndexts);
        for (uint32_t i = 0; i < ndexts; i++) fprintf(stderr, " %s", dexts[i]);
        fputc('\n', stderr);
        for (unsigned k = 0; k < ndlss; k++) {
            uint32_t i = 0;
            while (i < ndexts && strcmp(dexts[i], dlss_exts[k])) i++;
            if (i == ndexts) fprintf(stderr, "yw4: window: DLSS's %s left out\n", dlss_exts[k]);
        }
    }
    r = vkCreateDevice(vk.gpu, &dci, NULL, &vk.device);
    if (r != VK_SUCCESS) return vk_fail("vkCreateDevice", r);
    vk.device_exts = dexts, vk.ndevice_exts = ndexts;
    vk.shader_f16 = e12.shaderFloat16, vk.shader_i16 = enable.features.shaderInt16, vk.sync2 = e13.synchronization2;
    yw4_boot_mark("Vulkan device");
#define LOAD_DEVICE(f) f = (PFN_##f)vkGetDeviceProcAddr(vk.device, #f);
    VK_DEVICE_FNS(LOAD_DEVICE)
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    if (sl) dlssg_available();  /* checked and logged once, Reflex set up */
    yw4_boot_mark("DLSS frame generation and Reflex checked");
    gpu_dlss_available();
    yw4_boot_mark("DLSS (NGX) checked");
    __atomic_store_n(&vk.up, 1, __ATOMIC_RELEASE);
    {  /* a new install's first start: graphics for this GPU (settings_first_run), told in a notice */
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(vk.gpu, &mp);
        VkDeviceSize vram = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && mp.memoryHeaps[i].size > vram)
                vram = mp.memoryHeaps[i].size;
        int integrated = vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        int low = integrated || vram < (VkDeviceSize)4 << 30;
        fprintf(stderr, "yw4: window: %s GPU, %.1f GB of video memory\n", integrated ? "an integrated" : "a dedicated",
                (double)vram / (1 << 30));
        if (settings_first_run(low)) {
            const char *force = getenv("YW4_FIRST_RUN_LEVEL");
            if (force && *force) low = !strcmp(force, "low");
            window_notice(!low, low ? "Lighter graphics for this PC" : "High graphics for this PC",
                          "%s \xc2\xb7 Change them in Settings", vk.props.deviceName);
        }
        window_notice_keys();  /* each start: how to see the controls (after the graphics card, when there is one) */
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = vk.queue_family};
    vkCreateCommandPool(vk.device, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers(vk.device, &cai, &cmd);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(vk.device, &fci, NULL, &fence);
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(vk.device, &sci, NULL, &acquired);
    int failed = create_swapchain(0);
    yw4_boot_mark("swapchain");
    return failed;
}

/* The staging buffer and the image a w x h frame goes through. */
static int frame_resources(uint32_t w, uint32_t h) {
    if (frame && w == frame_w && h == frame_h) return 0;
    vkDeviceWaitIdle(vk.device);
    if (frame) vkDestroyImage(vk.device, frame, NULL), vkFreeMemory(vk.device, frame_mem, NULL);
    if (staging) vkDestroyBuffer(vk.device, staging, NULL), vkFreeMemory(vk.device, staging_mem, NULL);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    vkCreateBuffer(vk.device, &bci, NULL, &staging);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, staging, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(vk.device, &mai, NULL, &staging_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(vk.device, staging, staging_mem, 0);
    vkMapMemory(vk.device, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_ptr);
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    vkCreateImage(vk.device, &ici, NULL, &frame);
    vkGetImageMemoryRequirements(vk.device, frame, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &mai, NULL, &frame_mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(vk.device, frame, frame_mem, 0);
    frame_w = w, frame_h = h;
    return 0;
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    /* ALL_COMMANDS: also waits for the draw path's earlier submissions writing `image` */
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

static const char *shot_suffix = "";  /* in the file name after the frame number (frame generation's "_gen") */

static void save_screenshot(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
    char path[600];
    snprintf(path, sizeof path, "%s", screenshot_dir);  /* make the folder (and its parents) if missing */
    for (char *p = path + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            MKDIR(path);
            *p = c;
        }
    MKDIR(path);
    snprintf(path, sizeof path, "%s/frame_%06" PRIu64 "%s.bmp", screenshot_dir, n, shot_suffix);
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (s && SDL_SaveBMP(s, path)) fprintf(stderr, "yw4: screenshot %s\n", path), yw4_event_text("screenshot", n, "path", path);
    else fprintf(stderr, "yw4: screenshot %s failed: %s\n", path, SDL_GetError());
    if (s) SDL_DestroySurface(s);
}

/* F12: this many frames in a row (YW4_SCREENSHOT_BURST, default 1), to catch short glitches */
static int screenshot_burst(void) {
    const char *s = getenv("YW4_SCREENSHOT_BURST");
    int n = s ? atoi(s) : 1;
    return n > 0 ? n : 1;
}

void window_screenshot_suffix(const char *suffix) { shot_suffix = suffix ? suffix : ""; }

static int screenshot_wanted(uint64_t n) {
    int left = __atomic_load_n(&screenshot_requested, __ATOMIC_ACQUIRE);
    if (left > 0) {
        __atomic_store_n(&screenshot_requested, left - 1, __ATOMIC_RELEASE);
        return 1;
    }
    const char *list = getenv("YW4_SCREENSHOTS");  /* frame numbers, comma separated */
    for (const char *p = list; p && *p;) {
        char *end;
        unsigned long long f = strtoull(p, &end, 10);
        if (end == p) break;
        if (f == n) return 1;
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

static int hud_mode_read(void) {
    int on = __atomic_load_n(&hud_on, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("YW4_HUD");
        int want = e && *e ? atoi(e) : 0;  /* the old YW4_HUD=1: the detailed one */
        if (want < 0 || want > 2) want = 1;
        if (want) hud_last = want;
        __atomic_compare_exchange_n(&hud_on, &on, want, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        on = __atomic_load_n(&hud_on, __ATOMIC_ACQUIRE);
    }
    return on;
}

static int hud_enabled(void) { return hud_mode_read() > 0; }

int window_hud_mode(void) { return hud_mode_read(); }
void window_hud_set_mode(int m) {
    if (m) hud_last = m;
    __atomic_store_n(&hud_on, m, __ATOMIC_RELEASE);
}
int window_hud_corner(void) {
    if (hud_corner < 0) {
        const char *e = getenv("YW4_HUD_CORNER");
        hud_corner = e && *e ? atoi(e) & 3 : 0;
    }
    return hud_corner;
}
void window_hud_set_corner(int c) { hud_corner = c & 3; }
unsigned window_hud_parts(void) {
    if (hud_parts < 0) {
        const char *e = getenv("YW4_HUD_PARTS");
        hud_parts = e && *e ? (int)(strtoul(e, NULL, 0) & HUD_PART_ALL) : HUD_PART_ALL;
    }
    return (unsigned)hud_parts;
}
void window_hud_set_parts(unsigned p) { hud_parts = (int)(p & HUD_PART_ALL); }

/* An overlay's staging buffer (mapped) and image, made on first use. 0 when ready. */
static int overlay_resources(Overlay *o) {
    if (o->image) return 0;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)o->w * o->h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(vk.device, &bci, NULL, &o->buffer) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, o->buffer, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(vk.device, &mai, NULL, &o->buffer_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(vk.device, o->buffer, o->buffer_mem, 0);
    vkMapMemory(vk.device, o->buffer_mem, 0, VK_WHOLE_SIZE, 0, &o->ptr);
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {o->w, o->h, 1}, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image;
    if (vkCreateImage(vk.device, &ici, NULL, &image) != VK_SUCCESS) return -1;
    vkGetImageMemoryRequirements(vk.device, image, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &mai, NULL, &o->image_mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(vk.device, image, o->image_mem, 0);
    o->image = image;
    return 0;
}

/* Draws the HUD's lines into its buffer and records copying them onto dst (in TRANSFER_DST
 * layout) at the frame's top-left corner (x0, y0), scaled up with the window. The previous
 * present's fence has been waited for, so the buffer is free to write. */
static int hud_shown_w, hud_shown_h;  /* the part of hud.ptr drawn at the last present (0: none) */
/* What the HUD showed at the last present, and the scale and picture height it was drawn for: a screenshot draws
 * it again at the frame's own scale (hud_onto), so a 4K shot from a smaller window doesn't get a tiny HUD */
static struct {
    HudContent content;
    int compact;
    unsigned parts;
    float s;
    uint32_t bh;
} hud_shot;

/* Records copying an overlay's drawn w x h part onto dst (in TRANSFER_DST layout) with its top-left at
 * (x, y), each texel s x s window pixels: whole steps stay sharp, between them it is filtered. */
static void overlay_show(Overlay *o, VkImage dst, int w, int h, int32_t x, int32_t y, float s) {
    barrier(o->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferRowLength = o->w, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyBufferToImage(cmd, o->buffer, o->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(o->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int32_t xe = x + (int32_t)(w * s), ye = y + (int32_t)(h * s);  /* clipped to the window */
    if (xe > (int32_t)swap_extent.width) xe = (int32_t)swap_extent.width;
    if (ye > (int32_t)swap_extent.height) ye = (int32_t)swap_extent.height;
    if (xe <= x || ye <= y) return;
    int32_t sw = (int32_t)((xe - x) / s), sh = (int32_t)((ye - y) / s);  /* the overlay's texels that fit */
    if (sw <= 0 || sh <= 0) return;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x, y, 0}, {x + (int32_t)(sw * s), y + (int32_t)(sh * s), 1}}};
    vkCmdBlitImage(cmd, o->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, s == (float)(int32_t)s ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

/* The HUD's scale (the settings panel's HUD SCALE slider, YW4_HUD_SCALE=auto or a number): 0 is AUTO, which grows
 * with the window in whole steps (1 per 720 lines from 1440, else 1); anything else is that scale. */
static float hud_scale = -1.0f;

float hud_scale_get(void) {
    float v;
    __atomic_load(&hud_scale, &v, __ATOMIC_ACQUIRE);  /* the generic form: _n takes no float before Clang 19 */
    if (v < 0) {
        const char *e = getenv("YW4_HUD_SCALE");
        v = e && *e && strcmp(e, "auto") ? (float)atof(e) : 0.0f;
        if (v < 0) v = 0;
        __atomic_store(&hud_scale, &v, __ATOMIC_RELEASE);
    }
    return v;
}

void hud_scale_set(float v) { __atomic_store(&hud_scale, &v, __ATOMIC_RELEASE); }

static float overlay_scale(uint32_t h) {
    float s = hud_scale_get();
    return s > 0 ? s : h >= 1440 ? (float)(h / 720) : 1.0f;
}

/* where the HUD (w x h) goes in a picture bw x bh at the HUD's scale s: its corner, 6 units in */
static void hud_place(int w, int h, uint32_t bw, uint32_t bh, float s, int32_t *x, int32_t *y) {
    int c = window_hud_corner();
    *x = c & 1 ? (int32_t)bw - w - (int32_t)(6 * s) : (int32_t)(6 * s);
    *y = c & 2 ? (int32_t)bh - h - (int32_t)(6 * s) : (int32_t)(6 * s);
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

/* ---- the HUD blended onto the picture (hud_style.c draws it see-through, premultiplied). The window's other
 * overlays are copies, which can't blend, so this one is drawn: its image as a texture, two triangles over its
 * corner, blended src + dst * (1 - src.a), into the window's image (swap_views). */
VkShaderModule gpu_native_shader(const char *name);  /* gpu_aa.c: our SPIR-V from work/build/shaders */

static struct {
    VkBuffer buf;          /* the upload: drawn into `cpu` (fast memory), then copied here */
    VkDeviceMemory buf_mem;
    void *ptr;
    uint8_t *cpu;
    VkImage image;         /* HUD_W x HUD_H, sampled */
    VkDeviceMemory mem;
    VkImageView view;
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    struct { VkFormat format; VkPipeline pipe; } pipes[4];  /* one per window format (kept: it rarely changes) */
    int npipes, failed;
} hb;

static void hb_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

static int hb_resources(void) {
    if (hb.set) return 0;
    if (hb.failed) return -1;
    hb.failed = 1;  /* until everything below is made */
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)HUD_W * HUD_H * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(vk.device, &bci, NULL, &hb.buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, hb.buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(vk.device, &mai, NULL, &hb.buf_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(vk.device, hb.buf, hb.buf_mem, 0);
    vkMapMemory(vk.device, hb.buf_mem, 0, VK_WHOLE_SIZE, 0, &hb.ptr);
    if (!(hb.cpu = malloc((size_t)HUD_W * HUD_H * 4))) return -1;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {HUD_W, HUD_H, 1}, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(vk.device, &ici, NULL, &hb.image) != VK_SUCCESS) return -1;
    vkGetImageMemoryRequirements(vk.device, hb.image, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &mai, NULL, &hb.mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(vk.device, hb.image, hb.mem, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hb.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(vk.device, &vi, NULL, &hb.view) != VK_SUCCESS) return -1;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (vkCreateSampler(vk.device, &si, NULL, &hb.sampler) != VK_SUCCESS) return -1;
    VkDescriptorSetLayoutBinding bd = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &bd};
    if (vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &hb.dsl) != VK_SUCCESS) return -1;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    if (vkCreateDescriptorPool(vk.device, &pci, NULL, &hb.pool) != VK_SUCCESS) return -1;
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &hb.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    if (vkCreatePipelineLayout(vk.device, &pli, NULL, &hb.layout) != VK_SUCCESS) return -1;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = hb.pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &hb.dsl};
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(vk.device, &dai, &set) != VK_SUCCESS) return -1;
    VkDescriptorImageInfo ii = {hb.sampler, hb.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii};
    vkUpdateDescriptorSets(vk.device, 1, &wr, 0, NULL);
    hb.set = set, hb.failed = 0;
    return 0;
}

static VkPipeline hb_pipeline(VkFormat format) {
    for (int i = 0; i < hb.npipes; i++)
        if (hb.pipes[i].format == format) return hb.pipes[i].pipe;
    if (hb.npipes == 4) return VK_NULL_HANDLE;
    static VkShaderModule vs, fs;
    if (!vs) vs = gpu_native_shader("ui_quad.vert"), fs = gpu_native_shader("ui_blend.frag");
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
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
    VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                               .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                               .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                               .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                               .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &format};
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vin, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy,
        .layout = hb.layout};
    VkPipeline pipe;
    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &gci, NULL, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
    hb.pipes[hb.npipes].format = format, hb.pipes[hb.npipes].pipe = pipe;
    hb.npipes++;
    return pipe;
}

/* ---- a notice over the game (window_notice): a small card at the top of the picture that fades in, stays about six
 * seconds and fades out. Used for the first start's graphics (settings_first_run). Drawn with the HUD's blend
 * pipeline (its layout, sampler and pipelines) from an image of its own. */
enum { NOTE_W = 2048, NOTE_H = 256 };
static struct {
    VkBuffer buf;
    VkDeviceMemory buf_mem;
    void *ptr;
    uint8_t *cpu;  /* the card at full strength (premultiplied RGBA); faded into ptr each frame */
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    int failed, w, h;  /* w x h: the card's pixels, at its top-left */
    float s;           /* the scale it was drawn at */
} nb;
static char note_title[96], note_text[256];
static int note_on, note_high;
static int note_keys, note_keys_wait, note_pad, note_style;  /* the controls card: showing, waiting its turn, the device and the controller make it names */
static uint64_t note_start;  /* SDL ticks of its first frame (0: not shown yet) */
static int note_bottom;      /* pixels from the card's image top to the card's bottom (a shadow margin below) */

static void window_notice(int high, const char *title, const char *fmt, ...) {
    snprintf(note_title, sizeof note_title, "%s", title);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(note_text, sizeof note_text, fmt, ap);
    va_end(ap);
    note_high = high, note_keys = 0, note_start = 0, nb.s = 0, note_on = 1;
    fprintf(stderr, "yw4: window: notice: %s. %s\n", note_title, note_text);
}

/* The controls card (the owner's pick, option A of the mockup, 2026-11-13; the launcher's Controls hint setting): one line
 * that points at the quick look, F2 on a keyboard, the held Back button on a controller (pad_back_hold), with that
 * button's name as the controller prints it. It follows the device while it shows: the first touch of a pad or a
 * key rewrites it. It waits for a card that is showing (the first start's graphics) to fade first. */
static int note_keys_pad(void);  /* 1: the controller's words (below, with the input) */
static void note_keys_words(int pad) {
    static const char *const back[] = {"View", "Create", "Minus"};  /* Xbox, PlayStation, Nintendo */
    if (pad) snprintf(note_title, sizeof note_title, "Hold %s to see the buttons", back[bindings_pad_style() % 3]);
    else snprintf(note_title, sizeof note_title, "Press F2 to see the keys");
    snprintf(note_text, sizeof note_text, "%s \xc2\xb7 Change them in Controls", pad ? "Controller" : "Keyboard and mouse");
    note_pad = pad, note_style = bindings_pad_style(), nb.s = 0;
}
static void note_keys_start(void) {
    note_keys_wait = 0, note_keys = 1, note_start = 0, note_on = 1;
    note_keys_words(note_keys_pad());
    fprintf(stderr, "yw4: window: notice: %s. %s\n", note_title, note_text);
}
static void window_notice_keys(void) {
    if (!settings_keys_notice() || settings_legacy_keys()) return;  /* the old menus: no quick look to point at */
    if (note_on) note_keys_wait = 1;
    else note_keys_start();
}

static int nb_resources(void) {
    if (nb.set) return 0;
    if (nb.failed) return -1;
    nb.failed = 1;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)NOTE_W * NOTE_H * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(vk.device, &bci, NULL, &nb.buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, nb.buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(vk.device, &mai, NULL, &nb.buf_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(vk.device, nb.buf, nb.buf_mem, 0);
    vkMapMemory(vk.device, nb.buf_mem, 0, VK_WHOLE_SIZE, 0, &nb.ptr);
    if (!(nb.cpu = calloc((size_t)NOTE_W * NOTE_H, 4))) return -1;
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {NOTE_W, NOTE_H, 1}, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(vk.device, &ici, NULL, &nb.image) != VK_SUCCESS) return -1;
    vkGetImageMemoryRequirements(vk.device, nb.image, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &mai, NULL, &nb.mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(vk.device, nb.image, nb.mem, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = nb.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(vk.device, &vi, NULL, &nb.view) != VK_SUCCESS) return -1;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    if (vkCreateDescriptorPool(vk.device, &pci, NULL, &nb.pool) != VK_SUCCESS) return -1;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = nb.pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &hb.dsl};
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(vk.device, &dai, &set) != VK_SUCCESS) return -1;
    VkDescriptorImageInfo ii = {hb.sampler, nb.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii};
    vkUpdateDescriptorSets(vk.device, 1, &wr, 0, NULL);
    nb.set = set, nb.failed = 0;
    return 0;
}

/* The card, at scale s, into nb.cpu (the owner's pick, option C of the mockup, 2026-11-11): a corner toast, a
 * coloured tile on the left with a gauge (gold: lighter graphics, green: high), then a bold title over a softer
 * second line, on the theme's deep violet. The controls card (note_keys) is the same with a lilac tile. A margin around it is
 * left from the shadow it had until 2026-11-13 (the placement counts on it). */
static void note_render(float s) {
    const Theme *th = theme_colors();
    const float m = 24, tile = 84, t1 = 26, l1 = t1 * 1.2f, t2 = 19, l2 = t2 * 1.35f, pt = 16, pl = 22, pr = 28;
    float tw = ui_text_w(UI_ROUNDED_800, t1, note_title), tw2 = ui_text_w(UI_GOTHIC_500, t2, note_text);
    float cw = tile + pl + (tw > tw2 ? tw : tw2) + pr, ch = pt + l1 + l2 + pt;
    if ((cw + 2 * m) * s > NOTE_W) s = NOTE_W / (cw + 2 * m);  /* a long GPU name: smaller rather than cut */
    if ((ch + 2 * m) * s > NOTE_H) s = NOTE_H / (ch + 2 * m);
    memset(nb.cpu, 0, (size_t)NOTE_W * NOTE_H * 4);
    UiCanvas c = {nb.cpu, NOTE_W, NOTE_H, s, 0, 0};
    /* green: high; gold: lighter; the theme's lilac: the controls (not a graphics level) */
    uint32_t tint = note_keys ? th->lilac : note_high ? 0x4cc38a : th->pop_gold;
    ui_box(&c, m, m, cw, ch, 16, th->pop_deep, 0, 0);  /* no shadow under it (owner, 2026-11-13) */
    ui_box(&c, m, m, tile + 16, ch, 16, tint, 0, 0);  /* the tile, its right corners covered next */
    ui_rect(&c, m + tile, m, 16, ch, th->pop_deep, 1);
    /* the gauge's needle: up to the right for high, low on the left for lighter */
    /* the controls: a keyboard, or a controller's outline (two grips) with its d-pad and two buttons; a little
     * larger and finer than the gauge, which has less to show */
    const float isz = note_keys ? 48 : 40;
    ui_icon(&c, note_keys ? (note_pad ? "<path d=\"M6 6h12a4 4 0 0 1 3.9 3.2l1 6.3a3 3 0 0 1-5.3 1.8L15.5 15h-7l-2.1 2.3a3 3 0 0 1-5.3-1.8l1-6.3A4 4 0 0 1 6 6z\"/>"
                                        "<path d=\"M8 9v3M6.5 10.5h3M15.2 11.6h.01M17.8 9.4h.01\"/>"
                                      : "<rect x=\"2.5\" y=\"6\" width=\"19\" height=\"12\" rx=\"2.5\"/>"
                                        "<path d=\"M6.5 10h.01M10 10h.01M13.5 10h.01M17 10h.01M8 14h8\"/>")
                : note_high ? "<path d=\"M4 15a8 8 0 1 1 16 0\"/><path d=\"M12 15l4-5\"/>"
                            : "<path d=\"M4 15a8 8 0 1 1 16 0\"/><path d=\"M12 15l-5.5-2.5\"/>",
            m + (tile - isz) / 2, m + (ch - isz) / 2, isz, note_keys ? 2.0f : 2.6f, th->pop_deep);
    ui_text(&c, UI_ROUNDED_800, t1, m + tile + pl, m + pt, l1, note_title, th->pop_white);
    ui_text(&c, UI_GOTHIC_500, t2, m + tile + pl, m + pt + l1, l2, note_text, th->on_dark);
    nb.w = (int)ceilf((cw + 2 * m) * s), nb.h = (int)ceilf((ch + 2 * m) * s), note_bottom = (int)ceilf((m + ch) * s);
}

static void note_draw(VkImage dst, uint32_t index, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    (void)bw;
    if (!note_on || index >= 8 || !swap_views[index]) return;
    if (hb_resources() || nb_resources()) {
        note_on = note_keys_wait = 0;
        return;
    }
    VkPipeline pipe = hb_pipeline(swap_format);
    if (!pipe) return;
    uint64_t now = SDL_GetTicks();
    if (!note_start) note_start = now;
    const float in = 0.4f, hold = 6.0f, out = 1.2f;
    float t = (float)(now - note_start) / 1000.0f, a = t < in ? t / in : t < in + hold ? 1.0f : 1.0f - (t - in - hold) / out;
    if (a <= 0 && t > in) {
        note_on = 0;
        if (note_keys_wait) note_keys_start();  /* the controls card's turn, from the next frame */
        return;
    }
    /* the other device was touched, or the controller turned out to be another make (it is read after the start) */
    if (note_keys && (note_keys_pad() != note_pad || (note_pad && bindings_pad_style() != note_style)))
        note_keys_words(note_keys_pad());
    float s = overlay_scale(swap_extent.height);
    if (s != nb.s) note_render(s), nb.s = s;
    /* faded: premultiplied, so every channel times the strength */
    uint32_t k = (uint32_t)(a * 256.0f);
    for (int j = 0; j < nb.h; j++) {
        const uint8_t *src = nb.cpu + (size_t)j * NOTE_W * 4;
        uint8_t *row = (uint8_t *)nb.ptr + (size_t)j * NOTE_W * 4;
        for (int i = 0; i < nb.w * 4; i++) row[i] = (uint8_t)((src[i] * k) >> 8);
    }
    hb_barrier(nb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferRowLength = NOTE_W, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(uint32_t)nb.w, (uint32_t)nb.h, 1}};
    vkCmdCopyBufferToImage(cmd, nb.buf, nb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    hb_barrier(nb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    hb_barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = swap_views[index],
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, swap_extent},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = {0, 0, (float)swap_extent.width, (float)swap_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, swap_extent};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hb.layout, 0, 1, &nb.set, 0, NULL);
    float W = (float)swap_extent.width, H = (float)swap_extent.height;
    /* the bottom-left corner: the card 40 units in from the left, 48 up from the bottom (its image has a margin) */
    float px = (float)x0 + (40 - 24) * nb.s, py = (float)y0 + (float)bh - 48 * nb.s - (float)note_bottom;
    float push[8] = {px / W * 2 - 1, py / H * 2 - 1, (px + (float)nb.w) / W * 2 - 1, (py + (float)nb.h) / H * 2 - 1,
                     (float)nb.w / NOTE_W, (float)nb.h / NOTE_H, 0, 0};
    vkCmdPushConstants(cmd, hb.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);
    hb_barrier(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
}

static void hud_draw(VkImage dst, uint32_t index, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    hud_shown_w = hud_shown_h = 0;
    if (!hud_enabled() || hb_resources() || index >= 8 || !swap_views[index]) return;
    VkPipeline pipe = hb_pipeline(swap_format);
    if (!pipe) return;
    /* Drawn again 4 times a second, or at once when its form, parts or size change; the frames between show the
     * last picture. Drawing it costs the present 0.6 ms (detailed, 0.9 before letters were kept: ui.c), about 7% of
     * a frame at 120 frames/s: too much for what measures the game. It also keeps the numbers readable. The owner
     * (2026-11-07): it doesn't have to draw that often (was 10 a second). */
    static HudContent content;
    static int w, h, last_form = -1;
    static unsigned last_parts;
    static float last_s;
    static uint64_t last_at;
    int compact = hud_mode_read() == 2;
    unsigned parts = window_hud_parts();
    float s = overlay_scale(swap_extent.height);
    uint64_t now = SDL_GetTicks();
    int64_t t0 = host_now_ns();
    int redraw = compact != last_form || parts != last_parts || s != last_s || now - last_at >= 250;
    if (redraw) {
        last_form = compact, last_parts = parts, last_s = s, last_at = now;
        yw4_perf_hud(&content, compact, parts);
        hud_style_render(hb.cpu, HUD_W, HUD_H, &content, compact, parts, s, &w, &h);  /* w, h: last time's part */
        for (int j = 0; j < h; j++)  /* the drawn rows into the upload: bulk copies, the mapped memory is slow to read */
            memcpy((uint8_t *)hb.ptr + (size_t)j * HUD_W * 4, hb.cpu + (size_t)j * HUD_W * 4, (size_t)w * 4);
    }
    {  /* YW4_HUD_TIME=1: what the HUD's drawing costs per frame, redraws shared out, every 600 frames in the log */
        static int on = -1, n, redraws;
        static double sum, worst;
        if (on < 0) on = getenv("YW4_HUD_TIME") != NULL;
        double ms = (double)(host_now_ns() - t0) / 1e6;
        if (on && (sum += ms, worst = ms > worst ? ms : worst, redraws += redraw, ++n == 600)) {
            fprintf(stderr, "yw4: hud: %.3f ms per frame on average, %.3f ms at most (600 frames, %d redraws, "
                    "%.3f ms each)\n", sum / n, worst, redraws, redraws ? sum / redraws : 0.0);
            n = redraws = 0, sum = worst = 0;
        }
    }
    hud_shown_w = w, hud_shown_h = h;
    hud_shot.content = content, hud_shot.compact = compact, hud_shot.parts = parts, hud_shot.s = s, hud_shot.bh = bh;
    int32_t hx, hy;
    hud_place(w, h, bw, bh, s, &hx, &hy);
    if (redraw) {
        hb_barrier(hb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {.bufferRowLength = HUD_W, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {(uint32_t)w, (uint32_t)h, 1}};
        vkCmdCopyBufferToImage(cmd, hb.buf, hb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        hb_barrier(hb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    hb_barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = swap_views[index],
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, swap_extent},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = {0, 0, (float)swap_extent.width, (float)swap_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, swap_extent};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hb.layout, 0, 1, &hb.set, 0, NULL);
    float W = (float)swap_extent.width, H = (float)swap_extent.height, px = (float)(x0 + hx), py = (float)(y0 + hy);
    float push[8] = {px / W * 2 - 1, py / H * 2 - 1, (px + (float)w) / W * 2 - 1, (py + (float)h) / H * 2 - 1,
                     (float)w / HUD_W, (float)h / HUD_H, 0, 0};
    vkCmdPushConstants(cmd, hb.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);
    hb_barrier(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
}

/* The HUD as it would show now, drawn into rgba (w x h, straight RGBA: the overlay's Overlay page previews it on
 * the paused game); its size. canvas_s: the image pixels per unit of the page it goes on. Drawn at the size it has
 * against the picture, so the Scale slider shows, as far as the preview's box holds it (1.46 x a page unit: the
 * detailed card just fits). */
void window_hud_preview(uint8_t *rgba, int w, int h, float canvas_s, int *used_w, int *used_h) {
    static HudContent content;
    int compact = hud_mode_read() == 2;
    unsigned parts = window_hud_parts();
    yw4_perf_hud(&content, compact, parts);
    *used_w = *used_h = 0;  /* nothing known drawn before: the whole image is cleared */
    float s = overlay_scale(swap_extent.height), most = 1.46f * canvas_s, fit = (float)w / 235.0f;
    if (s > most) s = most;
    if (s > fit) s = fit;
    hud_style_render(rgba, w, h, &content, compact, parts, s, used_w, used_h);
    for (int j = 0; j < *used_h; j++)  /* premultiplied -> straight, as ui_image takes it */
        for (int i = 0; i < *used_w; i++) {
            uint8_t *p = rgba + ((size_t)j * w + i) * 4;
            if (p[3] && p[3] < 255)
                for (int k = 0; k < 3; k++) p[k] = (uint8_t)(p[k] * 255 / p[3] > 255 ? 255 : p[k] * 255 / p[3]);
        }
}

/* The settings panel (F1), in the middle of the picture (bw x bh at x0, y0). */
static int panel_shown_w, panel_shown_h;  /* the part of panel.ptr drawn at the last present (0: none) */
static uint32_t panel_scale(uint32_t bw, uint32_t bh);  /* below */

static void panel_draw(VkImage dst, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    panel_shown_w = panel_shown_h = 0;
    if (!settings_open() || overlay_resources(&panel)) return;
    int w, h;
    if (settings_page()) {
        static BindRow rows[32];
        char help[256];
        int row, col, n = bindings_rows(rows, 32, &row, &col, help, sizeof help);
        controls_render(panel.ptr, (int)panel.w, (int)panel.h, rows, n, row, col, bindings_capturing(), help, &w, &h);
    } else {
        SettingRow rows[48];
        int sel, n = settings_rows(rows, 32, &sel);
        panel_render(panel.ptr, (int)panel.w, (int)panel.h, rows, n, sel, settings_developer(), &w, &h);
    }
    panel_shown_w = w, panel_shown_h = h;
    /* scaled up by whole steps (the pixel font stays crisp) to about two thirds of the picture's height */
    int32_t s = (int32_t)panel_scale(bw, bh);
    overlay_show(&panel, dst, w, h, x0 + ((int32_t)bw - w * s) / 2, y0 + ((int32_t)bh - h * s) / 2, (float)s);
}

void window_hud_toggle(void) { __atomic_store_n(&hud_on, hud_enabled() ? 0 : hud_last, __ATOMIC_RELEASE); }

/* F3 (owner, 2026-11-11: it cycles the HUD instead of opening the overlay's Overlay page): off, compact, detailed,
 * off again. */
void window_hud_cycle(void) {
    int m = hud_mode_read(), next = m == 0 ? 2 : m == 2 ? 1 : 0;
    window_hud_set_mode(next);
    settings_save();
    fprintf(stderr, "yw4: hud: %s (F3)\n", next == 0 ? "off" : next == 2 ? "compact" : "detailed");
}
int window_hud_shown(void) { return hud_enabled(); }

static void menu_draw(VkImage dst, uint32_t index, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh);  /* below */

/* The in-game overlay's design is 900 units high and at least 1600 wide (16:9; wider pictures give it more width).
 * A picture narrower than 16:9 (the 16:10 aspect ratio) would cut its right side off: there the 1600 x 900 is
 * fitted to the picture's width and centred top to bottom, as the game's UI is (gpu_draw.c). gamemenu_draw moves its
 * frame (tint, side panel, bars) out over the strips above and below, to the picture's edges. For a picture of
 * pw x ph (any units): s, its units per design unit, and (ox, oy), where the design's (0, 0) is. */
static void overlay_fit(float pw, float ph, float *s, float *ox, float *oy) {
    float k = ph / 900.0f;
    *ox = 0, *oy = 0;
    if (pw < 1600.0f * k) k = pw / 1600.0f, *oy = (ph - 900.0f * k) * 0.5f;
    *s = k;
}

/* ---- the overlay's pause (2026-11-05, F4 for now): the game's own pause (Plus: the world, timers and sound
 * stop) with its pause screen hidden (pause.c). Pausing presses Plus for the player and keeps the picture of that
 * moment; while paused the window shows that picture, so the game's "Paused" screen under it never shows, and the
 * player's own input doesn't reach the game (a held stick would move its cursor onto Return to Title). Resuming
 * presses A on its first item, Continue, until the game has left its pause (pause.c sees it), then shows the live
 * picture. Again every 1.2 s: a press while the screen is still opening is lost (at 30 frames/s it opens for about
 * 2 s), and closing takes the game about 0.9 s at 30 frames/s, so a quicker second A could reach the field. Where
 * Plus pauses nothing (a cutscene), the window is live again after two seconds; if the game doesn't resume within
 * 20 A presses, the window shows it (its pause screen, to use by hand). A press lasts until the game has read the
 * input twice (as mousemenu.c; replays count as reads). */
enum { BTN_A_BIT = 1 << 0, BTN_PLUS_BIT = 1 << 10 };
static int paused_view;          /* the window shows the kept picture; the player's input is held back */
static int pause_keep_request;   /* keep the next presented picture */
static uint64_t pause_press, pause_press_reads;  /* the button pressed for the player */
static uint64_t pause_asked_at;  /* SDL ticks of F4 (pausing, then resuming) */
static uint64_t pause_a_at;      /* SDL ticks of the last A on Continue */
static int pause_a_tries;
static struct {
    VkImage image;
    VkDeviceMemory mem;
    uint32_t w, h;
    VkFormat format;
    int valid;
} kept;

static void game_pause_toggle(void);
void window_pause_toggle(void) { game_pause_toggle(); }  /* replays: the key PAUSE */
int window_paused(void) { return __atomic_load_n(&paused_view, __ATOMIC_ACQUIRE); }

static void pause_press_button(uint64_t button) {
    pause_press_reads = window_input_reads() + 2;
    __atomic_store_n(&pause_press, button, __ATOMIC_RELEASE);
}

static void pause_live(const char *why) {
    __atomic_store_n(&pause_press, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&paused_view, 0, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: pause: live again (%s), %.0f ms after F4\n", why, (double)(SDL_GetTicks() - pause_asked_at));
}

static int pause_resume_pending;  /* resume asked while the game was still pausing */

static void game_pause_toggle(void) {
    if (!paused_view) {
        pause_resume_pending = 0;
        pause_request();
        pause_keep_request = 1, kept.valid = 0, pause_asked_at = SDL_GetTicks();
        __atomic_store_n(&paused_view, 1, __ATOMIC_RELEASE);
        pause_press_button(BTN_PLUS_BIT);
        fprintf(stderr, "yw4: pause: Plus for the player, the picture kept\n");
    } else if (pause_resume()) {
        pause_asked_at = SDL_GetTicks(), pause_a_at = 0, pause_a_tries = 0;
        fprintf(stderr, "yw4: pause: resume: A on Continue\n");
    } else if (pause_state() == PAUSE_WANTED) {
        pause_resume_pending = 1;  /* not paused yet: resume as soon as it is */
        fprintf(stderr, "yw4: pause: resume once the game has paused\n");
    }
}

/* ---- Return to title (the overlay's, 2026-11-13): the game's pause screen is still open under the kept picture,
 * with Continue chosen. Its other item is Return to Title, which opens the game's own Yes / No window
 * (pause_menu_inc.lua: OnDecideFocus, OpenGeneralWindow, Callback_IsCloseYN). The live picture is shown and the
 * answer is pressed for the player, one button every 0.7 s (a screen opening drops a press that comes too soon).
 * YW4_TITLE_KEYS=Down,A,... changes the buttons (tests). */
enum { BTN_LEFT_BIT = 1 << 12, BTN_UP_BIT = 1 << 13, BTN_RIGHT_BIT = 1 << 14, BTN_DOWN_BIT = 1 << 15 };
static uint64_t title_keys[8];
static int title_n, title_at;   /* the buttons, how many, the next one */
static uint64_t title_next_ms;  /* SDL ticks of the next press */

int window_pause_title_state(void) {
    int st = pause_state();
    if (paused_view && st == PAUSE_ON) return 1;  /* the game's pause screen is open: Return to Title can be answered */
    if (paused_view && st == PAUSE_WANTED) return 0;  /* Plus pressed, the screen still opening (up to ~2 s) */
    return -1;  /* no pause screen here (a cutscene, the title screen), or the overlay's preview runs the game */
}

int window_pause_to_title(void) {
    if (!paused_view || pause_state() != PAUSE_ON) return 0;  /* not in the game's pause screen: nothing to answer */
    const char *e = getenv("YW4_TITLE_KEYS");
    if (!e) e = "Down,A,Left,A";  /* its Yes / No window: Yes left, No right and chosen first (seen 2026-11-13) */
    title_n = 0;
    for (const char *p = e; *p && title_n < 8;) {
        size_t n = strcspn(p, ",");
        uint64_t b = !strncmp(p, "Down", n) ? BTN_DOWN_BIT : !strncmp(p, "Up", n) ? BTN_UP_BIT
                   : !strncmp(p, "Left", n) ? BTN_LEFT_BIT : !strncmp(p, "Right", n) ? BTN_RIGHT_BIT
                   : !strncmp(p, "A", n) ? BTN_A_BIT : 0;
        if (b) title_keys[title_n++] = b;
        p += n + (p[n] == ',');
    }
    title_at = 0, title_next_ms = SDL_GetTicks() + 150;
    pause_to_title();  /* the kept picture stays up while the buttons answer the game's screens behind it */
    fprintf(stderr, "yw4: pause: return to title, %d buttons (%s), behind the kept picture\n", title_n, e);
    return 1;
}

/* once per input update (window's thread, and replays): moves the pause on; returns the buttons pressed for the
 * player, OR'd into the input */
uint64_t window_pause_buttons(void) {
    if (title_at < title_n && SDL_GetTicks() >= title_next_ms && !__atomic_load_n(&pause_press, __ATOMIC_ACQUIRE)) {
        uint64_t b = title_keys[title_at++];
        pause_press_button(b);
        /* the pause screen is open already (the player was in the popup): quick; after an A a window may open
         * (the Yes / No one, about half a second): wait for it before the next button */
        title_next_ms = SDL_GetTicks() + (b == BTN_A_BIT ? 600 : 250);
        fprintf(stderr, "yw4: pause: return to title, button %d of %d\n", title_at, title_n);
    }
    if (window_paused()) {
        uint64_t now = SDL_GetTicks();
        int st = pause_state();
        if (st == PAUSE_ON && pause_resume_pending) {
            pause_resume_pending = 0;
            game_pause_toggle();  /* the resume asked while it was pausing */
        } else if (st == PAUSE_OFF) {
            pause_live("the game resumed");
        } else if (st == PAUSE_TITLE && title_at >= title_n && now + 100 > title_next_ms) {
            /* half a second after the last button: the live picture, the game's own fade to its title screen (its
             * menu state's Exit doesn't run on that way, seen 2026-11-13: nothing to wait for; a press lost: the
             * game's own window is there to answer by hand) */
            pause_forget();
            pause_live("return to title: the buttons are done");
        } else if (st == PAUSE_WANTED && now - pause_asked_at > 2000) {
            pause_resume_pending = 0;
            pause_forget();
            pause_live("the game didn't pause here, a cutscene?");
        } else if (st == PAUSE_RESUME && (!pause_a_at || now - pause_a_at > 1200)) {
            if (pause_a_tries < 20) {
                pause_a_at = now, pause_a_tries++;
                pause_press_button(BTN_A_BIT);
                fprintf(stderr, "yw4: pause: A %d, %.0f ms after F4\n", pause_a_tries, (double)(now - pause_asked_at));
            } else {
                pause_forget();
                pause_live("the game didn't resume: its pause screen is shown");
            }
        }
    }
    uint64_t p = __atomic_load_n(&pause_press, __ATOMIC_ACQUIRE);
    if (p && window_input_reads() >= pause_press_reads) __atomic_store_n(&pause_press, 0, __ATOMIC_RELEASE), p = 0;
    return p;
}

static void kept_read_back(uint32_t w, uint32_t h);  /* below: the in-game overlay needs it on the CPU */

/* in present_image, while recording: keep src (w x h at sx, sy, in layout) when asked */
static void pause_keep(VkImage src, VkImageLayout layout, int32_t sx, int32_t sy, uint32_t w, uint32_t h, VkFormat format) {
    if (!pause_keep_request) return;
    pause_keep_request = 0;
    if (kept.image && (kept.w != w || kept.h != h || kept.format != format)) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyImage(vk.device, kept.image, NULL), vkFreeMemory(vk.device, kept.mem, NULL);
        kept.image = VK_NULL_HANDLE;
    }
    if (!kept.image) {
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        if (vkCreateImage(vk.device, &ici, NULL, &kept.image) != VK_SUCCESS) return;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk.device, kept.image, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (vkAllocateMemory(vk.device, &mai, NULL, &kept.mem) != VK_SUCCESS) {
            vkDestroyImage(vk.device, kept.image, NULL), kept.image = VK_NULL_HANDLE;
            return;
        }
        vkBindImageMemory(vk.device, kept.image, kept.mem, 0);
        kept.w = w, kept.h = h, kept.format = format;
    }
    barrier(kept.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {sx, sy, 0},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(cmd, src, layout, kept.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(kept.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    kept.valid = 1;
    kept_read_back(w, h);
}

/* ---- the in-game overlay (gamemenu.h) on screen. It is see-through (a tint over the game, a dimmed backdrop
 * behind its popup), and the window copies overlays without blending, so it is put together on the CPU: the kept
 * picture (read back once when the pause keeps it, scaled to the window and turned into what the window shows),
 * with the overlay drawn over it (ui.h), the whole picture area uploaded as one image when what it shows changes
 * (gamemenu_look) and copied onto each frame. While a settings page it opened is up, that page shows instead. */
static struct {
    VkBuffer buf;            /* the kept picture read back: kept.w x kept.h texels of kept.format */
    VkDeviceMemory mem;
    void *ptr;
    VkDeviceSize size;
    int state;               /* 0 none, 1 copy recorded, 2 in ptr (the frame's fence has passed) */
    uint8_t *bg;             /* it scaled to the picture area, RGBA as the window shows it */
    uint32_t bg_w, bg_h;
    uint8_t *px;             /* the overlay over it */
    Overlay img;             /* its upload buffer and image, the picture area's size */
    uint32_t look;           /* gamemenu_look of what img holds */
    int uploaded;
} ov;

static int pad_hints(const char **close);  /* below, with the gamepad */
static int pad_has_mic(void);
static const char *pad_name(void);
const uint8_t *game_logo(int *w, int *h);  /* gamelogo.c */

static void kept_read_back(uint32_t w, uint32_t h) {
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (ov.buf && ov.size < need) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyBuffer(vk.device, ov.buf, NULL), vkFreeMemory(vk.device, ov.mem, NULL);
        ov.buf = VK_NULL_HANDLE;
    }
    if (!ov.buf) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (vkCreateBuffer(vk.device, &bci, NULL, &ov.buf) != VK_SUCCESS) return;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, ov.buf, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (vkAllocateMemory(vk.device, &mai, NULL, &ov.mem) != VK_SUCCESS) {
            vkDestroyBuffer(vk.device, ov.buf, NULL), ov.buf = VK_NULL_HANDLE;
            return;
        }
        vkBindBufferMemory(vk.device, ov.buf, ov.mem, 0);
        vkMapMemory(vk.device, ov.mem, 0, VK_WHOLE_SIZE, 0, &ov.ptr);
        ov.size = need;
    }
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cmd, kept.image, VK_IMAGE_LAYOUT_GENERAL, ov.buf, 1, &copy);
    ov.state = 1, ov.bg_w = 0;  /* the scaled copy is made again */
}

/* the kept picture scaled to bw x bh (bilinear), as the window shows it: BGRA swapped, sRGB decoded as the blit
 * into the window does */
static int overlay_background(uint32_t bw, uint32_t bh) {
    if (ov.state != 2 || !kept.valid) return -1;
    if (ov.bg && ov.bg_w == bw && ov.bg_h == bh) return 0;
    free(ov.bg);
    ov.bg = malloc((size_t)bw * bh * 4);
    if (!ov.bg) return -1;
    uint32_t w = kept.w, h = kept.h;
    uint8_t *src = malloc((size_t)w * h * 4);  /* one bulk copy: the mapped memory is slow to read piecemeal */
    if (!src) return -1;
    memcpy(src, ov.ptr, (size_t)w * h * 4);
    int bgra = kept.format == VK_FORMAT_B8G8R8A8_UNORM || kept.format == VK_FORMAT_B8G8R8A8_SRGB;
    int srgb = kept.format == VK_FORMAT_B8G8R8A8_SRGB || kept.format == VK_FORMAT_R8G8B8A8_SRGB;
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        if (srgb) c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        lut[i] = (uint8_t)(c * 255.0 + 0.5);
    }
    for (uint32_t y = 0; y < bh; y++) {
        float fy = ((float)y + 0.5f) * (float)h / (float)bh - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < (int)h ? y0 + 1 : y0;
        float ty = fy < 0 ? 0 : fy - (float)y0;
        for (uint32_t x = 0; x < bw; x++) {
            float fx = ((float)x + 0.5f) * (float)w / (float)bw - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < (int)w ? x0 + 1 : x0;
            float tx = fx < 0 ? 0 : fx - (float)x0;
            const uint8_t *a = src + ((size_t)y0 * w + x0) * 4, *b = src + ((size_t)y0 * w + x1) * 4;
            const uint8_t *c = src + ((size_t)y1 * w + x0) * 4, *d = src + ((size_t)y1 * w + x1) * 4;
            uint8_t *o = ov.bg + ((size_t)y * bw + x) * 4;
            for (int k = 0; k < 3; k++) {
                float v = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
                o[bgra ? 2 - k : k] = lut[(int)(v + 0.5f)];
            }
            o[3] = 255;
        }
    }
    free(src);
    ov.bg_w = bw, ov.bg_h = bh;
    return 0;
}

/* The quick look's card through the HUD's blend pipeline (see-through around its rounded corners): an image the
 * card's size, made again when the card grows, uploaded from the overlay's canvas when it was drawn again. */
static struct {
    VkBuffer buf;
    VkDeviceMemory buf_mem;
    void *ptr;
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    uint32_t w, h;
} pk;

static int pk_resources(uint32_t w, uint32_t h) {
    if (pk.set) return pk.w >= w && pk.h >= h ? 0 : -1;  /* made once; a bigger card than that: the blit instead */
    if (hb_resources()) return -1;
    /* the size of the largest window it could be drawn in: a 4K one (or this one, if larger) */
    if (w < 3840) w = 3840;
    if (h < 2160) h = 2160;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateBuffer(vk.device, &bci, NULL, &pk.buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, pk.buf, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(vk.device, &mai, NULL, &pk.buf_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(vk.device, pk.buf, pk.buf_mem, 0);
    vkMapMemory(vk.device, pk.buf_mem, 0, VK_WHOLE_SIZE, 0, &pk.ptr);
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(vk.device, &ici, NULL, &pk.image) != VK_SUCCESS) return -1;
    vkGetImageMemoryRequirements(vk.device, pk.image, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk.device, &mai, NULL, &pk.mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(vk.device, pk.image, pk.mem, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = pk.image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(vk.device, &vi, NULL, &pk.view) != VK_SUCCESS) return -1;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps};
    if (vkCreateDescriptorPool(vk.device, &pci, NULL, &pk.pool) != VK_SUCCESS) return -1;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pk.pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &hb.dsl};
    if (vkAllocateDescriptorSets(vk.device, &dai, &pk.set) != VK_SUCCESS) return -1;
    VkDescriptorImageInfo ii = {hb.sampler, pk.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = pk.set, .dstBinding = 0,
                               .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii};
    vkUpdateDescriptorSets(vk.device, 1, &wr, 0, NULL);
    pk.w = w, pk.h = h;
    return 0;
}

/* The card's part of the canvas (rx, ry, rw x rh of a bw-wide RGBA canvas, premultiplied) blended onto dst at
 * (x0 + rx, y0 + ry). upload: the canvas was drawn again. 0 when drawn. */
static int pk_draw(VkImage dst, uint32_t index, const uint8_t *px, uint32_t bw, int32_t rx, int32_t ry, int32_t rw,
                   int32_t rh, int32_t x0, int32_t y0, int upload) {
    if (index >= 8 || !swap_views[index] || pk_resources((uint32_t)rw, (uint32_t)rh)) return -1;
    VkPipeline pipe = hb_pipeline(swap_format);
    if (!pipe) return -1;
    static int32_t last_w, last_h;
    if (upload || last_w != rw || last_h != rh) {
        for (int32_t j = 0; j < rh; j++)
            memcpy((uint8_t *)pk.ptr + (size_t)j * pk.w * 4, px + ((size_t)(ry + j) * bw + (size_t)rx) * 4, (size_t)rw * 4);
        hb_barrier(pk.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {.bufferRowLength = pk.w, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {(uint32_t)rw, (uint32_t)rh, 1}};
        vkCmdCopyBufferToImage(cmd, pk.buf, pk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        hb_barrier(pk.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        last_w = rw, last_h = rh;
    }
    hb_barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = swap_views[index],
                                     .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, swap_extent},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &att};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = {0, 0, (float)swap_extent.width, (float)swap_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, swap_extent};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hb.layout, 0, 1, &pk.set, 0, NULL);
    float W = (float)swap_extent.width, H = (float)swap_extent.height, px0 = (float)(x0 + rx), py0 = (float)(y0 + ry);
    float push[8] = {px0 / W * 2 - 1, py0 / H * 2 - 1, (px0 + (float)rw) / W * 2 - 1, (py0 + (float)rh) / H * 2 - 1,
                     (float)rw / (float)pk.w, (float)rh / (float)pk.h, 0, 0};
    vkCmdPushConstants(cmd, hb.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRendering(cmd);
    hb_barrier(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    return 0;
}

/* F2's quick look: the running picture behind its card blurred, cheaply: copied down to an eighth of its size and
 * back up with linear filtering, two image copies a frame. dst is in TRANSFER_DST and stays so. */
static struct {
    VkImage img;
    VkDeviceMemory mem;
    uint32_t w, h;
    VkFormat format;
} pb;

static void peek_blur(VkImage dst, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    static int told;
    if (!told++)
        fprintf(stderr, "yw4: window: quick look: %s\n",
                swap_readable ? "blurring the picture behind it" : "no blur: the window's images can't be read");
    if (!swap_readable || bw < 64 || bh < 64) return;
    uint32_t w = bw / 8, h = bh / 8;
    if (pb.img && (pb.w != w || pb.h != h || pb.format != swap_format)) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyImage(vk.device, pb.img, NULL), vkFreeMemory(vk.device, pb.mem, NULL);
        memset(&pb, 0, sizeof pb);
    }
    if (!pb.img) {
        VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                 .format = swap_format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        if (vkCreateImage(vk.device, &ici, NULL, &pb.img) != VK_SUCCESS) return;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk.device, pb.img, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (vkAllocateMemory(vk.device, &mai, NULL, &pb.mem) != VK_SUCCESS) {
            vkDestroyImage(vk.device, pb.img, NULL), pb.img = VK_NULL_HANDLE;
            return;
        }
        vkBindImageMemory(vk.device, pb.img, pb.mem, 0);
        pb.w = w, pb.h = h, pb.format = swap_format;
    }
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    barrier(pb.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit down = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{x0, y0, 0}, {x0 + (int32_t)bw, y0 + (int32_t)bh, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pb.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down,
                   VK_FILTER_LINEAR);
    barrier(pb.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit up = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstOffsets = {{x0, y0, 0}, {x0 + (int32_t)bw, y0 + (int32_t)bh, 1}}};
    vkCmdBlitImage(cmd, pb.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up,
                   VK_FILTER_LINEAR);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
}

static void menu_draw(VkImage dst, uint32_t index, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    int peek = gamemenu_peeking();  /* F2's quick look: the keyboard card alone over the running game, not paused */
    if ((!gamemenu_open() && !peek) || settings_open()) return;
    int pv = gamemenu_previewing() || peek;  /* Preview: only its bar, over the live picture (no kept picture needed) */
    if (peek) peek_blur(dst, x0, y0, bw, bh);
    if (!pv && overlay_background(bw, bh)) return;
    if (ov.img.image && (ov.img.w != bw || ov.img.h != bh)) {  /* the window changed size */
        vkDeviceWaitIdle(vk.device);
        vkDestroyImage(vk.device, ov.img.image, NULL), vkFreeMemory(vk.device, ov.img.image_mem, NULL);
        vkDestroyBuffer(vk.device, ov.img.buffer, NULL), vkFreeMemory(vk.device, ov.img.buffer_mem, NULL);
        memset(&ov.img, 0, sizeof ov.img);
        free(ov.px), ov.px = NULL;
    }
    if (!ov.img.image) {
        ov.img.w = bw, ov.img.h = bh, ov.uploaded = 0;
        if (overlay_resources(&ov.img)) return;
    }
    uint64_t now = SDL_GetTicks();
    uint32_t look = gamemenu_look(now);
    if (pv) look ^= 0x80000000u | (uint32_t)(now / 500) << 20;  /* the bar's frame rate, twice a second */
    int redrawn = !ov.uploaded || look != ov.look;
    if (redrawn) {  /* draw it again: on the CPU, then one copy into the upload buffer */
        if (!ov.px && !(ov.px = malloc((size_t)bw * bh * 4))) return;
        if (!pv) memcpy(ov.px, ov.bg, (size_t)bw * bh * 4);  /* the bar is opaque: drawn over anything */
        if (peek) memset(ov.px, 0, (size_t)bw * bh * 4);       /* the quick look: see-through around its card */
        float fs, fx, fy;
        overlay_fit((float)bw, (float)bh, &fs, &fx, &fy);
        UiCanvas c = {ov.px, (int)bw, (int)bh, fs, fx, fy};
        const char *close = "Esc";
        GameMenuInfo info = {hud_enabled(), yw4_version, pad_hints(&close), close, NULL, 0, 0, now};
        info.logo = game_logo(&info.logo_w, &info.logo_h);
        info.pad_name = pad_name();
        info.pad_mic = pad_has_mic();
        info.picture = pv ? NULL : ov.bg, info.picture_w = (int)bw, info.picture_h = (int)bh;
        if (pv) {
            static HudContent hc;
            yw4_perf_hud(&hc, 1, 0);
            info.fps = hc.fps, info.shown_fps = hc.shown_fps;
        }
        gamemenu_draw(&c, &info);
        memcpy(ov.img.ptr, ov.px, (size_t)bw * bh * 4);
        barrier(ov.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {bw, bh, 1}};
        vkCmdCopyBufferToImage(cmd, ov.img.buffer, ov.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(ov.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        ov.look = look, ov.uploaded = 1;
    }
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    int32_t rx = 0, ry = 0, rw = (int32_t)bw, rh = (int32_t)bh;  /* the part shown: all, or the preview's bar */
    if (pv) {
        float r[4], s, fx, fy;
        overlay_fit((float)bw, (float)bh, &s, &fx, &fy);
        gamemenu_preview_rect(r);
        rx = (int32_t)(fx + r[0] * s), ry = (int32_t)(fy + r[1] * s), rw = (int32_t)(r[2] * s + 0.5f),
        rh = (int32_t)(r[3] * s + 0.5f);
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        if (rx + rw > (int32_t)bw) rw = (int32_t)bw - rx;
        if (ry + rh > (int32_t)bh) rh = (int32_t)bh - ry;
        if (rw <= 0 || rh <= 0) return;
        /* the quick look: blended, so its rounded corners show the (blurred) picture; the blit below if that fails */
        if (peek && !pk_draw(dst, index, ov.px, bw, rx, ry, rw, rh, x0, y0, redrawn)) return;
    }
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{rx, ry, 0}, {rx + rw, ry + rh, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x0 + rx, y0 + ry, 0}, {x0 + rx + rw, y0 + ry + rh, 1}}};
    vkCmdBlitImage(cmd, ov.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);  /* a blit: the window's format may order the channels differently */
}


/* An overlay's drawn cw x ch part pasted 1:1 into rgba (the w x h frame) at (x, y), clipped. */
static void paste(uint8_t *rgba, uint32_t w, uint32_t h, const Overlay *o, int cw, int ch, uint32_t x, uint32_t y,
                  float scale) {
    if (!cw || !o->ptr || x >= w || y >= h) return;
    uint32_t sw = (uint32_t)(cw * scale), sh = (uint32_t)(ch * scale);
    uint32_t pw = sw < w - x ? sw : w - x;
    uint32_t ph = sh < h - y ? sh : h - y;
    for (uint32_t r = 0; r < ph; r++) {
        const uint8_t *src = (const uint8_t *)o->ptr + (size_t)(r / scale) * o->w * 4;
        uint8_t *dst = rgba + ((size_t)(y + r) * w + x) * 4;
        for (uint32_t c = 0; c < pw; c++) memcpy(dst + c * 4, src + (size_t)(c / scale) * 4, 4);
    }
}

/* The panel's whole-step scale for a picture of bw x bh (as panel_draw shows it). */
static uint32_t panel_scale(uint32_t bw, uint32_t bh) {
    uint32_t s = panel_shown_h ? bh * 2 / 3 / (uint32_t)panel_shown_h : 1, sw = panel_shown_w ? bw * 9 / 10 / (uint32_t)panel_shown_w : 1;
    if (sw < s) s = sw;
    return s ? s : 1;
}

/* Screenshots show the HUD and the settings panel when they are on: their texels from the last
 * present, the HUD at the top-left corner, scaled as overlay_scale says for its height, and the panel in its middle at the window's scale
 * for a picture of this size (rgba: the w x h frame, colours as the window shows). */
static void hud_onto(uint8_t *rgba, uint32_t w, uint32_t h) {
    /* The HUD drawn again for this frame, at the size it has on screen relative to the picture: the window's
     * scale times frame height / the picture's height in the window (owner, 2026-11-13: a 4K F12 from a smaller
     * window had a quarter-size HUD, its window-sized texels pasted 1:1). Up to 4x, what HUD_W x HUD_H holds. */
    if (hb.cpu && hud_shown_w && hud_shot.bh) {
        float hs = hud_shot.s * (float)h / (float)hud_shot.bh;
        if (hs > 4.0f) hs = 4.0f;
        uint8_t *buf = malloc((size_t)HUD_W * HUD_H * 4);
        if (buf) {
            int hw = 0, hh = 0;
            hud_style_render(buf, HUD_W, HUD_H, &hud_shot.content, hud_shot.compact, hud_shot.parts, hs, &hw, &hh);
            int32_t hx, hy;
            hud_place(hw, hh, w, h, hs, &hx, &hy);
            for (int j = 0; j < hh && hy + j < (int32_t)h; j++)  /* the see-through card, blended as the window does */
                for (int i = 0; i < hw && hx + i < (int32_t)w; i++) {
                    const uint8_t *sp = buf + ((size_t)j * HUD_W + (size_t)i) * 4;
                    uint8_t *dp = rgba + ((size_t)(hy + j) * w + (size_t)(hx + i)) * 4;
                    for (int k = 0; k < 3; k++) dp[k] = (uint8_t)(sp[k] + dp[k] * (255 - sp[3]) / 255);
                }
            free(buf);
        }
    }
    uint32_t s = panel_scale(w, h);
    if (panel_shown_w && (uint32_t)panel_shown_w * s <= w && (uint32_t)panel_shown_h * s <= h)
        paste(rgba, w, h, &panel, panel_shown_w, panel_shown_h, (w - (uint32_t)panel_shown_w * s) / 2,
              (h - (uint32_t)panel_shown_h * s) / 2, (float)s);
    /* the in-game overlay and the corner notice too, as the window shows them (owner, 2026-11-12: F12 takes what
     * is on screen); YW4_SHOT_MENU=0 leaves them out (pictures of the game alone) */
    static int shot_menu = -1;
    if (shot_menu < 0) {
        const char *e = getenv("YW4_SHOT_MENU");
        shot_menu = !(e && e[0] == '0');
    }
    if (shot_menu && note_on && nb.ptr && nb.w) {  /* the notice too, as faded now, at the window's pixel size */
        int32_t nx = (int32_t)((40 - 24) * nb.s), ny = (int32_t)h - (int32_t)(48 * nb.s) - note_bottom;
        for (int j = 0; j < nb.h && ny + j < (int32_t)h; j++)
            for (int i = 0; i < nb.w; i++) {
                if (nx + i < 0 || nx + i >= (int32_t)w) continue;
                const uint8_t *sp = (const uint8_t *)nb.ptr + ((size_t)j * NOTE_W + (size_t)i) * 4;
                uint8_t *dp = rgba + ((size_t)(ny + j) * w + (size_t)(nx + i)) * 4;
                for (int k = 0; k < 3; k++) dp[k] = (uint8_t)(sp[k] + dp[k] * (255 - sp[3]) / 255);
            }
    }
    if (!shot_menu || !(gamemenu_open() || gamemenu_peeking()) || settings_open() || !ov.px || !ov.uploaded) return;
    /* the part shown: all, or in the preview only its bar (the rest of ov.px is an older drawing), as menu_draw */
    float fs, fx, fy, f = (float)h / (float)ov.img.h;  /* f: the overlay's pixels to the frame's */
    overlay_fit((float)ov.img.w, (float)ov.img.h, &fs, &fx, &fy);
    float r[4] = {-fx / fs, -fy / fs, (float)ov.img.w / fs, (float)ov.img.h / fs};  /* all of it */
    if (gamemenu_previewing() || gamemenu_peeking()) gamemenu_preview_rect(r);
    float k = fs * f;  /* design units to the frame's pixels */
    float l = (fx + r[0] * fs) * f, t = (fy + r[1] * fs) * f, rgt = l + r[2] * k, bot = t + r[3] * k;
    /* F2's quick look is see-through around its card (premultiplied, blended as the window does); the full overlay
     * is opaque (its background is the kept game picture, whose alpha means nothing): copied */
    int blend = gamemenu_peeking();
    for (uint32_t y = 0; y < h; y++)  /* the overlay's picture (the window's size) scaled to the frame's */
        for (uint32_t x = 0; x < w; x++) {
            if ((float)x < l || (float)x >= rgt || (float)y < t || (float)y >= bot) continue;
            uint8_t *dp = rgba + ((size_t)y * w + x) * 4;
            const uint8_t *sp = ov.px + ((size_t)(y * ov.img.h / h) * ov.img.w + x * ov.img.w / w) * 4;
            if (!blend) memcpy(dp, sp, 4);
            else
                for (int c = 0; c < 3; c++) dp[c] = (uint8_t)(sp[c] + dp[c] * (255 - sp[3]) / 255);
        }
}

/* DLSS frame generation's HUD-less copy at the window's size and format (made again when either changes). */
static struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t w, h;
    VkFormat format;
} hlw;

static int hudless_window_image(uint32_t w, uint32_t h) {
    if (hlw.image && hlw.w == w && hlw.h == h && hlw.format == swap_format) return 1;
    if (hlw.image) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyImageView(vk.device, hlw.view, NULL);
        vkDestroyImage(vk.device, hlw.image, NULL);
        vkFreeMemory(vk.device, hlw.mem, NULL);
        memset(&hlw, 0, sizeof hlw);
    }
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = swap_format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT};
    if (vkCreateImage(vk.device, &ici, NULL, &hlw.image) != VK_SUCCESS) return hlw.image = VK_NULL_HANDLE, 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, hlw.image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkAllocateMemory(vk.device, &mai, NULL, &hlw.mem);
    vkBindImageMemory(vk.device, hlw.image, hlw.mem, 0);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = hlw.image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = swap_format,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCreateImageView(vk.device, &vci, NULL, &hlw.view);
    hlw.w = w, hlw.h = h, hlw.format = swap_format;
    return 1;
}

/* Flash detector (YW4_FLASH=1, docs/debugging.md): each presented frame is also scaled to a small thumbnail and
 * read back one frame later (after the fence the next present waits for anyway: no stall). A frame where many
 * thumbnail texels are brighter than in both the frame before and the frame after is logged ("yw4: flash:") and
 * the three thumbnails are saved, so a one-frame flash is found without screenshots (which stall every frame). */
#define FLASH_W 320
#define FLASH_RING 480  /* thumbnails kept for F12: 4 s at 120 frames/s */
static int flash_dump_requested;
static uint64_t flash_n = UINT64_MAX;  /* the frame window_present_image presents (UINT64_MAX: a generated one) */
static VkFormat flash_format;
static struct {
    int on;  /* -1 not read yet, 0 off, 1 on */
    VkImage image;
    VkDeviceMemory mem, buf_mem;
    VkBuffer buf;
    uint8_t *ptr;
    uint32_t h;
    VkFormat format;
    int pending;      /* a thumbnail was copied in the last present */
    uint64_t pending_n;
    uint8_t *lum[3];  /* luminance of the last three thumbnails, [2] the newest */
    uint8_t *rgb[3];
    uint64_t n[3];
    int have;
    int saved;
} fl = {.on = -1};

static int flash_setup(uint32_t w, uint32_t h, VkFormat format) {
    if (fl.on < 0) fl.on = getenv("YW4_FLASH") && atoi(getenv("YW4_FLASH")) > 0;
    if (!fl.on) return 0;
    uint32_t th = (uint32_t)((uint64_t)FLASH_W * h / w);
    if (th < 1) th = 1;
    if (fl.image && fl.h == th && fl.format == format) return 1;
    if (fl.image) {
        vkDeviceWaitIdle(vk.device);
        vkDestroyImage(vk.device, fl.image, NULL), vkFreeMemory(vk.device, fl.mem, NULL);
        vkDestroyBuffer(vk.device, fl.buf, NULL), vkFreeMemory(vk.device, fl.buf_mem, NULL);
        fl.image = VK_NULL_HANDLE, fl.pending = 0, fl.have = 0;
    }
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = format, .extent = {FLASH_W, th, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateImage(vk.device, &ici, NULL, &fl.image) != VK_SUCCESS) return fl.on = 0, 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, fl.image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkAllocateMemory(vk.device, &mai, NULL, &fl.mem);
    vkBindImageMemory(vk.device, fl.image, fl.mem, 0);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)FLASH_W * th * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    vkCreateBuffer(vk.device, &bci, NULL, &fl.buf);
    vkGetBufferMemoryRequirements(vk.device, fl.buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk.device, &mai, NULL, &fl.buf_mem);
    vkBindBufferMemory(vk.device, fl.buf, fl.buf_mem, 0);
    vkMapMemory(vk.device, fl.buf_mem, 0, VK_WHOLE_SIZE, 0, (void **)&fl.ptr);
    for (int i = 0; i < 3; i++) {
        free(fl.lum[i]), free(fl.rgb[i]);
        fl.lum[i] = malloc((size_t)FLASH_W * th), fl.rgb[i] = malloc((size_t)FLASH_W * th * 4);
    }
    fl.h = th, fl.format = format;
    fprintf(stderr, "yw4: flash: detector on, %ux%u thumbnails\n", FLASH_W, th);
    return 1;
}

/* in present_image, while recording: the frame into the thumbnail and the thumbnail into the buffer */
static void flash_record(VkImage src, VkImageLayout layout, int32_t sx, int32_t sy, uint32_t w, uint32_t h, uint64_t n) {
    barrier(fl.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{sx, sy, 0}, {sx + (int32_t)w, sy + (int32_t)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{0, 0, 0}, {FLASH_W, (int32_t)fl.h, 1}}};
    vkCmdBlitImage(cmd, src, layout, fl.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    barrier(fl.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {FLASH_W, fl.h, 1}};
    vkCmdCopyImageToBuffer(cmd, fl.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fl.buf, 1, &c);
    fl.pending = 1, fl.pending_n = n;
}

static void flash_save(int i, const char *tag) {
    char path[600];
    SDL_CreateDirectory(screenshot_dir);
    snprintf(path, sizeof path, "%s/flash_%06" PRIu64 "_%s.bmp", screenshot_dir, fl.n[i], tag);
    SDL_Surface *s = SDL_CreateSurfaceFrom(FLASH_W, (int)fl.h, SDL_PIXELFORMAT_ABGR8888, fl.rgb[i], FLASH_W * 4);
    if (s) SDL_SaveBMP(s, path), SDL_DestroySurface(s);
}

/* in present_image, after its fence wait: the last present's thumbnail is in the buffer */
static void flash_collect(void) {
    if (!fl.pending) return;
    fl.pending = 0;
    uint8_t *l = fl.lum[0], *c = fl.rgb[0];  /* the oldest becomes the newest */
    fl.lum[0] = fl.lum[1], fl.lum[1] = fl.lum[2], fl.lum[2] = l;
    fl.rgb[0] = fl.rgb[1], fl.rgb[1] = fl.rgb[2], fl.rgb[2] = c;
    fl.n[0] = fl.n[1], fl.n[1] = fl.n[2], fl.n[2] = fl.pending_n;
    size_t texels = (size_t)FLASH_W * fl.h;
    memcpy(c, fl.ptr, texels * 4);
    int bgra = fl.format == VK_FORMAT_B8G8R8A8_UNORM || fl.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < texels; i++) {
        uint8_t *p = c + i * 4;
        if (bgra) { uint8_t t = p[0]; p[0] = p[2], p[2] = t; }
        p[3] = 255;
        l[i] = (uint8_t)((p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8);
    }
    /* the last FLASH_RING thumbnails in memory; F12 saves them all (the owner presses it just after a flash) */
    static uint8_t *ring[FLASH_RING];
    static uint64_t ring_n[FLASH_RING];
    static size_t ring_bytes;
    if (ring_bytes != texels * 4) {
        for (int i = 0; i < FLASH_RING; i++) free(ring[i]), ring[i] = NULL, ring_n[i] = 0;
        ring_bytes = texels * 4;
    }
    int slot = (int)(fl.n[2] % FLASH_RING);
    if (!ring[slot]) ring[slot] = malloc(ring_bytes);
    if (ring[slot]) memcpy(ring[slot], c, ring_bytes), ring_n[slot] = fl.n[2];
    if (__atomic_exchange_n(&flash_dump_requested, 0, __ATOMIC_ACQ_REL)) {
        char dir[600], path[700];
        snprintf(dir, sizeof dir, "%s/flash_ring_%06" PRIu64, screenshot_dir, fl.n[2]);
        SDL_CreateDirectory(dir);
        int saved = 0;
        for (int i = 0; i < FLASH_RING; i++) {
            if (!ring[i] || !ring_n[i] || ring_n[i] + FLASH_RING <= fl.n[2]) continue;
            snprintf(path, sizeof path, "%s/flash_%06" PRIu64 "_all.bmp", dir, ring_n[i]);
            SDL_Surface *s = SDL_CreateSurfaceFrom(FLASH_W, (int)fl.h, SDL_PIXELFORMAT_ABGR8888, ring[i], FLASH_W * 4);
            if (s) SDL_SaveBMP(s, path), SDL_DestroySurface(s), saved++;
        }
        fprintf(stderr, "yw4: flash: F12 at frame %" PRIu64 ": %d thumbnails saved to %s\n", fl.n[2], saved, dir);
    }
    static uint64_t save_from = 1, save_to;  /* YW4_FLASH_SAVE=<from>-<to>: every thumbnail in those frames */
    if (save_from == 1) {
        const char *e = getenv("YW4_FLASH_SAVE");
        save_from = 0;
        if (e) sscanf(e, "%" SCNu64 "-%" SCNu64, &save_from, &save_to);
    }
    if (fl.n[2] >= save_from && fl.n[2] <= save_to) flash_save(2, "all");
    if (++fl.have < 3 || fl.n[2] != fl.n[1] + 1 || fl.n[1] != fl.n[0] + 1) return;
    /* the middle frame: texels brighter than both neighbours by more than 40 of 255 */
    static int thr = -1, min_pm = -1;
    if (thr < 0) thr = getenv("YW4_FLASH_THRESHOLD") ? atoi(getenv("YW4_FLASH_THRESHOLD")) : 40;
    if (min_pm < 0) min_pm = getenv("YW4_FLASH_MIN") ? atoi(getenv("YW4_FLASH_MIN")) : 3;  /* per mille */
    size_t count = 0;
    int64_t sum = 0;
    uint32_t x0 = FLASH_W, x1 = 0, y0 = fl.h, y1 = 0;
    for (size_t i = 0; i < texels; i++) {
        int a = fl.lum[0][i], m = fl.lum[1][i], b = fl.lum[2][i];
        int d = m - (a > b ? a : b);
        if (d > thr) {
            count++, sum += d;
            uint32_t x = (uint32_t)(i % FLASH_W), y = (uint32_t)(i / FLASH_W);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (count * 1000 < (size_t)min_pm * texels) return;
    fprintf(stderr, "yw4: flash: frame %" PRIu64 ": %zu of %zu thumbnail texels brighter than frames %" PRIu64
            " and %" PRIu64 " (by %.0f of 255 on average), area x %u-%u y %u-%u of %ux%u\n",
            fl.n[1], count, texels, fl.n[0], fl.n[2], (double)sum / (double)count, x0, x1, y0, y1, FLASH_W, fl.h);
    if (fl.saved++ < 40) flash_save(0, "a_before"), flash_save(1, "b_flash"), flash_save(2, "c_after");
}

/* Scales src (w x h colour texels) into the next swapchain image and presents it.
 * layout UNDEFINED: src is `frame`, filled from the staging buffer (the CPU path).
 * Otherwise src is the draw path's image in that layout (GENERAL, decision 0021); with
 * `readback` it is also copied into the staging buffer and this waits for the GPU, so the
 * caller can read the texels. present_lock held, frame_resources done. 0 when presented. */
static int present_image(VkImage src, VkImageLayout layout, VkRect2D r0, int readback) {
    uint32_t w = r0.extent.width, h = r0.extent.height;
    int32_t sx = r0.offset.x, sy = r0.offset.y;
    vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (ov.state == 1) ov.state = 2;  /* the kept picture's read-back copy is done */
    flash_collect();
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(vk.device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale = 1;
        return -1;
    }
    vkResetFences(vk.device, 1, &fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier(src, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cmd, staging, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    } else {
        barrier(src, layout, layout, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        pause_keep(src, layout, sx, sy, w, h, flash_format);
        if (paused_view && kept.valid && kept.w == w && kept.h == h) {  /* the kept picture instead of the game's */
            src = kept.image, layout = VK_IMAGE_LAYOUT_GENERAL, sx = sy = 0;  /* screenshots too: not its pause screen */
            barrier(src, layout, layout, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        }
        if (readback) {
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {sx, sy, 0},
                                      .imageExtent = {w, h, 1}};
            vkCmdCopyImageToBuffer(cmd, src, layout, staging, 1, &copy);
        }
        if (flash_n != UINT64_MAX && flash_setup(w, h, flash_format)) flash_record(src, layout, sx, sy, w, h, flash_n);
    }
    VkImage dst = swap_images[index];
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    /* Fit the frame into the window, keeping its shape (black bars on the sides that are left). */
    uint32_t ww = swap_extent.width, wh = swap_extent.height;
    uint32_t bw = ww, bh = (uint32_t)((uint64_t)ww * h / w);
    if (bh > wh) bh = wh, bw = (uint32_t)((uint64_t)wh * w / h);
    int32_t x0 = (int32_t)(ww - bw) / 2, y0 = (int32_t)(wh - bh) / 2;
    /* where the picture is, as fractions of the window, for the mouse in menus (window_picture_rect) */
    picture_rect[0] = (float)x0 / (float)ww, picture_rect[1] = (float)y0 / (float)wh;
    picture_rect[2] = (float)bw / (float)ww, picture_rect[3] = (float)bh / (float)wh;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{sx, sy, 0}, {sx + (int32_t)w, sy + (int32_t)h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x0, y0, 0}, {x0 + (int32_t)bw, y0 + (int32_t)bh, 1}}};
    vkCmdBlitImage(cmd, src, layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    /* DLSS frame generation works on this window image: its HUD-less copy the same, the picture before the UI
     * (gpu_fg.c) scaled into the same place, so DLSS-G keeps the menus, our HUD and the panel still */
    VkImage hl = layout == VK_IMAGE_LAYOUT_GENERAL && dlssg_on() && !paused_view ? gpu_fg_hudless() : VK_NULL_HANDLE;
    if (hl && hudless_window_image(ww, wh)) {
        barrier(hl, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier(hlw.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdClearColorImage(cmd, hlw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        barrier(hlw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdBlitImage(cmd, hl, VK_IMAGE_LAYOUT_GENERAL, hlw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        barrier(hlw.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT);
        dlssg_hudless(hlw.image, hlw.view, swap_format, ww, wh);
    } else {
        dlssg_hudless(VK_NULL_HANDLE, VK_NULL_HANDLE, swap_format, 0, 0);
    }
    hud_draw(dst, index, x0, y0, bw, bh);
    note_draw(dst, index, x0, y0, bw, bh);
    menu_draw(dst, index, x0, y0, bw, bh);
    panel_draw(dst, x0, y0, bw, bh);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
                       .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered[index]};
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &rendered[index], .swapchainCount = 1, .pSwapchains = &swapchain,
                           .pImageIndices = &index};
    host_lock(&vk.queue_lock);
    vkQueueSubmit(vk.queue, 1, &si, fence);
    int64_t pt0 = host_now_ns();
    dlssg_present_begin(x0, y0, bw, bh, swap_format, nswap);  /* DLSS frame generation: the game's rectangle */
    int64_t pt1 = host_now_ns();
    r = vkQueuePresentKHR(vk.queue, &pi);
    int64_t pt2 = host_now_ns();
    dlssg_present_end();
    if (host_now_ns() - pt0 > 30000000)  /* slow presents (hitches): which part */
        fprintf(stderr, "yw4: window: slow present: before %.1f ms, vkQueuePresentKHR %.1f ms, after %.1f ms\n",
                (double)(pt1 - pt0) / 1e6, (double)(pt2 - pt1) / 1e6, (double)(host_now_ns() - pt2) / 1e6);
    host_unlock(&vk.queue_lock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_stale = 1;
    if (readback) vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
    return 0;
}

void window_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
    if (screenshot_wanted(n)) save_screenshot(rgba, w, h, n);
    if (!have_vulkan || !w || !h) return;
    host_lock(&present_lock);
    if (swap_free != free_running() || swap_dlssg != dlssg_chosen()) swapchain_stale = 1;  /* frame rate across 60 */
    if (!(swapchain_stale && create_swapchain(n)) && !frame_resources(w, h)) {
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);  /* the staging buffer is free */
        memcpy(staging_ptr, rgba, (size_t)w * h * 4);
        present_image(frame, VK_IMAGE_LAYOUT_UNDEFINED, (VkRect2D){{0, 0}, {w, h}}, 0);
    }
    host_unlock(&present_lock);
}

/* the game's last frame, for window_repaint (present_lock) */
static VkImage last_image;
static VkFormat last_format;
static VkRect2D last_rect;
static uint64_t last_n;

/* While the game waits on the player (its software keyboard, swkbd.c): its last frame shown again with the overlay
 * over it, from the waiting thread. The first call keeps that frame as the overlay's background, as a pause does.
 * -1 when there is nothing to show (no window or no frame yet). */
int window_repaint(int keep) {
    if (!have_vulkan) return -1;
    host_lock(&present_lock);
    if (!last_image) {
        host_unlock(&present_lock);
        return -1;
    }
    if (keep) pause_keep_request = 1, kept.valid = 0, ov.uploaded = 0;
    if (swap_free != free_running() || swap_dlssg != dlssg_chosen()) swapchain_stale = 1;
    if (!(swapchain_stale && create_swapchain(last_n)) && !frame_resources(last_rect.extent.width, last_rect.extent.height))
        present_image(last_image, VK_IMAGE_LAYOUT_GENERAL, last_rect, 0);
    host_unlock(&present_lock);
    return 0;
}

void window_present_image(VkImage image, VkFormat format, VkRect2D rect, uint64_t n) {
    uint32_t w = rect.extent.width, h = rect.extent.height;
    if (!have_vulkan || !w || !h) return;
    host_lock(&present_lock);
    last_image = image, last_format = format, last_rect = rect, last_n = n;
    host_unlock(&present_lock);
    int shot = screenshot_wanted(n);
    if (shot && getenv("YW4_DT_LOG")) {  /* testing frame pacing: the game's time step with each screenshot */
        static int64_t last;
        int64_t now = host_now_ns();
        fprintf(stderr, "yw4: dt: frame %" PRIu64 ": game step %.3f ms, %.3f ms since the last screenshot\n", n,
                framerate_game_dt() * 1000.0f, last ? (double)(now - last) / 1e6 : 0.0);
        last = now;
    }
    int bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    if (shot && !bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        fprintf(stderr, "yw4: screenshot of Vulkan format %d not supported yet\n", (int)format);
        shot = 0;
    }
    host_lock(&present_lock);
    if (swap_free != free_running() || swap_dlssg != dlssg_chosen()) swapchain_stale = 1;
    flash_n = n, flash_format = format;
    if (gpu_fg_switching()) {  /* frame generation is switching: the GPU finishes everything first (gpu_fg.h) */
        host_lock(&vk.queue_lock);
        vkDeviceWaitIdle(vk.device);
        host_unlock(&vk.queue_lock);
    }
    if (!(swapchain_stale && create_swapchain(n)) && !frame_resources(w, h) &&
        !present_image(image, VK_IMAGE_LAYOUT_GENERAL, rect, shot) && shot) {
        /* One bulk copy out of the staging memory: it is uncached, and per-byte reads of it took ~1.7 s. */
        uint8_t *px = malloc((size_t)w * h * 4);
        if (!px) {
            host_unlock(&present_lock);
            return;
        }
        memcpy(px, staging_ptr, (size_t)w * h * 4);
        if (bgra)
            for (size_t i = 0; i < (size_t)w * h * 4; i += 4) {
                uint8_t t = px[i];
                px[i] = px[i + 2], px[i + 2] = t;
            }
        /* The blit into the UNORM swapchain decodes an sRGB image to linear; save what the window shows. */
        if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB) {
            uint8_t lut[256];
            for (int i = 0; i < 256; i++) {
                double c = i / 255.0;
                c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
                lut[i] = (uint8_t)(c * 255.0 + 0.5);
            }
            for (size_t i = 0; i < (size_t)w * h * 4; i += 4)
                px[i] = lut[px[i]], px[i + 1] = lut[px[i + 1]], px[i + 2] = lut[px[i + 2]];
        }
        hud_onto(px, w, h);
        save_screenshot(px, w, h, n);
        free(px);
    }
    host_unlock(&present_lock);
}

/* ---- input: the keyboard, mouse, and the first gamepad, as a Switch controller (window_input) */

static SDL_Gamepad *gamepad;
static uint64_t input_buttons;
static int32_t input_sticks[4];
static HostLock input_lock = HOST_LOCK_INIT;

static HostLock mouse_lock = HOST_LOCK_INIT;
static float mouse_accum_x;
static float mouse_accum_y;
static int wheel_queue[2];                   /* queued wheel notches [0] up, [1] down */
static int wheel_active[2];                  /* currently pulsing [0] up or [1] down */
static uint64_t wheel_press_until_reads;     /* input_reads until active pulse ends */
static uint64_t wheel_gap_until_reads;       /* input_reads until release gap ends */
static int mouse_held_off;         /* the click that gave the game the mouse: no press until it is let go */
/* The PlayStation touchpad as a menu pointer (owner, 2026-11-14; tp_* below the gamepad code): in the game's menus
 * and our overlay a finger moves the cursor like a laptop trackpad and a touchpad click chooses; elsewhere the
 * click still opens the overlay. */
static int tp_game_click;                   /* a touchpad click begun in a game menu: its left button while held */
/* when the touchpad last moved the cursor (SDL_GetTicks): cursor motion until 150 ms after is the pad's. By time,
 * not by place: a quick finger moves it again before the first move's event comes, which then matched no place
 * and counted as the mouse (the overlay's on-screen keyboard went off and on, 2026-11-14) */
static uint64_t tp_moved_ms;

/* YW4_MOUSE=0: the game never takes the mouse (no capture, no camera, no clicks or wheel): for test runs, so the
 * person at the PC can go on using it (tools.dev.run sets it). */
static int mouse_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("YW4_MOUSE") && !strcmp(getenv("YW4_MOUSE"), "0");
    return off;
}

void window_mouse_motion(float *dx, float *dy) {
    host_lock(&mouse_lock);
    *dx = mouse_off() ? 0.0f : mouse_accum_x;
    *dy = mouse_off() ? 0.0f : mouse_accum_y;
    mouse_accum_x = 0.0f;
    mouse_accum_y = 0.0f;
    host_unlock(&mouse_lock);
}

static uint64_t input_reads;  /* how many times the game has read the input (window_input_reads) */
uint64_t window_input_reads(void) { return __atomic_load_n(&input_reads, __ATOMIC_ACQUIRE); }

void window_input_read_replayed(void) { __atomic_add_fetch(&input_reads, 1, __ATOMIC_RELEASE); }

void window_input(uint64_t *buttons, int32_t sticks[4]) {
    __atomic_add_fetch(&input_reads, 1, __ATOMIC_RELEASE);
    host_lock(&input_lock);
    *buttons = input_buttons;
    memcpy(sticks, input_sticks, sizeof input_sticks);
    host_unlock(&input_lock);
}

static int rumble_enabled = -1;
static float rumble_gain = 1.0f;
static uint64_t rumble_test_until;

static void rumble_init_env(void) {
    if (rumble_enabled < 0) {
        const char *e = getenv("YW4_RUMBLE");
        rumble_enabled = !(e && !strcmp(e, "0"));
        const char *g = getenv("YW4_RUMBLE_GAIN");
        if (g && *g) {
            rumble_gain = (float)atof(g);
            if (rumble_gain < 0.0f) rumble_gain = 0.0f;
        }
    }
}

int window_rumble_enabled(void) {
    rumble_init_env();
    return rumble_enabled;
}

void window_rumble_set_enabled(int on) {
    rumble_init_env();
    rumble_enabled = on ? 1 : 0;
    rumble_test_until = 0;
    if (!rumble_enabled) {
        host_lock(&input_lock);
        SDL_Gamepad *pad = gamepad;
        if (pad) SDL_RumbleGamepad(pad, 0, 0, 0), SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
        host_unlock(&input_lock);
    }
}

void window_rumble_test(void) {
    rumble_init_env();
    if (!rumble_enabled) return;
    rumble_test_until = SDL_GetTicks() + 1000;
    host_lock(&input_lock);
    SDL_Gamepad *pad = gamepad;
    if (pad) {
        SDL_RumbleGamepad(pad, 0, 0, 0);
        SDL_RumbleGamepad(pad, 49152, 49152, 1000);
    }
    host_unlock(&input_lock);
}

void window_rumble(uint16_t low, uint16_t high, uint32_t duration_ms) {
    rumble_init_env();
    if (!rumble_enabled) return;
    if (rumble_test_until) {
        if (SDL_GetTicks() < rumble_test_until) return;
        rumble_test_until = 0;
    }

    if (rumble_gain != 1.0f) {
        float l = (float)low * rumble_gain;
        float h = (float)high * rumble_gain;
        low = l > 65535.0f ? 65535 : (uint16_t)l;
        high = h > 65535.0f ? 65535 : (uint16_t)h;
    }

    /* The game may send the same value every frame; each send can be a controller write (Bluetooth pads), so
     * only changes go out, plus a refresh of a running rumble before its duration runs out. */
    static uint16_t last_low, last_high;
    static SDL_Gamepad *last_pad;
    static uint64_t last_ms;
    uint64_t now = SDL_GetTicks();
    host_lock(&input_lock);
    SDL_Gamepad *pad = gamepad;
    int same = pad == last_pad && low == last_low && high == last_high;
    if (pad && (!same || ((low || high) && now - last_ms >= duration_ms / 2))) {
        SDL_RumbleGamepad(pad, low, high, duration_ms);
        last_pad = pad, last_low = low, last_high = high, last_ms = now;
    }
    host_unlock(&input_lock);
}

/* ---- the Switch's HD rumble on a PC pad (decision 0058). The game sends two bands, each an amplitude (0..1) and a
 * frequency, for the Joy-Cons' linear actuators, which follow any strength at once. Measured 2026-11-10 (late-game
 * save, YW4_RUMBLE_LOG=1): the effects are smooth swells of ~0.4 s (rise ~10 frames, fall ~13), often back to back,
 * mostly faint (median 0.04 low, 0.07 high band; peaks 0.12 / 0.6); and the game scales the frequencies with the
 * amplitude (frequency / amplitude stays constant through a swell), so they say nothing about pitch and are not used.
 * A PC pad's motors are spinning weights: slow to start, and they coast. Played as given (YW4_RUMBLE_SHAPE=0, as
 * before 2026-11-10) faint swells don't start them; lifted evenly they become one long buzz (owner: "BUZZZ BUZZZ").
 * So each motor follows its band as force-feedback drivers do with such motors:
 *   - faint texture (below RUMBLE_GATE) is dropped, which also leaves a gap between back-to-back swells;
 *   - a swell's start kicks the motor (RUMBLE_KICK for RUMBLE_KICK_MS) so it spins up at once: the sharp edge;
 *   - while it holds, a curve lifts it a little (a^0.75 over a low floor);
 *   - as it fades the motor is cut to RUMBLE_FADE of that, so it stops before the next one.
 * Xbox One / Series pads also get the high band's kicks on their trigger motors, which start almost at once
 * (YW4_RUMBLE_TRIGGERS=0: off). YW4_RUMBLE_LOG=1 logs each change of what the game asked and what the motors got. */
#define RUMBLE_GATE 0.04f      /* below this: off */
#define RUMBLE_FLOOR 0.10f     /* the lowest speed while a swell holds (the motor already spins from the kick) */
#define RUMBLE_KICK 0.70f      /* a swell's first moment */
#define RUMBLE_KICK_MS 50
#define RUMBLE_FADE 0.55f      /* while a swell falls */
#define RUMBLE_TRIGGER 0.45f   /* the trigger motors are strong under a finger */
static int rumble_shape = -1, rumble_triggers = -1, rumble_log = -1;

static void rumble_init_shape(void) {
    if (rumble_shape >= 0) return;
    const char *e = getenv("YW4_RUMBLE_SHAPE"), *t = getenv("YW4_RUMBLE_TRIGGERS"), *l = getenv("YW4_RUMBLE_LOG");
    rumble_shape = !(e && *e == '0');
    rumble_triggers = !(t && *t == '0');
    rumble_log = l && *l == '1';
}

typedef struct {
    float prev;
    int on;
    uint64_t kick_until;
} RumbleBand;

/* One motor's speed (0..1) for its band's amplitude now; *kick: in a swell's first moment. */
static float rumble_band(RumbleBand *b, float a, uint64_t now, int *kick) {
    *kick = 0;
    if (a > 1.0f) a = 1.0f;
    if (!rumble_shape) return a < 0.02f ? 0.0f : a;
    if (a < RUMBLE_GATE) {
        b->on = 0, b->prev = a;
        return 0.0f;
    }
    if (!b->on) b->on = 1, b->kick_until = now + RUMBLE_KICK_MS;
    float out = RUMBLE_FLOOR + (1.0f - RUMBLE_FLOOR) * powf(a, 0.75f);
    if (now < b->kick_until) out = fmaxf(out, RUMBLE_KICK), *kick = 1;
    else if (a < b->prev - 0.001f) out *= RUMBLE_FADE;
    b->prev = a;
    return out;
}

static uint16_t rumble_u16(float a) { return (uint16_t)(a * 65535.0f + 0.5f); }

/* The trigger motors (Xbox One / Series), only changes sent as window_rumble does. */
static void rumble_triggers_set(uint16_t v) {
    static uint16_t last;
    static SDL_Gamepad *last_pad;
    host_lock(&input_lock);
    SDL_Gamepad *pad = gamepad;
    if (pad && (pad != last_pad || v != last) &&
        SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)) {
        SDL_RumbleGamepadTriggers(pad, v, v, v ? 200 : 0);
        last_pad = pad, last = v;
    }
    host_unlock(&input_lock);
}

void window_vibration(float amp_low, float freq_low, float amp_high, float freq_high) {
    rumble_init_env();
    rumble_init_shape();
    if (!rumble_enabled) return;
    static RumbleBand lo, hi;
    uint64_t now = SDL_GetTicks();
    int kick_l, kick_h;
    uint16_t l = rumble_u16(rumble_band(&lo, amp_low, now, &kick_l));
    uint16_t h = rumble_u16(rumble_band(&hi, amp_high, now, &kick_h));
    uint16_t trig = rumble_triggers && kick_h ? rumble_u16(RUMBLE_TRIGGER * fmaxf(amp_high, 0.5f)) : 0;
    if (rumble_log) {
        static float last[2] = {-1, -1};
        if (amp_low != last[0] || amp_high != last[1])
            fprintf(stderr, "yw4: rumble: game low %.3f @ %.0f Hz, high %.3f @ %.0f Hz -> motors %u / %u, triggers %u\n",
                    amp_low, freq_low, amp_high, freq_high, l, h, trig);
        last[0] = amp_low, last[1] = amp_high;
    }
    window_rumble(l, h, (l || h) ? 1000 : 0);
    if (rumble_triggers && !rumble_test_until) rumble_triggers_set(trig);
}

/* The keyboard, mouse, and the gamepad through the controls (bindings.c; the settings panel's second page). */
static void pad_back_hold(void);  /* below, with the gamepad */
static void pad_shoulders_overlay(void);
static uint32_t pad_parts_held(void);
static void update_input(void) {
    uint64_t b;
    int32_t s[4];
    float mx, my;
    uint32_t mb = SDL_GetMouseState(&mx, &my);
    if (mouse_held_off && !mb) mouse_held_off = 0;
    /* clicks are buttons only while the game has the mouse */
    if (mouse_held_off || !window || !SDL_GetWindowRelativeMouseMode(window) || mouse_off()) mb = 0;
    int wup = 0, wdwn = 0;
    uint64_t reads = window_input_reads();
    host_lock(&mouse_lock);
    if (mouse_held_off || mouse_off() || (window && !SDL_GetWindowRelativeMouseMode(window))) {
        wheel_queue[0] = wheel_queue[1] = 0;
        wheel_active[0] = wheel_active[1] = 0;
        wheel_press_until_reads = wheel_gap_until_reads = 0;
    }
    for (int d = 0; d < 2; d++) {
        if (wheel_active[d] && reads >= wheel_press_until_reads) {
            wheel_active[d] = 0;
            wheel_gap_until_reads = reads + 2;  /* release gap: at least 2 game reads */
        }
    }
    if (!wheel_active[0] && !wheel_active[1] && reads >= wheel_gap_until_reads) {
        for (int d = 0; d < 2; d++) {
            if (wheel_queue[d] > 0) {
                wheel_queue[d]--;
                wheel_active[d] = 1;
                wheel_press_until_reads = reads + 2;  /* active pulse: at least 2 game reads */
                break;
            }
        }
    }
    wup = wheel_active[0];
    wdwn = wheel_active[1];
    host_unlock(&mouse_lock);

    pad_back_hold();
    pad_shoulders_overlay();
    bindings_read(SDL_GetKeyboardState(NULL), mb, wup, wdwn, gamepad, &b, s);
    /* the overlay's keyboard and controller views light up what is pressed */
    gamemenu_held(SDL_GetKeyboardState(NULL), SDL_GetMouseState(NULL, NULL), pad_parts_held());
    /* The settings panel takes the keyboard, mouse, and the gamepad while it is open, and after it closes until
     * everything is let go, so the button that closed it (B, Escape) doesn't reach the game as well. */
    static int withheld;
    static int was_open;
    int is_open = settings_open() || gamemenu_open();  /* the panel or the in-game overlay */
    if (is_open) {
        withheld = 1;
        if (window && SDL_GetWindowRelativeMouseMode(window))
            SDL_SetWindowRelativeMouseMode(window, false);
    } else if (was_open) {
        if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS))
            SDL_SetWindowRelativeMouseMode(window, true);
    }
    was_open = is_open;
    /* F2's quick look: the cursor is free while it shows, to point at its keys (owner, 2026-11-12). The keyboard
     * and the mouse are the card's alone, as the pad is (pad_back_hold): a key pressed to see it light up must not
     * also act in the game (owner, 2026-11-13), and nothing held when it hides reaches the game until it is let
     * go. F2, Escape and the other window keys are events, not read here, so they still work. The cursor is freed
     * once when it opens and given back when it hides (an overlay that opens from it gives it back itself,
     * above). */
    int peek = gamemenu_peeking();
    if (peek) withheld = 1;
    {
        static int was_peek, peek_freed;
        if (peek && !was_peek) {
            if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false), peek_freed = 1;
        } else if (!peek && was_peek) {
            if (peek_freed && !is_open && window && !SDL_GetWindowRelativeMouseMode(window) &&
                (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS))
                SDL_SetWindowRelativeMouseMode(window, true);
            peek_freed = 0;
        }
        was_peek = peek;
    }
    if (withheld) {
        if (!b && !s[0] && !s[1] && !s[2] && !s[3] && !is_open && !peek) withheld = 0;
        b = 0, memset(s, 0, sizeof s);
    }
    /* the mouse in a supported menu (mousemenu.c): the cursor shows while it is open, and hovering and clicking
     * become the menu's own D-pad, A and B presses; the camera gets the mouse back when it closes */
    if (window && !is_open && !peek) {
        static int freed;  /* we turned the relative mouse off for a menu */
        int ww = 0, wh = 0, want = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        /* the mouse in the picture's own rectangle (the window minus any black bars) */
        float px = picture_rect[0] * (float)ww, py = picture_rect[1] * (float)wh;
        float pw = picture_rect[2] * (float)ww, ph = picture_rect[3] * (float)wh;
        /* a touchpad click begun in the menu is the left button until the touchpad is let go */
        if (tp_game_click && !(gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD))) tp_game_click = 0;
        uint64_t mm = mousemenu_update(mx - px, my - py, (int)(pw + 0.5f), (int)(ph + 0.5f),
                                       SDL_GetMouseState(NULL, NULL) | (tp_game_click ? SDL_BUTTON_LMASK : 0), &want);
        {
            static uint64_t last_mm;
            if (mm != last_mm && mousemenu_log_on()) fprintf(stderr, "yw4: mouse menus: sends %#llx\n", (unsigned long long)mm);
            last_mm = mm;
        }
        b |= mm;
        if (want && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false), freed = 1;
        else if (!want && freed) {
            freed = 0;
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) SDL_SetWindowRelativeMouseMode(window, true);
        }
    }
    if (window_paused()) b = 0, memset(s, 0, sizeof s);  /* the overlay's pause: the player's input held back */
    b |= window_pause_buttons();  /* the overlay's pause: Plus and A pressed for the player */
    host_lock(&input_lock);
    input_buttons = b;
    memcpy(input_sticks, s, sizeof s);
    host_unlock(&input_lock);
}

/* The gamepad outside the game: the overlay button opens or closes the in-game overlay (gamemenu.h): the touchpad
 * on PlayStation pads, the button SDL calls MISC1 on the others (Share on Xbox Series, Capture on Switch Pro), and
 * Back + Start together on any pad (Xbox One and older have no Share), and L + R + ZL + ZR together
 * (pad_shoulders_overlay); on a DualSense MISC1 is the mic button,
 * which shows or hides the performance HUD (F3). (Not Home / PS: Steam takes that one and opens Big Picture.) In
 * the overlay the D-pad or the left stick choose, A selects, B resumes. In the settings panel the D-pad or the left
 * stick choose (up/down) and change (left/right); B closes; on the controls page A changes a binding and X clears
 * it (A, B, X as named on the controller); the shoulder buttons and the triggers switch pages. Named like the
 * keys, so recordings and replays see the same presses. While the controls wait for an input, the press goes
 * there instead. */
void yw4_hid_note_key(const char *name);  /* svc_hid.c: key presses go into the input recording */
void yw4_hid_note_text(const char *utf8);  /* and text typed into the overlay's fields */

static int pad_is_playstation(void) {
    SDL_GamepadType t = SDL_GetGamepadType(gamepad);
    return t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
}

/* the overlay's key hints: 0 the keyboard's (no pad), 1 a pad's, 2 a PlayStation pad's; *close: its overlay button */
/* A point of the window (SDL's window units) in the overlay's design units (its picture is 900 high), 0 outside it. */
static int overlay_point(float mx, float my, float *x, float *y) {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    float px = picture_rect[0] * (float)ww, py = picture_rect[1] * (float)wh;
    float pw = picture_rect[2] * (float)ww, ph = picture_rect[3] * (float)wh;
    if (ph <= 0 || mx < px || my < py || mx >= px + pw || my >= py + ph) return 0;
    float s, ox, oy;
    overlay_fit(pw, ph, &s, &ox, &oy);
    *x = (mx - px - ox) / s, *y = (my - py - oy) / s;
    return 1;
}

static int last_used_pad;  /* the overlay's hints: the controller was used last (else the keyboard or mouse) */
static int used_keys;      /* a key or the mouse was used this run (the controls card: a pad alone means the pad) */
static void used_pad(int pad) {
    if (!pad) used_keys = 1;
    if (pad != last_used_pad) last_used_pad = pad, bindings_pad_only(pad), gamemenu_refresh();
}
/* the controls card's device: YW4_HINTS when set; else the controller when one is connected and was used last, or
 * nothing was used yet (a player who starts the game and picks up the pad) */
static int note_keys_pad(void) {
    int forced = bindings_hints_forced();
    if (forced) return forced >= 2;
    return gamepad && (last_used_pad || !used_keys);
}
static int pad_hints(const char **close) {
    /* YW4_HINTS: one device's hints for the whole run, with or without a controller (screenshots of each style).
     * Without one: PlayStation's touchpad, Switch's Capture, and an Xbox pad without Share */
    int forced = bindings_hints_forced();
    if (forced == 1) return 0;
    if (forced >= 2 && !gamepad) {
        *close = forced == 3   ? (bindings_ps_model() == 3   ? "Select + Start"
                                  : bindings_ps_model() == 4 ? "Share + Options"
                                                             : "Create + Options")
                 : forced == 4 ? "Capture"
                               : (bindings_xbox360() ? "Back + Start (360)" : "Back + Start");
        return forced == 3 ? 2 : 1;
    }
    if (!gamepad || (!last_used_pad && !forced)) return 0;
    int ps = pad_is_playstation();
    /* in the overlay the touchpad clicks (the menu pointer, tp_click): it closes with the two middle buttons */
    if (ps && SDL_GetGamepadType(gamepad) != SDL_GAMEPAD_TYPE_PS3)
        *close = SDL_GetGamepadType(gamepad) == SDL_GAMEPAD_TYPE_PS4 ? "Share + Options" : "Create + Options";
    else if (ps) *close = "Select + Start";  /* a PS3 pad has no touchpad: its Back + Start, by their names */
    else if (SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_MISC1))
        *close = SDL_GetGamepadType(gamepad) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ? "Capture" : "Share";
    else *close = bindings_xbox360() ? "Back + Start (360)" : "Back + Start";  /* 360: the triangles */
    return ps ? 2 : 1;
}

static const char *pad_name(void) { return gamepad ? SDL_GetGamepadName(gamepad) : NULL; }
/* a DualSense: its mic button (SDL's MISC1 on a PlayStation pad) shows or hides the HUD */
static int pad_has_mic(void) {
    if (!gamepad) return bindings_hints_forced() == 3;
    return pad_is_playstation() && SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_MISC1);
}

/* The parts of the controller held now, as the overlay's controller view draws them (gamemenu.h): what is pressed
 * lights up there. By the buttons' places, whatever they are bound to; a stick counts pushed past half way, a
 * trigger pulled past half way, the D-pad in any direction. */
static uint32_t pad_parts_held(void) {
    if (!gamepad) {  /* YW4_PAD_HELD=<mask, hex>: parts held without a controller (tests: a screenshot of the ring) */
        static long env = -1;
        if (env < 0) env = getenv("YW4_PAD_HELD") ? strtol(getenv("YW4_PAD_HELD"), NULL, 16) : 0;
        return (uint32_t)env;
    }
    static const struct {
        SDL_GamepadButton button;
        int part;
    } map[] = {{SDL_GAMEPAD_BUTTON_SOUTH, GM_PAD_SOUTH}, {SDL_GAMEPAD_BUTTON_EAST, GM_PAD_EAST},
               {SDL_GAMEPAD_BUTTON_WEST, GM_PAD_WEST}, {SDL_GAMEPAD_BUTTON_NORTH, GM_PAD_NORTH},
               {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GM_PAD_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GM_PAD_R},
               {SDL_GAMEPAD_BUTTON_BACK, GM_PAD_MINUS}, {SDL_GAMEPAD_BUTTON_START, GM_PAD_PLUS},
               {SDL_GAMEPAD_BUTTON_LEFT_STICK, GM_PAD_LS}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, GM_PAD_RS},
               {SDL_GAMEPAD_BUTTON_DPAD_UP, GM_PAD_DPAD}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, GM_PAD_DPAD},
               {SDL_GAMEPAD_BUTTON_DPAD_LEFT, GM_PAD_DPAD}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GM_PAD_DPAD}};
    uint32_t m = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (SDL_GetGamepadButton(gamepad, map[i].button)) m |= 1u << map[i].part;
    int ps = pad_is_playstation();
    if (ps && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD)) m |= 1u << GM_PAD_SHARE;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_MISC1)) m |= 1u << (ps ? GM_PAD_MIC : GM_PAD_SHARE);
    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) m |= 1u << GM_PAD_ZL;
    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) m |= 1u << GM_PAD_ZR;
    for (int i = 0; i < 2; i++) {
        int x = SDL_GetGamepadAxis(gamepad, i ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX);
        int y = SDL_GetGamepadAxis(gamepad, i ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY);
        if ((int64_t)x * x + (int64_t)y * y > (int64_t)16000 * 16000) m |= 1u << (i ? GM_PAD_RS : GM_PAD_LS);
    }
    return m;
}

/* The quick look by controller (owner, 2026-11-12): holding Back (View / Create / Minus) for half a second while
 * playing shows the Controls card over the running game, as F2 does; while it shows, a short press goes to its
 * next page (Exploring, Battle, Cutscene, round again) and the same half-second hold hides it. The game
 * uses the same button (Minus: auto battle in battles), so its press is held back from the game until we know
 * what it is (bindings_pad_back): let go before the half second, the game gets a short press then (three of its
 * input reads long); held to the half second, the game never sees it. Start pressed meanwhile (Back + Start
 * opens the overlay, panel_pad_button): the hold is dropped and the game sees the buttons as they are. Not with
 * LEGACY MENUS, and not while the overlay or the panel is open (they take the input anyway). Called each time
 * the window polls, before the bindings are read. */
static void pad_back_hold(void) {
    /* PEEK_WAIT: pressed while the card shows; let go before the half second: its next page, held: it hides
     * (owner, 2026-11-13: a short press cycles the pages, not LB / RB) */
    enum { IDLE, WAITING, SWALLOWED, HANDED_OVER, PEEK_WAIT };  /* SWALLOWED, HANDED_OVER: until let go */
    static int state, tapping;
    static uint64_t since, tap_until;  /* ms; the game's input reads */
    int down = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK);
    int start = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);
    uint64_t now = SDL_GetTicks(), reads = window_input_reads();
    if (tapping && reads >= tap_until) tapping = 0;
    if (settings_legacy_keys() || settings_open() || gamemenu_open() || bindings_capturing()) {
        state = down ? HANDED_OVER : IDLE, tapping = 0;
    } else if (!down) {
        if (state == WAITING) tapping = 1, tap_until = reads + 3;  /* a short press: the game's, now */
        if (state == PEEK_WAIT && gamemenu_peeking()) gamemenu_peek_mode_step(1), yw4_hid_note_key("KV_NEXT");
        state = IDLE;
    } else if (state == IDLE) {
        if (gamemenu_peeking()) state = PEEK_WAIT, since = now;
        else state = start ? HANDED_OVER : WAITING, since = now;
    } else if (state == WAITING) {
        if (start) state = HANDED_OVER;
        else if (now - since >= 500) gamemenu_peek_toggle(), yw4_hid_note_key("KV_PEEK"), state = SWALLOWED;
    } else if (state == PEEK_WAIT) {
        if (!gamemenu_peeking()) state = SWALLOWED;  /* hidden meanwhile (F2) */
        else if (now - since >= 500) gamemenu_peek_toggle(), yw4_hid_note_key("KV_PEEK"), state = SWALLOWED;
    }
    bindings_pad_back(state == WAITING || state == SWALLOWED || state == PEEK_WAIT ? 0 : tapping ? 1 : -1);
    /* While the quick look shows the controller's view, the pad is the card's alone: a button pressed to see it
     * light up must not also act in the game (owner, 2026-11-12). The keyboard and mouse are held back where the window reads them
     * (update_input's withheld).
     * Kept until everything on the pad is let go after it hides, so nothing held then reaches the game. */
    static int locked;
    if (gamemenu_peeking() && last_used_pad) locked = 1;
    else if (locked && !pad_parts_held()) locked = 0;
    bindings_pad_lock(locked);
}

/* All four shoulder buttons held together (L + R + ZL + ZR) open or close the overlay too, once per hold. For
 * Switch-style pads in X-input mode (owner, 2026-11-13): they send a Guide press of their own with Back + Start,
 * so Windows' Game Bar and Steam's overlay open with ours. Four at once is hard to press by accident, unlike the
 * two stick presses. Not while the controls wait for an input (a shoulder or trigger being bound). */
static void pad_shoulders_overlay(void) {
    static int held;
    int all = gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
              SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) &&
              SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000 &&
              SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    if (all && !held && !bindings_capturing())
        gamemenu_toggle(), yw4_hid_note_key("MENU");
    held = all;
}

/* ---- the touchpad as a menu pointer (tp_game_click, tp_moved_ms: above). A PlayStation pad with a touchpad, while
 * one of the game's menus (mousemenu.c) or our overlay is open: dragging a finger moves the cursor from where it is,
 * like a laptop trackpad (lift and drag again to go further), and the touchpad click chooses: in the overlay a
 * click at the cursor, in a game menu the left button (hover moves the focus, a click picks: mousemenu.c). The
 * cursor really moves (a warp), so both see it as the mouse; that motion still counts as the controller's.
 * YW4_TOUCHPAD_SPEED scales it (1: one full stroke across the pad is 55% of the window's width). */
static int tp_pointer_here(void) {
    if (!gamepad || !pad_is_playstation() || SDL_GetNumGamepadTouchpads(gamepad) < 1) return 0;
    if (settings_open() || gamemenu_peeking()) return 0;
    return gamemenu_open() ? !bindings_capturing() : mousemenu_open();
}

/* The touchpad's cursor: a see-through grey disc with a white edge, like a console's pointer (the owner's pick B
 * from mockups, 2026-11-14), 3% of the window's height across. Shown while the touchpad moves the pointer; the
 * usual arrow comes back when the mouse moves (the SDL_EVENT_MOUSE_MOTION case). */
static SDL_Cursor *tp_cursor;
static int tp_cursor_size, tp_cursor_on;

static float tp_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void tp_cursor_show(int wh) {
    int d = (int)(wh * 0.03f + 0.5f);
    if (d < 20) d = 20;
    if (!tp_cursor || d != tp_cursor_size) {
        int pad = 4, n = d + 2 * pad;  /* room for the dark edge and the shadow */
        SDL_Surface *s = SDL_CreateSurface(n, n, SDL_PIXELFORMAT_RGBA32);
        if (!s) return;
        float c = n / 2.0f, R = d / 2.0f;
        for (int y = 0; y < n; y++) {
            uint8_t *row = (uint8_t *)s->pixels + y * s->pitch;
            for (int x = 0; x < n; x++) {
                float r = sqrtf((x + 0.5f - c) * (x + 0.5f - c) + (y + 0.5f - c) * (y + 0.5f - c));
                float rs = sqrtf((x + 0.5f - c) * (x + 0.5f - c) + (y - 0.5f - c) * (y - 0.5f - c));  /* 1 px lower */
                /* layers, bottom up (colour 0..1, alpha 0..1): a soft shadow, a thin dark edge, the grey disc,
                 * the white ring at its edge (each anti-aliased by its distance from the circle) */
                float L[4][4] = {
                    {0, 0, 0, 0.27f * tp_clamp01(1 - (rs - R) / 3.5f) * (rs > R - 1)},
                    {0.12f, 0.08f, 0.2f, 0.47f * tp_clamp01(R + 1.5f - r) * tp_clamp01(r - R + 1)},
                    {0.24f, 0.24f, 0.27f, 0.43f * tp_clamp01(R - r + 0.5f)},
                    {1, 1, 1, 0.92f * tp_clamp01(R - r + 0.5f) * tp_clamp01(r - (R - 2.4f) + 0.5f)}};
                float cr = 0, cg = 0, cb = 0, ca = 0;  /* "over", straight alpha */
                for (int k = 0; k < 4; k++) {
                    float a = L[k][3], na = a + ca * (1 - a);
                    if (na > 0) {
                        cr = (L[k][0] * a + cr * ca * (1 - a)) / na;
                        cg = (L[k][1] * a + cg * ca * (1 - a)) / na;
                        cb = (L[k][2] * a + cb * ca * (1 - a)) / na;
                    }
                    ca = na;
                }
                row[x * 4 + 0] = (uint8_t)(cr * 255 + 0.5f), row[x * 4 + 1] = (uint8_t)(cg * 255 + 0.5f);
                row[x * 4 + 2] = (uint8_t)(cb * 255 + 0.5f), row[x * 4 + 3] = (uint8_t)(ca * 255 + 0.5f);
            }
        }
        SDL_Cursor *cur = SDL_CreateColorCursor(s, n / 2, n / 2);
        SDL_DestroySurface(s);
        if (!cur) return;
        if (tp_cursor_on) SDL_SetCursor(cur);
        if (tp_cursor) SDL_DestroyCursor(tp_cursor);
        tp_cursor = cur, tp_cursor_size = d;
    }
    if (!tp_cursor_on) SDL_SetCursor(tp_cursor), tp_cursor_on = 1;
}

static void tp_cursor_hide(void) {
    if (tp_cursor_on) SDL_SetCursor(SDL_GetDefaultCursor()), tp_cursor_on = 0;
}

static void tp_touch(Uint32 type, float fx, float fy) {
    static float lx, ly;
    static int down;
    if (type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP) {
        down = 0;
        return;
    }
    if (type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN || !down) {  /* a new stroke: it moves from here */
        lx = fx, ly = fy, down = 1;
        return;
    }
    float dx = fx - lx, dy = fy - ly;
    lx = fx, ly = fy;
    if (!window || !tp_pointer_here()) return;
    static float speed = -1;
    if (speed < 0) {
        const char *e = getenv("YW4_TOUCHPAD_SPEED");
        speed = e && atof(e) > 0 ? (float)atof(e) : 1.0f;
    }
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    /* the pad's 0..1 is about 2.2 times wider than tall (a DualSense's ~52 x 23 mm): the same finger distance
     * moves the same screen distance either way */
    float k = 0.55f * speed * (float)ww, x, y;  /* 0.9 was too quick (owner, 2026-11-14) */
    SDL_GetMouseState(&x, &y);
    x += dx * k, y += dy * k * 0.45f;
    /* kept in the picture (the window minus its black bars) */
    float px = picture_rect[0] * (float)ww, py = picture_rect[1] * (float)wh;
    float pw = picture_rect[2] * (float)ww, ph = picture_rect[3] * (float)wh;
    x = x < px ? px : x > px + pw - 1 ? px + pw - 1 : x;
    y = y < py ? py : y > py + ph - 1 ? py + ph - 1 : y;
    tp_moved_ms = SDL_GetTicks();
    tp_cursor_show(wh);
    SDL_WarpMouseInWindow(window, x, y);
    used_pad(1);
}

/* the touchpad clicked: 1 when it chose in a menu (then it isn't the overlay button) */
static int tp_click(void) {
    if (!tp_pointer_here()) return 0;
    if (gamemenu_open()) {
        float mx, my, x, y;
        SDL_GetMouseState(&mx, &my);
        if (overlay_point(mx, my, &x, &y)) gamemenu_mouse(x, y, 1);
    } else {
        tp_game_click = 1;  /* the menu's left button until the touchpad is let go (update_input) */
    }
    return 1;
}

static void panel_pad_button(SDL_GamepadButton button) {
    /* the overlay's controls page waits for an input to bind: it takes the press */
    if (gamemenu_open() && !settings_open() && bindings_capturing() && bindings_capture_button(button)) return;
    int both = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) &&
               SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);
    /* buttons a Switch controller doesn't have (or two at once), so the game never sees them alone */
    int ps = pad_is_playstation();
    if (button == SDL_GAMEPAD_BUTTON_TOUCHPAD && ps && tp_click()) return;  /* in a menu: it chooses */
    if ((button == SDL_GAMEPAD_BUTTON_TOUCHPAD && ps) || (button == SDL_GAMEPAD_BUTTON_MISC1 && !ps) ||
        (both && (button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_START))) {
        gamemenu_toggle(), yw4_hid_note_key("MENU");
        return;
    }
    if (button == SDL_GAMEPAD_BUTTON_MISC1) {  /* the DualSense's mic button: the HUD */
        window_hud_toggle();
        return;
    }
    if (gamemenu_open() && !settings_open()) {  /* the overlay's side panel */
        int nin = bindings_pad_nintendo();
        const struct {
            SDL_GamepadButton button;
            int action;
            const char *name;
        } map[] = {{SDL_GAMEPAD_BUTTON_DPAD_UP, GM_UP, "M_UP"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, GM_DOWN, "M_DOWN"},
                   {SDL_GAMEPAD_BUTTON_DPAD_LEFT, GM_LEFT, "M_LEFT"}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GM_RIGHT, "M_RIGHT"},
                   {nin ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, GM_ACCEPT, "M_OK"},
                   {nin ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST, GM_BACK, "M_BACK"},
                   {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GM_PREV, "M_PREV"},
                   {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GM_NEXT, "M_NEXT"},
                   {nin ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST, GM_CLEAR, "M_CLEAR"},
                   /* Y (Triangle): Preview on the Settings page, its bar hidden or shown in it */
                   {nin ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH, GM_ALT, "M_ALT"}};
        for (size_t i = 0; i < sizeof map / sizeof *map; i++)
            if (button == map[i].button) gamemenu_action(map[i].action), yw4_hid_note_key(map[i].name);
        return;
    }
    if (!settings_open() || bindings_capture_button(button)) return;
    /* A changes, B closes, X clears, by the names on the controller: a Nintendo one has A on the right, B at the
     * bottom and X on top; Xbox and PlayStation ones A (cross) at the bottom, B (circle) right, X (square) left */
    int nin = bindings_pad_nintendo();
    const struct {
        SDL_GamepadButton button;
        int action;
        const char *name;
    } map[] = {{SDL_GAMEPAD_BUTTON_DPAD_UP, SETTINGS_UP, "UP"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SETTINGS_DOWN, "DOWN"},
               {SDL_GAMEPAD_BUTTON_DPAD_LEFT, SETTINGS_LEFT, "LEFT"},
               {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SETTINGS_RIGHT, "RIGHT"},
               {nin ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST, SETTINGS_CLOSE, "F1"},
               {nin ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, SETTINGS_ACCEPT, "ENTER"},
               {nin ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST, SETTINGS_CLEAR, "DELETE"},
               {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SETTINGS_PAGE, "PAGE"},
               {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SETTINGS_PAGE, "PAGE"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (button == map[i].button) settings_action(map[i].action), yw4_hid_note_key(map[i].name);
}

/* The triggers in the panel switch pages, once per pull past three quarters. */
static void panel_pad_triggers(void) {
    static int held[2];
    for (int i = 0; i < 2; i++) {
        int v = SDL_GetGamepadAxis(gamepad, i ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        if (v < 8000) held[i] = 0;
        else if (v > 24000 && !held[i]) {
            held[i] = 1;
            if (bindings_capturing()) continue;
            if (gamemenu_open() && !settings_open())  /* the overlay: LT / RT as LB / RB, a page's sections */
                gamemenu_action(i ? GM_NEXT : GM_PREV), yw4_hid_note_key(i ? "M_NEXT" : "M_PREV");
            else settings_action(SETTINGS_PAGE), yw4_hid_note_key("PAGE");
        }
    }
}

/* The left stick as a D-pad in the panel: one step each time it is pushed past half way. */
static void panel_pad_stick(void) {
    static int held;  /* the direction pushed, until the stick comes back near the centre */
    int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < 8000 && ay < 8000) held = 0;
    if (held || !(settings_open() || gamemenu_open()) || (ax < 16000 && ay < 16000)) return;
    if (bindings_capturing()) return;  /* bindings_capture_axis takes it */
    held = 1;
    panel_pad_button(ay > ax ? (y < 0 ? SDL_GAMEPAD_BUTTON_DPAD_UP : SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                             : (x < 0 ? SDL_GAMEPAD_BUTTON_DPAD_LEFT : SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
}

/* The overlay: a D-pad direction or the stick held repeats, as a held key does (after 0.4 s, then 11 a second). The
 * first step is the press's own (panel_pad_button, panel_pad_stick); this adds the rest. */
static void overlay_pad_repeat(void) {
    static int dir = -1;
    static uint64_t next;
    static const SDL_GamepadButton dirs[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                              SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    int d = -1;
    if (gamepad && gamemenu_open() && !settings_open() && !bindings_capturing()) {
        for (int i = 0; i < 4 && d < 0; i++)
            if (SDL_GetGamepadButton(gamepad, dirs[i])) d = i;
        if (d < 0) {
            int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
            int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
            if (ax >= 16000 || ay >= 16000) d = ay > ax ? (y < 0 ? 0 : 1) : (x < 0 ? 2 : 3);
        }
    }
    uint64_t now = SDL_GetTicks();
    if (d != dir) {
        dir = d, next = now + 400;
        return;
    }
    if (d >= 0 && now >= next) next = now + 90, panel_pad_button(dirs[d]);
}

/* ---- full screen (YW4_WINDOW, panel WINDOW MODE, F11): borderless, the size of the desktop (SDL3's fullscreen
 * without a display mode), so the monitor's mode never changes and Alt+Tab is instant. The panel can change it
 * from any thread (a replay's keys run on the guest's), so the wish is a flag and the window's thread applies it. */
static int fullscreen_want = -1;  /* -1: not read yet */

int window_fullscreen_index(void) {
    int w = __atomic_load_n(&fullscreen_want, __ATOMIC_ACQUIRE);
    if (w >= 0) return w;
    const char *e = getenv("YW4_WINDOW");
    w = e && !strcmp(e, "fullscreen");
    __atomic_store_n(&fullscreen_want, w, __ATOMIC_RELEASE);
    return w;
}

void window_fullscreen_cycle(void) {
    int w = !window_fullscreen_index();
    __atomic_store_n(&fullscreen_want, w, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: window: %s\n", w ? "full screen" : "windowed");
}

static void fullscreen_apply(void) {  /* the window's thread */
    int want = window_fullscreen_index();
    if (window && !!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != want) SDL_SetWindowFullscreen(window, want);
}

/* ---- the window's thread */

typedef struct {
    void (*fn)(void *);
    void *arg;
} Guest;

static void guest_main(void *p) {
    Guest *g = p;
    g->fn(g->arg);
    yw4_exit(0, __FILE__, __LINE__);  /* the game's main returned */
}

void window_request_screenshot(void) { __atomic_store_n(&screenshot_requested, screenshot_burst(), __ATOMIC_RELEASE); }

static void start_guest_thread(void (*guest)(void *), void *arg) {
    static Guest g;
    g.fn = guest, g.arg = arg;
    yw4_boot_mark("the guest starts");
    if (host_thread_start(guest_main, &g, GUEST_HOST_STACK)) {
        fprintf(stderr, "yw4: window: cannot start the guest's thread\n");
        yw4_exit(1, __FILE__, __LINE__);
    }
}

/* ---- the window's size. At start: YW4_WINDOW_SIZE=<w>x<h> (the environment, or the settings file's line: the
 * size the player left the window at), cut to what the desktop can show; without it 80% of the desktop's usable
 * area, 16:9, and never under the Switch's 1280x720 (a fixed 1280x720 looked small on a 1440p or 4K monitor).
 * While playing: a size the player gives the window is written to the settings file once it has stayed for a
 * second (a drag passes through many sizes). Full screen, maximized and minimized sizes are not the window's
 * own, so they are never kept: leaving full screen goes back to the kept size. */
static int size_kept[2];  /* 0: none yet */

void window_size_kept(int *w, int *h) {
    *w = __atomic_load_n(&size_kept[0], __ATOMIC_ACQUIRE), *h = __atomic_load_n(&size_kept[1], __ATOMIC_ACQUIRE);
    const char *ws = getenv("YW4_WINDOW_SIZE");  /* before the window exists: the line that was read */
    if ((!*w || !*h) && !(ws && sscanf(ws, "%dx%d", w, h) == 2 && *w > 0 && *h > 0)) *w = *h = 0;
}

static void window_start_size(int *w, int *h) {  /* after SDL_Init */
    SDL_Rect r;
    bool have = SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r) && r.w > 0 && r.h > 0;
    int sw, sh;
    const char *ws = getenv("YW4_WINDOW_SIZE");
    *w = 1280, *h = 720;
    if (ws && sscanf(ws, "%dx%d", &sw, &sh) == 2 && sw >= 320 && sh >= 180) {
        *w = have && sw > r.w ? r.w : sw, *h = have && sh > r.h ? r.h : sh;  /* kept on a larger monitor */
    } else if (have) {
        int fh = r.h * 4 / 5, fw = fh * 16 / 9;
        if (fw > r.w * 4 / 5) fw = r.w * 4 / 5, fh = fw * 9 / 16;
        if (fw > *w) *w = fw & ~1, *h = fh & ~1;
    }
    __atomic_store_n(&size_kept[0], *w, __ATOMIC_RELEASE), __atomic_store_n(&size_kept[1], *h, __ATOMIC_RELEASE);
}

static void window_size_track(void) {  /* the window's thread, each time it polls */
    static int cand[2];
    static uint64_t since;
    if (!window || window_fullscreen_index() ||
        (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
        since = 0;
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0 || (w == size_kept[0] && h == size_kept[1])) {
        since = 0;
        return;
    }
    uint64_t now = SDL_GetTicks();
    if (!since || w != cand[0] || h != cand[1]) {
        cand[0] = w, cand[1] = h, since = now ? now : 1;
        return;
    }
    if (now - since < 1000) return;
    __atomic_store_n(&size_kept[0], w, __ATOMIC_RELEASE), __atomic_store_n(&size_kept[1], h, __ATOMIC_RELEASE);
    since = 0;
    settings_save();
}

void window_run(void (*guest)(void *), void *arg) {
    const char *dir = getenv("YW4_SCREENSHOT_DIR");
    if (dir && *dir) snprintf(screenshot_dir, sizeof screenshot_dir, "%s", dir);
    const char *headless = getenv("YW4_HEADLESS");
    if (!(headless && *headless && *headless != '0')) {
        /* the game's icon, resource 1 in the exe when the build had one (decision 0048); without it Windows' default */
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5_RUMBLE", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4_RUMBLE", "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
        int win_w = 1280, win_h = 720;
        yw4_boot_mark("save folder ready; starting SDL");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "yw4: window: SDL_Init: %s; running headless\n", SDL_GetError());
        } else if (window_start_size(&win_w, &win_h), !(window = SDL_CreateWindow("Yo-kai Watch 4++", win_w, win_h, SDL_WINDOW_VULKAN |
                                               SDL_WINDOW_RESIZABLE |
                                               (window_fullscreen_index() ? SDL_WINDOW_FULLSCREEN : 0)))) {
            fprintf(stderr, "yw4: window: SDL_CreateWindow: %s; running headless\n", SDL_GetError());
        } else {
            if (boot_parallel()) start_guest_thread(guest, arg);  /* it boots while Vulkan starts */
            if (vk_init()) {
                fprintf(stderr, "yw4: window: no Vulkan; the window stays empty\n");
            } else {
                have_vulkan = 1;
                gpu_init();
                yw4_boot_mark("draw path ready");
                if (!mouse_off()) SDL_SetWindowRelativeMouseMode(window, true);
            }
        }
    }
    gpu_start_done();
    if (!window) {
        guest(arg);
        return;
    }
    if (!boot_parallel()) start_guest_thread(guest, arg);
    host_thread_set_priority(NULL, 39);  /* event pump and input polling: stay responsive under CPU load */
    for (;;) {
        SDL_Event e;
        int got = SDL_WaitEventTimeout(&e, 8);
        {  /* the window's display's refresh, for the frame pacing (framerate.c); read here, SDL's thread */
            static int64_t last;
            int64_t now = host_now_ns();
            if (now - last > 1000000000ll) {
                last = now;
                const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
                double hz = !m ? 0 : m->refresh_rate_numerator && m->refresh_rate_denominator
                                         ? (double)m->refresh_rate_numerator / m->refresh_rate_denominator
                                         : m->refresh_rate;
                __atomic_store_n(&refresh_mhz, (int64_t)(hz * 1000.0 + 0.5), __ATOMIC_RELEASE);
            }
        }
        update_input();
        overlay_pad_repeat();
        fullscreen_apply();
        window_size_track();
        {  /* the system's text input (a keyboard layout's letters, an IME) runs while a text field has the focus */
            static int text_on;
            static SDL_Rect last_rect;
            static int last_caret = -1;
            int want = window && gamemenu_open() && textin_active();
            if (want != text_on) {
                if (want) {
                    SDL_StartTextInput(window);
                } else if (window) {
                    SDL_StopTextInput(window);
                    memset(&last_rect, 0, sizeof last_rect);
                    last_caret = -1;
                }
                text_on = want;
            }
            if (want) {
                float r[4], caret_x;
                if (gamemenu_text_rect(r, &caret_x)) {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(window, &ww, &wh);
                    float px = picture_rect[0] * (float)ww, py = picture_rect[1] * (float)wh;
                    float pw = picture_rect[2] * (float)ww, ph = picture_rect[3] * (float)wh;
                    if (ph > 0) {
                        float s, ox, oy;
                        overlay_fit(pw, ph, &s, &ox, &oy);
                        SDL_Rect rect;
                        rect.x = (int)roundf(px + ox + r[0] * s);
                        rect.y = (int)roundf(py + oy + r[1] * s);
                        rect.w = (int)roundf(r[2] * s);
                        rect.h = (int)roundf(r[3] * s);
                        int caret_offset = (int)roundf((caret_x - r[0]) * s);
                        if (caret_offset < 0) caret_offset = 0;
                        else if (caret_offset > rect.w) caret_offset = rect.w;
                        if (rect.x != last_rect.x || rect.y != last_rect.y ||
                            rect.w != last_rect.w || rect.h != last_rect.h ||
                            caret_offset != last_caret) {
                            SDL_SetTextInputArea(window, &rect, caret_offset);
                            last_rect = rect;
                            last_caret = caret_offset;
                            fprintf(stderr, "yw4: text input area: %d,%d %dx%d caret %d (window %dx%d)\n",
                                    rect.x, rect.y, rect.w, rect.h, caret_offset, ww, wh);
                        }
                    }
                }
            }
        }
        if (!got) continue;
        switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
            if (gamemenu_open() && textin_active()) textin_type(e.text.text), yw4_hid_note_text(e.text.text);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            host_lock(&input_lock);
            if (!gamepad && (gamepad = SDL_OpenGamepad(e.gdevice.which)))
                fprintf(stderr, "yw4: window: gamepad %s\n", SDL_GetGamepadName(gamepad));
            host_unlock(&input_lock);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            host_lock(&input_lock);
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
                fprintf(stderr, "yw4: window: gamepad removed\n");
            }
            host_unlock(&input_lock);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gbutton.which) used_pad(1), panel_pad_button(e.gbutton.button);
            break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:  /* the first finger moves the menu pointer (tp_touch) */
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gtouchpad.which && e.gtouchpad.finger == 0)
                tp_touch(e.type, e.gtouchpad.x, e.gtouchpad.y);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gaxis.which && (settings_open() || gamemenu_open()) &&
                !bindings_capture_axis(e.gaxis.axis, e.gaxis.value)) {
                if (e.gaxis.value > 16000 || e.gaxis.value < -16000) used_pad(1);
                panel_pad_stick(), panel_pad_triggers();
            } else if (gamepad && SDL_GetGamepadID(gamepad) == e.gaxis.which && gamemenu_peeking() &&
                       (e.gaxis.value > 16000 || e.gaxis.value < -16000)) {
                used_pad(1);  /* F2's quick look: a stick or trigger shows the controller's view */
            }
            break;
        case SDL_EVENT_MOUSE_MOTION: {
            /* the touchpad's own cursor move (tp_touch) is the controller's, not the mouse's; the mouse itself
             * moving brings the usual arrow back */
            int pad_move = tp_moved_ms && SDL_GetTicks() - tp_moved_ms < 150;
            if (!pad_move) tp_cursor_hide();
            if (window && SDL_GetWindowRelativeMouseMode(window) && !gamemenu_peeking()) {
                host_lock(&mouse_lock);  /* the camera's; never while the quick look shows */
                mouse_accum_x += e.motion.xrel;
                mouse_accum_y += e.motion.yrel;
                host_unlock(&mouse_lock);
            } else if (gamemenu_open() && !settings_open()) {  /* the overlay: hovering chooses */
                float x, y;
                if (overlay_point(e.motion.x, e.motion.y, &x, &y)) {
                    if (!pad_move) used_pad(0);
                    gamemenu_mouse(x, y, e.motion.state & SDL_BUTTON_LMASK ? 4 : 0);
                }
            } else if (gamemenu_peeking()) {  /* the quick look: the key under the mouse */
                float x, y;
                if (overlay_point(e.motion.x, e.motion.y, &x, &y)) gamemenu_peek_point(x, y);
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (settings_open() || gamemenu_open()) {  /* not the game's: the panel's or the overlay's */
                float x, y;
                if (bindings_capturing()) bindings_capture_mouse(e.button.button);
                else if (!settings_open() && (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT) &&
                         overlay_point(e.button.x, e.button.y, &x, &y))
                    used_pad(0), gamemenu_mouse(x, y, e.button.button == SDL_BUTTON_LEFT ? (e.button.clicks >= 2 ? 2 : 1) : 3);
            } else if (gamemenu_peeking()) {  /* the quick look: a click doesn't take the cursor back (nor Alt) */
                float x, y;
                used_pad(0);  /* but shows the keyboard's view, and on the mode switch changes the mode */
                if (e.button.button == SDL_BUTTON_LEFT && overlay_point(e.button.x, e.button.y, &x, &y))
                    gamemenu_peek_click(x, y);
            } else if (window && !SDL_GetWindowRelativeMouseMode(window) && !mouse_off() && !mousemenu_open()) {
                SDL_SetWindowRelativeMouseMode(window, true);
                mouse_held_off = 1;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (settings_open() || gamemenu_open()) {
                if (bindings_capturing()) bindings_capture_wheel(e.wheel.y > 0 ? 1 : -1);
                else if (!settings_open() && e.wheel.y != 0) used_pad(0), gamemenu_wheel(e.wheel.y > 0 ? 1 : -1);
            } else if (e.wheel.y != 0 && mousemenu_wheel(e.wheel.y > 0 ? 1 : -1)) {
                used_pad(0);
            } else if (e.wheel.y != 0) {
                host_lock(&mouse_lock);
                int dir = e.wheel.y > 0 ? 0 : 1;
                int notches = e.wheel.y > 0 ? (e.wheel.y >= 1.0f ? (int)e.wheel.y : 1)
                                            : (e.wheel.y <= -1.0f ? -(int)e.wheel.y : 1);
                wheel_queue[!dir] = 0;  /* scrolling the other way cancels pending opposite steps */
                wheel_queue[dir] += notches;
                if (wheel_queue[dir] > 8) wheel_queue[dir] = 8;
                host_unlock(&mouse_lock);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (window && SDL_GetWindowRelativeMouseMode(window)) {
                SDL_SetWindowRelativeMouseMode(window, false);
            }
            break;
        case SDL_EVENT_QUIT:
            fprintf(stderr, "yw4: window closed\n");
            yw4_exit(0, __FILE__, __LINE__);
        case SDL_EVENT_KEY_DOWN:
            /* F2's quick look: a key (not F1..F12, so F12's screenshot keeps the view) shows the keyboard's view */
            if (gamemenu_peeking() && !e.key.repeat && (e.key.scancode < SDL_SCANCODE_F1 || e.key.scancode > SDL_SCANCODE_F12))
                used_pad(0);  /* no keys for its mode switch: keyboard players click it (owner, 2026-11-13) */
            /* Alt: the mouse to the camera and back; not while F2's quick look shows (owner, 2026-11-13: the cursor
             * stays a pointer there, and Alt pressed to see it light up mustn't take it away) */
            if ((e.key.key == SDLK_LALT || e.key.key == SDLK_RALT) && !settings_open() && !mouse_off() &&
                !gamemenu_peeking()) {
                if (window) {
                    bool rel = SDL_GetWindowRelativeMouseMode(window);
                    SDL_SetWindowRelativeMouseMode(window, !rel);
                }
            }
            if (e.key.key == SDLK_F12) __atomic_store_n(&screenshot_requested, screenshot_burst(), __ATOMIC_RELEASE),
                                        __atomic_store_n(&flash_dump_requested, 1, __ATOMIC_RELEASE), yw4_hid_note_key("F12");
            /* F11: full screen, like most PC games (saved like the panel's row); Shift+F11: the draws of the next
             * frame, in the log (still "F11" in recordings) */
            if (e.key.key == SDLK_F11 && !e.key.repeat) {
                if (e.key.mod & SDL_KMOD_SHIFT) gpu_request_trace(12000), yw4_hid_note_key("F11");
                else window_fullscreen_cycle(), settings_save();
            }
            if (e.key.key == SDLK_F10) gpu_request_dump(), yw4_hid_note_key("F10");      /* shadow maps etc.: work/dump/ */
            if (e.key.key == SDLK_F3 && settings_legacy_keys()) window_hud_toggle(), settings_save();  /* the HUD */
            /* LEGACY MENUS off (the default): F1 and F2 open the overlay on its Settings and Controls pages instead
             * (owner, 2026-11-08); the same key again resumes. F3 cycles the HUD (owner, 2026-11-11). Not while the
             * old panel is open: there F1 and F2 still close it (below). Not while typing: F keys go nowhere then. */
            if (!settings_legacy_keys() && !settings_open() && !e.key.repeat && !textin_active() &&
                e.key.key == SDLK_F3) {
                window_hud_cycle(), yw4_hid_note_key("HUDCYC");
                break;
            }
            /* F2 while playing: the keyboard card as a quick look, the game not paused (owner, 2026-11-11); in the
             * overlay F2 still goes to its Controls page */
            if (!settings_legacy_keys() && !settings_open() && !e.key.repeat && !textin_active() &&
                e.key.key == SDLK_F2 && !gamemenu_open()) {
                used_pad(0);  /* by the keyboard: its view */
                gamemenu_peek_toggle(), yw4_hid_note_key("KV_PEEK");
                break;
            }
            if (!settings_legacy_keys() && !settings_open() && !e.key.repeat && !textin_active() &&
                (e.key.key == SDLK_F1 || e.key.key == SDLK_F2)) {
                if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                if (e.key.key == SDLK_F1) gamemenu_open_page(GM_SETTINGS), yw4_hid_note_key("M_SET");
                else gamemenu_open_page(GM_CONTROLS), yw4_hid_note_key("M_CTRL");
                break;
            }
            if (e.key.key == SDLK_F4 && !e.key.repeat) game_pause_toggle();  /* test: the overlay's pause (below) */
            if (e.key.key == SDLK_F5) gpu_passes_toggle();  /* per-pass GPU timing in the log */
            if (e.key.key == SDLK_F7 && !e.key.repeat) screen_passes_toggle();  /* the screen passes: ours or the game's */
            /* the settings live in the panel only (F1); its keys are recorded like the F keys, so a replay
             * of a recording changes the same settings at the same ticks. While the controls page waits for
             * a key, any key but F1..F12 is the new binding. LEGACY MENUS off: F1 and F2 only still close a panel
             * that is open (the setting was just turned off in it). */
            int legacy_panel = settings_legacy_keys() || settings_open();
            if (e.key.key == SDLK_F1 && legacy_panel) {
                if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                settings_show(0), yw4_hid_note_key("F1");  /* the settings */
            }
            if (e.key.key == SDLK_F2 && legacy_panel) {
                if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                settings_show(1), yw4_hid_note_key("F2");  /* the controls */
            }
            /* the in-game overlay (gamemenu.h): Escape opens it; while it shows, the arrows or W A S D choose and
             * change, Enter or Space selects, Escape or Backspace go back one step (from the side panel: resume).
             * A settings page it opened takes Escape itself (back to the overlay). */
            /* typing in a text field of the overlay (textin.h): the letters come as text (above); here only the keys
             * that edit, paste or end the typing; every other key goes nowhere, so W A S D type instead of moving */
            if (gamemenu_open() && !settings_open() && textin_active() &&
                (e.key.scancode < SDL_SCANCODE_F1 || e.key.scancode > SDL_SCANCODE_F12)) {
                SDL_Keycode k = e.key.key;
                static const struct {
                    SDL_Keycode key;
                    int op;
                    const char *name;
                } ek[] = {{SDLK_LEFT, TI_LEFT, "T_LEFT"},    {SDLK_RIGHT, TI_RIGHT, "T_RIGHT"},
                          {SDLK_HOME, TI_HOME, "T_HOME"},    {SDLK_END, TI_END, "T_END"},
                          {SDLK_BACKSPACE, TI_BACK, "T_BACK"}, {SDLK_DELETE, TI_DEL, "T_DEL"},
                          {SDLK_RETURN, TI_DONE, "T_DONE"},  {SDLK_KP_ENTER, TI_DONE, "T_DONE"},
                          {SDLK_ESCAPE, TI_DONE, "T_DONE"},  {SDLK_TAB, TI_DONE, "T_DONE"}};
                used_pad(0);
                if (k == SDLK_C && (e.key.mod & SDL_KMOD_CTRL)) {  /* copy */
                    textin_edit(TI_COPY), yw4_hid_note_key("T_COPY");
                    break;
                }
                if (k == SDLK_X && (e.key.mod & SDL_KMOD_CTRL)) {  /* cut */
                    textin_edit(TI_CUT), yw4_hid_note_key("T_CUT");
                    break;
                }
                if (k == SDLK_A && (e.key.mod & SDL_KMOD_CTRL)) {  /* select all */
                    textin_edit(TI_ALL), yw4_hid_note_key("T_ALL");
                    break;
                }
                if (k == SDLK_V && (e.key.mod & SDL_KMOD_CTRL)) {  /* paste */
                    char *clip = SDL_GetClipboardText();
                    if (clip && *clip) textin_type(clip), yw4_hid_note_text(clip);
                    SDL_free(clip);
                    break;
                }
                if (e.key.mod & SDL_KMOD_SHIFT) {
                    if (k == SDLK_LEFT) { textin_edit(TI_SLEFT), yw4_hid_note_key("T_SLEFT"); break; }
                    if (k == SDLK_RIGHT) { textin_edit(TI_SRIGHT), yw4_hid_note_key("T_SRIGHT"); break; }
                    if (k == SDLK_HOME) { textin_edit(TI_SHOME), yw4_hid_note_key("T_SHOME"); break; }
                    if (k == SDLK_END) { textin_edit(TI_SEND), yw4_hid_note_key("T_SEND"); break; }
                }
                if (k == SDLK_ESCAPE && !e.key.repeat && gamemenu_prompting()) {  /* the game's popup: Cancel where offered */
                    gamemenu_prompt_cancel();
                    if (!gamemenu_prompting()) break;
                }
                for (size_t i = 0; i < sizeof ek / sizeof *ek; i++)
                    if (k == ek[i].key && !(e.key.repeat && ek[i].op == TI_DONE)) {
                        textin_edit(ek[i].op), yw4_hid_note_key(ek[i].name);
                        if (ek[i].op == TI_DONE) gamemenu_refresh();
                    }
                break;
            }
            if (!settings_open()) {
                /* held keys repeat only to move and change (the arrows, W A S D), never to select or go back */
                SDL_Keycode k = e.key.key;
                int moves = k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_W ||
                            k == SDLK_A || k == SDLK_S || k == SDLK_D;
                if (e.key.repeat && !(moves && gamemenu_open() && !bindings_capturing())) break;
                /* the overlay's controls page waits for a key to bind: it takes the key (not F1..F12; Escape
                 * gives up waiting) */
                if (gamemenu_open() && bindings_capturing()) {
                    if (k == SDLK_ESCAPE) bindings_cancel();
                    else if (e.key.scancode < SDL_SCANCODE_F1 || e.key.scancode > SDL_SCANCODE_F12)
                        bindings_capture_key(e.key.scancode);
                    break;
                }
                if (gamemenu_open() && (e.key.scancode < SDL_SCANCODE_F1 || e.key.scancode > SDL_SCANCODE_F12))
                    used_pad(0);
                if (k == SDLK_ESCAPE && !gamemenu_open()) {
                    gamemenu_toggle(), yw4_hid_note_key("MENU");
                } else if (gamemenu_open()) {
                    /* Escape and Backspace go back one step: a page to the side panel, the popup away, the side
                     * panel to the game. W A S D as the arrows. */
                    static const struct {
                        SDL_Keycode key;
                        int action;
                        const char *name;
                    } mk[] = {{SDLK_UP, GM_UP, "M_UP"},          {SDLK_DOWN, GM_DOWN, "M_DOWN"},
                              {SDLK_LEFT, GM_LEFT, "M_LEFT"},    {SDLK_RIGHT, GM_RIGHT, "M_RIGHT"},
                              {SDLK_W, GM_UP, "M_UP"},           {SDLK_S, GM_DOWN, "M_DOWN"},
                              {SDLK_A, GM_LEFT, "M_LEFT"},       {SDLK_D, GM_RIGHT, "M_RIGHT"},
                              {SDLK_RETURN, GM_ACCEPT, "M_OK"},  {SDLK_KP_ENTER, GM_ACCEPT, "M_OK"},
                              {SDLK_SPACE, GM_ACCEPT, "M_OK"},   {SDLK_BACKSPACE, GM_BACK, "M_BACK"},
                              {SDLK_ESCAPE, GM_BACK, "M_BACK"},  {SDLK_DELETE, GM_CLEAR, "M_CLEAR"},
                              {SDLK_Q, GM_PREV, "M_PREV"},       {SDLK_E, GM_NEXT, "M_NEXT"},
                              {SDLK_P, GM_ALT, "M_ALT"},         {SDLK_H, GM_ALT, "M_ALT"}};  /* Preview; its bar */
                    for (size_t i = 0; i < sizeof mk / sizeof *mk; i++)
                        if (k == mk[i].key) gamemenu_action(mk[i].action), yw4_hid_note_key(mk[i].name);
                }
                break;  /* not the settings panel's keys below: the overlay's Enter mustn't reach a page it opens */
            }
            if (settings_open() && !e.key.repeat &&
                !((e.key.scancode < SDL_SCANCODE_F1 || e.key.scancode > SDL_SCANCODE_F12) &&
                  bindings_capture_key(e.key.scancode))) {
                static const struct {
                    SDL_Keycode key;
                    int action;
                    const char *name;
                } map[] = {{SDLK_UP, SETTINGS_UP, "UP"},           {SDLK_DOWN, SETTINGS_DOWN, "DOWN"},
                           {SDLK_LEFT, SETTINGS_LEFT, "LEFT"},     {SDLK_RIGHT, SETTINGS_RIGHT, "RIGHT"},
                           {SDLK_ESCAPE, SETTINGS_CLOSE, "F1"},    {SDLK_RETURN, SETTINGS_ACCEPT, "ENTER"},
                           {SDLK_KP_ENTER, SETTINGS_ACCEPT, "ENTER"}, {SDLK_DELETE, SETTINGS_CLEAR, "DELETE"},
                           {SDLK_BACKSPACE, SETTINGS_CLEAR, "DELETE"},
                           /* WASD: W/S choose, D enters (a page, a binding), A leaves the page (back to the
                            * overlay when it opened it); values change with the arrows */
                           {SDLK_W, SETTINGS_UP, "UP"},            {SDLK_S, SETTINGS_DOWN, "DOWN"},
                           {SDLK_D, SETTINGS_ACCEPT, "ENTER"},     {SDLK_A, SETTINGS_CLOSE, "F1"}};
                for (size_t i = 0; i < sizeof map / sizeof *map; i++)
                    if (e.key.key == map[i].key) settings_action(map[i].action), yw4_hid_note_key(map[i].name);
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            host_lock(&present_lock);
            swapchain_stale = 1;
            host_unlock(&present_lock);
            break;
        }
    }
}
