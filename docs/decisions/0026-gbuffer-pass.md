# 0026 — The second replaced pass: the G-buffer, hybrid first

Date: 2026-10-25
Status: accepted (owner: "ok go for it", after choosing a hybrid first and the complete takeover later)

## The question

[frame.md](../frame.md) now maps the main view: the scene geometry is drawn twice (a light pre-pass renderer), first
into the **G-buffer** (layer kind 2), then again with full materials (kind 0, keys 0..2), both by the model renderer
`f_a45890` and its submesh draw `f_a75e90`, the same functions the shadow pass ([0024](0024-shadow-pass.md))
hooks. Materials are mapped too: a submesh's material record, the model's texture table, temporary texture views
the engine makes each draw. Which pass do we replace first, and how far do we go at once?

## What the G-buffer is

One layer on render thread B, ~200 submesh draws in the schoolyard, into 1600×900 with three colour targets and the
scene depth:

| Target | Format | The main program writes |
|---|---|---|
| 0 | 0x11 | screen-space motion (this frame's position against last frame's) and depth |
| 1 | 0x25 | zero for most materials |
| 2 | 0x25 | the surface normal (normal map turned by the vertex's tangent frame), and a lighting term in `.w` |

16 programs in that frame. One pair is half the draws: vertex `bc358b` + fragment `a93492` (97 of 195); `bc358b`
with four other fragment programs brings it to two thirds. `bc358b`'s inputs: per-object `cb2` (world in rows 0-2,
this frame's world-view-projection in rows 8-11, **last frame's** in rows 12-15), the view's `cb1`, the material's
`cb3` (UV transform, colour); `a93492` adds the material's `cb7`, the scene's `cb8` and the normal map (slot 1).

## Decision

1. **The G-buffer first, as a hybrid.** The game keeps choosing what to draw; our renderer draws the submeshes it
   can, and the game draws the rest. Each step shows a result and the game stays playable. A **complete takeover**
   (our own selection, materials decoded instead of borrowed) comes after the hybrid is proven, starting with the
   shadow pass (owner: later is preferable). Everything the hybrid builds is part of the takeover.
2. **Hook as the shadow pass does**: the model renderer hook notes a kind-2 layer; the submesh-draw hook decides per
   submesh and, when ours, skips the game's draw. Shared code moves out of `shadow.c` where both need it (the
   selection walk, the object and geometry decoding).
3. **Our draw goes exactly where the game's would** (a `CMD_NATIVE` record per submesh, at first). The G-buffer's
   targets, clears and the state around it stay the game's; our record draws into whatever is bound. Batching
   records comes once it works.
4. **Our own shaders**, one program family at a time, starting with `bc358b` + `a93492`. Written as readable GLSL
   that does what the translated program does, so they can be changed later (resolution, new effects).
5. **Data**: geometry, world matrix, bones and instance copies from the decoded objects (as for shadows). For the
   first stage, **material constants and textures come from the game's bound state** at the hook (the engine has
   resolved them by then: frame.md "Materials"); our matrices are computed by us and checked against `cb2`.
6. **Checking before it is visible**: `YW4_GBUFFER=check` compares our inputs with what the game bound, per draw
   (matrices, buffers, textures), as the shadow pass's check mode does. Then a GPU detector (copy the targets before
   and after our draw, count changed texels) and screenshot differences.
7. **Switches**: `YW4_GBUFFER=game|native|check` (default `game` until it matches), F6 toggles while playing.

## Order

1. Shared hook plumbing for kind-2 layers; check mode for `bc358b` draws (inputs only, nothing drawn).
2. Draw `bc358b` + `a93492` (static meshes); detector and screenshots must show no difference.
3. The other `bc358b` fragment programs, then the remaining static programs.
4. Skinned, instanced and cut-out programs, reusing the shadow pass's decoding.
5. Measure; then plan the takeover (selection first, in the shadow pass).

## Alternatives

- **Complete takeover now.** Needs the game's selection rules (our own selection is what made the shadow streaks in
  0024), every material field decoded and both geometry passes at once, with nothing to compare against until all of
  it works. Rejected for now, not forever: it is step 5.
