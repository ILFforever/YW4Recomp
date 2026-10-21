# runtime/

The native runtime the recompiled `main` and `sdk` link against (decisions 0005, 0014): the kernel (system calls, threads, memory), system services, and graphics (NVN on Vulkan).

So far:
- `include/yw4cpu.h`: the guest CPU state and the helpers generated code uses (flags, conditions, rotates, divides, memory access).
- `include/yw4rt.h` + `src/dispatch.c`: what generated functions call into: module bases, indirect-call lookup across modules (`yw4_resolve`), tail-call macros and traps (decisions 0007, 0014).
- `include/yw4load.h`, `src/loader.c`, `src/sha256.c`, `src/main.c`: load and relocate `main` and `sdk` (refusing any other build), host callbacks, start-up through the sdk's `nn::init::Start` (decisions 0010, 0014).
- `src/hle*.c`, `src/heap.c`: native versions of sdk functions written before 0014; unused since the sdk is recompiled, kept until the recompiled sdk passes the same checks.
- `src/hle.h`, `src/hle_os.c`, `src/hle_diag.c`, `src/hle_mem.c`, `src/heap.c`: first sdk imports: start-up memory, diag logging/asserts, `nn::mem::StandardAllocator` on a TLSF heap (decision 0011).
- `src/hle_libc.c`, `src/hle_string.c`: musl start-up (`__nnmusl_init_dso`), static-local guards, and the exact C library functions: memory, strings, ctype, integer parsing (decision 0012).
- `src/hle_math.c`: the 40 libm imports, bit-exact with the sdk: musl 1.1.19 (`third_party/musl`) plus exact instruction models; checked with `python -m tools.yw4.sdkref --check` (decision 0013).
- `src/crypto.c`: AES/SHA instructions (Crypto extension).
- `src/kernel.h`, `src/kernel.c`: the Horizon kernel the recompiled sdk calls through `SVC`: memory map, heap, thread properties, single-thread synchronisation, system counter, `FPCR` writes (decision 0014). `YW4_TRACE_SVC=1` traces calls.
