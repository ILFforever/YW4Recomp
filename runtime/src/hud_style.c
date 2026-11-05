/* The performance HUD's look (2026-11-05, owner: "do the full restyle"): a rounded, see-through violet card like
 * the in-game overlay's, labels in lilac in the overlay's font, digits in the system's monospaced font so they
 * don't jiggle, numbers coloured by how they are doing (green on target, amber slow, red well off), and the
 * frame-time graph as a line under a dashed target. Drawn on the CPU with ui.c into a see-through (premultiplied)
 * image, which window.c blends onto the picture. The parts and the compact form are the Overlay page's choices. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hud.h"
#include "ui.h"

enum { TEXT = 0xfdfcfe, LABEL = 0xd9cdea, GOOD = 0x7fe0a0, WARN = 0xf0c060, BAD = 0xff8a8a, CARD = 0x2a1b4c,
       PLOT = 0x3e2a6e, TARGET = 0xc6b2da };

typedef struct {
    const char *text;
    int font;
    float px;
    uint32_t rgb;
} Seg;

enum { MAXSEG = 8, PAD_X = 12, PAD_Y = 9, LABEL_W = 46, GAP = 7 };

/* the colour of a rate against the target: green at it, amber a little under, red well under; none: white */
static uint32_t rate_rgb(float v, float target) {
    if (target <= 0) return TEXT;
    return v >= target * 0.97f ? GOOD : v >= target * 0.85f ? WARN : BAD;
}

/* the colour of a time per frame against the frame's budget */
static uint32_t time_rgb(float ms, float budget) {
    return ms <= budget * 0.8f ? GOOD : ms <= budget ? WARN : BAD;
}

static float seg_w(const Seg *s, int n) {
    float w = 0;
    for (int i = 0; i < n; i++) w += ui_text_w(s[i].font, s[i].px, s[i].text) + (i ? GAP : 0);
    return w;
}

/* one row: its label, then its pieces on one baseline (all text boxes the same line height, centred) */
static void row(const UiCanvas *c, float y, float lh, const char *label, const Seg *s, int n) {
    if (label) ui_text_in(c, UI_GOTHIC_700, 10.5f, PAD_X, y, 0, lh, 0, label, LABEL);
    float x = PAD_X + (label ? LABEL_W : 0);
    for (int i = 0; i < n; i++) {
        ui_text_in(c, s[i].font, s[i].px, x, y, 0, lh, 0, s[i].text, s[i].rgb);
        x += ui_text_w(s[i].font, s[i].px, s[i].text) + GAP;
    }
}

