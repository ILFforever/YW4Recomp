/* The in-game overlay (2026-11-05): a side panel over the paused game, opened by the overlay button (Escape;
 * the touchpad on PlayStation pads, Share on Xbox Series, Capture on Switch Pro; Back + Start on any pad). Its
 * items: Resume; Settings, Controls and Overlay, each with its page beside the panel (the player's settings by
 * section, settings.h; the bindings as a table, bindings.h; the performance HUD's form, corner and parts, with a
 * preview); Report a bug (not built yet: a note says so; F12 takes screenshots); Exit game (with a popup to confirm). Opening it pauses the game (the game's own
 * pause, its screen hidden: pause.c), Resume or the overlay button again resumes. Its look is the design on the
 * canvas https://claude.ai/artifact/DpV5n3eHezBdAMY2aQUgPg (Main.dc.html), drawn 1:1 in the design's units by
 * gamemenu_draw (ui.h); window.c puts it over the paused picture and feeds it input. Why so: decision 0053. */
#ifndef YW4_GAMEMENU_H
#define YW4_GAMEMENU_H

#include <stdint.h>

#include "ui.h"

enum { GM_RESUME, GM_SETTINGS, GM_CONTROLS, GM_PERF, GM_BUG, GM_EXIT, GM_ITEMS };
/* GM_LEAVE: back out of a page (the A key; B and Backspace are GM_BACK, which also closes the overlay from the
 * side panel); GM_PREV / GM_NEXT: the page's previous / next section (LB / RB, Q / E) */
/* GM_CLEAR: clear a binding (X, Delete) */
enum { GM_UP, GM_DOWN, GM_LEFT, GM_RIGHT, GM_ACCEPT, GM_BACK, GM_LEAVE, GM_PREV, GM_NEXT, GM_CLEAR };

int gamemenu_open(void);       /* the overlay is open (its panel shows, or a page it opened) */
int gamemenu_selected(void);   /* GM_* */
int gamemenu_confirming(void); /* the exit popup shows */
void gamemenu_toggle(void);    /* the overlay button: open, or close and resume */
void gamemenu_action(int action);  /* GM_UP... while it is open and no settings page covers it */
/* Changes each time what the overlay shows changes (window.c draws it again then); `now` in ms, for the
 * "Screenshot saved" note, which shows 1.6 s. */
uint32_t gamemenu_look(uint64_t now);

/* What the overlay shows besides its own state. pad: the hints name 0 the keyboard's keys, 1 a pad's (A, B),
 * 2 a PlayStation pad's (Cross, Circle); close: the overlay button's name on it. */
typedef struct {
    int hud_on;
    const char *version;  /* "0.7.0" */
    int pad;
    const char *close;
    const char *pad_name;  /* the connected controller (the Controls page), NULL: none */
    const uint8_t *logo;  /* the game's icon (RGBA, logo_w x logo_h), NULL: none */
    int logo_w, logo_h;
    uint64_t now;  /* ms */
} GameMenuInfo;

/* Draws the overlay over what is in c (the paused picture): the tint, the side panel, the key hints, the note,
 * the exit popup. c's design width is c->w / c->s (1600 for 16:9; wider pictures widen the hint bar). */
void gamemenu_draw(const UiCanvas *c, const GameMenuInfo *info);

#endif
