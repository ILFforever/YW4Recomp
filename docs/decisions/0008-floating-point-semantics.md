# 0008 — Floating-point and SIMD semantics

Date: 2026-10-20
Status: accepted (FPCR = 0 verified 2026-10-21)

## Decision

**FP mode.** Generated code implements ARM floating point with FPCR = 0: round to nearest even, no flush-to-zero, no default-NaN mode.

**Verified (2026-10-21) that every game thread runs with FPCR = 0:**
- **`main`** contains no `MRS`/`MSR` of any system register (its only system instructions are 1,189 `clrex`, 505 `nop`, 16 `dmb`). It does import `fegetenv`, `fesetenv` and `fesetround`, but calls them from exactly two tiny functions:
  - `0x10048c0`: `fegetenv(saved)`, then `fesetenv(FE_DFL_ENV)` and `fesetround(0)`;
  - `0x10048e0`: `fesetenv(saved)`.

  That is a save / set-default / restore guard (typical of physics code), so the only mode the game itself sets is the default.
- **`sdk`** writes FPCR in three places:
  - `nn::os::detail::UserExceptionHandler` saves and restores FPCR around a callback;
  - `__fesetround` (reached only from `fesetround`);
  - `fesetenv`, which writes FPCR = 0 for `FE_DFL_ENV` (reached from `main`'s guard and from `feupdateenv`).

  The only internal callers of `fesetround` are in `fmal`, which saves the rounding mode, uses nearest, and restores it. `rtld` never touches FPCR.
- **Kernel** (Atmosphère's reimplementation, read as a reference: `KThreadContext::Initialize` and `svc::CreateThread`): a thread's context starts with FPCR = 0. `CreateThread` then copies the *creating* thread's FPCR into the new thread. So the first thread starts at 0, and since nothing above leaves FPCR non-zero, every later thread inherits 0 as well.

**For the runtime (Phase 3):** `fegetenv`/`fesetenv`/`fesetround` can be implemented as storing and returning the environment value. If one is ever called with a non-default mode, it must trap loudly, because generated code only implements FPCR = 0.

**FPSR** (cumulative exception flags) is not modelled. `main` never reads it.

**Arithmetic** goes through helpers in `runtime/include/yw4fp.h` that take and return raw bit patterns:
- For non-NaN inputs and results, x86-64 SSE rounds exactly like ARM, so the helpers use the plain C operation.
- **NaNs differ.** ARM returns the first signalling NaN (quietened), else the first quiet NaN, else the positive default NaN. x86 returns a *negative* NaN for invalid operations, and picks NaN operands by instruction-operand order, which the compiler may swap. So every helper handles NaN inputs first and replaces a NaN result with ARM's default NaN.
- ARM-specific rules are written out: `max(+0, -0) = +0`, `FMAXNM` ignoring one quiet NaN, saturating float→integer conversion with NaN → 0, `FCMP` flags, and NaN payloads in `FCVT`.
- `FRECPS`/`FRSQRTS` are fused (one rounding). Single precision computes in double. A float × float product is exact in double, but adding 2 or 3 to it is not, and rounding that sum first to double and then to float can go wrong. The Pi tests found such a case: the 3 is far below the product's last bit, the double sum drops it, and the float rounding becomes a false tie. The first version assumed the usual double-rounding argument covers this; it doesn't, because the product has 48 bits, not 24. The fix rounds the double sum *to odd* (TwoSum gives the exact error, and a nonzero error forces the last bit to 1), which makes the final rounding to float exact. Double precision uses FMA.
- Fixed-point `SCVTF`/`UCVTF` (4 uses) divide by 2^fbits after converting; this is exact except for results in the subnormal range.
- `FRECPE`/`FRSQRTE` implement the Arm ARM estimate tables bit for bit.

**No libm.** The Pi tests found that MinGW's math library is not exact:
- double `sqrt` and `fma` are off by one unit in the last place on hard inputs;
- `round(0.49999997) = 1`.

The helpers therefore use the CPU instructions instead: SSE2 `sqrtsd`/`sqrtss`, FMA3 `vfmadd`, and hand-written exact rounding. Consequences:
- **Hosts need FMA3** (every x86-64 CPU since 2013: Intel Haswell, AMD Piledriver), but only for double-precision `FRECPS`/`FRSQRTS`. Recheck if older CPUs ever matter.
- **Generated code must be built with `-ffp-contract=off`** and without `-ffast-math`, so the compiler doesn't fuse or reorder float operations itself.

**Vectors** are emitted as loops over lanes of a local copy (sources may alias the destination), using the same scalar helpers. Q=0 forms zero the upper 64 bits.

**Not implemented** (they would trap): `FMLA`/`FMLS` (unused in `main`) and half-precision arithmetic (ARMv8.2, not on the A57). Every SIMD/FP instruction in `main` is implemented.

## Testing

Golden tests as in 0006. On top of that, the vector registers get float edge cases (signed zeros, infinities, quiet/signalling NaNs with payloads, denormals, conversion limits, rounding ties) and *pair states* (e.g. +inf/−inf, two different NaNs, products near 2 and 3 for the reciprocal steps).

Every helper was checked by planting bugs: 23 planted FP/SIMD bugs are all caught, plus the missing round-to-odd (with curated operand pairs for exactly that case). One planted change was not caught, and it is provably equivalent: the `<`/`<=` in the reciprocal-sqrt table loop can't differ for any reachable input.

Golden files store registers an instruction can't read as `*`: a deterministic filler, `recomp.check.filler`. Output lines store only the registers that changed. This keeps 42 forms at 9 MB.

## Why

- **Bit patterns instead of `float` parameters:** NaN payloads and signs can't be lost or normalised by the compiler or the calling convention.
- **The ARM rules are written out instead of trusting the host:** ARM and x86 really do differ on NaNs, and only ARM's results are correct for this game. Writing the rules out costs one predictable branch per operation (NaN inputs are rare).
- **No libm:** "correctly rounded" isn't guaranteed by C for `sqrt`/`fma`/`round` in practice, and the tests proved it. CPU instructions are exact by definition.
