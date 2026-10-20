# 0002 — NSO input pipeline and version lock

Date: 2026-10-20
Status: accepted

## Decision

- Decompress NSO modules with our own code (`tools/yw4/nso.py`, `tools/yw4/lz4_block.py`), not nx2elf or the `lz4` package.
- Output is a **flat memory image** per module (`work/exefs/<module>.bin`): text, ro, and data placed at their memory offsets, gaps zero-filled, .bss left out. Plus `modules.json` with segment layout, build ID, and `.dynstr`/`.dynsym` locations.
- The build is identified by **SHA-256 of `main`'s flat image**, pinned in the committed `target.toml`. The build ID is recorded next to it for humans and IPS-patch tooling.
- `ingest` verifies `main` first and refuses to write anything on a mismatch. With nothing pinned yet, it prints the hash and needs `--pin` to record it. After that, changing the target means editing `target.toml` by hand.

## Why

- **Own decoder:** LZ4 block format is ~40 lines. Writing it keeps Phase 0 stdlib-only (no new dependency) and makes the pipeline fully explainable. Speed is fine: ~2 s for 32 MB of compressed data in pure Python. The `lz4` package is used only in tests, as a reference to compare against.
- **Flat image, not ELF:** the Ghidra Switch loader reads NSO directly, so we don't need an ELF yet. A flat image is the simplest thing that later tools (string scan, RTTI survey, import parsing) can index by virtual address. If we need an ELF later, it can be built from the image plus `modules.json`.
- **Hash the image, not the file:** the same game code can be stored compressed or uncompressed (some tools rewrite NSOs). The image hash only changes if the code or data changes. Each segment's SHA-256 in the NSO header is also checked during decompression, which catches corrupt dumps before hashing.
- **Hash + build ID:** the hash is what we enforce. The build ID is what the modding community uses to name patches, so it's useful to have both.

## Not done here

- Other modules (`rtld`, `sdk`, `subsdk*`) are decompressed and their hashes recorded in `modules.json`, but not pinned. If `main` matches, they come from the same update.
