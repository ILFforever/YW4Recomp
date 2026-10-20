# 0007 — Whole-function translation

Date: 2026-10-20
Status: accepted (revisit when the runtime first runs recompiled code)

Builds on 0006 (one C function per guest function, `CPUState`) and 0005 (guest addresses = host addresses).

## Decision

**Entry points** = `.eh_frame_hdr` starts ∪ address-taken targets ∪ Ghidra's entries ∪ every `BL` target. That gives 40,228 functions in `main`.

**Function bodies are found by following control flow** from each entry (`recomp/function.py`, `discover`). We don't use Ghidra's sizes, because they are sometimes wrong. Measured: ~230 branches landed "in the middle of another function" by Ghidra's boundaries, and the examples checked were Ghidra cutting a function short (e.g. a function's size ending 4 bytes before its own tail code). Following flow stops at `ret`, at traps, at a branch to another entry, and when code falls through into another entry. Code reachable from two entries is translated into both; only 162 instructions in `main` are duplicated this way.

**Control flow:**

| Guest | C |
|---|---|
| `b`/`b.cond`/`cbz`/`tbz` inside the function | `goto L_<offset>` |
| branch (or fall-through) to another entry | tail call: `TAILCALL(f_x)` |
| `bl` | `x30 = yw4_image_base + return offset; f_x(ctx);` |
| `blr` | the same through `yw4_resolve(target)` |
| `br` with a recognised jump table | `switch (target - yw4_image_base) { case …: goto …; default: indirect tail call }` |
| other `br` | indirect tail call (`TAILCALL_INDIRECT`) |
| `ret` | `return` |
| `adr`/`adrp`/`ldr` (literal) | `yw4_image_base + constant` |

- **Tail calls** use Clang's `musttail`, which guarantees no host stack growth. That matters if some loop goes through tail calls. GCC (used for tests today) usually turns them into jumps at -O2 but doesn't promise it, so the full build should use Clang.
- **Jump tables** are matched against the one pattern Clang emits in `main`: `cmp #N; b.hi; adrp/add (often hoisted, sometimes copied with mov); ldrsw [T, idx, lsl #2]; add; br`. The table gives the targets and lets discovery reach the case code. The switch is on the *target address*, not the index, so every instruction before the `br` still runs exactly as on hardware. A target we didn't predict goes to `yw4_resolve` and traps loudly instead of jumping somewhere wrong. In `main`: 1,344 tables recognised, 12 `br`s that look like tables are not (their table register is set somewhere the backwards scan can't follow), and 2,252 `br`s are real indirect tail calls (vtables, PLT stubs).
- **Indirect calls** look up the target in `yw4_funcs[]`, a generated table sorted by offset (binary search, `runtime/src/dispatch.c`). An unknown target aborts with the address.
- **Imports** need nothing special in the translator. PLT stubs are ordinary functions ending in `br x17` through the GOT. The runtime's loader will fill each GOT slot with an address `yw4_resolve` maps to the native implementation (Phase 3).
- **Image base** must be 4 KiB aligned, because `adrp` results are computed at translation time as page offsets.
- **Untranslated instructions** compile to `yw4_unimplemented(ctx, pc, enc)`, which aborts with the address. Today that is 11% of `main`, almost all SIMD/FP.

Output goes to `work/recomp/` (`python -m recomp.translate`): 81 files of 500 functions each, plus `funcs.h` and `func_table.c`. All of it compiles warning-free with GCC 13 `-O1 -Wall` (about 80 s on 16 threads) and links.

## Testing

`python -m recomp.functest`: our own test functions (`tests/functest/`: loops, recursion, mutual recursion, indirect calls, tail calls, stack arguments, rodata via ADRP, structs, and two jump tables written by hand in Clang's shape) are compiled on the Pi as ARMv8.0 without SIMD and linked into a flat image like `main`. The Pi runs them natively; the same image is translated, compiled on the host, and run on the same inputs. Return values and every changed byte of the scratch buffer must match. All 17 match. Planted translator bugs (inverted CBZ, wrong TBZ bit, wrong B.cond condition, ADRP off by a page, wrong jump-table targets, jump tables not recognised) are all caught.

## Open risks

- **`ret` ignores x30.** Correct for normal code. It's wrong for code that returns somewhere other than its caller. `main` imports **`setjmp`/`longjmp`** (Lua 5.2's error handling uses them), so this is real. Plan: the translator treats a call to the `setjmp` import specially, using host `setjmp` inside the calling C function so that the HLE `longjmp` can return there. Must be solved before Lua runs.
- **C++ exceptions.** `__cxa_begin_catch`, `_Unwind_Resume` and `__gxx_personality_v0` are imported, but `__cxa_throw` is not, so `main` may never throw. Check before relying on that.
- **False entry points** (e.g. Ghidra-only entries that are really labels) turn a `goto` into a tail call. That's still correct, but slower, and without `musttail` it uses host stack.
- **12 unrecognised jump tables** trap at run time if reached. Fix them when the trap fires, or improve the matcher.

## Why

- **Flow-following instead of trusting boundaries:** a wrong boundary silently drops code. Following flow can only add code, and every path still ends at a defined exit.
- **Switch on the address, with a trapping default:** an imperfect jump-table matcher can only make the result slower or louder, never wrong.
- **Testing on real compiled code:** single-instruction tests can't check branches, calls, the stack or jump tables. Compiling real C on the Pi gives those cases for free and checks the whole chain (discovery, labels, calls, dispatch).
