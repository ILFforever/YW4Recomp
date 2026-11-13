# 0067 — Several mods' cpk_list files are merged

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: runtime/src/romfs.c, launcher/src-tauri/src/ready.rs
-->

Status: accepted. Tested 2026-11-14 with the English translation and four gameplay mods together (see Checks).

## Context

`data/cpk_list.cfg.bin` names every file in the game and the `.cpk` archive it is in. The game reads a listed file
from its archive and a file with no archive (the value 0xFFFFFFFF or an empty string) from the folder. A mod that
ships loose files "frees" them by clearing their archive, in its own copy of the whole list (4.7 MB, 65,836 entries).

With mods laid over each other in name order (decision 0043), only the last mod's list is used. Measured on the
owner's downloads (2026-11-14):

- the English translation frees 2,984 files (5,157 entries: 8,504 paths are listed more than once, the same file
  in several archives, and a mod frees every entry of a file);
- Crank-a-kai Overhaul, Unrestricted Crank, Free Tag Battles, Uncapped Training and Unshackled Equipment each start
  from the original game's list and free only their own 1-7 files.

So a gameplay mod sorted after the translation sent ~2,980 translated files back to the Japanese archives, and one
sorted before it ("Tags") had its own files ignored. On a modded Switch the player merges the lists by hand.

Several of these mods also ship only the romfs contents (`<mod>/data/...`, no `romfs/` folder), which the loader
did not find.

## Decision

- After laying the mods over the tree, if two or more mods ship a `cpk_list`, the one used is the **last mod's list
  with every file any mod freed also freed** (archive set to an empty string, as the mods free files, in place: the
  offset of a NUL already in the string table). "Freed"
  is measured against the game's own list, per path (a path is loose only if all its entries are). Anything else a
  later mod changed in its list stays as it was. The result lives in memory (`File.mem`); no file is written.
- If any list can't be read as a cfg.bin, the old behaviour (last one wins) stays and the log says why.
- A mod folder with `data/` directly is a mod too (runtime `check_add_mod`, launcher `mod_roots`), and a folder that
  isn't a mod is looked into one level more: downloads wrap the mod in a folder of their own
  (`free_tag_battles/Tags/romfs`, found 2026-11-14 when it didn't load from `contents/<title id>/`).
- Two mods replacing the same file still can't both apply: the later one wins, as before (e.g. three of these mods
  ship `item_config_0.13.66.cfg.bin`).

## Rejected

- Writing the merged list to disk: needs a writable place that matches both the dev tree and launcher installs,
  and a stale copy could be read after a mod is removed.
- Building the list from the game's own and only the mods' freed entries: would drop any other change a mod made to
  its list (none of the measured mods made one, but nothing stops one from doing it).

## Checks

- Log with the translation + Overhaul + Tags + Uncapped Training + Unshackled Equipment: `cpk_list: 5 mods' lists
  merged, 5185 entries freed by an earlier mod kept loose`, the same count as an independent Python reader of the
  same files.
- Late-game save: text stays English (location banner, hints); the trace shows the item file read from Unshackled
  Equipment and the shop file from Crank-a-kai Overhaul, two different mods.
