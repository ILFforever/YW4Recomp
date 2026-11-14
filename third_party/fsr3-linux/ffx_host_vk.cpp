#include "ffx_host_vk.h"

#include <cstring>
#include <mutex>

namespace
{
std::mutex         g_lock;
FfxHostVkFunctions g_fn;
FfxHostVkEnabled   g_enabled;

bool enabledExtension(const char* name)
{
    // Never offered: with it the backend selects wave64 pipelines (required subgroup size 64), and
    // on RX 9070 XT (RADV GFX1201) that output is blurred, scoring below bilinear in fsr3_selftest
    // (FSR_FEATURES=wave64: 13.66 dB vs 16.63 dB for wave32). Cause not investigated.
    if (std::strcmp(name, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) == 0)
        return false;
    for (const auto& e : g_enabled.deviceExtensions)
        if (e == name)
            return true;
    return false;
}

// Clears feature bits the application did not enable, in every structure the backend queries.
void clampFeatures(VkPhysicalDeviceFeatures2* features)
{
    for (auto* s = reinterpret_cast<VkBaseOutStructure*>(features->pNext); s; s = s->pNext)
    {
        switch (s->sType)
        {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
            // The backend picks its 16-bit shader variants on shaderFloat16 alone, but they also
            // declare the Int16 capability (validation layer, RX 9070 XT), so require both.
            reinterpret_cast<VkPhysicalDeviceShaderFloat16Int8Features*>(s)->shaderFloat16 &= g_enabled.shaderFloat16 && g_enabled.shaderInt16;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            reinterpret_cast<VkPhysicalDeviceSynchronization2Features*>(s)->synchronization2 &= g_enabled.synchronization2;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD:
            reinterpret_cast<VkPhysicalDeviceCoherentMemoryFeaturesAMD*>(s)->deviceCoherentMemory &= g_enabled.deviceCoherentMemory;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
            reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(s)->shaderStorageBufferArrayNonUniformIndexing &=
                g_enabled.shaderStorageBufferArrayNonUniformIndexing;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:
            reinterpret_cast<VkPhysicalDeviceAccelerationStructureFeaturesKHR*>(s)->accelerationStructure = VK_FALSE;
            break;
        default:
            break;
        }
    }
}
}  // namespace

void ffxHostVkSet(const FfxHostVkFunctions& functions, const FfxHostVkEnabled& enabled)
{
    std::lock_guard<std::mutex> l(g_lock);
    g_fn      = functions;
    g_enabled = enabled;
}

extern "C" {

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p)
{
    g_fn.GetPhysicalDeviceProperties(pd, p);
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p)
{
    g_fn.GetPhysicalDeviceProperties2(pd, p);
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f)
{
    // The backend reads core features only for its own information; report them as supported.
    g_fn.GetPhysicalDeviceFeatures(pd, f);
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f)
{
    g_fn.GetPhysicalDeviceFeatures2(pd, f);
    clampFeatures(f);
}

VKAPI_ATTR void VKAPI_CALL __wrap_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties* p)
{
    g_fn.GetPhysicalDeviceMemoryProperties(pd, p);
}

// Reports only the extensions the application enabled, so capability detection matches the device.
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* layer, uint32_t* count,
                                                                            VkExtensionProperties* out)
{
    uint32_t n = 0;
    VkResult r = g_fn.EnumerateDeviceExtensionProperties(pd, layer, &n, nullptr);
    if (r != VK_SUCCESS)
        return r;
    std::vector<VkExtensionProperties> all(n);
    r = g_fn.EnumerateDeviceExtensionProperties(pd, layer, &n, all.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return r;
    all.resize(n);
    std::vector<VkExtensionProperties> kept;
    for (const auto& e : all)
        if (enabledExtension(e.extensionName))
            kept.push_back(e);
    if (!out)
    {
        *count = static_cast<uint32_t>(kept.size());
        return VK_SUCCESS;
    }
    uint32_t copied = *count < kept.size() ? *count : static_cast<uint32_t>(kept.size());
    std::memcpy(out, kept.data(), copied * sizeof(VkExtensionProperties));
    *count = copied;
    return copied < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL __wrap_vkGetDeviceProcAddr(VkDevice device, const char* name)
{
    return g_fn.GetDeviceProcAddr(device, name);
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a,
                                                      VkBuffer* out)
{
    return g_fn.CreateBuffer(device, ci, a, out);
}

}  // extern "C"
