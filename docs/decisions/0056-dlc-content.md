# 0056 — DLC content: extracting add-on RomFS files and serving them on `fsp-srv` 202
<!-- decision
state: accepted
date: 2026-11-07
replaces: none
code: target.toml, runtime/src/svc_misc.c, runtime/src/svc_fs.c, runtime/src/romfs.c, tools/yw4/extract.py, tools/yw4/build_install.py, launcher/src-tauri/src/check.rs, launcher/src-tauri/src/buildrun.rs
-->

Status: accepted (2026-11-07). Recognises add-ons 2 (`010086C00AF7D002`) and 100 (`010086C00AF7D064`, the 4++ Upgrade
Pack), extracts add-on RomFS sections to `<romfs_dir>/../dlc-romfs/<index>/`, and serves them through `fsp-srv`
command 202 (`OpenDataStorageByDataId`).

## Context

Decision 0049 taught `aoc:u` to recognise the 11 small item/pass add-ons (112 KiB content NCAs) on boot by hashing each
NSP's content NCA against `target.toml` `[dlc]`. Those small add-ons only need `aoc:u` to report their indices as owned;
the game never opens their RomFS (`aoc.cfg.bin`, 320 B).

Two more add-ons exist for the game:
- Index **2** (`010086C00AF7D002` v0): another 112 KiB ticket-encrypted PublicData NCA (`aoc.cfg.bin`, 320 B).
- Index **100** (`010086C00AF7D064` v196608, the 4 → 4++ Upgrade Pack): a 222 MiB NSP with no ticket (standard crypto,
  decrypted with `prod.keys` alone). Its PublicData NCA holds a RomFS section with `yw4_evop_0002.usm` (232,239,040 B).

When `aoc:u` reports index 100 as owned, the game calls `fsp-srv` command **202** (`OpenDataStorageByDataId`, storage id
+ 64-bit data id `0x010086C00AF7D064`) to mount the pack's RomFS. Without command 202 implemented, the runtime stops on
the unimplemented IPC command.

## Decision

- **`target.toml` `[dlc]`**: add `"2"` and `"100"` with their content NCA SHA-256 hashes.
- **Boot & launcher check (`runtime/src/svc_misc.c`, `launcher/src-tauri/src/check.rs`)**:
  - Raise the content NCA size cap from 16 MiB to 256 MiB and stream SHA-256 in chunks (`yw4_sha256_init` /
    `update` / `final_hex` in `runtime/src/sha256.c`; `std::io::copy` in `check.rs`) instead of allocating the whole
    NCA in memory. Use `_wfindfirst64` / `_wfopen` on Windows so NSPs with Japanese filenames open regardless of the
    active ANSI code page.
  - NCAs over 256 MiB are skipped without reading, so multi-GB base and update NCAs sharing the folder cost nothing.
    (Until 2026-11-12 the filter was the file name; see decision 0049's update: the hash now leaves out the header.)
  - Software SHA-256 on the 222 MiB pack takes ~1028 ms on the first boot; once an NCA over 1 MiB passes its full
    SHA-256 check, `svc_misc.c` records `(name, size, first-4-KiB SHA-256) -> body SHA-256` in `aoc-body.cache` next to
    `yw4.exe`, bringing subsequent boot scans of all 15 NSPs down to ~11–13 ms.
- **Extracting add-on RomFS (`tools/yw4/extract.py`, `tools/yw4/build_install.py`, `launcher/src-tauri/src/buildrun.rs`,
  `launcher/src-tauri/src/ready.rs`)**:
  - Reuse the existing `hactool` + `write_temp_keys` pipeline in `tools/yw4/extract.py` (`extract_dlc(cfg)`): scan
    `[input] dlc_dir` for recognised add-ons in `target.toml` `[dlc]`, unpack any whose `<romfs_dir>/../dlc-romfs/<index>/`
    folder is not yet populated, and extract its RomFS section there. Add-ons that have no ticket (such as the 4++
    Upgrade Pack) need only `prod.keys`; ticket-encrypted add-ons whose title key is absent from `title.keys` are
    skipped with a note (the game never opens the small add-ons' RomFS).
  - Run `extract_dlc` during `python -m tools.yw4.extract`, during the launcher's DLC step (`copy_dlc` in `buildrun.rs`,
    after copying checked NSPs into `data/dlc`), and in `ready.rs` (`install_info` and `play`) whenever the 4++ Upgrade
    Pack NSP is in `data/dlc` without `data/dlc-romfs/100/` unpacked yet. If `data/dlc-romfs/100/` is still missing
    (for instance after an "Extracted folders" setup with no `prod.keys`), the Ready screen's DLC tab shows a
    **Choose prod.keys…** banner (`set_dlc_keys` in `ready.rs`, `save_keys_dir` in `buildrun.rs`) that saves `keys_dir`
    to `config.toml` and unpacks the pack on the spot.
  - `game.ini` (`tools/yw4/build_install.py`) and `cfg.dlc_env()` (`tools/yw4/config.py`) set `YW4_DLCROMFS_DIR` to
    `<romfs_dir>/../dlc-romfs` (`../../data/dlc-romfs` in an install).
- **Serving add-on RomFS at run time (`runtime/src/romfs.c`, `runtime/src/svc_fs.c`)**:
  - Factor `romfs.c`'s virtual RomFS builder into `RomfsImage` so additional RomFS images (`romfs_image_open`,
    `romfs_image_size`, `romfs_image_read`) can be built in memory from `<YW4_DLCROMFS_DIR>/<index>/` without copying
    the files on disk.
  - Implement `fsp-srv` command **202** (`OpenDataStorageByDataId`): read `storage_id` (u8 at +0) and `data_id` (u64 at
    +8), check that `data_id - 0x010086C00AF7D000` is an owned add-on index (`aoc_owned`) and open its `RomfsImage` from
    `YW4_DLCROMFS_DIR/<index>/` (default `work/dlc-romfs/<index>`), returning an `IStorage` backed by `romfs_storage`.
    If the add-on is not owned or its folder is missing, return `FS_TARGET_NOT_FOUND` (`0x7D402`) instead of stopping.

## Consequences

- Owning the 4++ Upgrade Pack (index 100) no longer aborts on `fsp-srv` 202; the game mounts its RomFS and reads its
  files on demand.
- Players who add DLC NSPs after building get their RomFS extracted automatically on the next launch from the launcher
  (or via `python -m tools.yw4.extract --dlc-only`).
- The runtime still links no encryption or NCA code: decryption stays in `hactool` with the owner's keys.