- **The material pass first.** Same objects, more programs (more varied materials, reads the light buffers):
  harder, and it needs the G-buffer's output to be right anyway.
- **Reuse the game's translated programs with our own draws.** Fastest to get pixels, but it ties our pass to the
  game's constant buffer layouts and gains nothing over drawing through the game; readable shaders of our own are what
  the later improvements need.

## Progress

- **Step 1 (check):** buffers, matrices and last frame's camera match the game on every main-program draw over an
  hour of play (4.9 M draws), once the world matrix comes from the renderer's per-draw scratch record
  (`[sp+0x30]+0x40` at the submesh-draw hook) instead of the object: the object's own matrix is wrong for posed parts
  and for moving objects (the renderer draws a copy taken earlier). Index buffers: 99.88% (to identify).
- **Step 2a (our programs):** `gbuffer_main.vert/.frag` replace `bc358b` / `a93492` in the game's own draws (same
  interface, the game's state and buffers), switched by F6. Within one run, alternating frames
  (`YW4_GBUFFER_ALTERNATE`) and dumping the G-buffer targets, our output is bit-identical to the game's on every pixel
  that did not move between frames. This step went first so that any later difference comes from our data, not our
  shaders.
- **Step 2b (our draws):** `YW4_GBUFFER=native` now records our own draw in place of the game's `DrawElements` for
  every `bc358b` + `a93492` submesh (all 97 a frame in the schoolyard): vertex and index buffers from the submesh
  record, vertex constants 2 computed by us (`=programs` keeps step 2a alone). Material constants, textures, vertex
  formats and targets are still what the game bound. The constants are **bit-identical** to the game's on every draw
  checked once they are computed in the game's order, projection × (view × world), each product summed in order with
  no fused multiply-adds: (projection × view) × world, or fused sums, were off by one last bit on 2-60% of draws
  (found by dumping the inputs of mismatching draws and trying the orders offline). The model renderer spreads one
  vertex over five vertex streams bound to the same buffer, so our buffer goes into each. In one run with alternating
  frames, ours against the game's differs no more than the game's against itself. A draw whose buffers, count or
  index type are not the ones the game was about to draw stays the game's; the 0.12% index buffer mismatches of the
  hour-long check did not occur in the test saves, so check mode now logs the first ones in detail (both addresses,
  the submesh and buffer records) for when they do.
- **Step 3/4 (the other programs, skinned included):** 13 G-buffer vertex programs are ours now, 99.5-99.9% of
  the G-buffer's draws in the schoolyard, the town and the late-game forest; 1e14f9 (an extra row) and a few rare ones
  stay the game's. A table in `gbuffer.c` says where each program reads the object in vertex constants 2: world
  (3 or 4 rows), view × world, projection × view × world, at which rows. The skinned programs keep view × world
  where the others keep the world. Our draw changes only what the vertex program reads, so any fragment program
  behind these goes with it. Check mode compares each program's parts with the game's: bit-identical on all of
  400,000+ draws in the town. Found on the way:
  - The **index buffer** is the geometry's own index buffer object (+0x28), as the submesh draw reads it. It is
    usually, not always, the second object of the vertex buffer's record (+0x68 of it), which step 1 used. The
    shadow pass used the same wrong source; fixed there too (its learning had kept those draws with the game).
  - The step 1 **0.12% index buffer mismatches** were submeshes without indices: the game draws their vertices with
    `DrawArrays`. Ours does the same.
  - A sum that starts from 0 turns the game's −0 into +0; ours starts from the first product, as the game's.
  - The skinned programs' **last frame's view × world** is this frame's on most draws, but on 0.5-30% (by area) one
    of a few fixed matrices that are the same in every area, not the object's history. It is copied from the
    game's bound constants, as last frame's bones are; everything else in constants 2 is computed by us.
- Bones (constants 4 and 5), material constants and textures are still the game's bound state: their decoding is
  the takeover's work (step 5).
