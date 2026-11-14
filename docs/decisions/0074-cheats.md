# 0074 — Cheats: a page in the overlay, and unlimited Crank-a-kai

<!-- decision
state: accepted
date: 2026-11-15
replaces: none
code: runtime/src/cheats.c, runtime/src/cheats.h, runtime/src/settings.c, runtime/src/gamemenu.c, recomp/overrides.toml, launcher/ui/app.js, launcher/src-tauri/src/ready.rs
-->

Status: accepted. The crank cheat was checked in the owner's play (late-game save, 2026-11-15): the count stayed at
30 while coins went down. The overlay page and the launcher switch build; not yet seen on screen or tried from the
launcher.

## Context

The owner wanted unlimited Crank-a-kai cranks per day, with each crank still costing a coin. The game allows 30 a
day on the owner's save (an older copy of it had 3). The prize table (`capsule_config.cfg.bin`) has no limit, and
the community mod that changes it only blanks prize conditions, so the limit had to be found in the running game.

## How the count was found

- Memory searches for 2-byte and 4-byte numbers going 30, 29, 28 found nothing that held: the count is **one
  byte**.
- The game autosaves after each crank. Copying the autosave after each one and comparing the copies showed one byte
  stepping down by one per crank (`AUTOSAVE/data.bin` offset `0x822`), with the day's limit (30) in the next byte.
- A byte-pattern search (`YW4_FINDBYTES`, `runtime/src/memscan.c`, a test tool) found the same bytes in memory, and
  a write watch on them caught one write per crank.

## The command that writes it

The write comes from `main+0x296560`, a script command (registered as `0xd0c06ed4` by the startup function at
`main+0xe7b0`) that the Crank-a-kai menu's script calls: "save one byte by ID". It looks the ID up in a sorted table
of (ID, slot) pairs and stores the value at `[save data + slot + 0x288]`. No game code calls it directly.

A hook just before its store (`main+0x2966a8`: x9 the ID, x20 the value) logged every change in a play session
(`YW4_TRACE_SAVEBYTE=1`). Only the Crank-a-kai's three IDs changed anything:

| ID | Slot | What |
|---|---|---|
| `a45a65ec` | 11 | Cranks left today: set to the limit at the day's reset, one down per crank |
| `9076e662` | 12 | The day's limit, set at the reset |
| `2f508d80` | 13 | A byte set at the reset (seen 8, 15, 3, 5) |

## The cheat

Unlimited Crank-a-kai (`YW4_CHEAT_CRANK`): when a store to `a45a65ec` would lower it, the hook puts the old value in
x20, so the count stays. The day's reset (a raise) still happens, and coins are spent as usual: they are an item
count elsewhere in the save. The hook reads the switch at every store, so it changes while the game runs.

Turned on with no cranks left (0, or 255, -1 as a byte), the count is set to 1 so the next crank can happen; it then
stays there (owner, 2026-11-15). The byte is found as the command finds it, through the ID table.

## Where the switch is

The owner chose option B of the design canvas (<https://claude.ai/artifact/XcwzdE1gju4aNQJgxgE2e5>): a **Cheats**
page in the overlay's side menu, between Overlay and Report a bug, with the cheats as rows like the Settings page's.
The choice is saved in the settings file like any setting.

The launcher's Settings has a **Cheats** switch (on by default). Off, the game starts with `YW4_CHEATS=0`: the page is
hidden, the keys pass over its place, and no cheat acts whatever the settings file says.

## Not done

- The overlay page and the launcher switch have not been seen running.
- Report a bug moved one place down the side menu: a recording that reaches it with Down from Resume now lands on
  Cheats.
