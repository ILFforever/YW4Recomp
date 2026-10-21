# 0011 — First HLE: memory start-up, diag logging, StandardAllocator

Date: 2026-10-21
Status: accepted

## What nninitStartup does (read from the disassembly, main+0x94eb50)

1. `nn::os::QueryMemoryInfo`, then heap = (available − used) rounded down to 2 MiB.
2. Asserts heap > **0xB9C00000** (~2.9 GiB): the size the game wants for malloc.
3. Logs `Remain Memory Size %lluMiB (%llubyte)` (heap − malloc size, rounded to 2 MiB).
4. `SetMemoryHeapSize(heap)`, `AllocateMemoryBlock(0xB9C00000)`, and hands that block to an `nn::mem::StandardAllocator` (main+0xc5a8d0: constructor + `Initialize(addr, size, false)`).
5. `AllocateMemoryBlock(remainder)`, kept at main+0x5864cb0 for later.

`malloc` is not an import: the game defines its own on top of that StandardAllocator. So the allocator below is the game's whole C/C++ heap.

## Decisions

**Files.** `runtime/src/hle.h` (private to the runtime, so editing it doesn't rebuild the 88 generated files; `recomp/build.py` now tracks `runtime/src/*.h` for runtime objects only), `hle_os.c`, `hle_diag.c`, `hle_mem.c`, `heap.c`. `hle.c` keeps the name → function table.

**Memory numbers.** `QueryMemoryInfo` reports what a retail 4 GiB Switch gives an application: 0xCD500000 bytes available (the application pool of the 4 GiB memory arrangement; from the kernel's table, not measured on hardware). Used = main's image + bss + the 8 MiB main stack. The game then logs 198 MiB remaining. If the real value ever matters, it can be checked on the owner's console.

**Heap address.** The heap is reserved at `0x7200000000`, just above main, inside the 39-bit space. `AllocateMemoryBlock` hands out blocks bottom-up. `FreeMemoryBlock` isn't imported, so nothing more is needed. Only one `SetMemoryHeapSize` call is supported; a second, different size stops with a message.

**Host memory.** The whole ~3.2 GB heap is committed up front (`yw4_reserve`). On Windows that counts against the commit limit (RAM + page file) but uses no RAM until touched; Linux overcommits anyway. If this becomes a problem, reserve the heap and commit per block.

**StandardAllocator = TLSF over the game's own region** (`heap.c`). Reasons:
- Returned pointers must lie inside the block the game passed in: they stay within the 39-bit space, and `GetTotalFreeSize`/`GetAllocatableSize` mean the same as on the Switch. Host `malloc` gives neither.
- TLSF is O(1) for allocate/free, merges neighbours immediately (low fragmentation in a long session), and handles alignment. It is ~250 lines of plain C, so no new dependency.
- Block headers live in the guest region (16 bytes, payloads 16-aligned like AArch64 malloc). Only the free-list index lives in host memory. The guest object keeps a pointer to it in its first 8 bytes.
- `Free`/`Reallocate` check that the pointer is a live block (header + both neighbours agree). A double free or a stray pointer stops the program instead of corrupting the heap.
- Not thread-safe yet; a lock comes with threads.
- Unverified sdk behaviour is not guessed: `Reallocate(p, 0)` stops.

Tested in `tests/test_runtime.py` (300,000 random allocate/aligned allocate/free/realloc operations). Checked: alignment, no overlaps, contents preserved, double free/stray pointers refused, everything merges back into one block, and the largest free block can be allocated whole. Two injected bugs (no backward merge; no minimum gap for aligned allocations) both fail the test.

**diag.** `LogImpl` is variadic. Guest varargs can't go to the host `vprintf`, so `yw4_guest_format` reads each argument where AAPCS64 puts it: x-registers then stack for integers, v-registers then stack for doubles. It formats one conversion at a time with `snprintf`. `%Lf` (128-bit long double) is refused. Log lines are printed as `[game <function>] <message>`. `OnAssertionFailure` and `AbortImpl` stop with whatever strings the release build kept, plus the call address. `nndetailDiagAbortFormatImpl` is a C symbol with no signature, so it prints raw registers instead of guessing.

## Result

```
yw4: running nninitStartup (main+0x94eb50)
[game Startup] Remain Memory Size 198MiB (207618048byte)
yw4: nninitStartup returned, x0=0
yw4: running DT_INIT (static constructors) (main+0x30)
yw4: unimplemented import #1 __nnmusl_init_dso
```

## Next

`__nnmusl_init_dso` gets 18 pointers (x0 = image base, 7 more in registers, 10 on the stack). They look like the module's section bounds for musl's TLS and unwinder registration. If it returns 1, DT_INIT skips the constructors. Implementing it means deciding on TLS and C++ exception unwinding first.
