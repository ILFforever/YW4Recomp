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
#include "settings.h"
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
static VkSemaphore rendered[8];  /* one per swapchain image: reused only after that image is presented */
static uint32_t nswap;
static VkFormat swap_format;
static int swapchain_stale = 1;  /* (re)create before the next present: first use, resize */
static int swap_free;            /* the swapchain doesn't wait for the monitor (MAILBOX / IMMEDIATE wanted) */
static int swap_dlssg;           /* made while DLSS frame generation was on (NVIDIA: make it again on a switch) */

static int free_running(void) {
    int hz = framerate_hz();
    return !hz || hz > 60 || gpu_fg_active() || dlssg_on();  /* frame generation paces its frames itself; vsync (-1): FIFO */
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
enum { HUD_W = 256, HUD_H = 128 };
static Overlay hud = {HUD_W, HUD_H}, panel = {480, 480};
static int hud_on = -1;

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

static int create_swapchain(void) {
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
    swap_dlssg = dlssg_on();
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
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
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
    for (uint32_t i = 0; i < nswap; i++) {
        if (rendered[i]) vkDestroySemaphore(vk.device, rendered[i], NULL);
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(vk.device, &si, NULL, &rendered[i]);
    }
    swap_extent = extent;
    swap_format = fmt.format;
    swapchain_stale = 0;
    fprintf(stderr, "yw4: window: swapchain %ux%u, %u images, %s%s\n", extent.width, extent.height, nswap,
            mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (waits for the refresh)" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                                                                                                        : "IMMEDIATE",
            old ? " (made again)" : "");
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
    return create_swapchain();
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

static int hud_enabled(void) {
    int on = __atomic_load_n(&hud_on, __ATOMIC_ACQUIRE);
    if (on < 0) {
        const char *e = getenv("YW4_HUD");
        int want = e && *e && *e != '0';
        __atomic_compare_exchange_n(&hud_on, &on, want, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        on = __atomic_load_n(&hud_on, __ATOMIC_ACQUIRE);
    }
    return on > 0;
}

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
 * (x, y), each texel s x s window pixels. */
static void overlay_show(Overlay *o, VkImage dst, int w, int h, int32_t x, int32_t y, int32_t s) {
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
    int32_t xe = x + w * s, ye = y + h * s;  /* clipped to the window */
    if (xe > (int32_t)swap_extent.width) xe = (int32_t)swap_extent.width;
    if (ye > (int32_t)swap_extent.height) ye = (int32_t)swap_extent.height;
    if (xe <= x || ye <= y) return;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(xe - x) / s, (ye - y) / s, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x, y, 0}, {x + (xe - x) / s * s, y + (ye - y) / s * s, 1}}};
    vkCmdBlitImage(cmd, o->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
}

/* The HUD's whole-step scale for a picture h pixels high: 1 per 720 (the 5x7 font stays crisp) */
static int32_t overlay_scale(uint32_t h) { return h >= 1440 ? (int32_t)(h / 720) : 1; }

static void hud_draw(VkImage dst, int32_t x0, int32_t y0) {
    hud_shown_w = hud_shown_h = 0;
    if (!hud_enabled() || overlay_resources(&hud)) return;
    static HudContent content;
    int w, h;
    yw4_perf_hud(&content);
    hud_render(hud.ptr, HUD_W, HUD_H, &content, &w, &h);
    hud_shown_w = w, hud_shown_h = h;
    int32_t s = overlay_scale(swap_extent.height);
    overlay_show(&hud, dst, w, h, x0 + 6 * s, y0 + 6 * s, s);
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
    overlay_show(&panel, dst, w, h, x0 + ((int32_t)bw - w * s) / 2, y0 + ((int32_t)bh - h * s) / 2, s);
}

/* An overlay's drawn cw x ch part pasted 1:1 into rgba (the w x h frame) at (x, y), clipped. */
static void paste(uint8_t *rgba, uint32_t w, uint32_t h, const Overlay *o, int cw, int ch, uint32_t x, uint32_t y,
                  uint32_t scale) {
    if (!cw || !o->ptr || x >= w || y >= h) return;
    uint32_t pw = (uint32_t)cw * scale < w - x ? (uint32_t)cw * scale : w - x;
    uint32_t ph = (uint32_t)ch * scale < h - y ? (uint32_t)ch * scale : h - y;
    for (uint32_t r = 0; r < ph; r++) {
        const uint8_t *src = (const uint8_t *)o->ptr + (size_t)(r / scale) * o->w * 4;
        uint8_t *dst = rgba + ((size_t)(y + r) * w + x) * 4;
        for (uint32_t c = 0; c < pw; c++) memcpy(dst + c * 4, src + (c / scale) * 4, 4);
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
    uint32_t hs = (uint32_t)overlay_scale(h);
    paste(rgba, w, h, &hud, hud_shown_w, hud_shown_h, 6 * hs, 6 * hs, hs);
    uint32_t s = panel_scale(w, h);
    if (panel_shown_w && (uint32_t)panel_shown_w * s <= w && (uint32_t)panel_shown_h * s <= h)
        paste(rgba, w, h, &panel, panel_shown_w, panel_shown_h, (w - (uint32_t)panel_shown_w * s) / 2,
              (h - (uint32_t)panel_shown_h * s) / 2, s);
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

/* Scales src (w x h colour texels) into the next swapchain image and presents it.
 * layout UNDEFINED: src is `frame`, filled from the staging buffer (the CPU path).
 * Otherwise src is the draw path's image in that layout (GENERAL, decision 0021); with
 * `readback` it is also copied into the staging buffer and this waits for the GPU, so the
 * caller can read the texels. present_lock held, frame_resources done. 0 when presented. */
static int present_image(VkImage src, VkImageLayout layout, VkRect2D r0, int readback) {
    uint32_t w = r0.extent.width, h = r0.extent.height;
    int32_t sx = r0.offset.x, sy = r0.offset.y;
    vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
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
        if (readback) {
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {sx, sy, 0},
                                      .imageExtent = {w, h, 1}};
            vkCmdCopyImageToBuffer(cmd, src, layout, staging, 1, &copy);
        }
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
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{sx, sy, 0}, {sx + (int32_t)w, sy + (int32_t)h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x0, y0, 0}, {x0 + (int32_t)bw, y0 + (int32_t)bh, 1}}};
    vkCmdBlitImage(cmd, src, layout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    /* DLSS frame generation works on this window image: its HUD-less copy the same, the picture before the UI
     * (gpu_fg.c) scaled into the same place, so DLSS-G keeps the menus, our HUD and the panel still */
    VkImage hl = layout == VK_IMAGE_LAYOUT_GENERAL && dlssg_on() ? gpu_fg_hudless() : VK_NULL_HANDLE;
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
    hud_draw(dst, x0, y0);
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
    dlssg_present_begin(x0, y0, bw, bh, swap_format, nswap);  /* DLSS frame generation: the game's rectangle */
    r = vkQueuePresentKHR(vk.queue, &pi);
    dlssg_present_end();
    host_unlock(&vk.queue_lock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_stale = 1;
    if (readback) vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
    return 0;
}

void window_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
    if (screenshot_wanted(n)) save_screenshot(rgba, w, h, n);
    if (!have_vulkan || !w || !h) return;
    host_lock(&present_lock);
    if (swap_free != free_running() || swap_dlssg != dlssg_on()) swapchain_stale = 1;  /* frame rate across 60 */
    if (!(swapchain_stale && create_swapchain()) && !frame_resources(w, h)) {
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);  /* the staging buffer is free */
        memcpy(staging_ptr, rgba, (size_t)w * h * 4);
        present_image(frame, VK_IMAGE_LAYOUT_UNDEFINED, (VkRect2D){{0, 0}, {w, h}}, 0);
    }
    host_unlock(&present_lock);
}

