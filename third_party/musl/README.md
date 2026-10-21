# musl 1.1.19: math only

The sdk's math functions, so the native runtime returns exactly what the Switch returns (docs/decisions/0013). License: MIT, see `COPYRIGHT` (the fdlibm-derived files also keep their own permissive notices).

| | |
|---|---|
| Version | musl **1.1.19** (2018-01-30) |
| Source | https://musl.libc.org/releases/musl-1.1.19.tar.gz |
| Signature | `musl-1.1.19.tar.gz.asc`, good signature from the musl signing key `8364 8929 0BB6 B70F 99FF DA05 56BC DB59 3020 450F` |
| Files | `src/internal/libm.h` and 45 files of `src/math/`: the 36 functions main imports that aren't single instructions, plus what they call (link closure) |

**Why 1.1.19:** checked against the sdk's own code, run natively on the ARM64 reference machine (`python -m tools.yw4.sdkref`). With 20,000 inputs per function:

| musl | functions matching the sdk bit for bit (NaN bits aside) |
|---|---|
| 1.2.5 | 29 of 40 (trig and the exp/log/pow family differ) |
| 1.1.24 | 31 of 40 (exp/log/pow family differ: 1.1.20 switched to ARM's newer implementations) |
| 1.1.19 | 39 of 40 (only `acosf`, see the patch) |

## Local changes

- **`src/math/acosf.c`** (marked `YW4 PATCH`): for |x| < 2^-26 and x = −1, the sdk returns the correctly rounded π/2 and π (`0x3fc90fdb`, `0x40490fdb`). musl's `pio2_hi + 0x1p-120f` gives `0x3fc90fda`/`0x40490fda`. The sdk loads these as constants, so Nintendo's compiler folded the expression differently. All 1,249 mismatches were these two values.

Nothing else is changed. Our own files are in `yw4/`:
- `glue.h`: force-included. Gives every function a `musl_` prefix, so it can't mix with the host libm or be constant-folded as a builtin.
- `endian.h`: byte order for hosts without `<endian.h>`.

`sqrt`, `sqrtf`, `floor` (called inside musl) and the imports `ceil`, `floor`, `sqrt`, `fabs` (+ `f` versions) are not taken from here. musl's aarch64 build makes them single instructions, so `runtime/src/hle_math.c` uses the recompiler's exact models of those instructions.

Compiler flags: `recomp/build.py` `MUSL_CFLAGS` (the same list `sdkref --check` verifies with).
