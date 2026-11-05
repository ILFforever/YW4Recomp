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
static int in_rows;           /* the settings page: 0 the sections list has the focus, 1 the chosen section's rows */
static int prow;              /* the chosen row on the performance page: mode, corner, the four parts */
static uint32_t changes;      /* counts every change of what shows */
static int toast_wanted;      /* the note (Report a bug: coming soon) starts at the next look */
static uint64_t toast_until;  /* ms */
static int hover_kind = -1, hover_a, hover_b;  /* what the mouse is over (HIT_*, -1 nothing), for the hover looks */

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
    case GM_SETTINGS: focus = 1, in_rows = 0; break;  /* into its page: its sections first */
    case GM_CONTROLS: focus = 1; break;
    case GM_PERF: focus = 1; break;
    case GM_BUG: __atomic_store_n(&toast_wanted, 1, __ATOMIC_RELEASE); break;  /* not built yet: says so */
    case GM_EXIT: confirm = 1, choice = 0; break;  /* "keep playing" chosen first */
    }
}

/* the settings page: rows run on from one section into the next; PREV / NEXT jump to a section's first row */
static void settings_page_action(int a) {
    SettingRow rows[32];
    int n = settings_player_rows(rows, 32);
    if (n <= 0) return;
    if (srow >= n) srow = n - 1;
    int first = srow;  /* this section's first row */
    while (first > 0 && !strcmp(rows[first - 1].category, rows[srow].category)) first--;
    if (!in_rows) {  /* the sections: Up / Down choose one, Right / Enter go into its rows, Back to the side panel */
        if (a == GM_UP || a == GM_PREV) a = GM_PREV;
        else if (a == GM_DOWN || a == GM_NEXT) a = GM_NEXT;
        else if (a == GM_RIGHT || a == GM_ACCEPT) {
            in_rows = 1;
            return;
        } else if (a == GM_BACK || a == GM_LEAVE) {
            focus = 0;
            return;
        } else return;
    }
    /* the rows: the ends stop the choice (no wrapping round), so holding a direction stops at the last row */
    if (a == GM_UP) srow = srow > 0 ? srow - 1 : 0;
    else if (a == GM_DOWN) srow = srow < n - 1 ? srow + 1 : n - 1;
    else if (a == GM_LEFT || a == GM_RIGHT) settings_change(srow, a == GM_RIGHT ? 1 : -1);
    else if (a == GM_ACCEPT) settings_change(srow, 1);  /* Enter / A: the next option, as Right */
    else if (a == GM_BACK || a == GM_LEAVE) in_rows = 0, srow = first;  /* back to the sections */
    else if (a == GM_PREV || a == GM_NEXT) {
        if (a == GM_PREV) {
            if (first == 0) return;  /* the first section: stays */
            int r = first - 1;  /* a row of the section before: to its first */
            while (r > 0 && !strcmp(rows[r - 1].category, rows[r].category)) r--;
            srow = r;
        } else {
            int r = srow;
            while (r < n && !strcmp(rows[r].category, rows[srow].category)) r++;
            if (r < n) srow = r;  /* the last section: stays */
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

/* the performance page: its rows are the mode (Off / Compact / Detailed), the corner, the detailed HUD's four parts,
 * then the scale and opacity sliders; Left / Right change them, A / Enter or Left / Right turn a part on or off */
static const int perf_modes[3] = {0, 2, 1};  /* the chips' order: Off, Compact, Detailed (window_hud_mode) */

static void perf_page_action(int a) {
    if (a == GM_BACK || a == GM_LEAVE) {
        focus = 0;
        return;
    }
    if (a == GM_PREV || a == GM_NEXT) return;
    if (a == GM_UP) prow = prow > 0 ? prow - 1 : 0;
    else if (a == GM_DOWN) prow = prow < 7 ? prow + 1 : 7;
    else if (prow >= 6) {
        if (a == GM_LEFT || a == GM_RIGHT) settings_hud_step(prow == 7, a == GM_RIGHT ? 1 : -1);
    } else if (a == GM_LEFT || a == GM_RIGHT || a == GM_ACCEPT) {
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
    hover_kind = -1;  /* the keys or the pad took over: no hover look until the mouse moves again */
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
    if (a == GM_UP) sel = sel > 0 ? sel - 1 : 0;
    else if (a == GM_DOWN) sel = sel < GM_ITEMS - 1 ? sel + 1 : GM_ITEMS - 1;
    else if (a == GM_ACCEPT || (a == GM_RIGHT && (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_PERF)))
        accept();
    else if (a == GM_BACK) close_and_resume();  /* Escape, Backspace, B on the side panel: back to the game */
}

void gamemenu_refresh(void) { changed(); }

/* ---- the mouse: where things are, as the last drawing put them (design units). The present draws, the window's
 * thread reads: two lists, the drawing fills the one not being read, then swaps. */

enum { HIT_SIDE, HIT_SECTION, HIT_ROW, HIT_ROW_PREV, HIT_ROW_NEXT, HIT_BIND, HIT_MODE, HIT_CORNER, HIT_CORNER_BOX,
       HIT_PART, HIT_SLIDER, HIT_POPUP, HIT_HINT, HIT_BIND_STEP };
#define HINT_CLOSE (-1)  /* a key hint's action: close the overlay */
typedef struct {
    float x, y, w, h;
    int kind, a, b;
} Hit;
static Hit hits[2][96];
static int hit_n[2], hit_front;  /* hit_front: the list the mouse reads */
static int hit_back(void) { return __atomic_load_n(&hit_front, __ATOMIC_ACQUIRE) ^ 1; }
static void hit_add(float x, float y, float w, float h, int kind, int a, int b) {
    int k = hit_back();
    if (hit_n[k] < (int)(sizeof hits[k] / sizeof *hits[k])) hits[k][hit_n[k]++] = (Hit){x, y, w, h, kind, a, b};
}
/* the topmost thing under (x, y): the last added */
static int hit_find(float x, float y, Hit *out) {
    int k = __atomic_load_n(&hit_front, __ATOMIC_ACQUIRE);
    for (int i = hit_n[k] - 1; i >= 0; i--) {
        const Hit *h = &hits[k][i];
        if (x >= h->x && x < h->x + h->w && y >= h->y && y < h->y + h->h) {
            *out = *h;
            return 1;
        }
    }
    return 0;
}

void gamemenu_mouse(float x, float y, int button) {
    if (!gamemenu_open() || settings_open() || bindings_capturing()) return;
    if (button == 3) {  /* the right button: back, as Escape */
        gamemenu_action(GM_BACK);
        return;
    }
    Hit h;
    int found = hit_find(x, y, &h);
    if (button == 0 || button == 4) {  /* what is under the mouse now: its hover look */
        int k = found ? h.kind : -1, ha = found ? h.a : 0, hb = found ? h.b : 0;
        if (k != hover_kind || ha != hover_a || hb != hover_b) hover_kind = k, hover_a = ha, hover_b = hb, changed();
    }
    if (confirm && (!found || h.kind != HIT_POPUP)) return;  /* the popup covers the rest */
    if (!found) return;
    int click = button == 1, drag = button == 4;  /* 4: moved with the left button held (a slider's thumb) */
    int before = sel * 1000 + focus * 100 + srow + prow * 37 + choice * 7 + in_rows * 500;
    switch (h.kind) {
    case HIT_SIDE:
        /* hovering chooses on the side panel; on a page, only a click goes to another item */
        if (click) sel = h.a, focus = 0, accept();
        else if (!focus) sel = h.a;
        break;
    case HIT_SECTION:
        if (click) srow = h.a, focus = 1, in_rows = 0;
        break;
    case HIT_ROW: case HIT_ROW_PREV: case HIT_ROW_NEXT:
        srow = h.a, focus = 1, in_rows = 1;
        if (click) settings_change(h.a, h.kind == HIT_ROW_PREV ? -1 : 1);
        break;
    case HIT_BIND:
        focus = 1, bindings_select(h.a, h.b < 0 ? -1 : h.b);
        if (click && h.b != -2) bindings_action(SETTINGS_ACCEPT);  /* a key: waits for the new one; a value: the next */        break;
    case HIT_MODE:
        focus = 1, prow = 0;
        if (click) window_hud_set_mode(perf_modes[h.a]), settings_save();
        break;
    case HIT_CORNER:
        focus = 1, prow = 1;
        if (click) window_hud_set_corner(h.a), settings_save();
        break;
    case HIT_CORNER_BOX:
        focus = 1, prow = 1;
        if (click) window_hud_set_corner((window_hud_corner() + 1) % 4), settings_save();
        break;
    case HIT_PART:
        focus = 1, prow = 2 + h.a;
        if (click) window_hud_set_parts(window_hud_parts() ^ (1u << h.a)), settings_save();
        break;
    case HIT_SLIDER:  /* a: 1 opacity; the track from x to x + w */
        focus = 1, prow = 6 + h.a;
        if (click || drag) settings_hud_set(h.a, (x - h.x) / h.w), changed();
        break;
    case HIT_POPUP:
        choice = h.a;
        if (click) gamemenu_action(GM_ACCEPT);
        return;
    case HIT_HINT:  /* a key hint along the bottom: clicked, as its key */
        if (!click) return;
        if (h.a == HINT_CLOSE) close_and_resume();
        else gamemenu_action(h.a);
        return;
    case HIT_BIND_STEP:  /* a value's arrows on the controls page: b -1 or 1, as Left / Right */
        focus = 1, bindings_select(h.a, -1);
        if (click) bindings_action(h.b < 0 ? SETTINGS_LEFT : SETTINGS_RIGHT);
        break;
    }
    if (click || sel * 1000 + focus * 100 + srow + prow * 37 + choice * 7 + in_rows * 500 != before) changed();
}

void gamemenu_wheel(int dy) {
    if (!gamemenu_open() || settings_open() || bindings_capturing() || confirm) return;
    gamemenu_action(dy > 0 ? GM_UP : GM_DOWN);
}

/* ---- the look (Main.dc.html) */

enum {
    INK = 0x2b2338, NIGHT = 0x2a1b4c, PLUM = 0x3e2a6e, PAPER = 0xf8f4fc, GOLD = 0xe3be5c, LINE = 0xe2d5ef,
    WHITE = 0xffffff, SNOW = 0xfdfcfe, LILAC = 0xd9cdea, MUTED = 0x605673, WINE = 0x8c1f3f, SHADE = 0x160e26,
    /* the launcher's popup colours (style.css, OKLCH converted to sRGB) */
    POP_DEEP = 0x2b1a4d, POP_GOLD = 0xe1b75c, POP_WHITE = 0xfdfcff, POP_INK = 0x231e33, POP_LILAC = 0xf8f5fd,
    POP_LINE = 0xe1d5f1, POP_BORDER = 0x312451, POP_SHADOW = 0x0f0028, POP_BACKDROP = 0x1a0a35,
};

#define GAME_VERSION "2.2.0"  /* the one build this port runs (CLAUDE.md: the final 4++ update) */

/* the icons' SVG path data, from the design */
static const char *const ICON_RESUME = "<path d=\"M8 5v14l11-7z\"/>";
static const char *const ICON_SETTINGS =
    "<path d=\"M4 7h9m4 0h3M4 17h3m4 0h9M14 7a2 2 0 1 0 4 0a2 2 0 1 0-4 0M7 17a2 2 0 1 0 4 0a2 2 0 1 0-4 0\"/>";
/* a rounder, fuller pad than the design's (owner, 2026-11-05: the design's looked "malnourished") */
static const char *const ICON_CONTROLS =
    "<path d=\"M8 6h8c3.3 0 5.6 2.3 6 5.5l.8 5.5c.3 2.2-2.4 3.5-4 2L16 16H8l-2.8 3c-1.6 1.5-4.3.2-4-2L2 11.5"
    "C2.4 8.3 4.7 6 8 6zM8 9.5v4M6 11.5h4\"/><circle cx=\"15.4\" cy=\"10.2\" r=\"0.7\"/>"
    "<circle cx=\"18.4\" cy=\"13\" r=\"0.7\"/>";
static const char *const ICON_PERF = "<path d=\"M3 20h18M5 16l4-5 4 3 6-8\"/>";
/* a bug: its body, a line down its back, legs and feelers */
static const char *const ICON_BUG =
    "<path d=\"M8 10a4 4 0 0 1 8 0v4a4 4 0 0 1-8 0zM12 10v8M4 12h4M16 12h4M5 7.5l3 2M19 7.5l-3 2M5 18.5l3-2M19 18.5"
    "l-3-2M10 6.5L8.5 4M14 6.5L15.5 4\"/>";
static const char *const ICON_EXIT = "<path d=\"M12 3v9M6.3 6.3a8 8 0 1 0 11.4 0\"/>";
/* the value arrows (‹ ›): drawn as icons, not the font's characters, which sit low in their line */
static const char *const ICON_PREV = "<path d=\"M14.5 6l-6 6 6 6\"/>";
static const char *const ICON_NEXT = "<path d=\"M9.5 6l6 6-6 6\"/>";
static int hovered(int kind, int a, int b) { return hover_kind == kind && hover_a == a && hover_b == b; }
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
/* Buttons shown as their symbol in a key hint, not their name: PlayStation's shapes, the D-pad, and the overlay
 * buttons (24 x 24 view boxes, stroked). Back + Start shows both: View's two windows, Menu's three lines. */
static const struct {
    const char *name, *icon[2];
} hint_icons[] = {
    {"Cross", {"<path d=\"M7 7l10 10M17 7L7 17\"/>"}},
    {"Circle", {"<circle cx=\"12\" cy=\"12\" r=\"6.5\"/>"}},
    {"Square", {"<rect x=\"6\" y=\"6\" width=\"12\" height=\"12\" rx=\"1\"/>"}},
    {"Triangle", {"<path d=\"M12 5l7.5 12.5h-15z\"/>"}},
    {"D-pad", {"<path d=\"M9 3h6v6h6v6h-6v6H9v-6H3V9h6z\"/>"}},
    {"Touchpad", {"<rect x=\"3\" y=\"6\" width=\"18\" height=\"12\" rx=\"3\"/><path d=\"M12 6v5\"/>"}},
    {"Share", {"<path d=\"M12 3v11M8 7l4-4 4 4M5 11v9h14v-9\"/>"}},
    {"Capture", {"<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><circle cx=\"12\" cy=\"12\" r=\"4\"/>"}},
    {"Back + Start", {"<rect x=\"4\" y=\"5\" width=\"9\" height=\"9\" rx=\"1\"/><rect x=\"11\" y=\"10\" width=\"9\" "
                      "height=\"9\" rx=\"1\"/>",
                      "<path d=\"M5 7h14M5 12h14M5 17h14\"/>"}},
};

static float hint(const UiCanvas *c, float x, float y, const char *key, const char *label) {
    int sym = -1;
    for (size_t i = 0; i < sizeof hint_icons / sizeof *hint_icons; i++)
        if (!strcmp(hint_icons[i].name, key)) sym = (int)i;
    float kw;
    if (sym >= 0) {  /* the button's symbol on the chip, 18 units, one per button */
        int n = hint_icons[sym].icon[1] ? 2 : 1;
        kw = 30 + (n - 1) * 24;
        ui_box(c, x, y + 13, kw, 30, 15, SNOW, 0, 0);
        for (int k = 0; k < n; k++) ui_icon(c, hint_icons[sym].icon[k], x + 6 + k * 24, y + 19, 18, 2.4f, NIGHT);
    } else {
        kw = ui_text_w(UI_GOTHIC_700, 14, key) + 16;
        if (kw < 30) kw = 30;
        ui_box(c, x, y + 13, kw, 30, 15, SNOW, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 14, x, y + 13, kw, 30, 1, key, NIGHT);
    }
    float lw = ui_text_w(UI_GOTHIC_400, 16, label);
    ui_text_in(c, UI_GOTHIC_400, 16, x + kw + 10, y, 0, 56, 0, label, SNOW);
    /* the mouse: a hint of an action is a button too (Move and Choose aren't: they need a direction) */
    static const struct {
        const char *label;
        int action;
    } acts[] = {{"Back", GM_BACK}, {"Close overlay", HINT_CLOSE}, {"Select", GM_ACCEPT}, {"Open", GM_ACCEPT},
                {"Change", GM_ACCEPT}, {"Clear", GM_CLEAR}, {"Section", GM_NEXT}};
    for (size_t i = 0; i < sizeof acts / sizeof *acts; i++)
        if (!strcmp(label, acts[i].label)) hit_add(x - 6, y + 6, kw + 10 + lw + 12, 44, HIT_HINT, acts[i].action, 0);
    return kw + 10 + lw;
}

/* A popup button (Keep playing / Quit game): 56 high, radius 14; the chosen one ringed in gold. */
/* A popup button, as the launcher's (style.css dialog): 40 high, pill-shaped, 20 at the sides; the chosen one
 * ringed in gold. */
static float popup_button(const UiCanvas *c, float right, float y, const char *label, int quit, int on) {
    int f = quit ? UI_ROUNDED_800 : UI_ROUNDED_700;
    float w = ui_text_w(f, 14, label) + 40;
    float x = right - w;
    hit_add(x, y, w, 40, HIT_POPUP, quit, 0);
    if (on) ui_box(c, x - 4, y - 4, w + 8, 48, 24, GOLD, 0, 0);
    if (quit) ui_box(c, x, y, w, 40, 20, WINE, 0, 0);
    else ui_box(c, x, y, w, 40, 20, WHITE, 1, POP_LINE);
    ui_text_in(c, f, 14, x, y, w, 40, 1, label, quit ? SNOW : PLUM);
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

/* The settings' notes and descriptions as sentences: lower case, a capital at each sentence's start, names and
 * acronyms kept ("THE SWITCH'S DEFAULT: JAGGED EDGES" -> "The Switch's default: jagged edges", "ABOUT 2 MS" -> "about
 * 2 ms", "ENTER OR A: CHANGE" -> "Enter or A: change"). */
static void prose_case(const char *in, char *out, size_t n) {
    static const struct {
        const char *word, *as;
    } names[] = {
        {"SWITCH", "Switch"}, {"SWITCH'S", "Switch's"}, {"ALT", "Alt"}, {"TAB", "Tab"}, {"ENTER", "Enter"},
        {"DELETE", "Delete"}, {"ESCAPE", "Escape"}, {"BACKSPACE", "Backspace"}, {"SHIFT", "Shift"}, {"MS", "ms"},
        {"HZ", "Hz"}, {"NVIDIA", "NVIDIA"}, {"AMD", "AMD"}, {"GPU", "GPU"}, {"GPUS", "GPUs"}, {"CPU", "CPU"},
        {"HUD", "HUD"}, {"FXAA", "FXAA"}, {"SMAA", "SMAA"}, {"TAA", "TAA"}, {"FSR", "FSR"}, {"DLSS", "DLSS"},
        {"RTX", "RTX"}, {"AI", "AI"}, {"HD", "HD"}, {"PCSS", "PCSS"}, {"VSYNC", "VSync"}, {"F1", "F1"}, {"F2", "F2"},
        {"F3", "F3"}, {"F11", "F11"}, {"ZL", "ZL"}, {"ZR", "ZR"}, {"LB", "LB"}, {"RB", "RB"}, {"OK", "OK"},
    };
    size_t o = 0;
    int start = 1;     /* at a sentence's start */
    int after_or = 0;  /* the word before was OR / AND: a single letter is then a button ("ENTER OR A") */
    int after_the = 0; /* the word before was THE: "THE SWITCH" is the console, "LEFT/RIGHT: SWITCH" the verb */
    for (const char *p = in; *p && o + 1 < n;) {
        int alnum = (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!alnum) {
            if (*p == '.' || *p == '?' || *p == '!') start = 1;
            out[o++] = *p++;
            continue;
        }
        const char *e = p;
        while ((*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9') || (*e == '\'' && e[1] >= 'A' && e[1] <= 'Z')) e++;
        size_t len = (size_t)(e - p);
        const char *as = NULL;
        for (size_t k = 0; k < sizeof names / sizeof *names; k++)
            if (strlen(names[k].word) == len && !strncmp(names[k].word, p, len)) as = names[k].as;
        int digit = *p >= '0' && *p <= '9';
        if (as && len == 6 && !strncmp(p, "SWITCH", 6) && !after_the) as = NULL;  /* the verb */
        if (as) {
            for (const char *q = as; *q && o + 1 < n; q++) out[o++] = *q;
        } else if (len == 1 && (after_or || *e == ':' || *e == '/') && *p != 'I') {
            out[o++] = *p;  /* a button: A, B, X, Y */
        } else {
            for (size_t k = 0; k < len && o + 1 < n; k++) {
                char ch = p[k];
                int cap = start && k == 0 && !digit;
                out[o++] = cap || ch < 'A' || ch > 'Z' ? ch : (char)(ch + 32);  /* "1.5X" -> "1.5x" too */
            }
        }
        after_or = (len == 2 && !strncmp(p, "OR", 2)) || (len == 3 && !strncmp(p, "AND", 3));
        after_the = len == 3 && !strncmp(p, "THE", 3);
        if (!digit || as) start = 0;
        p = e;
    }
    out[o] = 0;
}

/* A note's highlights: "THE SWITCH'S DEFAULT" and "(RECOMMENDED)" become tags; *rest is what remains of it. */
enum { TAG_DEFAULT = 1, TAG_RECOMMENDED = 2 };
static int note_tags(const char *note, char *rest, size_t n) {
    int tags = 0;
    const char *def = "THE SWITCH'S DEFAULT";
    if (!strncmp(note, def, strlen(def))) {
        tags |= TAG_DEFAULT;
        note += strlen(def);
        while (*note == ':' || *note == ' ') note++;
    }
    snprintf(rest, n, "%s", note);
    char *r = strstr(rest, "(RECOMMENDED)");
    if (r) {
        tags |= TAG_RECOMMENDED;
        memmove(r, r + 13, strlen(r + 13) + 1);
        size_t l = strlen(rest);
        while (l && rest[l - 1] == ' ') rest[--l] = 0;
    }
    return tags;
}

/* a small tag (a note's highlight), centred at cx; returns its width */
static float tag(const UiCanvas *c, float x, float y, const char *label, uint32_t fill, uint32_t ink, int measure) {
    float w = ui_text_w(UI_GOTHIC_700, 12, label) + 16;
    if (!measure) {
        ui_box(c, x, y, w, 18, 9, fill, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 12, x, y, w, 18, 1, label, ink);
    }
    return w;
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
        if (on && focus && !in_rows) ui_box(c, 488 - 4, y - 4, 300 + 8, 52 + 8, 15, GOLD, 0, 0);  /* the focus */
        ui_box(c, 488, y, 300, 52, 12, on ? PLUM : WHITE, 2, on ? PLUM : LINE);
        hit_add(488, y, 300, 52, HIT_SECTION, i, 0);
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
        int on = focus && in_rows && i == cur, off = r->disabled != NULL;
        ui_box(c, x, y, w, 72, 14, WHITE, on ? 3 : 2, on ? GOLD : LINE);
        hit_add(x, y, w, 72, HIT_ROW, i, 0);
        sentence_case(r->name, t, sizeof t);
        ui_text_in(c, UI_GOTHIC_500, 18, x + 20, y, 0, 72, 0, t, off ? MUTED : INK);
        float nx = x + w - 12 - 44, vx = nx - 10 - 300, px = vx - 10 - 44, by = y + 14;
        hit_add(px, by, 44, 44, HIT_ROW_PREV, i, 0), hit_add(nx, by, 44, 44, HIT_ROW_NEXT, i, 0);
        for (int k = 0; k < 2; k++) {  /* ‹ and ›; darker under the mouse */
            float bx = k ? nx : px;
            int hv = !off && hovered(k ? HIT_ROW_NEXT : HIT_ROW_PREV, i, 0);
            ui_box(c, bx, by, 44, 44, 10, hv ? LILAC : 0xefe7f7, 2, hv ? PLUM : LINE);
            ui_icon(c, k ? ICON_NEXT : ICON_PREV, bx + 11, by + 11, 22, 2.4f, off ? MUTED : PLUM);
        }
        const char *value = r->options[r->current] ? r->options[r->current] : "";
        const char *note = r->notes[r->current] ? r->notes[r->current] : "";
        sentence_case(value, t, sizeof t);
        /* under the value: its tags (Switch default, Recommended), then the rest of its note as a sentence */
        char rest[200], prose[200];
        int tags = note_tags(note, rest, sizeof rest);
        prose_case(rest, prose, sizeof prose);
        float tw = 0;
        if (tags & TAG_DEFAULT) tw += tag(c, 0, 0, "Switch default", 0, 0, 1);
        if (tags & TAG_RECOMMENDED) tw += (tw ? 6 : 0) + tag(c, 0, 0, "Recommended", 0, 0, 1);
        float th = tags ? 18 : 0, gap = tags && *prose ? 3 : 0;
        float vh = 18 * 1.4f, nh = *prose ? ui_text_wrap(NULL, UI_GOTHIC_500, 13, 0, 0, 0, 280, 13 * 1.3f, 1, prose, 0) : 0;
        float bh2 = vh + (th || nh ? 2 + th + gap + nh : 0);
        float vy = y + (72 - bh2) / 2;
        ui_text_in(c, UI_ROUNDED_700, 18, vx, vy, 300, vh, 1, t, off ? MUTED : NIGHT);
        float ny = vy + vh + 2;
        if (tags) {
            float tx = vx + (300 - tw) / 2;
            if (tags & TAG_DEFAULT) tx += tag(c, tx, ny, "Switch default", 0xe8dff4, PLUM, 0) + 6;
            if (tags & TAG_RECOMMENDED) tag(c, tx, ny, "Recommended", GOLD, NIGHT, 0);
            ny += th + gap;
        }
        if (nh) ui_text_wrap(c, UI_GOTHIC_500, 13, 0, vx + 10, ny, 280, 13 * 1.3f, 1, prose, MUTED);  /* clear of the arrows */
        y += 72 + 10;
    }
    /* the chosen row's description */
    const SettingRow *r = &rows[cur];
    char desc[600], dprose[600], dname[160];
    if (r->disabled) snprintf(desc, sizeof desc, "%s OFF FOR NOW: %s", r->help, r->disabled);
    else snprintf(desc, sizeof desc, "%s", r->help);
    prose_case(desc, dprose, sizeof dprose);
    sentence_case(r->name, dname, sizeof dname);
    /* the setting's name in bold white, then what it does as sentences (no gold text: the owner's rule) */
    float lh = 15 * 1.5f, dh = lh + 4 + ui_text_wrap(NULL, UI_GOTHIC_500, 15, 0, 0, 0, w - 40, lh, 0, dprose, 0) + 32;
    ui_box(c, x, bottom - dh, w, dh, 14, NIGHT, 0, 0);
    ui_text(c, UI_ROUNDED_800, 15, x + 20, bottom - dh + 16, lh, dname, SNOW);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x + 20, bottom - dh + 16 + lh + 4, w - 40, lh, 0, dprose, LILAC);
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
    float hh = ui_text_wrap(NULL, UI_GOTHIC_500, 15, 0, 0, 0, 1024, 15 * 1.45f, 0, help, 0);
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
        /* the mouse: a setting row or the reset row as a whole (a click: as Enter); an input's row chooses it, its
         * key or input cells below take the click */
        hit_add(x, y, w, rh, HIT_BIND, i, r->group && !r->value ? -2 : -1);
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
        if (r->value && !strcmp(r->group, "KEYS FOR")) {  /* which set the page shows: one coloured pill, its name */
            const char *set = !strcmp(r->cells[0], "BATTLES") ? "Battles" : "Exploring";
            /* ‹ name ›: the arrows inside the pill, as the value Left / Right change */
            float sx = x + cw[0] + 22, sh = rh - 16, sy = y + 8, aw = 24;
            float nw = ui_text_w(UI_GOTHIC_700, 15, "Exploring"), sw = 16 + aw + nw + aw + 16;  /* as wide for both */
            ui_box(c, sx, sy, sw, sh, sh / 2, PLUM, 0, 0);
            ui_text_in(c, UI_GOTHIC_700, 15, sx + 16 + aw, sy, nw, sh, 1, set, SNOW);
            for (int k = 0; k < 2; k++) {  /* ‹ and ›, a lighter circle behind under the mouse */
                float ix = k ? sx + sw - 12 - aw : sx + 12, iy = sy + (sh - aw) / 2;
                int hv = hovered(HIT_BIND_STEP, i, k ? 1 : -1);
                if (hv) ui_box(c, ix, iy, aw, aw, aw / 2, 0x5b4691, 0, 0);
                ui_icon(c, k ? ICON_NEXT : ICON_PREV, ix + 4, iy + 4, aw - 8, 2.6f, hv ? SNOW : LILAC);
                hit_add(k ? sx + sw - 16 - aw : sx, sy, 16 + aw, sh, HIT_BIND_STEP, i, k ? 1 : -1);
            }
            continue;
        }
        if (r->value) {  /* a setting: its value in the keyboard column */
            /* ‹ value ›: the arrows either side of the value (as the set's pill), Left / Right; a box under the mouse */
            sentence_case(r->cells[0], t, sizeof t);
            float lx = x + cw[0] + 14, vw = 110, by = y + (rh - 26) / 2;
            ui_text_in(c, UI_GOTHIC_500, 17, lx + 30, y, vw, rh, 1, t, NIGHT);
            for (int k = 0; k < 2; k++) {
                float bx = k ? lx + 30 + vw + 4 : lx;
                int hv = hovered(HIT_BIND_STEP, i, k ? 1 : -1);
                if (hv) ui_box(c, bx, by, 26, 26, 7, LILAC, 0, 0);
                ui_icon(c, k ? ICON_NEXT : ICON_PREV, bx + 4, by + 4, 18, 2.4f, PLUM);
                hit_add(bx - 2, y, 30, rh, HIT_BIND_STEP, i, k ? 1 : -1);
            }
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
            hit_add(kx - 10, y, tw + 20, rh, HIT_BIND, i, k);
            if (on && col == k) ui_box(c, kx - 10, y + 8, tw + 20, rh - 16, 8, GOLD, 0, 0);
            ui_text_in(c, UI_GOTHIC_500, 17, kx, y, 0, rh, 0, t,
                       r->conflict[k] ? WINE : !strcmp(r->cells[k], "-") ? MUTED : INK);
        }
    }
    char hprose[512];
    prose_case(help, hprose, sizeof hprose);
    ui_text_wrap(c, UI_GOTHIC_500, 15, 0, x, top + th + 22, w, 15 * 1.45f, 0, hprose, MUTED);
}

/* A choice chip (the design's chipStyle): the chosen one filled; ringed in gold where the focus is. */
static void chip(const UiCanvas *c, float x, float y, float w, const char *label, int on, int ring) {
    if (ring) ui_box(c, x - 4, y - 4, w + 8, 60, 15, GOLD, 0, 0);
    ui_box(c, x, y, w, 52, 12, on ? PLUM : WHITE, 2, on ? PLUM : LINE);
    ui_text_in(c, UI_GOTHIC_700, 17, x, y, w, 52, 1, label, on ? SNOW : PLUM);
}

/* A slider of the Overlay page: the track, its filled part, the thumb, the value at the right. */
static void perf_slider(const UiCanvas *c, float x, float y, float w, const char *label, float lh, int opacity, int ring) {
    float frac;
    char text[16];
    settings_hud_slider(opacity, &frac, text, sizeof text);
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, label, PLUM);
    y += lh + 10;
    ui_box(c, x, y, w, 52, 12, WHITE, ring ? 3 : 2, ring ? GOLD : LINE);
    float tx = x + 18, tw = w - 18 - 16 - 54 - 14;
    hit_add(tx, y, tw, 52, HIT_SLIDER, opacity, 0);  /* the track, the box's height */
    ui_box(c, tx, y + 23, tw, 6, 3, LINE, 0, 0);
    if (frac > 0) ui_box(c, tx, y + 23, tw * frac, 6, 3, PLUM, 0, 0);
    ui_box(c, tx + tw * frac - 10, y + 16, 20, 20, 10, PLUM, 2, WHITE);
    ui_text_in(c, UI_ROUNDED_700, 16, x + w - 16 - 54, y, 54, 52, 2, text, INK);
}

/* The preview's picture: the paused game at its true size, the part where the HUD sits (the chosen corner), so the
 * opacity slider shows what the card will really be over. */
static void perf_preview_picture(const UiCanvas *c, const GameMenuInfo *in, float x, float y, float w, float h, int corner) {
    int pw = (int)(w * c->s), ph = (int)(h * c->s);
    if (!in->picture || pw > in->picture_w || ph > in->picture_h) {
        ui_box(c, x, y, w, h, 16, NIGHT, 0, 0);
        return;
    }
    uint8_t *crop = malloc((size_t)pw * ph * 4);
    if (!crop) {
        ui_box(c, x, y, w, h, 16, NIGHT, 0, 0);
        return;
    }
    int ox = corner & 1 ? in->picture_w - pw : 0, oy = corner >> 1 ? in->picture_h - ph : 0;
    for (int j = 0; j < ph; j++)
        memcpy(crop + (size_t)j * pw * 4, in->picture + ((size_t)(oy + j) * in->picture_w + ox) * 4, (size_t)pw * 4);
    ui_image(c, crop, pw, ph, x, y, w, h, 16);
    free(crop);
}

/* The performance page (design canvas: Overlay v2, the corner on a small screen): how the HUD shows while playing, its
 * corner, the detailed one's parts, a preview of the HUD over the paused game, its scale and opacity. */
static void draw_perf_page(const UiCanvas *c, const GameMenuInfo *in) {
    ui_box(c, 448, 40, 1104, 764, 20, PAPER, 0, 0);
    ui_text(c, UI_ROUNDED_800, 34, 488, 76, 34 * 1.25f, "Overlay", NIGHT);
    float y = 76 + 34 * 1.25f + 24, x = 488, lh = 16 * 1.45f;
    int mode = window_hud_mode(), corner = window_hud_corner();
    unsigned parts = window_hud_parts();
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "Show while playing", PLUM);
    y += lh + 10;
    static const char *const modes[3] = {"Off", "Compact", "Detailed"};
    float lw = 1024 - 360 - 32;
    float mw = (lw - 2 * 10) / 3;
    for (int i = 0; i < 3; i++) hit_add(x + (float)i * (mw + 10), y, mw, 52, HIT_MODE, i, 0);
    for (int i = 0; i < 3; i++)
        chip(c, x + (float)i * (mw + 10), y, mw, modes[i], perf_modes[i] == mode, focus && prow == 0 && perf_modes[i] == mode);
    y += 52 + 22;
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "Corner", PLUM);
    y += lh + 10;
    static const char *const corners[4] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    {  /* a small screen with the four corners on it, and the chosen one's name */
        int ring = focus && prow == 1;
        ui_box(c, x, y, lw, 124, 12, WHITE, ring ? 3 : 2, ring ? GOLD : LINE);
        float sx = x + 18, sy = y + 12;
        ui_box(c, sx, sy, 176, 100, 10, LINE, 0, 0);
        hit_add(x, y, lw, 124, HIT_CORNER_BOX, 0, 0);
        for (int i = 0; i < 4; i++)  /* each half of the small screen picks its corner */
            hit_add(sx + (i & 1 ? 88 : 0), sy + (i >> 1 ? 50 : 0), 88, 50, HIT_CORNER, i, 0);
        for (int i = 0; i < 4; i++)
            ui_box(c, sx + (i & 1 ? 176 - 6 - 58 : 6), sy + (i >> 1 ? 100 - 6 - 36 : 6), 58, 36, 7, i == corner ? PLUM : WHITE, 0, 0);
        ui_text_in(c, UI_GOTHIC_700, 17, sx + 176 + 24, y, 0, 124, 0, corners[corner & 3], PLUM);
    }
    y += 124 + 22;
    ui_text(c, UI_GOTHIC_700, 16, x, y, lh, "In the detailed overlay", PLUM);
    y += lh + 10;
    static const char *const names[4] = {"Frame rate", "Frame time graph", "GPU, CPU and RAM", "Frame generation"};
    for (int i = 0; i < 4; i++, y += 52 + 10) {
        int on = parts >> i & 1, ring = focus && prow == 2 + i, off = mode != 1;
        ui_box(c, x, y, lw, 52, 12, WHITE, ring ? 3 : 2, ring ? GOLD : LINE);
        hit_add(x, y, lw, 52, HIT_PART, i, 0);
        ui_box(c, x + 18, y + 13, 26, 26, 7, on ? PLUM : WHITE, 2, on ? PLUM : 0xc6b2da);
        if (on) ui_text_in(c, UI_ROUNDED_800, 16, x + 18, y + 13, 26, 26, 1, "\xe2\x9c\x93", SNOW);  /* ✓ */
        ui_text_in(c, UI_GOTHIC_700, 17, x + 18 + 26 + 14, y, 0, 52, 0, names[i], off ? MUTED : PLUM);
    }
    /* the preview: the HUD itself, drawn now, over the paused game; one height for every form, so the sliders below
     * stay put and the two columns end level */
    float px = 488 + lw + 32, py = 76 + 34 * 1.25f + 24, bh = 310;
    ui_text(c, UI_GOTHIC_700, 16, px, py, lh, "Preview", PLUM);
    py += lh + 10;
    static uint8_t img[1024 * 1024 * 4], tight[1024 * 1024 * 4];
    int iw = 0, ih = 0;
    if (mode) window_hud_preview(img, 1024, 1024, c->s, &iw, &ih);
    perf_preview_picture(c, in, px, py, 360, bh, corner);
    if (iw > 0 && ih > 0) {
        for (int j = 0; j < ih; j++) memcpy(tight + (size_t)j * iw * 4, img + (size_t)j * 1024 * 4, (size_t)iw * 4);
        /* pixel for pixel, so it is sharp and as big as it will be; centred in the box */
        float sw = (float)iw / c->s, sh = (float)ih / c->s;
        ui_image(c, tight, iw, ih, px + (360 - sw) * 0.5f, py + (bh - sh) * 0.5f, sw, sh, 0);
    } else {
        ui_box_alpha(c, px, py, 360, bh, 16, NIGHT, 0.6f);
        ui_text_in(c, UI_GOTHIC_700, 16, px, py, 360, bh, 1, "Off while playing", SNOW);
    }
    py += bh + 22;
    perf_slider(c, px, py, 360, "Scale", lh, 0, focus && prow == 6);
    perf_slider(c, px, py + lh + 10 + 52 + 22, 360, "Opacity", lh, 1, focus && prow == 7);
}

