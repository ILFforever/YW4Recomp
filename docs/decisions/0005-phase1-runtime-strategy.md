# 0005 — Phase 1 runtime strategy

Date: 2026-10-20
Status: **accepted** (2026-10-20, owner). CLAUDE.md's phase plan updated to match.

## Recommendation in one paragraph

Recompile **only `main`** to C. Write our own small native runtime that implements the **578 `sdk` imports** directly on the host (threads, files, input, audio, window). Handle graphics at the **NVN API level**: implement the **177 NVN functions the game actually uses** on top of Vulkan, and translate shaders ahead of time from the NVIDIA assembly shipped with every shader. Don't link any emulator code. Use an emulator only as an **external reference**: run the original game in it to get screenshots, frame captures, and timings to compare against.

## What the measurements say (docs/recon.md)

| Fact | Why it matters |
|---|---|
| `main` makes **0 syscalls**; everything goes through **578 `sdk` imports** | We can cut at the `sdk` boundary and never emulate the Switch kernel |
| The imports are high-level APIs: `nn::os` threads/mutex/event, `nn::fs` open/read/write, `nn::hid` controller state, `nn::audio` PCM output, `nn::vi` window | Each maps cleanly to host APIs (OS threads, files, SDL3 or similar for input/audio/window) |
| ~140 imports are online/peripheral (socket, ssl, ldn, nfc, ...) | Can start as stubs |
| CRI, PhysX, Lua, NEX/Pia are statically linked in `main` | They get recompiled with the game, so nothing extra to implement |
| NVN is reached through one import; **177 of 513** NVN functions are used | An NVN→Vulkan layer is a bounded job, not "all of NVN" |
| Every shader ships with **NV_gpu_program5 assembly** | Shader translation works from documented text, not GPU machine code |
| The NVN *driver* lives inside `sdk` | The emulator-GPU fallback would mean recompiling `sdk` too and emulating the kernel's GPU interface. That is the expensive path this proposal avoids |

## Options considered

### A. Hybrid with an emulator (the plan in CLAUDE.md before this decision)
Recompile `main` **and** `sdk`. Link an emulator (Eden/Citron/Ryubing-family) for kernel services, system services, and its Maxwell GPU emulation.

Why `sdk` has to come along: emulators don't implement NVN or `nn::fs`. They run the game's own `sdk` code and step in one level lower, at the kernel's system calls and service messages. Their GPU layer reads the raw Maxwell command stream that the NVN driver in `sdk` writes. So reusing any of it means running `sdk` and providing the kernel interface it talks to.

- For: rendering would be correct early; lots of existing behaviour comes for free.
- Against: we'd have to recompile `sdk` too (5.4 MB more code, including the NVN driver and the syscall layer), and adapt emulator code that expects to own guest CPU emulation, guest memory, and guest threads. Untangling that is a large, open-ended job. And everything that works only works through a GPU *emulator*: exactly the layer the project wants to replace.
- License: the yuzu-derived emulators are GPL, so linking them makes the project GPL. That is acceptable for a fan project if chosen on purpose; it was not the deciding factor. (Ryujinx/Ryubing is reportedly MIT, but it is C#. Verify its license file before relying on that.)

### B. Custom runtime, HLE at the `sdk` boundary, NVN→Vulkan (recommended)
- For: small and fully ours; no GPL; native x86-64 threads and memory; the NVN layer is already the start of the "own renderer" goal. It stays fast because nothing is emulated.
- Against: no free correctness reference for rendering. NVN details (memory pools, texture layouts, command buffers backed by app memory, sync) must be implemented carefully. The first time anything appears on screen is later than in A.

### C. B plus an emulator used only as an external reference (recommended, = B in practice)
Same as B. Separately, run the original game in an emulator to capture reference screenshots, RenderDoc frames, and timings. No emulator code in this repo, so no GPL. This keeps the "screenshot diff" check that CLAUDE.md wants for Phase 4c.

## Performance compared with A (expected, not yet measured)

- **CPU:** same. Both recompile `main` the same way; C runs less code because `sdk` is replaced by direct host calls.
- **GPU:** expected faster. C skips the work A does every frame: running the NVN driver, decoding the Maxwell command stream back into meaning, and the emulator's texture cache (guessing what memory holds textures and keeping copies in sync).
- **No shader stutter:** all shaders are translated ahead of time, while emulators compile shaders the first time an effect appears. The baseline benchmark should show that stutter; the port shouldn't have it.
- **Where C can be worse:** visual bugs early on (no years of emulator fixes), a naive first NVN layer that is slow in spots (over-cautious syncs, extra copies), and any CPU readback of GPU results, which emulators already handle. Check early how much the game reads back.

## Other design points this enables

- **Same addresses on host and guest.** The Switch is 64-bit little-endian like x86-64. We can map `main` at a fixed host address and let guest pointers *be* host pointers, with no address translation in generated code. (N64Recomp has to translate 32-bit addresses; we don't.) Risk to check: whether the game assumes addresses fit in the Switch's 39-bit space or tags pointers. That seems unlikely; verify early.
- **Function list:** `.eh_frame_hdr` starts (39,094) plus address-taken targets, cross-checked by Ghidra (docs/ghidra.md). Indirect calls go through a lookup table built from that list, with a loud "unknown target" trap instead of a JIT. If the trap ever fires in practice, we add an interpreter fallback then.
- **Atomics:** exclusive pairs become compare-and-swap on the host. x86's stronger memory model covers ordinary loads and stores.
- **Testing:** instruction-level golden tests generated on the Pi 5 (decision 0004).

## What changes in CLAUDE.md if accepted

- Phase 1: "Hybrid is the expected start" → custom runtime (option C).
- Phase 3: unchanged in spirit (implement only what the import list needs), now with no emulator code underneath.
- Phase 4a: "keep the emulator's GPU layer" → our NVN→Vulkan layer for the 177 used functions is the fallback, with AOT-translated shaders. The external emulator run is the correctness reference. The 4b/4c plan (trace the frame, replace passes one at a time) stays the same.
- The line "deliberately skipping a full NVN → Vulkan reimplementation" stays true in spirit: it's not *full*, only the 177 used functions.

## Risks to watch

1. **NVN semantics without a reference implementation.** Mitigation: emulator captures to compare against. The NVN headers' documented behaviour; small test scenes first (menus are 2D).
2. **Something in `main` depends on `sdk` internals** beyond the import list (shared structs, callbacks into `sdk`). The import list shows the surface, but data layouts passed across it still have to be right.
3. **Compile size:** 4.3 M instructions of generated C. Plan for splitting into many files and a parallel build from day one.

## First milestone if accepted

`main` recompiled, linked against a runtime where every import logs and returns a safe default, running from the entry point until it needs something real. That run produces the ordered to-do list for Phase 3.
