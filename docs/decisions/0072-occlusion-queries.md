# 0072 — The game's occlusion queries on Vulkan, and its draws skipped on the GPU

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: runtime/src/gpu_query.c, runtime/src/gpu.c, runtime/src/gpu_draw.c, runtime/src/nvn_cmdbuf.c, runtime/src/shadow.c, runtime/src/gpu_shadow.c, runtime/src/window.c
-->

Status: accepted. Measured 2026-11-14 on the owner's RTX 4070 laptop, saves `lategame` and `lag` (evening town).
Not yet seen on AMD, Intel or Linux.

## Context

`ReportCounter(SAMPLES_PASSED)` wrote 0 since decision 0017 (headless first), and `SetRenderEnable` /
`SetRenderEnableConditional` were recorded but never done. Answering "visible" instead changed nothing on screen,
which made the queries look unused.

Measured with `YW4_COUNTER_LOG`: before each shadow cascade and before the main view, `f_9ab8c0` draws one 36-vertex
box per object into the 240×135 depth target, each between two reports 16 bytes apart. The model renderer
(`f_a45890`, at `a4a18c`) then wraps that object's draws in `SetRenderEnableConditional(RENDER_IF_NOT_EQUAL, first
address)`: the GPU skips the draws of an object whose box had no visible sample. The CPU never reads the counts.
Answering "visible" changed nothing because the condition itself was ignored. Evening town: 270 pairs, 920
conditions, 878 conditional draws a frame, 498 of them hidden.

## Decision

`runtime/src/gpu_query.c`:

- A pair of reports is one Vulkan occlusion query (precise where the GPU has `occlusionQueryPrecise`) around the
  draws between them. The first value is written as 0, the second is the query's count, copied into the game's own
  memory on the GPU (`vkCmdCopyQueryPoolResults` into the imported pool, decision 0021). No wait, no read-back.
- The conditional draws use `VK_EXT_conditional_rendering` on the second value, begun and ended around each single
  draw. Nothing of ours (our passes, copies, upscaling) can fall under a condition by accident.
- Our own draws that stand in for a game draw take the game's condition along: the shadow casters (decision 0024),
  and the G-buffer, material and takeover draws that run in place.
- The NVN mode order (`RENDER_IF_EQUAL` = 0, `RENDER_IF_NOT_EQUAL` = 1) is from memory of the NVN headers, not
  checked against one. The check is the picture: with the order inverted, the ~380 visible objects of the town
  would vanish and the 498 hidden ones would be drawn. The town looks the same with and without culling.

Fallbacks, none of which fired in the tests: without the extension, or for an address no pair of ours wrote, the
draws are made as before; a pair that spans two render passes with draws in both answers "visible".

## Why not answer on the CPU

The CPU could read the counts a frame late and skip the draws while recording. But the game asks within the same
frame, on the GPU, so a CPU answer would be one frame stale: objects would pop in a frame late when they come out of
hiding. Conditional rendering is what the Switch's GPU does here.

## Checks

- Counts are real and varied, 0 to 64800, in both scenes.
- `YW4_OCCLUSION=0` (all conditional draws made) against the default, same frame numbers across runs: 0.6%
  (`lategame`) and 1.6% (`lag`) of pixels differ, from idle-animating characters and a passing car.
- Vulkan validation: no error from this code.
- Frame rate: unchanged within the noise. The town is CPU-bound, so skipping draws buys no speed here; it is on by
  default because it is what the game asks for. `YW4_OCCLUSION=0` turns it off.

## Not done

- `SetRenderEnable(false)`: not done, logged once if it ever happens (not seen).
- Whether any game code also reads the counts on the CPU was not checked.
