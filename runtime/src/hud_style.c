/* The performance HUD's look (2026-11-05/06, owner's design canvas, layout C): a rounded, see-through violet card
 * like the in-game overlay's. A big frame rate on top, then the 1% and 0.1% lows, the frame-time graph as a line under
 * a dashed target, and a grid of the times and resources. With frame generation on, the big number is the frames on
 * screen with DLSS or FSR beside it in large letters, and the game's own FPS goes small on the right; the big number's
 * colour still follows the game's own FPS, because generated frames can look fine while the game runs slow.
 * Labels are lilac in the overlay's font; digits in its rounded font (M PLUS Rounded: its digits differ by under 2%).
 * Numbers are coloured by how they are doing (green on target, amber slow, red well off). Drawn on the CPU with ui.c
 * into a see-through (premultiplied) image, which window.c blends onto the picture. The parts and the compact form
 * are the Overlay page's choices. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud.h"
#include "theme.h"
#include "ui.h"

/* the card's colours follow the theme (theme.h); the numbers' traffic-light colours stay the same in every theme */
enum { GOOD = 0x7fe0a0, WARN = 0xf0c060, BAD = 0xff8a8a };
#define TEXT (theme_colors()->hud_text)
#define LABEL (theme_colors()->hud_label)
#define CARD (theme_colors()->hud_card)
#define PLOT (theme_colors()->hud_plot)
#define TARGET (theme_colors()->hud_target)

/* the fonts: numbers, and the small words between them */
enum { NUM = UI_ROUNDED_700, WORD = UI_GOTHIC_700 };
#define WORD_PX 10.5f

enum { PAD_X = 12, PAD_Y = 9, GAP = 7, MAX_PIECES = 4, MAX_TILES = 6 };
/* the card's sizes, in the design's units */
enum { MIN_INNER = 206, HERO_H = 36, LOWS_H = 18, LOW2_X = 84, GRAPH_H = 34, WORD_LH = 14, NUM_LH = 17, TILE_GAP = 6 };

/* a piece of a line of text; more: extra space before it */
typedef struct {
    const char *text;
    int font;
    float px;
    uint32_t rgb;
    float more;
} Piece;

/* The card's background (the HUD OPACITY slider, YW4_HUD_OPACITY=0..1). The default is a slider step close to the
 * designed 0.72. The border and the graph's plate fade with it below that, so at 0 only the numbers are left. */
#define OPACITY_DEFAULT 0.70f
static float hud_opacity = -1.0f;

float hud_opacity_get(void) {
    float v;
    __atomic_load(&hud_opacity, &v, __ATOMIC_ACQUIRE);  /* the generic form: _n takes no float before Clang 19 */
    if (v < 0) {
        const char *e = getenv("YW4_HUD_OPACITY");
        v = e && *e ? (float)atof(e) : OPACITY_DEFAULT;
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        __atomic_store(&hud_opacity, &v, __ATOMIC_RELEASE);
    }
    return v;
}

void hud_opacity_set(float v) { __atomic_store(&hud_opacity, &v, __ATOMIC_RELEASE); }

/* the card: its fill at the slider's opacity, its border fading with it */
static void card(const UiCanvas *c, float w, float h, float r) {
    float op = hud_opacity_get(), fade = op < OPACITY_DEFAULT ? op / OPACITY_DEFAULT : 1;
    ui_card(c, 0.5f, 0.5f, w, h, r, CARD, op, 1, 0xffffff, 0.14f * fade);
}

/* the colour of a rate against the target: green at it, amber a little under, red well under; none: white */
static uint32_t rate_rgb(float v, float target) {
    if (target <= 0) return TEXT;
    return v >= target * 0.97f ? GOOD : v >= target * 0.85f ? WARN : BAD;
}

/* the colour of a time per frame against the frame's budget */
static uint32_t time_rgb(float ms, float budget) {
    return ms <= budget * 0.8f ? GOOD : ms <= budget ? WARN : BAD;
}

/* The width of a text, every digit counted as a 0: the digits differ by a little, which would make the pieces after a
 * number, and a card as wide as its text, tremble as the number changes. */
static float stable_w(int font, float px, const char *text) {
    char t[32];
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof t; i++) t[i] = text[i] >= '0' && text[i] <= '9' ? '0' : text[i];
    t[i] = 0;
    return ui_text_w(font, px, t);
}

static float line_w(const Piece *p, int n) {
    float w = 0;
    for (int i = 0; i < n; i++) w += stable_w(p[i].font, p[i].px, p[i].text) + (i ? GAP + p[i].more : 0);
    return w;
}

