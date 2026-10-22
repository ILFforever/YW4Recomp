# 0023 — Plan for the custom renderer (Phase 4b/4c)

Date: 2026-10-22
Status: accepted (owner: replace the 3D world; "UI and menu can stay on the old renderer" for now)

## The question

The NVN → Vulkan layer (0021, Phase 4a) now draws the house, town, battles and cutscenes like Eden. The project's
main goal is our own renderer, for two reasons the owner gave:

1. **Performance:** beat Eden on CPU and stutter. Ours already uses about half Eden's CPU (docs/baseline.md), but in
   busy scenes the frame is set by the main thread plus one render thread, and ~70% of that render thread is the
   game's own code building NVN command buffers.
2. **Enhancements:** higher resolution, widescreen, better shadows, 60 fps.

What do we replace, in what order, and where do we hook it?

## Measured: what one town frame draws

From an F11 log (work/logs/yw4_20261022_115926.log, street, daytime; the log stops at 450 draws, so the last
passes are cut off). Passes = consecutive draws with the same render targets:

| # | Target | Draws | What it probably is |
|---|---|---|---|
| 1 | 240×135, depth only, 36-vertex boxes | ~45 | occlusion-query boxes |
| 2 | 1024×1024 depth, 3 times | 54 / 20 / 80 | shadow cascades |
| 3 | 128×128, 1 colour | 3 | ambient cube map update (see a9c98f1) |
| 4 | 1600×900, 3 colour + depth | 64 | G-buffer (opaque geometry) |
| 5 | 800×450 and 533×300 | ~10 | half-res: screen-space shadow mask, SSAO-like, lamp light buffer |
| 6 | 1600×900, 1 colour + depth | 70 | lighting resolve + forward/transparent geometry |
| 7 | 1600×900 / 800×450 / 533×300 mix | ~30 | effects, fog, particles |
| 8 | 400×225 → 200×112 → 120×67 | 6 | bloom chain |
| 9 | 1600×900 no depth, 340×304, 516×460 | ~45 | UI, minimap |
| 10 | 1920×1080 | 1 | final copy into the window texture (cropped from 1600×900 = dynamic resolution) |

A classic deferred renderer: ~600-1500 draws a frame, about 40 passes, most of them full-screen quads. The heavy
CPU and GPU work is in passes 2, 4 and 6 (meshes).

## Decision

1. **Scope: the 3D world.** We replace shadows, the G-buffer, lighting, forward geometry and post-processing with
   our own Vulkan code that reads the engine's data (camera, meshes, materials, lights) instead of the game's NVN
   commands. **UI, menus, minimap, movies and particle effects stay on the NVN → Vulkan layer** for now: little
   to gain, many shaders. This can be revisited later.
2. **Hook at the engine level, one pass at a time.** For each pass: find the engine function that records it, call
   our renderer there instead, skip the game's command building. Our passes and the old path share one Vulkan
   device and draw into the same images within a frame.
3. **Every replaced pass must match the old output** (screenshot diff, `YW4_CAPTURE_FS`) and can be switched back
   to the old path with an env var. The goal of the replacement is the same picture, faster.
5. **Enhancements come last, as mods** (owner: "we will look at improvements at the end with mods"): better
   shadows, AA, resolution, widescreen and 60 fps are not part of replacing passes. Once the 3D world is ours, they
   are added as optional mods on top.
4. **No separate clean-up of the NVN layer.** Its resolution-scaling and occlusion-query gaps go away with the passes
   that replace them. The one piece we need is **image tracking** (who writes which image, the right barriers and
   layouts between our passes and the old path); it is built when the first replaced pass needs it.

## Order

1. **Map the frame** (Phase 4b), shadows first: name each pass by the engine function that records it (call stacks
   from the draw wrappers in work/recon/nvn.md; yw::CMainComponent slots 11/12; lives::CDeviceUnitNX), and find
   the data it reads. Output: `docs/frame.md`. No game code changes.
2. **Replace the shadow cascades** (pass 2), plus image tracking.
3. **Replace the G-buffer and forward geometry** (passes 4 and 6), with lighting (5, 6). Biggest CPU win, biggest job
   (materials, skinning, many mesh shaders).
4. **Replace post-processing** (passes 8, 10).
5. **Mods, at the end:** better shadows, AA, render resolution, widescreen (projection aspect + UI layout width),
   60 fps (game logic step, not only rendering).

## Alternatives

- **Clean up the NVN layer first** (barriers, pipeline cache, resolution scaling, occlusion queries). Gives quick
  wins, but most of it is thrown away once the 3D passes are ours. Rejected by the owner.
- **Replace the whole renderer at once.** Nothing to compare against until it all works. Rejected: one pass at a
  time keeps the game playable and checkable.
- **Also replace UI and menus.** Little speed or visual gain for hundreds of shaders. Left for later.