void window_present_image(VkImage image, VkFormat format, VkRect2D rect, uint64_t n) {
    uint32_t w = rect.extent.width, h = rect.extent.height;
    if (!have_vulkan || !w || !h) return;
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
    if (swap_free != free_running() || swap_dlssg != dlssg_on()) swapchain_stale = 1;
    if (!(swapchain_stale && create_swapchain()) && !frame_resources(w, h) &&
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

void window_input(uint64_t *buttons, int32_t sticks[4]) {
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
    int is_open = settings_open();
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
        if (!b && !s[0] && !s[1] && !s[2] && !s[3] && !settings_open()) withheld = 0;
        b = 0, memset(s, 0, sizeof s);
    }
    host_lock(&input_lock);
    input_buttons = b;
    memcpy(input_sticks, s, sizeof s);
    host_unlock(&input_lock);
}

/* The settings panel from the gamepad: the mic button (Share on Xbox pads) as F1, the settings; Minus and Plus
 * together (Create + Options) as F2, the controls (not Home / PS: Steam takes that one and opens Big Picture); the D-pad or the left stick choose (up/down) and change (left/right); B closes;
 * on the controls page A changes a binding and X clears it (A, B, X as named on the controller); the shoulder
 * buttons and the triggers switch pages. The touchpad shows or hides the performance HUD (F3). Named like the
 * keys, so recordings and replays see the same presses. While the controls wait for an input, the press goes
 * there instead. */
void yw4_hid_note_key(const char *name);  /* svc_hid.c: key presses go into the input recording */

static void panel_pad_button(SDL_GamepadButton button) {
    int both = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) &&
               SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START);
    /* buttons a Switch controller doesn't have, so the game never sees them: the mic button, the DualSense's
     * touchpad */
    if (button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
        __atomic_store_n(&hud_on, hud_enabled() ? 0 : 1, __ATOMIC_RELEASE);
        return;
    }
    if (button == SDL_GAMEPAD_BUTTON_MISC1) {  /* as F1: the settings */
        settings_show(0), yw4_hid_note_key("F1");
        return;
    }
    if (both && (button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_START)) {  /* as F2: the controls */
        settings_show(1), yw4_hid_note_key("F2");
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
            if (!bindings_capturing()) settings_action(SETTINGS_PAGE), yw4_hid_note_key("PAGE");
        }
    }
}

