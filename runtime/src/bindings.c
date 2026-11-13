/* The controls (bindings.h): a table of Switch inputs, each with two keyboard/mouse keys and one gamepad input,
 * read into the Switch controller state every time the window polls; the panel page that changes them;
 * the mouse turning the camera (hook inside f_846260); and the on-screen button icons following the bindings
 * and the device in use: keyboard and mouse, or a PlayStation, Xbox or Nintendo controller (hook on f_a8c3f0). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "battle.h"
#include "bindings.h"
#include "settings.h"
#include "window.h"
#include "yw4fp.h"
#include "yw4load.h"
#include "yw4rt.h"

/* A gamepad input: a button (SDL_GamepadButton), a stick or trigger pushed one way, or a face button by its
 * name (PAD_FACE + 0..3: A, B, X, Y as printed on the connected controller: confirm on a PlayStation pad is
 * cross, on a Nintendo one the right button). */
enum { PAD_NONE = -1, PAD_AXIS = 100, PAD_FACE = 200 };
#define AXIS(a, positive) (PAD_AXIS + (a) * 2 + (positive))

typedef struct {
    const char *group, *name, *id;  /* id: the name in the settings file */
    int bit;                        /* the Switch button; -1 for a stick direction */
    int stick, sign;                /* the stick direction: 0 left x, 1 left y, 2 right x, 3 right y; +1/-1 */
    int keys[BIND_KEYS];            /* the starting binding: SDL_Scancode or MOUSE_BIND_* */
    int pad;
} Input;

