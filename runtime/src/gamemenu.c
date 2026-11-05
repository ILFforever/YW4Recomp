/* The in-game overlay's side panel (gamemenu.h): which item is chosen, what each does, and how it looks. Runs on
 * the window's thread (key and pad events); the present reads the state to draw it. The layout's numbers are the
 * design's (Main.dc.html on the overlay's design canvas), in its CSS pixels. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamemenu.h"
#include "bindings.h"
#include "settings.h"
#include "window.h"

static int open_, sel, confirm, choice;
static int focus;             /* 0 the side panel, 1 the page it shows (Settings) */
static int srow;              /* the chosen row on the settings page (settings_player_rows) */
static int prow;              /* the chosen row on the performance page: mode, corner, the four parts */
static uint32_t changes;      /* counts every change of what shows */
static int toast_wanted;      /* a screenshot was asked for: the note starts at the next look */
static uint64_t toast_until;  /* ms */

int gamemenu_open(void) { return __atomic_load_n(&open_, __ATOMIC_ACQUIRE); }
int gamemenu_selected(void) { return sel; }
int gamemenu_confirming(void) { return confirm; }

static void changed(void) { __atomic_add_fetch(&changes, 1, __ATOMIC_RELEASE); }

uint32_t gamemenu_look(uint64_t now) {
    if (__atomic_exchange_n(&toast_wanted, 0, __ATOMIC_ACQ_REL)) toast_until = now + 1600;
    /* also when a key capture ends by itself (five seconds) */
    return __atomic_load_n(&changes, __ATOMIC_ACQUIRE) * 4u + (now < toast_until) + 2u * (uint32_t)bindings_capturing();
}

static void close_and_resume(void) {
    if (settings_open()) settings_toggle();
    bindings_cancel();
    confirm = 0, focus = 0;
    __atomic_store_n(&open_, 0, __ATOMIC_RELEASE);
    if (window_paused()) window_pause_toggle();
    changed();
    fprintf(stderr, "yw4: overlay: closed\n");
}

void gamemenu_toggle(void) {
    if (gamemenu_open()) {
        close_and_resume();
        return;
    }
    sel = GM_RESUME, confirm = 0, focus = 0;
    if (!window_paused()) window_pause_toggle();  /* F4 may have paused it already */
    __atomic_store_n(&open_, 1, __ATOMIC_RELEASE);
    changed();
    fprintf(stderr, "yw4: overlay: open\n");
}

static void accept(void) {
    switch (sel) {
    case GM_RESUME: close_and_resume(); break;
    case GM_SETTINGS: focus = 1; break;  /* into its page */
    case GM_CONTROLS: focus = 1; break;
    case GM_PERF: focus = 1; break;
    case GM_SHOT: window_request_screenshot(), __atomic_store_n(&toast_wanted, 1, __ATOMIC_RELEASE); break;
    case GM_EXIT: confirm = 1, choice = 0; break;  /* "keep playing" chosen first */
    }
}

/* the settings page: rows run on from one section into the next; PREV / NEXT jump to a section's first row */
static void settings_page_action(int a) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    if (a == GM_UP) srow = (srow + n - 1) % n;
    else if (a == GM_DOWN) srow = (srow + 1) % n;
    else if (a == GM_LEFT || a == GM_RIGHT) settings_change(srow, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_BACK || a == GM_LEAVE) focus = 0;
    else if (a == GM_PREV || a == GM_NEXT) {
        int first = srow;  /* this section's first row */
        while (first > 0 && !strcmp(rows[first - 1].category, rows[srow].category)) first--;
        if (a == GM_PREV) {
            int r = first > 0 ? first - 1 : n - 1;  /* a row of the section before (the last wraps round) */
            while (r > 0 && !strcmp(rows[r - 1].category, rows[r].category)) r--;
            srow = r;
        } else {
            int r = srow;
            while (r < n && !strcmp(rows[r].category, rows[srow].category)) r++;
            srow = r < n ? r : 0;
        }
    }
}

