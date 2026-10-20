# 0003 — Extracting ExeFS/RomFS from NSPs with hactool

Date: 2026-10-20
Status: accepted

## Decision

- `tools/yw4/extract.py` turns the user's own base + update NSPs into the `exefs_dir` / `romfs_dir` that `ingest` reads, with the update applied.
- We read the NSP container (PFS0) ourselves (`tools/yw4/pfs0.py`) and call **hactool 1.4.0** (SciresM, ISC license) for NCA decryption and update patching. hactool is a user-supplied tool, set by `[tools] hactool_exe`. It is not vendored.
- Keys are given to hactool through a temporary home folder (`.switch/prod.keys` + `.switch/title.keys`) that is deleted afterwards. The temporary `title.keys` holds only the rights IDs found in the two NSPs' tickets.

## Why

- **hactool, not hactoolnet:** the LibHac repo (hactoolnet) returned 404 on GitHub on 2026-10-20. hactool's official repo is still up. We only run it, never copy its code, and its license is ISC anyway, so the GPL question (CLAUDE.md rule 5) does not come up.
- **Old build is fine:** hactool 1.4.0 (2020) reads master keys from `prod.keys`. This game uses key generation 7 (firmware 7.0.0–8.0.1), so it is well within range.
- **Temporary home folder:** base and update have different rights IDs, so they need different title keys, but `--titlekey` takes only one. hactool also loads `~/.switch/title.keys` on its own and matches keys per NCA. Pointing `USERPROFILE`/`HOME` at a temp folder uses that without touching the user's real home. Writing only the two needed keys also gets around hactool 1.4 aborting on duplicate entries, which real `title.keys` dumps can contain.
- **Doing PFS0 ourselves:** it is a 20-line format, and it lets us find tickets (rights IDs) and skip `.cnmt.nca` without parsing hactool's text output.
- **Update applied via `--basenca`:** the update's RomFS is a patch (BKTR) on the base RomFS and can't be read alone.

## Consequences

- The unpacked NCAs in `work/nca/` (~9 GB) are deleted after a successful run unless `--keep-nca` is passed.
- If hactool disappears too, the same flow works with any tool that accepts a base NCA and a patch NCA; only `run_hactool` and `find_program_nca` would change.
