# 0084 — Saves: all or nothing, as on the Switch

<!-- decision
state: accepted
date: 2026-11-20
replaces: none
code: runtime/src/savejournal.c, runtime/src/savefs.c, runtime/src/ipc.c
-->

Status: accepted. Tested with tests/test_savejournal.py (crash-like runs as separate processes) and one hand test on
the owner's late-game save copy (2026-11-20): an autosave, a manual save into a new slot 2 and its autosave, four
Commits, no backup left over, and the Autosave row loaded slot 2 afterwards.

## Problem

The code review of 2026-11-19 found three ways to lose a save:

1. **Half-written saves.** The Switch journals save data: writes only count once the game commits, and a save that
   stops in the middle is thrown away whole. savefs.c wrote straight into the files and its Commit did nothing. A
   crash, a closed window (it exits at once) or a power cut mid-save left a file half old, half new, or cut short
   (SetSize runs first). There was no backup.
2. **Save files never closed.** Every opened save file's `FILE*` stayed open for the whole run. Windows' C runtime
   allows 512; on Windows `remove()` also fails on an open file, so a Diary slot deleted in the same run came back.
3. **The Autosave's slot was a guess.** All Diary slots share one AUTOSAVE file. Which slot's game made it was guessed
   from the newest timestamp in the slots' summaries. A clock set back (end of summer time, another time zone) made
   an older slot look newest; loading the Autosave then made that slot the one the next manual save overwrote.

## Decision

1. **A journal beside the files** (savejournal.c). Before the first change to a file since the last Commit, its old
   content is copied to `<file>.yw4bak` (written to `.yw4bak.part`, synced, then renamed, so a backup cut short is
   never used); a file new in this save gets an empty `<file>.yw4new` mark. Commit syncs the changed files, then
   deletes the backups and marks. The first time a run opens the save folder it undoes whatever a run left behind:
   each backup is put back, each new file removed. The game never sees these files (left out of directory lists).
2. **Save files close when the game closes them.** IPC services got an optional `close` hook, called when the last
   session or domain slot holding them closes: a written save file is synced and closed; a directory listing is freed.
3. **The Autosave's slot is recorded at its Commit**, in `<user folder>.autosave-slot` (outside the folder the game
   lists). The timestamp guess stays only for saves made before this.

The writes still go into the real files at once, not into a copy: the game reads a file back through other handles
in the same save, and a copy would need every handle to follow it.

## What we rely on, and what it costs

- **The game commits every save.** The logs show each written file followed by a Commit (USERDATA, then HEADERSAVE,
  each committed on its own; autosaves too). A write the game never committed is undone at the next start, as on the
  Switch.
- Each save copies the files it changes once (a few hundred KB) and syncs them: a few milliseconds, during a save.
- A crash between USERDATA's Commit and HEADERSAVE's keeps the new USERDATA with the old HEADERSAVE, as on the Switch,
  where each Commit is its own.
