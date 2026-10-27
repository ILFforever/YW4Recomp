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
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}}, {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}}, {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    /* for key names on the controls page */
    {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}}, {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
    {';', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08}}, {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}}, {'\\', {0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00}},
    {'`', {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00}}, {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
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

/* ---- the settings panel, in the style of the game's own menus: a cream card with a dark outline and
 * rounded corners, a purple title bar, teal group labels, the chosen row on a yellow bar, the current
 * option on a pink chip, and a box explaining the chosen setting. Drawn at a small size; window.c scales
 * it up by whole steps with the window. */

static const uint8_t CREAM[3] = {252, 246, 232}, OUTLINE[3] = {64, 40, 82}, PURPLE[3] = {104, 54, 150},
                     PURPLE2[3] = {136, 78, 186}, TEAL[3] = {22, 146, 158}, ROWSEL[3] = {255, 229, 120},
                     INK[3] = {58, 44, 72}, MUTED[3] = {165, 154, 172}, PINK[3] = {226, 66, 136},
                     ORANGE[3] = {240, 140, 24}, DESC[3] = {226, 244, 240}, WHITE2[3] = {255, 255, 255};

static void fill(const Canvas *cv, int x, int y, int w, int h, const uint8_t col[3]) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(cv, i, j, col);
}

/* A box with corners rounded by r and, if `line`, a one-texel outline. */
static void box(const Canvas *cv, int x, int y, int w, int h, int r, const uint8_t col[3], const uint8_t *line) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int dx = i < r ? r - i : i >= w - r ? i - (w - 1 - r) : 0;
            int dy = j < r ? r - j : j >= h - r ? j - (h - 1 - r) : 0;
            int d2 = dx * dx + dy * dy, r2 = r * r;
            if (d2 > r2 + r) continue;  /* outside the rounded corner */
            int edge = i == 0 || j == 0 || i == w - 1 || j == h - 1 || d2 > r2 - r;
            put(cv, x + i, y + j, line && edge ? line : col);
        }
}

/* Text twice the size (the title). */
static void text2(const Canvas *cv, int x0, int y0, const char *s, const uint8_t col[3]) {
    for (int k = 0; s[k]; k++, x0 += 2 * CELL_W) {
        const uint8_t *g = glyph(s[k]);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] >> (4 - c) & 1) fill(cv, x0 + 2 * c, y0 + 2 * r, 2, 2, col);
    }
}

/* s broken into lines of at most `cols` characters at spaces; returns the count (at most max). */
static int wrap(const char *s, int cols, char lines[][96], int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        int len = (int)strlen(s), cut = len;
        if (len > cols) {
            cut = cols;
            while (cut > 0 && s[cut] != ' ') cut--;
            if (!cut) cut = cols;
        }
        if (cut > 95) cut = 95;
        memcpy(lines[n], s, (size_t)cut), lines[n][cut] = 0;
        n++, s += cut;
    }
    return n;
}

/* The card both pages are drawn on: a square card in a dark frame (the window copies overlays without
 * blending, so nothing can be see-through, and rounded corners inside a square frame looked off), its
 * title bar square too, with the page's name. */
enum { CARD_PAD = 10, CARD_HEADER = 26 };

static void card(const Canvas *cv, int pw, int ph, int page) {
    fill(cv, 0, 0, pw, ph, OUTLINE);
    fill(cv, 2, 2, pw - 4, ph - 4, CREAM);
    fill(cv, 2, 2, pw - 4, CARD_HEADER - 2, PURPLE);
    for (int x = 2; x < pw - 2; x += 4) put(cv, x, CARD_HEADER - 1, PURPLE2);
    text2(cv, CARD_PAD + 2, 6, page ? "CONTROLS" : "SETTINGS", WHITE2);
    text(cv, pw - CARD_PAD - 13 * CELL_W, 10, page ? "F2 / B: CLOSE" : "F1 / B: CLOSE", ROWSEL);
}

static void hint(const Canvas *cv, int pw, int y, const char *s) {
    text(cv, (pw - (int)strlen(s) * CELL_W) / 2, y + 2, s, MUTED);
}

