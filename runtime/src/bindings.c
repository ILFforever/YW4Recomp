/* The controls (bindings.h): a table of Switch inputs, each with two keyboard keys and one gamepad input,
 * read into the Switch controller state every time the window polls; the panel page that changes them. */
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bindings.h"
#include "settings.h"

/* A gamepad input: a button (SDL_GamepadButton), or a stick or trigger pushed one way. */
enum { PAD_NONE = -1, PAD_AXIS = 100 };
#define AXIS(a, positive) (PAD_AXIS + (a) * 2 + (positive))

typedef struct {
    const char *group, *name, *id;  /* id: the name in the settings file */
    int bit;                        /* the Switch button; -1 for a stick direction */
    int stick, sign;                /* the stick direction: 0 left x, 1 left y, 2 right x, 3 right y; +1/-1 */
    SDL_Scancode keys[BIND_KEYS];   /* the starting binding */
    int pad;
} Input;

static const Input inputs[] = {
    {"BUTTONS", "A", "A", 0, 0, 0, {SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN}, SDL_GAMEPAD_BUTTON_EAST},
    {"BUTTONS", "B", "B", 1, 0, 0, {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_ESCAPE}, SDL_GAMEPAD_BUTTON_SOUTH},
    {"BUTTONS", "X", "X", 2, 0, 0, {SDL_SCANCODE_X}, SDL_GAMEPAD_BUTTON_NORTH},
    {"BUTTONS", "Y", "Y", 3, 0, 0, {SDL_SCANCODE_Y}, SDL_GAMEPAD_BUTTON_WEST},
    {"BUTTONS", "L", "L", 6, 0, 0, {SDL_SCANCODE_Q}, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"BUTTONS", "R", "R", 7, 0, 0, {SDL_SCANCODE_E}, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"BUTTONS", "ZL", "ZL", 8, 0, 0, {SDL_SCANCODE_1}, AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1)},
    {"BUTTONS", "ZR", "ZR", 9, 0, 0, {SDL_SCANCODE_3}, AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)},
    {"BUTTONS", "PLUS", "PLUS", 10, 0, 0, {SDL_SCANCODE_P}, SDL_GAMEPAD_BUTTON_START},
    {"BUTTONS", "MINUS", "MINUS", 11, 0, 0, {SDL_SCANCODE_M}, SDL_GAMEPAD_BUTTON_BACK},
    {"BUTTONS", "LEFT STICK PRESS", "LS_PRESS", 4, 0, 0, {SDL_SCANCODE_LSHIFT}, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"BUTTONS", "RIGHT STICK PRESS", "RS_PRESS", 5, 0, 0, {SDL_SCANCODE_RSHIFT}, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"D-PAD", "UP", "DPAD_UP", 13, 0, 0, {SDL_SCANCODE_UP}, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"D-PAD", "DOWN", "DPAD_DOWN", 15, 0, 0, {SDL_SCANCODE_DOWN}, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"D-PAD", "LEFT", "DPAD_LEFT", 12, 0, 0, {SDL_SCANCODE_LEFT}, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"D-PAD", "RIGHT", "DPAD_RIGHT", 14, 0, 0, {SDL_SCANCODE_RIGHT}, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    /* SDL's stick y grows downwards, the Switch's upwards */
    {"LEFT STICK", "UP", "LS_UP", -1, 1, 1, {SDL_SCANCODE_W}, AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0)},
    {"LEFT STICK", "DOWN", "LS_DOWN", -1, 1, -1, {SDL_SCANCODE_S}, AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1)},
    {"LEFT STICK", "LEFT", "LS_LEFT", -1, 0, -1, {SDL_SCANCODE_A}, AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0)},
    {"LEFT STICK", "RIGHT", "LS_RIGHT", -1, 0, 1, {SDL_SCANCODE_D}, AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1)},
    {"RIGHT STICK", "UP", "RS_UP", -1, 3, 1, {SDL_SCANCODE_I}, AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0)},
    {"RIGHT STICK", "DOWN", "RS_DOWN", -1, 3, -1, {SDL_SCANCODE_K}, AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1)},
    {"RIGHT STICK", "LEFT", "RS_LEFT", -1, 2, -1, {SDL_SCANCODE_J}, AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0)},
    {"RIGHT STICK", "RIGHT", "RS_RIGHT", -1, 2, 1, {SDL_SCANCODE_L}, AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1)},
};
enum { NINPUTS = sizeof inputs / sizeof *inputs, RESET_ROW = NINPUTS };

