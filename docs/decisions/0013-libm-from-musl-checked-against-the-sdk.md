# 0013 — libm from musl 1.1.19, checked against the sdk's own code

Date: 2026-10-21
Status: accepted (owner chose "use musl's source" over the host's libm)

## Problem

main imports 40 math functions (`sin`, `pow`, `expf`...). Their last bit depends on the implementation, and the host's libm is not the Switch's (decision 0008 found MinGW's off by one unit in the last place). Different last bits can make physics or battle results drift from the real game.

## Decision

1. **The reference is the sdk itself.** `python -m tools.yw4.sdkref` copies the owner's `sdk.bin` to the ARM64 reference machine (decision 0004), applies its relocations there and calls each function directly (`tools/yw4/pi/sdkrun.c`). That is Nintendo's compiled code running on a real ARM CPU with FPCR = 0, as on the Switch. 20,000 inputs per function: half random (edge values, random bit patterns), half where each function is hard (trig near multiples of π/4 and at huge arguments, exp near overflow, log near 1, integer/half-integer powers). Results go to `work/sdkref/` (derived from the sdk, so not committed). The sdk is now version-locked too: `[sdk]` in `target.toml`.
2. **The runtime uses musl 1.1.19's C** (`third_party/musl`, MIT). Measured, not guessed: 1.2.5 matches 29/40 functions, 1.1.24 31/40, 1.1.19 39/40. The sdk's math contains no fused multiply-add instructions, so compiling the C with `-ffp-contract=off` on x86 can reproduce it exactly.
3. **One patch:** `acosf` returns correctly rounded π/2 and π on two paths (the sdk has them as constants). All `acosf` mismatches were these two values.
4. **`ceil`/`floor`/`sqrt`/`fabs`** (and `f` versions) are single instructions in musl's aarch64 build, so they use the recompiler's exact instruction models (`yw4fp.h`). That covers musl's internal calls to `sqrt`/`floor` too. musl's portable C differs on signalling NaNs (it returns them unquieted).
5. **NaN results are made ARM's** after each call. Measured on the reference data: where musl returns a NaN argument without arithmetic, both machines agree, because it's the same C. They differ only where decision 0008 says: a NaN made from non-NaN inputs (x86's default NaN is negative, ARM's is `0x7ff8...`), and two NaN arguments (ARM prefers the signalling one). `hle_math.c` fixes exactly those two cases.

`python -m tools.yw4.sdkref --check` compiles the runtime's real `hle_math.c` + musl with the build's flags and drives it through a `CPUState`, as the game calls it. **Result: 40 of 40 functions match the sdk bit for bit on all 800,000 inputs.** Planted bugs are caught: undoing the `acosf` patch (1,150 mismatches) and turning off the NaN fix (thousands in `log`, `pow`).

## Why not the alternatives

- **Host libm:** not exact (decision 0008), and its errors can't be fixed afterwards.
- **Recompiling the sdk's math functions like main:** exact by construction, but it reverses decision 0005 (recompile only main) for a small set of functions. musl's C plus a bit-exact check gives the same result with readable code.
- **musl 1.2.x, the current release:** measured to differ from the Switch.

## Still open (same approach later)

`qsort` (smoothsort, order of equal elements), the `printf` family's float output, `strtod`, `strerror` text. The reference tool needs a way to pass buffers and callbacks for these.
