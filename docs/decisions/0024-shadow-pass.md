# 0024 — The first replaced pass: shadow casters

Date: 2026-10-22
Status: accepted (owner: "write it up, do what you think is best")

## The question

Decision 0023 replaces the 3D world pass by pass, shadows first. [frame.md](../frame.md) now maps the shadow pass:
where the cascades come from, which objects are drawn, their geometry, matrices, bones and instance transforms, and
the five shader programs. How exactly do we take it over?

## What the game does today (per frame, render thread A)

For each of 3 cascades: a few setup jobs (target layer, viewport, matrices into the render state), occlusion-query
boxes for 154 objects, then the model renderer `f_a45890` on a **layer of kind 1**. `f_a45890` walks the shared
draw list, keeps the shadow casters, sorts them, and for each submesh sets state (program, buffers, textures,
uniforms built on the CPU) and draws through the engine's NVN wrapper layer. Our runtime records those NVN calls
and turns them into Vulkan at submit. The expensive part is `f_a45890` plus the two layers below it.

## Decision

1. **Hook the model renderer for shadow layers only.** A new kind of recompiler entry, a *hook*: listed `main`
   functions start with `if (native(ctx)) return;`, so native code runs first and can hand back to the
   recompiled body. `f_a45890` gets one. The hook takes over when the layer item's kind is 1 and
   `YW4_SHADOWS=native` (the default stays `game` until the pass matches). Everything around it (the cascade
   setup jobs, target binding, clears, the occlusion boxes) stays the game's for now: cheap, and it keeps the
   depth target, layer and viewport exactly as the game sets them.

   Why here and not higher up: the game's setup jobs already put the right cascade layer and matrices in place,
   so the hook only has to draw casters; and when it declines (see 5) the game's code runs unchanged.

2. **Gather on the render thread, draw at submit.** In the hook (render thread, the same moment the game reads the
   list) we walk the draw list with `f_a45890`'s own filters (layer key range and mask, visible `+0x1c8`, casts a
   shadow `+0x1d1`, not `+0x1b0`), and copy what the GPU needs into a **native pass record**: per submesh the
   vertex and index addresses (game memory, already imported into Vulkan), counts, stride, vertex layout, index
   type, the world matrix, and bones / instance transforms when used. The record goes into the thread's NVN
   command buffer (`[device+0x10d0] + 0xd8`) as a new command, `CMD_NATIVE`. Copies, because the game rewrites
   this data for the next frame before our submit runs (the same reason recorded NVN commands keep copies).

3. **The executor calls our pass at that point in the stream.** `gpu_execute` meets `CMD_NATIVE` between the game's
   own commands, so ordering with the rest of the frame and the shared depth image come for free: the target is
   already bound by the game's preceding commands. Our pass binds its own pipelines and per-draw data (a
   per-frame uniform ring of our own; push constants for small things) and draws.

4. **Our own shaders, mirroring the game's math.** Hand-written GLSL, one vertex shader per caster kind: static
   (`proj·view·world·v`), instanced (`proj·view·instance·world·v`), skinned (up to 4 bones). Depth-only fragment
   stage, plus an alpha cut-out variant. The cascade matrix comes from the renderer object (`main+0x56ab1b0`
   `+0x80 + i·0x40`); the cascade index from the render state set by the game's job `f_7ee9a0`. Same operations in
   the same order as the game's shaders, so depth should match to the last bits or close to it.

5. **Decline instead of guess.** If a layer contains anything the hook does not handle yet (a custom draw callback,
   an unknown vertex format or program, a cut-out material whose texture we cannot find), the hook returns 0 for
   that layer and the game draws it as before. A log line says why, once per reason. So the switch can be flipped
   early and coverage grows while the game stays correct.

6. **How it is checked.** Same frame, `YW4_SHADOWS=game` vs `native`: the three cascade layers dumped as raw depth
   (`YW4_DUMP_IMAGE` `.depthf`) and compared (max difference, count of differing texels), plus screenshots of the
   scene. Then CPU: per-thread busy time of render thread A before and after (perf log), and frame time in the
   busy street (`docs/baseline.md`).

## Alternatives