static struct {
    int keys[BIND_KEYS];  /* SDL_Scancode, 0 for none */
    int pad;
} bound[NINPUTS];
static int bound_ready;

static void reset(void) {
    for (int i = 0; i < NINPUTS; i++) {
        for (int k = 0; k < BIND_KEYS; k++) bound[i].keys[k] = inputs[i].keys[k];
        bound[i].pad = inputs[i].pad;
    }
    bound_ready = 1;
}
static void ready(void) {
    if (!bound_ready) reset();
}

/* ---- gamepad inputs by name: the face buttons by position (south is B on a Switch controller, A on an
 * Xbox one), so a binding means the same button on any pad */

static const struct {
    int pad;
    const char *name;
} pad_names[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, "SOUTH"}, {SDL_GAMEPAD_BUTTON_EAST, "EAST"}, {SDL_GAMEPAD_BUTTON_WEST, "WEST"},
    {SDL_GAMEPAD_BUTTON_NORTH, "NORTH"}, {SDL_GAMEPAD_BUTTON_BACK, "BACK/MINUS"},
    {SDL_GAMEPAD_BUTTON_GUIDE, "HOME"}, {SDL_GAMEPAD_BUTTON_START, "START/PLUS"},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, "L STICK PRESS"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R STICK PRESS"},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L SHOULDER"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R SHOULDER"},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, "D-PAD UP"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "D-PAD DOWN"},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "D-PAD LEFT"}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "D-PAD RIGHT"},
    {SDL_GAMEPAD_BUTTON_MISC1, "SHARE"}, {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, "R PADDLE 1"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, "L PADDLE 1"}, {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, "R PADDLE 2"},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, "L PADDLE 2"}, {SDL_GAMEPAD_BUTTON_TOUCHPAD, "TOUCHPAD"},
    {AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0), "L STICK LEFT"}, {AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1), "L STICK RIGHT"},
    {AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0), "L STICK UP"}, {AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1), "L STICK DOWN"},
    {AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0), "R STICK LEFT"}, {AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1), "R STICK RIGHT"},
    {AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0), "R STICK UP"}, {AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1), "R STICK DOWN"},
    {AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), "L TRIGGER"}, {AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1), "R TRIGGER"},
};

static const char *pad_name(int pad) {
    for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
        if (pad_names[i].pad == pad) return pad_names[i].name;
    return pad == PAD_NONE ? "-" : "?";
}
static int pad_from_name(const char *name) {
    for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
        if (!strcmp(pad_names[i].name, name)) return pad_names[i].pad;
    return PAD_NONE;
}
static const char *key_name(int scancode) {
    const char *n = scancode ? SDL_GetScancodeName((SDL_Scancode)scancode) : "";
    return n && *n ? n : "-";
}

/* ---- reading the controls */

/* How far a gamepad input is pressed, 0..32767. */
static int32_t pad_amount(SDL_Gamepad *g, int pad) {
    if (!g || pad == PAD_NONE) return 0;
    if (pad < PAD_AXIS) return SDL_GetGamepadButton(g, (SDL_GamepadButton)pad) ? 32767 : 0;
    int32_t v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)((pad - PAD_AXIS) / 2));
    if (!((pad - PAD_AXIS) & 1)) v = -v;
    return v < 0 ? 0 : v > 32767 ? 32767 : v;
}

