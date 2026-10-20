# Phase 0 recon: Yo-kai Watch 4++ v2.2.0

Measured on 2026-10-20 from the pinned build (`target.toml`). Numbers come from
`python -m tools.yw4.recon`, which writes the full tables to `work/recon/`
(gitignored, because they contain names taken from the game). This file keeps
the summaries and what they mean for the port.

Status: sections 1–7 done, including the Ghidra-based NVN trace (setup in
`docs/ghidra.md`). Still open: baseline benchmark and the Phase 1
recommendation; see "Open questions".

## TL;DR

- **Engine:** Level-5's in-house C++ engine, namespace `lives`, with the game in namespace `yw`. Lua 5.2 for scripting. Middleware: CRI ADX2 (audio), CRI Sofdec (video), CRI FileSystem (CPK archives), NVIDIA PhysX 3.4, Nintendo NEX 4.6.2 + Pia 5.17 (online).
- **Size:** `main` is 4.34 M instructions in about **39,000 functions**. Every middleware library above is statically linked into `main`, so it gets recompiled with the game, not reimplemented.
- **OS boundary is small:** `main` imports **578 symbols** from `sdk` and makes **zero** syscalls itself. That list is the HLE checklist.
- **Graphics boundary is one function:** NVN is reached through `nvnBootstrapLoader`, which hands back a table of 513 function pointers. Intercepting that one import gives us every NVN call.
- **Indirect calls are tractable:** 39,891 indirect call sites, but the possible targets are known: 17,853 address-taken functions from relocations, and 17,850 of them line up exactly with unwind-table function starts.
- **Assets:** 8.5 GB RomFS, almost all in 2,185 CRI `.cpk` archives (63,504 files). Models and textures use Level-5's "G4" formats.
- **Shaders:** precompiled in RomFS (1,890 programs), and **each one still contains its readable NVIDIA assembly source** next to the GPU machine code. Shader translation can work from that assembly instead of decompiling GPU code.

## 1. Modules

| Module | Image | .text | .rodata | .data | .bss | Symbols |
|---|---|---|---|---|---|---|
| `rtld` | 13 KB | 6.5 KB | 3.3 KB | 0.6 KB | 3.4 KB | 19 (all imports) |
| `main` | 22.1 MB | 17.3 MB | 4.2 MB | 0.5 MB | **90.0 MB** | 578 imports, 17 exports |
| `sdk` | 12.7 MB | 5.4 MB | 6.8 MB | 0.4 MB | 0.8 MB | 25,077 exports |

No `subsdk*` modules. SDK version 7.3.2. Key generation 7.

- `rtld` is the dynamic loader. `sdk` is Nintendo's SDK as one module (system services, musl libc, libc++, NVN loader).
- `main`'s 90 MB `.bss` is a large static block, most likely the engine's own heap. `main` also exports `malloc`/`free`/`calloc`/`realloc`/`aligned_alloc`, so it replaces the SDK allocator. The runtime must give `main` a writable 90 MB region at the right place and let it own allocation.
- `main` keeps an `.eh_frame_hdr` (C++ exceptions are on: it imports `__gxx_personality_v0`, `_Unwind_Resume`). That table lists 39,094 function start addresses for free.

## 2. Engine and middleware

From `SDK MW+` tags and string scans (`work/recon/middleware.md`):

| Library | Evidence |
|---|---|
| Level-5 engine | RTTI namespace `lives` (1,012 classes), game in `yw` (926 classes) |
| Lua 5.2 | `Lua 5.2` string, `yw::CObjLua`, compiled Lua chunks (`\x1bLua`) in RomFS |
| CRI ADX2 / Atom | `CRIWARE/...` DSP effects, `#CRIATOM`, `.acb`/`.awb` in RomFS |
| CRI Sofdec / Mana | `CCriSofdecDecoder`, `.usm` videos (2.5 GB, 9 files) |
| CRI FileSystem | `CRIFSMEM`, all assets in `.cpk` |
| NVIDIA PhysX 3.4.0 | `SDK MW+NVIDIA+PhysX-3_4_0-release`, `physx` RTTI, `.col` (`PXCL`) files |
| NEX 4.6.2, Pia 5.17.0 | `SDK MW+Nintendo+NEX…`, `SDK MW+Nintendo+Pia…` (online/local multiplayer) |
| libcurl, zlib | Nintendo SDK builds, 7.3.2 |

Not found: Havok, Wwise, FMOD, Unity, Unreal, Bink, Effekseer, FreeType, libpng.

## 3. Imports from `sdk` (the HLE checklist)

578 imports: 554 functions, 20 data objects, 4 linker markers. All resolve to `sdk`. Full list: `work/recon/imports.md`.

