# runtime/

The native runtime the recompiled code links against (decision 0005): HLE of the `sdk` imports, memory, threading, and graphics (NVN on Vulkan).

So far:
- `include/yw4cpu.h`: the guest CPU state and the helpers generated code uses (flags, conditions, rotates, divides, memory access).
- `include/yw4rt.h` + `src/dispatch.c`: what generated functions call into: `yw4_image_base`, indirect-call lookup (`yw4_resolve`), tail-call macros and traps (decision 0007).
