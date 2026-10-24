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
   Then static cut-out (`4cbb8e`) and skinned casters (`71faac`; palettes in frame.md; with the fade program only
   while the fade is 1). Not ours yet: instanced casters with per-copy fade (`9904c2` + `b7cd6e`) and the
   wind-swayed plants (`fa9033`, `61ebac`: textures read in the vertex stage).

   **Changed 2026-10-23: replace the game's draws instead of choosing our own casters.** Hiding nodes and
   drawing what our gather selected went wrong in the owner's playtest: flickering streaks of shadow. Check mode
   showed why: about 40 submeshes per layer that we would draw and the game does not (it chooses per cascade and
   per frame by rules not decoded yet; an object 150 m away was in our list for the 20 m cascade). Learning a
   submesh once and then drawing it every frame anywhere can't match that. Now the game keeps its whole selection
   (walk, culling, sort) and the submesh-draw hook (`f_a75e90`) skips the game's draw of a submesh we can draw and
   adds ours to the pass: its state flush, NVN calls and their translation are what we save. The first draw of
   each layer stays with the game (its flush binds and clears the cascade's layer); each object goes back to the
   game one frame in 32 to be checked again. Owner's field save, every frame for 160 frames: the streaks are gone
   and the character shadows match the game's; 72% of the game's shadow draws are replaced.
   The full takeover (our own selection, no `f_a45890` at all) needs the game's selection rules first.

   **Changed 2026-10-23: instanced casters (repeated props) back to the game, for now.** With them ours, the owner
   saw bands of shadow for a frame or a few while the camera moved (never standing still, never with the game's
   shadows). Found by elimination, each a playtest with env switches: everything but instanced clean; instanced
   flash. Ours now take exactly the copies the game's callback kept (vertex cbuf 11 and the count at
   `device+0x45cc`, read at the submesh-draw hook) instead of our own culling of all copies. Measured and not
   the cause: those copies and the count against the game's draw (0 different of 40,000+, live play); each copy's
   final matrix, ours against cbuf 1 byte 128 × cbuf 11 copy × cbuf 2 byte 0 at the game's draw (0 of 40,000+,
   live, with the game also drawing); cull and front face (0 different); submesh draws with no GPU draw (0 of
   450,000); two layers sharing a pass buffer (one layer open at a time); copy count (2–4 copies flash, only the
   first copy drawn still flashes); a depth bias on our instanced draws only (still flashes, so not float rounding).
   With the game also drawing (`YW4_SHADOWS_BOTH=1`) it still flashes, so the bands come from our draws, while
   every value they take matches. Also equal (2026-10-24, check mode, 96,000 instanced draws): index count, base
   vertex (always 0), vertex stride, primitive (triangles) and index type. Nothing left to compare on the CPU
   side: the fault is in how our draws run on the GPU. Next: capture a flashing frame (RenderDoc) and find the
   draw that makes the band.
   `YW4_SHADOWS_NO_SKINNED` / `_NO_CUTOUT` / `_NO_INSTANCED` leave those kinds to the game for elimination.

   **Fixed 2026-10-24: the bands were nudged copies; instanced casters are ours again.** RenderDoc can't run the
   draw path (no `VK_EXT_external_memory_host` under it). Instead, a temporary detector (with
   `YW4_SHADOWS_BOTH=1`, so our draws only repeat the game's): the layer copied on the GPU before and after our
   pass, and after each draw in small passes, compared a few frames later. Our draws should change nothing; with
   instanced ours about 100 passes per walk replay changed 1,000+ texels, 0 without. Per draw, each band was one
   instanced draw writing far outside its own box, with copy matrices whose first value was ±100 or more: copies
   the callback nudged (`model+0x218`, flag × 100: fading). In check mode the game never draws a nudged copy with
   the instanced programs we mirror (0 of ~86,000 draws), while our learning, which sees the game's draw only one
   frame in 32, kept the submesh as ours. The per-copy fade program `9904c2` is the one that decodes a nudged
   value (scale `fract(|x|) × 100`, fade `floor(|x|) / 255`); which program, if any, the game uses for nudged
   copies in the shadow layers is not pinned down yet (none showed in the walk replay's check). Ours now leave
   any draw with a nudged pending copy to the game, every frame. Detector after the fix: 0 passes with 1,000+ changed texels (100–999: as few as with no
   instanced draws, edge and rounding noise). Owner's playtest: no flashes. Why the earlier checks missed it: they
   compared only draws the game made with the mirrored programs, which never carry nudged copies.

   **Fixed 2026-10-24: character shadows animated choppier than the characters.** Our skinned draws copied the
   palette from the submesh's own buffer (`palette_of`), which the animation is writing the next pose into: exactly
   the palette of the game's draw in only 45% of 70,000 check-mode draws. The snapshot the model renderer binds to
   vs cbuf 4 before the submesh draw (read at the hook like the copies in slot 11) was exactly the game's in all
   70,000; ours now copy that. Owner's playtest: smooth. (The frame-rate note above was this, not speed: on the
   walk replay our pass costs about the same CPU and GPU as the game's shadows.)
   **Changed 2026-10-24: instanced casters drawn instanced; fading copies ours.** Measured on a quiet machine (walk
   replay, 3 runs each, alternating), our pass with one draw per copy cost more than the game's shadows: game
   work +0.4 ms, GPU +1.1 ms a frame (about 207,000 of our draws where the game made 115,000; and every draw
   compared each pending copy with all the model's, up to 64 × 64). Now an instanced submesh is one draw, as the
   game's: each copy's projection × view × copy × world (the matrices verified before) and its fade go into a
   copies ring (`gpu_shadow_copies`, a storage buffer; `shadow_instanced.vert` / `_cutout.vert` read
   `copy[gl_InstanceIndex]`, the draw's first instance being its first copy), and only the first and last copy are
   checked to be the model's own. The fading programs are ours too (`9904c2` + `b7cd6e`, cut-out `f8951f` +
   `d5f536`: half of what the game still drew): a draw with any nudged copy decodes all of them as `9904c2` does,
   in the same float operations, and drops fragments below the 16×16 dither (fs slot 18; one texture, its handle's
   sampler part changes) in `shadow_fade.frag` / `_cutout.frag`; learned only while the material's own fade (fs cbuf
   3 byte 28) is 1. Texture handles are now traced for slots 0–19. Same measurement after: game work −0.6 ms (avg
   10.9 → 10.3, every pair), p95 −1.2 ms, GPU −0.3 ms; the game draws about 30,000 of its shadow draws per report
   window instead of 73,000. The before/after detector with the game also drawing: 0 passes with 1,000+ changed
   texels, 2 of ~7,000 with 100–999. Screenshots from the same save, game's against ours: differences only from
   animation timing.
   **Changed 2026-10-24: wind-swayed casters ours.** `fa9033` (instanced) and `61ebac` (not), both with the cut-out
   fragment program: the largest share the game still drew. The vertex program moves each vertex in world space
   (after copy x world) by a wind map (vs texture 12, placed by cbuf 6 row 47) giving a height band and bend, and
   two lookups of a wind noise (vs texture 11) scrolled by cbuf 6 row 36 (direction, strength, time), scaled by the
   material (cbuf 7 rows 0, 3) and the vertex's weights (attribute 1); texture coordinates are attribute 2.
   `shadow_wind.vert` does the same operations in the game's order. The constants are read at the submesh-draw
   hook from the bound slots, as the copies (slot 11) and palettes (slot 4): equal to the game's draw in all of
   50,000 checked; the two textures had one id each throughout (handles: the latest learned). A draw is one
   header entry (the four constant rows) plus its copies' copy x world rows in the copies ring. Before/after
   check with the game also drawing: passes with wind draws changed no more texels than the others (0 with
   1,000+, 5 with 100-999 of ~3,700); Vulkan validation clean. The game's remaining shadow draws per report
   window: about 18,500-26,000 (from ~30,000). Walk replay, 3 runs each: game work 9.5 ms against the game's
   shadows' 10.2 (p95 15.0 against 16.4), GPU and process CPU equal; one of the three pairs went the other way,
   so the gain is within run-to-run noise. `YW4_SHADOWS_NO_WIND=1` leaves them to the game.
   **Fixed 2026-10-24: shadows flickered in cutscenes (forest scene), never with the game's shadows.** In
   cutscenes a cascade's job list draws two shadow layers, into two shadow maps (the usual one and a second,
   bound before the second layer starts), and only then the cascade-end job. Ours ended the first layer when the
   second began and dropped its pass (the game's draws of it already skipped): about 70,000 of our draws lost per
   report window, the game's version showing only on the 1-in-32 re-check frames. Now the next layer of the same
   cascade queues the previous layer's pass; a pass draws where the game's draws in its layer went; and when the
   game has moved on to another target, the executor draws the pass into its own layer with the target and state
   (viewport, scissor, depth and polygon state) the game's first draw there used, then restores the game's.
   Restoring only the target was not enough: a few shadows stayed missing. Check mode in the cutscene: all 81,000
   draws per window equal to ours. Owner's playtests: no flicker, identical to the game's shadows.
5. Switch the default to `native` once every street, house and battle layer matches; measure CPU.