/* The starting keys are the owner's own (2026-11-13: "ship my keys as defaults"); the battle set's are below. */
static const Input inputs[] = {
    {"BUTTONS", "A", "A", 0, 0, 0, {SDL_SCANCODE_E, SDL_SCANCODE_RETURN}, PAD_FACE + 0},
    {"BUTTONS", "B", "B", 1, 0, 0, {SDL_SCANCODE_SPACE}, PAD_FACE + 1},
    {"BUTTONS", "X", "X", 2, 0, 0, {SDL_SCANCODE_TAB}, PAD_FACE + 2},
    {"BUTTONS", "Y", "Y", 3, 0, 0, {SDL_SCANCODE_M}, PAD_FACE + 3},
    {"BUTTONS", "L", "L", 6, 0, 0, {SDL_SCANCODE_1}, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"BUTTONS", "R", "R", 7, 0, 0, {SDL_SCANCODE_3}, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"BUTTONS", "ZL", "ZL", 8, 0, 0, {SDL_SCANCODE_Q}, AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1)},
    {"BUTTONS", "ZR", "ZR", 9, 0, 0, {SDL_SCANCODE_F}, AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)},
    {"BUTTONS", "PLUS", "PLUS", 10, 0, 0, {SDL_SCANCODE_P, SDL_SCANCODE_ESCAPE}, SDL_GAMEPAD_BUTTON_START},
    {"BUTTONS", "MINUS", "MINUS", 11, 0, 0, {SDL_SCANCODE_X}, SDL_GAMEPAD_BUTTON_BACK},
    {"BUTTONS", "LEFT STICK PRESS", "LS_PRESS", 4, 0, 0, {SDL_SCANCODE_LSHIFT}, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"BUTTONS", "RIGHT STICK PRESS", "RS_PRESS", 5, 0, 0, {0, SDL_SCANCODE_R}, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
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
/* The controls page's rows: the mouse's two sensitivities, its buttons (a page of their own in the overlay), the
 * vibration toggle, which set is shown, the inputs, the reset row. */
enum { NINPUTS = sizeof inputs / sizeof *inputs, NMOUSE = 2,
       SENS_ROW = 0,
       MOUSE_ROW = SENS_ROW + NMOUSE,
       RUMBLE_ROW = MOUSE_ROW + 1,
       LAYOUT_ROW = RUMBLE_ROW + 1,
       SET_ROW = LAYOUT_ROW + 1,
       FIRST_INPUT = SET_ROW + 1,
       RESET_ROW = FIRST_INPUT + NINPUTS, NROWS };

/* Two sets of keys: one while exploring, one in battles (battle.c says which), so a key can do one thing on the
 * field and another in a battle. The battle set starts as a copy of the exploring one, with mouse wheel up and
 * down bound to character selection (D-pad up and down); saved with a BATTLE_ prefix. */
enum { SET_FIELD, SET_BATTLE, NSETS };
typedef struct {
    int keys[BIND_KEYS];  /* SDL_Scancode or MOUSE_BIND_*, 0 for none */
    int pad;
} Bound;
static Bound sets[NSETS][NINPUTS];
static int edit_set;  /* the set the controls page shows and changes */
#define bound (sets[edit_set])
static int bound_ready, battle_set_known;  /* the battle set was read from the settings file or copied */

/* The battle set's starting keys (the owner's battle set, 2026-11-13), in the order of inputs[]; the controller's
 * inputs are exploring's but for X and Y (battle_starting_set) */
static const int battle_keys[NINPUTS][BIND_KEYS] = {
    {SDL_SCANCODE_E, SDL_SCANCODE_RETURN},      /* A */
    {SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT},  /* B */
    {SDL_SCANCODE_Q},                           /* X */
    {0, SDL_SCANCODE_U},                        /* Y (and the left button: mouse_starting) */
    {0},                                        /* L */
    {SDL_SCANCODE_E},                           /* R */
    {0, SDL_SCANCODE_O},                        /* ZL (and the right button) */
    {SDL_SCANCODE_F},                           /* ZR */
    {SDL_SCANCODE_R, SDL_SCANCODE_P},           /* PLUS */
    {SDL_SCANCODE_C},                           /* MINUS */
    {SDL_SCANCODE_TAB},                         /* LS_PRESS */
    {0},                                        /* RS_PRESS (the wheel press) */
    {SDL_SCANCODE_UP},                          /* DPAD_UP (and wheel up) */
    {SDL_SCANCODE_DOWN},                        /* DPAD_DOWN (and wheel down) */
    {SDL_SCANCODE_LEFT},  {SDL_SCANCODE_RIGHT}, /* DPAD_LEFT, DPAD_RIGHT */
    {SDL_SCANCODE_W}, {SDL_SCANCODE_S}, {SDL_SCANCODE_A}, {SDL_SCANCODE_D},  /* LS */
    {SDL_SCANCODE_I}, {SDL_SCANCODE_K}, {SDL_SCANCODE_J}, {SDL_SCANCODE_L},  /* RS */
};

/* The mouse's own bindings (owner, 2026-11-13: the Mouse buttons page): in each set, the Switch input each part of
 * the mouse presses (an index of inputs[], -1 none), one each. Kept apart from an input's two keys, so binding the
 * mouse never pushes a key out. Only the usual parts (owner: no side buttons). Saved as MOUSE_LEFT=Y and so on. */
static const int mouse_code[BMOUSE_PARTS] = {MOUSE_BIND_LEFT, MOUSE_BIND_RIGHT, MOUSE_BIND_MIDDLE, MOUSE_BIND_WHEEL_UP,
                                             MOUSE_BIND_WHEEL_DOWN};
static const char *const mouse_save[BMOUSE_PARTS] = {"MOUSE_LEFT", "MOUSE_RIGHT", "MOUSE_MID", "MOUSE_WHEEL_UP",
                                                     "MOUSE_WHEEL_DOWN"};
static int mouse_map[NSETS][BMOUSE_PARTS];
static int mouse_part(int code) {  /* the part a MOUSE_BIND_* is, or -1 (the side buttons, a key) */
    for (int p = 0; p < BMOUSE_PARTS; p++)
        if (mouse_code[p] == code) return p;
    return -1;
}
static int input_by_id(const char *id) {
    for (int i = 0; i < NINPUTS; i++)
        if (!strcmp(inputs[i].id, id)) return i;
    return -1;
}
/* the starting mouse (the owner's, 2026-11-13): exploring, left interacts (A), right is the watch scan (ZL), the
 * wheel press scans, turning the wheel changes the minimap's size (R); battles, left attacks (Y), right aims (ZL),
 * the wheel press is the right stick press, the wheel picks the character (D-pad up and down) */
static void mouse_starting(int set) {
    static const char *const field[BMOUSE_PARTS] = {"A", "ZL", "RS_PRESS", "R", "R"};
    static const char *const battle[BMOUSE_PARTS] = {"Y", "ZL", "RS_PRESS", "DPAD_UP", "DPAD_DOWN"};
    for (int p = 0; p < BMOUSE_PARTS; p++) {
        const char *id = (set == SET_BATTLE ? battle : field)[p];
        mouse_map[set][p] = id ? input_by_id(id) : -1;
    }
}

static void battle_starting_set(void) {
    for (int i = 0; i < NINPUTS; i++) {
        for (int k = 0; k < BIND_KEYS; k++) sets[SET_BATTLE][i].keys[k] = battle_keys[i][k];
        sets[SET_BATTLE][i].pad = inputs[i].pad;
    }
    /* the owner's battle pad (2026-11-14): Y (attack) on the left face button, X on the top one, by place on every
     * pad (Xbox X / PlayStation square / Nintendo Y attacks); the rest as when exploring */
    sets[SET_BATTLE][input_by_id("X")].pad = SDL_GAMEPAD_BUTTON_NORTH;
    sets[SET_BATTLE][input_by_id("Y")].pad = SDL_GAMEPAD_BUTTON_WEST;
    mouse_starting(SET_BATTLE);
}

/* the battle row's COPY THE EXPLORING KEYS: the exploring set and its mouse, with the wheel on character selection */
static void copy_exploring_to_battle(void) {
    memcpy(sets[SET_BATTLE], sets[SET_FIELD], sizeof sets[SET_FIELD]);
    memcpy(mouse_map[SET_BATTLE], mouse_map[SET_FIELD], sizeof mouse_map[SET_FIELD]);
    mouse_map[SET_BATTLE][BMOUSE_WHEEL_UP] = input_by_id("DPAD_UP");
    mouse_map[SET_BATTLE][BMOUSE_WHEEL_DOWN] = input_by_id("DPAD_DOWN");
}

static void reset_set(int set) {
    if (set == SET_BATTLE) {
        battle_starting_set();
        return;
    }
    for (int i = 0; i < NINPUTS; i++) {
        for (int k = 0; k < BIND_KEYS; k++) sets[set][i].keys[k] = inputs[i].keys[k];
        sets[set][i].pad = inputs[i].pad;
    }
    mouse_starting(set);
}
static void reset(void) {
    for (int s = 0; s < NSETS; s++) reset_set(s);
    bound_ready = 1;
}
static void ready(void) {
    if (!bound_ready) reset();
}
/* After the settings file: a battle set it didn't have gets the battle set's starting keys. */
static void sets_ready(void) {
    ready();
    if (!battle_set_known) {
        battle_starting_set();
        battle_set_known = 1;
    }
}

/* The Controls page's keyboard view (gamemenu.c): the inputs a key or mouse button is bound to in a set (0 exploring,
 * 1 battles), as a mask of input numbers, and an input's name as in the settings file ("A", "LS_UP"). */
uint32_t bindings_key_inputs(int set, int key) {
    sets_ready();
    if (set < 0 || set >= NSETS || !key) return 0;
    int part = mouse_part(key);  /* a mouse part: its own binding */
    if (part >= 0) return mouse_map[set][part] >= 0 ? 1u << mouse_map[set][part] : 0;
    uint32_t m = 0;
    for (int i = 0; i < NINPUTS && i < 32; i++)
        for (int k = 0; k < BIND_KEYS; k++)
            if (sets[set][i].keys[k] == key) m |= 1u << i;
    return m;
}

const char *bindings_input_id(int i) { return i >= 0 && i < NINPUTS ? inputs[i].id : ""; }

/* The controller view: the inputs a part of the pad (BPAD_*) is bound to in a set, as bindings_key_inputs; a face
 * button is the one at that place on the connected controller (resolve). And the keys of an input (k: 0, 1), for
 * the view's "the same on the keyboard". */
static int resolve(int pad);
uint32_t bindings_pad_inputs(int set, int part) {
    static const int buttons[] = {[BPAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [BPAD_EAST] = SDL_GAMEPAD_BUTTON_EAST,
                                  [BPAD_WEST] = SDL_GAMEPAD_BUTTON_WEST, [BPAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH,
                                  [BPAD_L1] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, [BPAD_R1] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                                  [BPAD_L2] = AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1),
                                  [BPAD_R2] = AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1),
                                  [BPAD_BACK] = SDL_GAMEPAD_BUTTON_BACK, [BPAD_START] = SDL_GAMEPAD_BUTTON_START,
                                  [BPAD_L3] = SDL_GAMEPAD_BUTTON_LEFT_STICK, [BPAD_R3] = SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    sets_ready();
    if (set < 0 || set >= NSETS || part < 0 || part > BPAD_RSTICK) return 0;
    uint32_t m = 0;
    for (int i = 0; i < NINPUTS && i < 32; i++) {
        int p = resolve(sets[set][i].pad), on;
        if (p == PAD_NONE) continue;
        int axis = p >= PAD_AXIS && p < PAD_FACE ? (p - PAD_AXIS) / 2 : -1;
        if (part == BPAD_DPAD) on = p >= SDL_GAMEPAD_BUTTON_DPAD_UP && p <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
        else if (part == BPAD_LSTICK) on = axis == SDL_GAMEPAD_AXIS_LEFTX || axis == SDL_GAMEPAD_AXIS_LEFTY;
        else if (part == BPAD_RSTICK) on = axis == SDL_GAMEPAD_AXIS_RIGHTX || axis == SDL_GAMEPAD_AXIS_RIGHTY;
        else on = p == buttons[part];
        if (on) m |= 1u << i;
    }
    return m;
}

static const char *key_name(int scancode);
const char *bindings_input_key(int set, int i, int k) {
    sets_ready();
    if (set < 0 || set >= NSETS || i < 0 || i >= NINPUTS || k < 0 || k >= BIND_KEYS || !sets[set][i].keys[k]) return NULL;
    return key_name(sets[set][i].keys[k]);
}

/* ---- mouse sensitivity, left-right and up-down: the controls page's first rows, saved as YW4_MOUSE_SENS_X and
 * YW4_MOUSE_SENS_Y (the environment wins; YW4_MOUSE_SENS, the older single one, sets both) */

static const char *const sens_values[] = {"0.5", "0.75", "1.0", "1.25", "1.5", "2.0", "3.0"};
enum { NSENS = sizeof sens_values / sizeof *sens_values, SENS_DEFAULT = 2 };
static const char *const sens_env[2] = {"YW4_MOUSE_SENS_X", "YW4_MOUSE_SENS_Y"};
static int sens_choice[2] = {SENS_DEFAULT, SENS_DEFAULT}, sens_env_read;

/* The nearest choice to a value. */
static int sens_from(const char *value) {
    float v = (float)atof(value);
    int best = SENS_DEFAULT;
    for (int i = 0; i < NSENS; i++) {
        float d = v - (float)atof(sens_values[i]), b = v - (float)atof(sens_values[best]);
        if (d * d < b * b) best = i;
    }
    return best;
}

/* axis: 0 left-right, 1 up-down */
static int sens_index(int axis) {
    if (!sens_env_read) {
        sens_env_read = 1;
        const char *both = getenv("YW4_MOUSE_SENS");
        for (int a = 0; a < 2; a++) {
            const char *e = getenv(sens_env[a]);
            if (e && *e) sens_choice[a] = sens_from(e);
            else if (both && *both) sens_choice[a] = sens_from(both);
        }
    }
    return sens_choice[axis];
}

static float mouse_sens(int axis) { return 0.02f * (float)atof(sens_values[sens_index(axis)]); }

/* ---- gamepad inputs by name, for the settings file: the face buttons by position (south is B on a Switch
 * controller, A on an Xbox one), so a binding means the same button on any pad. The panel shows the
 * controller's own names (pad_label). */

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
    {PAD_FACE + 0, "A BUTTON"}, {PAD_FACE + 1, "B BUTTON"}, {PAD_FACE + 2, "X BUTTON"}, {PAD_FACE + 3, "Y BUTTON"},
};

static const char *pad_name(int pad) {
    for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
        if (pad_names[i].pad == pad) return pad_names[i].name;
    return pad == PAD_NONE ? "-" : "?";
}
/* What the connected controller prints on its buttons (the settings file keeps the position names above). */
enum { STYLE_XBOX, STYLE_PLAYSTATION, STYLE_NINTENDO };
static int pad_style;           /* the connected controller's (bindings_read); Xbox names when there is none */
static int ps_model = 5;        /* the PlayStation pad last connected: 3, 4, 5 (a DualSense when none was) */
static int xbox360;             /* the Xbox-style pad last connected reports as an Xbox 360 one (Back, Start) */
static int using_pad, had_pad;  /* 1 after a gamepad connection or input, 0 after a key or mouse input */
/* CONTROLLER NINTENDO ICONS (YW4_NINTENDO_LAYOUT): a pad that reports as an Xbox one gets the Switch's button icons
 * and names. Switch-style pads in their X-input mode report as a Microsoft Xbox 360 pad (vendor 045e, product 028e;
 * tools/dev/padtest), so nothing tells them apart: the player says so here. The buttons keep their places (owner,
 * 2026-11-13: swap the hints, not the buttons): the bottom one still confirms, and its hint shows the B printed on
 * it. pad_style stays what the pad reports (which button does what); hint_style is what the icons follow. */
static int nintendo_layout;
static int hint_style(void) { return nintendo_layout && pad_style == STYLE_XBOX ? STYLE_NINTENDO : pad_style; }

int bindings_pad_nintendo(void) { return pad_style == STYLE_NINTENDO; }  /* which button confirms: by place */
int bindings_pad_style(void) { return hint_style(); }                    /* the controller drawn, its names */

/* YW4_HINTS: the device the button icons (and the overlay's views) follow for the whole run, for screenshots of
 * each style without a controller: 0 not set, 1 the keys, 2 + a style (Xbox, PlayStation, Nintendo). */
int bindings_hints_forced(void) {
    static int hint_env = -1;
    if (hint_env < 0) {
        const char *h = getenv("YW4_HINTS");
        hint_env = !h || !*h ? 0 : !strcmp(h, "key") ? 1 : !strcmp(h, "xbox") ? 2 : !strcmp(h, "ps") ? 3 : !strcmp(h, "nx") ? 4 : 0;
    }
    return hint_env;
}

/* The quick look's hold (window.c): what the game is to see of the pad's Back button (View / Create / Minus)
 * instead of the button itself: -1 the button, 0 not pressed, 1 pressed. */
static int back_shown = -1;
void bindings_pad_back(int state) { __atomic_store_n(&back_shown, state, __ATOMIC_RELEASE); }

/* The quick look's controller view (window.c): the whole pad is kept from the game while it shows, so a button
 * pressed to see it light up there does nothing in the game. */
static int pad_locked;
void bindings_pad_lock(int on) { __atomic_store_n(&pad_locked, on, __ATOMIC_RELEASE); }

/* A face button by name as the button it is on the connected controller; anything else as it is. */
static int resolve(int pad) {
    static const int nintendo[4] = {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH,
                                    SDL_GAMEPAD_BUTTON_WEST},
                     others[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                  SDL_GAMEPAD_BUTTON_NORTH};
    if (pad < PAD_FACE || pad > PAD_FACE + 3) return pad;
    return (pad_style == STYLE_NINTENDO ? nintendo : others)[pad - PAD_FACE];
}

static const char *pad_label(int pad) {
    pad = resolve(pad);
    static const struct {
        int pad;
        const char *name[3];  /* Xbox, PlayStation, Nintendo */
    } labels[] = {
        /* \x01..\x08: the panel font's arrows (up, down, left, right) and PlayStation shapes (hud.c) */
        {SDL_GAMEPAD_BUTTON_SOUTH, {"A", "\x08", "B"}}, {SDL_GAMEPAD_BUTTON_EAST, {"B", "\x06", "A"}},
        {SDL_GAMEPAD_BUTTON_WEST, {"X", "\x07", "Y"}}, {SDL_GAMEPAD_BUTTON_NORTH, {"Y", "\x05", "X"}},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, {"D-PAD \x01", "D-PAD \x01", "D-PAD \x01"}},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, {"D-PAD \x02", "D-PAD \x02", "D-PAD \x02"}},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, {"D-PAD \x03", "D-PAD \x03", "D-PAD \x03"}},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, {"D-PAD \x04", "D-PAD \x04", "D-PAD \x04"}},
        {AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0), {"L STICK \x01", "L STICK \x01", "L STICK \x01"}},
        {AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1), {"L STICK \x02", "L STICK \x02", "L STICK \x02"}},
        {AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0), {"L STICK \x03", "L STICK \x03", "L STICK \x03"}},
        {AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1), {"L STICK \x04", "L STICK \x04", "L STICK \x04"}},
        {AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0), {"R STICK \x01", "R STICK \x01", "R STICK \x01"}},
        {AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1), {"R STICK \x02", "R STICK \x02", "R STICK \x02"}},
        {AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0), {"R STICK \x03", "R STICK \x03", "R STICK \x03"}},
        {AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1), {"R STICK \x04", "R STICK \x04", "R STICK \x04"}},
        {SDL_GAMEPAD_BUTTON_BACK, {"VIEW", "SHARE", "MINUS"}}, {SDL_GAMEPAD_BUTTON_START, {"MENU", "OPTIONS", "PLUS"}},
        {SDL_GAMEPAD_BUTTON_GUIDE, {"GUIDE", "PS", "HOME"}},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, {"LB", "L1", "L"}}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, {"RB", "R1", "R"}},
        {AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), {"LT", "L2", "ZL"}},
        {AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1), {"RT", "R2", "ZR"}},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, {"L STICK PRESS", "L3", "L STICK PRESS"}},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, {"R STICK PRESS", "R3", "R STICK PRESS"}},
    };
    for (size_t i = 0; i < sizeof labels / sizeof *labels; i++)
        if (labels[i].pad == pad) return labels[i].name[hint_style()];
    return pad_name(pad);  /* the D-pad, the sticks' directions: the same on every controller */
}

