/* NVIDIA DLSS frame generation through Streamline (gpu_dlssg.cpp, decision 0041). C interface. */
#pragma once

#include <stdint.h>

#include "gpu_fg.h"
#include "vk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Before the Vulkan instance: starts Streamline (sl.interposer.dll next to yw4.exe) when the PC has an NVIDIA GPU
 * (YW4_STREAMLINE=0: never), so DLSS frame generation and Reflex can be switched while playing. 1 when it runs. */
int dlssg_start(void);
/* sl.interposer.dll's vkGetInstanceProcAddr while Streamline runs (every Vulkan function from it: Vulkan's own,
 * Streamline's for the instance, device, surface, swapchain and present), else NULL: SDL's loader. */
PFN_vkGetInstanceProcAddr dlssg_loader(void);
/* The window's surface through Streamline (hwnd: the window's HWND); VK_NULL_HANDLE: make it as usual. */
VkSurfaceKHR dlssg_create_surface(void *hwnd);
/* 1 when DLSS frame generation runs on this GPU (checked once, after the device). */
int dlssg_available(void);
/* NVIDIA Reflex: 1 when it runs on this GPU; the REFLEX setting (0 off, 1 on, 2 on + boost; YW4_REFLEX) and its
 * step; what runs (the setting, at least on while frame generation is DLSS: NVIDIA requires it). */
int dlssg_reflex_available(void);
int dlssg_reflex_setting(void);
void dlssg_reflex_cycle(void);
int dlssg_reflex(void);

/* The frame's steps, on the presenting (main) thread. Each does nothing unless Streamline runs. */
void dlssg_frame_begin(void);  /* after a present: a new frame, Reflex's sleep, the simulation-start marker */
void dlssg_tag(VkCommandBuffer cb, const GpuFgPrepare *p, uint32_t output_w, uint32_t output_h);  /* after the AA */
/* Just before vkQueuePresentKHR: the game's rectangle in the window image (x, y, w, h), the options (on when the
 * setting says DLSS and this frame was tagged), the present-start marker. */
void dlssg_present_begin(int32_t x, int32_t y, uint32_t w, uint32_t h, VkFormat format, uint32_t images);
void dlssg_present_end(void);
/* Before dlssg_present_begin (window.c): this present's HUD-less copy at the window image's size, the picture before
 * the UI scaled into the game's rectangle; VK_NULL_HANDLE: none this frame. */
void dlssg_hudless(VkImage image, VkImageView view, VkFormat format, uint32_t w, uint32_t h);
/* Frames on screen per game frame while DLSS frame generation runs (2: one generated each), else 0. */
float dlssg_shown_per_frame(void);
/* 1 while frame generation is switched on in Streamline (the window doesn't wait for the monitor then). */
int dlssg_on(void);

#ifdef __cplusplus
}
#endif