- **Replace the whole shadow block (`f_7ed570`'s jobs).** Saves the setup jobs and the occlusion boxes too, but we
  would also own target/layer binding and clears from day one, with nothing of the game's left to compare against
  inside the pass. Later step, once the casters match.
- **Keep `f_a45890` and only replace the layers below it (NVN wrapper, our translation).** Smaller win (about 60%
  of the thread's busy time is in those layers, but the walk and the CPU-built uniforms stay), and it changes code
  every pass shares. Not now.
- **Draw from our own copy of the scene, independent of the draw list.** Needs our own culling and sorting;
  nothing to gain until the pass matches.

## Open, to be found while building

- Where a material keeps its textures (for cut-out casters). `f_974cd0` binds them from the context object
  `[device+0x10d0]`, slots at `+0x780 + slot·16`; a write watch on such a slot will show who fills it. Until then
  cut-out casters make the hook decline.
- The vertex formats `0x22`, `0x12`, `0x2f`, `0x2c` in Vulkan terms (the existing `vertex_format` table) and the
  index type bit in the submesh record.
- How the game selects the program per submesh (static / instanced / skinned / cut-out), so the hook picks the
  matching shader.

## Steps

1. **Done:** recompiler hooks (`[[hook]]` in `recomp/overrides.toml`, by `main` offset) and a `CMD_NATIVE` command; a
   hook on `f_a45890` (`runtime/src/shadow.c`) that only counts kind-1 layers and declines. Measured: 3 shadow
   layers per frame (one per cascade), each walking the whole draw list (~185 nodes); game unchanged, tests pass.
2. **Done:** gather (`shadow.c`), from Ghidra's decompile of `f_a45890`: node, object, geometry and submesh filters,
   plus our own culling of each object's box against the cascade (side planes only: casters in front of the
   cascade still cast into it). `YW4_SHADOWS=check` compares with the game's draws: 0 missed. Instanced objects are
   not culled yet (identity world matrix, each copy placed by its own transform), so we select about 2.5× the
   game's submeshes: same image, more GPU work, to fix with per-copy culling.
3. **Working, being validated:** static casters drawn by our pass (`runtime/src/gpu_shadow.c`, shader
   `runtime/shaders/shadow_static.vert`, compiled by `recomp/build.py`). Built **hybrid** rather than as
   planned in 1-3 above: the engine binds and clears each cascade layer lazily inside `f_a45890` (2 clears per
   layer), so instead of skipping it, the hook hides the nodes we draw (their kind-1 bit at node `+0x20`), the
   game draws the rest and binds/clears the layer, and our pass is queued after it from a hook on the cascade's
   end job `f_7ed350`; the executor checks the bound target is the cascade's layer. Which submeshes are ours is
   **learned from the game's own draws**: drawn with the static programs (`b320bd` + `6ead6a`), our vertex and
   index addresses equal to the game's, our matrix equal to the one in cbuf 2 (verified per draw), and its
   culling recorded. Nodes go back to the game one frame in 32 to re-check. In the street: about 15 static
   submeshes per cascade layer drawn by us, the game the other ~70; 0 wrong vertex or index addresses over
   51,000 compared draws. Depth images against the game's: differences at the level of run-to-run noise
   (moving characters, cars), checked per run against a second run of the game's own shadows.
   Open: a few submeshes (about one per cascade layer, objects not identified yet) are drawn by the game with an
   extra per-submesh offset on top of the object's world matrix (a node transform, location unknown): their
   matrix doesn't match ours, so they stay with the game until node transforms are applied.
4. **Instanced casters: done** (vertex program `a74842` + `6ead6a`). The model renderer gets an instanced model's
   copies from the model's callback `main+0x5f9420` (model `+0xe0`): it culls the copies against the view (the
   object's box, which is one copy's, through each copy's transform; then an occlusion test) and writes the kept
   ones into a new buffer every frame, bound as vertex cbuf 11. We read all copies instead: `model+0x214` sets
   (0 = 1) of `[model+0xd0]` (0x38 bytes: first copy `+0x30`, count `+0x32`), transforms at `[model+0x168]`
   (3×4, 48 bytes), a byte per copy at `[model+0x170]` (with `model+0x218` set, the callback adds byte × 100 to
   a copy's first value when it is below 100; we do the same). Each copy is its own draw with
   projection × view × copy × world, culled by the side planes only (looser than the game's). Learned like the
   static ones; the check also needs every copy the game drew to be one of ours. Street, `YW4_SHADOWS=check`:
   0 of ~75,000 instanced draws with a copy we don't have, 0 drawn copies that our culling drops; the
   screenshot against the game's shadows differs only by run-to-run noise (animation timing). Not handled
   (the game keeps them): split copies (`model+0x216`), more than 8 sets.
   **Instanced cut-out casters: done** (`744205` + `dceb89`, the biggest remaining share): the same copies and
   matrices, plus the texture coordinates (attribute 1, from the same vertex buffer), the texture (fragment slot 0,
   by its handle) and the material's alpha threshold (vertex cbuf 3 byte 12), all learned from the game's draw;
   our `shadow_cutout.vert/.frag` drop texels below the threshold as `dceb89` does. Textures are made and uploaded
   before our pass begins rendering. Street: screenshots against the game's shadows differ less than two runs of
   the game differ from each other. The game now draws about 30% of the shadow submeshes (was 85%); CPU unchanged
   so far (the game's model renderer still runs for these layers).
   Not ours yet: skinned casters (all cut-out; their bone palettes are built per submesh each frame, source not
   found, see frame.md) and the wind-swayed plants (`fa9033`, `61ebac`: textures read in the vertex stage).
5. Switch the default to `native` once every street, house and battle layer matches; measure CPU.
