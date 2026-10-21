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

- 1,022 vertex (`.vsb`, and `.esb`, also `!!NVvp5.0`), 842 fragment, 13 geometry, 14 compute. Median 49 lines, longest 564. **1,201 distinct texts.**
- **57 opcodes**, 113 opcode+modifier forms. Control flow is only `IF/ELSE/ENDIF`, `REP/ENDREP`, `BRK`: structured, no `CAL`/`BRA`.
- Textures: `TEX`/`TXL` 2D nearly everywhere; a few `SHADOW2D`, `3D`, `TXF`; `TXQ` on cube maps.
- Every program has `NV_bindless_texture`, but **every texture handle is loaded from constant buffer 14 at a constant offset** (`LDC.U64 D0.x, buf14[k]; TEX ... handle(D0.x)`, 1,857 loads, none computed). Buffer 14 is where NVN puts bound textures' handles, so `buf14[k]` is texture binding k/8. Ordinary Vulkan descriptors are enough; no bindless emulation.
- Other constant buffers are slots 0–13; 268 programs use storage buffers; compute uses shared memory, barriers and a few atomics.

## The container (HBH1)

Documented in `tools/yw4/shaders.py`: six (offset, size) sections: vertex inputs (name, location), resources (name, kind, slot), constant-buffer members (name, offset), one not yet understood, `NXGL`, strings. `NXGL` holds the GPU code and the **control section**, which is what the game passes to `nvnProgramSetShaders` as `control` and which contains the assembly text.

## Identifying shaders at run time

Key = FNV-1a 64 of the assembly text, `!!NV` through `END`. `python -m tools.yw4.shaders` extracts each distinct text once to `<work_dir>/shaders/asm/<key>.<stage>.asm` with `index.json`. The runtime finds the text in the control section it is given and computes the same key. No file names, load order or GPU addresses are involved, and an unknown key is a loud stop.

## Next

The translator (assembly → GLSL), checked with `glslangValidator` on all 1,201 programs; then the Vulkan draw path.
