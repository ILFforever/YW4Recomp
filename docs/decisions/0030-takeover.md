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
record; needed once we stop taking constants from the game).

**Samplers: decoded (`sampler_state`, `sampler_desc` in `gbuffer.c`).** The engine makes a new NVN sampler id for
most draws, so the id means nothing; the settings do. A first probe that looked for the settings in the reference,
texture and UV bytes found no single byte that decides them. The engine's sampler builder (`f_976ab0`, the only
caller of `nvnSamplerBuilderSetWrapMode`) showed the path instead:

- Per texture slot the engine keeps a **15-byte sampler state** (`device+0x1120 + 0xf×slot`). Each layer starts it
  from defaults (`device+0x1300 + 0x1e0×level + 0xf×slot`, level = byte `device+0x4631`).
- A texture reference's byte 1 counts `(field, value)` byte pairs, starting at its u16 `+4`, in the geometry's header
  blob at `4×(u16 +0xa + u16 +0x6e)`; each sets one byte of the slot's state (first 12 references only).
- `f_976ab0` turns the 15 bytes into NVN settings: bytes 1-3 wrap s/t/r; 4 and 5 minification and magnification
  (low 4 bits; bit 4: anisotropic; bit 5: linear between mipmaps); 6 anisotropy; 7, 8 level-of-detail clamp (/255
  ×15); 9 level-of-detail bias (signed); 0xa depth compare function (0: off); 0xb-0xe border colour (/255). The
  device's texture quality settings (`+0x4620` mode, `+0x4624` anisotropy, `+0x4625` on) can raise the filter and
  the anisotropy.

Check mode, four saves, all layer kinds: **all 6.1 million samplers** equal the engine's state byte for byte, and our
settings equal the bound sampler's field for field. The layer defaults are still read from the engine's state (set
once per layer, not per draw).

**Render state: decoded as a block (`render_state`, `effect_id`, `effect_pass`, `draw_state` in `gbuffer.c`).** The
engine keeps the draw's fixed-function state in a 33-byte block (`device+0xeb9`), one byte per field, and turns
changed fields into NVN state at the commit through a table of handlers (`main+0x14d23b0`, one per field; e.g.
`f_979a90` for fields 4-7 and 9: depth test, depth write, depth compare function, stencil). A draw's block is:

1. **The layer's block**, after the layer's own setup (the model renderer sets some fields at its start, and the
   shadow and material layers push one of their own): the newest of the engine's saved copies
   (`device+0xefb + 0x21×(level-1)`, level = byte `device+0x4630`) at the layer's first submesh.
2. **The material's pairs**: `(field, value)` bytes, material record `+4` start and `+0xb` count, in the geometry
   header blob at `4×(u16 +0xa + u16 +0x6c)`. Setting field 20 without 21 and 22 while field 0x14 is 2 also sets
   0x15 = 1 and 0x16 = 2. In G-buffer and shadow layers they count only if they leave field 5 (depth write) set,
   and then field 0x10 is cleared. The engine restores the block after each draw.
3. **The shader pass's pairs.** The material names a **shader effect** (id: per-object overrides in the model
   record's `+0xe8` table, else the model record's `+0x110` table at the submesh's byte `+0x42`). The commit
   (`f_977e30`) picks the effect's pass whose key is the layer's pass key (`device+0x45d4`) and applies that pass's
   own pairs. This is where the material pass gets depth compare "equal" for alpha-tested materials (found with a
   write watch on the field: the only other write was the layer setup). The same pass record also picks the
   programs (next).
4. **The device's settings** at the commit (`f_9759d0`): `+0x4629` flips the cull face, `+0x462a` and `+0x462b`
   clear fields 5 and 0x10, `+0x462d` raises field 0x1c.

Check mode, four saves, all layer kinds, ~4.9 million draws: **every draw's block equals the engine's**. Still from
the engine: the conversion of the block to NVN state (the handlers; we read the NVN state the game binds).

**Effect and program: decoded (`effect_of`, `pass_shaders`).** The effect manager (`main+0x58656e8`) is a set of
plain arrays, read through small virtual methods (vtable `main+0x14deba8`):

- effect by id (from 1): `[mgr+0x10] + 0x28×(id-1)`, the effect object first (`f_a9edf0`);
- the pass record (above) names the stage shader objects: u16 `+4` the vertex shader in `[mgr+0x30]` (0x20 bytes
  each), u16 `+6` the fragment shader in `[mgr+0x78]` (0x10 bytes), u16 `+8` a third in `[mgr+0xc0]`
  (`f_a9ee60`, `f_a9eec0`, `f_a9ee90`);
