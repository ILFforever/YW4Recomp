# Vulkan headers

From [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers), tag `vulkan-sdk-1.4.357.0`, unmodified: `include/vulkan/{vk_platform.h, vulkan.h, vulkan_core.h}` and `include/vk_video/`. Licence: Apache-2.0 or MIT (`LICENSE.md`, `Apache-2.0.txt`, `MIT.txt`).

Committed so building needs nothing installed (decision 0017). The runtime loads `vulkan-1` at run time through SDL3, so no Vulkan import library is linked.
