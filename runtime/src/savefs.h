/* Save data in host folders (savefs.c). */
#ifndef SAVEFS_H
#define SAVEFS_H

#include <stdint.h>

#include "ipc.h"

#define YW4_PROGRAM_ID 0x010086C00AF7C000ull  /* Yo-kai Watch 4++ (CLAUDE.md) */

/* The folder all saves go under (yw4's third argument). 0 on success. */
int savefs_init(const char *dir);
/* The folder the saves are in (savefs_init's): the bug report copies it. */
const char *savefs_root(void);

/* The save of (program, user): with create = 0, an IFileSystem in *out, or fs
 * ResultTargetNotFound if it doesn't exist; with create = 1, makes it (out unused). */
uint32_t savefs_open(uint64_t program_id, const uint8_t uid[16], int create, Service **out);

/* When the game last saved, in Unix seconds: its last Commit in this run, else the newest file of its save as it
 * was opened (a save from an earlier run); 0 before any save is known. The overlay's popups say how long ago. */
int64_t savefs_last_save(void);

/* 1 while the Diary (title > Continue, or Save in the game) shows with the 10 slots on: the mouse's wheel then
 * steps the slot (bindings.c). Any thread. */
int savefs_diary_open(void);
/* 1 while that Diary shows its slots stacked (title > Continue with the six row copies): the wheel then moves up and
 * down the rows, as Up / Down do. */
int savefs_diary_rows(void);
/* The pad's buttons as the game gets them (svc_hid.c): A held back on an empty save row of those rows. */
uint64_t savefs_filter_buttons(uint64_t buttons);
/* swkbd.c, before a name popup the game gave a default for (token: that text; girl: "the girl he loves"): the boy's
 * name after the title was left without loading a save starts a new game, so the player picks its Diary slot first. */
void savefs_new_game_naming(const char *token, int girl);
/* pause.c: the title opened (SceneTitle), at the start and at each Return to Title. */
void savefs_title_opened(void);

#endif