void panel_render(uint8_t *rgba, int w, int h, const SettingRow *rows, int n, int selected, int *used_w,
                  int *used_h) {
    enum { PAD = 10, ROW_H = 13, NAME_COLS = 21, GAP = 3, HEADER = 26, DESC_LINES = 5 };
    /* the width: the widest row (name and all its options) */
    int cols = 0;
    for (int i = 0; i < n; i++) {
        int c = 2 + NAME_COLS + 2;
        for (int k = 0; k < rows[i].noptions; k++) c += (int)strlen(rows[i].options[k]) + GAP;
        if (c > cols) cols = c;
    }
    int pw = 2 * PAD + cols * CELL_W, text_cols = (pw - 2 * PAD - 12) / CELL_W;
    /* the height: title, rows with their group labels, the description box sized for the longest text (so the
     * card doesn't change size while moving), the key hints */
    int groups = 0;
    for (int i = 0; i < n; i++) groups += !i || strcmp(rows[i].category, rows[i - 1].category);
    int help_lines = 0;
    for (int i = 0; i < n; i++) {
        char l[DESC_LINES][96];
        int k = wrap(rows[i].help, text_cols, l, DESC_LINES);
        if (k > help_lines) help_lines = k;
    }
    int desc_h = (help_lines + 2) * 10 + 10;
    int ph = HEADER + 8 + (n + groups) * ROW_H + 6 + desc_h + 6 + 14;
    Canvas cv = {rgba, w, h, pw < w ? pw : w, ph < h ? ph : h};
    card(&cv, pw, ph, 0);
    /* rows */
    int y = HEADER + 8;
    int optx = PAD + (2 + NAME_COLS) * CELL_W;
    for (int i = 0; i < n; i++) {
        if (!i || strcmp(rows[i].category, rows[i - 1].category)) {
            text(&cv, PAD, y + 2, rows[i].category, TEAL);
            int lx = PAD + ((int)strlen(rows[i].category) + 1) * CELL_W;
            fill(&cv, lx, y + 5, pw - PAD - lx, 1, TEAL);
            y += ROW_H;
        }
        int sel = i == selected;
        if (sel) box(&cv, PAD - 4, y - 1, pw - 2 * PAD + 8, ROW_H, 4, ROWSEL, NULL);
        text(&cv, PAD + 2 * CELL_W, y + 2, rows[i].name, INK);
        if (sel) text(&cv, PAD + 2, y + 2, ">", PINK), text(&cv, optx - 2 * CELL_W, y + 2, "<", PINK);
        int x = optx;
        for (int k = 0; k < rows[i].noptions; k++) {
            int len = (int)strlen(rows[i].options[k]);
            if (k == rows[i].current) {
                box(&cv, x - 3, y, len * CELL_W + 5, ROW_H - 2, 3, sel ? ORANGE : PINK, NULL);
                text(&cv, x, y + 2, rows[i].options[k], WHITE2);
            } else {
                text(&cv, x, y + 2, rows[i].options[k], sel ? INK : MUTED);
            }
            x += (len + GAP) * CELL_W;
        }
        if (sel) text(&cv, x - (GAP - 1) * CELL_W, y + 2, ">", PINK);
        y += ROW_H;
    }
    /* the chosen setting explained: its name, what it does, what the current option means */
    y += 6;
    const SettingRow *r = selected >= 0 && selected < n ? &rows[selected] : NULL;
    box(&cv, PAD - 4, y, pw - 2 * PAD + 8, desc_h, 5, DESC, TEAL);
    if (r) {
        int ty = y + 6;
        text(&cv, PAD + 2, ty, r->name, TEAL);
        ty += 10;
        char l[DESC_LINES][96];
        int k = wrap(r->help, text_cols, l, DESC_LINES);
        for (int j = 0; j < k; j++, ty += 10) text(&cv, PAD + 2, ty, l[j], INK);
        char note[128];
        snprintf(note, sizeof note, "%s: %s", r->options[r->current], r->notes[r->current] ? r->notes[r->current] : "");
        text(&cv, PAD + 2, y + desc_h - 14, note, PINK);
    }
    y += desc_h + 6;
    hint(&cv, pw, y, "UP/DOWN: CHOOSE   LEFT/RIGHT: CHANGE   F2, L/R: CONTROLS");
    *used_w = cv.bw, *used_h = cv.bh;
}

static int same_group(const BindRow *a, const BindRow *b) {
    return a->group && b->group ? !strcmp(a->group, b->group) : a->group == b->group;
}

/* The controls page: a row per Switch input in groups, its two keys and gamepad input in columns, the
 * chosen cell on a chip (pink, orange when its key also presses another input), and the explanation box. */
