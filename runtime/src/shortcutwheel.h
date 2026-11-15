/* The shortcut wheel (owner, 2026-11-15; decision 0075): while exploring, holding the key bound to L (1 to start)
 * shows the game's eight field shortcuts in a ring; the mouse points at one and letting go of the key uses it, by
 * pressing the D-pad direction (shortcuts 1-4) or L and the direction (5-8, the game's hold-L set) for the player.
 * Letting go in the middle uses nothing. An arrow key (the D-pad) pressed while the key is held closes it and the
 * old way goes on (L + D-pad). Its look is the "Searchlight" mockup the owner chose (work/mockups/shortcut-wheel):
 * Watch Mode's teal lens over the pick, the rest dimmed, the shortcut's name in the middle. Off (the Controls page,
 * YW4_SHORTCUT_WHEEL in settings.ini): nothing of it runs and the key is plain L. */
#ifndef YW4_SHORTCUTWHEEL_H
#define YW4_SHORTCUTWHEEL_H

#include <stdint.h>

#include "ui.h"

int swheel_enabled(void);
void swheel_set_enabled(int on);

/* The window's thread, once per input update: b the Switch buttons read from the keys and the pad, key 1 while a
 * key bound to L (exploring set) is held, ok 1 while the wheel may open (exploring: no battle, game menu,
 * conversation, map, overlay or pause, and the game has the mouse), reads the game's input reads so far. Returns
 * the buttons to give the game. */
uint64_t swheel_update(uint64_t b, int key, int ok, uint64_t reads);
/* 1 once the game has built the field HUD's shortcut cross (its hook ran): not before the first load is done. */
int swheel_hud_ready(void);
/* 1 while the wheel is open: the mouse's movement goes to swheel_motion, not the camera. */
int swheel_open(void);
void swheel_motion(float dx, float dy);

/* 1 while the wheel is on screen: open, or its use / cancel animation still playing. */
int swheel_visible(void);

/* Drawing (window.c, over the running game, blended like F2's quick look): its part of the overlay's canvas in
 * design units (x, y, w, h), a number that changes when the picture would, and the picture itself on a cleared
 * (see-through) canvas. */
void swheel_rect(float r[4]);
uint32_t swheel_look(void);
void swheel_draw(const UiCanvas *c);

/* The game's shortcut icons (slot 1-8, straight RGBA, *w x *h), taken from the player's game files at build time
 * (tools.yw4.wheel_icons, decision 0048's way); NULL when the build had none: the wheel draws arrows instead. In a
 * generated file (recomp/build.py). */
const uint8_t *yw4_wheel_icon(int slot, int *w, int *h);

#endif
