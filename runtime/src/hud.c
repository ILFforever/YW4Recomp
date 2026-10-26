/* The HUD (hud.h) drawn into an RGBA image: its lines in a 5x7 font of the characters they use
 * (lower case drawn as upper case, characters without a glyph as blanks), then the frame-time graph. */
#include <stdio.h>
#include <string.h>

#include "hud.h"

/* One glyph: 7 rows, the 5 low bits of each, bit 4 the leftmost column. */
static const struct {
    char c;
    uint8_t rows[7];
} glyphs[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}}, {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}}, {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
};

static const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof glyphs / sizeof glyphs[0]; i++)
        if (glyphs[i].c == c) return glyphs[i].rows;
    return NULL;
}

enum { CELL_W = 6, CELL_H = 9, MARGIN = 4, GRAPH_H = 40 };

typedef struct {
    uint8_t *rgba;
    int w, h;  /* the image */
    int bw, bh;  /* the box: nothing is drawn outside it */
} Canvas;

static void put(const Canvas *cv, int x, int y, const uint8_t col[3]) {
    if (x < 0 || y < 0 || x >= cv->bw || y >= cv->bh) return;
    uint8_t *p = cv->rgba + ((size_t)y * cv->w + x) * 4;
    p[0] = col[0], p[1] = col[1], p[2] = col[2], p[3] = 255;
}

static void text(const Canvas *cv, int x0, int y0, const char *s, const uint8_t col[3]) {
    for (int k = 0; s[k]; k++, x0 += CELL_W) {
        const uint8_t *g = glyph(s[k]);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] >> (4 - c) & 1) put(cv, x0 + c, y0 + r, col);
    }
}

static const uint8_t WHITE[3] = {235, 235, 235}, GREY[3] = {90, 90, 100}, GREEN[3] = {80, 230, 110},
                     YELLOW[3] = {230, 190, 60}, BOX[3] = {16, 16, 20}, PLOT[3] = {28, 28, 34};

void hud_render(uint8_t *rgba, int w, int h, const HudContent *c, int *used_w, int *used_h) {
    int cols = 0;
    for (int i = 0; i < c->nlines; i++) {
        int len = (int)strnlen(c->lines[i], HUD_COLS);
        if (len > cols) cols = len;
    }
    int graph = c->npoints > 1;
    int tw = cols * CELL_W - 1, gw = graph ? HUD_POINTS : 0;
    Canvas cv = {rgba, w, h, 2 * MARGIN + (tw > gw ? tw : gw), 0};
    cv.bh = 2 * MARGIN + c->nlines * CELL_H - 2 + (graph ? 3 + GRAPH_H + 3 + CELL_H - 2 : 0);
    if (cv.bw > w) cv.bw = w;
    if (cv.bh > h) cv.bh = h;
    for (int y = 0; y < cv.bh; y++)
        for (int x = 0; x < cv.bw; x++) put(&cv, x, y, BOX);
    for (int i = 0; i < c->nlines; i++) text(&cv, MARGIN, MARGIN + i * CELL_H, c->lines[i], WHITE);
    if (graph) {
        /* frame time (green line) over the game's work (yellow columns), newest on the right; the top is
         * the next 25 ms step above the slowest frame shown, at least 50 ms; a grey line at the target */
        int gx = MARGIN, gy = MARGIN + c->nlines * CELL_H - 2 + 3;
        float top = 50;
        for (int i = 0; i < c->npoints; i++)
            while (c->frame_ms[i] > top || c->work_ms[i] > top) top += 25;
        for (int y = 0; y < GRAPH_H; y++)
            for (int x = 0; x < HUD_POINTS; x++) put(&cv, gx + x, gy + y, PLOT);
        int x0 = gx + HUD_POINTS - c->npoints, last = -1;
#define Y(ms) (gy + GRAPH_H - 1 - (int)((ms) / top * (float)(GRAPH_H - 1) + 0.5f))
        for (int i = 0; i < c->npoints; i++)
            for (int y = Y(c->work_ms[i]); y < gy + GRAPH_H; y++) put(&cv, x0 + i, y, YELLOW);
        if (c->target_ms > 0)
            for (int x = 0; x < HUD_POINTS; x += 2) put(&cv, gx + x, Y(c->target_ms), GREY);
        for (int i = 0; i < c->npoints; i++) {  /* joined to the previous point, so spikes show as lines */
            int y = Y(c->frame_ms[i]);
            int a = last < 0 ? y : last < y ? last : y, b = last < 0 ? y : last < y ? y : last;
            for (int k = a; k <= b; k++) put(&cv, x0 + i, k, GREEN);
            last = y;
        }
#undef Y
        int ly = gy + GRAPH_H + 3;
        char s[24];
        text(&cv, gx, ly, "FRAME", GREEN);
        text(&cv, gx + 6 * CELL_W, ly, "WORK", YELLOW);
        snprintf(s, sizeof s, "TOP %.0f MS", top);
        text(&cv, gx + 11 * CELL_W, ly, s, GREY);
    }
    *used_w = cv.bw, *used_h = cv.bh;
}

static const uint8_t PANEL[3] = {20, 22, 30}, CHOSEN[3] = {230, 190, 60}, DARK[3] = {16, 16, 20};

void panel_render(uint8_t *rgba, int w, int h, const SettingRow *rows, int n, int selected, int *used_w,
                  int *used_h) {
    enum { NAME_COLS = 19, GAP = 2 };
    int cols = 0;
    for (int i = 0; i < n; i++) {
        int c = 2 + NAME_COLS;
        for (int k = 0; k < rows[i].noptions; k++) c += (int)strlen(rows[i].options[k]) + GAP;
        if (c > cols) cols = c;
    }
    static const char *const hint = "UP/DOWN: CHOOSE   LEFT/RIGHT: CHANGE   F1: CLOSE";
    if ((int)strlen(hint) > cols) cols = (int)strlen(hint);
    Canvas cv = {rgba, w, h, 2 * MARGIN + cols * CELL_W, 2 * MARGIN + (n + 3) * (CELL_H + 3)};
    if (cv.bw > w) cv.bw = w;
    if (cv.bh > h) cv.bh = h;
    for (int y = 0; y < cv.bh; y++)
        for (int x = 0; x < cv.bw; x++) put(&cv, x, y, PANEL);
    int y = MARGIN;
    text(&cv, MARGIN, y, "SETTINGS", WHITE);
    y += 2 * (CELL_H + 3);
    for (int i = 0; i < n; i++, y += CELL_H + 3) {
        const uint8_t *namecol = i == selected ? CHOSEN : WHITE;
        if (i == selected) text(&cv, MARGIN, y, ">", CHOSEN);
        text(&cv, MARGIN + 2 * CELL_W, y, rows[i].name, namecol);
        int x = MARGIN + (2 + NAME_COLS) * CELL_W;
        for (int k = 0; k < rows[i].noptions; k++) {
            int len = (int)strlen(rows[i].options[k]);
            if (k == rows[i].current) {  /* the current option on a box */
                for (int by = y - 2; by < y + 9; by++)
                    for (int bx = x - 2; bx < x + len * CELL_W + 1; bx++) put(&cv, bx, by, i == selected ? CHOSEN : WHITE);
                text(&cv, x, y, rows[i].options[k], DARK);
            } else {
                text(&cv, x, y, rows[i].options[k], GREY);
            }
            x += (len + GAP) * CELL_W;
        }
    }
    text(&cv, MARGIN, y + 3, hint, GREY);
    *used_w = cv.bw, *used_h = cv.bh;
}
