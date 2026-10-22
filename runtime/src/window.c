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
#include "gpu.h"
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
static int swapchain_stale = 1;  /* (re)create before the next present: first use, resize */

static VkBuffer staging;
static VkDeviceMemory staging_mem, frame_mem;
static void *staging_ptr;
static VkImage frame;
static uint32_t frame_w, frame_h;

void gpu_request_trace(int draws);  /* gpu_draw.c */
void gpu_request_dump(void);        /* gpu.c */
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
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,  /* always available; the game paces itself to 30 */
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(vk.device, &ci, NULL, &swapchain);
    if (old) vkDestroySwapchainKHR(vk.device, old, NULL);
    if (r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return vk_fail("vkCreateSwapchainKHR", r);
    }
    nswap = 8;
    vkGetSwapchainImagesKHR(vk.device, swapchain, &nswap, swap_images);
    for (uint32_t i = 0; i < nswap; i++) {
        if (rendered[i]) vkDestroySemaphore(vk.device, rendered[i], NULL);
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(vk.device, &si, NULL, &rendered[i]);
    }
    swap_extent = extent;
    swapchain_stale = 0;
    return 0;
}

static int vk_init(void) {
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
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
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &surface)) {
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
    const char *dexts[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t ndexts = 1;
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n, sizeof *e);
        vkEnumerateDeviceExtensionProperties(vk.gpu, NULL, &n, e);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) vk.host_import = 1;
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
    VkPhysicalDeviceVulkan13Features e13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = f13.dynamicRendering,
                                            .synchronization2 = f13.synchronization2};
    VkPhysicalDeviceVulkan12Features e12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13,
                                            .timelineSemaphore = f12.timelineSemaphore};
    VkPhysicalDeviceVulkan11Features e11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &e12,
                                            .shaderDrawParameters = f11.shaderDrawParameters};
    VkPhysicalDeviceFeatures2 enable = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e11,
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
    snprintf(path, sizeof path, "%s/frame_%06" PRIu64 ".bmp", screenshot_dir, n);
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (s && SDL_SaveBMP(s, path)) fprintf(stderr, "yw4: screenshot %s\n", path);
    else fprintf(stderr, "yw4: screenshot %s failed: %s\n", path, SDL_GetError());
    if (s) SDL_DestroySurface(s);
}

static int screenshot_wanted(uint64_t n) {
    if (__atomic_exchange_n(&screenshot_requested, 0, __ATOMIC_ACQ_REL)) return 1;
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
    r = vkQueuePresentKHR(vk.queue, &pi);
    host_unlock(&vk.queue_lock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_stale = 1;
    if (readback) vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
    return 0;
}

void window_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
    if (screenshot_wanted(n)) save_screenshot(rgba, w, h, n);
    if (!have_vulkan || !w || !h) return;
    host_lock(&present_lock);
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
    int bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    if (shot && !bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        fprintf(stderr, "yw4: screenshot of Vulkan format %d not supported yet\n", (int)format);
        shot = 0;
    }
    host_lock(&present_lock);
    if (!(swapchain_stale && create_swapchain()) && !frame_resources(w, h) &&
        !present_image(image, VK_IMAGE_LAYOUT_GENERAL, rect, shot) && shot) {
        uint8_t *px = staging_ptr;
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
        save_screenshot(px, w, h, n);
    }
    host_unlock(&present_lock);
}

/* ---- input: the keyboard and the first gamepad, as a Switch controller (window_input) */

static SDL_Gamepad *gamepad;
static uint64_t input_buttons;
static int32_t input_sticks[4];
static HostLock input_lock = HOST_LOCK_INIT;

void window_input(uint64_t *buttons, int32_t sticks[4]) {
    host_lock(&input_lock);
    *buttons = input_buttons;
    memcpy(sticks, input_sticks, sizeof input_sticks);
    host_unlock(&input_lock);
}

/* Keyboard: Space/Enter A, Backspace/Escape B, X X, Y Y, arrows D-pad, WASD left stick,
 * IJKL right stick, Q/E L/R, 1/3 ZL/ZR, P Plus, M Minus, left/right Shift stick clicks.
 * Gamepad: by position, as on a Switch controller (east A, south B, north X, west Y). */
