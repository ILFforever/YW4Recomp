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

/* Pseudo-scancodes for mouse buttons and wheel directions */
enum {
    MOUSE_BIND_NONE = 0,
    MOUSE_BIND_LEFT = 1001,
    MOUSE_BIND_RIGHT = 1002,
    MOUSE_BIND_MIDDLE = 1003,
    MOUSE_BIND_X1 = 1004,
    MOUSE_BIND_X2 = 1005,
    MOUSE_BIND_WHEEL_UP = 1006,
    MOUSE_BIND_WHEEL_DOWN = 1007,
};

typedef struct {
    const char *group;  /* BUTTONS, D-PAD, LEFT STICK, RIGHT STICK; NULL: the reset row */
    const char *name;
    char cells[BIND_COLS][BIND_NAME];  /* the two keys, then the gamepad input; "-" for none */
    int conflict[BIND_COLS];           /* the cell's key or input also presses another Switch input */
    int value;                         /* a setting, not an input: its value in cells[0], LEFT/RIGHT change it */
} BindRow;

/* The Switch buttons (bits as in svc_hid) and sticks (left x, left y, right x, right y; up and right
 * positive) for keyboard state, mouse buttons/wheel, and gamepad. */
struct SDL_Gamepad;
void bindings_read(const bool *keyboard, uint32_t mouse_buttons, int wheel_up, int wheel_down,
                   struct SDL_Gamepad *gamepad, uint64_t *buttons, int32_t sticks[4]);

/* The connected controller is a Nintendo one (its A is on the right, B at the bottom; others the other way). */
int bindings_pad_nintendo(void);

/* The panel page: settings.h's actions (UP, DOWN, LEFT, RIGHT, ACCEPT, CLEAR) while it shows. */
void bindings_action(int action);
/* Waiting for a key, mouse, or gamepad input to bind: the window passes presses here instead of to the panel.
 * Each returns 1 if it took the press. Five seconds without one, or closing the panel, cancels. */
int bindings_capturing(void);
int bindings_capture_key(int scancode);
int bindings_capture_mouse(int button);
int bindings_capture_wheel(int y);
int bindings_capture_button(int button);
int bindings_capture_axis(int axis, int value);
void bindings_cancel(void);
/* Chooses row r (and column c, -1: as it was), as the mouse over the overlay's Controls table; not while waiting. */
void bindings_select(int r, int c);
/* 1: the controller was used last; the binding rows keep to the controller column (no keyboard key changed by a pad) */
void bindings_pad_only(int on);
/* The rows as they are now, the chosen row and column, and the text for the explanation box. */
int bindings_rows(BindRow *rows, int max, int *row, int *col, char *help, int help_size);
/* The keyboard view: the inputs `key` (an SDL_Scancode or MOUSE_BIND_*) is bound to in a set (0 exploring, 1 battles),
 * as a mask of input numbers; an input's settings-file name ("A", "LS_UP", "DPAD_LEFT"). */
uint32_t bindings_key_inputs(int set, int key);
const char *bindings_input_id(int i);
/* The controller view: the connected controller's style (0 Xbox, 1 PlayStation, 2 Nintendo; Xbox when there is
 * none); the inputs a part of the pad is bound to in a set (a face button by its place on that controller; the
 * D-pad and the sticks: any of their directions); an input's key k (0, 1) by name ("Left Shift", "MOUSE LEFT"), NULL: none. */
enum { BPAD_SOUTH, BPAD_EAST, BPAD_WEST, BPAD_NORTH, BPAD_L1, BPAD_R1, BPAD_L2, BPAD_R2, BPAD_BACK, BPAD_START,
       BPAD_L3, BPAD_R3, BPAD_DPAD, BPAD_LSTICK, BPAD_RSTICK };
int bindings_pad_style(void);
uint32_t bindings_pad_inputs(int set, int part);
const char *bindings_input_key(int set, int i, int k);
/* YW4_HINTS (docs/debugging.md): 0 not set, 1 the keys, 2 + a style: that device for the whole run. */
int bindings_hints_forced(void);
/* What the game sees of the pad's Back button (View / Create / Minus) instead of the button itself: -1 the button,
 * 0 not pressed, 1 pressed. The window holds a press back until it knows it isn't a hold for the quick look. */
void bindings_pad_back(int state);
/* 1: the game sees nothing of the pad (buttons, sticks, triggers); 0: as usual. The window sets it while the quick
 * look shows its controller view, and after it hides until the pad is let go. */
void bindings_pad_lock(int on);


/* The settings file's lines (settings.c): one KEY1_/KEY2_/PAD_<input>=<name> line each, and YW4_MOUSE_SENS. */
int bindings_load_line(const char *name, const char *value);
void bindings_save(FILE *f);

#endif