void controls_render(uint8_t *rgba, int w, int h, const BindRow *rows, int n, int row, int col, int capturing,
                     const char *help, int *used_w, int *used_h) {
    enum { PAD = CARD_PAD, ROW_H = 13, NAME_COLS = 19, CELL_COLS = 16, DESC_LINES = 4, SHOWN = 13, LINES = 64 };
    int pw = 2 * PAD + (2 + NAME_COLS + BIND_COLS * CELL_COLS) * CELL_W, text_cols = (pw - 2 * PAD - 12) / CELL_W;
    /* the lines: a group's label, then its rows; SHOWN of them at a time (so the card is as tall as the
     * settings page and scales up the same), scrolled to keep the chosen row in view with a line around it */
    int line_row[LINES], nlines = 0, sel_line = 0;
    for (int i = 0; i < n && nlines + 2 <= LINES; i++) {
        if (!i || !same_group(&rows[i], &rows[i - 1])) line_row[nlines++] = -1 - i;  /* label of i's group */
        if (i == row) sel_line = nlines;
        line_row[nlines++] = i;
    }
    static int first;
    if (sel_line - 1 < first) first = sel_line - 1;
    if (sel_line + 2 > first + SHOWN) first = sel_line + 2 - SHOWN;
    if (first > nlines - SHOWN) first = nlines - SHOWN;
    if (first < 0) first = 0;
    int desc_h = DESC_LINES * 10 + 12;
    int ph = CARD_HEADER + 8 + ROW_H + SHOWN * ROW_H + 6 + desc_h + 6 + 14;
    Canvas cv = {rgba, w, h, pw < w ? pw : w, ph < h ? ph : h};
    card(&cv, pw, ph, 1);
    int y = CARD_HEADER + 8, cellx = PAD + (2 + NAME_COLS) * CELL_W;
    static const char *const heads[BIND_COLS] = {"KEY", "OTHER KEY", "GAMEPAD"};
    for (int c = 0; c < BIND_COLS; c++) text(&cv, cellx + c * CELL_COLS * CELL_W, y + 2, heads[c], TEAL);
    /* more above / below: small teal triangles at the right edge */
    for (int k = 0; k < 4; k++) {
        if (first > 0) fill(&cv, pw - PAD - 4 - k, y + 2 + k, 2 * k + 1, 1, TEAL);
        if (first + SHOWN < nlines) fill(&cv, pw - PAD - 4 - k, y + ROW_H * (SHOWN + 1) + 3 - k, 2 * k + 1, 1, TEAL);
    }
    y += ROW_H;
    for (int li = first; li < nlines && li < first + SHOWN; li++, y += ROW_H) {
        if (line_row[li] < 0) {  /* the group's label and line (none above the reset row) */
            const BindRow *r = &rows[-1 - line_row[li]];
            const char *g = r->group ? r->group : "";
            text(&cv, PAD, y + 3, g, TEAL);
            int lx = PAD + ((int)strlen(g) + (*g ? 1 : 0)) * CELL_W;
            fill(&cv, lx, y + 6, pw - PAD - lx, 1, TEAL);
            continue;
        }
        int i = line_row[li], sel = i == row;
        if (sel) box(&cv, PAD - 4, y - 1, pw - 2 * PAD + 8, ROW_H, 4, ROWSEL, NULL);
        if (sel) text(&cv, PAD + 2, y + 2, ">", PINK);
        text(&cv, PAD + 2 * CELL_W, y + 2, rows[i].name, INK);
        if (!rows[i].group) continue;
        for (int c = 0; c < BIND_COLS; c++) {
            char s[CELL_COLS];
            snprintf(s, sizeof s, "%s", sel && c == col && capturing ? (c < BIND_KEYS ? "PRESS A KEY" : "PRESS INPUT")
                                                                     : rows[i].cells[c]);
            int x = cellx + c * CELL_COLS * CELL_W;
            if (sel && c == col) {
                box(&cv, x - 3, y, (int)strlen(s) * CELL_W + 5, ROW_H - 2, 3,
                    rows[i].conflict[c] && !capturing ? ORANGE : PINK, NULL);
                text(&cv, x, y + 2, s, WHITE2);
            } else {
                text(&cv, x, y + 2, s, rows[i].conflict[c] ? ORANGE : !strcmp(s, "-") ? MUTED : INK);
            }
        }
    }
    y = CARD_HEADER + 8 + ROW_H + SHOWN * ROW_H;
    y += 6;
    box(&cv, PAD - 4, y, pw - 2 * PAD + 8, desc_h, 5, DESC, TEAL);
    char l[DESC_LINES][96];
    int k = wrap(help, text_cols, l, DESC_LINES);
    for (int j = 0; j < k; j++) text(&cv, PAD + 2, y + 6 + j * 10, l[j], capturing ? PINK : INK);
    y += desc_h + 6;
    hint(&cv, pw, y, "ARROWS: MOVE  ENTER/A: CHANGE  DEL/X: CLEAR  F1, L/R: SETTINGS");
    *used_w = cv.bw, *used_h = cv.bh;
}

