# 0086 — The game's memory pools imported as a cached memory type

<!-- decision
state: accepted
date: 2026-11-21
replaces: none
code: runtime/src/gpu.c
-->

Status: accepted. Not yet confirmed on the reporter's GPU (issue #25); the old choice stays behind `YW4_POOL_CACHED=0`.

## Problem

Issue #25 (AMD RX 9070 XT, Windows, AMD driver 26.11): while riding the bike, the minimap flickered frame by frame
between the map and an empty purple box with one small square. Walking was fine. It did not happen on an RTX 4070
or on an RX 580 under Linux (RADV), and `YW4_IMAGE_BLOCKS=0` did not change it.

The reporter's videos show the minimap's own picture (the map drawn into its 340×304 / 516×460 images) missing
in some frames. On the owner's PC the minimap watch (`YW4_MINIMAP_WATCH=1`, gpu_watch.c) counts the same work
standing, walking and riding: 101 draws into each minimap image, one clear, one draw that shows it, no missing or
freshly uploaded textures. The bike adds no pass and no image; it only makes the map move further each frame, so
the quads the game writes for it change more.

Those quads live in the game's UI vertex ring, which it rewrites every frame. Decision 0028 leaves such ranges in
the game's memory pools (a range copied more than a few times stays there), so the GPU reads them straight from
the game's memory when it runs the draw.

## Cause (most likely)

The pools are the game's own memory imported into Vulkan (`VK_EXT_external_memory_host`). The import took the
first memory type the driver allowed, with no properties asked for. The game's memory is ordinary cached CPU
memory, written through the CPU's caches.

| Driver | Allowed for an imported pointer | Taken before |
|---|---|---|
| AMD Windows (780M, 26.8.1) | 1 (`HOST_VISIBLE \| HOST_COHERENT`), 3 (+ `HOST_CACHED`) | 1 |
| NVIDIA (RTX 4070 laptop) | 2 (`HOST_VISIBLE \| HOST_COHERENT`), 3 (+ `HOST_CACHED`) | 2 |

On AMD the uncached type is write-combined system memory: the GPU's reads of it do not look in the CPU's caches
(PCIe no-snoop). Memory meant for it is mapped write-combined by the driver, so nothing sits in a cache. The game's
memory is not: a line the game has just written can still be in the CPU's cache, and the GPU reads the older
bytes from RAM. Data written just before it is drawn and rewritten every frame (the minimap's quads, constant
buffers) is the most exposed. NVIDIA keeps its system-memory reads coherent whatever the type, and RADV only lets
an imported pointer have its cached type, which is why neither showed it.

## Decision

Import the pools as a `HOST_VISIBLE | HOST_CACHED` type when one is allowed (GPU reads then see the CPU's caches),
otherwise the first allowed type as before. `YW4_POOL_CACHED=0` brings the old choice back. The log names the type
taken (`yw4: gpu: the game's memory imported as memory type N (flags …; allowed types …)`), so a player's log
answers which one their driver used.

## Measured (2026-11-21, the reporter's save and settings, walk then ride)

- AMD 780M (integrated, AMD Windows driver): type 3 now, type 1 before; 21-23 frames/s either way. The bug did not
  show on it with the old type either: it shares the CPU's memory controller rather than reading over PCIe, so it
  is no test of the cause.
- RTX 4070 laptop: type 3 now, type 2 before; 96-110 frames/s either way (within run-to-run spread).

## Not checked

- On the reporter's RX 9070 XT: a build with this change is needed (`YW4_POOL_CACHED=0` then shows the old
  behaviour, so the same build tests both).
- Whether the AMD shadow streaks of issues #8 and #19 (AMD on Windows, "as the camera turns") have the same cause:
  shadow matrices come from constant buffers the game rewrites each frame, so it is possible.