void gamemenu_draw(const UiCanvas *c, const GameMenuInfo *in) {
    float W = (float)c->w / c->s, H = 900;
    int hk = hit_back();
    hit_n[hk] = 0;  /* the mouse's list, filled as things are drawn */
    int page = (sel == GM_SETTINGS || sel == GM_CONTROLS || sel == GM_PERF) && !confirm;  /* these show a page */
    ui_rect(c, 0, 0, W, H, SHADE, page ? 0.62f : 0.35f);  /* the tint over the game, darker behind a page */
    if (page && sel == GM_SETTINGS) draw_settings_page(c);
    if (page && sel == GM_CONTROLS) draw_controls_page(c, in);
    if (page && sel == GM_PERF) draw_perf_page(c, in);

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

    static const char *const labels[] = {"Resume", "Settings", "Controls", "Overlay", "Report a bug"};
    const char *icons[] = {ICON_RESUME, ICON_SETTINGS, ICON_CONTROLS, ICON_PERF, ICON_BUG};
    for (int i = 0; i < 5; i++) {
        const char *l = labels[i];
        side_button(c, 20, 128 + i * 66, 356, icons[i], l, sel == i, PLUM);
        hit_add(20, 128 + i * 66, 356, 56, HIT_SIDE, i, 0);
    }
    /* Exit game at the bottom, under a line; the version under it */
    float vy = H - 20 - 19, ey = vy - 14 - 56;
    ui_rect(c, 20, ey - 14 - 2, 356, 2, LINE, 1);
    side_button(c, 20, ey, 356, ICON_EXIT, "Exit game", sel == GM_EXIT, WINE);
    hit_add(20, ey, 356, 56, HIT_SIDE, GM_EXIT, 0);
    char ver[48];
    snprintf(ver, sizeof ver, "YW4 Recomp %s", in->version);
    ui_text(c, UI_GOTHIC_400, 13, 20, vy, 19, ver, MUTED);

    /* the key hints along the bottom */
    ui_rect(c, 400, H - 56, W - 400, 56, NIGHT, 1);
    float hx = 448;
    if (focus && sel == GM_CONTROLS) {  /* the controls page */
        int ps = in->pad == 2;
        hx += hint(c, hx, H - 56, in->pad ? "D-pad" : "W S", "Choose") + 28;
        if (!in->pad) hx += hint(c, hx, H - 56, "A D", "Navigate") + 28;  /* across: a key, the other key, the pad */
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Cross" : "A") : "Enter", "Change") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Square" : "X") : "Del", "Clear") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (ps ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus && sel == GM_SETTINGS && !in_rows) {  /* the settings page's sections */
        hx += hint(c, hx, H - 56, in->pad ? "D-pad" : "W S", "Choose section") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (in->pad == 2 ? "Cross" : "A") : "Enter", "Open") + 28;
        hx += hint(c, hx, H - 56, in->pad ? (in->pad == 2 ? "Circle" : "B") : "Esc", "Back") + 28;
        if (in->pad) hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus && in->pad) {  /* on a page */
        hx += hint(c, hx, H - 56, "D-pad", "Choose and change") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, H - 56, in->pad == 2 ? "L1 R1" : "LB RB", "Section") + 28;
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Back") + 28;
        hint(c, hx, H - 56, in->close, "Close overlay");
    } else if (focus) {  /* on a page, with the keyboard: Escape goes back to the side panel, again closes */
        hx += hint(c, hx, H - 56, "W S", "Move") + 28;
        hx += hint(c, hx, H - 56, "A D", "Change") + 28;
        hx += hint(c, hx, H - 56, "Enter", "Select") + 28;
        if (sel == GM_SETTINGS) hx += hint(c, hx, H - 56, "Q E", "Section") + 28;
        hint(c, hx, H - 56, "Esc", "Back");
    } else if (in->pad) {
        hx += hint(c, hx, H - 56, in->pad == 2 ? "Cross" : "A", "Select") + 28;
        hint(c, hx, H - 56, in->pad == 2 ? "Circle" : "B", "Close overlay");
    } else {
        hx += hint(c, hx, H - 56, "W S", "Move") + 28;
        hx += hint(c, hx, H - 56, "Enter", "Select") + 28;
        hint(c, hx, H - 56, "Esc", "Close overlay");
    }

    /* the note after Report a bug (not built yet): top right */
    if (in->now < toast_until) {
        const char *t = "Bug reports are coming soon";
        float tw = ui_text_w(UI_GOTHIC_700, 17, t), bw = tw + 44 + 6, bh = 17 * 1.45f + 28 + 6;
        ui_box(c, W - 48 - bw, 40, bw, bh, 14, SNOW, 3, GOLD);
        ui_text_in(c, UI_GOTHIC_700, 17, W - 48 - bw, 40, bw, bh, 1, t, NIGHT);
    }

    /* the exit popup: a small window as the launcher's popups (launcher/ui/style.css "dialog", 2026-11-05): a
     * violet title bar over a gold rule, the text on white, the buttons in a lilac tray at the bottom */
    if (confirm) {
        ui_rect(c, 0, 0, W, H, POP_BACKDROP, 0.6f);
        float pw = 460, lh_title = 17 * 1.3f, bar = 15 + lh_title + 13, lh = 14 * 1.5f, tray = 12 + 40 + 12;
        float ph = bar + 3 + 18 + lh + 18 + tray;
        float px = (W - pw) / 2, py = (H - ph) / 2;
        ui_shadow(c, px, py + 14, pw, ph, 14, 36, POP_SHADOW, 0.45f);
        ui_box_alpha(c, px - 1, py - 1, pw + 2, ph + 2, 15, POP_BORDER, 0.5f);  /* the 1 px border, half seen */
        ui_box(c, px, py, pw, ph, 14, POP_WHITE, 0, 0);
        ui_box(c, px, py, pw, bar + 14, 14, POP_DEEP, 0, 0);  /* the title bar (its lower corners covered below) */
        ui_rect(c, px, py + bar, pw, 3, POP_GOLD, 1);
        float ty = py + ph - tray;
        ui_box(c, px, ty - 14, pw, tray + 14, 14, POP_LILAC, 0, 0);  /* the tray (its upper corners covered) */
        ui_rect(c, px, py + bar + 3, pw, ty - (py + bar + 3), POP_WHITE, 1);
        ui_rect(c, px, ty, pw, 1, POP_LINE, 1);
        ui_text(c, UI_ROUNDED_800, 17, px + 22, py + 15, lh_title, "Quit the game?", POP_WHITE);
        ui_text(c, UI_GOTHIC_400, 14, px + 22, py + bar + 3 + 18, lh, "Progress since your last save is lost.", POP_INK);
        float right = px + pw - 16;
        right -= popup_button(c, right, ty + 12, "Quit game", 1, choice == 1) + 10;
        popup_button(c, right, ty + 12, "Keep playing", 0, choice == 0);
    }
    __atomic_store_n(&hit_front, hk, __ATOMIC_RELEASE);  /* the mouse reads this drawing's list now */
}
