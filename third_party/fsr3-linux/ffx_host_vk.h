// Routes the direct Vulkan calls in the FidelityFX Vulkan backend through functions the host
// supplies, and limits the capabilities the backend may use to what the host enabled.
//
// The backend (ffx_vk.cpp) resolves most functions through the vkGetDeviceProcAddr it is given,
// but calls eight functions directly, and it decides on fp16, wave64 and AMD device-coherent
// memory from the extensions the GPU *supports*, not the ones the application *enabled*. Inside
// another application's device that is invalid. Executables and libraries that link
// ffx_fsr3upscaler_vk get these eight symbols rewritten to __wrap_* by the linker
// (FFX_HOST_VK_WRAP in CMakeLists.txt) and call ffxHostVkSet() before creating the backend.
#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

struct FfxHostVkFunctions
{
    PFN_vkGetPhysicalDeviceProperties        GetPhysicalDeviceProperties        = nullptr;
    PFN_vkGetPhysicalDeviceProperties2       GetPhysicalDeviceProperties2       = nullptr;
    PFN_vkGetPhysicalDeviceFeatures          GetPhysicalDeviceFeatures          = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2         GetPhysicalDeviceFeatures2         = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties  GetPhysicalDeviceMemoryProperties  = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetDeviceProcAddr                  GetDeviceProcAddr                  = nullptr;
    PFN_vkCreateBuffer                       CreateBuffer                       = nullptr;
};

// What the application enabled on its device. The backend sees only these extensions, and the
// feature bits below are cleared in any feature query unless set here.
struct FfxHostVkEnabled
{
    std::vector<std::string> deviceExtensions;
    bool shaderFloat16                       = false;
    bool shaderInt16                         = false;  // the 16-bit shader variants declare Int16 too
    bool synchronization2                    = false;
    bool deviceCoherentMemory                = false;
    bool shaderStorageBufferArrayNonUniformIndexing = false;
};

void ffxHostVkSet(const FfxHostVkFunctions& functions, const FfxHostVkEnabled& enabled);
