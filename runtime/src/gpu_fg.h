/* Frame generation (gpu_fg.c): a frame made between each two of the game's, shown half a frame earlier. */
#pragma once

#include <stdint.h>

#include "vk.h"

/* The setting: 0 off, 1 AMD FSR, 2 NVIDIA DLSS (gpu_dlssg.cpp; Streamline starts only when it is set at start).
 * Works with anti-aliasing TAA, FSR or DLSS (they have the game's motion). */
int gpu_fg(void);
void gpu_fg_cycle(void);
void gpu_fg_test_switch(uint64_t frame);  /* YW4_FG_AT=<frame>:<mode>,... (tests) */
/* 1 while FSR's frame generation shows frames: the window then doesn't wait for the monitor (window.c). */
int gpu_fg_active(void);
/* Frames on screen per game frame while frame generation runs (about 2), else 0 (the HUD's SHOWN line). */
float gpu_fg_shown_per_frame(void);

typedef struct {
    VkImage depth, motion;  /* the 3D's size, layout GENERAL */
    VkImageView depth_view, motion_view;
    VkImage scene;          /* the picture before the UI is drawn (output size, layout GENERAL): the HUD-less copy */
    VkFormat scene_format;
    uint32_t scene_w, scene_h;
    VkFormat depth_format;
    uint32_t render_w, render_h;
    float jitter[2];        /* as told to FSR's upscaler: the same conventions */
    float motion_scale[2];
    float frame_ms, cam_near, cam_far, fov_y;
} GpuFgPrepare;

/* After the anti-aliasing (in cb, outside rendering): this frame's depth and motion. */
void gpu_fg_prepare(VkCommandBuffer cb, const GpuFgPrepare *p);
/* At the present (in cb, before it is submitted): when frame generation is on, copies the presented image and,
 * if this frame was prepared, makes the frame between the last one and it. Returns the slot to give to
 * gpu_fg_show after the submit, or -1: present as usual. */
int gpu_fg_record(VkCommandBuffer cb, VkImage image, VkFormat format, uint32_t w, uint32_t h);
/* Hands the slot's frames to the pacing thread, which presents the generated one, then the game's half a
 * frame later. */
void gpu_fg_show(int slot, VkRect2D rect, uint64_t frame);
/* Waits until the pacing thread has shown everything (before presenting without it). */
void gpu_fg_drain(void);
