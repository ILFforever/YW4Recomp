/* AMD FSR upscaling (gpu_fsr.c): the FidelityFX SDK's Vulkan DLLs, loaded on first use. */
#pragma once

#include <stdint.h>

#include "vk.h"

typedef struct {
    VkImage color, depth, motion, output;  /* all in layout GENERAL; output has storage use */
    VkFormat color_format, depth_format, output_format;
    uint32_t render_w, render_h, output_w, output_h;
    float jitter[2];        /* this frame's shift of the 3D draws, in render pixels */
    float motion_scale[2];  /* motion texel x scale = FSR's pixel motion (from this frame's position to the last) */
    int reset;              /* no usable history: the first frame, a size change, a frame without FSR */
    float frame_ms, sharpness, cam_near, cam_far, fov_y;
} GpuFsrInputs;

/* 1: the DLLs are there and FSR runs on this device (checked once). */
int gpu_fsr_available(void);
/* The shift for frame n of FSR's jitter sequence (render w against output ow), in pixels. */
void gpu_fsr_jitter(uint32_t w, uint32_t ow, uint64_t n, float *jx, float *jy);
/* Records FSR's passes into cb (outside rendering). 0 if FSR could not run. */
int gpu_fsr_dispatch(VkCommandBuffer cb, const GpuFsrInputs *in);
