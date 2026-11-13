# 0068 — Mods' copies of one data file are merged entry by entry

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: runtime/src/modmerge.c, runtime/src/romfs.c, runtime/src/main.c, launcher/src-tauri/src/ready.rs, launcher/ui/app.js
-->

Status: accepted. Tested 2026-11-14 with the owner's six mods (see Checks).

## Context

Mods lay files over the game's (decision 0043). When two mods ship the same file, the later one's copy replaces
the earlier one's, so the earlier mod's change does nothing. Decision 0067 merged `cpk_list`; the launcher's Mods
tab then warned about every other shared file.

Measured on the owner's downloads: Crank-a-kai Overhaul, Free Tag Battles and Unshackled Equipment all ship
`data/common/gamedata/item/item_config_0.13.66.cfg.bin` (1,566 entries in 2.2.0). Overhaul adds 1 item and raises a
count (2 entries), Tags edits 6, Unshackled edits 201, and **no entry is changed by two of them**. They only clash
because the file is replaced whole.

Level-5's `.cfg.bin` data files are flat lists of entries (a name crc and a few int, float or string values), so
two copies can be compared entry by entry.

## Decision

- **Merge** (`modmerge.c`): for each `.cfg.bin` that two or more mods replace (not `cpk_list`, 0067), each mod's
  copy is compared with the game's own copy (Myers diff over whole entries, after the common start and end), and
  the result takes every changed run of entries from the mod that changed it. Two mods' runs that share base
  entries, or two inserts at one place, are a clash: then nothing is merged, the later mod's copy is used as before,
  and the log names both. A copy that adds new kinds of entries (a different name table at the end) isn't merged.
- **The game's copy** is its loose file when it has one, else read from the archive the game's own `cpk_list` names,
  with our own CPK reader and CRILAYLA decompressor (ported from `tools/yw4/cpk.py`).
- **Writing**: entries, `0xFF` padding to 4 and 16, the strings once each in first-use order, the name table copied.
  Checked: writing back any of the four item files above gives the same bytes.
- The merged file is kept in memory (`File.mem`, as 0067), so nothing is written and removing a mod undoes it.
- **The launcher asks the game** (`yw4 --mod-clashes`, folders from game.ini): every pair of mods (on or off) that
  can't both have their way with a file, one `clash` line each. The Mods tab groups those by file, so it warns only
  about real clashes and the launcher and the game can't disagree. Games before 1.1.1 don't know the flag (an old
  build takes it for a start and writes a log, which would push aside the log a bug report sends), so for them the
  launcher keeps comparing file names.

## Rejected

- Merging without the game's copy (by majority of the mods' copies): can't tell which of two mods changed an entry.
- Merging a clash entry by entry ("later mod wins per entry"): a mod's entries depend on each other (Overhaul's
  count goes with its new item); mixing two mods' versions of one run could break both.
- Treating identical changes from two mods as agreement: two mods each adding an item would both raise a count to the
  same number, and the merge would keep one count for two new items.

## Checks

- Real files: the archive read matches `tools/yw4/cpk.py` byte for byte; the merge of the three item files equals an
  independent Python merge of the same changes (1,567 entries, all 209 changed entries kept).
- `yw4 --mod-clashes` with the six mods (1.5 s): only Overhaul + Unrestricted Crank (capsule prizes: the same entries)
  and the translation + Overhaul (item text, hidden by the launcher's known-harmless rule).
- Game start with all six: `item_config_0.13.66.cfg.bin: merged from 3 mods`, the game reads the merged size and plays.
- Tests: `RomfsTest.test_mods_data_files_merged` (a merge, a clash, `--mod-clashes`), launcher `test_clashes_from`.
- Not checked in play: each mod's feature (the new item in the Crank-a-kai, a tag battle, unrestricted equipment).
