# 0020 — Shaders: translate the shipped assembly, not the GPU code

Date: 2026-10-21
Status: accepted (owner: "which is best, not accounting for complexity")

## The choice

Every shader file carries two forms of the same program: Maxwell GPU machine code, and the NV_gpu_program5 assembly it was compiled from (recon section 6). The shaders are translated from the **assembly**, ahead of time, to GLSL and then SPIR-V. LAS Recomp's Maxwell translator (MIT, `runtime/nvn/maxwell/translate.c`) is a reference for the Vulkan side only: descriptor layout, constant buffers, texture handles.

Why the assembly, even ignoring effort:
- **It is the source.** The machine code is compiled from it and has lost what matters for a good translation: structured control flow, named and typed declarations, no register allocation or scheduling. The game's shaders were written in HLSL, converted by `hlslcc` (names like `hlslcc_mtx4x4matScale`), and compiled by NVIDIA's GLSL compiler to this assembly.
- **Better Vulkan shaders.** Structured input gives structured SPIR-V that the PC driver optimises well. Lifted machine code becomes jump-driven state machines.
- **Phase 4c edits shaders** (resolution, shadows, AA). That needs readable output.
- Against it: the machine code is exactly what the Switch ran, rounding included. Screenshot diffs against the emulator reference catch differences, and the machine code stays in the files as a cross-check.

## What the shaders use (all 1,891 programs measured)

- 1,022 vertex (`.vsb`, and `.esb`, also `!!NVvp5.0`), 842 fragment, 13 geometry, 14 compute. **1,349 distinct texts.**
- **58 opcodes.** Control flow is only `IF/ELSE/ENDIF`, `REP/ENDREP` (one with a fixed count), `BRK`: structured, no `CAL`/`BRA`. Conditions test the condition code with `NE` or `EQ` only; besides `IF`/`BRK`/`KIL` they also guard writes (`MOV R0.z (EQ.x), ...`, 296 uses).
- Textures: `TEX` 2D (4,077) and `SHADOWARRAY2D` (3,123, shadow cascades), `TXL` 2D/CUBE/3D, `SHADOW2D`, `ARRAY2D`, `TXF`, `TXQ`; one compute image store (`STOREIM`).
- Every program has `NV_bindless_texture`, but **every texture handle is loaded from constant buffer 14 at a constant offset** (`LDC.U64 D0.x, buf14[k]; TEX ... handle(D0.x)`, none computed). Buffer 14 is where NVN puts bound textures' handles, so `buf14[k]` is texture binding k/8. Ordinary Vulkan descriptors are enough; no bindless emulation.
- Other constant buffers are slots 0–13; storage buffers, shared memory, barriers and a few atomics in compute; local arrays (`TEMP lmem0[N]`) in 184 programs.
- Integer vertex inputs exist (bone indices and packed data read with `.U`/`.S` instructions).

## The container (HBH1)

Documented in `tools/yw4/shaders.py`: six (offset, size) sections: vertex inputs (name, location), resources (name, kind, slot), constant-buffer members (name, offset), one not yet understood, `NXGL`, strings. `NXGL` holds the GPU code and the **control section**, which is what the game passes to `nvnProgramSetShaders` as `control` and which contains the assembly text.

## Identifying shaders at run time

Key = FNV-1a 64 of the assembly text, `!!NV` through `END`. `python -m tools.yw4.shaders` extracts each distinct text once to `<work_dir>/shaders/asm/<key>.<stage>.asm` with `index.json`. The runtime finds the text in the control section it is given and computes the same key. No file names, load order or GPU addresses are involved, and an unknown key is a loud stop.

## The translator (`recomp/shader.py`)

`python -m recomp.shader [--check]` writes `<work_dir>/shaders/glsl/<key>.<stage>` (GLSL 4.60) and `<key>.json` (what it binds). `--check` compiles them to SPIR-V with `glslangValidator` from the Vulkan SDK (owner installs it; it also brings the validation layer). Choices, all written down in the module docstring:

- **Registers are raw bits** (`uvec4`), reinterpreted per instruction, because the assembly's registers have no type: `.F`/`.S`/`.U` says how an instruction reads them. A plain `MOV` copies bits. This is exact where a float model would not be (integers stored in float registers would be denormals and could be flushed).
- **One GLSL statement per instruction**, preceded by the instruction as a comment. The GPU driver's compiler removes the redundancy; readability is what matters for Phase 4.
- `MAD` → `fma()` (Maxwell fuses it). `ROUND` → `roundEven`. `TRUNC.U` clamps negatives to 0 (Maxwell's conversion saturates; GLSL's is undefined). `RSQ` takes the absolute value (spec).
- `H` registers (fp16 on the Switch) are fp32 here. Precision can differ; screenshot diffs will show if it matters.
- Bindings: descriptor set = stage; constant buffer n → binding n, texture/image slot k → 16 + k, storage buffer n → 64 + n (texture slots reach 32); inputs/varyings/colour outputs by location. Vertex inputs are `vec4` or `uvec4` by how they are read (two passes); varyings are `vec4` carrying bits, `flat` when read as integers.
- Depth: NVN's default clip space has z in [-w, w]; a push-constant bit maps it to Vulkan's [0, w] in the last stage before rasterisation. Y orientation is left to the viewport.
- `vertex.instance` excludes the base instance (as on Maxwell): `gl_InstanceIndex - gl_BaseInstance`.

Result: **all 1,349 programs translate and compile to SPIR-V** (`--check`, glslang 16.6.0, Vulkan 1.2 target, 47 s in parallel); the translator stops loudly on anything it does not know. Not yet verified: that they render correctly (needs the draw path; checked by screenshot diffs against the emulator reference).

The SDK installer needs administrator rights, so `[tools] glslang` can instead point at a standalone Khronos glslang release (no install; the owner's copy is in the tools folder outside the repo). The SDK is still wanted for the validation layer.

## Next

The Vulkan draw path: pipelines from NVN state, vertex input, descriptors from bound buffers and textures, draws, first real pixels.