| Group | Count | Notes |
|---|---|---|
| libc (musl) | 117 | math, string, stdio. Can map to the host C library. |
| `nn::os` | 92 | threads, mutexes, events, ticks, TLS. Must be real. |
| `nn::socket` | 56 | online only |
| C++ runtime | 29 | new/delete, RTTI, exceptions. Host toolchain provides these. |
| `nn::fs` | 29 | file access. Where RomFS / LayeredFS (translation mod) plugs in. |
| `nn::ldn` | 26 | local wireless play. Stub. |
| `nn::hid` | 24 | controllers, touch |
| `nn::ssl` | 20 | online only |
| `nn::mem` | 18 | SDK heap helpers |
| `nn::account` | 16 | user account / save selection |
| `nn::audio` | 15 | audio output (CRI renders, SDK plays) |
| libc++ | 14 | |
| `nn::nfc` | 13 | amiibo. Stub. |
| `nn::oe`, `nn::err`, `nn::swkbd` | 11 each | app events, error dialogs, software keyboard |
| `nn::nifm`, `nn::time`, `nn::crypto`, `nn::vi` | 8–9 each | network state, clock, crypto helpers, display layer |
| everything else | ≤5 each | friends, ngc (profanity filter), util, diag, result, web, settings, cstd, ec, aoc, nsd |
| `nv` | 4 | `nv::InitializeGraphics`, allocator setup |
| `nvn` | 1 | `nvnBootstrapLoader` |

What this means: 141 imports (socket, ssl, ldn, nfc, nifm, friends, web, ec, ngc, nsd, aoc) are online or peripheral and can start as stubs. The core to get booting is `os`, `fs`, `hid`, `audio`, `vi`, `account`, `time`, `mem`, libc/libc++, and the NVN table.

## 4. C++ classes, vtables, and indirect calls

From `work/recon/rtti.md` and `code.md`:

- **2,672 classes with RTTI**: 544 base, 1,969 single inheritance, 159 multiple inheritance. Class names survive in full, e.g. `lives::CShaderLibrary`, `yw::CCameraCtrlBattle`.
- **1,842 vtables** with **37,957 virtual-function slots** (largest: 88 slots); 1,549 slots are pure-virtual.
- RTTI by top-level namespace: `lives` 1,012, `yw` 926, `nn` 623 (statically linked NEX/Pia etc.), `pead` 34, `std` 32, `physx` 21.

Instruction mix in `.text` (4,335,852 instructions):

| Kind | Count | Per 1000 | Meaning for the recompiler |
|---|---|---|---|
| `bl` | 146,663 | 33.8 | direct calls: easy |
| `b` | 121,704 | 28.1 | jumps and tail calls |
| `blr` | 39,891 | 9.2 | **indirect calls** (virtual calls, callbacks): need a dispatch table |
| `br` | 3,596 | 0.8 | indirect jumps, mostly `switch` jump tables: need table recovery |
| `ret` | 43,477 | 10.0 | |
| `svc` | 0 | 0 | no syscalls in `main` |
| FP/SIMD | 332,994 | 76.8 | **7.7% of all instructions**: FP/NEON lifting is needed from day one, not later |

**Threads and atomics** (`code.md`):

| Instruction | Count | Meaning |
|---|---|---|
| `ldxr` / `stxr` (exclusive pairs) | 2,632 / 2,631 | ARMv8.0 atomics (read-modify-write loops), in 680 functions |
| `ldar` (acquire load) | 8,296 | atomic loads. Many are probably static-initialisation guard checks |
| `stlr` (release store) | 456 | atomic stores |
| `dmb` | 16 | explicit barriers |
| `cas`, LSE atomics | 0 | as expected: the Switch's Cortex-A57 is ARMv8.0 |
| `mrs tpidr_el0` | 0 | `main` never reads the TLS register itself; thread-local access goes through `sdk` |

Thread creation: only 7 direct call sites (`nn::os::CreateThread` x6, `nnosCreateThread` x1), plus 4 `SetThreadName` calls. The game is multithreaded, with a small, fixed set of threads.

What this means for the recompiler:
- Exclusive pairs map to compare-and-swap loops on the host (standard technique).
- **x86-64 first helps here.** x86's memory model is stronger than ARM's, so ordinary ARM loads and stores are never *more* ordered than what x86 gives for free. The generated C still needs compiler barriers or `atomic` operations at `ldar`/`stlr`/`dmb`/exclusives, so the C compiler doesn't reorder across them. An ARM host would need real barriers in more places.

