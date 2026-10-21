# YW4 Recomp

A fan static-recompilation project to run Yo-kai Watch 4++ (Nintendo Switch, final update) natively on Windows and Linux, in the style of N64Recomp.

This repo contains only tools, runtime code, and patches. It contains no game data. Provide your own dump of the final 4++ update.

Status: Phase 3 (kernel and system services). See [Progress](#progress) below.

## Setup

Requirements: Python 3.11+, Git, and a C11 compiler for the recompiler tests: Clang preferred ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on Windows; set `[tools] cc`), GCC works too. The host CPU needs FMA3 (any x86-64 CPU since 2013). CMake comes later.

0. Install the game-data guard as a pre-commit hook: `python -m tools.yw4.guard --install`.
1. Copy `config.example.toml` to `config.local.toml`.
2. Fill in `exefs_dir` and `romfs_dir` with the folders of your already-extracted ExeFS and RomFS.
   If you have your base and update NSPs instead, also set `base_nsp`, `update_nsp`, `hactool_exe`
   ([hactool](https://github.com/SciresM/hactool) 1.4.0) and `keys_dir` (your console's `prod.keys` and
   `title.keys`), point `exefs_dir`/`romfs_dir` at empty folders, and run `python -m tools.yw4.extract`.
3. Keep `work_dir` outside any cloud-synced folder.
4. Check it:

   ```
   python tools/yw4/config.py
   ```

   It prints the resolved paths, or a clear error saying what is missing.

5. Decompress and verify the ExeFS modules:

   ```
   python -m tools.yw4.ingest
   ```

   It refuses to continue unless `main` matches the build pinned in `target.toml`. Outputs go to `<work_dir>/exefs/`.

6. Measure the binary and assets (tables go to `<work_dir>/recon/`, summary in `docs/recon.md`):

   ```
   python -m tools.yw4.recon
   ```

Tests (synthetic data only): `python -m unittest discover -s tests -t .`

7. Recompile `main` and run it (Phase 3; details in `recomp/README.md`):

   ```
   python -m recomp.translate      # all of main -> <work_dir>/recomp/ (C, never committed)
   python -m recomp.build --run    # build <work_dir>/build/yw4.exe and run it
   ```

   It runs until the first system call (`SVC`) the runtime doesn't implement yet and prints its number.

## Progress

Where the recompiled game stops right now, and how it got there. Newest first. Each step links to the decision record that explains it.

**Current stop:** the game's first NVN call, `nvnDeviceBuilderSetDefaults`: the start of the NVN → Vulkan layer (Phase 4a). The sdk's own GPU driver is skipped (0016). Before that: 8 guest threads running in parallel as host threads (file loading and others), clock, filesystem with the RomFS (4,518 files, served from the owner's folder).

**Direction change (0014):** the `sdk` will be recompiled too, instead of re-implementing its 578 functions by hand; native code moves to the kernel/service level. Done: every instruction in `sdk` translates; both modules are translated, loaded and linked; real guest threads. NVN is reached through native overrides; next: the NVN → Vulkan layer.

| Date | Step | Result |
|---|---|---|
| 2026-10-21 | Native overrides by symbol in the translator (`recomp/overrides.toml`); NVN entered at `nvnBootstrapLoader`, the sdk's GPU driver (`nv::InitializeGraphics`, `nvdrv`) skipped; `tools/yw4/where.py` names addresses (0016) | The game asks for its NVN functions and makes its first NVN call |
| 2026-10-21 | Guest threads as host threads: one kernel lock, blocking mutexes, condition variables, events, thread exit/join, address arbiter, cancel, a deadlock detector; transfer memory (0015) | 8 threads start and run in parallel; through to the first graphics service, `nvdrv` |
| 2026-10-21 | Clock service with shared memory; filesystem service with a virtual RomFS image of the owner's folder (tables built at start-up, file data read on demand, no copy); `MapMemory` (0014) | Through the constructors' clock and file set-up to the first `CreateThread` |
| 2026-10-21 | IPC: message codec (HIPC/CMIF, domains, control messages), service manager, first services (`appletOE`, `apm`, `lm`, `aoc:u`, `pctl`), kernel events, `svcBreak` with a guest backtrace, Horizon's three-region module layout in `QueryMemory` (0014) | Through the sdk's start-up and the game's `nninitStartup` into `main`'s constructors |
| 2026-10-21 | Minimal kernel: address-space layout, `GetInfo`, heap (`SetHeapSize`, reserved and committed as it grows), `QueryMemory`, thread properties, single-thread mutex/condition-variable calls, sleep, system tick (0014) | The sdk's start-up runs through 50 system calls to its first IPC |
| 2026-10-21 | Translate and load both modules: symbols resolved at translation time in load order, one dispatch over both, start-up through the sdk's own `nn::init::Start` (0014) | 73,844 functions; Nintendo's start-up code runs natively until its first system call |
| 2026-10-21 | Instructions the `sdk` needs: AES/SHA/PMULL crypto, SIMD integer (`addv`, `smlal`, `sqadd`...), CRC32, fused multiply-add, `MRS`/`MSR`, cache ops, `SVC` (0014) | All of `sdk` translates; 13 forms Pi-checked (120,000 cases), 3 planted bugs caught |
| 2026-10-21 | Decision: recompile `sdk` as well as `main`, measured first: 99.3% of its 33,616 functions already translate (0014) | Plan for the rest of Phase 3 |
| 2026-10-21 | Math library: musl 1.1.19's C (`third_party/musl`), checked against the sdk's own functions run on the Pi; sdk version-locked in `target.toml` (0013) | All 40 math functions match the Switch bit for bit on 800,000 inputs |
| 2026-10-21 | C library basics: memory, strings, ctype, integer parsing; `__cxa_guard_*`; `__nnmusl_init_dso` (0012) | The 830 constructors start running |
| 2026-10-21 | First native `sdk` functions: start-up memory (`nn::os`), logging and asserts (`nn::diag`), the game's heap (`nn::mem::StandardAllocator` on a TLSF allocator) (0011) | `nninitStartup` runs to the end; the game logs `Remain Memory Size 198MiB` |
| 2026-10-21 | Loader and import stubs (0010) | `main` loads at its Switch address `0x7100000000`, relocates, and runs natively until its first `sdk` call |
| 2026-10-21 | Every instruction in `main` translates (0006, 0007) | 40,228 functions, 4.25 M instructions, 0 untranslated; compiles and links with Clang |
| 2026-10-20 – 21 | Recompiler: integer, loads/stores, floating point and SIMD, atomics, whole functions (0006–0009) | Each instruction form checked against a real ARM64 CPU (Raspberry Pi 5, 0004) |
| 2026-10-20 | Phase 1 decision: custom runtime, recompile only `main`, NVN on Vulkan (0005) | |
| 2026-10-14 – 20 | Phase 0: scaffold, NSO pipeline with version lock, recon report, Ghidra setup (0001–0003) | `docs/recon.md` |

Still open from Phase 0: the baseline benchmark (`docs/baseline.md`), which needs the owner's emulator runs.

Known gaps, in the order they will come up:
- Recompiling `sdk` (0014): real threads (the kernel is single-threaded so far), then more system services.
- `qsort`, `printf`-family float output, `strtod`, `strerror`: same approach as the math functions (musl source, checked against the sdk), not done yet.
- Threads (one guest thread so far), `setjmp`/`longjmp` for Lua, contents of the 20 data imports (RTTI, `stdio`).
- C++ exceptions are not supported: nothing in `main` throws (0012).

## Layout

| Path | What |
|---|---|
| `tools/` | Python analysis and input-pipeline scripts |
| `runtime/` | Native runtime (Phase 1+) |
| `recomp/` | AArch64 â†’ C recompiler (Phase 2+) |
| `docs/` | Recon report, baseline, decision records |
| `third_party/musl/` | musl 1.1.19's math functions (MIT), see its README |
| `work/`, `build/` | Generated output (gitignored) |

See `docs/decisions/` for why things are set up this way.
