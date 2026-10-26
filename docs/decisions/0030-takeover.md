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

**Textures: decoded (`material_textures` in `gbuffer.c`, counted by `YW4_GBUFFER=check`).** A material's texture
reference (6 bytes in the draw item's `+0x28` table; byte 0 = entry of the model record's texture table `+0xf8`)
gives a texture resource (0x48 bytes) and an index. The resource's `+0x18` is a Level-5 **G4TX** texture container:
byte `+0x20` images, each with a 0x18-byte record at `hdr + 4×u16(hdr+0xa) + 4×u16(hdr+0x3e) + 0x18×index`
whose first word + 0x18 is the NVN texture the engine made for it (the engine's textures of one container sit in
one array, 0x140 bytes apart, with consecutive texture pool ids). An index past the images is a region of an
atlas: its record (`+0x42` table) starts with the image it lies in, then the region's rectangle.

**Material texture *k* goes to fragment slot *k*.** The engine binds its slot *s* to shader binding *s* (the
program's per-stage table of (slot, binding) pairs: program `[device+0x10d0]+0x2f8`, stage objects
`+0xc8/+0xd0/+0xd8`, table `+0xb8`, count `+0xc0`); the same table lists exactly the slots the shader reads, so the
check compares only those. The shader headers name the bindings: `in_tex0` 0, `in_texNrm` 1, ...; the two-layer
ground material, for example, lists colour, normal, colour 2, normal 2, height, in slots 0-4.

How it was found: first a search for the bound texture inside the resource, which failed, then a probe that only
recorded *where* each bound texture lies relative to each reference, run over four saves (schoolyard, the town walk
replay, the late-game forest, the evening town). One off-by-one in the record index had made the slot order look
scrambled; with it fixed every program followed slot = *k*.

Check mode, four saves, all four kinds of layer (G-buffer, material pass, stage layers, shadows): **all 5.7 million
texture slots read equal ours**, none wrong, none left undecoded (the engine's other table, model record `+0xf0`
when the geometry's `+0x108` is set, would show as a difference: not met).

Still open for textures: the atlas region's rectangle becomes a UV transform in the constants (decoded from the same
record; needed once we stop taking constants from the game), and the samplers (each binding's handle also carries a
sampler).

## Alternatives

- **Selection first.** Removes the walk, but needs the rules that the shadow streaks showed we don't have, and
  every draw's data still comes from the game: nothing to skip yet.
- **Leave the hybrid.** Fine for pictures, but the engine keeps doing every draw's state work and our renderer can't
  draw anything the game didn't set up (no resolution scaling of materials, no new passes).
