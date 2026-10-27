/* The controls (the settings panel's second page): which keyboard keys and which gamepad input press each
 * Switch button or push each stick direction. Two keys and one gamepad input per Switch input; a gamepad
 * input is a button, or one direction of a stick or trigger (sticks stay analog). Saved with the settings
 * (settings.c). The panel's own keys (arrows, Enter, Escape, F1, the D-pad) are fixed, so a bad binding
 * can always be undone from the panel. */
#ifndef YW4_BINDINGS_H
#define YW4_BINDINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { BIND_KEYS = 2, BIND_COLS = BIND_KEYS + 1, BIND_NAME = 24 };

typedef struct {
    const char *group;  /* BUTTONS, D-PAD, LEFT STICK, RIGHT STICK; NULL: the reset row */
    const char *name;
    char cells[BIND_COLS][BIND_NAME];  /* the two keys, then the gamepad input; "-" for none */
    int conflict[BIND_COLS];           /* the cell's key or input also presses another Switch input */
} BindRow;

/* The Switch buttons (bits as in svc_hid) and sticks (left x, left y, right x, right y; up and right
 * positive) for the keyboard state (SDL's, by scancode) and gamepad (an SDL_Gamepad, or NULL). */
struct SDL_Gamepad;
void bindings_read(const bool *keyboard, struct SDL_Gamepad *gamepad, uint64_t *buttons, int32_t sticks[4]);

/* The connected controller is a Nintendo one (its A is on the right, B at the bottom; others the other way). */
int bindings_pad_nintendo(void);

/* The panel page: settings.h's actions (UP, DOWN, LEFT, RIGHT, ACCEPT, CLEAR) while it shows. */
void bindings_action(int action);
/* Waiting for a key or gamepad input to bind: the window passes presses here instead of to the panel.
 * Each returns 1 if it took the press. Five seconds without one, or closing the panel, cancels. */
int bindings_capturing(void);
int bindings_capture_key(int scancode);
int bindings_capture_button(int button);
int bindings_capture_axis(int axis, int value);
void bindings_cancel(void);
/* The rows as they are now, the chosen row and column, and the text for the explanation box. */
int bindings_rows(BindRow *rows, int max, int *row, int *col, char *help, int help_size);

/* The settings file's lines (settings.c): one KEY1_/KEY2_/PAD_<input>=<name> line each. */
int bindings_load_line(const char *name, const char *value);
void bindings_save(FILE *f);

#endif
