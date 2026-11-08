/* The in-game overlay (2026-11-05): a side panel over the paused game, opened by the overlay button (Escape;
 * the touchpad on PlayStation pads, Share on Xbox Series, Capture on Switch Pro; Back + Start on any pad). Its
 * items: Resume; Settings, Controls and Overlay, each with its page beside the panel (the player's settings by
 * section, settings.h; the bindings as a table, bindings.h; the performance HUD's form, corner and parts, with a
 * preview); Report a bug (two pages the player fills in: what's wrong, what happened, how often, what to attach;
 * saving and sending come next); Exit game (with a popup to confirm). Opening it pauses the game (the game's own
 * pause, its screen hidden: pause.c), Resume or the overlay button again resumes. Its look is the design on the
 * canvas https://claude.ai/artifact/DpV5n3eHezBdAMY2aQUgPg (Main.dc.html), drawn 1:1 in the design's units by
 * gamemenu_draw (ui.h); window.c puts it over the paused picture and feeds it input. Why so: decision 0053. */
#ifndef YW4_GAMEMENU_H
#define YW4_GAMEMENU_H

#include <stddef.h>
#include <stdint.h>

#include "ui.h"

enum { GM_RESUME, GM_SETTINGS, GM_CONTROLS, GM_PERF, GM_BUG, GM_EXIT, GM_ITEMS };
/* GM_BACK: one step back (B, Escape, Backspace, the right mouse button): a page to the side panel, the side panel
 * to the game. GM_LEAVE: as GM_BACK on a page (older recordings' A key). GM_PREV / GM_NEXT: the page's previous /
 * next section (LB / RB, LT / RT, Q / E). Lists stop at their ends. */
/* GM_CLEAR: clear a binding (X, Delete). GM_ALT (P or H, Y / Triangle): on the Settings page Preview, in the
 * preview the bar hidden or shown. */
enum { GM_UP, GM_DOWN, GM_LEFT, GM_RIGHT, GM_ACCEPT, GM_BACK, GM_LEAVE, GM_PREV, GM_NEXT, GM_CLEAR, GM_ALT };

/* Preview (owner, 2026-11-06, canvas https://claude.ai/artifact/KvDDHw4thzRqDvzKWnBFn9 board B): from the Settings
 * page, the game runs again (the player held still) with a bar at the bottom holding the chosen section's graphics
 * settings, so each change shows live; Back pauses again and returns to the Settings page. While it shows, the
 * window draws only gamemenu_preview_rect's part of the overlay (design units: x, y, w, h; w 0: nothing) over
 * the live picture. */
int gamemenu_previewing(void);
void gamemenu_preview_rect(float r[4]);

/* Name entry (2026-11-07, canvas board N1): the game's software keyboard (swkbd.c) asks for a name and waits; the
 * overlay shows only a popup with the game's question (title), a field of at most max_chars letters and OK. Typing
 * starts at once; Enter, OK or a pad's A finishes it (empty: fallback). gamemenu_prompt_take: 1 and the name once
 * it is finished (the overlay is closed again then). While it shows, gamemenu_open() is 1. */
void gamemenu_prompt_begin(const char *title, int max_chars, const char *fallback);
/* Before begin: at least min_chars letters (OK greyed out until then); cancel: a Cancel button (Esc, B). */
void gamemenu_prompt_options(int min_chars, int cancel);
void gamemenu_prompt_cancel(void);
/* 0 still open, 1 answered (out), 2 cancelled */
int gamemenu_prompt_take(char *out, size_t n);
/* The same popup with a message instead of a field, and OK (a pad's A or B, Enter, Escape); taken the same way. */
void gamemenu_message_begin(const char *title, const char *body);
int gamemenu_prompting(void);
/* The focused text field's box in design units (x, y, w, h) and the caret's x, for the system's IME
 * (SDL_SetTextInputArea); 0 when no field is focused. */
int gamemenu_text_rect(float r[4], float *caret_x);

int gamemenu_open(void);       /* the overlay is open (its panel shows, or a page it opened) */
int gamemenu_selected(void);   /* GM_* */
int gamemenu_confirming(void); /* the exit popup shows */
void gamemenu_toggle(void);    /* the overlay button: open, or close and resume */
/* LEGACY MENUS off (settings.h): F1, F2 and F3 open the overlay straight on GM_SETTINGS, GM_CONTROLS or GM_PERF
 * (pausing the game); the same key on its own page closes it and resumes. */
void gamemenu_open_page(int item);
void gamemenu_action(int action);  /* GM_UP... while it is open and no settings page covers it */
/* The mouse at (x, y) in the design's units (1600 x 900 for 16:9): button 0 moved (hovering chooses), 1 the left
 * button pressed (as Enter on what is under it), 3 the right one (back, as Escape), 4 moved with the left one held
 * (a slider follows it). gamemenu_wheel: the wheel (dy > 0 up) scrolls the Controls table one row a notch;
 * nothing elsewhere. */
void gamemenu_mouse(float x, float y, int button);
void gamemenu_wheel(int dy);
/* Draw it again (the hints changed between keyboard and controller). */
void gamemenu_refresh(void);
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
    const uint8_t *picture;  /* the paused game without the overlay (RGBA, picture_w x picture_h): the HUD's preview */
    int picture_w, picture_h;
    float fps, shown_fps;  /* the preview: the game's frame rate; frames shown with frame generation (0: off) */
} GameMenuInfo;

/* Draws the overlay over what is in c (the paused picture): the tint, the side panel, the key hints, the note,
 * the exit popup. c's design width is c->w / c->s (1600 for 16:9; wider pictures widen the hint bar). */
void gamemenu_draw(const UiCanvas *c, const GameMenuInfo *info);

#endif
