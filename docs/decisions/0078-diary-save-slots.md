# 0078 — Five Diary save slots, stacked, and one Autosave

<!-- decision
state: accepted
date: 2026-11-17
replaces: none
code: runtime/src/savefs.c, runtime/src/savefs.h, runtime/src/diaryrows.c, runtime/src/diaryrows.h, runtime/src/romfs.c, runtime/src/svc_hid.c, runtime/src/kernel.h, runtime/src/gamemenu.c, runtime/src/gamemenu.h, runtime/src/bindings.c, runtime/src/swkbd.c, runtime/src/pause.c, recomp/overrides.toml
-->

Status: accepted (2026-11-17). First written as 0077 by an earlier session (Gemini, 2026-11-16); renumbered because
0077 is the Discord Rich Presence record on the `discord-presence` branch. Reviewed and corrected 2026-11-17 (see
"Changes after review"), then the title's Diary changed from one paged row to five stacked rows and the Autosave became
one shared file (see "Stacked rows"). The research behind it, with the formats and every game function used:
`docs/research/diary-save-slots.md`.

## Context

On the Switch, Yo-kai Watch 4++ gives a user one manual save (`USERDATA00/data.bin`), one autosave
(`AUTOSAVE/data.bin`), a two-row summary (`HEADERSAVE/data.bin`: row 0 the manual save, row 1 the autosave) and
shared system data (`SYSTEM/data.bin`). The Diary menu (class at `main+0x14d87f8`, `work/recomp/main_128.c`) has two
fixed `0x288`-byte rows of data at `menu + 0x210` and `menu + 0x498`, filled by `f_761aa0` from the two `0xa0`-byte
records of `headersave_obj` (`*(u64 *)(*(u64 *)(main + 0x1548fd8) + 0x57f8)`). New Game clears the save structs in
memory only; the old files are overwritten at the new game's first save (research, Round 4).

