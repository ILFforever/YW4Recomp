# 0064 — Holding 30 and 60 frames/s: the vsync tick, or the display's refresh (to measure)
<!-- decision
state: proposed
date: 2026-11-12
replaces: none
code: runtime/src/framerate.c, runtime/src/vi.c, runtime/src/perf.c
-->

Status: proposed (2026-11-12). Both ways are in the program; the tick is the default. Nothing is measured yet:
this record is the plan for the measurement and the rule for choosing.

## Context

At FRAME RATE 30 and 60 the present waits so that frames come at an even pace. There are now two ways to wait:

- **Tick (ours, the default).** A thread counts an emulated 60 Hz vsync (`vi.c`); the present waits for the next
  tick, or every second one at 30. The same tick drives the game's vsync event and the input update. Since 232829c
  that thread runs at the highest priority, so that it is not woken late.
- **Refresh (`YW4_PACE=refresh`, from Quandiy's fork, commit 58b448c).** The presenting thread keeps its own
  deadline. Its period is a whole number of the display's refreshes when the cap divides the refresh rate within
  1% (two refreshes of 8.33 ms on a 120 Hz screen), else 1/cap. It sleeps to 1 ms before the deadline and spins
  the rest. A frame more than half a period late starts the deadline again from now.

Quandiy measured the tick wait on a Radeon RX 6650 XT at 120 Hz, before 232829c: about 1 frame in 400 was shown
for 33 ms with 6 ms of work, because a tick was lost. In the fork the refresh way was the default.

What differs, besides who keeps time:

| | Tick | Refresh |
|---|---|---|
| Period | 16.667 ms, whatever the display | Matched to the display when the cap divides its refresh |
| A 59.94 Hz display | Drifts against it: a frame repeats or drops about every 17 s | Follows it (60 is within 1%) |
| Frame and game's vsync event / input | Same clock | Two clocks: their phase can wander |
| A late frame | Waits for the next tick: one late frame costs a whole tick | Starts again from now |
| Cost | A sleeping thread | Up to 1 ms of spinning per frame on the presenting thread |

## Decision

Keep the tick as the default until the two are measured on the same machine, then make refresh the default **if
it has clearly fewer long frames and brings no new problem**; else keep the tick and leave refresh as an option
(or remove it, if it is never better).

## The measurement

What to run (the same replay each time, as for [baseline.md](../baseline.md): `python -m tools.dev.run` with the
town walk, `work/testsaves/save_base_walk` and `work/inputs/walk.txt`, 135 s):

| Run | FRAME RATE | `YW4_PACE` | Frame generation |
|---|---|---|---|
| 1, 2 | 60 | unset, then `refresh` | off |
| 3, 4 | 30 | unset, then `refresh` | off |
| 5, 6 | 60 | unset, then `refresh` | on (FSR; DLSS if the GPU has it) |

- On a 60 Hz display, and on a 120 Hz one if there is one. Each pair twice, alternating, so that one odd run
  doesn't decide it (`python -m tools.dev.perf_compare` alternates two settings).
- Settings at which the GPU has room (the cap must be what limits the frame rate, not the GPU): for example
  1080p or upscaling on. At the baseline's 4K settings the game runs near 50 and neither way is doing the pacing.
- Check the log's first lines: `yw4: pacing: tick ...` or `yw4: pacing: refresh, period ... ms at ... Hz`.

What to read, from each run's performance summary (`work/perf/perf_<time>.txt`, also in the log every 600 frames):

- `pacing <way> at <cap> frames/s: N frames over 25 ms = x%, standard deviation y ms`: frames longer than one and
  a half of the cap's frame time (25 ms at 60, 50 ms at 30), and the spread of the frame times.
- The line above it: the 99th percentile and the longest frame.
- By eye: the menus' scrolling background and a slow camera pan (uneven pacing shows there first); input feel.

With frame generation the swapchain presents without waiting and the generated frames are paced by their own
thread (`gpu_fg.c`), so runs 5 and 6 show whether the game's frames still arrive evenly enough for it.

New problems to look for with refresh: judder that comes and goes over tens of seconds (the two clocks' phase),
input that feels later, a higher CPU use of the presenting thread, and the replay ending in another place.

## Consequences

- One more variable to test with (`YW4_PACE`, [debugging.md](../debugging.md)); the summary line is there for
  both ways at caps of 30 and 60.
- If refresh wins, the default flips in `framerate_pacing` and this record is updated with the numbers.
