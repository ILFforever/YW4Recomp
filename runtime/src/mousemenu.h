/* The mouse in the game's menus (the Yo-kai Pad and the pages it opens). mousemenu.c */
#pragma once

#include <stdint.h>

/* Returns 1 while the Yo-kai Pad or any of its sub-pages is open, 0 otherwise. */
int mousemenu_open(void);

/* Returns 1 during a conversation (the talk window, from its first focus until the field's HUD comes back): the
 * mouse works it as a menu (bindings.c): the wheel moves between choices, left confirms, right cancels. */
int mousemenu_talking(void);
/* Returns 1 while the map is open: moving the mouse pans it (the left stick, window.c), the wheel zooms (the
 * right stick, bindings.c). */
int mousemenu_map(void);
/* Returns 1 while Mirapo's small screen is up over the map: the mouse works it as a menu instead (no panning, the
 * wheel steps its rows, left confirms, right goes back). */
int mousemenu_mirapo(void);
/* The field HUD's shortcut cross was updated (shortcutwheel.c): the field is back, so a map closed without the field's
 * menu focusing again (Mirapo's list backed out of) stops counting as open. */
void mousemenu_field_hud_seen(void);
/* Returns 1 while Naviwoof Guide's row of pins is up: the wheel steps left and right (bindings.c). */
int mousemenu_naviwoof(void);
/* Returns 1 while the battle's Stopwatching menu or a page it opened is up: the clicks confirm and go back. */
int mousemenu_stopwatch(void);
/* Returns 1 while a Yes / No box with its buttons side by side has the focus: the wheel steps left and right. */
int mousemenu_sideways(void);

/* Returns 1 when YW4_MOUSE_LOG=1 or YW4_MOUSE_SCRIPT is set. */
int mousemenu_log_on(void);

/* The window's thread, on a mouse wheel event: +1 up, -1 down. Returns 1 when a menu is open and takes it. */
int mousemenu_wheel(int dir);

/* The window's thread, once per input update: the mouse at (mx, my) relative to the picture's top-left, in a
 * picture of w x h window points (the window minus its black bars), its buttons (SDL masks). Returns Switch button bits to add to the game's input (D-pad steps, A, B).
 * *want_cursor: 1 while a supported menu is open (show the cursor: no relative mouse), 0 otherwise. */
uint64_t mousemenu_update(float mx, float my, int w, int h, uint32_t mouse_buttons, int *want_cursor);

