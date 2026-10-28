# 0035 — Guest registers in local variables: tried, off

Status: rejected for now (2026-10-28); the code stays in `recomp/function.py`, off unless
`YW4_CACHE_REGISTERS=1` at translation.

## Context

The skeleton functions `main+0xa77390` / `main+0xa79c00` (bone rotations to matrices, the hottest game code
on the main thread, about doubled at 21:9) are ~550 ARM instructions each and compile to ~12,800 x86 ones.
Every guest register lives in the CPUState, so nearly every translated instruction loads its registers from
memory and stores the result back, and the vector code moves lanes between vector and integer registers.
Other recompilers (N64Recomp) keep registers in locals so the host compiler can hold them in host registers.

## What was done

`cache_registers` rewrites a function's statements after emission: each register it mentions becomes a
local loaded at the entry; registers it assigns are written back to the CPUState before every call, tail
call, return and kernel call, and everything is reloaded after a call. A function with setjmp, or one that
hands the CPUState somewhere unknown, stays as it was. 40,227 of 40,228 functions in main took the new form.
All instruction checks and whole-function tests (`recomp.functest`, against the Pi) passed.

## Why it is off

- **Slower:** main thread 9.3–9.8 ms per frame against 7.97 ms (same benchmark as decision 0034). Most game
  functions call others often; the write-back and reload of every register around each call cost more than
  the locals save in the few math-heavy stretches. The code also grew (a full build took 616 s, not ~300).
- **Hangs at start-up** in 2 of 6 runs: the main thread waiting for a job, the workers waiting for work, two
  threads running with nothing advancing. Most likely a guest wait loop that reads a memory flag set by
  another thread: once the loop no longer touches the CPUState, the host compiler may read the flag once and
  loop on the old value (plain C loads may be moved out of a loop that writes nothing). Not proven.

## If it is tried again

1. A compiler barrier (`__asm__ volatile("" ::: "memory")`) at every backward jump, so a loop reads memory
   again each time round.
2. Use the calling convention: callees keep x19–x28 and the low halves of v8–v15, so those need neither the
   write-back nor the reload around a call; that roughly halves the per-call traffic.
3. Measure with the same benchmark before turning it on.
