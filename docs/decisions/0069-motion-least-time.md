# 0069 — Above 30 frames/s a motion's time counts at least 1/30 s for its least time

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: runtime/src/framerate.c, recomp/overrides.toml, runtime/src/kernel.c, runtime/src/dispatch.c
-->

Status: accepted. Measured 2026-11-14 at 30, 60 and 120 frames/s (see Checks); not yet confirmed by the reporter.

## Context

Issue #9: at high frame rates a jump made while running is a small hop; standing still, or on the Switch, it is a
full jump. The game measures its time step every frame and scales its work by it (framerate.c), so most of it runs
right at any rate. This did not.

Measured with a replay whose clock steps a chosen rate (`YW4_REPLAY_HZ`, below) and the player's records dumped
every frame (`YW4_PROBE_DUMP`):

| | Peak above the ground | Rising part |
|---|---|---|
| 30 frames/s | +1.39 | about 0.23 s |
| 60 frames/s | +1.38 | about 0.23 s |
| 120 frames/s | +0.62 | 0.1 s |

The chain, from the jump's height back to its cause:

- The physics record's target height (`+0xb2c`) is copied every frame from the action parameters (`+0x94`), which
  the action state machine (`main+0x1bcd70`) fills from the current motion's data (`motion+0xbc`). At 120 it was
  1.4 for one frame, then 0.8.
- 0.8 is the take-off's own height. At 30 and 60 the player leaves the take-off (state 0x11) for the air (state
  0x10) one frame after it starts. At 120 it never left.
- Leaving needs "may leave" (`action+0xb1e`). `main+0x1c6170(action, dt)` adds dt to the time in the motion
  (`action+0xafc`) and sets it once that reaches the motion's least time (`motion+0xb8`, when bit 0 of
  `motion+0xc1` is set). The take-off's least time is about one 60 Hz frame. One 120 Hz step (1/120 s) falls short;
  while the flag is unset the state machine (`main+0x1c2aa0`) asks for the take-off again, which restarts it and
  sets its time back to 0. So the time never grows past one step.

## Decision

A hook on `main+0x1c6170` (`yw4_hook_motion_time`) runs the game's update, then, if the time step is under 1/30 s
and "may leave" is still unset: a motion whose least time is 1/30 s or less, and whose time is still under 1/30 s,
may be left. That is what the Switch does, where every frame counts 1/30 s: such a motion can always be left after
its first frame.

Why this and not the alternatives:

- **Not stopping the restart.** Asking for the same state again is how the state machine waits; changing it touches
  every state, not only motions with a least time.
- **Not adding to the motion's time.** `action+0xafc` is read elsewhere; only the one comparison changes here.
- **1/30 s, not 1/60 s.** The game was made for 30 frames/s. 1/60 s would fix 120 but leave least times between
  1/60 and 1/30 s stuck the same way at 60.
- Longer least times are counted as before, and at 30 frames/s (dt = 1/30) nothing changes. `YW4_MOTION_TIME_FIX=0`
  turns it off to compare.

## Checks

- Running jump, same replay and save: 30 +1.39 / 0.63 s in the air, 60 +1.38 / 0.65 s, 120 +1.38 / 0.68 s (was
  +0.62). Screenshots 0.1 s apart look the same at 60 and 120.
- Standing jump: the same at 30, 60 and 120 (see Standing jump).
- Town walk at 60 frames/s with the fix on and off: it ends in the same place; the pictures at frames 5000 and
  7300 differ by a frame or so of timing (camera and running pose a little apart). Expected: at 60 a motion whose
  least time lies between 1/60 and 1/30 s can now be left after one frame, as on the Switch, instead of two. With
  60 frames/s a real frame's step also jitters around 1/60 s, so a least time of one 60 Hz frame could be missed at
  60 too; counting 1/30 covers that.

## Standing jump

A first measurement suggested the standing jump fell faster at 60 and 120 than at 30. Measured again on its own
(walk a moment, stop, wait, jump): peak +1.37 to +1.39 about 0.5 s after take-off and back on the ground at about
0.9 s at 30, 60 and 120, within one 30 Hz frame of each other (at 30 gravity starts one bigger step later). The
first reading came from a jump taken right after a run toward a building; nothing to fix.

## Test tools added for this

- `YW4_REPLAY_HZ=<rate>` with `YW4_REPLAY_HZ_AT=<frame>` (kernel.c): during a frame-clock replay, each frame moves
  the game's clock 1/rate s instead of 1/60 from that frame on, so a route can load at 60 and then play as at
  another frame rate. `YW4_DT_LOG=1` shows the step the game got.
- `YW4_PROBE` lines now show s0 to s2 and the presented frame; `YW4_PROBE_X0=<hex>` keeps one object's calls;
  `YW4_PROBE_DUMP=<bytes>` prints that many bytes at x0 (or at the pointer chain `YW4_PROBE_DUMP_PTR=<off>[,<off>]`)
  as floats on each call (dispatch.c).