/* the controls page: the controls' own selection and key capture (bindings.h), as the F2 page's */
static void controls_page_action(int a) {
    static const int map[] = {[GM_UP] = SETTINGS_UP, [GM_DOWN] = SETTINGS_DOWN, [GM_LEFT] = SETTINGS_LEFT,
                              [GM_RIGHT] = SETTINGS_RIGHT, [GM_ACCEPT] = SETTINGS_ACCEPT, [GM_CLEAR] = SETTINGS_CLEAR};
    if (a == GM_BACK || a == GM_LEAVE) {
        bindings_cancel();
        focus = 0;
    } else if (a == GM_UP || a == GM_DOWN || a == GM_LEFT || a == GM_RIGHT || a == GM_ACCEPT || a == GM_CLEAR) {
        bindings_action(map[a]);
    }
}

/* the performance page: its rows are the mode (Off / Compact / Detailed), the corner, then the detailed HUD's four
 * parts; Left / Right change the first two, A / Enter or Left / Right turn a part on or off */
static const int perf_modes[3] = {0, 2, 1};  /* the chips' order: Off, Compact, Detailed (window_hud_mode) */

static void perf_page_action(int a) {
    if (a == GM_BACK || a == GM_LEAVE) {
        focus = 0;
        return;
    }
    if (a == GM_UP) prow = (prow + 5) % 6;
    else if (a == GM_DOWN) prow = (prow + 1) % 6;
    else if (a == GM_LEFT || a == GM_RIGHT || a == GM_ACCEPT) {
        int d = a == GM_LEFT ? -1 : 1;
        if (prow == 0) {
            int m = window_hud_mode(), i = 0;
            while (i < 3 && perf_modes[i] != m) i++;
            window_hud_set_mode(perf_modes[((i < 3 ? i : 0) + d + 3) % 3]);
        } else if (prow == 1) {
            window_hud_set_corner((window_hud_corner() + d + 4) % 4);
        } else {
            window_hud_set_parts(window_hud_parts() ^ (1u << (prow - 2)));
        }
        settings_save();
    }
}

void gamemenu_action(int a) {
    if (!gamemenu_open() || settings_open()) return;
    changed();
    if (confirm) {
        if (a == GM_LEFT) choice = 0;
        else if (a == GM_RIGHT) choice = 1;
        else if (a == GM_UP || a == GM_DOWN) choice ^= 1;
        else if (a == GM_BACK || a == GM_LEAVE) confirm = 0;
        else if (a == GM_ACCEPT) {
            if (!choice) {
                confirm = 0;
                return;
            }
            fprintf(stderr, "yw4: overlay: exit game\n");
            exit(0);  /* as closing the window */
        }
        return;
    }
    if (focus) {
        if (sel == GM_CONTROLS) controls_page_action(a);
        else if (sel == GM_PERF) perf_page_action(a);
        else settings_page_action(a);
        return;
    }
    if (a == GM_UP) sel = (sel + GM_ITEMS - 1) % GM_ITEMS;
    else if (a == GM_DOWN) sel = (sel + 1) % GM_ITEMS;
    else if (a == GM_ACCEPT || (a == GM_RIGHT && (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_PERF)))
        accept();
    else if (a == GM_BACK) close_and_resume();
}

/* ---- the look (Main.dc.html) */

enum {
    INK = 0x2b2338, NIGHT = 0x2a1b4c, PLUM = 0x3e2a6e, PAPER = 0xf8f4fc, GOLD = 0xe3be5c, LINE = 0xe2d5ef,
    WHITE = 0xffffff, SNOW = 0xfdfcfe, LILAC = 0xd9cdea, MUTED = 0x605673, WINE = 0x8c1f3f, SHADE = 0x160e26,
};

#define GAME_VERSION "2.2.0"  /* the one build this port runs (CLAUDE.md: the final 4++ update) */

/* the icons' SVG path data, from the design */
static const char *const ICON_RESUME = "<path d=\"M8 5v14l11-7z\"/>";
static const char *const ICON_SETTINGS =
    "<path d=\"M4 7h9m4 0h3M4 17h3m4 0h9M14 7a2 2 0 1 0 4 0a2 2 0 1 0-4 0M7 17a2 2 0 1 0 4 0a2 2 0 1 0-4 0\"/>";
