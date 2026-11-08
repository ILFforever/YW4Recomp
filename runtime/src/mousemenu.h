/* The mouse in the game's menus (the Yo-kai Pad and the pages it opens). mousemenu.c */
#pragma once

#include <stdint.h>

/* Returns 1 while the Yo-kai Pad or any of its sub-pages is open, 0 otherwise. */
int mousemenu_open(void);

/* Returns 1 when YW4_MOUSE_LOG=1 or YW4_MOUSE_SCRIPT is set. */
int mousemenu_log_on(void);

/* The window's thread, on a mouse wheel event: +1 up, -1 down. Returns 1 when a menu is open and takes it. */
int mousemenu_wheel(int dir);

/* The window's thread, once per input update: the mouse at (mx, my) relative to the picture's top-left, in a
 * picture of w x h window points (the window minus its black bars), its buttons (SDL masks). Returns Switch button bits to add to the game's input (D-pad steps, A, B).
 * *want_cursor: 1 while a supported menu is open (show the cursor: no relative mouse), 0 otherwise. */
uint64_t mousemenu_update(float mx, float my, int w, int h, uint32_t mouse_buttons, int *want_cursor);

