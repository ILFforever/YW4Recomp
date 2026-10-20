# 0006 — Recompiler design (first version)

Date: 2026-10-20
Status: accepted (revisit after the first full compile of `main`)

## Decision

**Language.** The recompiler (ARM64 → C translator) is written in **Python**, in `recomp/`. It shares the readers in `tools/yw4/` (NSO, relocations, symbols). The *generated* code is C, compiled with any C11 compiler (GCC 13 today; Clang later for the full build). This replaces the "C/C++ in `recomp/`" note in decision 0001.

**Guest CPU state.** One struct holds the ARM64 registers:

```c
typedef struct {
    uint64_t x[31];   // x0..x30 (x30 = link register)
    uint64_t sp;
    uint64_t nzcv;    // flags in bits 31..28, same layout as the NZCV system register
    v128 v[32];       // SIMD/FP registers (later)
    uint32_t fpcr, fpsr;
    uint64_t tpidr;   // thread pointer (later)
} CPUState;
```

**Functions.** Each guest function becomes one C function `void f_<offset>(CPUState *ctx)`:
- branches inside the function become `goto` labels per instruction address
- `bl` becomes a direct C call
- `ret` becomes `return`
- `br`/`blr` go through a lookup table from guest address to C function; an unknown target traps loudly
- a `b` to another function's start is a tail call: call it, then return

**Pointers.** Guest addresses are host addresses (decision 0005), so loads and stores are plain memory accesses at the register value. Nothing is translated.

**Instruction semantics** follow the Arm Architecture Reference Manual pseudocode (DDI 0487), restricted to ARMv8.0 (what the Switch's Cortex-A57 implements).

## Testing

Every supported instruction form is tested against real hardware (decision 0004):

1. `recomp/golden.py` makes test cases: random encodings within an instruction form (only fields that don't touch SP or the PC for now), plus register values that mix edge cases (0, 1, -1, signed min/max, sign-boundary values) with random ones.
2. It copies `recomp/pi/` to the Pi 5 over SSH, builds the harness with the Pi's GCC, and runs the cases. The harness copies each instruction into an executable page between code that loads and stores the full register file and flags, then runs it.
3. The results (golden files) come back to `work/golden/`.
4. `recomp/check.py` translates each test encoding to C, compiles one test program with the host GCC, runs it on the same inputs, and reports every mismatch with the instruction, inputs, expected, and actual.

Synthetic test cases (random encodings, not taken from the game) are safe to commit, but they are cheap to regenerate, so they also stay in `work/`.

## Why

- **Python:** the recompiler runs once per build, not at game time, so speed barely matters (4.3 M instructions is minutes at worst). Iteration speed and readability matter more for a learning project. If it becomes the bottleneck, the decoder can move to C++ later behind the same tests.
- **Registers in a struct first:** simplest correct translation, easy to test and debug. Keeping registers in C locals inside a function (much faster) is an optimisation for later, guarded by the same tests.
- **Test against hardware per instruction form:** recompiler bugs are silent wrong results. Catching them per instruction is far cheaper than debugging a crashed game.
