# runtime/

The native runtime the recompiled code links against (decision 0005): HLE of the `sdk` imports, memory, threading, and graphics (NVN on Vulkan).

So far only `include/yw4cpu.h` exists: the guest CPU state and the helper functions generated code uses (flags, conditions, rotates, divides, ...).
