# 0014 — Recompile the sdk too; native code at the kernel and service level

Date: 2026-10-21
Status: accepted (owner, 2026-10-21). Replaces the "recompile only main, implement the 578 sdk imports natively" part of decision 0005. 0005's graphics plan (NVN at the API level on Vulkan) stands.

## Why change

Implementing sdk functions natively means proving each one matches the Switch. Decision 0013 did that for libm and needed an old musl version, a patch and an ARM NaN rule. Doing it ~480 more times (`nn::account`, `nn::fs`, `nn::os` threads, sockets, `printf`, `qsort`...) is most of the remaining work, and every one is a chance to differ from the real game.

Measured 2026-10-21 by translating the owner's `sdk.bin` with the current recompiler, in memory:
- 33,616 functions found (96% of `.text`); **33,366 (99.3%) translate completely already**.
- 2,245 instructions out of 1.3 M are missing: AES/SHA/PMULL crypto (~1,400, `nn::crypto`), SIMD integer (~600: `addv`, `smlal`, `sqadd`, `sqrshrn`... in audio DSP and string code), CRC32 (14), cache maintenance, `MRS`/`MSR` of the thread pointers, counters and FPCR/FPSR.
- 60 different `SVC` numbers: the sdk's whole interface to the kernel.

LAS Recomp (github.com/peiikai/LASRecomp, MIT), a working static recompilation of Link's Awakening for Switch, uses this model: main + sdk recompiled, a small Horizon kernel and the system services implemented natively, NVN and a few functions overridden by symbol.

## Decision

- **Recompile `main` and `sdk`.** `rtld` is not needed: the loader does its job (load, relocate, resolve symbols between the two modules).
- **Native code sits at the kernel boundary:** `SVC` calls (memory, threads, synchronisation, handles, IPC), the thread-local region (`TPIDRRO_EL0`), and the system services the game opens through IPC (`fs`, `hid`, `acc`, `time`, `audio`, `vi`...). Like unimplemented imports today, an unimplemented SVC or service command stops with its name and caller.
- **Overrides by symbol stay possible**, for NVN (graphics, decision 0005) and for any function where native code is clearly better. Each override is listed and justified.
- **Start-up as LAS Recomp does it:** call the sdk's `nn::init::Start(thread handle, argument region, init, fini)` on the main guest thread. It calls back to run the sdk's DT_INIT after `nnosInitialize` and main's DT_INIT after `nninitStartup`, then `nnMain`.
- **Memory:** main stays at `0x7100000000`; sdk follows it; everything stays inside the 39-bit space.

## What happens to the native sdk functions written so far

They become unnecessary once the recompiled sdk runs: heap (0011), diag, string and libc (0012), libm (0013). They stay in the tree until the recompiled sdk passes the same tests: `tools/yw4/sdkref.py --check` then checks the *recompiled* sdk functions against the sdk running on the Pi. After that they are removed, `third_party/musl` included, unless one is kept deliberately as an override.

## Plan

1. **Recompiler:** the missing instruction groups, each with Pi golden tests like the existing 51 forms; `SVC` and `MRS`/`MSR` become calls into the runtime.
2. **Translate both modules:** per-module function names and one address-sorted dispatch table.
3. **Loader:** load main + sdk, relocate both, resolve imports in both directions.
4. **Kernel, minimal:** the main thread, its thread-local region, memory SVCs, `GetInfo`. Grow it as the start-up path reaches more.
5. **Services** as the game opens them.

Each step lands as its own commit, with the README progress log updated.

## Progress

- **Step 1 done (2026-10-21).** New and extended forms, all checked against the Pi: CRC32/CRC32C (`dp2`); `sqadd`, `sqsub`, `smax`, `mla`, `mul` (three same); `cnt`, `neg`, `rev16`, `rev32`, `shll`, `fcvtl`, `fcvtn`, `frintm` (two-register misc); `smlal`, `uaddw`, `pmull` incl. 64-bit (three different); `shrn`, `sqshrn`, fixed-point `fcvtzs` (shift by immediate); `fmla`, `mul`, `smlal` by element; new forms `simd_across` (`addv`, `smaxv`, `uaddlv`), `simd_scalar_addp`, `simd_scalar_shift_imm`, `crypto_aes`, `crypto_sha3`, `crypto_sha2`, `fp_dp3` (`fmadd` family). 13 forms, 120,000 cases, all match. Planted bugs caught: double-rounded single-precision FMA, AES row shift reversed, wrong CRC32C polynomial.
  - Fused multiply-add (`f32_muladd`/`f64_muladd` in `yw4fp.h`), which decision 0008 skipped because main had none: the Arm ARM's `FPMulAdd` NaN order, single precision via the existing round-to-odd add.
  - AES/SHA in `runtime/src/crypto.c`, written from the pseudocode; the AES S-box is computed, not typed.
  - System instructions (`function.py`): `MRS` of `TPIDRRO_EL0`, `TPIDR_EL0`, `CNTPCT_EL0` (19.2 MHz), `CTR_EL0` (the A57's value), `FPCR`, `FPSR`; `MSR` of `TPIDR_EL0`, `FPCR` (non-zero stops, 0008), `FPSR`; `DC CVAU/CVAC/CIVAC` and `IC IVAU` are no-ops; `SVC` calls `yw4_svc` (`runtime/src/kernel.c`, which stops on every call for now).
  - Result: every instruction the sdk's 33,616 functions reach translates, except one word that is not an instruction (data between functions; it traps if executed). main: still 0 traps, same run as before.
- **Steps 2 and 3 done (2026-10-21).**
  - `recomp.translate` translates both modules (main `f_<off>` with base `yw4_image_base`, sdk `s_<off>` with base `yw4_sdk_base`): 40,228 + 33,616 functions. One function table per module (`yw4_code[]`); `yw4_resolve` finds the module containing an address, so calls between modules (through the PLT, `BR x17`) just work.
  - Symbols are resolved at translation time, in load order (main, then sdk), as rtld does: main's 574 imports go to the sdk, the sdk's 8 imports to main, and the 9 symbols both define resolve to main's copy inside the sdk too. 5 weak symbols nobody defines (linker markers, `nnosGetDefaultUserExceptionStackForApplication`) are 0. The import stubs of decision 0010 are gone.
  - Memory: main at `0x7100000000`, sdk right after it at `0x7106c00000` (2 MiB aligned), main thread stack and priority from the NPDM (1 MiB, 44), thread-local region at `0x7040000000`, host callbacks at `0x7080000000`.
  - Start-up: `nn::init::Start(0x101, 0, run sdk DT_INIT, run main DT_INIT)` on the main thread. The thread handle is a placeholder until the kernel owns handles.
  - Result: Nintendo's start-up code runs natively until its first system call, `SVC 0x29` (`GetInfo`, info type 11: random entropy). Next: step 4, the kernel.
