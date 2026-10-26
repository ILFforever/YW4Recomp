# 0029 — Translating command lists on a worker thread

Date: 2026-10-26
Status: accepted (owner chose it over more small fixes and over the takeover first: "sure go A"; asked whether the
takeover would invalidate it: it doesn't, the takeover's draws go through the same worker)

## The question

After decision 0028 the busy towns were no longer GPU-bound, but the game's work per frame was still 21-23 ms in the
owner's evening street, with spikes past the 33 ms of a 30 frames/s frame. Where does the CPU time go, and what is
on the chain the frame waits for?

## What we measured

The sampling profiler (`YW4_PROFILE`), now able to sample one thread (`YW4_PROFILE_THREAD`):

- **The main thread** runs ~7 ms of game code a frame and waits for the render threads most of the rest.
- **The busiest render thread** (thread 20) waits half of its time too; of its busy time ~55% is the game's own
  per-draw work (the model renderer `f_a45890`, state setup `f_974cd0`, ...) and **~45% is ours**: translating the
  command lists to Vulkan, including the driver's time in our calls.
- Our translation ran **inside the game's `nvnQueueSubmitCommands`**, on the game's render threads, under one lock,
  before the call returned: 9-11 ms a frame across the three render threads, all of it on the chain the main thread
  waits for.

Two small fixes cut it first (dynamic state set only when it changed; textures remember their image: 11.3 → 8.9 ms),
but the frame barely moved: the cost was *where* the work ran, not only how much.

## The decision

**One worker thread of ours runs the translation** (`runtime/src/gpu_queue.c`). The game's submit puts the command
list in a queue and returns; the worker records and submits the Vulkan command buffers in the game's order.

- **Presents** go through the same queue, so a frame is shown after its work; the game's vsync pacing stays on its
  own thread.
- **Syncs.** A fence (`nvnQueueFenceSync`, a window texture's acquire sync) records a *ticket*, the number of jobs
  queued so far, not a Vulkan submission (the work may not be submitted yet). Waiting for a ticket waits for the worker
  to have run that job, then for the Vulkan submissions it had made by then. Nothing the game waits for is signalled
  before its work has been recorded and run, so the game's reuse of command memory, buffers and textures stays safe
  by the same rules as before.
- **Freeing** a memory pool, texture, program or texture/sampler pool goes through the queue, after the queued work
  that may still read it. **Re-setting a program's shaders** first waits for the queue to empty (only while loading).
- Hooks that read the game's data for our passes (shadow.c, gbuffer.c) are unchanged: they run on the game's thread
  at the draw and copy what they need into the command list.
- `YW4_GPU_THREAD=0` runs everything in the caller as before.

What could go wrong, and why it shouldn't: the worker reads the game's memory (texture uploads, constants copied for
shadow quality) a little later than before. By NVN's rules the game may not change memory the GPU will read until the
GPU is done, which our syncs only report after the worker; a game that breaks that rule would race on the Switch too.

## Result

Owner's evening street (`save_base_lag`): game work per frame 21 → 12.9 ms (p99 29 → 22.5), the busiest render thread
38% → 22% of a core, our translation on the game's threads 8.9 → 0.4 ms. Town 9.3 ms, late-game forest 7.2 ms,
schoolyard 9.1 ms, all at 30 frames/s. Same-frame A/B 0 texels differ for the G-buffer (town), the shadow maps (forest)
and the material pass (schoolyard). 45 tests pass.

Not yet checked by a run: cutscenes (replays drift and don't reach them), battles, movies, menus: the owner's playtest.

## What it means later

- The takeover's own draws are records in the game's command lists: they run on the worker too.
- One worker is now the GPU's only feeder. If it becomes the bottleneck (it does ~9 ms a frame in the busy street),
  the next step is splitting a frame's command lists over two workers recording secondary command buffers.
- 60 frames/s needs the game's timing patched as well; the CPU budget (16.7 ms) is within reach in these places now.