static int pad_from_name(const char *name) {
    for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
        if (!strcmp(pad_names[i].name, name)) return pad_names[i].pad;
    return PAD_NONE;
}

static const char *key_name(int scancode) {
    switch (scancode) {
    case MOUSE_BIND_LEFT: return "MOUSE LEFT";
    case MOUSE_BIND_RIGHT: return "MOUSE RIGHT";
    case MOUSE_BIND_MIDDLE: return "MOUSE MID";
    case MOUSE_BIND_X1: return "MOUSE 4";
    case MOUSE_BIND_X2: return "MOUSE 5";
    case MOUSE_BIND_WHEEL_UP: return "WHEEL UP";
    case MOUSE_BIND_WHEEL_DOWN: return "WHEEL DOWN";
    default: {
        const char *n = scancode ? SDL_GetScancodeName((SDL_Scancode)scancode) : "";
        return n && *n ? n : "-";
    }
    }
}

static const char *key_save_name(int sc) {
    switch (sc) {
    case MOUSE_BIND_LEFT: return "MOUSE_LEFT";
    case MOUSE_BIND_RIGHT: return "MOUSE_RIGHT";
    case MOUSE_BIND_MIDDLE: return "MOUSE_MID";
    case MOUSE_BIND_X1: return "MOUSE_4";
    case MOUSE_BIND_X2: return "MOUSE_5";
    case MOUSE_BIND_WHEEL_UP: return "WHEEL_UP";
    case MOUSE_BIND_WHEEL_DOWN: return "WHEEL_DOWN";
    default: return key_name(sc);
    }
}

static int key_from_name(const char *name) {
    if (!strcmp(name, "-")) return 0;
    if (!strcmp(name, "MOUSE_LEFT") || !strcmp(name, "MOUSE LEFT")) return MOUSE_BIND_LEFT;
    if (!strcmp(name, "MOUSE_RIGHT") || !strcmp(name, "MOUSE RIGHT")) return MOUSE_BIND_RIGHT;
    if (!strcmp(name, "MOUSE_MID") || !strcmp(name, "MOUSE MIDDLE") || !strcmp(name, "MOUSE_MIDDLE")) return MOUSE_BIND_MIDDLE;
    if (!strcmp(name, "MOUSE_4") || !strcmp(name, "MOUSE 4")) return MOUSE_BIND_X1;
    if (!strcmp(name, "MOUSE_5") || !strcmp(name, "MOUSE 5")) return MOUSE_BIND_X2;
    if (!strcmp(name, "WHEEL_UP") || !strcmp(name, "WHEEL UP")) return MOUSE_BIND_WHEEL_UP;
    if (!strcmp(name, "WHEEL_DOWN") || !strcmp(name, "WHEEL DOWN")) return MOUSE_BIND_WHEEL_DOWN;
    return (int)SDL_GetScancodeFromName(name);
}

/* ---- reading the controls */

/* How far a gamepad input is pressed, 0..32767. */
static int32_t pad_amount(SDL_Gamepad *g, int pad) {
    pad = resolve(pad);
    if (!g || pad == PAD_NONE) return 0;
    if (__atomic_load_n(&pad_locked, __ATOMIC_ACQUIRE)) return 0;
    if (pad == SDL_GAMEPAD_BUTTON_BACK) {  /* held back while the window waits to see a press or a hold */
        int shown = __atomic_load_n(&back_shown, __ATOMIC_ACQUIRE);
        if (shown >= 0) return shown ? 32767 : 0;
    }
    if (pad < PAD_AXIS) return SDL_GetGamepadButton(g, (SDL_GamepadButton)pad) ? 32767 : 0;
    int32_t v = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)((pad - PAD_AXIS) / 2));
    if (!((pad - PAD_AXIS) & 1)) v = -v;
    return v < 0 ? 0 : v > 32767 ? 32767 : v;
}

void bindings_read(const bool *keyboard, uint32_t mouse_buttons, int wheel_up, int wheel_down,
                   struct SDL_Gamepad *gamepad, uint64_t *buttons, int32_t sticks[4]) {
    sets_ready();
    int battle = battle_active();
    const Bound *use = sets[battle ? SET_BATTLE : SET_FIELD];
    static int layout_env_read;
    if (!layout_env_read) {  /* YW4_NINTENDO_LAYOUT from the environment, with or without a settings file line */
        const char *e = getenv("YW4_NINTENDO_LAYOUT");
        if (e && *e) nintendo_layout = !strcmp(e, "1") || !strcmp(e, "ON") || !strcmp(e, "on");
        layout_env_read = 1;
    }
    static int model_env = -1;  /* YW4_PS_MODEL=3/4/5: that PlayStation model's names, for tests without the pad */
    if (model_env < 0) {
        const char *e = getenv("YW4_PS_MODEL");
        model_env = e && (atoi(e) == 3 || atoi(e) == 4 || atoi(e) == 5) ? atoi(e) : 0;
        if (model_env) ps_model = model_env;
    }
    static int x360_env = -1;  /* YW4_XBOX360=1 (0): an Xbox 360 pad's names (or not), for tests without the pad */
    if (x360_env < 0) {
        const char *e = getenv("YW4_XBOX360");
        x360_env = e && *e ? 1 : 0;
        if (x360_env) xbox360 = atoi(e) != 0;
    }
    if (gamepad) {
        SDL_GamepadType t = SDL_GetGamepadType(gamepad);
        if (!model_env && (t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5))
            ps_model = t == SDL_GAMEPAD_TYPE_PS3 ? 3 : t == SDL_GAMEPAD_TYPE_PS4 ? 4 : 5;
        /* an Xbox-style pad: a 360 one (or one that says it is: many third-party pads) has Back and Start */
        if (!x360_env && t != SDL_GAMEPAD_TYPE_PS3 && t != SDL_GAMEPAD_TYPE_PS4 && t != SDL_GAMEPAD_TYPE_PS5 &&
            !(t >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && t <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR))
            xbox360 = t == SDL_GAMEPAD_TYPE_XBOX360;
        pad_style = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? STYLE_PLAYSTATION
                    : t >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && t <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
                        ? STYLE_NINTENDO
                        : STYLE_XBOX;
        if (!had_pad) using_pad = 1, had_pad = 1;
    } else {
        using_pad = 0, had_pad = 0;
    }
    uint64_t b = 0;
    int32_t key_stick[4] = {0, 0, 0, 0}, pad_stick[4] = {0, 0, 0, 0};
    int any_key = 0, any_pad = 0;
    /* the mouse's parts held now, and the Switch input each presses in this set (mouse_map) */
    const int held[BMOUSE_PARTS] = {(mouse_buttons & SDL_BUTTON_LMASK) != 0, (mouse_buttons & SDL_BUTTON_RMASK) != 0,
                                    (mouse_buttons & SDL_BUTTON_MMASK) != 0, wheel_up != 0, wheel_down != 0};
    const int *mouse = mouse_map[battle ? SET_BATTLE : SET_FIELD];
    for (int i = 0; i < NINPUTS; i++) {
        int key = 0;
        for (int k = 0; k < BIND_KEYS; k++) {
            int sc = use[i].keys[k];
            if (sc && keyboard && sc < SDL_SCANCODE_COUNT) key |= keyboard[sc];
        }
        for (int p = 0; p < BMOUSE_PARTS; p++)
            if (mouse[p] == i && held[p]) key = 1;
        any_key |= key;
        int32_t amount = pad_amount(gamepad, use[i].pad);
        if (inputs[i].bit >= 0) {
            /* a stick or trigger counts as a button pushed past half way */
            int pressed = amount > (use[i].pad >= PAD_AXIS ? 16000 : 0);
            any_pad |= pressed;
            if (key || pressed) b |= 1ull << inputs[i].bit;
        } else {
            key_stick[inputs[i].stick] += inputs[i].sign * (key ? 32767 : 0);
            pad_stick[inputs[i].stick] += inputs[i].sign * amount;
        }
    }
    for (int i = 0; i < 4; i++) {
        int32_t v = pad_stick[i] > 32767 ? 32767 : pad_stick[i] < -32767 ? -32767 : pad_stick[i];
        int32_t k = key_stick[i] > 32767 ? 32767 : key_stick[i] < -32767 ? -32767 : key_stick[i];
        if (v > 4000 || v < -4000) any_pad = 1;
        sticks[i] = v > 4000 || v < -4000 ? v : k;  /* dead zone: the keyboard wins near the centre */
    }
    if (any_pad) using_pad = 1;
    else if (any_key) using_pad = 0;
    int hint_env = bindings_hints_forced();
    if (hint_env == 1) using_pad = 0;
    else if (hint_env >= 2) using_pad = 1, pad_style = hint_env - 2;
    *buttons = b;
}

/* ---- the mouse turns the camera (hook inside f_846260 at main+0x846bac, overrides.toml)
 *
 * f_846260 (x22: the field camera's pad state) still has the PC build's mouse path: at 0x846bac it reads a
 * mouse movement (floats at +0xcc, +0xd0); if that is nonzero and camera input is allowed (bytes +0x4ba and
 * +0x4bb both 0), it turns the camera by it, scaled by the game's own mouse camera speed (main+0x56b9130),
 * instead of by the right stick (+0xc0). The Switch build never writes +0xcc/+0xd0, so they stay 0 and only
 * the stick path runs. The hook writes the window's mouse movement since the last frame there, every time:
 * nothing else clears it. While the game locks the camera (+0x4ba or +0x4bb set) the movement is dropped. */

int yw4_hook_mouse_look(CPUState *ctx) {
    uint64_t cam = ctx->x[22];
    float dx, dy;
    window_mouse_motion(&dx, &dy);
    if (!cam) return 0;
    if (ld8(cam + 0x4ba) | ld8(cam + 0x4bb)) dx = dy = 0.0f;
    st32(cam + 0xcc, bits_f32(dx * mouse_sens(0)));
    st32(cam + 0xd0, bits_f32(-dy * mouse_sens(1)));  /* the window's y grows downwards */
    if (dx != 0.0f || dy != 0.0f) {
        st8(cam + 0x48a, 1);  /* what the game sets when the right stick moves the camera */
        st8(cam + 0x48b, 1);
        using_pad = 0;
    }
    return 0;
}

