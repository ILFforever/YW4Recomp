# 0028 — The game's geometry in video memory

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
