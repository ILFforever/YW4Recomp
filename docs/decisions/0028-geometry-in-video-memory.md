# 0028 — The game's geometry in video memory

<!-- decision
state: accepted
date: 2026-10-26
replaces: none
code: runtime/src/gpu.c, runtime/src/gpu_passes.c
-->

Date: 2026-10-26
Status: accepted (the owner reported towns at 20-25 frames/s; the fix is on by default, `YW4_VRAM_GEOMETRY=0` turns
it off)

## The question

Some towns in the owner's late-game save never reached 30 frames/s: 20-26, always in the same places and looking the
same way. On the Switch the game needed an overclock for a steady 30, but this is an RTX 4070 laptop GPU. Where does
the time go?

## What we measured

**The slow places were GPU-bound.** Over a 10-minute walk, the slow stretches had the CPU's work at 14-17 ms a frame
and the GPU's at 37-45 ms (a frame at 30 frames/s has 33 ms). Switching our shadow pass, PCSS or our G-buffer draws on
and off changed nothing: the cost was in what every draw goes through.

**A per-pass GPU timer** (`runtime/src/gpu_passes.c`, F5) writes a GPU timestamp at every change of render targets
and reports where each frame's GPU time goes. In the owner's evening street (`work/testsaves/save_base_lag`):

| Pass | Draws | Vertices | GPU time |
|---|---|---|---|
| Lit material pass | 714 | 1,639k | 15.6 ms |
| G-buffer | 643 | 1,603k | 15.5 ms |
| Our shadow pass | | | 5.2 ms |
| Occlusion boxes (240x135) | 283 | 28k | 0.27 ms |
| Everything else | | | about 2.5 ms |

The two main-view passes took ~10 ns per vertex, and grew with the number of draws and vertices, not with what is on
screen. Restarts of the render pass and texture uploads inside them were counted and were not it (1-2 a pass). The
occlusion boxes, small meshes, cost under 1 µs a draw.

**The cause: every vertex came from system memory.** Decision 0021 imports the game's memory pools into Vulkan as
they are (`VK_EXT_external_memory_host`): no copies, the GPU reads the game's buffers where the game wrote them. For
vertex and index data that means reads across PCIe, every frame, for every draw. Fine for small scenes; a street of
houses at 1.6 million vertices drawn twice is not.

## The decision

**Each draw's vertex and index ranges get a copy in video memory** (`runtime/src/gpu.c`, "geometry in video
memory"): one device-local arena of 1 GiB; a range is copied the first time a draw uses it and again only when the CPU
has written one of its 4 KiB pages since (the write generations the pools already keep for textures, from the
host's page write tracking). Draws, the in-place G-buffer/material/shadow draws and our shadow pass bind the copy.

- Copies are made outside rendering, so a draw that needs one ends the render pass first: once, when a model first
  appears.
- **Ranges the CPU keeps writing** (UI, effects: rewritten every frame) are copied a few times, then left in the
  pool, read from system memory as before; copying them every frame would end the render pass at every such draw.
- **When the arena is full** (many areas visited), it is emptied at the start of the next submission, after the GPU
  has finished everything submitted, and fills again from what is drawn: a one-time pause.
- Constant buffers and textures are unchanged (constants are small; textures were already copied into images).

## Result

The same evening street: G-buffer 15.5 → 1.1 ms, material pass 15.6 → 1.3 ms, our shadow pass 5.2 → 2.4 ms, the
frame's GPU time 39 → 12 ms. All four of the owner's slow places (`save_base_lag`, `lag2`, `lag3`, `lag4`) now run
at 29.9 frames/s (the game's 30 cap), 12 ms of GPU time each.

## What it means later

- The GPU has room now: higher resolution and the other enhancements have ~20 ms of headroom in these places.
- The same is likely true of any data the GPU reads repeatedly from the pools (large constant buffers, storage
  buffers): measure first with the per-pass timer.
- The arena's emptying is simple and pauses once; a finer eviction (least recently used ranges) can come if the
  pause shows.

## Addition, 2026-11-13: small ranges are also compared by content

The minimap went blank for one frame now and then while walking. The minimap watch (`runtime/src/gpu_watch.c`,
`python -m tools.dev.minimap`) showed the bad frames had the same draws, programs and textures as their neighbours,
and that in them 76 to 107 of the ~112 minimap draws took their vertices from an old copy (0, 39 and 8 in the frames
before): the image held the icon sheet drawn over the whole map.

The cause is in "copied again only when the CPU has written one of its pages since". Written pages are read once a
frame, at present, and the game writes a frame's UI quads after that. The game hands that vertex memory out as a
ring, so most addresses are new and get a first copy. When the ring comes round to an address that still has a copy,
its pages look clean for one more frame and the draw gets the quad that was there a lap ago: an icon drawn with the
map's full-size quad.

So a range of up to 1 KiB (a quad is 160 bytes) is now also compared by content: a 64-bit FNV-1a of it is kept with
the copy and checked before the copy is used (`geo_print`, `gpu.c`). A mismatch counts as a rewrite like any other,
so such a range is left in the pool after a few. Measured on the owner's late-game save: the case happened over
2000 times in 5000 frames of walking and the watch saw no blank frame (3 in a walk of the same length before);
frame times unchanged. `YW4_GEO_PRINT=0` turns the comparison off.

Larger ranges (models) still go by pages alone: the same gap exists for them in principle (a mesh loaded into
reused memory and drawn before the next present), not seen so far.
