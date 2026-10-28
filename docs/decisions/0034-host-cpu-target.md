# 0034 — Host CPU target: a setting, the 2003 baseline by default

Status: accepted (2026-10-28). The owner asked for the CPU side to be "very very optimized, or allowed to use
more resources" after battles at 21:9 were mostly limited by the game's main thread.

## Context

The build passed no `-march`, so clang compiled the game code and runtime for the first x86-64 CPUs (SSE2
only): no SSSE3 byte shuffle (`pshufb`, close to ARM's `TBL`/`EXT`), no SSE4.1 lane inserts, no AVX2.

"More resources" in the other sense was checked first and is already the case: every guest thread is its own
host thread with no affinity and no global lock (`runtime/src/kernel.c`). The limit is the game's main
thread, which is serial by design; in the owner's 21:9 battle session the whole process used 55% of one core.

## Decision

`recomp/check.py target_flags()`: `-march` from `YW4_MARCH`, default `x86-64` (every PC). The build and the
instruction checks (`recomp.check`, `recomp.functest`) use the same value, and the build recompiles
everything but musl when the flags change (`work/build/obj/flags.txt`). musl keeps its verified flags.

Results do not depend on the target: `-ffp-contract=off` keeps every multiply and add separate and nothing
reorders floating point. With `x86-64-v3` all instruction forms matched the Pi's results.

## Measured

Benchmark: the Yo-kai World replay (busy street, ~790 draws), 21:9 at 900p so the GPU is not the limit, 3
runs of 160 s; the main thread's CPU per frame from the per-thread reports.

| Build | main thread ms/frame | game work ms |
|---|---|---|
| x86-64 (before) | 7.97 | 11.25 |
| x86-64-v3 | 7.92 | 10.30 |

No gain on the main thread beyond the noise (±0.4 ms): the generated code's shape (registers in memory,
lane-by-lane vector code) leaves the compiler nothing the newer instructions would speed up. Hence the
baseline default. A release could build both and pick at start; not worth it without a gain.
