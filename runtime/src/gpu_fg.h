/* Frame generation (gpu_fg.c): a frame made between each two of the game's, shown half a frame earlier. */
#pragma once

#include <stdint.h>

#include "vk.h"

/* The setting: 0 off, 1 AMD FSR, 2 NVIDIA DLSS (gpu_dlssg.cpp; Streamline starts only when it is set at start).
 * Works with anti-aliasing TAA, FSR or DLSS (they have the game's motion). */
int gpu_fg(void);
void gpu_fg_cycle(void);
void gpu_fg_step(int dir);
void gpu_fg_test_switch(uint64_t frame);  /* YW4_FG_AT=<frame>:<mode>,... (tests) */
/* 1 for the frames after a switch of the setting: the window lets the GPU finish all its work before each of
 * them and shows "switching" (FSR's and Streamline's switch freed image views a command buffer still used:
 * Vulkan validation, 2026-11-04; an occasional device-lost crash when turning frame generation on). */
int gpu_fg_switching(void);
/* 1 while FSR's frame generation shows frames: the window then doesn't wait for the monitor (window.c). */
int gpu_fg_active(void);
/* Why a presented frame got no generated frame: counted by these reasons, printed with the every-600 line. */
enum { FG_SKIP_MOTION, FG_SKIP_SIZE, FG_SKIP_INACTIVE, FG_SKIP_CONTEXT, FG_SKIP_CONFIGURE, FG_SKIP_DISPATCH,
       FG_SKIP_NO_PREPARE, FG_SKIP_NO_CONTEXT, FG_SKIP_N };
void gpu_fg_skip(int reason);
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
    /* The game's camera for this frame (has_cam; else the placeholders above stay): Streamline's conventions, ready to
     * copy: row-major for row vectors, clip depth 0..1, the view looking down +z (gpu_aa.c, fg_camera) */
    int has_cam;
    float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
    float cam_pos[3], cam_right[3], cam_up[3], cam_fwd[3];
} GpuFgPrepare;

/* After the anti-aliasing (in cb, outside rendering): this frame's depth and motion. */
void gpu_fg_prepare(VkCommandBuffer cb, const GpuFgPrepare *p);
/* At the present (in cb, before it is submitted): when frame generation is on, copies the presented image and,
 * if this frame was prepared, makes the frame between the last one and it. Returns the slot to give to
 * gpu_fg_show after the submit, or -1: present as usual. */
int gpu_fg_record(VkCommandBuffer cb, VkImage image, VkFormat format, uint32_t w, uint32_t h);
/* Every present (gpu.c): the presented image's format and size, and the part of it the window shows. */
void gpu_fg_note_present(VkFormat format, uint32_t w, uint32_t h, VkRect2D rect);
/* The HUD-less copy: the picture before the UI in the presented format, at its place in the presented image
 * (made after the anti-aliasing, for FSR's and DLSS's frame generation). gpu_fg_hudless: it, when made for the
 * present now happening, else VK_NULL_HANDLE. */
void gpu_fg_hudless_copy(VkCommandBuffer cb, const GpuFgPrepare *p);
VkImage gpu_fg_hudless(void);
/* Hands the slot's frames to the pacing thread, which presents the generated one, then the game's half a
 * frame later. */
void gpu_fg_show(int slot, VkRect2D rect, uint64_t frame);
/* Waits until the pacing thread has shown everything (before presenting without it). */
void gpu_fg_drain(void);
