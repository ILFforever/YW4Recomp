/* The Diary's stacked rows (owner, 2026-11-17: "the save slots stack, not Left / Right"): three of the game's data
 * files changed in memory at start, from the owner's own copies (no game data in the repo). The research and the
 * formats: docs/research/diary-save-slots.md, "Stacked rows". The Diary code that fills the rows: savefs.c. */
#ifndef YW4_DIARYROWS_H
#define YW4_DIARYROWS_H

#include <stddef.h>
#include <stdint.h>

#define DIARY_ROW_COPIES 6  /* five save rows, then the Autosave row (savefs.c's DIARY_ROWS) */

/* The three files, by their RomFS paths. */
#define DIARYROWS_MODEL "data/common/menu/104_save/104_save00/save00_06/save00_06.g4pkm"
#define DIARYROWS_LOCATOR "data/common/gamedata/menu/obj/diary_load_locator.objbin"
#define DIARYROWS_SETTING "data/common/gamedata/menu/cfg/save_load_menu_setting.cfg.bin"

/* Each takes the game's copy and returns the changed one (malloc'd), or NULL when the copy isn't the one this
 * expects (another layout, a mod's own change, already changed): then nothing is changed and the Diary pages. */
uint8_t *diaryrows_model(const uint8_t *b, size_t n, size_t *out_n);    /* the row anchor model: six row bones */
uint8_t *diaryrows_locator(const uint8_t *b, size_t n, size_t *out_n);  /* a row copy on each of them */
uint8_t *diaryrows_setting(const uint8_t *b, size_t n, size_t *out_n);  /* the menu: six row copies, not two */

/* The in-game Save's hint with its slot paging (Left / Right): the English line of a language's system text. */
uint8_t *diaryrows_save_hint(const uint8_t *b, size_t n, size_t *out_n);

/* 1 unless YW4_DIARY_SLOTS=0 (the game's single slot). */
int diaryrows_slots_enabled(void);
/* 1 unless YW4_DIARY_ROWS=0 or YW4_DIARY_SLOTS=0 (the game's two rows; with the slots off, its single slot). */
int diaryrows_enabled(void);

#endif