**Indirect-call targets are mostly known statically.** 46,622 pointer relocations point into `.text`, at 17,853 distinct functions. 17,850 of those are also function starts in `.eh_frame_hdr`. Two independent sources agree, so function discovery can seed from the unwind table (39,094 starts) and the dispatch table can start from the address-taken set. Gap: function pointers built in code with `adrp`+`add` produce no relocation. Those still show up as unwind-table starts, so registering every unwind-table function in the dispatch table covers them, unless a function has no unwind info at all. A runtime fallback (interpreter/JIT) is still needed for that leftover case, as CLAUDE.md expects.

## 5. NVN usage

- `main` imports only `nvnBootstrapLoader`, plus `nv::InitializeGraphics` and allocator setup.
- It contains **513 `nvn*` function-name strings**, about the whole NVN API. That is the standard loader pattern: get `nvnDeviceGetProcAddress` from the bootstrap loader, then fill a table of every NVN entry point by name. 144 of the names are `nvnCommandBuffer*`.
- The loader stores the pointers in **one contiguous table of 512 slots** (one per name, `0x5863c98`–`0x5864c98` in `.bss`). Traced with `python -m tools.yw4.nvn_report` (Ghidra script `NvnCallers.java`); details in `work/recon/nvn.md`.
- **177 of the 513 NVN functions are actually used** (read from the table by some code). By object: command buffer 54, texture + texture builder 26, memory pool 10, sampler builder 9, queue builder 8, device 8, and so on.
- Each used NVN function is read by a thin **wrapper** function (108 wrappers in total). Draw, dispatch, submit, and present each have exactly one wrapper.
- **Draw wrappers are never called directly.** Their addresses are built in code (`adrp`+`add`) and stored, presumably as function pointers in render commands that get run later. This is the case section 4 warned about: pointers with no relocation. All of them are unwind-table function starts, so the dispatch table plan still covers them.
- **96 "producer" functions** take the address of NVN wrappers. They are the bottom of the engine's render layer:
  - One large setup function references 24 wrappers (device builder and init): NVN device creation.
  - About 7 functions each reference the same 6 draw variants (arrays, elements, instanced, indirect). They look like per-mesh draw-submission paths that pick a draw call by type.
  - `yw::CMainComponent` vtable slots 11 and 12 reference buffer map/flush/invalidate and barriers: likely the per-frame begin/end. This is the first place to start tracing the frame (Phase 4b).
  - `lives::CDeviceUnitNX` owns the present (`QueuePresentTexture`) and submit (`QueueSubmitCommands`) wrappers: the engine's Switch graphics-device class.
- For the port: hooking `nvnBootstrapLoader` / `GetProcAddress` still gives one place to redirect any NVN function, and the 177-function list is how much of NVN a direct NVN→Vulkan layer would need, if we ever wrote one.

## 6. Renderer recon (static part)

**Shaders:** precompiled, in RomFS, not in `main`.
- `data/nx/shader/shader.cpk` (15 MB): 2,161 files. Vertex `.vsb`, fragment `.fsb`, geometry `.gsb`, `.esb` (unknown stage), plus `.fxbin` files that look like effect descriptions binding them together.
- `data/nx/shader/*.csb`: 13 loose compute shaders in an `HBH1` container (culling, light tiles, occlusion, texture reduction).
- Across all CPKs: 1,008 `.vsb`, 842 `.fsb`, 13 `.gsb`, 14 `.esb`, 282 `.fxbin`. All of them are in `shader.cpk`.
- The compute shaders hint at the renderer design: GPU culling (`cs_cull_polygon*`, `cs_instance_occlude*`) and tiled lighting (`cs_light_tile`).

**Every shader ships with its NVIDIA assembly source.** Each `HBH1` file (all 1,877 programs in `shader.cpk` plus the 13 loose compute shaders) contains three things:
1. A Level-5 header with resource bindings, keyed by name hashes.
2. An `NXGL` section with the Maxwell GPU machine code.
3. The **NV_gpu_program5 assembly** it was compiled from (`!!NVvp5.0` for vertex and `.esb`, `!!NVfp5.0` fragment, `!!NVgp5.0` geometry, `!!NVcp5.0` compute), plus uniform/sampler names (`u_rate`, `t_color`, ...).

What that assembly looks like. This is a **made-up example** in the public NV_gpu_program5 syntax, not code from the game: a fragment shader that samples a bindless texture whose handle sits in a constant buffer.

```
!!NVfp5.0
OPTION NV_bindless_texture;
CBUFFER cb[] = { program.buffer[0] };
LDC.U64 R0.x, cb[0];
TEX.F R1, fragment.attrib[0], handle(R0.x), 2D;
MOV.F result.color, R1;
END
```

