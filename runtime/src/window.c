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
static int have_vulkan;
static HostLock present_lock = HOST_LOCK_INIT;

static VkSurfaceKHR surface;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkSemaphore acquired;

static VkSwapchainKHR swapchain;
static VkExtent2D swap_extent;
static VkImage swap_images[8];
static VkImageView swap_views[8];  /* to draw into (the HUD, blended) */
static VkSemaphore rendered[8];  /* one per swapchain image: reused only after that image is presented */
static uint32_t nswap;
static VkFormat swap_format;
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
static Overlay panel = {480, 480}, notice = {1024, 160};
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
    VkSurfaceFormatKHR fmt = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++)  /* UNORM: the game's bytes are shown as they are, as on the Switch */
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) fmt = fmts[i];
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
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,  /* copies; the HUD drawn */
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

static int vk_init(void) {
    /* DLSS frame generation (gpu_dlssg.cpp): Vulkan through Streamline, which adds what it needs */
    int sl = dlssg_start();
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
    const char *dexts[16] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
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
    int bda_ext = 0;
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n, sizeof *e);
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, e);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) vk.host_import = 1;
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) vk.memory_budget = 1;
        for (unsigned k = 0; k < ndlss && ndexts < 14; k++)
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
    if (vk.memory_budget && ndexts < 16) dexts[ndexts++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;  /* gpu_vram */
    else vk.memory_budget = 0;

    /* Features: what the translated shaders (0020) and NVN's state use, where the GPU has it. */
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
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
                     .multiDrawIndirect = h->multiDrawIndirect, .textureCompressionBC = h->textureCompressionBC}};
    if (!f13.dynamicRendering || !f12.timelineSemaphore || !f11.shaderDrawParameters) {
        fprintf(stderr, "yw4: window: the GPU lacks dynamic rendering, timeline semaphores or draw parameters\n");
        return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk.queue_family,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &enable, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = ndexts, .ppEnabledExtensionNames = dexts};
    r = vkCreateDevice(vk.gpu, &dci, NULL, &vk.device);
    if (r != VK_SUCCESS) return vk_fail("vkCreateDevice", r);
#define LOAD_DEVICE(f) f = (PFN_##f)vkGetDeviceProcAddr(vk.device, #f);
    VK_DEVICE_FNS(LOAD_DEVICE)
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    if (sl) dlssg_available();  /* checked and logged once, Reflex set up */

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
    return create_swapchain(0);
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
    if (s && SDL_SaveBMP(s, path)) fprintf(stderr, "yw4: screenshot %s\n", path);
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
    float v = __atomic_load_n(&hud_scale, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("YW4_HUD_SCALE");
        v = e && *e && strcmp(e, "auto") ? (float)atof(e) : 0.0f;
        if (v < 0) v = 0;
        __atomic_store_n(&hud_scale, v, __ATOMIC_RELEASE);
    }
    return v;
}

void hud_scale_set(float v) { __atomic_store_n(&hud_scale, v, __ATOMIC_RELEASE); }

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
        SettingRow rows[32];
        int sel, n = settings_rows(rows, 32, &sel);
        panel_render(panel.ptr, (int)panel.w, (int)panel.h, rows, n, sel, settings_developer(), &w, &h);
    }
    panel_shown_w = w, panel_shown_h = h;
    /* scaled up by whole steps (the pixel font stays crisp) to about two thirds of the picture's height */
    int32_t s = (int32_t)panel_scale(bw, bh);
    overlay_show(&panel, dst, w, h, x0 + ((int32_t)bw - w * s) / 2, y0 + ((int32_t)bh - h * s) / 2, (float)s);
}

void window_hud_toggle(void) { __atomic_store_n(&hud_on, hud_enabled() ? 0 : hud_last, __ATOMIC_RELEASE); }
int window_hud_shown(void) { return hud_enabled(); }

static void menu_draw(VkImage dst, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh);  /* below, with the pause */

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

/* once per input update (window's thread, and replays): moves the pause on; returns the buttons pressed for the
 * player, OR'd into the input */
