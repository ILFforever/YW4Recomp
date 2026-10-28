# 0036 — Native skeleton matrices

Status: accepted, on by default (2026-10-28). Code in `runtime/src/bones.c`, hooks in `recomp/overrides.toml`.

## Context

`main+0xa77390` and `main+0xa79c00` (twins) turn a skeleton's local bone transforms (scale, rotation
quaternion, translation) into matrices: bone 0 alone, then parent matrix × local matrix for every other bone.
They are the hottest game code on the main thread (about a quarter of its busy time in busy scenes, roughly
doubled at 21:9 in battles). The recompiled versions are ~550 ARM instructions that become ~12,800 x86 ones,
because every register lives in memory (decisions 0034 and 0035: neither a newer CPU target nor caching
registers helped).

## Decision

Hand-written C versions, run by a hook before each function (the hook returns 1, so the recompiled body is
skipped). They use the same float operations, in the same order and grouping, so the results are
bit-identical, not merely close:

- x86 SSE and ARM give the same result for every add, multiply and divide when no NaN is involved (FPCR is 0:
  round to nearest, no flush-to-zero; decision 0008). Addition and multiplication are commutative in IEEE,
  so only the grouping has to match (read from the disassembly).
- NaNs: we refuse inputs with NaN up front (the game's code runs then). Without NaN inputs every NaN comes
  from an invalid operation; ARM gives the default NaN `0x7FC00000`, x86 a negative one, so ours are fixed
  up to ARM's when stored.
- The constants the functions use (a lane mask, the vector for the parent's 4th column, the "usual scale")
  live in `.bss`, filled at start-up, so we read them from memory instead of hard-coding them.
- The rare parts call the game's own recompiled code: `main+0xa77c30` (a bone whose parent has an unusual
  scale, when the caller asks for it) and `main+0xa7a530` (the second function's per-bone extra step), with
  the registers and stack the original would give them.
- Not done natively, decided before anything is written so the game's code can take over: the definition's
  second mode (flag `+0x22` bit 0: tracks non-uniform scale with a bit set and rebuilds bones from column
  lengths with `frecpe`), and parents that don't come before their child.

`YW4_BONES=check` runs the game's code, then ours from the same starting matrices, compares every float and
keeps the game's result. `YW4_BONES=game` turns ours off for A/B timing.

## Why not a recompiler change

A general fix (registers in host registers) was tried and was slower (0035). These two functions are a
known, measured hot spot; a native version costs ~200 lines and can be checked exactly in game.

## Results (2026-10-28)

- `YW4_BONES=check` on all nine test saves (`work/testsaves`: Yo-kai World, school, forest, late game, walk,
  lag 1-4), 2 minutes each: about 10 million calls, **0 mismatches**. The scaled-parent path ran ~350,000
  times and the extra step ~5 million times; the second mode never came up, nor did a NaN input.
- Benchmark (Yo-kai World street, 21:9, 900p, 3 runs each): main thread 7.60 -> 6.45 ms per frame (-15%),
  game work 10.01 -> 8.99 ms.
