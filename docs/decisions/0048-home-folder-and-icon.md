# 0048 — The exe finds its own files from anywhere; the game's icon from the user's files
<!-- decision
state: accepted
date: 2026-11-03
replaces: none
code: tools/yw4/icon.py, recomp/build.py
-->

Status: accepted (2026-11-03). Both checked: the game started from an unrelated folder finds its shaders, mods
and DLC and draws the town; the exe and its window carry the 4++ icon made from the owner's update NSP.

## Context

`yw4.exe` is given its game files (`yw4 <exefs work folder> <romfs folder> <save folder>`), but finds its own
files by paths starting `work/`: the translated and our shaders (`work/shaders/glsl`, `work/build/shaders`),
mods (`work/mods`), logs, perf files, screenshots and input recordings. Those are relative to the folder it is
started from, so it only worked when started from the repo (as `play_from_save.bat` does with `cd`). A
double-click, a shortcut or a launcher starting it from elsewhere would find no shaders. (`settings.ini` and the
DLLs were already found next to the exe.)

Players also expect the game's own icon on the exe and its window. The icon is game data (rule 1): it can't be
in the repo, only made on the player's PC from their own files.

## Decision

**Home folder.** At start-up, before the log opens, the game makes the three paths it was given absolute and
then changes to its home folder: `YW4_HOME` if set, else the folder above `work/` when the exe sits in
`<home>/work/build/` (what `recomp.build` makes: the owner's repo, and an install the launcher builds the same
way), else it stays where it was started. Every existing `work/...` path then works unchanged; the log names the
folder (`yw4: home ...`). Chosen over renaming every path to exe-relative folders: one place, no change to the
layout tools and docs rely on.

**Icon.** `python -m tools.yw4.icon` (and `tools.yw4.extract` at the end of an extraction) takes the icon from
the user's own NSP or XCI:

- it lives in the Control NCA (with `control.nacp`), which extracted ExeFS/RomFS folders don't have: those users
  get no icon, nothing else changes;
- only NCAs under 64 MB are unpacked (the Control one is under 1 MB); hactool names the Control one; the
  update's is preferred (the 4++ icon);
- the 256x256 JPEG (`icon_AmericanEnglish.dat`, else another language) is drawn as PNGs at 256, 48, 32 and 16
  pixels by Windows' own System.Drawing (through PowerShell) and packed into `<work_dir>/icon/yw4.ico`: no new
  Python dependency;
- `recomp.build` compiles it as resource 1 (`llvm-windres`, from the same llvm-mingw as clang) into `yw4.exe`
  when the file exists; the window takes resource 1 (`SDL_HINT_WINDOWS_INTRESOURCE_ICON`).

## Consequences

- The game can be started from anywhere; relative paths in environment variables (`YW4_REPLAY=work/...`) are
  now relative to the home folder, which is where they pointed before for every existing use.
- The icon stays in `work/` (gitignored) and in the built exe, which is never shared.
