/* Intel XeSS upscaling (gpu_xess.c, decision 0089): Intel's libxess.dll next to yw4.exe, loaded at start. Windows. */
#pragma once

#include <stdint.h>

#include "vk.h"

typedef struct {
    VkImage color, depth, motion, output;  /* all in layout GENERAL; output has storage use */
    VkImageView color_view, depth_view, motion_view, output_view;
    VkFormat color_format, depth_format, output_format;  /* depth: a depth-only format */
    uint32_t render_w, render_h, output_w, output_h;
    int quality;            /* the upscaling mode: 0 none (XeSS's anti-aliasing), 1 quality, 2 balanced, 3 performance */
    float jitter[2];        /* this frame's shift of the 3D draws, in render pixels */
    float motion_scale[2];  /* motion texel x scale = pixel motion from this frame's position to the last */
    int reset;
} GpuXessInputs;

/* 1: built with the XeSS headers. */
int gpu_xess_built(void);
/* Before vkCreateInstance: the instance extensions XeSS needs (libxess.dll loaded now); 0 if none or no XeSS. */
int gpu_xess_instance_extensions(const char *const **exts, uint32_t *count);
/* Before vkCreateDevice: the device extensions XeSS needs on this GPU; 0 if none or no XeSS. */
int gpu_xess_device_extensions(VkInstance instance, VkPhysicalDevice gpu, const char *const **exts, uint32_t *count);
/* Before vkCreateDevice: XeSS fills in the features it needs in the chain starting at features (a
 * VkPhysicalDeviceFeatures2) and may add structures of its own to it. */
void gpu_xess_device_features(VkInstance instance, VkPhysicalDevice gpu, VkPhysicalDeviceFeatures2 *features);
/* 1: XeSS runs on this device (its context made; checked once, after the device exists). */
int gpu_xess_available(void);
/* XeSS made ready for these sizes and this mode now, if it isn't (ahead of its first use). */
void gpu_xess_prepare(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int quality);
/* Records XeSS into cb (outside rendering). 0 if it could not run. */
int gpu_xess_dispatch(VkCommandBuffer cb, const GpuXessInputs *in);