static const char *const ICON_CONTROLS =
    "<path d=\"M6 9h12a4 4 0 0 1 4 4v1a3 3 0 0 1-5.2 2L15 14H9l-1.8 2A3 3 0 0 1 2 14v-1a4 4 0 0 1 4-4zM8 11v4M6 13h4"
    "M16 12h.01M18 14h.01\"/>";
static const char *const ICON_PERF = "<path d=\"M3 20h18M5 16l4-5 4 3 6-8\"/>";
static const char *const ICON_SHOT = "<path d=\"M4 8h3l2-3h6l2 3h3v11H4zM12 17a3.5 3.5 0 1 0 0-7a3.5 3.5 0 1 0 0 7z\"/>";
static const char *const ICON_EXIT = "<path d=\"M12 3v9M6.3 6.3a8 8 0 1 0 11.4 0\"/>";
static const char *const ICON_CLOCK = "<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M12 7v5l3 2\"/>";

/* A side panel button (tabStyle): 56 high, radius 12, a 2 px border; the chosen one gold. */
static void side_button(const UiCanvas *c, float x, float y, float w, const char *icon, const char *label, int on,
                        uint32_t ink) {
    ui_box(c, x, y, w, 56, 12, on ? GOLD : WHITE, 2, on ? GOLD : LINE);
    uint32_t col = on && ink == PLUM ? NIGHT : ink;
    ui_icon(c, icon, x + 20, y + 16, 24, 2, col);
    ui_text_in(c, UI_ROUNDED_700, 18, x + 20 + 24 + 14, y, 0, 56, 0, label, col);
}

/* A key hint: the key on a round chip, then what it does (the bottom bar). Returns its width. */
static float hint(const UiCanvas *c, float x, float y, const char *key, const char *label) {
    float kw = ui_text_w(UI_GOTHIC_700, 14, key) + 16;
    if (kw < 30) kw = 30;
    ui_box(c, x, y + 13, kw, 30, 15, SNOW, 0, 0);
    ui_text_in(c, UI_GOTHIC_700, 14, x, y + 13, kw, 30, 1, key, NIGHT);
    float lw = ui_text_w(UI_GOTHIC_400, 16, label);
    ui_text_in(c, UI_GOTHIC_400, 16, x + kw + 10, y, 0, 56, 0, label, SNOW);
    return kw + 10 + lw;
}

/* A popup button (Keep playing / Quit game): 56 high, radius 14; the chosen one ringed in gold. */
static float popup_button(const UiCanvas *c, float right, float y, const char *label, int quit, int on) {
    int f = quit ? UI_ROUNDED_800 : UI_ROUNDED_700;
    float w = ui_text_w(f, 18, label) + 52 + (quit ? 0 : 4);
    float x = right - w;
    if (on) ui_box(c, x - 5, y - 5, w + 10, 66, 19, GOLD, 0, 0);
    if (quit) ui_box(c, x, y, w, 56, 14, WINE, 0, 0);
    else ui_box(c, x, y, w, 56, 14, WHITE, 2, LINE);
    ui_text_in(c, f, 18, x, y, w, 56, 1, label, quit ? SNOW : PLUM);
    return w;
}

/* The settings' texts are upper case (the F1 panel's pixel font has no lower case); the design writes names,
 * options and sections in sentence case and keeps acronyms: "WINDOW MODE" -> "Window mode", "X16" -> "x16". */