/* ---- on-screen button hints (hook on f_a8c3f0, main+0xa8c3f0; decision 0042)
 *
 * Button pictures in text are font icons ("gaiji"), named in the text as Switch tags such as
 * [$gaiji_bt02_s000]. The game loads five icon sheets (romfs data/nx/font/font.cpk) into a list at
 * [main+0x58654f8] (count at main+0x5865500):
 *   0: gaiji_ps4.g4tx     PlayStation         (gaiji_bt01..bt12, gaiji_key_*, gaiji_l*, gaiji_r*)
 *   1: gaiji_xinput.g4tx  Xbox                (the same 25 names)
 *   2: gaiji_key.g4tx     keyboard and mouse  (gaiji_a.., gaiji_space, gaiji_mouse_l, ...)
 *   3: gaiji_yw4.g4tx     the game's own icons
 *   4: gaiji_nx.g4tx      Switch              (gaiji_bt01_s000, ..., gaiji_lst_s100, ...)
 * f_a8c3f0 (x0 a sheet, x2 the requests, w3 their count) draws the requests it finds in that sheet, if the
 * sheet's byte +0x1c is set; a request is 0x38 bytes with the CRC-32 of its icon name at +0. It runs for each
 * sheet in turn, so before it runs for sheet 0 we switch sheets 0..2 on or off and swap each Switch button's
 * name for the icon of what is bound to that button now: a key, or the button on the connected controller.
 * Text already on screen follows on the next frame.
 *
 * btNN numbering: the game's own table of button icons (menu_nx_gaiji_config.cfg.bin) lists them in Sony's
 * pad bit order (select, L3, R3, start, up, right, down, left, L2, R2, L1, R1, triangle, circle, cross,
 * square), and every sheet uses the same numbers: bt01 is the top face button, bt02 the right one (Switch A),
 * bt03 the bottom one (Switch B), bt04 the left one. */

static uint32_t gaiji_crc(const char *s) {
    uint32_t c = ~0u;
    for (; *s; s++) c = crc32_update(c, (uint8_t)*s, 1, 0xedb88320u);
    return ~c;
}

/* The icon in gaiji_key.g4tx for a key or mouse binding, or NULL. */
static const char *key_gaiji(int sc) {
    switch (sc) {
    case MOUSE_BIND_LEFT: return "gaiji_mouse_l";
    case MOUSE_BIND_RIGHT: return "gaiji_mouse_r";
    case MOUSE_BIND_MIDDLE: return "gaiji_mouse_c";
    case MOUSE_BIND_X1: return "gaiji_mouse_4";
    case MOUSE_BIND_X2: return "gaiji_mouse_5";
    case MOUSE_BIND_WHEEL_UP: return "gaiji_mouse_scr_up";
    case MOUSE_BIND_WHEEL_DOWN: return "gaiji_mouse_scr_dwn";
    }

    static const char *const letters[26] = {
        "gaiji_a", "gaiji_b", "gaiji_c", "gaiji_d", "gaiji_e", "gaiji_f", "gaiji_g", "gaiji_h", "gaiji_i",
        "gaiji_j", "gaiji_k", "gaiji_l_key", "gaiji_m", "gaiji_n", "gaiji_o", "gaiji_p", "gaiji_q", "gaiji_r_key",
        "gaiji_s", "gaiji_t", "gaiji_u", "gaiji_v", "gaiji_w", "gaiji_x", "gaiji_y", "gaiji_z",
    };
    static const char *const digits[10] = {
        "gaiji_1", "gaiji_2", "gaiji_3", "gaiji_4", "gaiji_5",
        "gaiji_6", "gaiji_7", "gaiji_8", "gaiji_9", "gaiji_0",
    };
    static const char *const fn_keys[12] = {
        "gaiji_f1", "gaiji_f2", "gaiji_f3", "gaiji_f4", "gaiji_f5", "gaiji_f6",
        "gaiji_f7", "gaiji_f8", "gaiji_f9", "gaiji_f10", "gaiji_f11", "gaiji_f12",
    };
    static const char *const numpad[10] = {
        "gaiji_num0", "gaiji_num1", "gaiji_num2", "gaiji_num3", "gaiji_num4",
        "gaiji_num5", "gaiji_num6", "gaiji_num7", "gaiji_num8", "gaiji_num9",
    };

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return letters[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return digits[sc - SDL_SCANCODE_1];
    if (sc == SDL_SCANCODE_0) return digits[9];
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return fn_keys[sc - SDL_SCANCODE_F1];
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return numpad[sc - SDL_SCANCODE_KP_1];
    if (sc == SDL_SCANCODE_KP_0) return numpad[0];

    switch (sc) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return "gaiji_enter";
    case SDL_SCANCODE_ESCAPE: return "gaiji_esc";
    case SDL_SCANCODE_BACKSPACE: return "gaiji_backspace";
    case SDL_SCANCODE_TAB: return "gaiji_tab";
    case SDL_SCANCODE_SPACE: return "gaiji_space";
    case SDL_SCANCODE_CAPSLOCK: return "gaiji_capslock";
    case SDL_SCANCODE_LSHIFT: return "gaiji_shift_l";
    case SDL_SCANCODE_RSHIFT: return "gaiji_shift_r";
    case SDL_SCANCODE_LCTRL: return "gaiji_ctrl_l";
    case SDL_SCANCODE_RCTRL: return "gaiji_ctrl_r";
    case SDL_SCANCODE_LALT: return "gaiji_alt_l";
    case SDL_SCANCODE_RALT: return "gaiji_alt_r";
    case SDL_SCANCODE_UP: return "gaiji_up";
    case SDL_SCANCODE_DOWN: return "gaiji_down";
    case SDL_SCANCODE_LEFT: return "gaiji_left";
    case SDL_SCANCODE_RIGHT: return "gaiji_right";
    case SDL_SCANCODE_INSERT: return "gaiji_insert";
    case SDL_SCANCODE_DELETE: return "gaiji_delete";
    case SDL_SCANCODE_HOME: return "gaiji_home";
    case SDL_SCANCODE_END: return "gaiji_end";
    case SDL_SCANCODE_PAGEUP: return "gaiji_pageup";
    case SDL_SCANCODE_PAGEDOWN: return "gaiji_pagedown";
    case SDL_SCANCODE_PAUSE: return "gaiji_pause";
    case SDL_SCANCODE_SCROLLLOCK: return "gaiji_scrolllock";
    case SDL_SCANCODE_NUMLOCKCLEAR: return "gaiji_numlock";
    case SDL_SCANCODE_MINUS: return "gaiji_symbol00";
    case SDL_SCANCODE_EQUALS: return "gaiji_eu01";
    case SDL_SCANCODE_LEFTBRACKET: return "gaiji_symbol04";
    case SDL_SCANCODE_RIGHTBRACKET: return "gaiji_symbol07";
    case SDL_SCANCODE_BACKSLASH: return "gaiji_symbol11";
    case SDL_SCANCODE_SEMICOLON: return "gaiji_symbol05";
    case SDL_SCANCODE_APOSTROPHE: return "gaiji_eu02";
    case SDL_SCANCODE_GRAVE: return "gaiji_eu09";
    case SDL_SCANCODE_COMMA: return "gaiji_symbol08";
    case SDL_SCANCODE_PERIOD: return "gaiji_symbol09";
    case SDL_SCANCODE_SLASH: return "gaiji_symbol10";
    case SDL_SCANCODE_KP_DIVIDE: return "gaiji_num_symbol10";
    case SDL_SCANCODE_KP_MULTIPLY: return "gaiji_num_eu06";
    case SDL_SCANCODE_KP_MINUS: return "gaiji_num_symbol00";
    case SDL_SCANCODE_KP_PLUS: return "gaiji_num_eu10";
    case SDL_SCANCODE_KP_PERIOD: return "gaiji_num_symbol09";
    default: return NULL;
    }
}

/* The button icons. nx: the Switch name before its style suffix (_s000, _l001, ...); suffix: for the D-pad and
 * stick direction variants, their one style; pad: the PlayStation and Xbox name; row: the Switch input
 * (inputs[]) it stands for, -1 for a group (the whole D-pad, a stick). */
enum {
    G_SELECT, G_L3, G_R3, G_START, G_UP, G_RIGHT, G_DOWN, G_LEFT, G_L2, G_R2, G_L1, G_R1,
    G_NORTH, G_EAST, G_SOUTH, G_WEST, G_DPAD_LR, G_DPAD_UPDOWN, G_DPAD, G_LSTICK, G_RSTICK,
    G_LSTICK_LR, G_LSTICK_UPDOWN, G_RSTICK_LR, G_RSTICK_UPDOWN, NGLYPHS
};
static const struct {
    const char *nx, *suffix, *pad;
    int row;
} glyphs[NGLYPHS] = {
    [G_SELECT] = {"gaiji_bt09", NULL, "gaiji_bt09", 9},
    [G_L3] = {"gaiji_bt11", NULL, "gaiji_bt11", 10},
    [G_R3] = {"gaiji_bt12", NULL, "gaiji_bt12", 11},
    [G_START] = {"gaiji_bt10", NULL, "gaiji_bt10", 8},
    [G_UP] = {"gaiji_key_up", NULL, "gaiji_key_up", 12},
    [G_RIGHT] = {"gaiji_key_rt", NULL, "gaiji_key_rt", 15},
    [G_DOWN] = {"gaiji_key_dwn", NULL, "gaiji_key_dwn", 13},
    [G_LEFT] = {"gaiji_key_lt", NULL, "gaiji_key_lt", 14},
    [G_L2] = {"gaiji_bt06", NULL, "gaiji_bt06", 6},
    [G_R2] = {"gaiji_bt08", NULL, "gaiji_bt08", 7},
    [G_L1] = {"gaiji_bt05", NULL, "gaiji_bt05", 4},
    [G_R1] = {"gaiji_bt07", NULL, "gaiji_bt07", 5},
    [G_NORTH] = {"gaiji_bt01", NULL, "gaiji_bt01", 2},
    [G_EAST] = {"gaiji_bt02", NULL, "gaiji_bt02", 0},
    [G_SOUTH] = {"gaiji_bt03", NULL, "gaiji_bt03", 1},
    [G_WEST] = {"gaiji_bt04", NULL, "gaiji_bt04", 3},
    [G_DPAD_LR] = {"gaiji_key_all", "s100", "gaiji_key_lr", -1},
    [G_DPAD_UPDOWN] = {"gaiji_key_all", "s200", "gaiji_key_updwn", -1},
    [G_DPAD] = {"gaiji_key_all", NULL, "gaiji_key_all", -1},
    [G_LSTICK] = {"gaiji_lst", NULL, "gaiji_l", -1},
    [G_RSTICK] = {"gaiji_rst", NULL, "gaiji_r", -1},
    [G_LSTICK_LR] = {"gaiji_lst", "s100", "gaiji_l_lr", -1},
    [G_LSTICK_UPDOWN] = {"gaiji_lst", "s200", "gaiji_l_updwn", -1},
    [G_RSTICK_LR] = {"gaiji_rst", "s100", "gaiji_r_lr", -1},
    [G_RSTICK_UPDOWN] = {"gaiji_rst", "s200", "gaiji_r_updwn", -1},
};

