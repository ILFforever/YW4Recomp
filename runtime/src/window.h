/* The game's window: SDL3 for the window and events, Vulkan to show frames (decision 0019). */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#include "vk.h"

/* Opens the window and runs guest(arg) on a new host thread; this (the process's main)
 * thread then handles window events until the window is closed, and the process exits.
 * With YW4_HEADLESS=1, or if no window can be made, guest(arg) runs here with no window. */
void window_run(void (*guest)(void *), void *arg);

/* Shows a frame: w x h pixels, RGBA8 rows of w * 4 bytes, the top row first. Called from
 * a guest thread (NVN's present). Does nothing without a window. Also saves it as a
 * screenshot when asked (F12, or YW4_SCREENSHOTS=<frame>,<frame>,...). */
void window_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t frame);

/* The same for an image of the draw path (gpu.c, decision 0021): w x h colour texels in
 * layout GENERAL, written by earlier submissions on the queue. Screenshots need an RGBA8 or
 * BGRA8 format. */
void window_present_image(VkImage image, VkFormat format, uint32_t w, uint32_t h, uint64_t frame);

#endif
