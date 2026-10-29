/* NVIDIA DLSS upscaling (gpu_dlss.c, decision 0040): NGX from the NVIDIA driver, nvngx_dlss.dll next to yw4.exe. */
#pragma once

#include <stdint.h>

#include "vk.h"

typedef struct {
    VkImage color, depth, motion, output;  /* all in layout GENERAL; output has storage use */
    VkImageView color_view, depth_view, motion_view, output_view;
    VkFormat color_format, depth_format, output_format;
    uint32_t render_w, render_h, output_w, output_h;
    int quality;            /* the upscaling mode: 0 none (DLAA), 1 quality, 2 balanced, 3 performance */
    float jitter[2];        /* this frame's shift of the 3D draws, in render pixels */
    float motion_scale[2];  /* motion texel x scale = pixel motion from this frame's position to the last */
    int reset;
    float frame_ms;
} GpuDlssInputs;

/* The device extensions DLSS needs (NVIDIA GPUs only: vendor id 0x10de), for window.c to enable; 0 if none. */
int gpu_dlss_device_extensions(uint32_t vendor, const char ***exts, unsigned *count);
/* The DLSS model (panel DLSS MODEL, YW4_DLSS_MODEL): 0 fast (the CNN, preset E), 1 best (the transformers, DLSS's
 * own defaults). */
int gpu_dlss_model(void);
void gpu_dlss_model_cycle(void);
/* 1: NGX started and DLSS is available on this GPU (checked once, after the device exists). */
int gpu_dlss_available(void);
/* Records DLSS into cb (outside rendering). 0 if it could not run. */
int gpu_dlss_dispatch(VkCommandBuffer cb, const GpuDlssInputs *in);