/* The Switch sheet's style suffixes; not every icon has every style (nx_has). */
static const char *const nx_styles[] = {"s000", "s001", "s002", "s003", "s010", "s020", "l000", "l001", "m000"};
enum { NSTYLES = sizeof nx_styles / sizeof *nx_styles };

/* The icon for a controller button or stick, by its position (south: the bottom face button), or -1. */
static int glyph_for_pad(int pad) {
    switch (pad) {
    case SDL_GAMEPAD_BUTTON_NORTH: return G_NORTH;
    case SDL_GAMEPAD_BUTTON_EAST: return G_EAST;
    case SDL_GAMEPAD_BUTTON_SOUTH: return G_SOUTH;
    case SDL_GAMEPAD_BUTTON_WEST: return G_WEST;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return G_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return G_R1;
    case AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1): return G_L2;
    case AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1): return G_R2;
    case SDL_GAMEPAD_BUTTON_BACK: return G_SELECT;
    case SDL_GAMEPAD_BUTTON_START: return G_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return G_L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return G_R3;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return G_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return G_RIGHT;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return G_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return G_LEFT;
    case AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0): case AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1): return G_LSTICK_LR;
    case AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0): case AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1): return G_LSTICK_UPDOWN;
    case AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0): case AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1): return G_RSTICK_LR;
    case AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0): case AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1): return G_RSTICK_UPDOWN;
    default: return -1;
    }
}

static void nx_name(int g, int style, char *out, size_t size) {
    snprintf(out, size, "%s_%s", glyphs[g].nx, glyphs[g].suffix ? glyphs[g].suffix : nx_styles[style]);
}

/* Every Switch icon name the game may ask for: its CRC, which icon, which style. */
static struct {
    uint32_t crc;
    uint8_t glyph, style;
} nx_names[NGLYPHS * NSTYLES];
static int n_nx_names;
static uint32_t pad_crc[NGLYPHS];

static void gaiji_init(void) {
    if (n_nx_names) return;
    for (int g = 0; g < NGLYPHS; g++) {
        pad_crc[g] = gaiji_crc(glyphs[g].pad);
        for (int s = 0; s < (glyphs[g].suffix ? 1 : NSTYLES); s++) {
            char name[48];
            nx_name(g, s, name, sizeof name);
            nx_names[n_nx_names].crc = gaiji_crc(name);
            nx_names[n_nx_names].glyph = (uint8_t)g;
            nx_names[n_nx_names++].style = (uint8_t)s;
        }
    }
}

/* The Switch sheet has this icon in this style (gaiji_nx.g4tx's names). */
static int nx_has(const char *name) {
    static const char *const sheet[] = {
        "gaiji_bt01_l000", "gaiji_bt01_l001", "gaiji_bt01_s000", "gaiji_bt01_s001", "gaiji_bt01_s002",
        "gaiji_bt01_s010", "gaiji_bt02_l000", "gaiji_bt02_l001", "gaiji_bt02_s000", "gaiji_bt02_s001",
        "gaiji_bt02_s002", "gaiji_bt02_s003", "gaiji_bt02_s010", "gaiji_bt03_l000", "gaiji_bt03_l001",
        "gaiji_bt03_s000", "gaiji_bt03_s001", "gaiji_bt03_s002", "gaiji_bt03_s010", "gaiji_bt03_s020",
        "gaiji_bt04_l000", "gaiji_bt04_l001", "gaiji_bt04_s000", "gaiji_bt04_s001", "gaiji_bt04_s002",
        "gaiji_bt04_s003", "gaiji_bt04_s010", "gaiji_bt05_s000", "gaiji_bt05_s001", "gaiji_bt05_s002",
        "gaiji_bt05_s010", "gaiji_bt06_s000", "gaiji_bt06_s001", "gaiji_bt06_s002", "gaiji_bt06_s010",
        "gaiji_bt06_s020", "gaiji_bt07_s000", "gaiji_bt07_s001", "gaiji_bt07_s002", "gaiji_bt07_s010",
        "gaiji_bt08_s000", "gaiji_bt08_s001", "gaiji_bt08_s002", "gaiji_bt08_s010", "gaiji_bt08_s020",
        "gaiji_bt09_s000", "gaiji_bt09_s001", "gaiji_bt09_s002", "gaiji_bt09_s010", "gaiji_bt10_s000",
        "gaiji_bt10_s001", "gaiji_bt10_s002", "gaiji_bt10_s010", "gaiji_bt10_s020", "gaiji_bt11_s000",
        "gaiji_bt11_s001", "gaiji_bt11_s002", "gaiji_bt11_s010", "gaiji_bt12_s000", "gaiji_bt12_s001",
        "gaiji_bt12_s002", "gaiji_bt12_s010", "gaiji_key_all_m000", "gaiji_key_all_s000", "gaiji_key_all_s002",
        "gaiji_key_all_s010", "gaiji_key_all_s100", "gaiji_key_all_s200", "gaiji_key_dwn_s000",
        "gaiji_key_dwn_s001", "gaiji_key_dwn_s002", "gaiji_key_lt_s000", "gaiji_key_lt_s001", "gaiji_key_lt_s002",
        "gaiji_key_rt_s000", "gaiji_key_rt_s001", "gaiji_key_rt_s002", "gaiji_key_up_s000", "gaiji_key_up_s001",
        "gaiji_key_up_s002", "gaiji_lst_s000", "gaiji_lst_s001", "gaiji_lst_s002", "gaiji_lst_s010",
        "gaiji_lst_s100", "gaiji_lst_s200", "gaiji_rst_s000", "gaiji_rst_s001", "gaiji_rst_s002",
        "gaiji_rst_s010", "gaiji_rst_s100", "gaiji_rst_s200",
    };
    for (size_t i = 0; i < sizeof sheet / sizeof *sheet; i++)
        if (!strcmp(sheet[i], name)) return 1;
    return 0;
}

enum { SHEET_NX, SHEET_KEY, SHEET_PAD };

/* The icon to show for a Switch icon name the game asked for, or 0 to leave it; *sheet: which sheet it is in. */
static uint32_t gaiji_swap(uint32_t crc, int *sheet) {
    int i = 0;
    while (i < n_nx_names && nx_names[i].crc != crc) i++;
    if (i == n_nx_names) return 0;  /* not a button: one of the game's own icons */
    int g = nx_names[i].glyph, style = nx_names[i].style, row = glyphs[g].row;
    sets_ready();
    const Bound *use = sets[battle_active() ? SET_BATTLE : SET_FIELD];  /* the keys in use now */
    if (!using_pad) {
        /* the row's first key that has an icon, else its mouse part (the keyboard first: owner, 2026-11-13); in
         * battles the mouse first (owner, 2026-11-13: you attack and aim with the mouse there, so the hint shows
         * the mouse button, not U or O). Groups (a stick, the whole D-pad) keep the Switch icon: the keyboard sheet
         * has none for a set of keys */
        const int battle = battle_active() != 0;
        const int *mouse = mouse_map[battle ? SET_BATTLE : SET_FIELD];
        for (int pass = 0; row >= 0 && pass < 2; pass++) {
            if (pass == !battle) {  /* the mouse: pass 0 in battles, pass 1 when exploring */
                for (int p = 0; p < BMOUSE_PARTS; p++)
                    if (mouse[p] == row) return *sheet = SHEET_KEY, gaiji_crc(key_gaiji(mouse_code[p]));
            } else {
                for (int k = 0; k < BIND_KEYS; k++) {
                    const char *name = use[row].keys[k] ? key_gaiji(use[row].keys[k]) : NULL;
                    if (name) return *sheet = SHEET_KEY, gaiji_crc(name);
                }
            }
        }
        return 0;
    }
    int shown = g;
    if (row >= 0) {
        int p = glyph_for_pad(resolve(use[row].pad));
        if (p >= 0) shown = p;
    }
    if (hint_style() != STYLE_NINTENDO) return *sheet = SHEET_PAD, pad_crc[shown];
    if (shown == g) return 0;
    char name[48];
    nx_name(shown, style, name, sizeof name);
    if (!nx_has(name)) nx_name(shown, 0, name, sizeof name);  /* that icon lacks the style: the plain one */
    return gaiji_crc(name);
}

/* The keyboard, PlayStation and Xbox sheets' icons fill less of their cell than the Switch ones (more empty
 * border), so they are drawn this much larger, around the same centre. */
static float icon_scale(int sheet) {
    static float scale[3];
    if (!scale[SHEET_KEY]) {
        const char *k = getenv("YW4_KEY_ICON_SCALE"), *p = getenv("YW4_PAD_ICON_SCALE");
        scale[SHEET_NX] = 1.0f;
        scale[SHEET_KEY] = k && atof(k) > 0 ? (float)atof(k) : 1.5f;
        scale[SHEET_PAD] = p && atof(p) > 0 ? (float)atof(p) : 1.25f;
    }
    return scale[sheet];
}

/* The keyboard sheet's mouse icons fill more of their cell than its keys: at the keys' scale they looked too large
 * (owner, 2026-11-04: 1.3% smaller, 0.987; 2026-11-11: "still too large", 30% smaller again, 0.987 x 0.7). They get
 * this much of the keys' scale (YW4_MOUSE_ICON_SCALE); centred in the same place. */
static float mouse_icon_scale(uint32_t crc) {
    static uint32_t crcs[7];
    static float k;
    if (!k) {
        static const int ids[7] = {MOUSE_BIND_LEFT, MOUSE_BIND_RIGHT, MOUSE_BIND_MIDDLE, MOUSE_BIND_X1, MOUSE_BIND_X2,
                                   MOUSE_BIND_WHEEL_UP, MOUSE_BIND_WHEEL_DOWN};
        for (int i = 0; i < 7; i++) crcs[i] = gaiji_crc(key_gaiji(ids[i]));
        const char *e = getenv("YW4_MOUSE_ICON_SCALE");
        k = e && atof(e) > 0 ? (float)atof(e) : 0.69f;
    }
    for (int i = 0; i < 7; i++)
        if (crcs[i] == crc) return k;
    return 1.0f;
}

/* A request's box: +4 x (left), +8 y (middle), +0xc and +0x10 its size, in pixels. */
typedef struct {
    int32_t x, y, h, w;
} Box;

