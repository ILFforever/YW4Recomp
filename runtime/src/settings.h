/* The settings panel (F1): the graphics settings in one list, each with all its options shown and the
 * current one marked. Up/Down pick a setting, Left/Right change it, F1 or Escape close the panel. While it
 * is open the keyboard doesn't reach the game (a gamepad still does). The F keys stay as shortcuts; both go
 * through the same functions (gpu.c, gpu_draw.c, shadow.c, gbuffer.c). */
#ifndef YW4_SETTINGS_H
#define YW4_SETTINGS_H

enum { SETTINGS_MAX_OPTIONS = 5 };

typedef struct {
    const char *name;
    const char *options[SETTINGS_MAX_OPTIONS];
    int noptions;
    int current;  /* index into options */
} SettingRow;

enum { SETTINGS_UP, SETTINGS_DOWN, SETTINGS_LEFT, SETTINGS_RIGHT, SETTINGS_CLOSE };

int settings_open(void);
void settings_toggle(void);
/* One of the actions above, from the window's key presses while the panel is open. */
void settings_action(int action);
/* The rows as they are now, and the selected one; returns the count. */
int settings_rows(SettingRow *rows, int max, int *selected);

#endif