static void sentence_case(const char *in, char *out, size_t n) {
    static const char *const keep[] = {"HUD", "FXAA", "SMAA", "TAA", "FSR", "DLSS", "GPU", "GPUS", "CPU", "AMD",
                                       "NVIDIA", "RTX", "HD", "MS", "F3", "F11", "AI", "ZL", "ZR", "LB", "RB", "LT",
                                       "RT", "L1", "R1", "L2", "R2", "L3", "R3", "PS"};
    /* the F1 panel font's own symbols (bindings.c): arrows, and PlayStation's shapes by name */
    static const char *const sym[9] = {NULL, "\xe2\x86\x91", "\xe2\x86\x93", "\xe2\x86\x90", "\xe2\x86\x92",
                                       "Triangle", "Circle", "Square", "Cross"};
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n;) {
        if (*p >= 1 && *p <= 8) {
            for (const char *q = sym[(int)*p]; *q && o + 1 < n; q++) out[o++] = *q;
            p++;
            continue;
        }
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {  /* a word */
            const char *e = p;
            while ((*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9')) e++;
            size_t len = (size_t)(e - p);
            int acr = 0;
            for (size_t k = 0; k < sizeof keep / sizeof *keep; k++)
                if (strlen(keep[k]) == len && !strncmp(keep[k], p, len)) acr = 1;
            int times = *p == 'X' && p[1] >= '0' && p[1] <= '9';  /* "X16": a multiplier, lower case */
            for (const char *q = p; q < e && o + 1 < n; q++)
                out[o++] = acr || (q == in && !times) ? *q : (char)(*q >= 'A' && *q <= 'Z' ? *q + 32 : *q);
            p = e;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
}

/* The settings page (the design's isSettings section): the sections on the left (the chosen row's lit), its
 * section's rows with the value, its note and arrows, the chosen row's description at the bottom. */
static void draw_settings_page(const UiCanvas *c) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    int cur = srow < n ? srow : n - 1;
    const char *sec = rows[cur].category;
    char t[160];
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Settings", NIGHT);
    float top = 76 + 34 * 1.25f + 22, bottom = 40 + 764 - 36;
    /* the sections */
    float y = top;
    for (int i = 0; i < n; i++) {
        if (i && !strcmp(rows[i].category, rows[i - 1].category)) continue;
        int on = !strcmp(rows[i].category, sec);
        ui_box(c, 488, y, 300, 52, 12, on ? PLUM : WHITE, 2, on ? PLUM : LINE);
        sentence_case(rows[i].category, t, sizeof t);
        ui_text_wrap(c, UI_GOTHIC_700, 17, 0, 488 + 16, y + 26 - 17 * 1.45f / 2, 300 - 32, 17 * 1.45f, 0, t,
                     on ? SNOW : PLUM);
        y += 52 + 8;
    }
    /* its rows */
    float x = 812, w = 700;
    y = top;
    for (int i = 0; i < n; i++) {
        if (strcmp(rows[i].category, sec)) continue;
        const SettingRow *r = &rows[i];
        int on = focus && i == cur, off = r->disabled != NULL;
        ui_box(c, x, y, w, 72, 14, WHITE, on ? 3 : 2, on ? GOLD : LINE);
        sentence_case(r->name, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_500, 18, x + 20, y, 0, 72, 0, t, off ? MUTED : INK);
        float nx = x + w - 12 - 44, vx = nx - 10 - 300, px = vx - 10 - 44, by = y + 14;
        ui_box(c, px, by, 44, 44, 10, 0xefe7f7, 2, LINE);
        ui_text_in(c, UI_GOTHIC_500, 22, px, by, 44, 44, 1, "\xe2\x80\xb9", off ? MUTED : PLUM);  /* ‹ */
        ui_box(c, nx, by, 44, 44, 10, 0xefe7f7, 2, LINE);
        ui_text_in(c, UI_GOTHIC_500, 22, nx, by, 44, 44, 1, "\xe2\x80\xba", off ? MUTED : PLUM);  /* › */
        const char *value = r->options[r->current] ? r->options[r->current] : "";
        const char *note = r->notes[r->current] ? r->notes[r->current] : "";
        sentence_case(value, t, sizeof t);
        float vh = 18 * 1.45f, nh = *note ? ui_text_wrap(NULL, UI_GOTHIC_700, 12, 0.04f, 0, 0, 300, 12 * 1.3f, 1, note, 0) : 0;
        float vy = y + (72 - (vh + (nh ? 2 + nh : 0))) / 2;
        ui_text_in(c, UI_ROUNDED_700, 18, vx, vy, 300, vh, 1, t, off ? MUTED : NIGHT);
        if (nh) ui_text_wrap(c, UI_GOTHIC_700, 12, 0.04f, vx, vy + vh + 2, 300, 12 * 1.3f, 1, note, MUTED);
        y += 72 + 10;
    }
    /* the chosen row's description */
    const SettingRow *r = &rows[cur];
    char desc[600];
    if (r->disabled) snprintf(desc, sizeof desc, "%s OFF FOR NOW: %s", r->help, r->disabled);
    else snprintf(desc, sizeof desc, "%s", r->help);
    float dh = ui_text_wrap(NULL, UI_GOTHIC_700, 14, 0.04f, 0, 0, w - 40, 14 * 1.55f, 0, desc, 0) + 32;
    ui_box(c, x, bottom - dh, w, dh, 14, NIGHT, 0, 0);
    ui_text_wrap(c, UI_GOTHIC_700, 14, 0.04f, x + 20, bottom - dh + 16, w - 40, 14 * 1.55f, 0, desc, SNOW);
}

/* The controls page (the design's isControls section): a table of each Switch input with its two keys and its
 * controller input, the chosen row ringed in gold and the chosen key or input on a gold chip; the rows scroll.
 * Under it, what the chosen row does (the design's note line). */
static void draw_controls_page(const UiCanvas *c, const GameMenuInfo *in) {
    static BindRow rows[64];
    char help[512], t[160], u[160];
    int row, col, n = bindings_rows(rows, 64, &row, &col, help, sizeof help);
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Controls", NIGHT);
    if (in->pad_name) snprintf(t, sizeof t, "%s connected", in->pad_name);
    else snprintf(t, sizeof t, "No controller connected");
    ui_text_in(c, UI_GOTHIC_400, 16, 488, 76 + 12, 1024, 34 * 1.25f - 12, 2, t, MUTED);
    float top = 76 + 34 * 1.25f + 22, bottom = 40 + 764 - 36;
    float hh = ui_text_wrap(NULL, UI_GOTHIC_400, 15, 0, 0, 0, 1024, 15 * 1.45f, 0, help, 0);
    float x = 488, w = 1024, cw[3] = {379, 379, 266}, hdr = 16 * 1.45f + 28, rh = 17 * 1.45f + 24;
    int visible = (int)((bottom - hh - 22 - top - hdr - 4) / rh);
    if (visible < 1) visible = 1;
    float th = hdr + visible * rh + 4;
    int first = row - visible / 2;
    if (first > n - visible) first = n - visible;
    if (first < 0) first = 0;
    /* the table: a white box, its header row lilac */
    ui_box(c, x, top, w, th, 16, WHITE, 2, LINE);
    ui_box(c, x + 2, top + 2, w - 4, hdr, 14, 0xefe7f7, 0, 0);
    ui_rect(c, x + 2, top + 2 + hdr - 14, w - 4, 14, 0xefe7f7, 1);
    static const char *const heads[3] = {"Switch button", "Keyboard", "Controller"};
    float cx = x;
    for (int k = 0; k < 3; k++) ui_text_in(c, UI_GOTHIC_700, 16, cx + 22, top + 2, 0, hdr, 0, heads[k], PLUM), cx += cw[k];
    float y = top + 2 + hdr;
    for (int i = first; i < n && i < first + visible; i++, y += rh) {
        const BindRow *r = &rows[i];
        int on = focus && i == row;
        ui_rect(c, x + 2, y, w - 4, 2, 0xefe7f7, 1);  /* the line between rows */
        if (on) {  /* a gold ring */
            ui_box(c, x + 4, y + 2, w - 8, rh - 2, 10, GOLD, 0, 0);
            ui_box(c, x + 7, y + 5, w - 14, rh - 8, 7, WHITE, 0, 0);
        }
        /* the Switch input's name */
        if (!r->group) snprintf(u, sizeof u, "%s", r->name);  /* the reset row */
        else if (!strcmp(r->group, "BUTTONS")) snprintf(u, sizeof u, "%s", r->name);
        else if (!strcmp(r->group, "KEYS FOR")) snprintf(u, sizeof u, "KEYS FOR");
        else snprintf(u, sizeof u, "%s %s", r->group, r->name);
        sentence_case(u, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_400, 17, x + 22, y, 0, rh, 0, t, INK);
        if (!r->group) continue;
        if (r->value) {  /* a setting: its value in the keyboard column */
            sentence_case(r->cells[0], t, sizeof t);
            ui_text_in(c, UI_GOTHIC_500, 17, x + cw[0] + 22, y, 0, rh, 0, t, NIGHT);
            if (on) ui_text_in(c, UI_GOTHIC_500, 17, x + cw[0] + 22 + ui_text_w(UI_GOTHIC_500, 17, t) + 12, y, 0, rh, 0,
                               "\xe2\x80\xb9 \xe2\x80\xba", PLUM);  /* ‹ ›: Left / Right change it */
            continue;
        }
        /* the two keys, then the controller input: each a chip when chosen */
        for (int k = 0; k < 3; k++) {
            float kx = k < 2 ? x + cw[0] + 22 : x + cw[0] + cw[1] + 22;
            if (k == 1) {  /* after the first key: "E, Enter" */
                char k0[64];
                sentence_case(r->cells[0], k0, sizeof k0);
                kx += ui_text_w(UI_GOTHIC_500, 17, k0);
                ui_text_in(c, UI_GOTHIC_500, 17, kx, y, 0, rh, 0, ",", MUTED);
                kx += ui_text_w(UI_GOTHIC_500, 17, ", ") + 6;
            }
            const char *cell = r->cells[k];
            if (on && col == k && bindings_capturing()) cell = "PRESS...";
            sentence_case(cell, t, sizeof t);
            float tw = ui_text_w(UI_GOTHIC_500, 17, t);
            if (on && col == k) ui_box(c, kx - 10, y + 8, tw + 20, rh - 16, 8, GOLD, 0, 0);
            ui_text_in(c, UI_GOTHIC_500, 17, kx, y, 0, rh, 0, t,
                       r->conflict[k] ? WINE : !strcmp(r->cells[k], "-") ? MUTED : INK);
        }
    }
    ui_text_wrap(c, UI_GOTHIC_400, 15, 0, x, top + th + 22, w, 15 * 1.45f, 0, help, MUTED);
}

/* A choice chip (the design's chipStyle): the chosen one filled; ringed in gold where the focus is. */
static void chip(const UiCanvas *c, float x, float y, float w, const char *label, int on, int ring) {
    if (ring) ui_box(c, x - 4, y - 4, w + 8, 60, 15, GOLD, 0, 0);
    ui_box(c, x, y, w, 52, 12, on ? PLUM : WHITE, 2, on ? PLUM : LINE);
    ui_text_in(c, UI_GOTHIC_700, 17, x, y, w, 52, 1, label, on ? SNOW : PLUM);
}

/* The performance page (the design's isPerf section): how the HUD shows while playing, its corner, the detailed
 * one's parts, and a preview of the HUD as it is now (live, where the design shows example values). */
static void draw_perf_page(const UiCanvas *c) {
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Performance overlay", NIGHT);
    float y = 76 + 34 * 1.25f + 24, x = 488, lh = 16 * 1.45f;
    int mode = window_hud_mode(), corner = window_hud_corner();
    unsigned parts = window_hud_parts();
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "Show while playing", PLUM);
    y += lh + 10;
    static const char *const modes[3] = {"Off", "Compact", "Detailed"};
    for (int i = 0; i < 3; i++) {
        float w = ui_text_w(UI_GOTHIC_700, 17, modes[i]) + 40;
        if (w < 130) w = 130;
        chip(c, x, y, w, modes[i], perf_modes[i] == mode, focus && prow == 0 && perf_modes[i] == mode);
        x += w + 10;
    }
    x = 488, y += 52 + 22;
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "Corner", PLUM);
    y += lh + 10;
    static const char *const corners[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    for (int i = 0; i < 4; i++)
        chip(c, x + (i & 1) * 215, y + (i >> 1) * 62, 205, corners[i], i == corner, focus && prow == 1 && i == corner);
    y += 2 * 52 + 10 + 22;
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "In the detailed overlay", PLUM);
    y += lh + 10;
    static const char *const names[4] = {"FPS and 1% low", "Frame time graph", "GPU and CPU time", "Frame generation"};
    float lw = 1024 - 360 - 32;
    for (int i = 0; i < 4; i++, y += 52 + 10) {
        int on = parts >> i & 1, ring = focus && prow == 2 + i, off = mode != 1;
        ui_box(c, x, y, lw, 52, 12, WHITE, ring ? 3 : 2, ring ? GOLD : LINE);
        ui_box(c, x + 18, y + 13, 26, 26, 7, on ? PLUM : WHITE, 2, on ? PLUM : 0xc6b2da);
        if (on) ui_text_in(c, UI_ROUNDED_800, 16, x + 18, y + 13, 26, 26, 1, "\xe2\x9c\x93", SNOW);  /* ✓ */
        ui_text_in(c, UI_GOTHIC_400, 17, x + 18 + 26 + 14, y, 0, 52, 0, names[i], off ? MUTED : INK);
    }
    /* the preview: the HUD itself, drawn now */
    float px = 488 + lw + 32, py = 76 + 34 * 1.25f + 24;
    ui_text(c, UI_GOTHIC_700, 16, px, py, lh, "Preview", PLUM);
    py += lh + 10;
    static uint8_t img[512 * 512 * 4], tight[512 * 512 * 4];
    int iw = 0, ih = 0;
    if (mode) window_hud_preview(img, 512, 512, &iw, &ih);
    float bh = 120;
    if (iw > 0 && ih > 0) {
        for (int j = 0; j < ih; j++) memcpy(tight + (size_t)j * iw * 4, img + (size_t)j * 512 * 4, (size_t)iw * 4);
        float sw = 320, sh = sw * (float)ih / (float)iw;
        if (sh > 420) sh = 420, sw = sh * (float)iw / (float)ih;
        bh = sh + 36;
        ui_box(c, px, py, 360, bh, 16, NIGHT, 0, 0);
        ui_image(c, tight, iw, ih, px + 20, py + 18, sw, sh, 0);
    } else {
        ui_box(c, px, py, 360, bh, 16, NIGHT, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 16, px, py, 360, bh, 1, "Off while playing", LILAC);
    }
    ui_text(c, UI_GOTHIC_400, 14, px, py + bh + 10, 14 * 1.45f, "Live values, as the HUD shows them now.", MUTED);
}