static Box box_get(uint64_t req) {
    return (Box){(int32_t)ld32(req + 4), (int32_t)ld32(req + 8), (int32_t)ld32(req + 0xc), (int32_t)ld32(req + 0x10)};
}

/* What each request had from the game, by request address: a request changed once still holds our name and
 * box next frame if the game didn't write it again, and must be changed again from the game's. */
enum { REMEMBERED = 4096 };
static struct {
    uint64_t req;
    uint32_t game, shown;
    Box game_box, shown_box;
} remembered[REMEMBERED];

static void gc_follow(void);  /* the PlayStation sheet's middle buttons, below */

int yw4_hook_gaiji_render(CPUState *ctx) {
    uint64_t main_base = *yw4_modules[YW4_MAIN].base;
    uint64_t sheets = ld64(main_base + 0x58654f8);
    uint32_t nsheets = ld32(main_base + 0x5865500);
    if (!sheets || nsheets < 5 || ctx->x[0] != ld64(sheets)) return 0;  /* only before sheet 0 */
    ready();
    gaiji_init();
    gc_follow();
    uint64_t ps = ld64(sheets), xbox = ld64(sheets + 8), key = ld64(sheets + 16);
    if (ps) st8(ps + 0x1c, (uint8_t)(using_pad && hint_style() == STYLE_PLAYSTATION));
    if (xbox) st8(xbox + 0x1c, (uint8_t)(using_pad && hint_style() == STYLE_XBOX));
    if (key) st8(key + 0x1c, 1);
    uint64_t reqs = ctx->x[2];
    uint32_t n = (uint32_t)ctx->x[3];
    for (uint32_t i = 0; i < n; i++) {
        uint64_t req = reqs + (uint64_t)i * 0x38;
        uint32_t now = ld32(req);
        if (!now) continue;
        unsigned slot = (unsigned)(((req >> 3) * 2654435761u) % REMEMBERED);
        Box box = box_get(req);
        int ours = remembered[slot].req == req && remembered[slot].shown == now &&
                   !memcmp(&remembered[slot].shown_box, &box, sizeof box);
        uint32_t game = ours ? remembered[slot].game : now;
        Box game_box = ours ? remembered[slot].game_box : box;
        int sheet = SHEET_NX;
        uint32_t shown = gaiji_swap(game, &sheet);
        if (!shown) shown = game;
        Box shown_box = game_box;
        if (sheet != SHEET_NX) {
            float mk = sheet == SHEET_KEY ? mouse_icon_scale(shown) : 1.0f;
            float k = icon_scale(sheet) * mk;
            /* the box is whole pixels of the HUD's 1080-line space: rounded to the nearest (the mouse icons were
             * rounded down while they were only 1.3% smaller, under half a pixel) */
            float round_add = 0.5f;
            (void)mk;
            shown_box.h = (int32_t)((float)game_box.h * k + round_add);
            shown_box.w = (int32_t)((float)game_box.w * k + round_add);
            shown_box.x = game_box.x - (shown_box.w - game_box.w) / 2;
        }
        if (shown != now) st32(req, shown);
        if (memcmp(&shown_box, &box, sizeof box)) {
            st32(req + 4, (uint32_t)shown_box.x), st32(req + 8, (uint32_t)shown_box.y);
            st32(req + 0xc, (uint32_t)shown_box.h), st32(req + 0x10, (uint32_t)shown_box.w);
        }
        remembered[slot].req = req, remembered[slot].game = game, remembered[slot].shown = shown;
        remembered[slot].game_box = game_box, remembered[slot].shown_box = shown_box;
    }
    return 0;
}

/* ---- the PlayStation sheet's middle buttons (owner, 2026-11-13). The game ships only a PS4 sheet, so its Minus
 * icon (gaiji_bt09, at 104,104 in the 256 x 256 BC7 sheet) is the PS4 touchpad: the wrong button on every
 * PlayStation pad (auto battle is on Create on a DualSense, Share on a DualShock 4, Select on a PS3 pad), and its
 * Plus icon (gaiji_bt10, at 156,0) says OPTIONS, which a PS3 pad calls Start. Those cells are rewritten in the
 * game's memory for the pad in use, in the Options icon's style: the word above a small dark pill. Rows 0-23 are
 * our drawing (gaiji_create.h, from tools/yw4/gaiji_create.py: the word and the pill's top, which Options hides
 * under its word); rows 24-47 are the Options icon's own, from the same texture, so no game art is in the repo.
 *
 * The first time, before the texture's first upload (gpu.c), the sheet is known by the touchpad icon's blocks (a
 * fingerprint); its Options top half is kept here then, to put back. After that the hint hook (game thread) writes
 * the cells again when the PlayStation model changes, and tells the GPU layer (gpu_note_written), which uploads the
 * texture again. Before each such write the cells must still be what we wrote (else the memory is something else
 * now and is left alone). */
#include "gaiji_create.h"
#include "gpu.h"

enum { GC_CELL = 48, GC_TOP = 24, GC_ROW = GC_CELL / 4 * 16, GC_HALF = GC_TOP / 4 * GC_ROW };
static uint8_t *gc_texels;  /* the sheet's storage once found (set once, by the GPU thread) */
static TexLayout gc_layout;
static uint8_t gc_options_top[GC_HALF], gc_body[GC_HALF];  /* bt10's own rows 0-23 and 24-47 */
static int gc_model;                                       /* the model the cells are written for (3, 4, 5) */
static uint64_t gc_print9, gc_print10;                     /* what the cells hold now */

/* The Xbox sheet: an Xbox 360 pad's Back and Start (a left and a right triangle) in place of View and Menu, in the
 * middle 6 x 4 blocks of the two circle icons (x 12-35, y 16-31 of the cell; the circle's edge stays the game's).
 * Found the same way (the View icon's fingerprint); the game's own middles are kept, to put back for other pads. */
enum { XB_X = 12, XB_Y = 16, XB_W = 24, XB_H = 16, XB_SIZE = XB_W / 4 * XB_H / 4 * 16 };
static uint8_t *gc_xb_texels;
static TexLayout gc_xb_layout;
static uint8_t gc_xb_view[XB_SIZE], gc_xb_menu[XB_SIZE];
static int gc_xb_mode = -1;  /* written for: 1 an Xbox 360 pad, 0 others (the game's View and Menu) */
static uint64_t gc_xb_print9, gc_xb_print10;

int bindings_ps_model(void) { return ps_model; }
int bindings_xbox360(void) { return xbox360; }

static uint64_t gc_fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* both cells for a model, into the sheet; their fingerprints kept */
static void gc_write(uint8_t *texels, int model) {
    uint8_t cell[2 * GC_HALF];
    const TexLayout *L = &gc_layout;
    int word9 = model == 3 ? GAIJI_WORD_SELECT : model == 4 ? GAIJI_WORD_SHARE : GAIJI_WORD_CREATE;
    memcpy(cell, gaiji_word_top[word9], GC_HALF);
    memcpy(cell + GC_HALF, gc_body, GC_HALF);
    tegra_write_region(L, texels, 0, 0, 104, 104, 0, GC_CELL, GC_CELL, 1, cell);
    gc_print9 = gc_fnv(cell, sizeof cell);
    memcpy(cell, model == 3 ? gaiji_word_top[GAIJI_WORD_START] : gc_options_top, GC_HALF);
    tegra_write_region(L, texels, 0, 0, 156, 0, 0, GC_CELL, GC_CELL, 1, cell);
    gc_print10 = gc_fnv(cell, sizeof cell);
    gc_model = model;
    fprintf(stderr, "yw4: hints: PlayStation sheet: %s and %s (PS%d)\n",
            model == 3 ? "SELECT" : model == 4 ? "SHARE" : "CREATE", model == 3 ? "START" : "OPTIONS", model);
}

static void xb_found(const TexLayout *L, uint8_t *texels);

void bindings_patch_sheet(const TexLayout *L, uint32_t nvn_format, uint8_t *texels) {
    if (!texels || (nvn_format != 0x4D && nvn_format != 0x4E) || L->width != 256 || L->height != 256 ||
        L->depth != 1 || L->layers != 1)
        return;
    uint8_t cell[2 * GC_HALF];
    tegra_read_region(L, texels, 0, 0, 104, 104, 0, GC_CELL, GC_CELL, 1, cell);
    uint64_t h = gc_fnv(cell, sizeof cell);
    if (h == GAIJI_XBOX_VIEW_PRINT && !__atomic_load_n(&gc_xb_texels, __ATOMIC_ACQUIRE)) xb_found(L, texels);
    if (h != GAIJI_TOUCHPAD_PRINT || __atomic_load_n(&gc_texels, __ATOMIC_ACQUIRE)) return;
    gc_layout = *L;
    tegra_read_region(L, texels, 0, 0, 156, 0, 0, GC_CELL, GC_CELL, 1, cell);
    memcpy(gc_options_top, cell, GC_HALF);
    memcpy(gc_body, cell + GC_HALF, GC_HALF);
    gc_write(texels, ps_model);  /* before the upload this is called from: nothing more to tell the GPU layer */
    __atomic_store_n(&gc_texels, texels, __ATOMIC_RELEASE);  /* only now may the hint hook write too */
}

static void xb_write(uint8_t *texels, int mode) {
    const TexLayout *L = &gc_xb_layout;
    const uint8_t *a = mode ? gaiji_xbox_back : gc_xb_view, *b = mode ? gaiji_xbox_start : gc_xb_menu;
    tegra_write_region(L, texels, 0, 0, 104 + XB_X, 104 + XB_Y, 0, XB_W, XB_H, 1, a);
    tegra_write_region(L, texels, 0, 0, 156 + XB_X, 0 + XB_Y, 0, XB_W, XB_H, 1, b);
    gc_xb_print9 = gc_fnv(a, XB_SIZE), gc_xb_print10 = gc_fnv(b, XB_SIZE);
    gc_xb_mode = mode;
    fprintf(stderr, "yw4: hints: Xbox sheet: %s\n", mode ? "Back and Start (Xbox 360)" : "View and Menu");
}

static void xb_found(const TexLayout *L, uint8_t *texels) {
    gc_xb_layout = *L;
    tegra_read_region(L, texels, 0, 0, 104 + XB_X, 104 + XB_Y, 0, XB_W, XB_H, 1, gc_xb_view);
    tegra_read_region(L, texels, 0, 0, 156 + XB_X, 0 + XB_Y, 0, XB_W, XB_H, 1, gc_xb_menu);
    gc_xb_print9 = gc_fnv(gc_xb_view, XB_SIZE), gc_xb_print10 = gc_fnv(gc_xb_menu, XB_SIZE);
    gc_xb_mode = 0;
    if (xbox360) xb_write(texels, 1);  /* before the upload this is called from */
    __atomic_store_n(&gc_xb_texels, texels, __ATOMIC_RELEASE);
}

