# 0012 — musl start-up, TLS, C++ exceptions, first C library functions

Date: 2026-10-21
Status: accepted (except the open question at the end)

## `__nnmusl_init_dso`: return "run the constructors", nothing else

main's DT_INIT (main+0x30) calls `__nnmusl_init_dso` with main's base and section bounds: dynamic section, relocation tables, hash table, GOT, init/fini arrays, and what look like TLS bounds (0x150d650/0x150d651, base+1). It saves the result in a flag (main+0x150e0d0):
- result **1**: skip the constructors. On the next DT_INIT call the flag is non-zero and DT_INIT jumps straight to the constructor loop. So 1 means "not now".
- any other result: run the 830 `.init_array` constructors now.

The sdk's own version (read in the owner's `sdk.bin`, ~775 instructions) returns only 1 or 2. Its real work is registering the module and giving it a TLS block in the calling thread (`nnosAllocateMemoryForThreadLocal` + `memset`). A search of all of `sdk` for an init-array call loop found only the sdk's own `_init`, so nothing else ever runs main's constructors.

**Decision:** return 2 after checking that the base is main's. `__nnmusl_fini_dso` does nothing (main's `.fini_array` is empty).

## TLS: main has none to set up

main contains no `MRS` at all (decision 0008), so it never reads the thread pointer and can't reach a TLS block. Its per-thread values go through `nn::os` TLS slots (`AllocateTlsSlot`, `GetTlsValue`, `SetTlsValue`...), which are ordinary imports. They come with threads.

## C++ exceptions: not supported, because nothing throws

`main` neither imports nor defines `__cxa_throw`, `__cxa_allocate_exception` or `__cxa_rethrow`. It imports `_Unwind_Resume`, `__cxa_begin_catch` and `__gxx_personality_v0` (landing pads compiled into the code), but those run only while an exception unwinds, and none can start. They stay unimplemented: if one is ever reached, the import stub stops with its name. No unwinder is needed.

## `__cxa_guard_acquire` / `__cxa_guard_release`

Function-local statics. The game's inline fast path is `ldarb` + `tbz #0` on the guard's first byte (main+0x5c64), the Itanium C++ ABI layout. So byte 0 bit 0 = initialized (stored with release order), and byte 1 = initialization in progress (a second acquire then stops as recursion). With threads, acquire must instead wait for the other thread.

## C library: only what is exact

The host computes a libc function only if its result is fully defined by the inputs: memory and string functions, character classes, integer parsing (`hle_string.c`, 37 functions). Where C leaves the value open, musl's choice is reproduced:
- `memcmp`/`strcmp`/`strncmp`/`strcasecmp` return the difference of the first differing bytes (as unsigned chars), not just −1/0/1. Game code may store the value.
- Character classes are musl's "C" locale: ASCII only, nothing above 127.
- Guest `long` is 64-bit and the Windows host's is 32-bit, so `strtol`/`strtoul` go through `strtoll`/`strtoull`.
- `errno` is one guest-visible int for now (per thread later).

## Open: functions whose results depend on the implementation

`sin`/`cos`/`pow`/`exp`/... (28 math imports), `qsort` (the order of equal elements depends on the algorithm; musl uses smoothsort), the `printf` family's float output, `strtod`, `strerror` text. The host's versions can differ from musl's (decision 0008 found MinGW's libm off by one unit in the last place). Matching the Switch exactly means using musl's own source for these. musl is MIT-licensed, so it's compatible, but it's a new third-party dependency (CLAUDE.md rule 6). **Not decided yet**; until then these imports stay unimplemented and stop the program when reached.

## Result

```
yw4: running DT_INIT (static constructors) (main+0x30)
yw4: unimplemented import #94 _ZN2nn7account12AsyncContextC1Ev
     called from main+0x3a9a8
```
