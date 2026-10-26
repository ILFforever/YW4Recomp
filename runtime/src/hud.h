/* The performance HUD (F3 or YW4_HUD=1): a few lines of numbers and a frame-time graph drawn on
 * the CPU with a built-in 5x7 font into a small RGBA image, which window.c copies onto the window's
 * top-left corner after the game's frame. Not in F12 screenshots (they read the game's frame). */
#ifndef YW4_HUD_H
#define YW4_HUD_H

#include <stdint.h>

enum { HUD_COLS = 40, HUD_LINES = 8, HUD_POINTS = 180 };

typedef struct {
    char lines[HUD_LINES][HUD_COLS];
    int nlines;
    float frame_ms[HUD_POINTS], work_ms[HUD_POINTS];  /* the graph, oldest first */
    int npoints;
    float target_ms;  /* drawn as a line: the game's frame pacing */
} HudContent;

/* perf.c: the HUD's lines and graph. */
void yw4_perf_hud(HudContent *c);

/* shadow.c: whose shadow casters are drawn ("ORIGINAL": the game's, "CUSTOM": our pass, "CHECK"). */
const char *shadow_mode_name(void);
/* gpu.c: the shadow maps' size (1024 is the game's; F8). */
int gpu_shadow_res(void);
/* gpu.c: the 3D view's height (900 is the game's; F4). */
int gpu_res_height(void);
/* gpu.c: texture filtering, 0 the game's, else anisotropic up to this (F2). */
int gpu_aniso(void);
/* gpu_draw.c: PCSS (soft contact shadows), 0 off, 1..3 = x1, x2, x4 (F7). */
int gpu_pcss_level(void);
/* gbuffer.c: our G-buffer programs on (F6). */
int gbuffer_native(void);

/* Draws c into rgba (w x h texels, 4 bytes each, rows of w); *used_w and *used_h: the part drawn. */
void hud_render(uint8_t *rgba, int w, int h, const HudContent *c, int *used_w, int *used_h);

#endif