The Diary's look is data: its menu setting (`save_load_menu_setting.cfg.bin`) gives the row layer `diary_load_slot`
2 copies, and its locator object puts them on two bones of the anchor model `save00_06.g4pkm` (research, "Stacked
rows").

## Decision

**5 slots and one Autosave** (the owner, 2026-11-17: "5-10 save slots and then under all those 1 autosave slot which
saves your latest game save"; "autosave is autosave"). On by default; `YW4_DIARY_SLOTS=0` gives the game's single slot
back.

1. **Folders.** Slot 1 is the user's save folder as before, so an existing save is slot 1 with no migration. Slot
   *n* (2..5) is the folder `SLOTnn` in it (`SLOT02`..`SLOT05`): `host_path` sends `/USERDATA*` and `/HEADERSAVE*`
   there while that slot is active. `AUTOSAVE` and `SYSTEM` stay in the main folder, shared. A slot's folder doesn't
   exist before its first save, so for paths in a slot folder only, a missing parent folder, a folder opened, or a file
   opened for writing is made on demand. Anywhere else a missing path is still "not found".
2. **One Autosave.** Each slot's summary row 1 is the autosave its game last made; the newest of them is the one on
   disk (`newest_auto_slot_locked`). Loading it continues that slot's game, whose saves then go to that slot. Deleting
   a slot leaves the Autosave (and the slot's summary while it holds the Autosave's row). A slot counts as used when it
   has its save or its game made the Autosave.
3. **Summary cache.** `parse_slot_from_disk_locked` reads each slot's `HEADERSAVE` (Level-5 TLV, magic `0x10f2ffee`)
   into `slot_cache` at start, at each Commit and when the Diary opens. Row 0 counts only if its valid flag is set and
   its `USERDATA00` exists; row 1 if the shared `AUTOSAVE` exists.
4. **Title > Continue: five stacked rows, then the Autosave row** (owner: "the save slots stack, not Left / Right").
   At start the runtime changes three of the owner's game files in memory (`diaryrows.c`, `romfs.c`, after the mods;
   all three or none, and freed from their archives in the list): six row copies, six new row bones (saves 100 apart
   from y -250, the Autosave 128 below), a locator line for each. `YW4_DIARY_ROWS=0` keeps the game's two rows.
   The game's own row code draws every row: its setup is skipped for copies 1+ and our paint pass sends its copy
   lookup (`f_9f3e90`) to the row being drawn. The focus grid is made 6 rows; the cursor's attach line is moved over
   all six; Up from the top wraps to the Autosave; the first focus is the newest row, as the game does. The labels
   have no `"<n>/5"` (the rows show which slot it is). Rows are drawn 15..20 from the top, so a row's "Latest!" badge
   stands over the row above (their texts stay at 20). The game's "has row N a save?" and "load row N" Lua calls
   (`f_2672d0`, `f_266fc0`), which only know rows 0 and 1, are answered from the focused row. On an empty slot A is
   held back from the game (`savefs_filter_buttons`): no "load this diary?" box. The mouse wheel moves up and down
   the rows, and left and right on their Yes / No box.
5. **Save in the game: one card that pages.** Its menu has no row list, so Left / Right, L / R, the left stick and the
   mouse wheel change the slot, its name prefixed `"<n>/5 "`. It opens on the slot being played. Its hint reads
   "[Left][Right] Change Slot   [A] Save   [B] Back" (the English system text's line, changed in memory at start; the
   D-pad's two icons, which show the bound keys on a keyboard). Paging stops while the menu is opening, while its
   "Would you like to save all data to your diary?" box has the input (the menu's `+0x1a6` / `+0x1ac` are 0; that box
   isn't counted at `main+0x5511a84`) or while a save job runs.
6. **Clean HEADERSAVE.** The save writes both rows of `headersave_obj`, but only updates one (`f_32ec30`), so before
   it runs (`f_8a7b10` hook) the slot's own unprefixed rows are put back. No label and no other slot's row reaches
   the file.
7. **New Game asks for a slot** (owner: "choose on beginning which save slot to write to"). Just before the boy's name
   popup (swkbd.c), `savefs_new_game_naming` shows an overlay list of the 5 slots (`gamemenu_choice_begin`), the first
   empty one chosen; the game waits, as for the name entry. A new game is one that left the title without loading a
   save; once per new game. An occupied slot asks "Overwrite Diary n/5?" (No by default). Its old files stay until
   the new game's first save: until then the slot reads as empty (`fresh_slot`). Replays and
   `YW4_NEWGAME_SLOT=<1..5>` answer at once.
8. **Delete:** on the title's Continue, X on an occupied slot arms it for 5 s ("Press again to delete": no button
   named, it differs by device), X again deletes its save. Any other button or a slot change cancels; nothing on the
   Autosave row. Its hint shows it: "Delete" with the X button's icon before Select / Back (text `0xbdaf66c1`, the
   `diary_load` script's only, changed in memory where it is the English line). On a keyboard the icon is the X key,
   which deletes there too (it is the game's Minus, left out on that screen); Tab, the X button's key, still does. Not from the in-game Save (2026-11-17): it had refused only the slot being played ("In use: can't delete"),
   and the hint there has no room for it.
9. **Boot:** if slot 1 is empty but another slot has a save, the first used slot is made active.

## Why not other ways

- A list drawn by the overlay over the Diary: quicker, but not the game's look. The owner chose the game's own rows.
- Editing the game's files on disk, or shipping changed copies: they are derived from the game (rule 1 of
  `CLAUDE.md`); the runtime makes them from the owner's files at each start instead.
- A per-slot Autosave (the first version): the owner wants one ("the game tells you that the save is newest
  anyways"), and the game's own checks look for `AUTOSAVE` in the active slot's folder, which said "There is no save
  data available." after a save in another slot.
- A slot picker for New Game in the game's own UI would need a menu we don't have; the overlay already has a popup
  the game waits on (the name entry, decision 0053's overlay).

## Not verified

- The TLV reader in `parse_slot_from_disk_locked` was checked on the test saves only.
- Return to Title, then New Game, in one run.
- X delete on the stacked rows, and the mouse wheel there and on their boxes (the owner's hands).
- Other languages than the English patch: the in-game Save's hint is only changed where the line is the English one.

## Tested (2026-11-17)

- Stacked rows on the late-game test save and the owner's save (`dev_run_20261117_055132` .. `_073106`): six rows,
  the newest focused at open with its card; Up / Down through all six, wrapping both ways; A on an empty slot does
  nothing; A on a save asks and loads that slot's save; A on the Autosave asks and loads it; text and badges drawn
  right. Opening takes the same time with and without the stacked rows (832 ms, 26 frames; our drawing ~1 ms).
- In-game Save on the owner's save (`dev_run_20261117_065407`): opens on the slot loaded; Right in its box only moves
  the box's choice, after it the slot pages; the hint shows the arrow keys.
- New Game with `YW4_NEWGAME_SLOT=2`: chosen once at the boy's name, not again at the girl's; no save file changed
  through both names and into the intro (`dev_run_20261117_025213`).

## Still to do

- The game's "denied" sound for A on an empty slot (A does nothing now).
- The rows slide in as the game's two did (the new bones have no animation tracks).

## Changes after review (2026-11-17)

- New Game: the first version's hook (`f_32e540`) never ran in three runs, so a new game saved into the slot active at
  start, over its save. Now the picker before the boy's name, with an overwrite question.
- Folders `SLOT01..SLOT09` (one less than the slot shown) renamed `SLOT02..SLOT10`, then five slots `SLOT02..SLOT05`.
- On-demand file and folder creation was on for every path; now only in a slot's own folder.
- Deleting from the in-game Save is off (2026-11-17; before, only the slot being played was refused).
- `hid_current_buttons` declared in `kernel.h` beside `yw4_hid_update`, not in `savefs.h`.
- The title's Diary: one paged card became five stacked rows; the per-slot Autosave became one shared file; the
  "card shows the other row" problem of the paged card went with it.