void bindings_read(const bool *keyboard, struct SDL_Gamepad *gamepad, uint64_t *buttons, int32_t sticks[4]) {
    ready();
    uint64_t b = 0;
    int32_t key_stick[4] = {0, 0, 0, 0}, pad_stick[4] = {0, 0, 0, 0};
    for (int i = 0; i < NINPUTS; i++) {
        int key = 0;
        for (int k = 0; k < BIND_KEYS; k++) key |= bound[i].keys[k] && keyboard[bound[i].keys[k]];
        int32_t amount = pad_amount(gamepad, bound[i].pad);
        if (inputs[i].bit >= 0) {
            /* a stick or trigger counts as a button pushed past half way */
            if (key || amount > (bound[i].pad >= PAD_AXIS ? 16000 : 0)) b |= 1ull << inputs[i].bit;
        } else {
            key_stick[inputs[i].stick] += inputs[i].sign * (key ? 32767 : 0);
            pad_stick[inputs[i].stick] += inputs[i].sign * amount;
        }
    }
    for (int i = 0; i < 4; i++) {
        int32_t v = pad_stick[i] > 32767 ? 32767 : pad_stick[i] < -32767 ? -32767 : pad_stick[i];
        int32_t k = key_stick[i] > 32767 ? 32767 : key_stick[i] < -32767 ? -32767 : key_stick[i];
        sticks[i] = v > 4000 || v < -4000 ? v : k;  /* dead zone: the keyboard wins near the centre */
    }
    *buttons = b;
}

/* ---- the panel page */

static int row, col, capturing;
static uint64_t capture_since;

int bindings_capturing(void) {
    if (capturing && SDL_GetTicks() - capture_since > 5000) capturing = 0;
    return capturing;
}
void bindings_cancel(void) { capturing = 0; }

void bindings_action(int action) {
    ready();
    switch (action) {
    case SETTINGS_UP: row = (row + NINPUTS) % (NINPUTS + 1); break;
    case SETTINGS_DOWN: row = (row + 1) % (NINPUTS + 1); break;
    case SETTINGS_LEFT: col = (col + BIND_COLS - 1) % BIND_COLS; break;
    case SETTINGS_RIGHT: col = (col + 1) % BIND_COLS; break;
    case SETTINGS_ACCEPT:
        if (row == RESET_ROW) {
            reset(), settings_save();
        } else {
            capturing = 1, capture_since = SDL_GetTicks();
        }
        break;
    case SETTINGS_CLEAR:
        if (row == RESET_ROW) break;
        if (col < BIND_KEYS) bound[row].keys[col] = 0;
        else bound[row].pad = PAD_NONE;
        settings_save();
        break;
    }
}

int bindings_capture_key(int scancode) {
    if (!bindings_capturing()) return 0;
    capturing = 0;
    if (col < BIND_KEYS) {  /* a key for a key column; the other key of the row cleared if it is the same */
        for (int k = 0; k < BIND_KEYS; k++)
            if (bound[row].keys[k] == scancode) bound[row].keys[k] = 0;
        bound[row].keys[col] = scancode;
        settings_save();
    }  /* a key while waiting for a gamepad input cancels */
    return 1;
}

int bindings_capture_button(int button) {
    if (!bindings_capturing()) return 0;
    capturing = 0;
    if (col == BIND_KEYS) bound[row].pad = button, settings_save();
    return 1;
}

int bindings_capture_axis(int axis, int value) {
    if (!bindings_capturing() || col != BIND_KEYS || (value < 24000 && value > -24000)) return 0;
    capturing = 0;
    bound[row].pad = AXIS(axis, value > 0);
    settings_save();
    return 1;
}

/* The other Switch input that a key or gamepad input also presses, or -1. */
static int also_used(int self, int key, int pad) {
    for (int i = 0; i < NINPUTS; i++) {
        if (i == self) continue;
        for (int k = 0; k < BIND_KEYS; k++)
            if (key && bound[i].keys[k] == key) return i;
        if (pad != PAD_NONE && bound[i].pad == pad) return i;
    }
    return -1;
}

