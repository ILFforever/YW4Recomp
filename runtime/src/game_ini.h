/* game.ini next to yw4.exe: the game's folders and settings when it is started without arguments (game_ini.c,
 * decision 0050). */
#pragma once

/* Reads game.ini into the environment (a variable already set wins); 1 if there was one. */
int game_ini_load(void);
