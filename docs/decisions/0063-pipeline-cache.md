# 0063 — Pipeline cache and journal on disk, and when they are thrown away
<!-- decision
state: accepted
date: 2026-11-12
replaces: none
code: runtime/src/gpu_pcache.c, runtime/src/gpu_draw.c
-->

Status: accepted (2026-11-12). The code is from Quandiy's fork (Quandiy/YW4Recomp, commits 69bf471 and 58b448c)
with the changes below. The numbers are Quandiy's, on a Radeon RX 6650 XT; the owner has no AMD card and the
merged code has not run on a real GPU yet.

## Context

Every graphics pipeline was built from nothing in every run, on the GPU thread, the first time a draw needed it,
and every translated shader was loaded from its files the same way. On the owner's NVIDIA card the driver's own
disk cache hides this ([baseline.md](../baseline.md): no stalls). On the Radeon it did not: 40 to 300 ms per
pipeline, 117 at once when the town appears, 35 hitches in four minutes; shader loads of 5 to 10 ms each.

## Decision

Taken from the fork:

- **A `VkPipelineCache` kept in `work/pipelines.cache`** (`gpu_pcache.c`), passed to every pipeline build, saved
  every 10 s when something was built and at exit (temporary file, then rename).
- **A journal, `work/pipelines.journal`:** each pipeline the game builds, as its shader keys and state. At start,
  background threads build the journal's pipelines into the cache while the game boots, so the game's own builds
  are cache hits. The shadow-mask prebuild (decision 0025's pipeline, `prebuild_thread`) goes through the same
  cache.
- **Every translated shader loaded on background threads at start** (`YW4_SHADER_PRELOAD=0`: off).
- `load_shader` splits its interface file by hand. `strtok` keeps its place in one hidden variable, and
  `prebuild_thread` already called `load_shader` beside the GPU thread: a bug before the fork's change too.

Changed from the fork:

- **Background builds that skip the draw are off** (`YW4_ASYNC_PIPELINES=1` turns them on). The owner: an object
  missing for a few frames is worse than a hitch that happens once.
- **The prewarm and preload threads run at low priority** (as `prebuild_thread`, 232829c).
- **The files are thrown away when what they were made with changes.** `work/pipelines.cache.stamp` holds one
  line: the GPU's `vendorID`, `deviceID` and `pipelineCacheUUID`, and a number for the shaders. A different line,
  or no stamp, deletes the cache and the journal before either is read. The cache's own header (the same three
  values) is also compared before the data goes to the driver; Vulkan's specification leaves that to the
  application.

**Why the shader number is a directory listing.** It is a sum over the names, sizes and modification times of the
`*.spv` and `*.iface` files in the two shader folders (`YW4_SHADER_DIR`, `YW4_NATIVE_SHADER_DIR`). The other
choices were worse:

- *The folder's own time*: it changes when a file is added or removed, not when one is written again in place,
  which is what the shader build does.
- *A hash of the files' contents*: exact, but it reads thousands of files at every start; the start-up work of
  8013a94 was about not doing that.
- *A stamp written by the build*: two programs would have to agree, and a shader rebuilt by hand
  (`python -m recomp.shader`) would be missed.

The listing costs one directory read per folder (on Windows the listing carries sizes and times; a time over
20 ms is logged). Its weakness: a shader rebuilt to the same bytes has a new time, so the files go although
they were still good. That costs one run of first-time builds, after a game update.

## Consequences

- Three small files in `work/`, next to the other run-time files; the cache is replaced in place and its stale
  entries go whenever it is thrown away. `YW4_PIPELINE_CACHE` and `YW4_PIPELINE_JOURNAL` move them or turn them
  off (`none`).
- Open: the journal's records (shader keys and pipeline state) would still be right after a shader rebuild, and
  keeping it would let the next start prewarm the new cache in the background. It is deleted with the cache for
  now, as decided; keeping it is a small change in `gpu_pcache_init`.
- Not taken from the fork: FSR 3.1 as the default on Radeons without RDNA 4 (it belongs with decision 0062, whose
  first start only writes settings the panel has), and refresh-aligned pacing for 30 and 60 frames/s (it replaces
  the wait on the vsync thread that 232829c tuned; not measured together).
- To check on a real GPU: the files appear in `work/`; a second run logs `pipelines to prewarm` and fewer
  `stall:` lines than the first; and the same on an AMD card.