static void input_title(int i, char *s, size_t n) {
    if (inputs[i].group[0] == 'B') snprintf(s, n, "%s", inputs[i].name);
    else snprintf(s, n, "%s %s", inputs[i].group, inputs[i].name);
}

int bindings_rows(BindRow *rows, int max, int *sel_row, int *sel_col, char *help, int help_size) {
    ready();
    int n = 0;
    for (int i = 0; i < NINPUTS && n < max; i++, n++) {
        BindRow *r = &rows[n];
        r->group = inputs[i].group, r->name = inputs[i].name;
        for (int k = 0; k < BIND_KEYS; k++) {
            snprintf(r->cells[k], BIND_NAME, "%s", key_name(bound[i].keys[k]));
            r->conflict[k] = also_used(i, bound[i].keys[k], PAD_NONE) >= 0;
        }
        snprintf(r->cells[BIND_KEYS], BIND_NAME, "%s", pad_name(bound[i].pad));
        r->conflict[BIND_KEYS] = also_used(i, 0, bound[i].pad) >= 0;
    }
    if (n < max) {
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->name = "RESET ALL TO THE STARTING KEYS";
    }
    *sel_row = row, *sel_col = col;
    char title[48], other[48];
    if (row == RESET_ROW) {
        snprintf(help, (size_t)help_size, "PUTS EVERY KEY AND GAMEPAD BUTTON BACK TO HOW IT STARTED. ENTER OR A: RESET.");
        return n;
    }
    input_title(row, title, sizeof title);
    int key = col < BIND_KEYS ? bound[row].keys[col] : 0, pad = col == BIND_KEYS ? bound[row].pad : PAD_NONE;
    int o = also_used(row, key, pad);
    if (o >= 0) input_title(o, other, sizeof other);
    if (bindings_capturing())
        snprintf(help, (size_t)help_size,
                 col < BIND_KEYS ? "PRESS THE KEY FOR %s NOW. A GAMEPAD BUTTON CANCELS."
                                 : "PRESS THE GAMEPAD BUTTON, OR PUSH THE STICK OR TRIGGER, FOR %s NOW. A KEY CANCELS.",
                 title);
    else if (o >= 0)
        snprintf(help, (size_t)help_size, "%s ALSO PRESSES %s. BOTH WORK, BUT THE GAME SEES THEM TOGETHER.",
                 col < BIND_KEYS ? key_name(key) : pad_name(pad), other);
    else
        snprintf(help, (size_t)help_size,
                 "%s: TWO KEYS AND ONE GAMEPAD INPUT. ENTER OR A: CHANGE. DELETE OR X: CLEAR. GAMEPAD FACE BUTTONS "
                 "GO BY POSITION: EAST IS A ON A SWITCH PAD, B ON AN XBOX PAD.",
                 title);
    return n;
}

/* ---- the settings file */

int bindings_load_line(const char *name, const char *value) {
    ready();
    for (int i = 0; i < NINPUTS; i++) {
        char want[32];
        for (int k = 0; k < BIND_KEYS; k++) {
            snprintf(want, sizeof want, "KEY%d_%s", k + 1, inputs[i].id);
            if (!strcmp(name, want)) {
                bound[i].keys[k] = strcmp(value, "-") ? (int)SDL_GetScancodeFromName(value) : 0;
                return 1;
            }
        }
        snprintf(want, sizeof want, "PAD_%s", inputs[i].id);
        if (!strcmp(name, want)) {
            bound[i].pad = pad_from_name(value);
            return 1;
        }
    }
    return 0;
}

void bindings_save(FILE *f) {
    ready();
    fprintf(f, "# Controls: two keys (SDL's key names) and one gamepad input per Switch input; - for none.\n");
    for (int i = 0; i < NINPUTS; i++) {
        for (int k = 0; k < BIND_KEYS; k++) fprintf(f, "KEY%d_%s=%s\n", k + 1, inputs[i].id, key_name(bound[i].keys[k]));
        fprintf(f, "PAD_%s=%s\n", inputs[i].id, pad_name(bound[i].pad));
    }
}