/* the Xbox sheet again when the pad changed between an Xbox 360 pad and others */
static void xb_follow(void) {
    uint8_t *texels = __atomic_load_n(&gc_xb_texels, __ATOMIC_ACQUIRE);
    if (!texels || gc_xb_mode == xbox360) return;
    uint8_t a[XB_SIZE], b[XB_SIZE];
    tegra_read_region(&gc_xb_layout, texels, 0, 0, 104 + XB_X, 104 + XB_Y, 0, XB_W, XB_H, 1, a);
    tegra_read_region(&gc_xb_layout, texels, 0, 0, 156 + XB_X, 0 + XB_Y, 0, XB_W, XB_H, 1, b);
    if (gc_fnv(a, XB_SIZE) != gc_xb_print9 || gc_fnv(b, XB_SIZE) != gc_xb_print10) {
        fprintf(stderr, "yw4: hints: Xbox sheet changed under us; its icons are left as they are\n");
        __atomic_store_n(&gc_xb_texels, NULL, __ATOMIC_RELEASE);
        return;
    }
    xb_write(texels, xbox360);
    gpu_note_written((uint64_t)(uintptr_t)texels, tegra_total_size(&gc_xb_layout));
}

/* the hint hook, each frame: the cells again when the PlayStation model (or Xbox 360 or not) changed */
static void gc_follow(void) {
    xb_follow();
    uint8_t *texels = __atomic_load_n(&gc_texels, __ATOMIC_ACQUIRE);
    if (!texels || gc_model == ps_model) return;
    uint8_t cell[2 * GC_HALF];
    tegra_read_region(&gc_layout, texels, 0, 0, 104, 104, 0, GC_CELL, GC_CELL, 1, cell);
    uint64_t p9 = gc_fnv(cell, sizeof cell);
    tegra_read_region(&gc_layout, texels, 0, 0, 156, 0, 0, GC_CELL, GC_CELL, 1, cell);
    if (p9 != gc_print9 || gc_fnv(cell, sizeof cell) != gc_print10) {  /* not ours any more: leave it */
        fprintf(stderr, "yw4: hints: PlayStation sheet changed under us; its icons are left as they are\n");
        __atomic_store_n(&gc_texels, NULL, __ATOMIC_RELEASE);
        return;
    }
    gc_write(texels, ps_model);
    gpu_note_written((uint64_t)(uintptr_t)texels, tegra_total_size(&gc_layout));
}

/* ---- the panel page */

static int row, col, capturing;
static uint64_t capture_since;
/* The controller was used last (window.c used_pad): the binding rows keep to the controller column, so a pad can't
 * change or clear a keyboard key (owner, 2026-11-13). The keyboard and mouse still reach every column. */
static int pad_only;
void bindings_pad_only(int on) {
    pad_only = on;
    if (on && !capturing) col = BIND_KEYS;
}

int bindings_capturing(void) {
    if (capturing && SDL_GetTicks() - capture_since > 5000) capturing = 0;
    return capturing;
}
void bindings_cancel(void) { capturing = 0; }

void bindings_select(int r, int c) {
    if (capturing || r < 0 || r >= NROWS) return;
    row = r;
    if (c >= 0 && c < BIND_COLS) col = c;
    if (pad_only) col = BIND_KEYS;
}

void bindings_action(int action) {
    sets_ready();
    if (pad_only && !capturing) col = BIND_KEYS;
    int sens = row - SENS_ROW;  /* the sensitivity rows' axis */
    switch (action) {
    /* the ends stop the choice (no wrapping round: holding a direction stops at the last row) */
    case SETTINGS_UP: row = row > 0 ? row - 1 : 0; break;
    case SETTINGS_DOWN: row = row < NROWS - 1 ? row + 1 : NROWS - 1; break;
    case SETTINGS_LEFT:
    case SETTINGS_RIGHT:
        if (row == SET_ROW) {
            edit_set = !edit_set;
        } else if (row >= SENS_ROW && row < SENS_ROW + NMOUSE) {
            if (action == SETTINGS_LEFT && sens_index(sens) > 0) sens_choice[sens]--, settings_save();
            if (action == SETTINGS_RIGHT && sens_index(sens) < NSENS - 1) sens_choice[sens]++, settings_save();
        } else if (row == RUMBLE_ROW) {
            int next = !window_rumble_enabled();
            window_rumble_set_enabled(next);
            if (next) window_rumble_test();
            settings_save();
        } else if (row == LAYOUT_ROW) {
            nintendo_layout = !nintendo_layout, settings_save();
        } else if (row == MOUSE_ROW) {  /* the Mouse buttons row opens its page (gamemenu.c): nothing here */
        } else if (!pad_only) {  /* a pad stays on its own column */
            col = action == SETTINGS_LEFT ? (col > 0 ? col - 1 : 0) : (col < BIND_COLS - 1 ? col + 1 : BIND_COLS - 1);
        }
        break;
    case SETTINGS_ACCEPT:
        if (row == SET_ROW) {
            edit_set = !edit_set;
        } else if (row >= SENS_ROW && row < SENS_ROW + NMOUSE) {
            sens_choice[sens] = (sens_index(sens) + 1) % NSENS, settings_save();
        } else if (row == RUMBLE_ROW) {
            int next = !window_rumble_enabled();
            window_rumble_set_enabled(next);
            if (next) window_rumble_test();
            settings_save();
        } else if (row == LAYOUT_ROW) {
            nintendo_layout = !nintendo_layout, settings_save();
        } else if (row == RESET_ROW) {
            /* the exploring set back to the starting keys; the battle set to a copy of the exploring set */
            if (edit_set == SET_BATTLE) copy_exploring_to_battle();
            else reset_set(SET_FIELD);
            settings_save();
        } else if (row == MOUSE_ROW) {  /* opens its page (gamemenu.c) */
        } else {
            capturing = 1, capture_since = SDL_GetTicks();
        }
        break;
    case SETTINGS_CLEAR:
        if (row >= SENS_ROW && row < SENS_ROW + NMOUSE) sens_choice[sens] = SENS_DEFAULT, settings_save();
        if (row == RUMBLE_ROW) {
            window_rumble_set_enabled(1);
            window_rumble_test();
            settings_save();
        }
        if (row == LAYOUT_ROW) nintendo_layout = 0, settings_save();
        if (row < FIRST_INPUT || row == RESET_ROW) break;
        if (col < BIND_KEYS) bound[row - FIRST_INPUT].keys[col] = 0;
        else bound[row - FIRST_INPUT].pad = PAD_NONE;
        settings_save();
        break;
    }
}

int bindings_capture_key(int scancode) {
    if (!bindings_capturing()) return 0;
    capturing = 0;
    if (col < BIND_KEYS) {  /* a key for a key column; the other key of the row cleared if it is the same */
        for (int k = 0; k < BIND_KEYS; k++)
            if (bound[row - FIRST_INPUT].keys[k] == scancode) bound[row - FIRST_INPUT].keys[k] = 0;
        bound[row - FIRST_INPUT].keys[col] = scancode;
        using_pad = 0;
        settings_save();
    }  /* a key while waiting for a gamepad input cancels */
    return 1;
}

/* A mouse part pressed while a key column waits: that part now presses this row's Switch input, in the set shown
 * (the mouse's own binding, mouse_map; the row's keys stay). The side buttons aren't bound (owner, 2026-11-13). */
static int capture_mouse_part(int part) {
    if (!bindings_capturing() || col >= BIND_KEYS || part < 0) return 0;
    capturing = 0;
    mouse_map[edit_set][part] = row - FIRST_INPUT;
    using_pad = 0;
    settings_save();
    return 1;
}
int bindings_capture_mouse(int button) {
    return capture_mouse_part(button == SDL_BUTTON_LEFT ? BMOUSE_LEFT : button == SDL_BUTTON_RIGHT ? BMOUSE_RIGHT
                              : button == SDL_BUTTON_MIDDLE ? BMOUSE_MID : -1);
}
int bindings_capture_wheel(int y) { return capture_mouse_part(y > 0 ? BMOUSE_WHEEL_UP : BMOUSE_WHEEL_DOWN); }

/* ---- the Mouse buttons page (gamemenu.c) */
int bindings_mouse_get(int set, int part) {
    sets_ready();
    return set >= 0 && set < NSETS && part >= 0 && part < BMOUSE_PARTS ? mouse_map[set][part] : -1;
}
void bindings_mouse_set(int set, int part, int input) {
    sets_ready();
    if (set < 0 || set >= NSETS || part < 0 || part >= BMOUSE_PARTS || input < -1 || input >= NINPUTS) return;
    mouse_map[set][part] = input;
    settings_save();
}
/* the inputs a mouse part can press: the buttons and the D-pad (not a stick's directions) */
int bindings_mouse_choices(int *out, int max) {
    int n = 0;
    for (int i = 0; i < NINPUTS && n < max; i++)
        if (inputs[i].bit >= 0) out[n++] = i;
    return n;
}
int bindings_on_mouse_row(void) { return row == MOUSE_ROW; }
int bindings_set_shown(void) { return edit_set; }

int bindings_capture_button(int button) {
    if (!bindings_capturing()) return 0;
    capturing = 0;
    if (col == BIND_KEYS) bound[row - FIRST_INPUT].pad = button, using_pad = 1, settings_save();
    return 1;
}

int bindings_capture_axis(int axis, int value) {
    if (!bindings_capturing() || col != BIND_KEYS || (value < 24000 && value > -24000)) return 0;
    capturing = 0;
    bound[row - FIRST_INPUT].pad = AXIS(axis, value > 0);
    using_pad = 1;
    settings_save();
    return 1;
}

/* The other Switch input that a key or gamepad input also presses, or -1. */
static int also_used(int self, int key, int pad) {
    for (int i = 0; i < NINPUTS; i++) {
        if (i == self) continue;
        for (int k = 0; k < BIND_KEYS; k++)
            if (key && bound[i].keys[k] == key) return i;
        if (pad != PAD_NONE && resolve(bound[i].pad) == resolve(pad)) return i;
    }
    return -1;
}

static void input_title(int i, char *s, size_t n) {
    if (inputs[i].group[0] == 'B') snprintf(s, n, "%s", inputs[i].name);
    else snprintf(s, n, "%s %s", inputs[i].group, inputs[i].name);
}
void bindings_input_title(int i, char *s, size_t n) {
    if (i >= 0 && i < NINPUTS) input_title(i, s, n);
    else if (n) *s = 0;
}

