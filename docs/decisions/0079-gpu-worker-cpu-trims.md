# 0079 — GPU WORKER CPU: push descriptors and smaller per-draw costs

<!-- decision
state: accepted
date: 2026-11-17
replaces: none
code: runtime/src/gpu_draw.c, runtime/src/gpu.c, runtime/src/window.c, runtime/src/vk.h, runtime/src/gpu_queue.c, runtime/src/gpu.h, runtime/src/perf.c
-->

Status: accepted (owner, 2026-11-17: "I want the CPU to be cut down as much as possible"; "if it is better and it
doesn't prove to break things keep it in"). Step log: `work/tasks/cpu-optimization-log.md`, steps 57-85.

## Context

On the slow street (scene `lag`, 1440, DLSS performance, owner's settings) the game is CPU bound: the GPU is busy
about 6 ms of an ~11.5 ms frame. The busiest thread is not a game thread but ours: the GPU worker
(`gpu_queue.c`, decision 0029), which turns the game's command lists into Vulkan. It used a whole core, about 11 ms a
frame on its own, and the GPU and then the main thread waited for it (the main thread ~3 ms a frame in
`nvnSyncWait`). An earlier session had looked at the game's mutexes instead (lock spinning, `YW4_LOCK_SPIN`); they
cost about 1.2 ms a frame across all threads, mostly not on this path.

The per-thread CPU report did not list the worker (it is a host thread, not a guest one), so this was not visible.

## Decision

1. **The worker in the per-thread report**: `gpu worker (host): N% of a core, X ms/frame` every 600 frames.
2. **Push descriptors** (`VK_KHR_push_descriptor`, where the GPU has it): fragment and compute programs push their
   bindings into the command buffer instead of a descriptor set being allocated, written and bound for each draw.
   Vertex and geometry programs keep allocated sets: a pipeline layout may have only one push-descriptor set.
   `YW4_PUSH_DESC=0` turns it off. Measured: +9.4% fps (3 pairs, every pair), the worker -1.3 ms a frame.
3. **Geometry fingerprint 8 bytes a step** (`geo_print`, run for every small vertex/index range of every draw):
   +3.8% (3 pairs); the same rewrites are caught.
4. **Texture keys**: a texture that is neither screen-sized nor the shadow map's shape no longer asks the size
   settings (render height, upscaling, presented images); the widescreen check's second lookup only with widescreen
   on. Same keys by construction. +3.6% (3 pairs).
5. **Smaller trims, kept although their gain is within the machine's noise** (the laptop's speed swings ~15% between
   runs; less work helps slower CPUs more): only the dynamic states that changed are set again; image keys compared
   and hashed a word at a time; samplers found through a hash index and the upscale mip bias worked out once per
   mode; a small memo of recent geometry ranges in front of the 16 MB table.

Every change was checked with the Vulkan validation layer (only the two old messages: the swapchain image in
Streamline's draws, and its semaphore) and screenshots at the same replay frames.

## Tried and not kept

- **Lock spinning** (`YW4_LOCK_SPIN`, earlier session): +11% on the snow bridge only, nothing elsewhere; stays off.
- **Reusing identical descriptor sets** within a command buffer: only 7% could be reused (offsets differ per draw);
  3% slower. Push descriptors replaced the idea.
- **Flush rules instead of the page scan** (`YW4_UNTRACKED=1`): slower; the per-draw checks cost more than the scan.
- **The page scan split over helper threads**: the scan (~1.1 ms a frame, 1,092 MiB walked for ~1,900 written pages)
  took as long on 4 threads: GetWriteWatch calls don't overlap. A log line now reports its cost every 600 frames.
- **Scanning only the pages something reads** (2026-11-18): only textures we uploaded and geometry we copied read
  the write generations, so the scan was limited to those pages ("live" pages, tracking reset when a page joins, runs
  closer than 16 pages scanned as one call). Short runs: 0.26-0.64 ms a frame against 0.94-1.30. Dropped:
  - The saving fades: pages never leave the set, and after 9 minutes of play it held 600 MiB and cost 0.90 ms
    (whole pools: 0.97). Making pages leave again is more code for a background thread's fraction of a millisecond.
  - A content audit (a texture or copy the pages call unchanged, compared with its upload) over 8 scenes, live
    against whole pools: textures were all caught by the next scan, but geometry copies left stale for more than
    3 frames were 108 against 40, cause not found.
  - Windows itself was fine: GetWriteWatch and ResetWriteWatch on part of a region behave as documented (a standalone
    test). An early "missed write" was the test's own snapshot taken before the reset; resetting must come before
    anything reads the storage.
  - The whole-pool scan also leaves 40 geometry copies stale for more than 3 frames (ranges of 100 bytes to 5 KB)
    in those 8 scenes: not looked into yet.

## Consequences

- The first start after this rebuilds the driver's pipeline cache once (the fragment layouts changed).
- The worker is still the busiest thread (~11 ms). The remaining large step would be translating command lists on
  more than one thread, a large change to the renderer; not started.
- How to judge a change here: by fps or frame time in alternating pairs. The worker's CPU ms also falls when it only
  waits more (the split scan looked like -0.7 ms and gained nothing).