static void update_input(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    static const struct {
        SDL_Scancode key;
        int bit;
    } keys[] = {{SDL_SCANCODE_SPACE, 0}, {SDL_SCANCODE_RETURN, 0}, {SDL_SCANCODE_BACKSPACE, 1}, {SDL_SCANCODE_ESCAPE, 1},
                {SDL_SCANCODE_X, 2}, {SDL_SCANCODE_Y, 3}, {SDL_SCANCODE_LSHIFT, 4}, {SDL_SCANCODE_RSHIFT, 5},
                {SDL_SCANCODE_Q, 6}, {SDL_SCANCODE_E, 7}, {SDL_SCANCODE_1, 8}, {SDL_SCANCODE_3, 9},
                {SDL_SCANCODE_P, 10}, {SDL_SCANCODE_M, 11}, {SDL_SCANCODE_LEFT, 12}, {SDL_SCANCODE_UP, 13},
                {SDL_SCANCODE_RIGHT, 14}, {SDL_SCANCODE_DOWN, 15}};
    uint64_t b = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (k[keys[i].key]) b |= 1ull << keys[i].bit;
    int32_t s[4] = {(k[SDL_SCANCODE_D] - k[SDL_SCANCODE_A]) * 32767, (k[SDL_SCANCODE_W] - k[SDL_SCANCODE_S]) * 32767,
                    (k[SDL_SCANCODE_L] - k[SDL_SCANCODE_J]) * 32767, (k[SDL_SCANCODE_I] - k[SDL_SCANCODE_K]) * 32767};
    if (gamepad) {
        static const struct {
            SDL_GamepadButton button;
            int bit;
        } pad[] = {{SDL_GAMEPAD_BUTTON_EAST, 0}, {SDL_GAMEPAD_BUTTON_SOUTH, 1}, {SDL_GAMEPAD_BUTTON_NORTH, 2},
                   {SDL_GAMEPAD_BUTTON_WEST, 3}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4},
                   {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6},
                   {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7}, {SDL_GAMEPAD_BUTTON_START, 10}, {SDL_GAMEPAD_BUTTON_BACK, 11},
                   {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 13},
                   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15}};
        for (size_t i = 0; i < sizeof pad / sizeof *pad; i++)
            if (SDL_GetGamepadButton(gamepad, pad[i].button)) b |= 1ull << pad[i].bit;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= 1ull << 8;
        if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= 1ull << 9;
        static const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                                SDL_GAMEPAD_AXIS_RIGHTY};
        for (int i = 0; i < 4; i++) {
            int32_t v = SDL_GetGamepadAxis(gamepad, axes[i]);
            if (i & 1) v = -v;  /* SDL: down positive; Switch: up positive */
            if (v > 32767) v = 32767;
            if (v < -32767) v = -32767;
            if (v > 4000 || v < -4000) s[i] = v;  /* dead zone: the keyboard wins near the centre */
        }
    }
    host_lock(&input_lock);
    input_buttons = b;
    memcpy(input_sticks, s, sizeof s);
    host_unlock(&input_lock);
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

void window_request_screenshot(void) { __atomic_store_n(&screenshot_requested, 1, __ATOMIC_RELEASE); }

void yw4_hid_note_key(const char *name);  /* svc_hid.c: key presses go into the input recording */

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
        case SDL_EVENT_QUIT:
            fprintf(stderr, "yw4: window closed\n");
            exit(0);
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_F12) __atomic_store_n(&screenshot_requested, 1, __ATOMIC_RELEASE), yw4_hid_note_key("F12");
            if (e.key.key == SDLK_F11) gpu_request_trace(450), yw4_hid_note_key("F11");  /* the draws of the next frame, in the log */
            if (e.key.key == SDLK_F10) gpu_request_dump(), yw4_hid_note_key("F10");      /* shadow maps etc.: work/dump/ */
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            host_lock(&present_lock);
            swapchain_stale = 1;
            host_unlock(&present_lock);
            break;
        }
    }
}
