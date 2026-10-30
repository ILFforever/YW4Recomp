/* The frame rate (framerate.c): the game's own limiter is off, the present holds the rate the panel picks. */
#pragma once

#include <stdint.h>

int framerate_index(void);  /* the panel's option: 0 30, 1 60, 2 120, 3 144, 4 vsync, 5 unlimited */
void framerate_cycle(void);
int framerate_hz(void);     /* frames per second to hold, 0 = no limit, -1 = the display's refresh (vsync) */
float framerate_game_dt(void);  /* the game's time step of its latest frame, seconds */
void framerate_wait(void);  /* at the present, on the presenting thread: until this frame's time is up */
