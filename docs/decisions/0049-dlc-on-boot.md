# 0049 — DLC: the game finds and checks the owner's add-ons on boot
<!-- decision
state: accepted
date: 2026-11-03
replaces: none
code: target.toml, recomp/build.py, runtime/src/svc_misc.c
-->

Status: accepted (2026-11-03). The owner's 11 add-ons are found; an empty file named like an add-on unlocks
nothing; a real add-on renamed to another's title ID counts as what it is.

## Context

The game asks `aoc:u` once at start which add-ons are owned. Until now Python worked that out from the title IDs
in the file names of `[input] dlc_dir` and passed the indices in `YW4_AOC`: only `recomp.build --run` did it
(fixed for `tools.dev.run` in a8b8184), the exe couldn't run without a launcher handing it the list, and a renamed
empty file would unlock paid DLC.

What an add-on NSP of this game holds (the owner's 11, measured): a content NCA (112 KiB), a meta `.cnmt.nca`
(3.5 KiB), a ticket (`.tik`, 704 B) and a certificate. A content file's name is the first half of its own
SHA-256 (Nintendo's content IDs), so it is the same file for every owner; a ticket can carry console- or
account-specific data, so the NSP as a whole is not.

## Decision

- `target.toml` `[dlc]`: index (title ID − application ID − 0x1000) = SHA-256 of that add-on's content NCA.
  Hashes only (rule 1); the latest versions of each add-on (4 of them have updates: v65536 to v262144).
- `recomp.build` writes them into `<work_dir>/build/dlc_known.c` (rewritten only when the table changes), as it
  writes `version.c` (decision 0047).
- On boot (`runtime/src/svc_misc.c`) the game reads every `.nsp` in `YW4_DLC_DIR` (default `work/dlc`; the dev
  tools and `recomp.build --run` set it to `[input] dlc_dir`), hashes each content NCA under 16 MiB and owns the
  add-on whose hash it is. Files that match nothing are named in the log and left out (the base game and update
  NSPs, when they share the folder, are among them). Hashing 11 × 112 KiB costs nothing noticeable.
- `YW4_AOC` (indices) still replaces the scan, for tests.

## Consequences

- The exe needs no launcher for DLC: drop the NSPs in its DLC folder. The launcher only shows what the game will
  find.
- An owner with an older version of an updated add-on isn't recognised for that add-on until its hash is added.
- Adding DLC support for another dump format (an XCI of add-ons, NSZ) means reading that container here too.

## Update 2026-11-12: the hash leaves out the NCA header

A tester's add-ons (issue #3) were all left out although 12 of them hold the same content as the owner's. They had been
run through a ticket-removal tool: it rewrites bytes 0x200–0x33F of each content NCA's header (rights ID and key
area) and can rename the file (the 4++ Upgrade Pack's was `bcca0183….nca`), so neither the whole-file hash nor the
name prefix matched.

- `[dlc]` now holds the SHA-256 of each content NCA from byte 0xC00 to the end: the body, past the main header and
  the four section headers. It is the same with or without a ticket.
- The name prefix filter is gone: every content NCA up to 256 MiB is hashed (the game's and update's program NCAs are
  GiBs and still skipped; their small control/manual NCAs cost nothing). The runtime's cache is now `aoc-body.cache`;
  the old `aoc.cache` held whole-file hashes and is not read.
- Same rule in `tools/yw4/extract.py` (add-on RomFS) and the launcher's check (`launcher/src-tauri/src/check.rs`).
  The launcher's DLC tab now lists every file it left out, with a reason: not an `.nsp`, can't be read, the game or
  an update, or not a known copy.
- Add-ons 1 and 9 stay unknown. The only copies found (a widely shared "15 DLC Pack") have blank header signatures,
  SDK 0.12.17.0 and key generation 0: built by a homebrew tool, not dumped. Their `aoc.cfg.bin` names slots that do
  exist in the game's `add_content_config.cfg.bin` (33 slots), but nothing shows they were ever sold. Files nobody can
  own are not added to the table.
