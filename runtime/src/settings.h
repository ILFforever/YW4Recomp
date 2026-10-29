/* The settings panel (F1, or a gamepad's Home or Minus+Plus): the graphics settings in groups, each with all
 * its options shown and the current one marked, and a description of the chosen setting and option. Up/Down
 * pick a setting, Left/Right change it, F1, Escape or B close the panel. While it is open neither the keyboard
 * nor the gamepad reaches the game. The settings change through their own functions (gpu.c, gpu_draw.c,
 * gpu_aa.c, shadow.c, gbuffer.c). Drawn by hud.c (panel_render), shown by window.c. */
#ifndef YW4_SETTINGS_H
#define YW4_SETTINGS_H

enum { SETTINGS_MAX_OPTIONS = 5 };

typedef struct {
    const char *category;  /* the group it is listed under */
    const char *name;
    const char *help;      /* what the setting does */
    const char *options[SETTINGS_MAX_OPTIONS];
    const char *notes[SETTINGS_MAX_OPTIONS];  /* a few words per option */
    int noptions;
    int current;  /* index into options */
    const char *disabled;  /* not NULL: greyed out, cannot be changed, and why (shown in the description) */
} SettingRow;

enum { SETTINGS_UP, SETTINGS_DOWN, SETTINGS_LEFT, SETTINGS_RIGHT, SETTINGS_CLOSE, SETTINGS_ACCEPT, SETTINGS_CLEAR,
       SETTINGS_PAGE };

/* Reads the settings file (settings.ini next to the executable, or YW4_SETTINGS_FILE; "none": no file) into the
 * environment, line by line, where the variable isn't set already, so the environment still wins. Called
 * first thing at start, before anything reads its variable; every change in the panel writes the file. */
void settings_load(void);
int settings_open(void);
void settings_toggle(void);
/* The page shown: 0 the settings (F1), 1 the controls (F2, bindings.h); SETTINGS_PAGE switches (a gamepad's
 * L/R, which has no F keys). */
int settings_page(void);
/* F1/F2: opens the panel on that page, or switches to it; closes the panel if it already shows it. */
void settings_show(int page);
/* Writes the settings file (the settings and the controls); every change calls it. */
void settings_save(void);
/* One of the actions above, from the window's key presses while the panel is open. */
void settings_action(int action);
/* The rows as they are now, and the selected one; returns the count. */
int settings_rows(SettingRow *rows, int max, int *selected);

#endif
