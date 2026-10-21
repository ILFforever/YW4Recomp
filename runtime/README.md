# runtime/

The native runtime the recompiled code links against (decision 0005): HLE of the `sdk` imports, memory, threading, and graphics (NVN on Vulkan).

So far:
- `include/yw4cpu.h`: the guest CPU state and the helpers generated code uses (flags, conditions, rotates, divides, memory access).
- `include/yw4rt.h` + `src/dispatch.c`: what generated functions call into: `yw4_image_base`, indirect-call lookup (`yw4_resolve`), tail-call macros and traps (decision 0007).
- `include/yw4load.h`, `src/loader.c`, `src/sha256.c`, `src/hle.c`, `src/main.c`: load and relocate main (refusing any other build), import stubs, start-up order (decision 0010).
- `src/hle.h`, `src/hle_os.c`, `src/hle_diag.c`, `src/hle_mem.c`, `src/heap.c`: first sdk imports: start-up memory, diag logging/asserts, `nn::mem::StandardAllocator` on a TLSF heap (decision 0011).
