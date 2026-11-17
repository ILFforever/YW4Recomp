/* Running modes (issue #18; run.c): DEFAULT (the game's), ALWAYS, TOGGLE, HOLD, all with one Run input. */
#ifndef YW4_RUN_H
#define YW4_RUN_H

enum { RUN_DEFAULT, RUN_ALWAYS, RUN_TOGGLE, RUN_HOLD, RUN_MODES };

int run_mode(void);
void run_mode_set(int mode);
const char *run_mode_name(int mode);  /* upper case, for the Controls page */
const char *run_mode_save(int mode);  /* the settings file's value */

#endif
