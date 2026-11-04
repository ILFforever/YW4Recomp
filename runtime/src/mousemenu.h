/* The mouse in the game's menus (prototype: the Yo-kai Pad's icon grid). mousemenu.c */
#pragma once

#include <stdint.h>

/* The window's thread, once per input update: the mouse at (mx, my) relative to the picture's top-left, in a
 * picture of w x h window points (the window minus its black bars), its buttons (SDL masks). Returns Switch button bits to add to the game's input (D-pad steps, A, B).
 * *want_cursor: 1 while a supported menu is open (show the cursor: no relative mouse), 0 otherwise. */
uint64_t mousemenu_update(float mx, float my, int w, int h, uint32_t mouse_buttons, int *want_cursor);
