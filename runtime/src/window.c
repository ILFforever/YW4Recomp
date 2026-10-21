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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "host.h"
#include "window.h"

#define GUEST_HOST_STACK (64u << 20)  /* the guest main thread's host stack: reserved, not committed */

/* ---- Vulkan functions, loaded at run time */

#define GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties)
#define INSTANCE_FNS(X)                                                                                         \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)          \
    X(vkGetDeviceProcAddr)
#define DEVICE_FNS(X)                                                                                         \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)          \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)         \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences)       \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer)         \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                  \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBlitImage) X(vkCmdClearColorImage)              \
    X(vkDeviceWaitIdle)

#define DECLARE(f) static PFN_##f f;
GLOBAL_FNS(DECLARE)
INSTANCE_FNS(DECLARE)
DEVICE_FNS(DECLARE)

/* ---- state */

static SDL_Window *window;
static int have_vulkan;
static HostLock present_lock = HOST_LOCK_INIT;

static VkInstance instance;
static VkSurfaceKHR surface;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
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

static int screenshot_requested;  /* F12 */
static char screenshot_dir[512] = "screenshots";

static int vk_fail(const char *what, VkResult r) {
    fprintf(stderr, "yw4: window: %s failed (VkResult %d)\n", what, (int)r);
    return -1;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(gpu, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if (bits & (1u << i) && (p.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int create_swapchain(void) {
    vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    if (!extent.width || !extent.height) return -1;  /* minimised: try again later */
    uint32_t nfmt = 0;
    VkSurfaceFormatKHR fmts[64];
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &nfmt, NULL);
    if (nfmt > 64) nfmt = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &nfmt, fmts);
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
    VkResult r = vkCreateSwapchainKHR(device, &ci, NULL, &swapchain);
    if (old) vkDestroySwapchainKHR(device, old, NULL);
    if (r != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return vk_fail("vkCreateSwapchainKHR", r);
    }
    nswap = 8;
    vkGetSwapchainImagesKHR(device, swapchain, &nswap, swap_images);
    for (uint32_t i = 0; i < nswap; i++) {
        if (rendered[i]) vkDestroySemaphore(device, rendered[i], NULL);
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &si, NULL, &rendered[i]);
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
    GLOBAL_FNS(LOAD_GLOBAL)
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
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = next, .ppEnabledExtensionNames = exts,
                                .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS) return vk_fail("vkCreateInstance", r);
#define LOAD_INSTANCE(f) f = (PFN_##f)gipa(instance, #f);
    INSTANCE_FNS(LOAD_INSTANCE)
    if (!SDL_Vulkan_CreateSurface(window, instance, NULL, &surface)) {
        fprintf(stderr, "yw4: window: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return -1;
    }

    /* A GPU with a queue that draws and presents to this window; a discrete one if there is one. */
    uint32_t ngpu = 16;
    VkPhysicalDevice gpus[16];
    vkEnumeratePhysicalDevices(instance, &ngpu, gpus);
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
            if (score > best) best = score, gpu = gpus[i], queue_family = q;
            break;
        }
    }
    if (best < 0) {
        fprintf(stderr, "yw4: window: no Vulkan GPU can draw to this window\n");
        return -1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    fprintf(stderr, "yw4: window: Vulkan on %s\n", props.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = queue_family,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts};
    r = vkCreateDevice(gpu, &dci, NULL, &device);
    if (r != VK_SUCCESS) return vk_fail("vkCreateDevice", r);
#define LOAD_DEVICE(f) f = (PFN_##f)vkGetDeviceProcAddr(device, #f);
    DEVICE_FNS(LOAD_DEVICE)
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = queue_family};
    vkCreateCommandPool(device, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    vkAllocateCommandBuffers(device, &cai, &cmd);
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(device, &fci, NULL, &fence);
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device, &sci, NULL, &acquired);
    return create_swapchain();
}

/* The staging buffer and the image a w x h frame goes through. */
static int frame_resources(uint32_t w, uint32_t h) {
    if (frame && w == frame_w && h == frame_h) return 0;
    vkDeviceWaitIdle(device);
    if (frame) vkDestroyImage(device, frame, NULL), vkFreeMemory(device, frame_mem, NULL);
    if (staging) vkDestroyBuffer(device, staging, NULL), vkFreeMemory(device, staging_mem, NULL);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    vkCreateBuffer(device, &bci, NULL, &staging);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, staging, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(device, &mai, NULL, &staging_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(device, staging, staging_mem, 0);
    vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_ptr);
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    vkCreateImage(device, &ici, NULL, &frame);
    vkGetImageMemoryRequirements(device, frame, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &mai, NULL, &frame_mem) != VK_SUCCESS) return -1;
    vkBindImageMemory(device, frame, frame_mem, 0);
    frame_w = w, frame_h = h;
    return 0;
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void save_screenshot(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
    char path[600];
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

void window_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t n) {
    if (screenshot_wanted(n)) save_screenshot(rgba, w, h, n);
    if (!have_vulkan || !w || !h) return;
    host_lock(&present_lock);
    if ((swapchain_stale && create_swapchain()) || frame_resources(w, h)) goto out;
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    uint32_t index;
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale = 1;
        goto out;
    }
    vkResetFences(device, 1, &fence);
    memcpy(staging_ptr, rgba, (size_t)w * h * 4);

    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    barrier(frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, staging, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
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
                        .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x0, y0, 0}, {x0 + (int32_t)bw, y0 + (int32_t)bh, 1}}};
    vkCmdBlitImage(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
                       .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmd,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered[index]};
    vkQueueSubmit(queue, 1, &si, fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &rendered[index], .swapchainCount = 1, .pSwapchains = &swapchain,
                           .pImageIndices = &index};
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_stale = 1;
out:
    host_unlock(&present_lock);
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

void window_run(void (*guest)(void *), void *arg) {
    const char *dir = getenv("YW4_SCREENSHOT_DIR");
    if (dir && *dir) snprintf(screenshot_dir, sizeof screenshot_dir, "%s", dir);
    const char *headless = getenv("YW4_HEADLESS");
    if (!(headless && *headless && *headless != '0')) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "yw4: window: SDL_Init: %s; running headless\n", SDL_GetError());
        } else if (!(window = SDL_CreateWindow("Yo-kai Watch 4++ (recompiled)", 1280, 720,
                                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE))) {
            fprintf(stderr, "yw4: window: SDL_CreateWindow: %s; running headless\n", SDL_GetError());
        } else if (vk_init()) {
            fprintf(stderr, "yw4: window: no Vulkan; the window stays empty\n");
        } else {
            have_vulkan = 1;
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
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        switch (e.type) {
        case SDL_EVENT_QUIT:
            fprintf(stderr, "yw4: window closed\n");
            exit(0);
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_F12) __atomic_store_n(&screenshot_requested, 1, __ATOMIC_RELEASE);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            host_lock(&present_lock);
            swapchain_stale = 1;
            host_unlock(&present_lock);
            break;
        }
    }
}