void gamemenu_draw(const UiCanvas *c, const GameMenuInfo *in) {
    float W = (float)c->w / c->s, H = 900;
    int page = (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_PERF) && !confirm;  /* these show a page */
    ui_rect(c, 0, 0, W, H, SHADE, page ? 0.62f : 0.35f);  /* the tint over the game, darker behind a page */
    if (page && sel == GM_SETTINGS) draw_settings_page(c);
    if (page && sel == GM_CONTROLS) draw_controls_page(c, in);
    if (page && sel == GM_PERF) draw_perf_page(c);

    /* the side panel: 400 wide with a 4 px gold edge towards the game */
    ui_rect(c, 0, 0, 400, H, PAPER, 1);
    ui_rect(c, 396, 0, 4, H, GOLD, 1);
    ui_rect(c, 0, 0, 396, 108, NIGHT, 1);  /* the header: padding 28 / 24 around the 56 px logo */
    if (in->logo) {  /* the game's own icon (gamelogo.c), else the design's clock */
        ui_image(c, in->logo, in->logo_w, in->logo_h, 28, 28, 56, 56, 14);
    } else {
        ui_box(c, 28, 28, 56, 56, 14, GOLD, 0, 0);
        ui_icon(c, ICON_CLOCK, 28 + 13, 28 + 13, 30, 2.2f, NIGHT);
    }
    float th = 21 * 1.2f, sh = 14 * 1.45f, ty = 28 + (56 - (th + 4 + sh)) / 2;
    ui_text(c, UI_ROUNDED_800, 21, 100, ty, th, "Yo-kai Watch 4 Recomp", SNOW);
    ui_text(c, UI_GOTHIC_400, 14, 100, ty + th + 4, sh, "Game version " GAME_VERSION, LILAC);

    static const char *const labels[] = {"Resume", "Settings", "Controls", "Performance", "Take screenshot"};
    const char *icons[] = {ICON_RESUME, ICON_SETTINGS, ICON_CONTROLS, ICON_PERF, ICON_SHOT};
    for (int i = 0; i < 5; i++) {
        const char *l = labels[i];
        side_button(c, 20, 128 + i * 66, 356, icons[i], l, sel == i, PLUM);
    }
    /* Exit game at the bottom, under a line; the version under it */
    float vy = H - 20 - 19, ey = vy - 14 - 56;
    ui_rect(c, 20, ey - 14 - 2, 356, 2, LINE, 1);
    side_button(c, 20, ey, 356, ICON_EXIT, "Exit game", sel == GM_EXIT, WINE);
    char ver[48];
    snprintf(ver, sizeof ver, "YW4 Recomp %s", in->version);
    ui_text(c, UI_GOTHIC_400, 13, 20, vy, 19, ver, MUTED);

    /* the key hints along the bottom */
    ui_rect(c, 400, H - 56, W - 400, 56, NIGHT, 1);
    float hx = 448;
    if (focus && sel == GM_CONTROLS) {  /* the controls page */
        int ps = in->pad == 2;
        hx += hint(c, hx, H - 56, in->pad ? "D-pad" : "W S", "Choose") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Cross" : "A") : "Enter", "Change") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Square" : "X") : "Del", "Clear") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Circle" : "B") : "A", "Back") + 28;
        hint(c, hx, H - 56, in->pad ? in->close : "Esc", "Close overlay");
    } else if (focus && in->pad) {  /* on a page */
        hx += hint(c, hx, H - 56, "D-pad", "Choose and change") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, H - 56, "LB RB", "Section") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Back") + 28;
        hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus) {
        hx += hint(c, hx, H - 56, "W S", "Move") + 28;
        hx += hint(c, hx, H - 56, "\xe2\x86\x90 \xe2\x86\x92", "Change") + 28;  /* ← → */
        if (sel == GM_SETTINGS) hx += hint(c, hx, H - 56, "Q E", "Section") + 28;
        hx += hint(c, hx, H - 56, "A", "Back") + 28;
        hint(c, hx, H - 56, "Esc", "Close overlay");
    } else if (in->pad) {
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Back") + 28;
        hint(c, hx, H - 56, in->close, "Close overlay");
    } else {
        hx += hint(c, hx, H - 56, "W S", "Move") + 28;
        hx += hint(c, hx, H - 56, "Enter", "Select") + 28;
        hint(c, hx, H - 56, "Esc", "Close overlay");
    }

    /* the note after a screenshot: top right */
    if (in->now < toast_until) {
        const char *t = "Screenshot saved";
        float tw = ui_text_w(UI_GOTHIC_700, 17, t), bw = tw + 44 + 6, bh = 17 * 1.45f + 28 + 6;
        ui_box(c, W - 48 - bw, 40, bw, bh, 14, SNOW, 3, GOLD);
        ui_text_in(c, UI_GOTHIC_700, 17, W - 48 - bw, 40, bw, bh, 1, t, NIGHT);
    }

    /* the exit popup: 560 wide in the middle, a gold top edge */
    if (confirm) {
        ui_rect(c, 0, 0, W, H, SHADE, 0.55f);
        float pw = 560, ph = 32 + 28 * 1.25f + 16 + 17 * 1.5f + 16 + 8 + 56 + 32;
        float px = (W - pw) / 2, py = (H - ph) / 2;
        ui_box(c, px, py, pw, ph, 20, GOLD, 0, 0);
        ui_box(c, px, py + 6, pw, ph - 6, 20, PAPER, 0, 0);
        float y = py + 6 + 32;
        ui_text(c, UI_ROUNDED_800, 28, px + 36, y, 28 * 1.25f, "Quit the game?", NIGHT);
        y += 28 * 1.25f + 16;
        ui_text(c, UI_GOTHIC_400, 17, px + 36, y, 17 * 1.5f, "Progress since your last save is lost.", INK);
        y += 17 * 1.5f + 16 + 8;
        float right = px + pw - 36;
        right -= popup_button(c, right, y, "Quit game", 1, choice == 1) + 12;
        popup_button(c, right, y, "Keep playing", 0, choice == 0);
    }
}
