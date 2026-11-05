/* Themes for the in-game overlay and the performance HUD (2026-11-06): the launcher's themes, the same files
 * (launcher/ui/themes/<id>.css: a "name: X" comment, then a :root block of CSS colour tokens, launcher/ui/style.css's
 * names), read when the game starts, so a theme dropped in that folder shows up in both. The built-in look
 * ("recomp", the overlay's design colours) needs no file and is what any token a theme leaves out falls back to.
 * The folder: YW4_THEMES_DIR, else "themes" next to yw4.exe, else the repo's launcher/ui/themes (a build in
 * work/build). The choice: YW4_THEME=<id> (settings.ini, the overlay's Settings > Look > Theme row). Why: decision
 * 0055. */
#ifndef YW4_THEME_H
#define YW4_THEME_H

#include <stdint.h>

/* The colours by what they do, 0xRRGGBB. */
typedef struct {
    uint32_t ink;      /* body text */
    uint32_t night;    /* the darkest: the side panel's header, the hint bar, titles, description boxes */
    uint32_t plum;     /* headings, outlines of the chosen, filled chips */
    uint32_t paper;    /* the page and side panel background */
    uint32_t gold;     /* gold fills: the chosen side button, tags, the gold rules */
    uint32_t ring;     /* the focus: the rings and outlines of what is chosen */
    uint32_t line;     /* borders */
    uint32_t white;    /* boxes on the paper */
    uint32_t snow;     /* text and key chips on the dark */
    uint32_t lilac;    /* hover fills */
    uint32_t on_dark;  /* soft text on the dark (descriptions, the version line) */
    uint32_t muted;    /* secondary text, greyed rows */
    uint32_t wine;     /* Exit game, Quit, conflicts */
    uint32_t shade;    /* the tint over the game */
    uint32_t soft;     /* soft fills: arrow boxes, the table's header, strips */
    uint32_t edge;     /* a darker edge: hover borders, check boxes */
    uint32_t tag;      /* the Switch default tag */
    uint32_t vsoft;    /* a lighter dark: hover on dark pills */
    uint32_t on_gold;  /* text on gold */
    uint32_t pop_deep, pop_gold, pop_white, pop_ink, pop_lilac, pop_line, pop_border, pop_shadow, pop_backdrop;
    uint32_t hud_text, hud_label, hud_card, hud_plot, hud_target;  /* the HUD's see-through card */
} Theme;

const Theme *theme_colors(void);  /* the chosen theme's */
int theme_count(void);            /* the built-in one, then the folder's, by id */
int theme_index(void);            /* the chosen one */
const char *theme_name(int i);    /* "YW4 Recomp", "Jibanyan" */
void theme_step(int dir);         /* the next (1) or the one before (-1), wrapping (settings.c saves it) */
const char *theme_id(void);       /* the chosen one's id, for settings.ini ("recomp") */

#endif
