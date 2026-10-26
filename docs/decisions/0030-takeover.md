# 0030 — The takeover: our renderer does the per-draw work, then the selection

Date: 2026-10-26
Status: accepted (owner: "Take over", then "yes go" to the order below)

## The question

Decisions 0024 and 0026 replaced the game's *draws* in the shadow, G-buffer, material and stage passes: the game
still walks its draw lists, sorts them, binds every material's constants and textures and commits its state, and
only then do we draw in place of its draw call. 0026 put off the complete takeover until the hybrid was proven.
It is proven now (same-frame A/B: 0 texels differ). What do we take over first?

## What we measured

Profile of the threads that submit GPU work, owner's evening town save (`save_base_lag`), 60,000 samples:
81% waiting, ~14% game code. Of that game code, what the takeover removes:

- ~3.5% the model renderer `f_a45890` itself (walk, cull, sort; per-draw state decisions),
- ~6% the engine's per-draw state layer (`f_974cd0`, `f_97aff0`, `f_965eb0`, `f_976ab0`, `f_977e30`, ...),
- ~0.9% the prop-copy callback `f_5f9420`.

The render threads wait most of the time, so frames per second may not move much; what the takeover buys is a
renderer that owns every draw (resolution scaling and the later improvements need it) and less CPU.

## The decision

Two steps, in this order:

1. **Per-draw work first.** The game keeps choosing and sorting what to draw (its selection rules are not decoded;
   our own selection is what made the shadow streaks of 0024). Everything it does per draw becomes ours: textures
   decoded from the model's own data instead of the views the engine makes each draw, render state, which program a
   material uses, material constants (already decoded, 0026), bones. Each piece gets a check mode that compares our
   value with what the game binds, on every draw, before anything is drawn from it. When all of a draw's inputs are
   ours, the game's state work for that draw is skipped.
2. **Selection second**: our own walk, culling and sort, once every draw's data is ours.

## Progress

**Textures (in progress, `YW4_PROBE_TEXTURES=<frame>`, temporary probe in `gbuffer.c`).** A material's texture
reference (6 bytes in the draw item's `+0x28` table; byte 0 = entry of the model record's texture table `+0xf8`)
gives a texture resource (0x48 bytes) and an index. The resource's `+0x18` is a Level-5 **G4TX** texture container.
Its per-texture records are 0x18 bytes at `hdr + 4×u16(hdr+0xa) + 4×u16(hdr+0x3e) + 0x18×index` (two pointers and a
zero); the engine's NVN textures of one container sit in one array, 0x140 bytes apart, with consecutive texture pool
ids. Measured (schoolyard, 254,000 references): the bound texture equals `[record(index+1)] + 0x18` for every
reference whose texture is found bound; no other reading matched.

The engine binds its texture slot *s* to shader binding *s* (the program's table at `+0xb8`, count `+0xc0`, of the
per-stage shader objects at program `+0xc8/+0xd0/+0xd8`; program = `[device+0x10d0]+0x2f8`: identity in every
program seen). The shader headers name each binding (`in_tex0` 0, `in_texNrm` 1, `in_texOccl` 2, ...).

Open: which slot a material's reference *k* goes to. Reference order is not the slot order (`5eb3b2`'s second
reference is found in slots 0, 2 or 4 by material), and the colour texture in slot 0 is, for `5eb3b2`, always
0x280 bytes (two array entries) before what the rule above gives for the second reference. Not yet decoded:
`f_a45890` stores the descriptors per reference into three arrays (`device+0x3808/0x3b08/0x3e08`, 0x18 each) and a
pending list (`device+0x1d78`), read somewhere in the commit `f_977e30` and its helpers (not in `f_975540`,
`f_9759d0`, `f_965880`, `f_965eb0`, `f_966120` by offset search).

## Alternatives

- **Selection first.** Removes the walk, but needs the rules that the shadow streaks showed we don't have, and
  every draw's data still comes from the game: nothing to skip yet.
- **Leave the hybrid.** Fine for pictures, but the engine keeps doing every draw's state work and our renderer can't
  draw anything the game didn't set up (no resolution scaling of materials, no new passes).