Why it matters: emulators have to decompile Maxwell machine code to translate shaders. We can translate this readable, documented assembly (NV_gpu_program5 is a public OpenGL extension spec) to SPIR-V/GLSL instead. That makes ahead-of-time shader translation for Phase 4a much easier, and makes the shaders readable for 4b/4c. Caveat: the assembly uses `NV_internal` options and bindless handles loaded from constant buffers, so binding layout still has to be worked out from the `HBH1` header. The remaining `.fxbin` files don't contain shader code; they look like effect/pipeline descriptions (not parsed yet).

CPK entries are mostly CRILAYLA-compressed. `tools/yw4/cpk.py` decompresses them, checked against all 2,161 files in `shader.cpk` (sizes match exactly).

**Renderer-looking classes** (352 names match render/draw/shader/material/camera/light/etc.; list in `work/recon/render_classes.txt`). The most useful starting points for Phase 4b:
- `lives::CShaderLibrary` (with `VS`/`PS`/`GS` subtypes), `lives::IShaderLibrary`, `lives::CComputeShaderManager`, `lives::CResComputeShader`
- `lives::CRenderBuffer`, `lives::CDrawIF`, `lives::CMenuRender`
- `lives::CCamera`, `lives::CCameraCtrl`, plus about 15 `yw::CCameraCtrl*` game cameras
- `lives::CLightManager`, `lives::CLightObject`, `yw::CCharaSelfShadow`, `yw::CCharaLightData` (a `CUniformBlockProperty`)
- `lives::CCustomShaderParam`, `lives::CCustomAddMaterial`
- The engine uses a component/property system (`lives::TComponent<T>`, `lives::CProperty`, `TAddPropertyCreator`). Render settings appear to be attached as properties, which may make per-object render data easier to find.

## 7. RomFS

8.46 GB, 4,518 files on disk (full tables: `work/recon/romfs.md`).

| Folder | Size | Contents |
|---|---|---|
| `data/common` | 6.6 GB | `chara`, `effect`, `event/ev01..ev91`, `font`, `map`, `menu`, `movie`, `sound`, `system`, `text` |
| `data/nx` | 2.1 GB | Switch-specific: `font` (ja, zh_hans, zh_hant), `map`, `shader`, `sound` |
| `data/cpk_list.cfg.bin` | 7 MB | index of the CPK archives |

On disk: `.cpk` 4.6 GB (2,185), `.usm` video 2.5 GB, `.awb`/`.acb` audio 1.4 GB, `.col` PhysX collision, `.p3lip` (lip sync?), `.bin` configs.

Inside the 2,185 CPKs (63,504 files, all readable; most entries compressed with CRI's CRILAYLA):

| Format | Files | Uncompressed | Likely meaning (unverified) |
|---|---|---|---|
| `.g4tx` | 7,343 | 7.8 GB | textures (magic `G4TX`) |
| `.g4mg` | 4,270 | 1.6 GB | mesh geometry |
| `.g4pk` | 5,015 | 612 MB | model package |
| `.g4pkm` | 4,266 | 66 MB | model package metadata? |
| `.bin` | 31,490 | 81 MB | Level-5 config tables; some compiled Lua |
| `.objbin`, `.mevbin`, `.linb`, `.clobin` | ~7,600 | 15 MB | object / event / script data |
| `.ptlb` | 1,078 | 4 MB | particles |
| `.g4sk`, `.g4md`, `.g4cm`, `.g4la`, `.g4nv`, `.g4tg` | 267 | 7 MB | skeleton, model, camera?, light anim?, nav mesh? |
| shaders | ~2,000 | 28 MB | see section 6 |

Translation mod / LayeredFS note: text lives in `data/common/text` and fonts in `data/common/font` + `data/nx/font`. A file-level override inside `nn::fs` can redirect any path. Overriding single files *inside* a CPK would need CPK-level support. Check how the existing translation mod packages its files before designing this.

Existing tools: whether community Level-5 tools already parse the G4 formats is **not yet checked**.

## Open questions

1. ~~Which NVN functions are actually called, and by whom.~~ Answered statically (section 5): 177 used, 108 wrappers, 96 producers. Still open: following the producers up to the frame loop (Phase 4b, best done in the Ghidra GUI or with runtime tracing).
2. ~~Shader binary format.~~ Answered: `HBH1` wrapper with Maxwell code **and** NV_gpu_program5 assembly (section 6). Still open: the exact `HBH1` header layout (bindings), and what `.fxbin` describes.
3. **G4 formats.** Check existing Level-5 modding tools before writing parsers.
4. **`sdk` strategy.** `main` uses 578 of `sdk`'s 25,077 exports. HLE those 578, or recompile `sdk` too and HLE at the syscall level? This is the main Phase 1 decision. The small import list favors HLE at the `sdk` boundary.
5. **Translation mod packaging:** loose-file replacements vs. rebuilt CPKs.
6. **Baseline benchmark** (Phase 0 step 6): needs an emulator choice.
