# 0008 — Floating-point and SIMD semantics

Date: 2026-10-20
Status: accepted

## Decision

**FP mode.** Generated code implements ARM floating point with FPCR = 0: round to nearest even, no flush-to-zero, no default-NaN mode. `main` contains no `MRS`/`MSR` of any system register at all (measured: its only system instructions are 1,189 `clrex`, 505 `nop`, 16 `dmb`), so it never changes FPCR itself. **Assumption to verify** once the runtime runs: that Horizon starts the game's threads with FPCR = 0 (the `sdk` could set it; check with an emulator's thread context or by reading FPCR in a homebrew test).

**FPSR** (cumulative exception flags) is not modelled. `main` never reads it.

**Arithmetic** goes through helpers in `runtime/include/yw4fp.h` that take and return raw bit patterns:
- For non-NaN inputs and results, x86-64 SSE rounds exactly like ARM, so the helpers use the plain C operation.
- **NaNs differ.** ARM returns the first signalling NaN (quietened), else the first quiet NaN, else the positive default NaN. x86 returns a *negative* NaN for invalid operations, and picks NaN operands by instruction-operand order, which the compiler may swap. So every helper handles NaN inputs first and replaces a NaN result with ARM's default NaN.
- ARM-specific rules are written out: `max(+0, -0) = +0`, `FMAXNM` ignoring one quiet NaN, saturating float→integer conversion with NaN → 0, `FCMP` flags, and NaN payloads in `FCVT`.
- `FRECPS`/`FRSQRTS` are fused (one rounding). Single precision computes in double: a float × float product is exact in double, and one double operation followed by rounding to float equals a single rounding (2p+2 ≤ 53). Double precision uses FMA.
- `FRECPE`/`FRSQRTE` implement the Arm ARM estimate tables bit for bit.

**No libm.** The Pi tests found that MinGW's math library is not exact:
- double `sqrt` and `fma` are off by one unit in the last place on hard inputs;
- `round(0.49999997) = 1`.

The helpers therefore use the CPU instructions instead: SSE2 `sqrtsd`/`sqrtss`, FMA3 `vfmadd`, and hand-written exact rounding. Consequences:
- **Hosts need FMA3** (every x86-64 CPU since 2013: Intel Haswell, AMD Piledriver), but only for double-precision `FRECPS`/`FRSQRTS`. Recheck if older CPUs ever matter.
- **Generated code must be built with `-ffp-contract=off`** and without `-ffast-math`, so the compiler doesn't fuse or reorder float operations itself.

**Vectors** are emitted as loops over lanes of a local copy (sources may alias the destination), using the same scalar helpers. Q=0 forms zero the upper 64 bits.

**Not implemented yet** (they trap): `FMLA`/`FMLS` (unused in `main`), half-precision arithmetic (ARMv8.2, not on the A57), and FP fixed-point conversions (4 uses).

## Testing

Golden tests as in 0006. On top of that, the vector registers get float edge cases (signed zeros, infinities, quiet/signalling NaNs with payloads, denormals, conversion limits, rounding ties) and *pair states* (e.g. +inf/−inf, two different NaNs, products near 2 and 3 for the reciprocal steps).

Every helper was checked by planting bugs: 23 planted FP/SIMD bugs are all caught. One planted change was not caught, and it is provably equivalent: the `<`/`<=` in the reciprocal-sqrt table loop can't differ for any reachable input.

Golden files store registers an instruction can't read as `*`: a deterministic filler, `recomp.check.filler`. Output lines store only the registers that changed. This keeps 42 forms at 9 MB.

## Why

- **Bit patterns instead of `float` parameters:** NaN payloads and signs can't be lost or normalised by the compiler or the calling convention.
- **The ARM rules are written out instead of trusting the host:** ARM and x86 really do differ on NaNs, and only ARM's results are correct for this game. Writing the rules out costs one predictable branch per operation (NaN inputs are rare).
- **No libm:** "correctly rounded" isn't guaranteed by C for `sqrt`/`fma`/`round` in practice, and the tests proved it. CPU instructions are exact by definition.