void hud_style_render(uint8_t *rgba, int w, int h, const HudContent *c, int compact, unsigned parts, float s,
                      int *used_w, int *used_h) {
    /* only what the last drawing touched (the rest is still clear); none known: everything */
    if (*used_w > 0 && *used_h > 0) {
        for (int j = 0; j < *used_h && j < h; j++) memset(rgba + (size_t)j * (size_t)w * 4, 0, (size_t)*used_w * 4);
    } else {
        memset(rgba, 0, (size_t)w * (size_t)h * 4);
    }
    UiCanvas cv = {rgba, w, h, s, 0, 0};
    char b[16][32];
    float budget = c->target_ms > 0 ? c->target_ms : 33.3f;

    if (!c->valid) {  /* no frames yet, or the log is off: its one line */
        float tw = ui_text_w(UI_GOTHIC_700, 12, c->nlines ? c->lines[0] : ""), cw = tw + 2 * PAD_X, ch = 30;
        ui_card(&cv, 0.5f, 0.5f, cw, ch, 10, CARD, 0.72f, 1, 0xffffff, 0.14f);
        ui_text_in(&cv, UI_GOTHIC_700, 12, PAD_X, 0.5f, 0, ch, 0, c->nlines ? c->lines[0] : "", LABEL);
        *used_w = (int)ceilf((cw + 1) * s), *used_h = (int)ceilf((ch + 1) * s);
        return;
    }

    if (compact) {  /* a chip: the frame rate in its colour, "FPS", the 1% low */
        snprintf(b[0], sizeof b[0], "%.0f", c->fps);
        snprintf(b[1], sizeof b[1], "1%% %.1f", c->low1);
        Seg sg[3] = {{b[0], UI_MONO, 18, rate_rgb(c->fps, c->target_fps)}, {"FPS", UI_GOTHIC_700, 11, LABEL},
                     {b[1], UI_MONO, 12, LABEL}};
        float cw = seg_w(sg, 3) + 2 * PAD_X, ch = 30;
        ui_card(&cv, 0.5f, 0.5f, cw, ch, 10, CARD, 0.72f, 1, 0xffffff, 0.14f);
        float x = PAD_X;
        for (int i = 0; i < 3; i++) {
            ui_text_in(&cv, sg[i].font, sg[i].px, x, 0.5f, 0, ch, 0, sg[i].text, sg[i].rgb);
            x += ui_text_w(sg[i].font, sg[i].px, sg[i].text) + GAP;
        }
        *used_w = (int)ceilf((cw + 1) * s), *used_h = (int)ceilf((ch + 1) * s);
        return;
    }

    /* the detailed card: its rows (the Overlay page's parts), measured first for the card's size */
    struct {
        const char *label;
        Seg s[MAXSEG];
        int n;
        float lh;
    } rows[6];
    int nr = 0;
    int k = 0;
#define B(...) (snprintf(b[k], sizeof b[k], __VA_ARGS__), b[k++])
    int fps_on = parts & HUD_PART_FPS, times_on = parts & HUD_PART_TIMES, fg_on = parts & HUD_PART_FG;
    if (fps_on) {
        rows[nr].label = "FPS", rows[nr].lh = 22, rows[nr].n = 5;
        rows[nr].s[0] = (Seg){B("%.1f", c->fps), UI_MONO, 16, rate_rgb(c->fps, c->target_fps)};
        rows[nr].s[1] = (Seg){"1%", UI_MONO, 12, LABEL};
        rows[nr].s[2] = (Seg){B("%.1f", c->low1), UI_MONO, 12, TEXT};
        rows[nr].s[3] = (Seg){"0.1%", UI_MONO, 12, LABEL};
        rows[nr].s[4] = (Seg){B("%.1f", c->low01), UI_MONO, 12, TEXT};
        nr++;
        rows[nr].label = "Frame", rows[nr].lh = 18, rows[nr].n = 5;
        rows[nr].s[0] = (Seg){B("%.1f ms", c->frame_avg_ms), UI_MONO, 12, TEXT};
        rows[nr].s[1] = (Seg){"min", UI_MONO, 12, LABEL};
        rows[nr].s[2] = (Seg){B("%.1f", c->min_ms), UI_MONO, 12, TEXT};
        rows[nr].s[3] = (Seg){"max", UI_MONO, 12, LABEL};
        rows[nr].s[4] = (Seg){B("%.1f", c->max_ms), UI_MONO, 12, c->max_ms > budget * 1.25f ? WARN : TEXT};
        nr++;
    }
    if (fg_on && c->shown_fps > 0) {
        rows[nr].label = "Gen", rows[nr].lh = 18, rows[nr].n = 2;
        rows[nr].s[0] = (Seg){B("%.0f", c->shown_fps), UI_MONO, 12, TEXT};
        rows[nr].s[1] = (Seg){B("shown, %s", c->fg), UI_MONO, 12, LABEL};
        nr++;
    }
    if (times_on) {
        rows[nr].label = "GPU", rows[nr].lh = 18;
        if (c->gpu_ms >= 0) rows[nr].s[0] = (Seg){B("%.1f ms", c->gpu_ms), UI_MONO, 12, time_rgb(c->gpu_ms, budget)};
        else rows[nr].s[0] = (Seg){"- ms", UI_MONO, 12, TEXT};
        rows[nr].s[1] = (Seg){"work", UI_MONO, 12, LABEL};
        rows[nr].s[2] = (Seg){B("%.1f ms", c->work_avg_ms), UI_MONO, 12, time_rgb(c->work_avg_ms, budget)};
        rows[nr].n = 3;
        nr++;
        rows[nr].label = "CPU", rows[nr].lh = 18, rows[nr].n = 2;
        rows[nr].s[0] = (Seg){B("%.0f%%", c->cpu_pct), UI_MONO, 12, TEXT};
        rows[nr].s[1] = (Seg){B("%.1f of %d cores", c->cpu_cores, c->ncpu), UI_MONO, 12, LABEL};
        nr++;
        rows[nr].label = "Draws", rows[nr].lh = 18, rows[nr].n = 3;
        rows[nr].s[0] = (Seg){B("%.0f", c->draws), UI_MONO, 12, TEXT};
        rows[nr].s[1] = (Seg){"RAM", UI_MONO, 12, LABEL};
        rows[nr].s[2] = (Seg){B("%.1f GB", c->ram_mb / 1024.0f), UI_MONO, 12, TEXT};
        nr++;
    }
#undef B
    float inner = 206;  /* at least the graph's width */
    for (int i = 0; i < nr; i++) {
        float rw = LABEL_W + seg_w(rows[i].s, rows[i].n);
        if (rw > inner) inner = rw;
    }
    int graph = (parts & HUD_PART_GRAPH) && c->npoints > 1;
    float gh = 34, ch = 2 * PAD_Y;
    for (int i = 0; i < nr; i++) ch += rows[i].lh;
    if (graph) ch += (nr ? 6 : 0) + gh;
    float cw = inner + 2 * PAD_X;
    ui_card(&cv, 0.5f, 0.5f, cw, ch, 12, CARD, 0.72f, 1, 0xffffff, 0.14f);
    float y = 0.5f + PAD_Y;
    for (int i = 0; i < nr; i++) {
        row(&cv, y, rows[i].lh, rows[i].label, rows[i].s, rows[i].n);
        y += rows[i].lh;
    }
    if (graph) {  /* the frame times, oldest left; the target dashed; up to twice the budget */
        if (nr) y += 6;
        float gx = PAD_X, gw = inner, top = budget * 2.0f;
        for (int i = 0; i < c->npoints; i++)
            if (c->frame_ms[i] > top) top = c->frame_ms[i];
        ui_card(&cv, gx, y, gw, gh, 6, PLOT, 0.6f, 0, 0, 0);
        float ty = y + gh - 3 - (budget / top) * (gh - 6);
        for (float dx = 0; dx < gw; dx += 8) ui_rect(&cv, gx + dx, ty, 4, 1, TARGET, 0.9f);
        float step = gw / (float)(c->npoints - 1);
        for (int i = 1; i < c->npoints; i++) {  /* a 2-unit line: each step's vertical span, as a thin column */
            float y0 = y + gh - 3 - (c->frame_ms[i - 1] / top) * (gh - 6);
            float y1 = y + gh - 3 - (c->frame_ms[i] / top) * (gh - 6);
            float lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
            ui_rect(&cv, gx + (float)(i - 1) * step, lo - 1, step + 0.5f, hi - lo + 2,
                    c->frame_ms[i] > budget * 1.25f ? WARN : GOOD, 1);
        }
    }
    *used_w = (int)ceilf((cw + 1) * s), *used_h = (int)ceilf((ch + 1) * s);
    if (*used_w > w) *used_w = w;
    if (*used_h > h) *used_h = h;
}
