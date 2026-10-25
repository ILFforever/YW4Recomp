# 0025 — Soft contact shadows (PCSS) in the shadow mask shader

Date: 2026-10-25
Status: accepted (owner picked the approach and the defaults: "put it in the shadow mask shader only"; "4096 and x2
is best")

## The question

With shadow maps at 2048 or 4096 (F8), hard objects (poles, characters) get clean edges but tree canopies turn into a
speckled field of dots; the game's 1024 blur hides that but softens everything. We want both: sharp where a shadow
touches its caster, soft where the caster is far above the ground. That is contact-hardening shadows, PCSS
(percentage-closer soft shadows): find the casters around a point, and blur by how far they are from it.

Where do we add it, and how do we keep it from costing load time?

## What we measured

**The obvious place was too expensive to compile.** The game has ~200 translated shaders that sample shadow maps,
each doing 5 lookups per cascade, up to 27 lookups in one shader. A helper at every lookup made the house take 40-67 s
to load, **even with PCSS switched off**: the driver copies the helper into every lookup and builds pipelines when a
scene first draws them. A tool that times a cold pipeline build per shader (a random string added to the SPIR-V so
the driver's cache can't answer) showed:

| One shader with 27 lookups | Cold pipeline build |
|---|---|
| The game's lookups | 73 ms |
| Helper, 16-tap blocker search + 16-tap filter | 24,600 ms |

Two causes:

1. **`textureGrad(.., 0, 0)` on an array shadow map** compiles about 10x slower on NVIDIA than `texture()`. The
   translator used it to avoid derivatives in branches; the shadow maps have one level, so `texture()` reads the
   same texels.
2. **Any real work per lookup multiplies.** Even a loop-free version (4 gathers + 4 compare taps) stayed 4-25x the
   game's build time; telling the compiler not to unroll changed little.

**Only a few of those shaders matter.** In 80 traced frames only 7 of the ~200 ever draw. One is the game's
**screen-space shadow mask**: a full-screen pass at half resolution (800x450) that shadows everything drawn into the
G-buffer. The other 6 shadow forward-drawn objects.

## Decision

1. **PCSS goes only into the shadow mask shader** (`PCSS_SHADERS` in `recomp/shader.py`, one key today). All other
   shadow shaders keep the game's own lookups and compile as before. Its lookups call `yw4_shadow`: a 16-tap blocker
   search on the raw depth, then a 16-tap filter whose radius is the average caster-to-receiver depth gap times a
   per-cascade scale, clamped between half a texel and 6 texels of a 1024 map (map coordinates, so the size is the
   same at every F8 setting). The game averages 5 lookups per cascade; the first does the work and the other 4 reuse
   it. The compare taps use `texture()` (cause 1).
2. **The raw depth is a second binding of the same image** (`d <slot>` in the shader interface, binding 96 + slot,
   nearest filtering, no compare), and the settings are a small uniform buffer (`p 0`, binding 95) written per draw.
3. **Casters are texels with a smaller depth than the receiver.** The maps are cleared to 1.0. The game samples them
   with a GREATER compare; that only means a result of 1 is "in shadow", not that depth runs the other way (reading
   it that way first made every empty texel a caster and blurred everything).
4. **The mask pipeline is built at start-up** on a background thread with the game's exact pipeline state (it is
   the same in every traced frame). It takes ~0.2-0.3 s cold, done before the first frame; the driver's disk cache
   keeps it, so the game's own build of it later is immediate. Warming it with a stand-in vertex shader did not
   work: NVIDIA's cache depends on the vertex shader and the blend state.
5. **Defaults: 4096 maps with PCSS x2**, the owner's pick after testing the bamboo forest. F8 and F7 switch while
   playing; `YW4_SHADOW_RES=1024 YW4_PCSS=0` gives the game's original shadows. The scale per cascade
   (0.01, 0.005, 0.0025) was set from measured depth gaps: the near cascade covers a thin depth range, so a character
   standing on the ground is a gap of ~0.3-0.5.

## Consequences

- Load times are unchanged; the one extra pipeline is built before the game shows anything.
- Objects the game draws forward keep the game's filter (known-issues.md). Adding PCSS to one of those 6 shaders is
  possible one at a time, each with a start-up build like the mask's.
- Any slow pipeline build is now logged (`pipeline took N ms`), so a shader that stalls a scene shows up at once.
- Cold starts still build every *other* pipeline when first used (~60-75 ms each), as before this change. Shipping a
  list of pipeline states and building them all at start-up would remove that too; not done yet.
