/* padtest: what SDL3 (the game's own copy) sees of a controller, and what the game would do with each button.
 * Built and started by `python -m tools.dev.padtest`. Not part of the game.
 *
 * For each button press it prints the button's place on the pad (bottom, right, left, top), the letter SDL thinks is
 * printed on it, and the Switch button the game gives for it. The game picks Xbox, PlayStation or Nintendo names from
 * the type the pad reports (bindings.c, bindings_read): a pad that reports "Xbox 360" but has A on the right gets
 * the Xbox meaning, so its printed letters and the game's disagree. It also shows how the pad is connected (USB or
 * Bluetooth) and finger positions on a touchpad (PlayStation pads; over Bluetooth only once SDL has switched the pad
 * to its full reports). Quit: Back + Start together, or Ctrl+C. */
#include <stdio.h>

#include <SDL3/SDL.h>

static const char *place(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "BOTTOM face button";
    case SDL_GAMEPAD_BUTTON_EAST: return "RIGHT face button ";
    case SDL_GAMEPAD_BUTTON_WEST: return "LEFT face button  ";
    case SDL_GAMEPAD_BUTTON_NORTH: return "TOP face button   ";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back / View / Minus";
    case SDL_GAMEPAD_BUTTON_START: return "Start / Menu / Plus";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide / Home";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Left shoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Right shoulder";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick press";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick press";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad click";
    default: return "other button";
    }
}

static const char *label(SDL_GamepadButtonLabel l) {
    static const char *const n[] = {"?", "A", "B", "X", "Y", "Cross", "Circle", "Square", "Triangle"};
    return l >= 0 && l < (int)(sizeof n / sizeof *n) ? n[l] : "?";
}

/* bindings.c's pad_style rule */
static int style_of(SDL_GamepadType t) {
    if (t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5) return 1;
    if (t >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && t <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) return 2;
    return 0;
}

/* the Switch button the game gives for a face button (bindings.c's resolve, default bindings) */
static const char *switch_button(int style, int b) {
    if (style == 2) {
        switch (b) {
        case SDL_GAMEPAD_BUTTON_EAST: return "A";
        case SDL_GAMEPAD_BUTTON_SOUTH: return "B";
        case SDL_GAMEPAD_BUTTON_NORTH: return "X";
        case SDL_GAMEPAD_BUTTON_WEST: return "Y";
        }
    } else {
        switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
        case SDL_GAMEPAD_BUTTON_EAST: return "B";
        case SDL_GAMEPAD_BUTTON_WEST: return "X";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  /* events while the console has focus */
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "padtest: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    printf("padtest (SDL %d.%d.%d)\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    printf("Plug in the controller if it isn't, then press the 4 face buttons one by one.\n");
    printf("Quit: Back + Start together, or Ctrl+C.\n\n");
    fflush(stdout);
    static const char *const style_name[] = {"Xbox", "PlayStation", "Nintendo"};
    SDL_Gamepad *pad = NULL;
    int style = 0, running = 1;
    while (running) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 200)) continue;
        switch (e.type) {
        case SDL_EVENT_QUIT: running = 0; break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (pad) break;
            pad = SDL_OpenGamepad(e.gdevice.which);
            if (!pad) break;
            {
                SDL_GamepadType t = SDL_GetGamepadType(pad);
                style = style_of(t);
                char *map = SDL_GetGamepadMapping(pad);
                printf("Connected: %s\n", SDL_GetGamepadName(pad));
                printf("  reports type: %s (vendor %04x, product %04x)\n", SDL_GetGamepadStringForType(t),
                       SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
                printf("  the game uses: %s button names\n", style_name[style]);
                SDL_JoystickConnectionState cs = SDL_GetGamepadConnectionState(pad);
                printf("  connected by: %s\n", cs == SDL_JOYSTICK_CONNECTION_WIRED      ? "USB"
                                               : cs == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless (Bluetooth)"
                                                                                        : "unknown");
                printf("  touchpads: %d%s\n", SDL_GetNumGamepadTouchpads(pad),
                       SDL_GetNumGamepadTouchpads(pad) ? " (drag a finger on it)" : "");
                printf("  SDL mapping: %s\n\n", map ? map : "(none)");
                SDL_free(map);
            }
            fflush(stdout);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                SDL_CloseGamepad(pad);
                pad = NULL;
                printf("Controller removed.\n\n");
                fflush(stdout);
            }
            break;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: {
            /* a finger: x, y from 0 to 1 across the touchpad; moves at most ten times a second */
            static Uint64 last;
            Uint64 now = SDL_GetTicks();
            if (e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION && now - last < 100) break;
            last = now;
            printf("Touch finger %d %-5s x %.3f  y %.3f\n", (int)e.gtouchpad.finger,
                   e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN ? "down" : e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_UP ? "up" : "move",
                   e.gtouchpad.x, e.gtouchpad.y);
            fflush(stdout);
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            if (!pad || SDL_GetGamepadID(pad) != e.gbutton.which) break;
            int b = e.gbutton.button;
            const char *sw = switch_button(style, b);
            if (sw)
                printf("%s  SDL thinks it says: %-8s  the game presses Switch %s\n", place(b),
                       label(SDL_GetGamepadButtonLabel(pad, (SDL_GamepadButton)b)), sw);
            else
                printf("%s\n", place(b));
            fflush(stdout);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START))
                running = 0;
            break;
        }
        }
    }
    if (pad) SDL_CloseGamepad(pad);
    SDL_Quit();
    return 0;
}