/* The left stick as a D-pad in the panel: one step each time it is pushed past half way. */
static void panel_pad_stick(void) {
    static int held;  /* the direction pushed, until the stick comes back near the centre */
    int x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    if (ax < 8000 && ay < 8000) held = 0;
    if (held || !settings_open() || (ax < 16000 && ay < 16000)) return;
    if (bindings_capturing()) return;  /* bindings_capture_axis takes it */
    held = 1;
    panel_pad_button(ay > ax ? (y < 0 ? SDL_GAMEPAD_BUTTON_DPAD_UP : SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                             : (x < 0 ? SDL_GAMEPAD_BUTTON_DPAD_LEFT : SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
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
    if (!(headless && *headless && *headless != '0')) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "yw4: window: SDL_Init: %s; running headless\n", SDL_GetError());
        } else if (!(window = SDL_CreateWindow("Yo-kai Watch 4++ (recompiled)", 1280, 720,
                                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE))) {
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
        if (!got) continue;
        switch (e.type) {
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
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gbutton.which) panel_pad_button(e.gbutton.button);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (gamepad && SDL_GetGamepadID(gamepad) == e.gaxis.which && settings_open() &&
                !bindings_capture_axis(e.gaxis.axis, e.gaxis.value))
                panel_pad_stick(), panel_pad_triggers();
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (window && SDL_GetWindowRelativeMouseMode(window)) {
                host_lock(&mouse_lock);
                mouse_accum_x += e.motion.xrel;
                mouse_accum_y += e.motion.yrel;
                host_unlock(&mouse_lock);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (settings_open()) {
                if (bindings_capturing()) bindings_capture_mouse(e.button.button);
            } else if (window && !SDL_GetWindowRelativeMouseMode(window) && !mouse_off()) {
                SDL_SetWindowRelativeMouseMode(window, true);
                mouse_held_off = 1;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (settings_open()) {
                if (bindings_capturing()) bindings_capture_wheel(e.wheel.y > 0 ? 1 : -1);
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
            if (e.key.key == SDLK_F12) __atomic_store_n(&screenshot_requested, screenshot_burst(), __ATOMIC_RELEASE), yw4_hid_note_key("F12");
            if (e.key.key == SDLK_F11) gpu_request_trace(450), yw4_hid_note_key("F11");  /* the draws of the next frame, in the log */
            if (e.key.key == SDLK_F10) gpu_request_dump(), yw4_hid_note_key("F10");      /* shadow maps etc.: work/dump/ */
            if (e.key.key == SDLK_F3) __atomic_store_n(&hud_on, hud_enabled() ? 0 : 1, __ATOMIC_RELEASE);  /* the HUD */
            if (e.key.key == SDLK_F5) gpu_passes_toggle();  /* per-pass GPU timing in the log */
            /* the settings live in the panel only (F1); its keys are recorded like the F keys, so a replay
             * of a recording changes the same settings at the same ticks. While the controls page waits for
             * a key, any key but F1..F12 is the new binding. */
            if (e.key.key == SDLK_F1) {
                if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                settings_show(0), yw4_hid_note_key("F1");  /* the settings */
            }
            if (e.key.key == SDLK_F2) {
                if (window && SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, false);
                settings_show(1), yw4_hid_note_key("F2");  /* the controls */
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
                           {SDLK_BACKSPACE, SETTINGS_CLEAR, "DELETE"}};
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