int bindings_rows(BindRow *rows, int max, int *sel_row, int *sel_col, char *help, int help_size) {
    sets_ready();
    int n = 0;
    static const char *const sens_names[NMOUSE] = {"HORIZONTAL", "VERTICAL"};
    for (int a = 0; a < NMOUSE && n < max; a++) {
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->group = "MOUSE", r->name = sens_names[a], r->value = 1;
        snprintf(r->cells[0], BIND_NAME, "%sX", sens_values[sens_index(a)]);
    }
    if (n < max) {  /* the Mouse buttons row: the overlay draws it as a way into its page */
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->group = "MOUSE", r->name = "BUTTONS", r->value = 1;
        snprintf(r->cells[0], BIND_NAME, "OPEN");
    }
    if (n < max) {
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->group = "CONTROLLER", r->name = "VIBRATION", r->value = 1;
        snprintf(r->cells[0], BIND_NAME, "%s", window_rumble_enabled() ? "ON" : "OFF");
    }
    if (n < max) {
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->group = "CONTROLLER", r->name = "NINTENDO ICONS", r->value = 1;
        snprintf(r->cells[0], BIND_NAME, "%s", nintendo_layout ? "ON" : "OFF");
    }
    if (n < max) {
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->group = "KEYS FOR", r->name = "SET", r->value = 1;
        snprintf(r->cells[0], BIND_NAME, "%s", edit_set ? "BATTLES" : "EXPLORING");
    }
    for (int i = 0; i < NINPUTS && n < max; i++, n++) {
        BindRow *r = &rows[n];
        r->group = inputs[i].group, r->name = inputs[i].name;
        for (int k = 0; k < BIND_KEYS; k++) {
            snprintf(r->cells[k], BIND_NAME, "%s", key_name(bound[i].keys[k]));
            r->conflict[k] = also_used(i, bound[i].keys[k], PAD_NONE) >= 0;
        }
        snprintf(r->cells[BIND_KEYS], BIND_NAME, "%s", pad_label(bound[i].pad));
        r->conflict[BIND_KEYS] = also_used(i, 0, bound[i].pad) >= 0;
    }
    if (n < max) {
        BindRow *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->name = edit_set ? "COPY THE EXPLORING KEYS" : "RESET ALL TO THE STARTING KEYS";
    }
    *sel_row = row, *sel_col = col;
    char title[48], other[48];
    if (row >= SENS_ROW && row < SENS_ROW + NMOUSE) {
        int a = row - SENS_ROW;
        snprintf(help, (size_t)help_size,
                 "HOW FAST THE CAMERA TURNS %s WITH THE MOUSE: %sX (1.0X TO START), IN BOTH SETS. LEFT/RIGHT: CHANGE. "
                 "DELETE OR X: BACK TO 1.0X. A CLICK IN THE WINDOW GIVES THE MOUSE TO THE GAME, ALT TAKES IT BACK.",
                 a ? "UP AND DOWN" : "LEFT AND RIGHT", sens_values[sens_index(a)]);
        return n;
    }
    if (row == MOUSE_ROW) {
        snprintf(help, (size_t)help_size,
                 "WHAT THE LEFT AND RIGHT BUTTONS, THE WHEEL PRESS AND TURNING THE WHEEL DO, FOR EXPLORING AND FOR "
                 "BATTLES. ENTER OR CLICK: OPEN.");
        return n;
    }
    if (row == RUMBLE_ROW) {
        snprintf(help, (size_t)help_size,
                 "WHETHER CONTROLLER VIBRATION AND HAPTICS ARE ENABLED: %s. LEFT/RIGHT OR ENTER/A: TOGGLE. "
                 "WHEN TURNED ON, THE CONTROLLER RUMBLES FOR A SECOND AS A TEST.",
                 window_rumble_enabled() ? "ON" : "OFF");
        return n;
    }
    if (row == LAYOUT_ROW) {
        snprintf(help, (size_t)help_size,
                 "FOR NINTENDO-STYLE CONTROLLERS THAT THE GAME SEES AS AN XBOX CONTROLLER: %s. WHEN ON, THE BUTTON "
                 "ICONS SHOW THE LETTERS PRINTED ON YOUR CONTROLLER. THE BUTTONS KEEP THEIR PLACES. "
                 "LEFT/RIGHT OR ENTER/A: TOGGLE.",
                 nintendo_layout ? "ON" : "OFF");
        return n;
    }
    if (row == SET_ROW) {
        snprintf(help, (size_t)help_size,
                 "WHICH KEYS THIS PAGE SHOWS AND CHANGES: THE ONES FOR EXPLORING OR THE ONES FOR BATTLES. THE GAME "
                 "USES EACH SET BY ITSELF, SO A KEY CAN DO ONE THING ON THE FIELD AND ANOTHER IN A BATTLE. "
                 "LEFT/RIGHT: SWITCH.%s",
                 battle_active() ? " A BATTLE IS ON NOW." : "");
        return n;
    }
    if (row == RESET_ROW) {
        snprintf(help, (size_t)help_size, edit_set ? "MAKES THE BATTLE KEYS THE SAME AS THE EXPLORING KEYS. ENTER OR A: COPY."
                                                   : "PUTS EVERY KEY AND GAMEPAD BUTTON BACK TO HOW IT STARTED. ENTER OR A: RESET.");
        return n;
    }
    int in = row - FIRST_INPUT;
    input_title(in, title, sizeof title);
    int key = col < BIND_KEYS ? bound[in].keys[col] : 0, pad = col == BIND_KEYS ? bound[in].pad : PAD_NONE;
    int o = also_used(in, key, pad);
    if (o >= 0) input_title(o, other, sizeof other);
    if (bindings_capturing())
        snprintf(help, (size_t)help_size,
                 col < BIND_KEYS ? "PRESS THE KEY OR CLICK MOUSE FOR %s NOW. A GAMEPAD BUTTON CANCELS."
                                 : "PRESS THE GAMEPAD BUTTON, OR PUSH THE STICK OR TRIGGER, FOR %s NOW. A KEY CANCELS.",
                 title);
    else if (o >= 0)
        snprintf(help, (size_t)help_size, "%s ALSO PRESSES %s. BOTH WORK, BUT THE GAME SEES THEM TOGETHER.",
                 col < BIND_KEYS ? key_name(key) : pad_label(pad), other);
    else
        snprintf(help, (size_t)help_size,
                 "%s: TWO KEYS AND ONE GAMEPAD INPUT. ENTER OR A: CHANGE. DELETE OR X: CLEAR. GAMEPAD BUTTONS ARE "
                 "NAMED AS ON YOUR CONTROLLER.",
                 title);
    return n;
}

/* ---- the settings file */

int bindings_load_line(const char *name, const char *value) {
    for (int a = 0; a < NMOUSE; a++)
        if (!strcmp(name, sens_env[a])) {
            if (!getenv(name)) sens_choice[a] = sens_from(value);
            return 1;
        }
    if (!strcmp(name, "YW4_MOUSE_SENS")) {  /* the older single setting: both, unless set on their own */
        if (!getenv(name) && !getenv(sens_env[0])) sens_choice[0] = sens_from(value);
        if (!getenv(name) && !getenv(sens_env[1])) sens_choice[1] = sens_from(value);
        return 1;
    }
    if (!strcmp(name, "YW4_RUMBLE")) {
        if (!getenv(name)) window_rumble_set_enabled(!strcmp(value, "1") || !strcmp(value, "ON") || !strcmp(value, "on"));
        return 1;
    }
    if (!strcmp(name, "YW4_NINTENDO_LAYOUT")) {
        const char *e = getenv(name);  /* the environment wins, as for every setting */
        if (e) value = e;
        nintendo_layout = !strcmp(value, "1") || !strcmp(value, "ON") || !strcmp(value, "on");
        return 1;
    }
    ready();
    int set = SET_FIELD;
    if (!strncmp(name, "BATTLE_", 7)) {
        if (!battle_set_known) {
            battle_starting_set();  /* each BATTLE_ line then changes its own key */
            battle_set_known = 1;
        }
        set = SET_BATTLE, name += 7;
    }
    Bound *b = sets[set];
    for (int p = 0; p < BMOUSE_PARTS; p++)  /* the mouse's own lines: MOUSE_LEFT=Y, - for none */
        if (!strcmp(name, mouse_save[p])) {
            mouse_map[set][p] = strcmp(value, "-") ? input_by_id(value) : -1;
            return 1;
        }
    for (int i = 0; i < NINPUTS; i++) {
        char want[32];
        for (int k = 0; k < BIND_KEYS; k++) {
            snprintf(want, sizeof want, "KEY%d_%s", k + 1, inputs[i].id);
            if (!strcmp(name, want)) {
                int code = key_from_name(value), part = mouse_part(code);
                /* a mouse button in a key slot (files before the Mouse buttons page, 2026-11-13): it becomes the
                 * mouse's own binding and the slot is freed; the side buttons are dropped */
                if (part >= 0) mouse_map[set][part] = i, code = 0;
                else if (code == MOUSE_BIND_X1 || code == MOUSE_BIND_X2) code = 0;
                b[i].keys[k] = code;
                return 1;
            }
        }
        snprintf(want, sizeof want, "PAD_%s", inputs[i].id);
        if (!strcmp(name, want)) {
            b[i].pad = pad_from_name(value);
            /* the first defaults went by the Switch's positions (A on the right button): those become the
             * controller's own A, B, X, Y */
            static const char *const old[4] = {"EAST", "SOUTH", "NORTH", "WEST"};
            if (i < 4 && !strcmp(value, old[i])) b[i].pad = PAD_FACE + i;
            return 1;
        }
    }
    return 0;
}

void bindings_save(FILE *f) {
    sets_ready();
    fprintf(f, "# Controls: two keys (SDL key names) and one gamepad input per Switch input; - for none.\n"
               "# MOUSE_LEFT/RIGHT/MID/WHEEL_UP/WHEEL_DOWN: the Switch input that part of the mouse presses.\n"
               "# BATTLE_...: the same for battles.\n");
    for (int s = 0; s < NSETS; s++) {
        const char *pre = s == SET_BATTLE ? "BATTLE_" : "";
        for (int i = 0; i < NINPUTS; i++) {
            for (int k = 0; k < BIND_KEYS; k++)
                fprintf(f, "%sKEY%d_%s=%s\n", pre, k + 1, inputs[i].id, key_save_name(sets[s][i].keys[k]));
            fprintf(f, "%sPAD_%s=%s\n", pre, inputs[i].id, pad_name(sets[s][i].pad));
        }
        for (int p = 0; p < BMOUSE_PARTS; p++)
            fprintf(f, "%s%s=%s\n", pre, mouse_save[p], mouse_map[s][p] >= 0 ? inputs[mouse_map[s][p]].id : "-");
    }
    for (int a = 0; a < NMOUSE; a++) fprintf(f, "%s=%s\n", sens_env[a], sens_values[sens_index(a)]);
    fprintf(f, "YW4_RUMBLE=%d\n", window_rumble_enabled());
    fprintf(f, "YW4_NINTENDO_LAYOUT=%d\n", nintendo_layout);
}
