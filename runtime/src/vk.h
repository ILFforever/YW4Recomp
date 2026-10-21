/* Vulkan, loaded at run time through SDL (decision 0019), shared by the window (window.c)
 * and the draw path (gpu.c, decision 0021).
 *
 * Functions are global pointers named like the Vulkan functions (VK_NO_PROTOTYPES, so they
 * don't clash); window.c defines and loads them. One queue for everything; vkQueueSubmit
 * and vkQueuePresentKHR are called with vk.queue_lock held.
 */
#ifndef YW4_VK_H
#define YW4_VK_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "host.h"

#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties)
#define VK_INSTANCE_FNS(X)                                                                                      \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)          \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures2)            \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFormatProperties)
#define VK_DEVICE_FNS(X)                                                                                      \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)          \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)         \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences)       \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer)         \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                  \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage)            \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkDeviceWaitIdle) X(vkResetCommandBuffer)       \
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkGetMemoryHostPointerPropertiesEXT)                 \
    X(vkCreateImageView) X(vkDestroyImageView)

#define VK_DECLARE_EXTERN(f) extern PFN_##f f;
VK_GLOBAL_FNS(VK_DECLARE_EXTERN)
VK_INSTANCE_FNS(VK_DECLARE_EXTERN)
VK_DEVICE_FNS(VK_DECLARE_EXTERN)

typedef struct {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    HostLock queue_lock;
    VkPhysicalDeviceProperties props;
    int host_import;               /* VK_EXT_external_memory_host is enabled */
    VkDeviceSize host_import_align;
} VkContext;
extern VkContext vk;

/* A memory type index in `bits` with the properties `want`; UINT32_MAX if none. */
uint32_t vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);

#endif
