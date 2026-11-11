/* The game's window: SDL3 for the window and events, Vulkan to show frames (decision 0019). */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

#include "vk.h"

/* Opens the window and runs guest(arg) on a new host thread; this (the process's main)
 * thread then handles window events until the window is closed, and the process exits.
 * With YW4_HEADLESS=1, or if no window can be made, guest(arg) runs here with no window. */
void window_run(void (*guest)(void *), void *arg);
/* Early in main(), once the settings are read: starts what the window needs later and takes long (Streamline). */
void window_prestart(void);

/* Shows a frame: w x h pixels, RGBA8 rows of w * 4 bytes, the top row first. Called from
 * a guest thread (NVN's present). Does nothing without a window. Also saves it as a
 * screenshot when asked (F12, or YW4_SCREENSHOTS=<frame>,<frame>,...). */
void window_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t frame);

/* The same for an image of the draw path (gpu.c, decision 0021): the rectangle `rect` of a
 * colour image in layout GENERAL, written by earlier submissions on the queue, scaled to the
 * window. Screenshots (of the rectangle) need an RGBA8 or BGRA8 format. */
void window_present_image(VkImage image, VkFormat format, VkRect2D rect, uint64_t frame);
/* Added to screenshot file names after the frame number until set back to "" (frame_N_gen.bmp). */
void window_screenshot_suffix(const char *suffix);

/* The controller as the window sees it now, from the keyboard and any gamepad: Switch
 * button bits (HidNpadButton: A 0, B 1, X 2, Y 3, L-stick 4, R-stick 5, L 6, R 7, ZL 8,
 * ZR 9, Plus 10, Minus 11, D-pad left 12 up 13 right 14 down 15) and stick positions (left
 * x, y, right x, y; -32767..32767, up positive). Zero without a window. Any thread. */
void window_input(uint64_t *buttons, int32_t sticks[4]);

/* How many times the game has read window_input so far (a press must last at least two reads to be seen). */
uint64_t window_input_reads(void);
void window_input_read_replayed(void);  /* a replay gave the game its input instead: counts as a read too */
/* The overlay's pause on or off (the game's Plus pause, its screen hidden): F4, replays' PAUSE key. */
void window_pause_toggle(void);
int window_paused(void);  /* the overlay's pause is on: the player's input is held back */
uint64_t window_pause_buttons(void);  /* the buttons it presses for the player now (live input and replays) */

/* Relative mouse motion accumulated since the last call (bindings.c yw4_hook_mouse_look). */
void window_mouse_motion(float *dx, float *dy);

/* Rumble the active gamepad (low-frequency and high-frequency motors, 0..65535, duration in ms). Any thread. */
void window_rumble(uint16_t low, uint16_t high, uint32_t duration_ms);
/* The game's HD rumble (amplitudes 0..1, frequencies in Hz) shaped for a PC pad's motors (window.c). Any thread. */
void window_vibration(float amp_low, float freq_low, float amp_high, float freq_high);
void window_rumble_test(void);
int window_rumble_enabled(void);
void window_rumble_set_enabled(int on);

/* The same as pressing F12 (a screenshot of the next frame), for replays. */
void window_request_screenshot(void);
/* While the game waits on the player (the name entry, swkbd.c): its last frame again with the overlay over it;
 * keep: first call, the frame becomes the overlay's background. -1 when nothing can be shown. */
int window_repaint(int keep);
/* The performance HUD (F3, the in-game overlay's Performance page): on or off (back to its last mode); its mode
 * (0 off, 1 detailed, 2 compact), corner (0 top left, 1 top right, 2 bottom left, 3 bottom right) and parts in the
 * detailed one (hud.h HUD_PART_*); a preview of it drawn into an RGBA image (w x h), and the part drawn. */
void window_hud_toggle(void);
int window_hud_shown(void);
int window_hud_mode(void);
void window_hud_set_mode(int mode);
int window_hud_corner(void);
void window_hud_set_corner(int corner);
unsigned window_hud_parts(void);
void window_hud_set_parts(unsigned parts);
void window_hud_preview(uint8_t *rgba, int w, int h, float canvas_s, int *used_w, int *used_h);

/* Full screen (borderless, the desktop's size) or a window: 1 or 0, from YW4_WINDOW=fullscreen|windowed at first;
 * the cycle flips it (panel WINDOW MODE, F11). Any thread; the window's thread applies it within a few ms. */
int window_fullscreen_index(void);
void window_fullscreen_cycle(void);

#endif
