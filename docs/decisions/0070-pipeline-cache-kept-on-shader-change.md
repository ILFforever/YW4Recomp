# 0070 — The pipeline cache and journal are kept when only the shaders change

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: runtime/src/gpu_pcache.c
-->

Status: accepted. Measured 2026-11-14 on the owner's RTX 4070 laptop (see Checks). Changes one rule of
[decision 0063](0063-pipeline-cache.md); the rest of 0063 stands.

## Context

Decision 0063 threw the pipeline cache and the journal away whenever the stamp changed: another GPU, another
driver or another set of translated shaders. Every game update rebuilds the shaders, so every update started
from nothing. On NVIDIA that went unnoticed, because the driver keeps its own cache keyed by each shader's bytes,
and updates rarely changed those bytes.

The shader fix of 2026-11-14 (c386b89, the vertex outputs within 128 components) changes the bytes of every
vertex and geometry shader. Measured on the town walk with the old rule:

| | Pipelines built in play | Build time | Worst frame |
|---|---|---|---|
| A normal update (driver cache warm) | 136 | 0.56 s | 0.25 s as the town appears |
| Shaders changed, driver cache empty | 170 | 4.0 s | **2.9 s** as the town appears |
| Our cache kept, driver cache empty | 138 | 0.33 s | only at start-up |

Our own cache alone was enough; the freeze came from throwing it away. 0063 already listed keeping the journal
as an open point.

## Decision

The stamp's two parts are compared on their own:

- **Another GPU or driver** (vendor, device, `pipelineCacheUUID`), or no stamp: both files go, as before. Data
  from another driver is never handed to it.
- **Only the shaders changed:** both files stay and the stamp is updated. The log says so.
  - The cache's entries are keyed by the shaders' bytes: those of changed shaders just don't match any more,
    those of unchanged ones still do. Vulkan lets a cache hold entries that are never used.
  - The journal's records name the shaders by the game's keys and hold the pipeline state, which a shader
    rebuild doesn't change. At start the prewarm threads build them again from the new files, in the background,
    before the game asks for them. Records whose shader files are gone are skipped (as before).
  - A cache file over 64 MiB is emptied then (the journal fills it again), so stale entries can't pile up over
    many updates. It was 5 to 8 MiB after a full town walk.

## Checks

The update simulated: shaders translated as in 1.2.2, the town walk played once (cache and journal filled), then
the new shaders and NVIDIA's driver cache set aside (the worst case), then the walk again:

- 261 pipelines prewarmed in 0.66 s in the background, done before the first frame.
- In play: 130 pipelines asked for, 18 ms in all (cache hits); nothing when the town appears (was 2.9 s).
- The first frame takes about 1 s, as in every start-up.