uint64_t window_pause_buttons(void) {
    if (window_paused()) {
        uint64_t now = SDL_GetTicks();
        int st = pause_state();
        if (st == PAUSE_ON && pause_resume_pending) {
            pause_resume_pending = 0;
            game_pause_toggle();  /* the resume asked while it was pausing */
        } else if (st == PAUSE_OFF) {
            pause_live("the game resumed");
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

static void menu_draw(VkImage dst, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    if (!gamemenu_open() || settings_open()) return;
    int pv = gamemenu_previewing();  /* Preview: only its bar, over the live picture (no kept picture needed) */
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
    if (!ov.uploaded || look != ov.look) {  /* draw it again: on the CPU, then one copy into the upload buffer */
        if (!ov.px && !(ov.px = malloc((size_t)bw * bh * 4))) return;
        if (!pv) memcpy(ov.px, ov.bg, (size_t)bw * bh * 4);  /* the bar is opaque: drawn over anything */
        UiCanvas c = {ov.px, (int)bw, (int)bh, (float)bh / 900.0f, 0, 0};
        const char *close = "Esc";
        GameMenuInfo info = {hud_enabled(), yw4_version, pad_hints(&close), close, NULL, 0, 0, now};
        info.logo = game_logo(&info.logo_w, &info.logo_h);
        info.pad_name = pad_name();
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
        float r[4], s = (float)bh / 900.0f;
        gamemenu_preview_rect(r);
        rx = (int32_t)(r[0] * s), ry = (int32_t)(r[1] * s), rw = (int32_t)(r[2] * s + 0.5f), rh = (int32_t)(r[3] * s + 0.5f);
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        if (rx + rw > (int32_t)bw) rw = (int32_t)bw - rx;
        if (ry + rh > (int32_t)bh) rh = (int32_t)bh - ry;
        if (rw <= 0 || rh <= 0) return;
    }
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{rx, ry, 0}, {rx + rw, ry + rh, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x0 + rx, y0 + ry, 0}, {x0 + rx + rw, y0 + ry + rh, 1}}};
    vkCmdBlitImage(cmd, ov.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);  /* a blit: the window's format may order the channels differently */
}

/* A notice in the middle of the picture: "Switching frame generation" while the switch's safe frames last
 * (gpu_fg.h). F4's test pause shows none (owner, 2026-11-06: the picture stays clean). */
static void notice_draw(VkImage dst, int32_t x0, int32_t y0, uint32_t bw, uint32_t bh) {
    const char *msg = gpu_fg_switching() ? "SWITCHING FRAME GENERATION..." : NULL;
    if (!msg || overlay_resources(&notice)) return;
    float s = overlay_scale(swap_extent.height);
    int w, h;
    notice_render(notice.ptr, (int)notice.w, (int)notice.h, msg, s, &w, &h);
    overlay_show(&notice, dst, w, h, x0 + ((int32_t)bw - w) / 2, y0 + ((int32_t)bh - h) / 2, 1.0f);
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
    float hs = overlay_scale(h);
    int32_t hx, hy;
    hud_place(hud_shown_w, hud_shown_h, w, h, hs, &hx, &hy);
    if (hb.cpu && hud_shown_w)  /* the see-through card, blended as the window does: src + dst * (1 - src.a) */
        for (int j = 0; j < hud_shown_h && hy + j < (int32_t)h; j++)
            for (int i = 0; i < hud_shown_w && hx + i < (int32_t)w; i++) {
                const uint8_t *sp = hb.cpu + ((size_t)j * HUD_W + (size_t)i) * 4;
                uint8_t *dp = rgba + ((size_t)(hy + j) * w + (size_t)(hx + i)) * 4;
                for (int k = 0; k < 3; k++) dp[k] = (uint8_t)(sp[k] + dp[k] * (255 - sp[3]) / 255);
            }
    uint32_t s = panel_scale(w, h);
    if (panel_shown_w && (uint32_t)panel_shown_w * s <= w && (uint32_t)panel_shown_h * s <= h)
        paste(rgba, w, h, &panel, panel_shown_w, panel_shown_h, (w - (uint32_t)panel_shown_w * s) / 2,
              (h - (uint32_t)panel_shown_h * s) / 2, (float)s);
    /* YW4_SHOT_MENU=1 (tests): the in-game overlay too; normally left out, so its TAKE SCREENSHOT shows the game */
    static int shot_menu = -1;
    if (shot_menu < 0) shot_menu = getenv("YW4_SHOT_MENU") != NULL;
    if (!shot_menu || !gamemenu_open() || settings_open() || !ov.px || !ov.uploaded) return;
    /* the part shown: all, or in the preview only its bar (the rest of ov.px is an older drawing), as menu_draw */
    float r[4] = {0, 0, (float)ov.img.w * 900.0f / (float)ov.img.h, 900};
    if (gamemenu_previewing()) gamemenu_preview_rect(r);
    float k = (float)h / 900.0f;  /* design units to the frame's pixels */
    for (uint32_t y = 0; y < h; y++)  /* the overlay's picture (the window's size) scaled to the frame's */
        for (uint32_t x = 0; x < w; x++) {
            if ((float)x < r[0] * k || (float)x >= (r[0] + r[2]) * k || (float)y < r[1] * k || (float)y >= (r[1] + r[3]) * k)
                continue;
            memcpy(rgba + ((size_t)y * w + x) * 4,
                   ov.px + ((size_t)(y * ov.img.h / h) * ov.img.w + x * ov.img.w / w) * 4, 4);
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
    menu_draw(dst, x0, y0, bw, bh);
    panel_draw(dst, x0, y0, bw, bh);
    notice_draw(dst, x0, y0, bw, bh);
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
static int mouse_wheel_pulses[2];  /* [0] wheel up, [1] wheel down */
static int mouse_held_off;         /* the click that gave the game the mouse: no press until it is let go */

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

/* The keyboard, mouse, and the gamepad through the controls (bindings.c; the settings panel's second page). */
static void update_input(void) {
    uint64_t b;
    int32_t s[4];
    float mx, my;
    uint32_t mb = SDL_GetMouseState(&mx, &my);
    if (mouse_held_off && !mb) mouse_held_off = 0;
    /* clicks are buttons only while the game has the mouse */
    if (mouse_held_off || !window || !SDL_GetWindowRelativeMouseMode(window) || mouse_off()) mb = 0;
    int wup = 0, wdwn = 0;
    host_lock(&mouse_lock);
    if (mouse_off()) mouse_wheel_pulses[0] = mouse_wheel_pulses[1] = 0;
    if (mouse_wheel_pulses[0] > 0) { wup = 1; mouse_wheel_pulses[0]--; }
    if (mouse_wheel_pulses[1] > 0) { wdwn = 1; mouse_wheel_pulses[1]--; }
    host_unlock(&mouse_lock);

    bindings_read(SDL_GetKeyboardState(NULL), mb, wup, wdwn, gamepad, &b, s);
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
    if (withheld) {
        if (!b && !s[0] && !s[1] && !s[2] && !s[3] && !is_open) withheld = 0;
        b = 0, memset(s, 0, sizeof s);
    }
    /* the mouse in a supported menu (mousemenu.c): the cursor shows while it is open, and hovering and clicking
     * become the menu's own D-pad, A and B presses; the camera gets the mouse back when it closes */
    if (window && !is_open) {
        static int freed;  /* we turned the relative mouse off for a menu */
        int ww = 0, wh = 0, want = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        /* the mouse in the picture's own rectangle (the window minus any black bars) */
        float px = picture_rect[0] * (float)ww, py = picture_rect[1] * (float)wh;
        float pw = picture_rect[2] * (float)ww, ph = picture_rect[3] * (float)wh;
        uint64_t mm = mousemenu_update(mx - px, my - py, (int)(pw + 0.5f), (int)(ph + 0.5f), SDL_GetMouseState(NULL, NULL), &want);
        {
            static uint64_t last_mm;
            if (mm != last_mm && getenv("YW4_MOUSE_SCRIPT")) fprintf(stderr, "yw4: mouse menus: sends %#llx\n", (unsigned long long)mm);
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
 * Back + Start together on any pad (Xbox One and older have no Share); on a DualSense MISC1 is the mic button,
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
    *x = (mx - px) * 900.0f / ph, *y = (my - py) * 900.0f / ph;
    return 1;
}

static int last_used_pad;  /* the overlay's hints: the controller was used last (else the keyboard or mouse) */
static void used_pad(int pad) {
    if (pad != last_used_pad) last_used_pad = pad, gamemenu_refresh();
}
static int pad_hints(const char **close) {
    if (!gamepad || !last_used_pad) return 0;
    int ps = pad_is_playstation();
    if (ps) *close = "Touchpad";
    else if (SDL_GamepadHasButton(gamepad, SDL_GAMEPAD_BUTTON_MISC1))
        *close = SDL_GetGamepadType(gamepad) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ? "Capture" : "Share";
    else *close = "Back + Start";
    return ps ? 2 : 1;
}

static const char *pad_name(void) { return gamepad ? SDL_GetGamepadName(gamepad) : NULL; }

static void panel_pad_button(SDL_GamepadButton button) {
    /* the overlay's controls page waits for an input to bind: it takes the press */
    if (gamemenu_open() && !settings_open() && bindings_capturing() && bindings_capture_button(button)) return;
    int both = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) &&
               SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);
    /* buttons a Switch controller doesn't have (or two at once), so the game never sees them alone */
    int ps = pad_is_playstation();
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
    exit(0);  /* the game's main returned */
}

void window_request_screenshot(void) { __atomic_store_n(&screenshot_requested, screenshot_burst(), __ATOMIC_RELEASE); }

void window_run(void (*guest)(void *), void *arg) {
    const char *dir = getenv("YW4_SCREENSHOT_DIR");
    if (dir && *dir) snprintf(screenshot_dir, sizeof screenshot_dir, "%s", dir);
    const char *headless = getenv("YW4_HEADLESS");
    char title[96];  /* the window's title, with the version (decision 0047) */
    if (!(headless && *headless && *headless != '0')) {
        /* the game's icon, resource 1 in the exe when the build had one (decision 0048); without it Windows' default */
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "yw4: window: SDL_Init: %s; running headless\n", SDL_GetError());
        } else if (snprintf(title, sizeof title, "Yo-kai Watch 4++ (recompiled) · YW4 Recomp %s", yw4_version) < 0 ||
                   !(window = SDL_CreateWindow(title, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                               (window_fullscreen_index() ? SDL_WINDOW_FULLSCREEN : 0)))) {
            fprintf(stderr, "yw4: window: SDL_CreateWindow: %s; running headless\n", SDL_GetError());
        } else if (vk_init()) {
            fprintf(stderr, "yw4: window: no Vulkan; the window stays empty\n");
        } else {
            have_vulkan = 1;
            gpu_init();
            if (!mouse_off()) SDL_SetWindowRelativeMouseMode(window, true);
        }
    }
    if (!window) {
        guest(arg);
        return;
    }
    static Guest g;
    g.fn = guest, g.arg = arg;
    if (host_thread_start(guest_main, &g, GUEST_HOST_STACK)) {
        fprintf(stderr, "yw4: window: cannot start the guest's thread\n");
        exit(1);
    }
    for (;;) {
        SDL_Event e;
        int got = SDL_WaitEventTimeout(&e, 8);
        update_input();
        overlay_pad_repeat();
        fullscreen_apply();
        {  /* the system's text input (a keyboard layout's letters, an IME) runs while a text field has the focus */
            static int text_on;
            int want = window && gamemenu_open() && textin_active();
            if (want != text_on) {
                if (want) SDL_StartTextInput(window);
                else if (window) SDL_StopTextInput(window);
                text_on = want;
            }
        }
        if (!got) continue;
        switch (e.type) {
        case SDL_EVENT_TEXT_INPUT:
            if (gamemenu_open() && textin_active()) textin_type(e.text.text), yw4_hid_note_text(e.text.text);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad && (gamepad = SDL_OpenGamepad(e.gdevice.which)))
                fprintf(stderr, "yw4: window: gamepad %s\n", SDL_GetGamepadName(gamepad));
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
                fprintf(stderr, "yw4: window: gamepad removed\n");
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gbutton.which) used_pad(1), panel_pad_button(e.gbutton.button);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gaxis.which && (settings_open() || gamemenu_open()) &&
                !bindings_capture_axis(e.gaxis.axis, e.gaxis.value)) {
                if (e.gaxis.value > 16000 || e.gaxis.value < -16000) used_pad(1);
                panel_pad_stick(), panel_pad_triggers();
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (window && SDL_GetWindowRelativeMouseMode(window)) {
                host_lock(&mouse_lock);
                mouse_accum_x += e.motion.xrel;
                mouse_accum_y += e.motion.yrel;
                host_unlock(&mouse_lock);
            } else if (gamemenu_open() && !settings_open()) {  /* the overlay: hovering chooses */
                float x, y;
                if (overlay_point(e.motion.x, e.motion.y, &x, &y))
                    used_pad(0), gamemenu_mouse(x, y, e.motion.state & SDL_BUTTON_LMASK ? 4 : 0);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (settings_open() || gamemenu_open()) {  /* not the game's: the panel's or the overlay's */
                float x, y;
                if (bindings_capturing()) bindings_capture_mouse(e.button.button);
                else if (!settings_open() && (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT) &&
                         overlay_point(e.button.x, e.button.y, &x, &y))
                    used_pad(0), gamemenu_mouse(x, y, e.button.button == SDL_BUTTON_LEFT ? 1 : 3);
            } else if (window && !SDL_GetWindowRelativeMouseMode(window) && !mouse_off()) {
                SDL_SetWindowRelativeMouseMode(window, true);
                mouse_held_off = 1;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (settings_open() || gamemenu_open()) {
                if (bindings_capturing()) bindings_capture_wheel(e.wheel.y > 0 ? 1 : -1);
                else if (!settings_open() && e.wheel.y != 0) used_pad(0), gamemenu_wheel(e.wheel.y > 0 ? 1 : -1);
            } else {
                host_lock(&mouse_lock);
                if (e.wheel.y > 0) mouse_wheel_pulses[0] = 3;
                else if (e.wheel.y < 0) mouse_wheel_pulses[1] = 3;
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
            exit(0);
        case SDL_EVENT_KEY_DOWN:
            if ((e.key.key == SDLK_LALT || e.key.key == SDLK_RALT) && !settings_open() && !mouse_off()) {
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
                if (e.key.mod & SDL_KMOD_SHIFT) gpu_request_trace(450), yw4_hid_note_key("F11");
                else window_fullscreen_cycle(), settings_save();
            }
            if (e.key.key == SDLK_F10) gpu_request_dump(), yw4_hid_note_key("F10");      /* shadow maps etc.: work/dump/ */
            if (e.key.key == SDLK_F3 && settings_legacy_keys()) window_hud_toggle(), settings_save();  /* the HUD */
            /* LEGACY MENUS off (the default): F1, F2 and F3 open the overlay on its Settings, Controls and Overlay
             * pages instead (owner, 2026-11-08); the same key again resumes. Not while the old panel is open: there
             * F1 and F2 still close it (below). Not while typing: F keys go nowhere then. */
            if (!settings_legacy_keys() && !settings_open() && !e.key.repeat && !textin_active() &&
                (e.key.key == SDLK_F1 || e.key.key == SDLK_F2 || e.key.key == SDLK_F3)) {
                if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                if (e.key.key == SDLK_F1) gamemenu_open_page(GM_SETTINGS), yw4_hid_note_key("M_SET");
                else if (e.key.key == SDLK_F2) gamemenu_open_page(GM_CONTROLS), yw4_hid_note_key("M_CTRL");
                else gamemenu_open_page(GM_PERF), yw4_hid_note_key("M_PERF");
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
                if (k == SDLK_V && (e.key.mod & SDL_KMOD_CTRL)) {  /* paste */
                    char *clip = SDL_GetClipboardText();
                    if (clip && *clip) textin_type(clip), yw4_hid_note_text(clip);
                    SDL_free(clip);
                    break;
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
