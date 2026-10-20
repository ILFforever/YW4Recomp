# 0004 — Raspberry Pi 5 as the ARM64 test reference; x86-64 is the first target

Date: 2026-10-20
Status: accepted

## Decision

- The recompiled game targets **x86-64 (Windows, then Linux) first**. The generated C stays portable, but nothing is tuned for ARM hosts yet.
- Correctness of the instruction translation is checked against **real ARM64 hardware**: the owner's Raspberry Pi 5 (Cortex-A76, 8 GB, GCC 14.2). A small harness on the Pi runs instruction sequences with chosen inputs and records the resulting registers, flags, and memory as golden test files. The x86 test suite runs the recompiled C on the same inputs and compares.
- We do **not** use Unicorn (or any other CPU emulator) as the reference.

## Why

- **Real silicon beats an emulator as the reference.** A reference emulator has its own bugs; the Pi doesn't.
- **No GPL.** Unicorn is GPL-2. Using it only in tests would probably not affect the project's license, but avoiding it removes the question (CLAUDE.md rule 5).
- **Close enough to the Switch.** The Switch's Cortex-A57 is ARMv8.0; the A76 is ARMv8.2 and runs every v8.0 instruction the same way. The A76 also has newer instructions (LSE `atomics`, `lrcpc`, dot product, fp16) that the Switch lacks, so the harness only tests encodings that appear in `main` (or synthetic v8.0 encodings), never "everything the Pi supports".
- **Golden files make day-to-day work Pi-free.** Results are generated in batches; the normal test run happens on the PC.

## Data handling

- Golden files built from synthetic encodings can be committed.
- Golden files built from instructions taken from `main` are game-derived and stay in `work/` (CLAUDE.md rule 1).

## Access

The Pi is reached over SSH with a project-only key (`~/.ssh/yw4_pi`), installed by the owner. The host and user will come from `config.local.toml`, never hardcoded.
