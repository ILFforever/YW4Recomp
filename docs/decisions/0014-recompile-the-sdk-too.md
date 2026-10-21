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
