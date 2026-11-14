// YW4 Recomp's entry into the FidelityFX host shim (ffx_host_vk.h, from xplane-fsr3): the game names its Vulkan
// loader, instance and what it enabled on its device before it creates an FSR context. Exported from
// libamd_fidelityfx_vk.so next to the five ffx-api functions.
#include "ffx_host_vk.h"

#include <cstring>

extern "C" __attribute__((visibility("default"))) void yw4FfxHostSet(
    PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char* const* extensions, uint32_t extensionCount,
    uint32_t shaderFloat16, uint32_t shaderInt16, uint32_t synchronization2)
{
    FfxHostVkFunctions f;
#define GET(name) f.name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
    GET(GetPhysicalDeviceProperties);
    GET(GetPhysicalDeviceProperties2);
    GET(GetPhysicalDeviceFeatures);
    GET(GetPhysicalDeviceFeatures2);
    GET(GetPhysicalDeviceMemoryProperties);
    GET(EnumerateDeviceExtensionProperties);
    GET(GetDeviceProcAddr);
    GET(CreateBuffer);
#undef GET
    FfxHostVkEnabled e;
    for (uint32_t i = 0; i < extensionCount; i++)
        e.deviceExtensions.emplace_back(extensions[i]);
    e.shaderFloat16 = shaderFloat16 != 0;
    e.shaderInt16 = shaderInt16 != 0;
    e.synchronization2 = synchronization2 != 0;
    ffxHostVkSet(f, e);
}
