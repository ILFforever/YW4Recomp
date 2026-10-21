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

/* The same for an image of the draw path (gpu.c, decision 0021): the rectangle `rect` of a
 * colour image in layout GENERAL, written by earlier submissions on the queue, scaled to the
 * window. Screenshots (of the rectangle) need an RGBA8 or BGRA8 format. */
void window_present_image(VkImage image, VkFormat format, VkRect2D rect, uint64_t frame);

/* The controller as the window sees it now, from the keyboard and any gamepad: Switch
 * button bits (HidNpadButton: A 0, B 1, X 2, Y 3, L-stick 4, R-stick 5, L 6, R 7, ZL 8,
 * ZR 9, Plus 10, Minus 11, D-pad left 12 up 13 right 14 down 15) and stick positions (left
 * x, y, right x, y; -32767..32767, up positive). Zero without a window. Any thread. */
void window_input(uint64_t *buttons, int32_t sticks[4]);

/* The same as pressing F12 (a screenshot of the next frame), for replays. */
void window_request_screenshot(void);

#endif
