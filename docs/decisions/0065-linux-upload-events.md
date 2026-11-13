# 0065 — Linux: textures and geometry follow the game's events, not page tracking

<!-- decision
state: accepted
date: 2026-11-13
replaces: none
code: runtime/src/gpu.c, runtime/src/nvn_objects.c, runtime/src/gpu.h, docs/debugging.md
-->

Status: accepted (2026-11-13).

## Context

The game's memory pools are its own memory, imported into Vulkan as they are (decision 0021). A texture is uploaded
again, and a vertex or index range copied again into video memory (decision 0028), when the CPU has written one of
its pages since. On Windows the host says which pages were written (`GetWriteWatch`). On Linux `host_written_pages`
had no implementation, so every pool counted as "untracked": every CPU-visible texture was uploaded again every
frame, and geometry was read across PCIe from system memory.

A Linux tester (issue #3) measured 20-28 fps in town on an RX 6600 XT and an Intel UHD 730, with the GPU fans idle.
His log showed "378 texture uploads" every few frames. In WSL the title screen alone re-uploaded 1,768 times in about
500 frames, and only 129 of those were real changes (the movie).

## Why not page tracking on Linux

The obvious way is Linux's equivalent of write-watch: `PAGEMAP_SCAN` (kernel 6.7+), soft-dirty bits, or
write-protect + `SIGSEGV`. All of them watch the page tables, and the pools are host memory shared with the GPU
driver. In the kernel 6.12 source, amdgpu calls `hmm_range_fault` with write access over every userptr buffer on
**every submission** (`amdgpu_cs_parser_bos`), and its MMU notifier waits on the buffer's fences. i915 pins the
pages again with `FOLL_WRITE` after any invalidation, and a write-protect is one. So every page would look written
every frame, plus the cost of the faults. WSL could not have shown this: it renders with llvmpipe, which has no
such driver path.

## Decision

On a host that can't track writes, uploads follow the events the game gives:

- **Textures** are uploaded again when
  - a newer registration (`nvnTexturePoolRegisterTexture`) of the same memory is drawn with. Draws use the
    registration's copy, so loading a new texture into old memory always goes through one.
  - our own code wrote its pages (`WriteTexels`: `gpu_note_written`).
  - it is a movie plane (one level of R8, 2D) and a sample of its content (every 64th word) changed.
- **Geometry** copies are stale when the game flushed one of their pages since the copy (`nvnBufferFlushMappedRange`,
  `nvnMemoryPoolFlushMappedRange`: `gpu_note_flushed`), checked at every use.

## Evidence (Windows, where page tracking works, `YW4_REUPLOAD_LOG=1`)

- **Textures, 7 scenes** (town walk, busy spot, slow street, late game, Yo-kai World, forest cutscene, schoolyard):
  apart from the title movie's Y/U/V planes, no texture's content ever changed after its first upload without a
  new registration. 0 `untracked rule MISSED` lines. The movie planes are not flushed, hence their own check.
- **Geometry, 2 scenes**: all 31,522 copies made again whose content had changed were of ranges the game had
  flushed since. The 32 unflushed ones were unchanged.
- **`YW4_UNTRACKED=1`** (the Linux rules on Windows), busy spot / slow street, uncapped: 118 / 93 fps, against
  120 / 105 with page tracking. The old Linux behaviour, faked with `YW4_GPU_REUPLOAD=1 YW4_VRAM_GEOMETRY=0`: 24 / 17
  fps, with 449 / 683 stall lines.
- **Screenshots**, Linux rules against normal, at the same frames of frame-locked replays: they differ only where
  two normal runs also differ (characters' idle animation, an NPC, wind on bushes, minimap dots).

- **The owner's own play session** (late-game save, a battle, Yo-kai Pad screens, 16,385 frames, page tracking on,
  `YW4_REUPLOAD_LOG=1`): 0 `untracked rule MISSED` for textures and for geometry. All 186,787 geometry copies whose
  content had changed had been flushed; the 78 unflushed ones were unchanged. Menu icons (64x64, 32x32, 256x16) had
  their pages written with the same content. A second session on another map (22,206 frames): 0 `MISSED` again;
  the map's textures loaded into memory older ones had used (1024x1024 and 512x512 maps, icons) all came with a new
  registration, and all 651,337 changed geometry copies had been flushed.

The `MISSED` check only means something with page tracking on: with `YW4_UNTRACKED=1` the rules decide the uploads,
so a texture they miss is never uploaded and never logged.

## Not covered

The slow street's 12% gap to page tracking is not looked into. Not measured on a real Linux GPU yet.