- a stage object's first word is its shader code, the address our runtime names its translated shader by;
- the engine joins the stages into one NVN program (`f_965880`, cached by the stages' ids); the engine's program
  object *is* that NVN program.

Check mode, four saves: on every draw (about 4.6 million) our effect is the engine's and our vertex and fragment
code are the bound program's. So a material, through its effect and the layer's pass key, names its programs,
its render state, its textures and samplers, all from the game's data.

**Bones: decoded (`bone_palette`).** A skinned submesh's record (model record `+0xc8`, 0x140 bytes) holds three
palettes, 0x38 bytes apart from `+0x70`, which the animation writes in turn; `+0x118` points at the newest, and that
is the one the engine copies into vertex constants 4. The shadow pass had always read the first one (right a third
of the time, hence its copy of the engine's bound constants). A probe first suggested a race (the game "one step
behind" in 12% of draws); it was the probe: a character standing still has the same pose in two buffers, and the
probe named the last match. Check mode, four saves: **every skinned draw** (~390,000) equals ours.

Everything one draw needs from its material is now decoded and checked in four saves, every layer kind:
constants 2 (matrices), material constants, textures, samplers, render state block, effect, programs, bones.
Still the game's: last frame's matrices and bones (motion vectors), the scene's constant buffers (camera,
lights; per layer), vertex attribute formats, the atlas regions' UV transforms. Next: skip the game's per-draw
setup for draws whose inputs are all ours.

**The block as NVN state: decoded (`nvn_state`).** At the commit the engine runs one handler per changed field
(table `main+0x14d23b0`), which writes NVN state objects in its state (`[device+0x10d0]+0x240..0x2f2`): blend
states per colour target, channel mask, colour state, depth stencil, multisample, polygon, plus the stencil reference
and mask and the depth bias as plain fields. Which bytes feed which call is listed at `nvn_state`. We build the same
objects in our own packing (`nvn_objects.c`), starting each layer from the engine's objects for what the block doesn't
drive (the other colour targets' blend states, the colour state's logic op). Check mode, four saves, all layer kinds,
~4.4 million draws: **our objects equal the engine's byte for byte on every draw**. The check was tested by switching
the conversion off: then nearly every draw differs, in every part but the two flags.

**Vertex layout: decoded (`vertex_layout`).** It belongs to the effect, not the geometry (`f_965070`): the effect's
handle in the manager's array lists vertex elements (`+0x10`, u16 `+0x22` of them, 8 bytes: semantic, offset,
stream, NVN format at `+4`), and the pass's program entry (handle `+8`, 0x22 bytes by the pass record's byte `+0xd`)
maps each semantic to the shader's attribute. Each attribute of stream 0 gets a stream of its own (the attribute's
number) over the submesh's vertices with its stride: the five streams on one buffer seen in 0026. Check mode, four
saves: attribute states and strides equal the bound ones on every draw (~4.4 million); no draw had elements on
another stream. Self-test (every offset +4): no draw matches. (A lesson from that self-test: a run that fails to
start leaves the previous log as the newest; the runs now check that a new log appeared.)

**Last frame's matrices and bones (motion vectors).** Constants 2 are now ours in every row:

- last frame's world (rows 12-15) is this frame's world on every draw checked, walking characters too (they move
  through their bones); its bottom row is copied from the record (the game has -0 there on some objects, a last-bit
  difference otherwise);
- last frame's view × world (rows 16-19) is the renderer's record `+0x240`, which **the engine writes during its
  commit**, after our hook: read before the commit it was wrong on ~2% of G-buffer draws (the skinned ones and a few
  others). Our draws read it when the draw is recorded, after the commit. A draw that skips the commit has to compute
  it, or stay with the game while its program reads rows 16-19 (open);
- last frame's bones (constants 5) are the palette before the newest, on 99.6% of skinned draws; the rest have the
  newest there too (not decoded; it only touches motion vectors).

The check counts material constants only in the stages whose shader reads their slot (the shader's `.iface`); the
other stage keeps an older block bound. Summary over four saves, every layer kind: constants 2, material constants,
textures, samplers, effect, programs, bones, render state, NVN state and vertex layout equal the game's on every
draw; last frame's bones on 99.6% of skinned draws.

**What else changes per draw.** Check mode now counts, per layer kind, the constant buffers (only slots the draw's
shaders read, and only when the contents change) and textures the engine binds for a draw beyond what ours
provides: what a draw that skips the engine's work would still miss. Schoolyard, before the model material block:

| Slot | Name (shader headers) | Changes on |
|---|---|---|
| constants 3 | `CBUSE_UB_MODEL_MATERIAL` | every draw (now decoded, below) |
| constants 11 | `CBUSE_UB_USER_DATA01` | 12-30% of draws: instance copies |
| fragment constants 12 | `CBUSE_UB_USER_DATA02` | 79% of material-pass draws |
| fragment constants 10 | `CBUSE_UB_POINT_LIGHT` | ~2% of material-pass draws |
| fragment texture 9 | `in_texCube` | 6% of material-pass draws: a reflection probe per object |

Everything else rebound per draw is the same value again (counts of ~730 per run are once per layer).

**Model material block: decoded (`model_material`).** Constants 3 (0x160 bytes, slot u16 `list+0x74`), built on the
model renderer's stack: `u_ubParam` (1, 0, submesh index + model `+0x1fe`, alpha test reference), diffuse, ambient
and specular from a 0x30-byte colour record (diffuse times the object's colour and a fade: object `+0x1bc` × the
model record's per-submesh `+0x138`, on RGB for additive-style blending else on alpha), the UV transforms and the
atlas regions' rectangles. Check mode, four saves: equal on every draw (~3.7 million); self-test (fade × 1.001):
no draw equal.

## Alternatives

- **Selection first.** Removes the walk, but needs the rules that the shadow streaks showed we don't have, and
  every draw's data still comes from the game: nothing to skip yet.
- **Leave the hybrid.** Fine for pictures, but the engine keeps doing every draw's state work and our renderer can't
  draw anything the game didn't set up (no resolution scaling of materials, no new passes).
