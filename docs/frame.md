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
| B | Occlusion boxes for the camera (240×135), ambient cube map (128×128), some passes | `yw::CMainComponent` slot 12 (`f_5d77f0`) |
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
| `71faac…`, `4cbb8e…` | 10 | skinned: up to 4 bones per vertex from cbuf 4 (3×4 each; `object+0x110 → +0x138`) |

| Fragment shader | Does |
|---|---|
| `6ead6a…` | nothing: depth only |
| `dceb89…` | alpha cut-out: samples the material's texture (bindless handle in cbuf 14 slot 0), discards |
| `b7cd6e…` | alpha cut-out with a threshold from cbuf 3 byte 16, texture handle at cbuf 14 byte 144 |

Checked by value: cbuf 1 bytes 128-191 in a shadow draw equal the renderer's projection × view of that cascade
(`+0x80 + i·0x40`, same row-major layout; the shader takes `dot(row, position)` per output component). Per-object
cbuf 2 starts with the object's world matrix (`object+0`, 38 of 38).

**What a replaced shadow pass needs per frame:** the three cascade matrices (renderer object); the shadow layer's
objects (draw list, layer kind 1, `object+0x1d1` set); per object its world matrix, geometry and submesh records
(vertex/index data, counts, stride), bones if skinned, instance transforms if instanced, and the cut-out texture
if alpha-tested. Still open: the vertex attribute layout per shader, where the material's texture handle comes
from, and how the engine culls objects per cascade (the occlusion boxes).

## The model renderer `f_a45890`

Draws one **layer**: `f_a45890(device, draw list, layer item)`. Every layer gets the same draw list (nodes from
`[list + 0x20]`, linked at `+0x30`); the layer item picks which nodes it draws: kind byte at `+8` (0 = normal view,
1 = shadow, 2 = seen with the ambient cube), a key range at `+0xa`/`+0xc`, a mask at `+0x10`. For kind 1 it skips
objects whose byte `+0x1d1` is 0 (likely "casts no shadow"). This list is the engine-level data a replaced pass
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
| object `+0x1b0`, `+0x1c8`, `+0x1cb`, `+0x1d1` | skip in shadow layers if `+0x1b0` ≠ 0; visible; layer bit; casts shadow |
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
| index area base | `[geometry+0x20] + 0x68` | all |
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

## Open

- Which job draws into which target in the main view (G-buffer, lighting, transparent, post): match the jobs to the
  GPU passes of an F11 log.
- The draw list and draw objects `f_a45890` walks: per object the world matrix, mesh buffers, bones, material,
  and the "casts a shadow" flag. This is the main data every replaced mesh pass needs (next step).
- The three unused shadow blocks in `f_7ed570`.
