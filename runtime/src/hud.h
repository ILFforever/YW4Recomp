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

/* Draws c into rgba (w x h texels, 4 bytes each, rows of w); *used_w and *used_h: the part drawn. */
void hud_render(uint8_t *rgba, int w, int h, const HudContent *c, int *used_w, int *used_h);

#endif