/* pieces side by side at (x, y) on one baseline, in a line box lh high */
static void line(const UiCanvas *c, float x, float y, float lh, const Piece *p, int n) {
    float base = 0;
    for (int i = 0; i < n; i++) {
        float b = ui_baseline(p[i].font, p[i].px, lh);
        if (b > base) base = b;
    }
    for (int i = 0; i < n; i++) {
        if (i) x += GAP + p[i].more;
        ui_text(c, p[i].font, p[i].px, x, y + base - ui_baseline(p[i].font, p[i].px, lh), lh, p[i].text, p[i].rgb);
        x += stable_w(p[i].font, p[i].px, p[i].text);
    }
}

/* the frame times, oldest left; the target dashed; up to twice the budget */
static void graph_draw(const UiCanvas *cv, const HudContent *c, float x, float y, float w, float budget) {
    float h = GRAPH_H, top = budget * 2.0f;
    for (int i = 0; i < c->npoints; i++)
        if (c->frame_ms[i] > top) top = c->frame_ms[i];
    float op = hud_opacity_get();
    ui_card(cv, x, y, w, h, 6, PLOT, 0.6f * (op < OPACITY_DEFAULT ? op / OPACITY_DEFAULT : 1), 0, 0, 0);
    float ty = y + h - 3 - (budget / top) * (h - 6);
    for (float dx = 0; dx < w; dx += 8) ui_rect(cv, x + dx, ty, 4, 1, TARGET, 0.9f);
    float step = w / (float)(c->npoints - 1);
    for (int i = 1; i < c->npoints; i++) {  /* a 2-unit line: each step's vertical span, as a thin column */
        float y0 = y + h - 3 - (c->frame_ms[i - 1] / top) * (h - 6);
        float y1 = y + h - 3 - (c->frame_ms[i] / top) * (h - 6);
        float lo = y0 < y1 ? y0 : y1, hi = y0 < y1 ? y1 : y0;
        ui_rect(cv, x + (float)(i - 1) * step, lo - 1, step + 0.5f, hi - lo + 2,
                c->frame_ms[i] > budget * 1.25f ? WARN : GOOD, 1);
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
    char b[12][32];
    int k = 0;
#define B(...) (snprintf(b[k], sizeof b[k], __VA_ARGS__), b[k++])
    float budget = c->target_ms > 0 ? c->target_ms : 33.3f;

    if (!c->valid) {  /* no frames yet, or the log is off: its one line */
        float tw = ui_text_w(WORD, 12, c->nlines ? c->lines[0] : ""), cw = tw + 2 * PAD_X, ch = 30;
        card(&cv, cw, ch, 10);
        ui_text_in(&cv, WORD, 12, PAD_X, 0.5f, 0, ch, 0, c->nlines ? c->lines[0] : "", LABEL);
        *used_w = (int)ceilf((cw + 1) * s), *used_h = (int)ceilf((ch + 1) * s);
        return;
    }

    uint32_t rate = rate_rgb(c->fps, c->target_fps);  /* the game's own, with frame generation too */

    if (compact) {  /* a chip: the frame rate; frame generation: the frames on screen, its name, then the game's */
        Piece p[MAX_PIECES];
        int n = 0;
        if (c->shown_fps > 0) {
            p[n++] = (Piece){B("%.0f", c->shown_fps), NUM, 18, rate, 0};
            p[n++] = (Piece){c->fg, NUM, 13, TEXT, 0};
            p[n++] = (Piece){B("%.0f", c->fps), NUM, 12, TEXT, 5};
        } else {
            p[n++] = (Piece){B("%.0f", c->fps), NUM, 18, rate, 0};
        }
        p[n++] = (Piece){"FPS", WORD, 11, LABEL, 0};
        float cw = line_w(p, n) + 2 * PAD_X, ch = 30;
        card(&cv, cw, ch, 10);
        line(&cv, PAD_X + 0.5f, 0.5f, ch, p, n);
        *used_w = (int)ceilf((cw + 1) * s), *used_h = (int)ceilf((ch + 1) * s);
        return;
    }

    /* the detailed card, by the Overlay page's parts: measured first for the card's size */
    int fps_on = parts & HUD_PART_FPS, times_on = parts & HUD_PART_TIMES;
    int shown_on = (parts & HUD_PART_FG) && c->shown_fps > 0;
    int graph_on = (parts & HUD_PART_GRAPH) && c->npoints > 1;

    Piece hero[2];
    const char *small = NULL;  /* the game's own frame rate on the right, when the big number is the frames on screen */
    int hero_on = fps_on || shown_on;
    if (shown_on) {
        hero[0] = (Piece){B("%.0f", c->shown_fps), NUM, 30, rate, 0};
        hero[1] = (Piece){c->fg, NUM, 20, TEXT, 0};
        if (fps_on) small = B("%.1f", c->fps);
    } else {
        hero[0] = (Piece){B("%.1f", c->fps), NUM, 30, rate, 0};
        hero[1] = (Piece){"FPS", NUM, 20, TEXT, 0};
    }
    Piece low1[2] = {{"1% low", WORD, WORD_PX, LABEL, 0}, {B("%.1f", c->low1), NUM, 12, TEXT, 0}};
    Piece low01[2] = {{"0.1% low", WORD, WORD_PX, LABEL, 0}, {B("%.1f", c->low01), NUM, 12, TEXT, 0}};

    struct {
        const char *label, *value;
        uint32_t rgb;
    } tiles[MAX_TILES];
    int nt = 0;
#define TILE(l, v, col) (tiles[nt].label = (l), tiles[nt].value = (v), tiles[nt].rgb = (col), nt++)
    if (fps_on) {
        TILE("Frame", B("%.1f ms", c->frame_avg_ms), TEXT);
        TILE("Max", B("%.1f ms", c->max_ms), c->max_ms > budget * 1.25f ? WARN : TEXT);
    }
    if (times_on) {
        if (c->gpu_ms >= 0) TILE("GPU", B("%.1f ms", c->gpu_ms), time_rgb(c->gpu_ms, budget));
        else TILE("GPU", "- ms", TEXT);
        TILE("Game", B("%.1f ms", c->work_avg_ms), time_rgb(c->work_avg_ms, budget));
        TILE("CPU", B("%.0f%%", c->cpu_pct), TEXT);
        TILE("RAM", B("%.1f GB", c->ram_mb / 1024.0f), TEXT);
    }
#undef TILE

    float inner = MIN_INNER;
    if (hero_on) {
        float hw = line_w(hero, 2) + (small ? GAP + stable_w(NUM, 12, small) : 0);
        if (hw > inner) inner = hw;
    }
    if (fps_on && LOW2_X + line_w(low01, 2) > inner) inner = LOW2_X + line_w(low01, 2);
    int tile_rows = (nt + 2) / 3;
    float tile_h = WORD_LH + NUM_LH, ch = 2 * PAD_Y;
    if (hero_on) ch += HERO_H;
    if (fps_on) ch += LOWS_H;
    int above = hero_on || fps_on;  /* something above the graph, and below it */
    if (graph_on) ch += GRAPH_H + (above ? 5 : 0) + (nt ? 7 : 0);
    else if (above && nt) ch += TILE_GAP;
    if (nt) ch += (float)tile_rows * tile_h + (float)(tile_rows - 1) * TILE_GAP;
    float cw = inner + 2 * PAD_X;

    card(&cv, cw, ch, 12);
    float x = PAD_X + 0.5f, y = PAD_Y + 0.5f;
    if (hero_on) {
        line(&cv, x, y, HERO_H, hero, 2);
        if (small) {  /* value over word, ending at the card's right, in the middle of the big line */
            float right = x + inner, top = y + (HERO_H - NUM_LH - WORD_LH) * 0.5f;
            ui_text(&cv, NUM, 12, right - stable_w(NUM, 12, small), top, NUM_LH, small, TEXT);
            ui_text_in(&cv, WORD, WORD_PX, right - inner, top + NUM_LH, inner, WORD_LH, 2, "FPS", LABEL);
        }
        y += HERO_H;
    }
    if (fps_on) {
        line(&cv, x, y, LOWS_H, low1, 2);
        line(&cv, x + LOW2_X, y, LOWS_H, low01, 2);
        y += LOWS_H;
    }
    if (graph_on) {
        if (above) y += 5;
        graph_draw(&cv, c, x, y, inner, budget);
        y += GRAPH_H + (nt ? 7 : 0);
    } else if (above && nt) {
        y += TILE_GAP;
    }
    for (int i = 0; i < nt; i++) {  /* three to a row: the label over the value */
        float tx = x + (float)(i % 3) * (inner / 3), ty = y + (float)(i / 3) * (tile_h + TILE_GAP);
        ui_text(&cv, WORD, WORD_PX, tx, ty, WORD_LH, tiles[i].label, LABEL);
        ui_text(&cv, NUM, 12, tx, ty + WORD_LH, NUM_LH, tiles[i].value, tiles[i].rgb);
    }
#undef B
    *used_w = (int)ceilf((cw + 1) * s), *used_h = (int)ceilf((ch + 1) * s);
    if (*used_w > w) *used_w = w;
    if (*used_h > h) *used_h = h;
}
