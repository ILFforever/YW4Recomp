# 0043 — Mods: a mod's romfs laid over the game's (LayeredFS)

Status: accepted, first version (2026-10-30). The English patch runs: the copyright warning and the town's quest
marker ("Head to Mt. Wildwood!") show in English.

## Context

The English translation is a Switch mod in Atmosphère's layout:
`contents/010086c00af7c000/English Patch/romfs/...`, 2.4 GB. Atmosphère's LayeredFS merges it with the game's RomFS:
a file at the same path replaces the game's, a new path is added. Our runtime already builds the game's RomFS
image itself from the owner's extracted `romfs/` folder at start (decision 0014), so a mod can join that build.

What the patch contains (measured against the owner's 2.2.0 romfs with `tools/yw4/cpk.py`):

- 8 replaced files: 7 rebuilt `.cpk` archives (`text/text_all.cpk`, `menu/menu.cpk`, `effect/effect.cpk`, four
  `ev_item` ones) and `data/cpk_list.cfg.bin`, the list of which file lives in which archive;
- 3,085 new loose files at paths that are inside archives in the base game (2,316 text, 714 menu, 49 effect, ...).

Its `cpk_list.cfg.bin` names the same files as the game's (none added or missing) but has fewer entries: the
game reads a file listed there from its archive and any other from the folder, so the loose files are what it
reads. 487 of them differ from their copy inside the patch's own `text_all.cpk`. The patch needs no change inside
an archive: whole-file replacement is enough.

## Decision

`romfs.c` scans the game's folder into its tree as before, then lays each mod's `romfs/` folder over that tree
(a same-named file takes the mod's path and size, new files and folders are added), then builds the image as
before. Reads need no change: each file is read from wherever its path points.

- Where: `YW4_MODS=<folder>`, default `work/mods`, `none` for no mods. Inside it, Atmosphère's layout
  `contents/010086c00af7c000/<mod>/romfs` or simply `<mod>/romfs`. Several mods go in name order; a later one wins.
- The log names each mod with its count of replaced and added files.

## Rejected

- Replacing single entries inside `.cpk` archives: the patch doesn't need it, and it would mean rebuilding
  archives or answering reads from inside them.
- Copying the patch into the owner's `romfs/` folder: changes their dump, no way back without a second copy.
