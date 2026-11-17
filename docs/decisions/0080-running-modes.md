# 0080 — Running modes: run without holding B (issue #18)

<!-- decision
state: accepted
date: 2026-11-18
replaces: none
code: runtime/src/run.c, runtime/src/run.h, recomp/overrides.toml, runtime/src/bindings.c, runtime/src/bindings.h, runtime/src/gamemenu.c, runtime/src/window.c
-->

Status: accepted (owner, 2026-11-17 and 18: the four modes, one Run key, the layout of the Controls page). Measured
with replays at 30 and 120 frames/s (Checks); the owner played it by hand.

## Context

Issue #18: running needs B held, which is uncomfortable while turning the camera; the reporter asked for a toggle.
On the Switch B does two things: a tap jumps, a hold runs. A run toggle has to leave the jump alone.

## How the game runs (traced 2026-11-17)

With `YW4_PROBE_DUMP` (walking against running in one replay), `YW4_WATCHW` write watches, and `llvm-nm` on
`yw4.exe` to name the recompiled function behind each host write address:

- Walking and running are the same action state (`action+0x8b5` = 7); running is a top speed, 6.55 against 9.0
  (`action+0xa4`, `+0xab0`), copied from `+0x6e0`, chosen in `main+0x1c2aa0` by `action+0x614 == 2`.
- That byte is a copy of `action+0x234`, set in `main+0x1bcd70` from the player's command object
  (`cmd = *(action+0x10f8)`): `+0x234 = cmd+0xa9 ? 2 : 1`.
- `cmd+0xa9` is set by the field's player controller, `main+0x1fe950` (one object, called once a frame), when its
  "run this frame" flag (`ctrl+0x5d`) is set. That happens when B's hold time (`ctrl+0x54`, + dt while B is down)
  is **greater than** a threshold from the game's config (`main+0x1531390`, 0.25 s), or, in the "moving" state only,
  while a carry-on timer (`ctrl+0x58`, - dt a frame) is above zero. `ctrl+0x5c` is "B was down last frame": when it
  is 0 the hold time starts again from 0. When B goes up, a long hold refills the carry-on timer (about 0.55 s); a
  short one is a tap, which jumps.
- B reaches the game as bit `0x4000` of the input object (`*(main+0x14d1008)`; held at `+0x8`), mapped from the
  Switch's bit 1 by the table at `main+0x12bdfb0`. The pad itself is read only in `main+0x964550`.

## Decision

1. **A hook before the player controller** (`yw4_hook_player_run`, `recomp/overrides.toml`): while running is
   wanted and the real B is up, it sets the controller up as if B had been held past the threshold: `ctrl+0x5c = 1`
   and `ctrl+0x54` a little over the threshold (the game's check is "greater than"). The controller runs by its own
   path, in any action state, and clears both values itself at the end of the frame.
2. **B stays the game's.** While the real B is down, and on the frame it goes up (when the game judges a tap by its
   hold time), the hook stays out, so a tap still jumps.
3. **Stopping is immediate:** when running is switched off, the carry-on timer the game set at the end of the last
   faked frame is cleared (else about 0.55 s more running). A real B hold still carries on as the game does.
4. **Four modes** (Controls page, RUNNING; `YW4_RUN_MODE`): DEFAULT (the game's; the Run key does nothing and is
   greyed out), ALWAYS (hold Run to walk), TOGGLE (Run switches running; off when a battle starts), HOLD (hold Run to
   run). Nothing changes in battles.
5. **One Run input of ours** (`INPUT_OURS`: read, never sent to the game): Left Shift; on a pad the left stick press,
   shared there with Close hints (no button is free), not marked as a clash. Close hints moved to Left Ctrl on the
   keyboard; a settings file from before (no `RUN` line) moves its Left Shift there when Left Ctrl is free.
6. **The Controls page:** RUNNING, then Run under it, below NINTENDO ICONS. Every row's help is now "what it is"
   above "how to change it" in a fixed-height area, so the table no longer resizes from row to row; a game input's
   "what it is" comes from the F2 sheet's action names (exploring, battles, cutscenes; the set shown first).

Why not the alternatives:

- **The carry-on timer alone** (tried first): the controller clears it outside the "moving" state, so a walk with
  turns fell back to walking; and it is cleared at the start of the frame unless the player ran the frame before.
- **Holding B for the player** (the input layer): the hold would also begin with a press, and a jump would need B
  let go and pressed again by us; the game's own run path is cleaner.
- **Patching the run speed**: running is more than a speed (animation, `cmd+0xa9` read elsewhere).

## Checks

Replay `work/inputs/run_modes.txt` on `save_base_walk` (schoolyard): three stretches of walking forward, the last with
a B tap. `YW4_RUN_KEY_TEST=<from>-<to>[,...]` holds the Run input (a replay feeds the Switch's buttons, not keys);
the player's speed from `YW4_PROBE_MEM` on `main+0x1bcd70`.

| Mode | Walk 1 | Walk 2 | Walk 3 + B tap |
|---|---|---|---|
| DEFAULT | 6.55 | 6.55 | 6.55, jump |
| ALWAYS, Run held in walk 2 | 9.0 | 6.55 | 9.0, jump |
| TOGGLE, pressed before walk 3 | 6.55 | 6.55 | 9.0, jump |
| HOLD, Run held in walk 3 | 6.55 | 6.55 | 9.0, jump |

- The same at 120 frames/s (`YW4_FPS=120`, `YW4_REPLAY_HZ=120`): every mode as above.
- TOGGLE on at frame 3905: running at 3906; off at 3960: walking at 3961 (was about 0.55 s).
- Left: while B is held for a tap (about 0.1 s) the player is at walking speed for those frames, then jumps and runs
  on. Covering it would mean faking B's hold during a real press, which is what broke the jump.

## Not done

- A pad button of its own for Run (none is free); the player can rebind it.
- An enemy-only target switch in battles (the game has allies only, Tab, and everyone, D-pad left / right): a
  separate feature, not started.
