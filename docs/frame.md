# The engine's frame

How the game builds a frame, from the engine's side: who decides what is drawn, and where our renderer can take
over ([decision 0023](decisions/0023-custom-renderer-plan.md)). Measured in the town street (the owner's save) with
`YW4_TRACE_RENDER_JOBS` and `YW4_TRACE_DRAW_CALLERS` ([debugging.md](debugging.md)); function names are offsets in
`main`.

## How work reaches the GPU

1. **The main thread schedules.** The engine runs named tasks each frame (registered through `f_96c470` with a
   name string: game logic such as `CSceneRpg080::Player` and the render stages below go through the same
   system). Render tasks don't draw: they append **jobs** to a per-frame list.
2. **A job is a callback:** a list node holds a function and two arguments (node `+8` function, `+0x10` a,
   `+0x18` b, `+0x30` next). `f_971ad0` is a job that runs a sub-list of the same kind.
3. **Three render threads run the lists** (`f_979790`: for each node, `fn(device, a, b)`), each with its own NVN
   command buffer, and submit. The NVN calls happen inside the jobs.

So the engine's passes are visible as jobs, and the place to hook a pass is its job function (render thread) or
the task that queues it (main thread).

## The three render threads

| Thread | Work | Queued by |
|---|---|---|
| A | **Shadow maps**: 3 cascades, 1024×1024 depth | `yw::CMainEndComponent` vtable slot 13 (`f_5dbe30`) → `f_7ed570` |
| B | Occlusion boxes for the camera (240×135), ambient cube map (128×128), **the G-buffer** (layer kind 2), the screen-space shadow mask | `yw::CMainComponent` slot 12 (`f_5d77f0`) |
| C | Main view: the named stages below, then UI, then present (`f_95b740`) | the stage tasks below |

## Shadow maps (thread A)

`f_7ed570` queues, per cascade (cascade index 0, 1, 2 passed to `f_7ee9a0`):

| Job | Arguments seen | Probably |
|---|---|---|
| `f_7ee4e0` | a = `0x71012982bc` / `c0` / `c4` (4 apart) | per-cascade value |
| `f_7ee510` | a, b = per-cascade structs (`0x71056ab3b0`, `…3f0`, `…430`: 0x40 apart) | set the cascade's target / matrices |
| `f_9abfc0` ×7, `f_9ab8c0` ×154 (in a sub-list) | a = `0x7105883790`, b = one object each | occlusion-query boxes (the 36-vertex boxes at 240×135, shader `b320bd…`) per caster |
| `f_7ed4d0`, `f_7ee830`, `f_7ee980` | | pass setup |
| `f_7ee9a0` | a = cascade index | select the cascade |
| `f_7ed100`, `f_7ecf00` | | render targets / state |
| `f_a45890` | b = layer item of **kind 1** | draw the shadow casters |
| `f_7ed350` | | end of cascade |

`f_7ed570` has six copies of this block; three ran in the street. The other three are not identified yet.

### Cascade data

Computed on the main thread every frame by `yw::CMainComponent` slot 11 (`f_5d4270`, the update; slot 12 queues
the render jobs) → `f_7e5570` → `f_a4e830(camera, out, near[], far[], third[], 3)`. `f_a4e830` inverts the camera
matrix, then calls `f_a4d100` once per cascade; `f_7e5570` copies the results into the **renderer object**
(`a` = `0x71056ab1b0` = `main+0x56ab1b0`, a static object, same address every run):

| Renderer offset | Per cascade i | Contents |
|---|---|---|
| `+0x200 + i·0x40` | 4×4 floats, row-major | light view: same rotation for all cascades, own translation |
| `+0x140 + i·0x40` | 4×4 floats | projection, **perspective along light-space y** (last row `0 1 0 0`, so w = y): a perspective-warped shadow map (LiSPSM family), not the usual orthographic cascades |
| `+0x080 + i·0x40` | 4×4 floats | projection × view, multiplied in `f_7e5570` |
| `+0x2c0 + i·0x10` | 4 floats | per-cascade values (not decoded) |

The per-cascade settings (`main+0x56b07a0` = `[renderer+0x568] + 0x1cf0`, three arrays of three floats):

| Cascade | near | far | third (light depth range?) |
|---|---|---|---|
| 0 | 0.1 | 20 | 60 |
| 1 | 20 | 50 | 110 |
| 2 | 50 | 150 | 200 |

`f_7ee510` (the per-cascade job) copies the view matrix (a) and projection (b) into the render state
(`device+0x2c0`, `+0x280`) and takes the viewport from render target #0x14 (the shadow map).

**For the replacement:** read the finished matrices from the renderer object; the warped-projection math only
needs re-implementing for a better-shadows mod later.

### What the GPU gets

- One depth texture, **3 layers** (one per cascade), 1024×1024, format `0x34`.
- Culling per draw: none or back faces (polygon state `0,…` / `2,…`); depth test and write on.
- Five programs in the street (`YW4_TRACE_DRAWLIST`, 38 shadow draws):

| Vertex shader | Draws | Position |
|---|---|---|
| `b320bd…` | 16 | cbuf 2 bytes 128-191 (projection × view × world, per object, made on the CPU) × vertex |
| `a74842…`, `744205…` | 12 | cbuf 1 bytes 128-191 (projection × view of the cascade) × instance transform (cbuf 11, 3×4 per instance, `instance × 48` bytes; from `object+0x140 → +0x80`) × world (cbuf 2 bytes 0-63) × vertex: **instanced** props |
| `71faac…` | | **skinned**: up to 8 bones per vertex from cbuf 4 (3×4 each), then cbuf 2 bytes 128-191 like `b320bd` |
| `4cbb8e…` | | static (`b320bd`) plus texture coordinates (attribute 1) and the alpha threshold (cbuf 3 byte 12): **static cut-out** |
| `9904c2…` | | instanced (`a74842`), with the fade fragment shader `b7cd6e` |
| `fa9033…`, `61ebac…` | | wind-swayed plants: textures read in the vertex stage |

**Share of the shadow draws by program pair** (street, `YW4_SHADOWS=check`, about 300,000 draws): `b320bd`+`6ead6a`
(static) 38%; `744205`+`dceb89` (instanced cut-out) 20%; `a74842`+`6ead6a` (instanced) 13%; `fa9033`+`dceb89` 8%;
`71faac`+`b7cd6e` (skinned, fade) 6%; `4cbb8e`+`dceb89` (static cut-out) 6%; `9904c2`+`b7cd6e` (instanced, fade) 5%;
`e758a5`/`f8951f`+`d5f536` 3%; `61ebac`+`dceb89` 1%.

**Skinned draws, as bound** (`71faac`): the three vertex streams are one buffer, the submesh's vertices (stride 60):
position `0x22` at 0, weights `0x2f` at 28 (eight 16-bit weights in four 32-bit words; the shader scales the sum by
1/65535; bones 5-8 only if weight 5 is not 0), bone indices `0x2c` at 44 (eight bytes, ×48 into cbuf 4). cbuf 2
byte 128 equals our projection × view × world.

**Where the bone palette comes from:** cbuf 4 is a copy, made right before each skinned submesh's draw, of a palette
the game built earlier (one 3×4 matrix per bone of that submesh, e.g. 41, 51 or 6 bones). The copy is the engine's
generic constant-buffer upload `f_965eb0`: a state slot (`device + stage·0x150 + slot·0x18 + 13000`) holds a buffer
object B, the CPU data is at `[B+0x40] + [B+0x58]`, the size in the slot (`+0x32d0`), and when the slot is dirty
the data is copied into the frame's buffer (`f_977550`) and bound. The model renderer puts the submesh's palette
object in the slot: for submesh `i` of a geometry, B = `[[object+0x140] + 0xc8] + 0x140·i + 0x78` (a record of
0x140 bytes per submesh; the size at `+0x98`). Found by value: for each skinned draw, the bytes at that B's data
equal cbuf 4 (check mode, every draw tried).

| Fragment shader | Does |
|---|---|
| `6ead6a…` | nothing: depth only |
| `dceb89…` | alpha cut-out: drops texels of the material's texture (fs slot 0) below the threshold the vertex stage passes |
| `b7cd6e…` | **fade**: drops pixels by a 16×16 screen pattern (texture fs slot 18, `texelFetch` at the pixel position mod 16) where the object's fade (fs cbuf 3 byte 28 × attribute 3.w) is below the pattern's value; at full opacity nothing is dropped |

Checked by value: cbuf 1 bytes 128-191 in a shadow draw equal the renderer's projection × view of that cascade
(`+0x80 + i·0x40`, same row-major layout; the shader takes `dot(row, position)` per output component). Per-object
cbuf 2 starts with the object's world matrix (`object+0`, 38 of 38).

Vertex attribute layouts seen in the shadow draws (format / stream / byte offset, from `BindVertexAttribState`):

| Attributes | Draws | Likely |
|---|---|---|
| 0: `0x22` / 0 / 0 | 22 | position only |
| 0: `0x22` / 0 / 0, 1: `0x12` / 1 / 32 | 7 | position + texture coordinates (cut-out) |
| 0: `0x22` / 0 / 0, 1: `0x2f` / 1 / 28, 2: `0x2c` / 2 / 44 | 11 | position + bone weights and indices (skinned) |

**How textures are bound** (read from `f_974cd0`, the engine's "apply pending state" function, which also holds its
`BindTexture` and `GetTextureHandle` calls): the state block passed to it as its second argument has texture slots
at `+0x780 + slot·16` (texture object pointer, "set" byte at `+8`). A texture object keeps its NVN texture id at
`+8` and a small cache of finished handles per sampler from `+0xf8` (16-byte entries: handle, sampler id). The
slot's name hash is looked up in the bound program's binding table (`program+0xb8`, count at `+0xc0`: pairs of
name hash and binding index), the handle is made with `GetTextureHandle(texture id, sampler id)` and bound. Not
found yet: where the material keeps its texture objects (searching the draw object and the material set
`object+0x120` up to three pointers deep for the ids found nothing; next try is a write watch on a texture slot).

**What a replaced shadow pass needs per frame:** the three cascade matrices (renderer object); the shadow layer's
objects (draw list, layer kind 1, `object+0x1d1` set); per object its world matrix, geometry and submesh records
(vertex/index data, counts, stride), bones if skinned, instance transforms if instanced, and the cut-out texture
if alpha-tested. Still open: the vertex attribute layout per shader, where the material's texture handle comes
from, and how the engine culls objects per cascade (the occlusion boxes).

## The model renderer `f_a45890`

Draws one **layer**: `f_a45890(device, draw list, layer item)`. Every layer gets the same draw list (nodes from
`[list + 0x20]`, linked at `+0x30`); the layer item picks which nodes it draws: kind byte at `+8` (0 = normal view,
1 = shadow, 2 = seen with the ambient cube), a key range at `+0xa`/`+0xc`, a mask at `+0x10`. For kind 1 it skips
objects whose byte `+0x1d1` is 0 ("casts no shadow"). `runtime/src/shadow.c` (`gather`) re-implements this selection; `YW4_SHADOWS=check` compares it with what the game draws (0 missed over 3,600 shadow layers). This list is the engine-level data a replaced pass
would read instead of NVN commands.

Read so far from the start of its loop (offsets; meanings partly guessed):

| Where | Field |
|---|---|
| node `+0x00` | draw object (if the object's `+0x108` is set, that object is used instead) |
| node `+0x10` | per-instance data; per-submesh records of 0xb8 bytes from `+0xb0` |
| node `+0x20` | layer mask byte (skip if it has the layer's bit) |
| node `+0x30` | next node |
| object `+0x101`, `+0x103` | signed bytes: priority / sort range |
| object `+0x120` | material set (its `[+0x18]+0x23` byte changes kind-0/kind-1 handling) |
| object `+0x128`, `+0x140` | model resource (`+0x140`: submesh count at `+0x200`) |
| object `+0x1b0`, `+0x1c8`, `+0x1cb`, `+0x1d1` | skip in shadow layers if `+0x1b0` ≠ 0; **number of geometries** (per geometry: `*(+0x128)[g]`, model `+0x140 + g·0x240`); layer bit; casts shadow |
| object `+0x80`, `+0x90` | bounding box min and max (local space) |
| model `+0x80` | instance transforms (instanced objects; their world matrix is the identity) |
| `[draw list + 0x18]` | submesh table, records of 0x50 bytes (bytes `+0x34`, `+0x3f`, `+0x43` tested) |

### How a draw item reaches the GPU

`f_a45890` works in two phases: it walks the nodes and fills an array of 0x48-byte draw items on its stack, then
sorts and draws them. Each mesh draw is `f_a75e90(device, geometry, primitive)` (call sites `a4bf90`, `a4c2cc`;
the object being drawn is in `f_a45890`'s stack slot `sp+0x390`, the geometry is `*(object+0x128)`); `f_a75e90`
sets the vertex range and draw call, then the engine's generic "flush state and draw" (`f_977e30` / `f_974630`)
issues the NVN calls. Measured with `YW4_TRACE_DRAWLIST` (150 draws in the street: 38 shadow, 112 main view):

| Data | Where | Found in |
|---|---|---|
| world matrix (4×4) | **object `+0x00`** (a copy at `+0x40`); it is the start of vertex constant buffer 2 | 143 of 150 draws |
| vertex data base | `[geometry+0x20] + 0x00` | all |
| index area base | `[[geometry+0x28]]`: the geometry's index buffer object, as `f_a75e90` reads it; usually the second object of the vertex buffer's record (`[geometry+0x20] + 0x68`), not always (one town object: 1.5% of draws there) | all |
| bones (vertex constant buffer 4) | `object+0x110 → +0x138` | skinned draws (59 of 112) |
| vertex constant buffer 6 (material values?) | `object+0x280 → +0x328 → +0x140` | all shadow, 62 of 112 |

The **primitive record** (one per submesh, 0x50 bytes):

| Offset | Meaning |
|---|---|
| `+0x00` | vertex offset from the vertex data base |
| `+0x04` | index offset from the index area base |
| `+0x08` | vertex count (× stride = the bound vertex buffer size) |
| `+0x0c` | index count (0: not indexed) |
| `+0x10`, `+0x20` | two float4 (w = 1, 0): probably bounding box min and max |
| `+0x38` | small number (submesh or material slot?) |
| `+0x3d` | primitive type (through a table) |
| `+0x3e` | vertex stride in bytes (0x24, 0x28 seen) |
| `+0x3f` | flags; bit 1 picks the index type |
| `+0x43` | byte the renderer uses as an index (bone palette or LOD?) |

Why decode it rather than keep `f_a45890`: in the street the render threads' busy time is about 3% `f_a45890`,
4% the engine's NVN wrapper layer (`f_965xxx`-`f_97xxxx`), 4.5% other game code and 7.5% our NVN → Vulkan
translation (`YW4_PROFILE=submit`, percent of all samples; the threads wait the other 84%). A pass drawn from the
list directly drops the wrapper layer and the translation, roughly 60% of that busy time.

## Main view: the passes (measured 2026-10-25)

Schoolyard, daytime, one frame (~1,000 draws), from `YW4_GPU_TRACE` + `YW4_TRACE_DRAW_CALLERS` (grouped by
`python -m tools.yw4.mainview`) and `YW4_TRACE_LAYERS`. The drawing function is the one right below the engine's NVN
wrappers (`f_9749c4`, `f_974bec`) in the call stack.

| # | Target | Draws | What | Drawn by |
|---|---|---|---|---|
| 1 | 240×135 depth | ~110 | occlusion boxes, before each cascade | `f_9ab8c0`, `f_9abfc0` |
| 2 | shadow map, 3 layers | 27 / 84 / 137 | shadow cascades: model renderer, **layer kind 1** | `f_a45890` (our pass, 0024) |
| 3 | 128×128 | 3 | ambient cube map | `f_7f88e0` |
| 4 | 1600×900, 3 colour (formats 0x11, 0x25, 0x25) + depth 0x35 | ~200 | **G-buffer**: model renderer, **layer kind 2** (keys 0..25) | `f_a75e90` < `f_a45890` |
| 5 | 800×450 / 533×300 | ~5 | half-res: depth copy (`f_80d270`, `f_8050d0`), **shadow mask** (`f_7f3e90`, the PCSS shader of 0025), `f_7f4c90` (2 targets) | |
| 6 | 1600×900, 1 colour (0x3f) + the G-buffer's depth | ~210 | **lit material pass**: the same objects again with their materials, reading the half-res results: model renderer, **layer kind 0, keys 0..2** | `f_a75e90` < `f_a45890` |
| 7 | 1600×900 and half-res | ~30 | full-screen lighting, fog and effects; a few more model layers (kind 0, other key ranges) and `f_a3dcb0` < `f_a3dbf0` < `f_a45890` (effects) | `f_807980`, `f_80c9e0`, `f_80b810`, `f_7fcd20`, `f_7f3900`, `f_8007d0` |
| 8 | 400×225 → 200×112 → 120×67 | 6 | bloom chain, then composite | `f_802e40` |
| 9 | 1600×900, 340×304, 516×460 | ~120 | UI and minimap | `f_9cfec0`, `f_9dfb40`, `f_a8c3f0` |
| 10 | 1920×1080 | 1 | copy into the window texture | `f_95b740` |

**The scene geometry is drawn twice** (a light pre-pass renderer): once into the G-buffer (kind 2), then, after
the half-res shadow and light passes, again with full materials (kind 0, keys 0..2). Both go through the same model
renderer and mesh-draw function as the shadow cascades, so what [the model renderer section](#the-model-renderer-f_a45890)
says about draw items, objects, matrices, buffers and bones holds for them too.

Layers of one frame (`YW4_TRACE_LAYERS`):

| Kind | Keys | Submeshes | Pass |
|---|---|---|---|
| 1 | 0..25 | 29 / 88 / 133 | shadow cascades (thread A) |
| 2 | 0..25 | 202 | G-buffer (thread B) |
| 0 | 0..2 | 180 | lit material pass (thread C) |
| 0 | 2..3, 3..10, 10..16, 16..20, 22..23, 30..38, 60..64 ... | 0-25 each | the named stages below (thread C) |
| 0 | 0..65535 | 0-9 each, ~40 layers | small special layers (to identify) |

## Main-view stages (thread C)

Registered by `f_5d8010` in this order, each queueing jobs (mostly `f_a45890` layers plus pass functions in
`f_7ecf00`–`f_811400` that take the renderer object `0x71056ab1b0`):

| Stage | Function |
|---|---|
| `CMainComponent::PostCamera` | `f_5d9380` |
| `gmdCDrawObjModelPriority:00_UI_Before` | `f_5d9420` |
| `…:00_Zero` | `f_5d94d0` |
| `…:10_MapBefore` | `f_5d9a10` |
| `…:15_ProjEffect` | `f_5d9c70` |
| `…:20_CharaBefore` | `f_5d9f90` |
| `…:25_CharaAfter` | `f_5da4d0` |
| `…:30_MapAfter` | `f_5da6d0` |
| `…:40_Effect` | `f_5dac50` |
| `…:50_Post` | `f_5db060` |
| `…:51_PostAfter` | `f_5db0d0` |
| `…:60_UI` | `f_5db190` |
| `PostMenuEndDraw` | `f_5db300` |

UI is drawn by other layer drawers (`f_9f6170`, `f_a88c70`, `f_9cfe60`, `f_9dfb40`).

## Materials (measured 2026-10-25)

Read from `f_a45890`'s second phase (Ghidra merges `f_a4bf90` into it) and checked on G-buffer draws with
`YW4_TRACE_DRAWLIST` (`YW4_TRACE_DRAWLIST_COLOURS=3`: G-buffer draws only).

The second phase walks the sorted **draw items** (0x48 bytes): `+0` geometry, `+8` the **model record** of that
geometry (`object+0x140` + 0x240 × geometry index), `+0x10` the submesh records, `+0x18` the **material records**,
`+0x28` texture references, `+0x30` material constants. A submesh's byte `+0x43` picks its material record (the
same index the shadow selection uses for the sort key).

| Material record (16 bytes) | |
|---|---|
| `+2` (u16), `+0xa` (u8) | constants: start (in 16-byte units, from the draw item's `+0x30` array) and count; uploaded per draw with `f_977550` into a fresh uniform buffer. The model record's `+0x78` holds per-object overrides (0x80 bytes each) when `+0x219` bit 0 is set |
| `+4` (u16), `+0xb` (u8) | render state entries (2 bytes each, in the geometry header blob) |
| `+6` (u16), `+0xc` (u8) | textures: start in the draw item's `+0x28` table (6 bytes each; byte 0 = entry of the model's texture table) and count |
| `+8` (s8) | sort key |
| `+0xe` (u16) | UV transforms, 0x20 bytes per texture, from the model record's `+0x98` (`+0x90` when `+0x219` bit 1) |

**Textures:** the model record's texture table (`+0xf8`; `+0xf0` in one case) has 16-byte entries
`{texture resource, index}`. The resource's header (`+0x18`) holds a 0x18-byte descriptor per index
(`hdr + 4×u16(hdr+0xa) + 4×u16(hdr+0x3e) + index×0x18`). From those the engine makes **temporary texture views**
each draw (0x50 bytes: `+0` the NVN texture, `+8` the texture id that the shader handle carries, then format and
swizzle; ids rise through the frame and differ between runs) and puts them in the texture slots of a static state
at `main+0x58569f8 + 0x780 + slot×16`, which `f_977e30` commits when the submesh is drawn (`f_a75e90`).

So at our submesh-draw hook the engine has already resolved a draw's textures and constants: a first replaced
G-buffer or material pass can take them from the bound state (as the shadow pass does for cut-out textures) and only
a full takeover needs the descriptors decoded.

## What depends on the screen size (measured 2026-10-27)

For resolution scaling of the 3D view: what in a frame is sized from the game's 1600×900. Town walk, one frame
(`YW4_GPU_TRACE` with `YW4_GPU_TRACE_SCREEN=1`, which logs every constant that equals a screen-derived size or
0.25..4 over one; and a scan of the 565 translated fragment shaders in `work/shaders/asm`).

**Images.** Every image sized from the screen is a GPU-only render target:

| Size | Of 1600×900 | Used for |
|---|---|---|
| 1600×900 | 1 | G-buffer (3 colour + depth), lit material pass, lighting/fog/effects, and the UI (drawn into the same colour image) |
| 800×450 | 1/2 | depth copy, shadow mask, lamp lights, effects |
| 533×300 | 1/3 | effects |
| 400×225, 200×112, 120×67 | 1/4, 1/8, ~1/13 | bloom chain |

Not screen-sized and left alone: the window image (1920×1080; the last pass copies the 1600×900 image into it by
sampling), the occlusion boxes (240×135 depth, culling only), the ambient cube (128×128), the minimap (340×304,
516×460), the shadow maps (own setting).

**Shaders: resolution-independent, nearly.** They reach the screen three ways, none tied to the size:

- the pixel position (`fragment.position.xy`, 295 shaders): times 1/16 to tile a 16×16 dither texture (the
  screen-door fade of objects), or written out as depth. At a higher resolution the dither pattern gets finer.
- whole-pixel reads (`TXF`, 261 shaders): position × 16, truncated, into the same dither texture.
- size queries (`TXQ`, 74 shaders): they return the image's real size, so they follow it.

Screen images are read in 0..1 coordinates from interpolated positions, so the ratio between images (a half-size
image read at half the position) holds as long as all of them grow by the same factor.

**Constants.** One row: the camera block (`CBUSE_UB_CAMERA_IDX`, cbuf 8) row 32 = 1.5/1600, 1.5/900, 0.75/533,
0.75/300: tap offsets in 0..1 coordinates, read by 215 shaders (the lit pass's edge-aware upsample of the half-size
lighting: the position ± row 32, a few taps). Unscaled, the taps spread wider in new pixels (a little softer, not
wrong); scaled by 1/factor, the same look. Row 33 = 1920, 1080 (the window), read by no shader. Every other match
was a coincidence (fog and light distances of 300, 120, 200; 0.01 written in 397 shaders). Three shaders hold odd
small numbers worth a look before shipping: `4f5854cdab65c4b5` (0.005), `5ef1a937fe5a9908` (1/320, 1/160), the
compute shader `ff9fe0a375532914` (1/1920-like; not run in a town frame).

**Commands.** The frame copies no image by command (no copy, blit or resolve is recorded): the engine shrinks images
by drawing them into smaller ones. Pixel rectangles appear only as viewports and scissors (and clears through the
scissor), which the runtime already scales for the shadow maps (`gpu_dynamic_state`).

So resolution scaling is runtime work: make the screen-sized images bigger by one factor (fractional: 1440p is ×1.6,
and 533 does not divide evenly), scale viewports and scissors of draws into them, and scale camera row 32. Battles,
menus and cutscenes are still to be checked for other screen-sized images.

## Post-processing (measured 2026-10-28)

Bamboo forest, late-game save, one frame (`YW4_GPU_TRACE` + `YW4_TRACE_DRAW_CALLERS`, grouped by `tools.yw4.mainview`).
After the lit pass, the lighting and the effects, two engine functions do the post-processing, then the UI follows:

| Draws | Target | Programs (vs/fs) | What | Drawn by |
|---|---|---|---|---|
| 2 | 800×450 | `98c09b`/`2d68c8` | distance blur: the scene shrunk | `f_8007d0` (`f_80eb90`) |
| 2 | 800×450 | `29d827`/`b8856c` | distance blur: two blur passes | `f_7f3900` < `f_7f3620` < `f_8007d0` |
| 1 | 1600×900, blend | `93ff16`/`27b63a` | distance blur: blended over the scene by depth (decision 0031's half setting) | `f_8007d0` |
| 1 | 800×450 | `abf6e5`/`d794c4` | bloom: bright parts | `f_802e40` |
| 6 | 400×225, 200×112, 120×67 | `e1e948`/`158d33` | bloom: blur, across then down, per level | `f_802e40` |
| 1 | 1600×900, blend | `53e413`/`58b87f` | bloom: the three levels onto the scene | `f_802e40` |

### Bloom `f_802e40(device, renderer)`

`renderer` is the renderer object (`main+0x56ab1b0`). Its images are 0x28-byte records from `+0x2e28`: record `i` at
`+0x2e28 + 0x28*i` holds the engine texture object at `+0`, a descriptor at `+8` and two flags at `+0x22`/`+0x23`.
Render targets come from the target manager `[main+0x5865158]`, vtable slot `+0xd8`, by id; their size is
`u16 +0x3a` × `u16 +0x3c`.

1. **Bright parts** (`d794c4`, into the 800×450 target) from records 0 (slot 0) and 11 (slot 1). Constants (fragment
   cbuf 6, 0x30 bytes) = `renderer +0x800..+0x80f` (row 0, for slot 0), `+0x7f0..+0x7ff` (row 16, for slot 1) and
   `+0x7b0`; when `renderer +0x388d` is set, the
   `.w` of both rows is raised to at least 1.0. The shader: `d = in1 - in0`, `m = max|d|` over rgb,
   `a = max(in1 - 1.5 m, 0)`, out.rgb = `max(a * row16.rgb - row16.w, in0 * row0.rgb - row0.w)`,
   out.a = `max(in1.a, in0.a)`, all clamped at 0. Its output is record 9.
2. **Blur**, three levels, each across (into attachment 1 of target id 7, 8, 9) then down (into attachment 0):
   9 → 20 → 19 (400×225), 19 → 22 → 21 (200×112), 21 → 24 → 23 (120×67) (tables `main+0x1298380` ids, `+0x129838c`
   / `+0x12983a4` / `+0x1298398` records). Constants (cbuf 6, 0x40 bytes per pass): row 0 = (step, 0, 0, 0) across or
   (0, step, 0, 0) down with step = `renderer +0x7c0 + 4*level` / the target's width (or height), row 16 = 1.0 ×4,
   row 32 = the tap weights 0.150217, 0.138667, 0.118165, 0.092952, row 48 = 0. The shader adds 4 pairs of taps
   (8 taps) with those weights; the tap positions come from the vertex shader.
3. **Composite** (`58b87f`, into the 1600×900 scene image, blend on) from records 19, 21, 23 (slots 0, 1, 2).
   Constants (0x18 bytes, rows 0, 8, 16, two floats each) = (`+0x7d0`, `+0x7e0`), (`+0x7d4`, `+0x7e4`),
   (`+0x7d8`, `+0x7e8`): per level a brightness and a weight. The shader combines the three levels as a "screen"
   (1 - (1-a)(1-b)(1-c), each clamped) and its alpha from their brightest channels.

At 4K 21:9 the composite took 1.5-2.2 ms of GPU per frame (2026-10-28), the rest of bloom well under 1 ms.

All 8 draws use one sampler: min filter 3, mag 1, clamped (7), levels 0-15. The runtime's texture filtering
setting upgrades it (it allows mip levels), so a pass of ours must make its sampler the same way
(`gpu_sampler`), or about 20 texels per frame come out one bit different (decision 0037).
The full-screen quad in the engine's vertex buffer: corners (-1, 1), (-1, -1), (1, -1), (1, 1) with texture
coordinates (0, 0), (0, 1), (1, 1), (1, 0).

## Model detail at a distance (checked 2026-10-27): not used

The engine has a level-of-detail class, `lives::gmdCObjModelLodInfo`, registered with its property system in
`main+0xa31a20`: `m_pLodDistance` at `+0x38` (a pointer to floats in the loaded model data), `m_lodDistanceNum` at
`+0x40` (a byte), set by `SetupLodDistance` (`main+0xa319e0`: info `x1`, floats `x2`, their size in bytes `x3`);
`lodChangeFadeTime` suggests models fade between levels. In the town walk, the late-game bamboo forest and the
Yo-kai World, a hook on `SetupLodDistance` was never called, every object had one geometry (object `+0x1c8`), and
the submeshes an object draws changed only by a few at a time as the camera turned (per-submesh culling by their
bounding boxes), never as a swap at a distance. So these maps don't use model LOD, and a "push detail further"
setting has nothing to act on there. What looked like distance drop-off was the distance blur and the texture
filtering (decision 0031 and the settings panel). Battles and other maps are not checked.

## Open

- ~~Which job draws into which target in the main view~~: measured, see "Main view: the passes". Still open: which
  key ranges belong to which named stage, and what the ~40 kind-0 layers with keys 0..65535 are.
- ~~Materials~~: see "Materials". Still open: the texture descriptor's fields (image address, format, size), the
  render state entries, and which shader program a material selects.
- The draw list and draw objects `f_a45890` walks: per object the world matrix, mesh buffers, bones, material,
  and the "casts a shadow" flag. This is the main data every replaced mesh pass needs (next step).
- The three unused shadow blocks in `f_7ed570`.
