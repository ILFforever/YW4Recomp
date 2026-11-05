/* The performance HUD (F3 or YW4_HUD=1): a few lines of numbers and a frame-time graph drawn on
 * the CPU with a built-in 5x7 font into a small RGBA image, which window.c copies onto the window's
 * top-left corner after the game's frame. Not in F12 screenshots (they read the game's frame). */
#ifndef YW4_HUD_H
#define YW4_HUD_H

#include <stdint.h>

#include "bindings.h"
#include "settings.h"

enum { HUD_COLS = 40, HUD_LINES = 8, HUD_POINTS = 180 };

typedef struct {
    char lines[HUD_LINES][HUD_COLS];
    int nlines;
    float frame_ms[HUD_POINTS], work_ms[HUD_POINTS];  /* the graph, oldest first */
    int npoints;
    float target_ms;  /* drawn as a line: the game's frame pacing */
    /* the same numbers as values, for the styled HUD (hud_style.c); valid 0: no frames yet (lines[0] says why) */
    int valid;
    float target_fps;  /* the FRAME RATE setting; 0: none (vsync, unlimited) */
    float fps, low1, low01;              /* the last 30 frames' rate; the 1% and 0.1% lows of the last 600 */
    float frame_avg_ms, min_ms, max_ms;  /* frame time: average, lowest, highest of the last 600 */
    float work_avg_ms, gpu_ms;           /* the game's work and the GPU's busy time per frame; gpu_ms < 0: not yet */
    float cpu_pct, cpu_cores;            /* the whole CPU's share, and in cores */
    int ncpu;
    float draws, ram_mb;
    float shown_fps;                     /* frames on screen with frame generation; 0: off */
    const char *fg;                      /* "DLSS" or "FSR" */
} HudContent;

/* The HUD's parts in its detailed form: FPS and 1% low, the frame-time graph, GPU and CPU time, frame
 * generation. */
enum { HUD_PART_FPS = 1, HUD_PART_GRAPH = 2, HUD_PART_TIMES = 4, HUD_PART_FG = 8, HUD_PART_ALL = 15 };
/* perf.c: the HUD's lines and graph: compact (one line), or the detailed one's chosen parts. */
void yw4_perf_hud(HudContent *c, int compact, unsigned parts);

/* hud_style.c: the HUD as a see-through card (premultiplied RGBA, w x h) at s pixels per unit: compact (a chip) or
 * the detailed card with the chosen parts (HUD_PART_*). *used_w x *used_h: in, the part the last drawing in this
 * image touched, cleared first (0: the whole image); out, the part drawn. */
void hud_style_render(uint8_t *rgba, int w, int h, const HudContent *c, int compact, unsigned parts, float s,
                      int *used_w, int *used_h);

/* shadow.c: whose shadow casters are drawn ("ORIGINAL": the game's, "CUSTOM": our pass, "CHECK"). */
const char *shadow_mode_name(void);
/* gpu.c: the shadow maps' size (1024 is the game's). */
int gpu_shadow_res(void);
/* gpu.c: the 3D view's height (900 is the game's). */
int gpu_res_height(void);
/* gpu.c: texture filtering, 0 the game's, else anisotropic up to this. */
int gpu_aniso(void);
/* gpu_draw.c: PCSS (soft contact shadows), 0 off, 1..3 = x1, x2, x4. */
int gpu_pcss_level(void);
/* gbuffer.c: our G-buffer programs on. */
int gbuffer_native(void);

/* window.c: the HUD's scale, the settings panel's HUD SCALE slider: 0 auto (grows with the window), else the scale. */
float hud_scale_get(void);
void hud_scale_set(float v);
/* hud_style.c: the HUD's background, the HUD OPACITY slider: 0 (only the numbers) to 1. */
float hud_opacity_get(void);
void hud_opacity_set(float v);

/* Draws c into rgba (w x h texels, 4 bytes each, rows of w) at scale s (the text in the system's font at that
 * size); *used_w and *used_h: the part drawn. */
void hud_render(uint8_t *rgba, int w, int h, const HudContent *c, float s, int *used_w, int *used_h);
/* One line of text in the HUD's box, at scale s (a notice shown in the middle of the screen). */
void notice_render(uint8_t *rgba, int w, int h, const char *msg, float s, int *used_w, int *used_h);
/* The settings panel (settings.h) the same way: every row with all its options, the current one boxed. */
void panel_render(uint8_t *rgba, int w, int h, const SettingRow *rows, int n, int selected, int developer,
                  int *used_w, int *used_h);
/* The panel's controls page (bindings.h) the same way: the chosen row and column, whether it waits for an
 * input, and the text for the explanation box. */
void controls_render(uint8_t *rgba, int w, int h, const BindRow *rows, int n, int row, int col, int capturing,
                     const char *help, int *used_w, int *used_h);

#endif
