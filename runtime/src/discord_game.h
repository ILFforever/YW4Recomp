/* What Discord Rich Presence shows about the game (discord_game.c, decision 0077): where the player is (the title's
 * menus, the field, a battle), the current area's name and the Watch Rank, read live from the game. */
#ifndef YW4_DISCORD_GAME_H
#define YW4_DISCORD_GAME_H

#include "discord.h"
#include "yw4rt.h"

/* The field's player controller, once a frame on the game's thread (run.c's hook): reads the area and the rank, and
 * looks the area's name up with the game's own text when the area changes. Nothing while DISCORD STATUS is off. */
void discord_game_field_frame(CPUState *ctx);

#endif
